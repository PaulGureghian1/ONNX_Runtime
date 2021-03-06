// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/parallel_executor.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "core/common/common.h"
#include "core/common/logging/logging.h"

#ifndef USE_EIGEN_THREADPOOL
#include "core/common/task_thread_pool.h"
#endif

#include "core/framework/allocation_planner.h"
#include "core/framework/execution_frame.h"
#include "core/framework/session_state.h"
#include "core/framework/op_kernel_context_internal.h"

namespace onnxruntime {

ParallelExecutor::ParallelExecutor(const SessionState& session_state, const bool& terminate_flag)
    : out_standings_(0), terminate_flag_{terminate_flag} {
  auto graph_viewer = session_state.GetGraphViewer();
  node_refs_.resize(graph_viewer->MaxNodeIndex());
  for (auto& node : graph_viewer->Nodes()) {
    node_refs_[node.Index()] = node.GetInputEdgesCount();
  }
}

Status ParallelExecutor::Execute(const SessionState& session_state,
                                 const NameMLValMap& feeds,
                                 const std::vector<std::string>& output_names,
                                 std::vector<MLValue>& fetches,
                                 const logging::Logger& logger) {
  TimePoint tp;
  bool f_profiler_enabled = session_state.Profiler().FEnabled();
  if (f_profiler_enabled) {
    tp = session_state.Profiler().StartTime();
  }

  root_frame_ = std::make_unique<ExecutionFrame>(feeds, output_names, fetches, session_state);
  //std::cout << "start nodes:" << std::endl;
  for (auto node_index : session_state.GetGraphViewer()->GetRootNodes()) {
    auto p_op_kernel = session_state.GetKernel(node_index);
    if (!p_op_kernel)
      continue;

    //std::cout << "\t" << p_op_kernel->Node().Name() << std::endl;
    EnqueueNode(node_index, session_state, logger);
  }

  // Wait for finish.
  {
    std::unique_lock<OrtMutex> lock(complete_mutex_);
    while (out_standings_ > 0) complete_cv_.wait(lock);
  }

  VLOGS(logger, 1) << "Fetching output.";
  ORT_RETURN_IF_ERROR(FetchOutput(session_state.GetMLValueNameIdxMap(), *root_frame_, output_names, fetches, logger));

  if (root_frame_->HasPlan()) {
    std::vector<TensorShape> input_shapes;
    bool all_tensors = true;
    for (const auto& feed : feeds) {
      if (!(feed.second.IsTensor())) {
        all_tensors = false;
        break;
      }
      auto& tensor = feed.second.Get<Tensor>();
      input_shapes.push_back(tensor.Shape());
    }

    if (all_tensors) {
      auto mem_patterns = std::make_unique<MemoryPatternGroup>();
      ORT_RETURN_IF_ERROR(root_frame_->GeneratePatterns(mem_patterns.get()));
      ORT_RETURN_IF_ERROR(session_state.UpdateMemoryPatternGroupCache(input_shapes, std::move(mem_patterns)));
    }
  }

  if (f_profiler_enabled) {
    session_state.Profiler().EndTimeAndRecordEvent(profiling::SESSION_EVENT, "ParallelExecutor::Execute", tp);
  }
  return Status::OK();
}

void ParallelExecutor::RunNodeAsync(size_t p_node_index,
                                    const SessionState& session_state,
                                    const logging::Logger& logger) {
  try {
    RunNodeAsyncInternal(p_node_index, session_state, logger);
  } catch (...) {
    FinishNodeRun();
    throw;
  }
}

void ParallelExecutor::RunNodeAsyncInternal(size_t p_node_index,
                                            const SessionState& session_state,
                                            const logging::Logger& logger) {
  LOGS(logger, INFO) << "Begin execution";

  size_t node_index = p_node_index;
  bool keep_running = true;
  auto graph_viewer = session_state.GetGraphViewer();
  TimePoint sync_time_begin;
  TimePoint kernel_begin_time;
  bool f_profiler_enabled = session_state.Profiler().FEnabled();
  // Avoid context switching if possible.
  while (keep_running) {
    // TODO: Convert RunNodeAsync return Status.
    // to also handle exception propagation
    if (terminate_flag_) {
      LOGS(logger, WARNING) << "Exiting due to terminate flag being set to true.";
      ORT_THROW("Exiting due to terminate flag being set to true.");
    }

    auto p_op_kernel = session_state.GetKernel(node_index);

    // if a kernel has been added in the session state, it better be NON-null.
    if (p_op_kernel == nullptr) {
      ORT_THROW("Got nullptr from GetKernel for node: ",
                graph_viewer->GetNode(node_index)->Name());
    }

    OpKernelContextInternal op_kernel_context(*root_frame_, *p_op_kernel, logger,
                                              p_op_kernel->Node().ImplicitInputDefs(),
                                              terminate_flag_);

    if (f_profiler_enabled) {
      sync_time_begin = session_state.Profiler().StartTime();
    }
    // sync before compute
    int queue_id = p_op_kernel->KernelDef().ExecQueueId();

    for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
      Fence_t fence = op_kernel_context.InputFence(input_index);
      if (fence) {
        auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
        if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
          execution_provider_type = kCpuExecutionProvider;
        }
        fence->BeforeUsingAsInput(execution_provider_type, queue_id);
      }
    }

