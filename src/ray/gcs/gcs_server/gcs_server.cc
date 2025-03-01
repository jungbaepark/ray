// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_server/gcs_server.h"

#include "ray/common/asio/asio_util.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/common/network_util.h"
#include "ray/common/ray_config.h"
#include "ray/gcs/gcs_server/gcs_actor_manager.h"
#include "ray/gcs/gcs_server/gcs_job_manager.h"
#include "ray/gcs/gcs_server/gcs_node_manager.h"
#include "ray/gcs/gcs_server/gcs_object_manager.h"
#include "ray/gcs/gcs_server/gcs_placement_group_manager.h"
#include "ray/gcs/gcs_server/gcs_worker_manager.h"
#include "ray/gcs/gcs_server/stats_handler_impl.h"
#include "ray/gcs/gcs_server/task_info_handler_impl.h"
#include "ray/stats/stats.h"
#include "ray/util/agent_finder.h"

namespace ray {
namespace gcs {

GcsServer::GcsServer(const ray::gcs::GcsServerConfig &config,
                     instrumented_io_context &main_service)
    : config_(config),
      main_service_(main_service),
      rpc_server_(config.grpc_server_name, config.grpc_server_port,
                  config.grpc_server_thread_num,
                  /*keepalive_time_ms=*/RayConfig::instance().grpc_keepalive_time_ms()),
      client_call_manager_(main_service),
      raylet_client_pool_(
          std::make_shared<rpc::NodeManagerClientPool>(client_call_manager_)),
      pubsub_periodical_runner_(main_service_) {}

GcsServer::~GcsServer() { Stop(); }

void GcsServer::Start() {
  // Init backend client.
  RedisClientOptions redis_client_options(config_.redis_address, config_.redis_port,
                                          config_.redis_password,
                                          config_.enable_sharding_conn);
  redis_client_ = std::make_shared<RedisClient>(redis_client_options);
  auto status = redis_client_->Connect(main_service_);
  RAY_CHECK(status.ok()) << "Failed to init redis gcs client as " << status;

  // Init stats.
  const ray::stats::TagsType global_tags = {
      {ray::stats::ComponentKey, "gcs_server"},
      {ray::stats::VersionKey, "2.0.0.dev0"},
      {ray::stats::NodeAddressKey, config_.node_ip_address}};
  ray::stats::Init(
      global_tags, [this](const ray::stats::GetAgentAddressCallback &callback) {
        // This is the opencensus report thread, we should do the GCS operation
        // in main_service_.
        main_service_.post(
            [this, callback] {
              if (!gcs_node_manager_) {
                callback(ray::Status::Invalid("The GcsNodeManager is not initialized."),
                         std::string());
                return;
              }
              auto all_alive_nodes = gcs_node_manager_->GetAllAliveNodes();
              if (all_alive_nodes.empty()) {
                callback(ray::Status::Invalid("No alive nodes."), std::string());
                return;
              }
              auto selected_node_id = all_alive_nodes.begin()->first;
              GetAgentAddress(redis_client_, selected_node_id, callback);
            },
            "GetAgentAddressCallback");
      });

  // Init redis failure detector.
  gcs_redis_failure_detector_ = std::make_shared<GcsRedisFailureDetector>(
      main_service_, redis_client_->GetPrimaryContext(), [this]() { Stop(); });
  gcs_redis_failure_detector_->Start();

  // Init gcs pub sub instance.
  gcs_pub_sub_ = std::make_shared<gcs::GcsPubSub>(redis_client_);

  if (config_.grpc_pubsub_enabled) {
    // Init grpc based pubsub
    // TODO(before merging): Make these constants configurable.
    grpc_pubsub_publisher_.reset(new pubsub::Publisher(
        /*periodical_runner=*/&pubsub_periodical_runner_,
        /*get_time_ms=*/[]() { return absl::GetCurrentTimeNanos() / 1e6; },
        /*subscriber_timeout_ms=*/RayConfig::instance().subscriber_timeout_ms(),
        /*publish_batch_size_=*/RayConfig::instance().publish_batch_size()));
  }

  // Init gcs table storage.
  gcs_table_storage_ = std::make_shared<gcs::RedisGcsTableStorage>(redis_client_);

  // Load gcs tables data asynchronously.
  auto gcs_init_data = std::make_shared<GcsInitData>(gcs_table_storage_);
  gcs_init_data->AsyncLoad([this, gcs_init_data] { DoStart(*gcs_init_data); });
}

void GcsServer::DoStart(const GcsInitData &gcs_init_data) {
  // Init gcs resource manager.
  InitGcsResourceManager(gcs_init_data);

  // Init gcs resource scheduler.
  InitGcsResourceScheduler();

  // Init gcs node manager.
  InitGcsNodeManager(gcs_init_data);

  // Init gcs heartbeat manager.
  InitGcsHeartbeatManager(gcs_init_data);

  // Init KV Manager
  InitKVManager();

  // Init RuntimeENv manager
  InitRuntimeEnvManager();

  // Init gcs job manager.
  InitGcsJobManager(gcs_init_data);

  // Init gcs placement group manager.
  InitGcsPlacementGroupManager(gcs_init_data);

  // Init gcs actor manager.
  InitGcsActorManager(gcs_init_data);

  // Init object manager.
  InitObjectManager(gcs_init_data);

  // Init gcs worker manager.
  InitGcsWorkerManager();

  // Init task info handler.
  InitTaskInfoHandler();

  // Init stats handler.
  InitStatsHandler();

  // Init resource report polling.
  InitResourceReportPolling(gcs_init_data);

  // Init resource report broadcasting.
  InitResourceReportBroadcasting(gcs_init_data);

  // Install event listeners.
  InstallEventListeners();

  // Start RPC server when all tables have finished loading initial
  // data.
  rpc_server_.Run();

  // Store gcs rpc server address in redis.
  StoreGcsServerAddressInRedis();
  // Only after the rpc_server_ is running can the heartbeat manager
  // be run. Otherwise the node failure detector will mistake
  // some living nodes as dead as the timer inside node failure
  // detector is already run.
  gcs_heartbeat_manager_->Start();

  // Print debug info periodically.
  PrintDebugInfo();

  // Print the asio event loop stats periodically if configured.
  PrintAsioStats();

  CollectStats();

  is_started_ = true;
}

void GcsServer::Stop() {
  if (!is_stopped_) {
    RAY_LOG(INFO) << "Stopping GCS server.";
    // GcsHeartbeatManager should be stopped before RPCServer.
    // Because closing RPC server will cost several seconds, during this time,
    // GcsHeartbeatManager is still checking nodes' heartbeat timeout. Since RPC Server
    // won't handle heartbeat calls anymore, some nodes will be marked as dead during this
    // time, causing many nodes die after GCS's failure.
    gcs_heartbeat_manager_->Stop();

    gcs_resource_report_poller_->Stop();

    if (config_.grpc_based_resource_broadcast) {
      grpc_based_resource_broadcaster_->Stop();
    }

    // Shutdown the rpc server
    rpc_server_.Shutdown();

    // Shutdown stats.
    ray::stats::Shutdown();

    is_stopped_ = true;
    RAY_LOG(INFO) << "GCS server stopped.";
  }
}

void GcsServer::InitGcsNodeManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(redis_client_ && gcs_table_storage_ && gcs_pub_sub_);
  gcs_node_manager_ = std::make_shared<GcsNodeManager>(gcs_pub_sub_, gcs_table_storage_);
  // Initialize by gcs tables data.
  gcs_node_manager_->Initialize(gcs_init_data);
  // Register service.
  node_info_service_.reset(
      new rpc::NodeInfoGrpcService(main_service_, *gcs_node_manager_));
  rpc_server_.RegisterService(*node_info_service_);
}

void GcsServer::InitGcsHeartbeatManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_node_manager_);
  gcs_heartbeat_manager_ = std::make_shared<GcsHeartbeatManager>(
      heartbeat_manager_io_service_, /*on_node_death_callback=*/
      [this](const NodeID &node_id) {
        main_service_.post(
            [this, node_id] { return gcs_node_manager_->OnNodeFailure(node_id); },
            "GcsServer.NodeDeathCallback");
      });
  // Initialize by gcs tables data.
  gcs_heartbeat_manager_->Initialize(gcs_init_data);
  // Register service.
  heartbeat_info_service_.reset(new rpc::HeartbeatInfoGrpcService(
      heartbeat_manager_io_service_, *gcs_heartbeat_manager_));
  rpc_server_.RegisterService(*heartbeat_info_service_);
}

