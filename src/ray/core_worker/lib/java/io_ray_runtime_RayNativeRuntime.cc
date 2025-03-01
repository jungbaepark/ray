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

#include "io_ray_runtime_RayNativeRuntime.h"

#include <jni.h>

#include <sstream>

#include "jni_utils.h"
#include "ray/common/id.h"
#include "ray/core_worker/actor_handle.h"
#include "ray/core_worker/core_worker.h"

thread_local JNIEnv *local_env = nullptr;
jobject java_task_executor = nullptr;

/// Store Java instances of function descriptor in the cache to avoid unnessesary JNI
/// operations.
thread_local std::unordered_map<size_t,
                                std::vector<std::pair<FunctionDescriptor, jobject>>>
    executor_function_descriptor_cache;

inline gcs::GcsClientOptions ToGcsClientOptions(JNIEnv *env, jobject gcs_client_options) {
  std::string ip = JavaStringToNativeString(
      env, (jstring)env->GetObjectField(gcs_client_options, java_gcs_client_options_ip));
  int port = env->GetIntField(gcs_client_options, java_gcs_client_options_port);
  std::string password = JavaStringToNativeString(
      env,
      (jstring)env->GetObjectField(gcs_client_options, java_gcs_client_options_password));
  return gcs::GcsClientOptions(ip, port, password);
}

jobject ToJavaArgs(JNIEnv *env, jbooleanArray java_check_results,
                   const std::vector<std::shared_ptr<RayObject>> &args) {
  if (java_check_results == nullptr) {
    // If `java_check_results` is null, it means that `checkByteBufferArguments`
    // failed. In this case, just return null here. The args won't be used anyway.
    return nullptr;
  } else {
    jboolean *check_results = env->GetBooleanArrayElements(java_check_results, nullptr);
    size_t i = 0;
    jobject args_array_list = NativeVectorToJavaList<std::shared_ptr<RayObject>>(
        env, args,
        [check_results, &i](JNIEnv *env,
                            const std::shared_ptr<RayObject> &native_object) {
          if (*(check_results + (i++))) {
            // If the type of this argument is ByteBuffer, we create a
            // DirectByteBuffer here To avoid data copy.
            // TODO: Check native_object->GetMetadata() == "RAW"
            jobject obj = env->NewDirectByteBuffer(native_object->GetData()->Data(),
                                                   native_object->GetData()->Size());
            RAY_CHECK(obj);
            return obj;
          }
          return NativeRayObjectToJavaNativeRayObject(env, native_object);
        });
    env->ReleaseBooleanArrayElements(java_check_results, check_results, JNI_ABORT);
    return args_array_list;
  }
}

