/* Copyright 2017 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_BACKENDS_GPU_RUNTIME_WHILE_THUNK_H_
#define XLA_BACKENDS_GPU_RUNTIME_WHILE_THUNK_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "xla/backends/gpu/runtime/host_memory_pool.h"
#include "xla/backends/gpu/runtime/sequential_thunk.h"
#include "xla/backends/gpu/runtime/thunk.h"
#include "xla/backends/gpu/runtime/thunk.pb.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/buffer_assignment.h"
#include "xla/stream_executor/stream_executor.h"

namespace xla {
namespace gpu {

// WhileThunk implements the while instruction on GPU by invoking a thunk
// sequence for the while 'condition' computation, and (conditionally) another
// thunk sequence for the while 'body' computation. WhileThunk assumes that
// buffers for the following set of while-related instructions share the same
// allocation:
//   init, condition.parameter, body.parameter, body.root, while.result
//
// WhileThunk synchronizes the stream to test the result of the 'condition'
// computation.
//
// If `trip_count` is available it means that the while loop trip count is known
// statically and while loop is actually a for loop, and in this case at run
// time condition thunk might not be executed and instead body thunk will be
// executed for `trip_count` times.
class WhileThunk : public Thunk {
 public:
  // Constructs a WhileThunk to compute while instruction 'hlo'.
  WhileThunk(ThunkInfo thunk_info, const HloInstruction* loop,
             const BufferAllocation::Slice& condition_result_buffer_index,
             std::unique_ptr<SequentialThunk> condition_thunk_sequence,
             std::unique_ptr<SequentialThunk> body_thunk_sequence,
             std::optional<int64_t> trip_count = std::nullopt);
  WhileThunk(const WhileThunk&) = delete;
  WhileThunk& operator=(const WhileThunk&) = delete;

  absl::Status Prepare(const PrepareParams& params,
                       ResourceRequestsInterface& resource_requests) override;
  absl::Status Initialize(const InitializeParams& params) override;
  absl::Status ExecuteOnStream(const ExecuteParams& params) override;

  SequentialThunk* condition_thunk_sequence() const {
    return condition_thunk_sequence_.get();
  }

  SequentialThunk* body_thunk_sequence() const {
    return body_thunk_sequence_.get();
  }

  const BufferAllocation::Slice& condition_result_buffer() const {
    return condition_result_buffer_index_;
  }

  std::optional<int64_t> trip_count() const { return trip_count_; }

  // Returns the current loop iteration if the caller is inside a while loop(s).
  //
  // Implementation relies on thread local storage, be careful when call it from
  // code running on multiple threads.
  static absl::StatusOr<int64_t> CurrentLoopIteration(int64_t depth = 0);
  static absl::StatusOr<int64_t> CurrentLoopIteration(
      const HloInstruction* while_instr);

  void ForAllThunks(absl::FunctionRef<void(const Thunk*)> fn) const override;

  std::string ToString(int indent) const override;

  absl::StatusOr<ThunkProto> ToProto() const override;

  // Deserializes a WhileThunk from its proto representation.
  // Parameters:
  // - thunk_info: Metadata about the thunk
  // - thunk_proto: Serialized WhileThunk proto message.
  // - buffer_allocations: Buffer allocations available for use by the thunk.
  // - deserializer: Callable (e.g., lambda) for deserializing nested thunks.
  //
  // Returns a unique_ptr to a WhileThunk on success, or an error status on
  // failure.
  static absl::StatusOr<std::unique_ptr<WhileThunk>> FromProto(
      ThunkInfo thunk_info, const WhileThunkProto& thunk_proto,
      absl::Span<const BufferAllocation> buffer_allocations,
      const Deserializer& deserializer);

 private:
  const HloInstruction* loop_;
  const BufferAllocation::Slice condition_result_buffer_index_;
  std::unique_ptr<SequentialThunk> condition_thunk_sequence_;
  std::unique_ptr<SequentialThunk> body_thunk_sequence_;
  std::optional<int64_t> trip_count_;

  // Host memory pool for transfering predicate value from device to host.
  absl::Mutex mutex_;
  absl::flat_hash_map<se::StreamExecutor*, std::unique_ptr<HostMemoryPool>>
      host_memory_pools_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace gpu
}  // namespace xla

#endif  // XLA_BACKENDS_GPU_RUNTIME_WHILE_THUNK_H_