void GcsServer::InitGcsResourceManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_table_storage_ && gcs_pub_sub_);
  gcs_resource_manager_ = std::make_shared<GcsResourceManager>(
      main_service_, gcs_pub_sub_, gcs_table_storage_,
      !config_.grpc_based_resource_broadcast);
  // Initialize by gcs tables data.
  gcs_resource_manager_->Initialize(gcs_init_data);
  // Register service.
  node_resource_info_service_.reset(
      new rpc::NodeResourceInfoGrpcService(main_service_, *gcs_resource_manager_));
  rpc_server_.RegisterService(*node_resource_info_service_);
}

void GcsServer::InitGcsResourceScheduler() {
  RAY_CHECK(gcs_resource_manager_);
  gcs_resource_scheduler_ =
      std::make_shared<GcsResourceScheduler>(*gcs_resource_manager_);
}

void GcsServer::InitGcsJobManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_table_storage_ && gcs_pub_sub_);
  gcs_job_manager_ = std::make_unique<GcsJobManager>(gcs_table_storage_, gcs_pub_sub_,
                                                     *runtime_env_manager_);
  gcs_job_manager_->Initialize(gcs_init_data);
  // Register service.
  job_info_service_ =
      std::make_unique<rpc::JobInfoGrpcService>(main_service_, *gcs_job_manager_);
  rpc_server_.RegisterService(*job_info_service_);
}