JNIEnv *GetJNIEnv() {
  JNIEnv *env = local_env;
  if (!env) {
    // Attach the native thread to JVM.
    auto status =
        jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void **>(&env), nullptr);
    RAY_CHECK(status == JNI_OK) << "Failed to get JNIEnv. Return code: " << status;
    local_env = env;
  }
  RAY_CHECK(env);
  return env;
}

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeInitialize(
    JNIEnv *env, jclass, jint workerMode, jstring nodeIpAddress, jint nodeManagerPort,
    jstring driverName, jstring storeSocket, jstring rayletSocket, jbyteArray jobId,
    jobject gcsClientOptions, jint numWorkersPerProcess, jstring logDir,
    jbyteArray jobConfig) {
  auto task_execution_callback =
      [](TaskType task_type, const std::string task_name, const RayFunction &ray_function,
         const std::unordered_map<std::string, double> &required_resources,
         const std::vector<std::shared_ptr<RayObject>> &args,
         const std::vector<ObjectID> &arg_reference_ids,
         const std::vector<ObjectID> &return_ids, const std::string &debugger_breakpoint,
         std::vector<std::shared_ptr<RayObject>> *results,
         std::shared_ptr<LocalMemoryBuffer> &creation_task_exception_pb) {
        JNIEnv *env = GetJNIEnv();
        RAY_CHECK(java_task_executor);

        // convert RayFunction
        auto function_descriptor = ray_function.GetFunctionDescriptor();
        size_t fd_hash = function_descriptor->Hash();
        auto &fd_vector = executor_function_descriptor_cache[fd_hash];
        jobject ray_function_array_list = nullptr;
        for (auto &pair : fd_vector) {
          if (pair.first == function_descriptor) {
            ray_function_array_list = pair.second;
            break;
          }
        }
        if (!ray_function_array_list) {
          ray_function_array_list =
              NativeRayFunctionDescriptorToJavaStringList(env, function_descriptor);
          fd_vector.emplace_back(function_descriptor, ray_function_array_list);
        }

        // convert args
        // TODO (kfstorm): Avoid copying binary data from Java to C++
        jbooleanArray java_check_results =
            static_cast<jbooleanArray>(env->CallObjectMethod(
                java_task_executor, java_task_executor_parse_function_arguments,
                ray_function_array_list));
        RAY_CHECK_JAVA_EXCEPTION(env);
        jobject args_array_list = ToJavaArgs(env, java_check_results, args);

        // invoke Java method
        jobject java_return_objects =
            env->CallObjectMethod(java_task_executor, java_task_executor_execute,
                                  ray_function_array_list, args_array_list);
        // Check whether the exception is `IntentionalSystemExit`.
        jthrowable throwable = env->ExceptionOccurred();
        if (throwable) {
          Status status_to_return = Status::OK();
          if (env->IsInstanceOf(throwable,
                                java_ray_intentional_system_exit_exception_class)) {
            status_to_return = Status::IntentionalSystemExit();
          } else if (env->IsInstanceOf(throwable, java_ray_actor_exception_class)) {
            creation_task_exception_pb = SerializeActorCreationException(env, throwable);
            status_to_return = Status::CreationTaskError();
          } else {
            RAY_LOG(ERROR) << "Unkown java exception was thrown while executing tasks.";
          }
          env->ExceptionClear();
          return status_to_return;
        }
        RAY_CHECK_JAVA_EXCEPTION(env);

        int64_t task_output_inlined_bytes = 0;
        // Process return objects.
        if (!return_ids.empty()) {
          std::vector<std::shared_ptr<RayObject>> return_objects;
          JavaListToNativeVector<std::shared_ptr<RayObject>>(
              env, java_return_objects, &return_objects,
              [](JNIEnv *env, jobject java_native_ray_object) {
                return JavaNativeRayObjectToNativeRayObject(env, java_native_ray_object);
              });
          results->resize(return_ids.size(), nullptr);
          for (size_t i = 0; i < return_objects.size(); i++) {
            auto &result_id = return_ids[i];
            size_t data_size =
                return_objects[i]->HasData() ? return_objects[i]->GetData()->Size() : 0;
            auto &metadata = return_objects[i]->GetMetadata();
            std::vector<ObjectID> contained_object_ids;
            for (const auto &ref : return_objects[i]->GetNestedRefs()) {
              contained_object_ids.push_back(ObjectID::FromBinary(ref.object_id()));
            }
            auto result_ptr = &(*results)[0];

            RAY_CHECK_OK(CoreWorkerProcess::GetCoreWorker().AllocateReturnObject(
                result_id, data_size, metadata, contained_object_ids,
                task_output_inlined_bytes, result_ptr));

            // A nullptr is returned if the object already exists.
            auto result = *result_ptr;
            if (result != nullptr) {
              if (result->HasData()) {
                memcpy(result->GetData()->Data(), return_objects[i]->GetData()->Data(),
                       data_size);
              }
            }

            RAY_CHECK_OK(
                CoreWorkerProcess::GetCoreWorker().SealReturnObject(result_id, result));
          }
        }

        env->DeleteLocalRef(java_check_results);
        env->DeleteLocalRef(java_return_objects);
        env->DeleteLocalRef(args_array_list);
        return Status::OK();
      };

  auto gc_collect = []() {
    // A Java worker process usually contains more than one worker.
    // A LocalGC request is likely to be received by multiple workers in a short time.
    // Here we ensure that the 1 second interval of `System.gc()` execution is
    // guaranteed no matter how frequent the requests are received and how many workers
    // the process has.
    static absl::Mutex mutex;
    static int64_t last_gc_time_ms = 0;
    absl::MutexLock lock(&mutex);
    int64_t start = current_time_ms();
    if (last_gc_time_ms + 1000 < start) {
      JNIEnv *env = GetJNIEnv();
      RAY_LOG(DEBUG) << "Calling System.gc() ...";
      env->CallStaticObjectMethod(java_system_class, java_system_gc);
      last_gc_time_ms = current_time_ms();
      RAY_LOG(DEBUG) << "GC finished in " << (double)(last_gc_time_ms - start) / 1000
                     << " seconds.";
    }
  };

  auto on_worker_shutdown = [](const WorkerID &worker_id) {
    JNIEnv *env = GetJNIEnv();
    auto worker_id_bytes = IdToJavaByteArray<WorkerID>(env, worker_id);
    if (java_task_executor) {
      env->CallVoidMethod(java_task_executor,
                          java_native_task_executor_on_worker_shutdown, worker_id_bytes);
      RAY_CHECK_JAVA_EXCEPTION(env);
    }
  };

  std::string serialized_job_config =
      (jobConfig == nullptr ? "" : JavaByteArrayToNativeString(env, jobConfig));
  CoreWorkerOptions options;
  options.worker_type = static_cast<WorkerType>(workerMode);
  options.language = Language::JAVA;
  options.store_socket = JavaStringToNativeString(env, storeSocket);
  options.raylet_socket = JavaStringToNativeString(env, rayletSocket);
  options.job_id = JavaByteArrayToId<JobID>(env, jobId);
  options.gcs_options = ToGcsClientOptions(env, gcsClientOptions);
  options.enable_logging = true;
  options.log_dir = JavaStringToNativeString(env, logDir);
  // TODO (kfstorm): JVM would crash if install_failure_signal_handler was set to true
  options.install_failure_signal_handler = false;
  options.node_ip_address = JavaStringToNativeString(env, nodeIpAddress);
  options.node_manager_port = static_cast<int>(nodeManagerPort);
  options.raylet_ip_address = JavaStringToNativeString(env, nodeIpAddress);
  options.driver_name = JavaStringToNativeString(env, driverName);
  options.task_execution_callback = task_execution_callback;
  options.on_worker_shutdown = on_worker_shutdown;
  options.gc_collect = gc_collect;
  options.num_workers = static_cast<int>(numWorkersPerProcess);
  options.serialized_job_config = serialized_job_config;

  CoreWorkerProcess::Initialize(options);
}

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeRunTaskExecutor(
    JNIEnv *env, jclass o, jobject javaTaskExecutor) {
  java_task_executor = javaTaskExecutor;
  CoreWorkerProcess::RunTaskExecutionLoop();
  java_task_executor = nullptr;

  // NOTE(kfstorm): It's possible that users spawn non-daemon Java threads. If these
  // threads are not stopped before exiting `RunTaskExecutionLoop`, the JVM won't exit but
  // Raylet has unregistered this worker. In this case, even if the job has finished, the
  // worker process won't be killed by Raylet and it results in an orphan worker.
  // TO fix this, we explicitly quit the process here. This only affects worker processes,
  // not driver processes because only worker processes call `RunTaskExecutionLoop`.
  _Exit(0);
}

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeShutdown(JNIEnv *env,
                                                                           jclass o) {
  CoreWorkerProcess::Shutdown();
}

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeSetResource(
    JNIEnv *env, jclass, jstring resourceName, jdouble capacity, jbyteArray nodeId) {
  const auto node_id = JavaByteArrayToId<NodeID>(env, nodeId);
  const char *native_resource_name = env->GetStringUTFChars(resourceName, JNI_FALSE);

  auto status = CoreWorkerProcess::GetCoreWorker().SetResource(
      native_resource_name, static_cast<double>(capacity), node_id);
  env->ReleaseStringUTFChars(resourceName, native_resource_name);
  THROW_EXCEPTION_AND_RETURN_IF_NOT_OK(env, status, (void)0);
}

