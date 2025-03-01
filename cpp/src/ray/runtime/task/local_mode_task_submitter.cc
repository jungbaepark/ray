// Copyright 2020-2021 The Ray Authors.
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

#include "local_mode_task_submitter.h"

#include <ray/api/ray_exception.h>

#include <boost/asio/post.hpp>
#include <memory>

#include "../abstract_ray_runtime.h"

namespace ray {
namespace internal {

LocalModeTaskSubmitter::LocalModeTaskSubmitter(
    LocalModeRayRuntime &local_mode_ray_tuntime)
    : local_mode_ray_tuntime_(local_mode_ray_tuntime) {
  thread_pool_.reset(new boost::asio::thread_pool(10));
}

ObjectID LocalModeTaskSubmitter::Submit(InvocationSpec &invocation,
                                        const ActorCreationOptions &options) {
  /// TODO(Guyang Song): Make the information of TaskSpecification more reasonable
  /// We just reuse the TaskSpecification class and make the single process mode work.
  /// Maybe some infomation of TaskSpecification are not reasonable or invalid.
  /// We will enhance this after implement the cluster mode.
  auto functionDescriptor = FunctionDescriptorBuilder::BuildCpp(
      invocation.remote_function_holder.function_name);
  rpc::Address address;
  std::unordered_map<std::string, double> required_resources;
  std::unordered_map<std::string, double> required_placement_resources;
  TaskSpecBuilder builder;
  std::string task_name =
      invocation.name.empty() ? functionDescriptor->DefaultTaskName() : invocation.name;
  builder.SetCommonTaskSpec(invocation.task_id, task_name, rpc::Language::CPP,
                            functionDescriptor, local_mode_ray_tuntime_.GetCurrentJobID(),
                            local_mode_ray_tuntime_.GetCurrentTaskId(), 0,
                            local_mode_ray_tuntime_.GetCurrentTaskId(), address, 1,
                            required_resources, required_placement_resources,
                            std::make_pair(PlacementGroupID::Nil(), -1), true, "");
  if (invocation.task_type == TaskType::NORMAL_TASK) {
  } else if (invocation.task_type == TaskType::ACTOR_CREATION_TASK) {
    invocation.actor_id = local_mode_ray_tuntime_.GetNextActorID();
    builder.SetActorCreationTaskSpec(invocation.actor_id, /*serialized_actor_handle=*/"",
                                     options.max_restarts, /*max_task_retries=*/0, {},
                                     options.max_concurrency);
  } else if (invocation.task_type == TaskType::ACTOR_TASK) {
    const TaskID actor_creation_task_id =
        TaskID::ForActorCreationTask(invocation.actor_id);
    const ObjectID actor_creation_dummy_object_id =
        ObjectID::FromIndex(actor_creation_task_id, 1);
    builder.SetActorTaskSpec(invocation.actor_id, actor_creation_dummy_object_id,
                             ObjectID(), invocation.actor_counter);
  } else {
    throw RayException("unknown task type");
  }
  for (size_t i = 0; i < invocation.args.size(); i++) {
    builder.AddArg(*invocation.args[i]);
  }
  auto task_specification = builder.Build();
  ObjectID return_object_id = task_specification.ReturnId(0);

  std::shared_ptr<msgpack::sbuffer> actor;
  std::shared_ptr<absl::Mutex> mutex;
  if (invocation.task_type == TaskType::ACTOR_TASK) {
    absl::MutexLock lock(&actor_contexts_mutex_);
    actor = actor_contexts_.at(invocation.actor_id).get()->current_actor;
    mutex = actor_contexts_.at(invocation.actor_id).get()->actor_mutex;
  }
  AbstractRayRuntime *runtime = &local_mode_ray_tuntime_;
  if (invocation.task_type == TaskType::ACTOR_CREATION_TASK ||
      invocation.task_type == TaskType::ACTOR_TASK) {
    /// TODO(Guyang Song): Handle task dependencies.
    /// Execute actor task directly in the main thread because we must guarantee the actor
    /// task executed by calling order.
    TaskExecutor::Invoke(task_specification, actor, runtime, actor_contexts_,
                         actor_contexts_mutex_);
  } else {
    boost::asio::post(*thread_pool_.get(),
                      std::bind(
                          [actor, mutex, runtime, this](TaskSpecification &ts) {
                            if (mutex) {
                              absl::MutexLock lock(mutex.get());
                            }
                            TaskExecutor::Invoke(ts, actor, runtime,
                                                 this->actor_contexts_,
                                                 this->actor_contexts_mutex_);
                          },
                          std::move(task_specification)));
  }
  return return_object_id;
}

ObjectID LocalModeTaskSubmitter::SubmitTask(InvocationSpec &invocation,
                                            const CallOptions &call_options) {
  return Submit(invocation, {});
}

ActorID LocalModeTaskSubmitter::CreateActor(InvocationSpec &invocation,
                                            const ActorCreationOptions &create_options) {
  Submit(invocation, create_options);
  return invocation.actor_id;
}

ObjectID LocalModeTaskSubmitter::SubmitActorTask(InvocationSpec &invocation,
                                                 const CallOptions &call_options) {
  return Submit(invocation, {});
}

}  // namespace internal
}  // namespace ray