void GcsServer::InitGcsActorManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_table_storage_ && gcs_pub_sub_ && gcs_node_manager_);
  auto scheduler = std::make_shared<RayletBasedActorScheduler>(
      main_service_, gcs_table_storage_->ActorTable(), *gcs_node_manager_, gcs_pub_sub_,
      /*schedule_failure_handler=*/
      [this](std::shared_ptr<GcsActor> actor) {
        // When there are no available nodes to schedule the actor the
        // gcs_actor_scheduler will treat it as failed and invoke this handler. In
        // this case, the actor manager should schedule the actor once an
        // eligible node is registered.
        gcs_actor_manager_->OnActorCreationFailed(std::move(actor));
      },
      /*schedule_success_handler=*/
      [this](std::shared_ptr<GcsActor> actor) {
        gcs_actor_manager_->OnActorCreationSuccess(std::move(actor));
      },
      raylet_client_pool_,
      /*client_factory=*/
      [this](const rpc::Address &address) {
        return std::make_shared<rpc::CoreWorkerClient>(address, client_call_manager_);
      });
  gcs_actor_manager_ = std::make_shared<GcsActorManager>(
      scheduler, gcs_table_storage_, gcs_pub_sub_, *runtime_env_manager_,
      [this](const ActorID &actor_id) {
        gcs_placement_group_manager_->CleanPlacementGroupIfNeededWhenActorDead(actor_id);
      },
      [this](const JobID &job_id) { return gcs_job_manager_->GetRayNamespace(job_id); },
      [this](std::function<void(void)> fn, boost::posix_time::milliseconds delay) {
        boost::asio::deadline_timer timer(main_service_);
        timer.expires_from_now(delay);
        timer.async_wait([fn](const boost::system::error_code &error) {
          if (error != boost::asio::error::operation_aborted) {
            fn();
          } else {
            RAY_LOG(WARNING)
                << "The GCS actor metadata garbage collector timer failed to fire. This "
                   "could old actor metadata not being properly cleaned up. For more "
                   "information, check logs/gcs_server.err and logs/gcs_server.out";
          }
        });
      },
      [this](const rpc::Address &address) {
        return std::make_shared<rpc::CoreWorkerClient>(address, client_call_manager_);
      });

  // Initialize by gcs tables data.
  gcs_actor_manager_->Initialize(gcs_init_data);
  // Register service.
  actor_info_service_.reset(
      new rpc::ActorInfoGrpcService(main_service_, *gcs_actor_manager_));
  rpc_server_.RegisterService(*actor_info_service_);
}

void GcsServer::InitGcsPlacementGroupManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_table_storage_ && gcs_node_manager_);
  auto scheduler = std::make_shared<GcsPlacementGroupScheduler>(
      main_service_, gcs_table_storage_, *gcs_node_manager_, *gcs_resource_manager_,
      *gcs_resource_scheduler_, raylet_client_pool_);

  gcs_placement_group_manager_ = std::make_shared<GcsPlacementGroupManager>(
      main_service_, scheduler, gcs_table_storage_, *gcs_resource_manager_,
      [this](const JobID &job_id) { return gcs_job_manager_->GetRayNamespace(job_id); });
  // Initialize by gcs tables data.
  gcs_placement_group_manager_->Initialize(gcs_init_data);
  // Register service.
  placement_group_info_service_.reset(new rpc::PlacementGroupInfoGrpcService(
      main_service_, *gcs_placement_group_manager_));
  rpc_server_.RegisterService(*placement_group_info_service_);
}