JNIEXPORT jbyteArray JNICALL
Java_io_ray_runtime_RayNativeRuntime_nativeGetActorIdOfNamedActor(JNIEnv *env, jclass,
                                                                  jstring actor_name,
                                                                  jboolean global) {
  const char *native_actor_name = env->GetStringUTFChars(actor_name, JNI_FALSE);
  auto full_name = GetFullName(global, native_actor_name);

  const auto pair = CoreWorkerProcess::GetCoreWorker().GetNamedActorHandle(
      full_name, /*ray_namespace=*/"");
  const auto status = pair.second;
  if (status.IsNotFound()) {
    return IdToJavaByteArray<ActorID>(env, ActorID::Nil());
  }
  THROW_EXCEPTION_AND_RETURN_IF_NOT_OK(env, status, nullptr);
  const auto actor_handle = pair.first;
  return IdToJavaByteArray<ActorID>(env, actor_handle->GetActorID());
}

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeKillActor(
    JNIEnv *env, jclass, jbyteArray actorId, jboolean noRestart) {
  auto status = CoreWorkerProcess::GetCoreWorker().KillActor(
      JavaByteArrayToId<ActorID>(env, actorId),
      /*force_kill=*/true, noRestart);
  THROW_EXCEPTION_AND_RETURN_IF_NOT_OK(env, status, (void)0);
}

JNIEXPORT void JNICALL Java_io_ray_runtime_RayNativeRuntime_nativeSetCoreWorker(
    JNIEnv *env, jclass, jbyteArray workerId) {
  const auto worker_id = JavaByteArrayToId<WorkerID>(env, workerId);
  CoreWorkerProcess::SetCurrentThreadWorkerId(worker_id);
}

#ifdef __cplusplus
}
#endif