    for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
      Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
      if (fence) {
        auto execution_provider_type = p_op_kernel->Node().GetExecutionProviderType();
        if (OrtMemTypeCPUInput == p_op_kernel->KernelDef().InputMemoryType(input_index)) {
          execution_provider_type = kCpuExecutionProvider;
        }
        fence->BeforeUsingAsInput(execution_provider_type, queue_id);
      }
    }

    for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
      Fence_t fence = op_kernel_context.OutputFence(output_index);
      if (fence) {
        fence->BeforeUsingAsOutput(p_op_kernel->Node().GetExecutionProviderType(), queue_id);
      }
    }

    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_fence_before",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});

      kernel_begin_time = session_state.Profiler().StartTime();
    }

    // call compute on the kernel
    VLOGS(logger, 1) << "Computing kernel: " << p_op_kernel->Node().Name();

    // Execute the kernel.
    auto status = p_op_kernel->Compute(&op_kernel_context);
    if (!status.IsOK()) {
      ORT_THROW("Compute failed for node: ", graph_viewer->GetNode(node_index)->Name());
    }
    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_kernel_time",
                                                     kernel_begin_time,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});

      sync_time_begin = session_state.Profiler().StartTime();
    }
    // sync after compute for outputs
    for (int input_index = 0; input_index < op_kernel_context.InputCount(); ++input_index) {
      Fence_t fence = op_kernel_context.InputFence(input_index);
      if (fence) {
        fence->AfterUsedAsInput(queue_id);
      }
    }

    for (int input_index = 0; input_index < op_kernel_context.ImplicitInputCount(); ++input_index) {
      Fence_t fence = op_kernel_context.ImplicitInputFence(input_index);
      if (fence) {
        fence->AfterUsedAsInput(queue_id);
      }
    }

    for (int output_index = 0; output_index < op_kernel_context.OutputCount(); ++output_index) {
      Fence_t fence = op_kernel_context.OutputFence(output_index);
      if (fence) {
        fence->AfterUsedAsOutput(queue_id);
      }
    }
    if (f_profiler_enabled) {
      session_state.Profiler().EndTimeAndRecordEvent(profiling::NODE_EVENT,
                                                     p_op_kernel->Node().Name() + "_fence_after",
                                                     sync_time_begin,
                                                     {{"op_name", p_op_kernel->KernelDef().OpName()}});
    }
    //std::cout << "Run async node finish: " << p_node_index << std::endl;

    keep_running = false;

    // Checking which output nodes ready for running.
    {
      auto begin = p_op_kernel->Node().OutputEdgesBegin();
      auto end = p_op_kernel->Node().OutputEdgesEnd();

      std::lock_guard<OrtMutex> lock(ref_mutex_);
      for (auto it = begin; it != end; it++) {
        auto idx = (*it).GetNode().Index();
        if ((--node_refs_[idx]) == 0) {
          if (!keep_running) {
            node_index = idx;
            keep_running = true;
          } else {
            EnqueueNode(idx, session_state, logger);
          }
        }

        //std::cout << "handle output, current name: " << p_op_kernel->Node().Name() << ", current index: " << p_node_index << ", output name: " << (*it)->GetNode().Name() << ", output index: " << (*it)->GetNode().Index() << ", after -- output ref: " << node_refs_[idx] << std::endl;
      }
    }
  }

  FinishNodeRun();
}

void ParallelExecutor::EnqueueNode(size_t p_node_index, const SessionState& session_state, const logging::Logger& logger) {
  {
    std::unique_lock<OrtMutex> lock(complete_mutex_);
    out_standings_++;
  }

#ifdef USE_EIGEN_THREADPOOL
  session_state.GetThreadPool()->Schedule([this, p_node_index, &session_state, &logger]() {
    try {
      ParallelExecutor::RunNodeAsync(p_node_index, std::cref(session_state), std::cref(logger));
    } catch (...) {
      // catch node processing failure exceptions here to prevent app crash.
    }
  });
#else
  std::packaged_task<void()> task{std::bind(&ParallelExecutor::RunNodeAsync, this, p_node_index, std::cref(session_state), std::cref(logger))};
  session_state.GetThreadPool()->RunTask(std::move(task));
#endif
}

Status ParallelExecutor::FetchOutput(const MLValueNameIdxMap& name_idx_map,
                                     ExecutionFrame& frame,
                                     const std::vector<std::string>& output_names,
                                     std::vector<MLValue>& fetches,
                                     const logging::Logger& logger) {
  if (fetches.empty()) {
    fetches.resize(output_names.size());
  } else {
    // this should've been checked before already
    ORT_ENFORCE(output_names.size() == fetches.size(),
                "output_names vector size: " + std::to_string(output_names.size()) +
                    " does not match that of fetches vector: " + std::to_string(fetches.size()));
  }

  auto idx = 0;

  for (const auto& oname : output_names) {
    VLOGS(logger, 1) << "Attempting to fetch output with name: " << oname;
    int mlvalue_index;
    ORT_RETURN_IF_ERROR(name_idx_map.GetIdx(oname, mlvalue_index));
    const MLValue& output_mlvalue = frame.GetMLValue(mlvalue_index);
    VLOGS(logger, 1) << "Copying fetched MLValue to output vector";
    fetches[idx++] = output_mlvalue;
  }

  VLOGS(logger, 1) << "Done with execution.";
  return Status::OK();
}

}  // namespace onnxruntime