void GcsServer::InitObjectManager(const GcsInitData &gcs_init_data) {
  RAY_CHECK(gcs_table_storage_ && gcs_pub_sub_ && gcs_node_manager_);
  gcs_object_manager_.reset(
      new GcsObjectManager(gcs_table_storage_, gcs_pub_sub_, *gcs_node_manager_));
  // Initialize by gcs tables data.
  gcs_object_manager_->Initialize(gcs_init_data);
  // Register service.
  object_info_service_.reset(
      new rpc::ObjectInfoGrpcService(main_service_, *gcs_object_manager_));
  rpc_server_.RegisterService(*object_info_service_);
}

void GcsServer::StoreGcsServerAddressInRedis() {
  std::string ip = config_.node_ip_address;
  if (ip.empty()) {
    ip = GetValidLocalIp(
        GetPort(),
        RayConfig::instance().internal_gcs_service_connect_wait_milliseconds());
  }
  std::string address = ip + ":" + std::to_string(GetPort());
  RAY_LOG(INFO) << "Gcs server address = " << address;

  RAY_CHECK_OK(redis_client_->GetPrimaryContext()->RunArgvAsync(
      {"SET", "GcsServerAddress", address}));
  RAY_LOG(INFO) << "Finished setting gcs server address: " << address;
}

void GcsServer::InitTaskInfoHandler() {
  RAY_CHECK(gcs_table_storage_ && gcs_pub_sub_);
  task_info_handler_.reset(
      new rpc::DefaultTaskInfoHandler(gcs_table_storage_, gcs_pub_sub_));
  // Register service.
  task_info_service_.reset(
      new rpc::TaskInfoGrpcService(main_service_, *task_info_handler_));
  rpc_server_.RegisterService(*task_info_service_);
}

void GcsServer::InitResourceReportPolling(const GcsInitData &gcs_init_data) {
  gcs_resource_report_poller_.reset(new GcsResourceReportPoller(
      raylet_client_pool_, [this](const rpc::ResourcesData &report) {
        gcs_resource_manager_->UpdateFromResourceReport(report);
      }));

  gcs_resource_report_poller_->Initialize(gcs_init_data);
  gcs_resource_report_poller_->Start();
}

void GcsServer::InitResourceReportBroadcasting(const GcsInitData &gcs_init_data) {
  if (config_.grpc_based_resource_broadcast) {
    grpc_based_resource_broadcaster_.reset(new GrpcBasedResourceBroadcaster(
        raylet_client_pool_,
        [this](rpc::ResourceUsageBroadcastData &buffer) {
          gcs_resource_manager_->GetResourceUsageBatchForBroadcast(buffer);
        }

        ));

    grpc_based_resource_broadcaster_->Initialize(gcs_init_data);
    grpc_based_resource_broadcaster_->Start();
  }
}

void GcsServer::InitStatsHandler() {
  RAY_CHECK(gcs_table_storage_);
  stats_handler_.reset(new rpc::DefaultStatsHandler(gcs_table_storage_));
  // Register service.
  stats_service_.reset(new rpc::StatsGrpcService(main_service_, *stats_handler_));
  rpc_server_.RegisterService(*stats_service_);
}

void GcsServer::InitKVManager() {
  kv_manager_ = std::make_unique<GcsInternalKVManager>(redis_client_);
  kv_service_ = std::make_unique<rpc::InternalKVGrpcService>(main_service_, *kv_manager_);
  // Register service.
  rpc_server_.RegisterService(*kv_service_);
}

void GcsServer::InitRuntimeEnvManager() {
  runtime_env_manager_ =
      std::make_unique<RuntimeEnvManager>([this](const std::string &uri, auto cb) {
        std::string sep = "://";
        auto pos = uri.find(sep);
        if (pos == std::string::npos || pos + sep.size() == uri.size()) {
          RAY_LOG(ERROR) << "Invalid uri: " << uri;
          cb(false);
        } else {
          auto scheme = uri.substr(0, pos);
          if (scheme != "gcs") {
            // Skip other uri
            cb(true);
          } else {
            this->kv_manager_->InternalKVDelAsync(uri, [cb](int deleted_num) {
              if (deleted_num == 0) {
                cb(false);
              } else {
                cb(true);
              }
            });
          }
        }
      });
}

void GcsServer::InitGcsWorkerManager() {
  gcs_worker_manager_ =
      std::make_unique<GcsWorkerManager>(gcs_table_storage_, gcs_pub_sub_);
  // Register service.
  worker_info_service_.reset(
      new rpc::WorkerInfoGrpcService(main_service_, *gcs_worker_manager_));
  rpc_server_.RegisterService(*worker_info_service_);
}

void GcsServer::InstallEventListeners() {
  // Install node event listeners.
  gcs_node_manager_->AddNodeAddedListener([this](std::shared_ptr<rpc::GcsNodeInfo> node) {
    // Because a new node has been added, we need to try to schedule the pending
    // placement groups and the pending actors.
    gcs_resource_manager_->OnNodeAdd(*node);
    gcs_placement_group_manager_->SchedulePendingPlacementGroups();
    gcs_actor_manager_->SchedulePendingActors();
    gcs_heartbeat_manager_->AddNode(NodeID::FromBinary(node->node_id()));
    gcs_resource_report_poller_->HandleNodeAdded(*node);
    if (config_.grpc_based_resource_broadcast) {
      grpc_based_resource_broadcaster_->HandleNodeAdded(*node);
    }
  });
  gcs_node_manager_->AddNodeRemovedListener(
      [this](std::shared_ptr<rpc::GcsNodeInfo> node) {
        auto node_id = NodeID::FromBinary(node->node_id());
        // All of the related placement groups and actors should be reconstructed when a
        // node is removed from the GCS.
        gcs_resource_manager_->OnNodeDead(node_id);
        gcs_placement_group_manager_->OnNodeDead(node_id);
        gcs_actor_manager_->OnNodeDead(node_id);
        raylet_client_pool_->Disconnect(NodeID::FromBinary(node->node_id()));
        gcs_resource_report_poller_->HandleNodeRemoved(*node);
        if (config_.grpc_based_resource_broadcast) {
          grpc_based_resource_broadcaster_->HandleNodeRemoved(*node);
        }
      });

  // Install worker event listener.
  gcs_worker_manager_->AddWorkerDeadListener(
      [this](std::shared_ptr<rpc::WorkerTableData> worker_failure_data) {
        auto &worker_address = worker_failure_data->worker_address();
        auto worker_id = WorkerID::FromBinary(worker_address.worker_id());
        auto node_id = NodeID::FromBinary(worker_address.raylet_id());
        std::shared_ptr<rpc::RayException> creation_task_exception = nullptr;
        if (worker_failure_data->has_creation_task_exception()) {
          creation_task_exception = std::make_shared<rpc::RayException>(
              worker_failure_data->creation_task_exception());
        }
        gcs_actor_manager_->OnWorkerDead(node_id, worker_id,
                                         worker_failure_data->exit_type(),
                                         creation_task_exception);
      });

  // Install job event listeners.
  gcs_job_manager_->AddJobFinishedListener([this](std::shared_ptr<JobID> job_id) {
    gcs_actor_manager_->OnJobFinished(*job_id);
    gcs_placement_group_manager_->CleanPlacementGroupIfNeededWhenJobDead(*job_id);
  });
}

void GcsServer::CollectStats() {
  gcs_actor_manager_->CollectStats();
  gcs_placement_group_manager_->CollectStats();
  execute_after(
      main_service_, [this] { CollectStats(); },
      (RayConfig::instance().metrics_report_interval_ms() / 2) /* milliseconds */);
}

void GcsServer::PrintDebugInfo() {
  std::ostringstream stream;
  stream << gcs_node_manager_->DebugString() << "\n"
         << gcs_actor_manager_->DebugString() << "\n"
         << gcs_object_manager_->DebugString() << "\n"
         << gcs_placement_group_manager_->DebugString() << "\n"
         << gcs_pub_sub_->DebugString() << "\n"
         << ((rpc::DefaultTaskInfoHandler *)task_info_handler_.get())->DebugString();

  if (config_.grpc_based_resource_broadcast) {
    stream << "\n" << grpc_based_resource_broadcaster_->DebugString();
  }
  // TODO(ffbin): We will get the session_dir in the next PR, and write the log to
  // gcs_debug_state.txt.
  RAY_LOG(INFO) << stream.str();
  execute_after(main_service_, [this] { PrintDebugInfo(); },
                (RayConfig::instance().gcs_dump_debug_log_interval_minutes() *
                 60000) /* milliseconds */);
}

void GcsServer::PrintAsioStats() {
  /// If periodic asio stats print is enabled, it will print it.
  const auto event_stats_print_interval_ms =
      RayConfig::instance().event_stats_print_interval_ms();
  if (event_stats_print_interval_ms != -1 && RayConfig::instance().event_stats()) {
    RAY_LOG(INFO) << "Event stats:\n\n" << main_service_.StatsString() << "\n\n";
    execute_after(main_service_, [this] { PrintAsioStats(); },
                  event_stats_print_interval_ms /* milliseconds */);
  }
}

}  // namespace gcs
}  // namespace ray
