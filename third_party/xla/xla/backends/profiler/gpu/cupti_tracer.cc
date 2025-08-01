/* Copyright 2019 The OpenXLA Authors.

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

#include "xla/backends/profiler/gpu/cupti_tracer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <list>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_activity.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_callbacks.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_driver_cbid.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_result.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_target.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "xla/backends/profiler/gpu/cupti_buffer_events.h"
#include "xla/backends/profiler/gpu/cupti_collector.h"
#include "xla/backends/profiler/gpu/cupti_interface.h"
#include "xla/backends/profiler/gpu/cupti_pm_sampler_factory.h"
#include "xla/tsl/platform/env.h"
#include "xla/tsl/platform/errors.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/tsl/profiler/backends/cpu/annotation_stack.h"
#include "xla/tsl/profiler/utils/per_thread.h"
#include "tsl/platform/host_info.h"
#include "tsl/platform/thread_annotations.h"

namespace xla {
namespace profiler {

namespace {

using tsl::Env;
using tsl::profiler::AnnotationStack;

static thread_local int internalCuCall = 0;

// Temporary disable cupti api tracing for this thread during the life scope of
// this class. Used for the API calls that initiated by us.
class CuptiApiTracingDisabler {
 public:
  CuptiApiTracingDisabler() { internalCuCall++; }
  ~CuptiApiTracingDisabler() { internalCuCall--; }
};

// CUPTI_ERROR_INSUFFICIENT_PRIVILEGES is introduced at CUDA 10.1.
#if CUDA_VERSION <= 10000
#define CUPTI_ERROR_INSUFFICIENT_PRIVILEGES 35
#endif

#define RETURN_IF_CUPTI_ERROR(expr)                                           \
  do {                                                                        \
    CUptiResult status = (cupti_interface_->expr);                            \
    if (ABSL_PREDICT_FALSE(status != CUPTI_SUCCESS)) {                        \
      const char* errstr = "";                                                \
      cupti_interface_->GetResultString(status, &errstr);                     \
      LOG(ERROR) << "function " << #expr << "failed with error " << errstr;   \
      if (status == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES) {                    \
        return absl::PermissionDeniedError("CUPTI needs root access");        \
      }                                                                       \
      return absl::InternalError(absl::StrCat("CUPTI call error: ", errstr)); \
    }                                                                         \
  } while (false)

size_t Bytes2D(const CUDA_MEMCPY2D* p) { return p->Height * p->WidthInBytes; }

size_t Bytes3D(const CUDA_MEMCPY3D* p) {
  return p->Depth * p->Height * p->WidthInBytes;
}

template <typename CudaMemcpy>
CuptiTracerEventType MemcpyKind(const CudaMemcpy* p) {
  if (p->srcMemoryType == CU_MEMORYTYPE_HOST &&
      p->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    return CuptiTracerEventType::MemcpyH2D;
  }
  if (p->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
      p->dstMemoryType == CU_MEMORYTYPE_HOST) {
    return CuptiTracerEventType::MemcpyD2H;
  }
  if (p->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
      p->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    return CuptiTracerEventType::MemcpyD2D;
  }
  return CuptiTracerEventType::Unsupported;
}

std::tuple<size_t /*bytes*/, CuptiTracerEventType, bool /*async*/>
DecodeDriverMemcpy(CUpti_CallbackId cbid, const void* params) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2: {
      const auto* p = reinterpret_cast<const cuMemcpyHtoD_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyH2D,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2: {
      const auto* p =
          reinterpret_cast<const cuMemcpyHtoDAsync_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyH2D,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2: {
      const auto* p = reinterpret_cast<const cuMemcpyDtoH_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2H,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2: {
      const auto* p =
          reinterpret_cast<const cuMemcpyDtoHAsync_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2H,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2: {
      const auto* p = reinterpret_cast<const cuMemcpyDtoD_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2D,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2: {
      const auto* p =
          reinterpret_cast<const cuMemcpyDtoDAsync_v2_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2D,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy: {
      const auto* p = reinterpret_cast<const cuMemcpy_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyOther,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync: {
      const auto* p = reinterpret_cast<const cuMemcpyAsync_params*>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyOther,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2: {
      const auto* p = reinterpret_cast<const cuMemcpy2D_v2_params*>(params);
      return std::make_tuple(Bytes2D(p->pCopy), MemcpyKind(p->pCopy), false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2: {
      const auto* p =
          reinterpret_cast<const cuMemcpy2DAsync_v2_params*>(params);
      return std::make_tuple(Bytes2D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2: {
      const auto* p = reinterpret_cast<const cuMemcpy3D_v2_params*>(params);
      return std::make_tuple(Bytes3D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2: {
      const auto* p =
          reinterpret_cast<const cuMemcpy3DAsync_v2_params*>(params);
      return std::make_tuple(Bytes3D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer: {
      const auto* p2p_params =
          reinterpret_cast<const cuMemcpyPeer_params*>(params);
      return std::make_tuple(p2p_params->ByteCount,
                             CuptiTracerEventType::MemcpyP2P, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync: {
      const auto* p2p_params =
          reinterpret_cast<const cuMemcpyPeerAsync_params*>(params);
      return std::make_tuple(p2p_params->ByteCount,
                             CuptiTracerEventType::MemcpyP2P, true);
    }
    default: {
      LOG(ERROR) << "Unsupported memcpy activity observed: " << cbid;
      return std::make_tuple(0, CuptiTracerEventType::Unsupported, false);
    }
  }
}

std::tuple<size_t /*bytes*/, CuptiTracerEventType, bool /*async*/>
DecodeDriverMemset(CUpti_CallbackId cbid, const void* params) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD8_v2_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD16_v2_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD32_v2_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD2D8_v2_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD2D16_v2_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32_v2: {
      const auto* p = reinterpret_cast<const cuMemsetD2D32_v2_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8Async: {
      const auto* p = reinterpret_cast<const cuMemsetD8Async_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16Async: {
      const auto* p = reinterpret_cast<const cuMemsetD16Async_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32Async: {
      const auto* p = reinterpret_cast<const cuMemsetD32Async_params*>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8Async: {
      const auto* p = reinterpret_cast<const cuMemsetD2D8Async_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16Async: {
      const auto* p =
          reinterpret_cast<const cuMemsetD2D16Async_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32Async: {
      const auto* p =
          reinterpret_cast<const cuMemsetD2D32Async_params*>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    default: {
      LOG(ERROR) << "Unsupported memset activity observed: " << cbid;
      return std::make_tuple(0, CuptiTracerEventType::Unsupported, false);
    }
  }
}

// Cupti callback corresponding to a driver or runtime API. This global function
// is invoked twice for each API: at entry and at exit. The cbdata
// parameter is guaranteed by Cupti to be thread-safe. Most invocations are
// dropped to the floor and entry/exit is tracked for the APIs we deem
// performance-relevant.
void CUPTIAPI ApiCallback(void* user_data, CUpti_CallbackDomain domain,
                          CUpti_CallbackId cbid,
                          const CUpti_CallbackData* cbdata) {
  CuptiTracer* tracer = reinterpret_cast<CuptiTracer*>(user_data);
  tracer->HandleCallback(domain, cbid, cbdata).IgnoreError();
}

// Callback which is invoked when an empty buffer is requested by CUPTI.
// Allocates an empty aligned-memory buffer. The buffer is used by CUPTI as a
// ring buffer where device maintains activity profiles that have been
// collected.
void CUPTIAPI RequestCuptiActivityBuffer(uint8_t** buffer, size_t* size,
                                         size_t* maxNumRecords) {
  CuptiTracer::GetCuptiTracerSingleton()->RequestActivityBuffer(buffer, size);
  VLOG(3) << "Requested CUPTI Buffer, buffer=" << std::hex
          << reinterpret_cast<uintptr_t>(*buffer) << std::dec
          << " size=" << *size;
  // Request CUPTI to fill as many records as possible in the buffer.
  *maxNumRecords = 0;
}

// Callback which is invoked when a buffer containing activity records is
// available from CUPTI. Processes the buffer after reading activity records
// from it.
void CUPTIAPI ProcessCuptiActivityBuffer(CUcontext context, uint32_t stream_id,
                                         uint8_t* buffer, size_t size,
                                         size_t valid_size) {
  VLOG(3) << "Processing CUPTI Buffer, buffer:" << std::hex
          << reinterpret_cast<uintptr_t>(buffer) << std::dec
          << " size: " << size << " valid_size: " << valid_size;
  VLOG(3) << "Activity profile for stream " << stream_id;

  absl::Status status =
      CuptiTracer::GetCuptiTracerSingleton()->ProcessActivityBuffer(
          context, stream_id, buffer, valid_size);
  if (!status.ok()) {
    LOG(ERROR) << status;
  }
}

void SetKernelEventUponApiExit(CuptiTracerEvent& event, uint32_t device_id,
                               const CUpti_CallbackData* cbdata,
                               uint64_t start_time, uint64_t end_time) {
  event.type = CuptiTracerEventType::Kernel;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->symbolName ? cbdata->symbolName : cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  VLOG(3) << "Cuda Kernel launch API exit. name=" << event.name;
}

// Performs the actual callback for both normal and P2P memcpy operations.
void PopulateMemcpyCallbackEvent(CuptiTracerEvent& event,
                                 CuptiTracerEventType type,
                                 const CUpti_CallbackData* cbdata,
                                 size_t num_bytes, uint32_t src_device,
                                 uint32_t dst_device, bool async,
                                 uint64_t start_time, uint64_t end_time) {
  event.type = type;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = src_device;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memcpy_info.num_bytes = num_bytes;
  event.memcpy_info.destination = dst_device;
  event.memcpy_info.async = async;
  // These are not populated during callback for API activities.
  event.memcpy_info.copy_kind = CUPTI_ACTIVITY_MEMCPY_KIND_UNKNOWN;
  event.memcpy_info.dst_mem_kind = CUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN;
  event.memcpy_info.src_mem_kind = CUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN;
}

void SetNormalMemcpyEventUponApiExit(CuptiTracerEvent& event,
                                     uint32_t device_id, CUpti_CallbackId cbid,
                                     const CUpti_CallbackData* cbdata,
                                     uint64_t start_time, uint64_t end_time) {
  size_t num_bytes;
  CuptiTracerEventType type;
  bool async;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemcpy(cbid, cbdata->functionParams);

  VLOG(3) << "Cuda Memcpy API exit. sz=" << num_bytes;
  PopulateMemcpyCallbackEvent(event, type, cbdata, num_bytes, device_id,
                              device_id, async, start_time, end_time);
}

void SetCuMemsetEventUponApiExit(CuptiTracerEvent& event, uint32_t device_id,
                                 CUpti_CallbackId cbid,
                                 const CUpti_CallbackData* cbdata,
                                 uint64_t start_time, uint64_t end_time) {
  // We are casting all variants of cuMemset to cuMemsetD8 for accessing the
  // first member attribute, a CUdeviceptr.
  const auto* params =
      static_cast<const cuMemsetD8_v2_params*>(cbdata->functionParams);
  size_t num_bytes;
  bool async;
  CuptiTracerEventType type;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemset(cbid, cbdata->functionParams);

  event.type = type;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memset_info.num_bytes = num_bytes;
  // memset_info.kind cannot be determined from API.
  event.memset_info.async = async;
  VLOG(3) << "Cuda Memset API exit."
          << " dptr=" << reinterpret_cast<void*>(params->dstDevice)
          << " sz=" << num_bytes;
}

void SetP2PMemcpyEventUponApiExit(CuptiTracerEvent& event,
                                  CuptiInterface* cupti_interface,
                                  uint32_t device_id, CUpti_CallbackId cbid,
                                  const CUpti_CallbackData* cbdata,
                                  uint64_t start_time, uint64_t end_time) {
  size_t num_bytes;
  CuptiTracerEventType type;
  bool async;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemcpy(cbid, cbdata->functionParams);

  uint32_t dst_device = -1, src_device = -1;
  const auto* p2p_params =
      static_cast<const cuMemcpyPeer_params*>(cbdata->functionParams);
  cupti_interface->GetDeviceId(p2p_params->srcContext, &src_device);
  cupti_interface->GetDeviceId(p2p_params->dstContext, &dst_device);
  VLOG(3) << "Cuda P2P Memcpy API exit, src: " << src_device
          << " dst: " << dst_device << " size:" << num_bytes;
  PopulateMemcpyCallbackEvent(event, type, cbdata, num_bytes, src_device,
                              dst_device, async, start_time, end_time);
}

void SetCuMemAllocEventUponApiExit(CuptiTracerEvent& event, uint32_t device_id,
                                   CUpti_CallbackId cbid,
                                   const CUpti_CallbackData* cbdata,
                                   uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemAlloc_v2_params*>(cbdata->functionParams);
  const void* dptr = reinterpret_cast<void*>(*params->dptr);
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAlloc API exit."
          << " dptr=" << dptr << " sz=" << params->bytesize;
}

void SetCuMemAllocPitchEventUponApiExit(
    CuptiTracerEvent& event, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData* cbdata, uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemAllocPitch_v2_params*>(cbdata->functionParams);
  const void* dptr = reinterpret_cast<void*>(*params->dptr);
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  const size_t size_in_bytes = *params->pPitch * params->Height;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = size_in_bytes;
  VLOG(3) << "Cuda MemAllocPitch API exit."
          << " dptr=" << dptr << " sz=" << size_in_bytes;
}

void SetCuMemAllocManagedEventUponApiExit(
    CuptiTracerEvent& event, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData* cbdata, uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemAllocManaged_params*>(cbdata->functionParams);
  const void* dptr = reinterpret_cast<void*>(*params->dptr);
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAllocManaged API exit."
          << " dptr=" << dptr << " sz=" << params->bytesize;
}

void SetCuMemAllocHostEventUponApiExit(CuptiTracerEvent& event,
                                       uint32_t device_id,
                                       CUpti_CallbackId cbid,
                                       const CUpti_CallbackData* cbdata,
                                       uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemAllocHost_v2_params*>(cbdata->functionParams);
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(*params->pp);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAllocHost API exit."
          << " pp=" << *params->pp << " sz=" << params->bytesize;
}

void SetCuMemHostAllocEventUponApiExit(CuptiTracerEvent& event,
                                       uint32_t device_id,
                                       CUpti_CallbackId cbid,
                                       const CUpti_CallbackData* cbdata,
                                       uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemHostAlloc_params*>(cbdata->functionParams);
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(*params->pp);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemHostAlloc API exit."
          << " pp=" << *params->pp << " sz=" << params->bytesize
          << " Flags=" << params->Flags;
}

void SetCuMemFreeEventUponApiExit(CuptiTracerEvent& event, uint32_t device_id,
                                  CUpti_CallbackId cbid,
                                  const CUpti_CallbackData* cbdata,
                                  uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemFree_v2_params*>(cbdata->functionParams);
  const void* dptr = reinterpret_cast<void*>(params->dptr);
  event.type = CuptiTracerEventType::MemoryFree;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memfree_info.address = reinterpret_cast<uintptr_t>(dptr);
  VLOG(3) << "Cuda MemFree API exit."
          << " dptr=" << dptr;
}

void SetCuMemFreeHostEventUponApiExit(CuptiTracerEvent& event,
                                      uint32_t device_id, CUpti_CallbackId cbid,
                                      const CUpti_CallbackData* cbdata,
                                      uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemFreeHost_params*>(cbdata->functionParams);
  event.type = CuptiTracerEventType::MemoryFree;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memfree_info.address = reinterpret_cast<uintptr_t>(params->p);
  VLOG(3) << "Cuda MemFreeHost API exit."
          << " p=" << params->p;
}

void SetCuMemHostRegisterEventUponApiExit(
    CuptiTracerEvent& event, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData* cbdata, uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemHostRegister_v2_params*>(cbdata->functionParams);
  event.type = CuptiTracerEventType::HostRegister;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.host_register_info.address = reinterpret_cast<uintptr_t>(params->p);
  event.host_register_info.num_bytes = params->bytesize;
  event.host_register_info.flags = params->Flags;
  VLOG(3) << "Cuda HostRegister API exit."
          << " p=" << params->p << " bytesize=" << params->bytesize
          << " flags=" << params->Flags;
}

void SetCuMemHostUnregisterEventUponApiExit(
    CuptiTracerEvent& event, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData* cbdata, uint64_t start_time, uint64_t end_time) {
  const auto* params =
      static_cast<const cuMemHostUnregister_params*>(cbdata->functionParams);
  event.type = CuptiTracerEventType::HostUnregister;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.host_unregister_info.address = reinterpret_cast<uintptr_t>(params->p);
  VLOG(3) << "Cuda HostUnregister API exit."
          << " p=" << params->p;
}

struct GraphResourceCreationInfo {
  uint32_t graph_id = 0;
  uint32_t orig_graph_id = 0;
  absl::flat_hash_map<uint64_t, uint64_t> node_id_map;
};

static GraphResourceCreationInfo& GetGraphResourceCreationInfo() {
  static thread_local GraphResourceCreationInfo per_thread_graph_info;
  return per_thread_graph_info;
}

// This class is instantiated per thread. The contention will happen at the
// moment of start/stop the tracing, when control thread is clearing all thread
// local data, while worker threads are injecting events. The mutex in practice
// will have no contention at all, so still cheap.
class GuardedCallbackAnnotationsAndEvents {
 public:
  CallbackAnnotationsAndEvents Consume() {
    absl::MutexLock lock(&mu_);
    CallbackAnnotationsAndEvents grabbed;
    std::swap(grabbed, annotations_and_events_);
    return grabbed;
  }

  void Clear() {
    absl::MutexLock lock(&mu_);
    annotations_and_events_.Clear();
  }

  void IncNumDroppedEvents() {
    absl::MutexLock lock(&mu_);
    annotations_and_events_.IncNumDroppedEvents();
  }

  void Push(const CuptiTracer& tracer, CuptiTracerEvent&& event) {
    absl::MutexLock lock(&mu_);
    // Some logic change as no cross thread string comparison should be
    // made here. The max_annotation_string is used to limit per-thread
    // annotation string count. And annotation string is not collected
    // if total callback event count overflow.
    bool too_many_annotations = tracer.TooManyAnnotationStrings(
        annotations_and_events_.NumAnnotations());
    event.annotation = annotations_and_events_.DedupAnnotation(
        too_many_annotations ? absl::string_view() : event.annotation),
    event.nvtx_range = annotations_and_events_.DedupNvtxRange(
        too_many_annotations ? absl::string_view() : event.nvtx_range);
    annotations_and_events_.event_queue().Push(std::move(event));
  }

  void AddScopeRangeIdSequence(absl::Span<const int64_t> sequence) {
    if (sequence.size() > 1) {
      const int64_t* head = sequence.data();
      const int64_t* curr = &sequence.back();

      absl::MutexLock lock(&mu_);
      ScopeRangeIdTree& tree = annotations_and_events_.scope_range_id_tree();
      for (; curr > head && !tree.contains(*curr); --curr) {
        tree.emplace(*curr, *(curr - 1));
      }
    }
  }

 private:
  absl::Mutex mu_;
  CallbackAnnotationsAndEvents annotations_and_events_ TF_GUARDED_BY(mu_);
};

using PerThreadCallbackAnnotationsAndEvents =
    tsl::profiler::PerThread<GuardedCallbackAnnotationsAndEvents>;

// Currently used for cuGraphInstantiate*, cuGraphLaunch*, cuGraphCreate,
// cuGraphClone.
void SetCudaGraphEventUponApiExit(
    CuptiTracerEvent& event, CuptiInterface* cupti_interface,
    uint32_t device_id, CUpti_CallbackId cbid, const CUpti_CallbackData* cbdata,
    uint64_t start_time, uint64_t end_time,
    GuardedCallbackAnnotationsAndEvents& guarded_annotations_and_events,
    CuptiTracer* tracer) {
  GraphResourceCreationInfo& graph_id_info = GetGraphResourceCreationInfo();
  if (cbid == CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch ||
      cbid == CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch_ptsz) {
    const auto* params =
        static_cast<const cuGraphLaunch_params*>(cbdata->functionParams);
    cupti_interface->GetGraphExecId(params->hGraph, &graph_id_info.graph_id);
    graph_id_info.orig_graph_id = 0;
  }
  // Create multiple events for cuGraphClone/cuGraphInstantiateWithFlags.
  // The multiple events are created for each node in the graph.
  // Each event has a unique node_id and orig_node_id. The orig_graph_id and
  // graph_id are the same for all the events.
  if (cbid == CUPTI_DRIVER_TRACE_CBID_cuGraphClone ||
      cbid == CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithFlags) {
    // Create multiple events based on the node_id_map size
    uint64_t current_start_time = start_time;
    // The time increment is set to 1 ns. This is to make sure that each
    // CudaGraphNodeMap events has a time width of 1 ns.
    constexpr uint64_t kTimeIncrementNs = 1;
    for (const auto& [node_id, orig_node_id] : graph_id_info.node_id_map) {
      CuptiTracerEvent current_event = event;
      current_event.type = CuptiTracerEventType::CudaGraphNodeMap;
      current_event.source = CuptiTracerEventSource::DriverCallback;
      current_event.name =
          absl::StrCat("CudaGraphNodeMap: ", cbdata->functionName);
      current_event.start_time_ns = current_start_time;
      current_event.end_time_ns = current_start_time + kTimeIncrementNs;
      current_event.thread_id = Env::Default()->GetCurrentThreadId();
      current_event.device_id = device_id;
      current_event.context_id = cbdata->contextUid;
      current_event.correlation_id = cbdata->correlationId;
      current_event.cuda_graph_info.cbid = cbid;
      current_event.graph_id = graph_id_info.graph_id;
      current_event.graph_node_id = node_id;
      current_event.cuda_graph_info.orig_graph_node_id = orig_node_id;
      current_event.cuda_graph_info.orig_graph_id = graph_id_info.orig_graph_id;
      VLOG(3) << "Observed CudaGraphNodeMap API exit."
              << " name=" << cbdata->functionName;
      guarded_annotations_and_events.Push(*tracer, std::move(current_event));
    }
    graph_id_info.node_id_map.clear();
  }

  event.type = CuptiTracerEventType::CudaGraph;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.cuda_graph_info.cbid = cbid;
  event.graph_id = graph_id_info.graph_id;
  event.cuda_graph_info.orig_graph_id = graph_id_info.orig_graph_id;
  VLOG(3) << "Observed CudaGraph API exit."
          << " name=" << cbdata->functionName;
}

// Currently used for all CUPTI_DRIVER_TRACE_CBID_cuGraphAdd*.
void SetCudaGraphNodeEventUponApiExit(CuptiTracerEvent& event,
                                      CuptiInterface* cupti_interface,
                                      uint32_t device_id, CUpti_CallbackId cbid,
                                      const CUpti_CallbackData* cbdata,
                                      uint64_t start_time, uint64_t end_time) {
  GraphResourceCreationInfo& graph_id_info = GetGraphResourceCreationInfo();

  event.type = CuptiTracerEventType::CudaGraph;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.cuda_graph_info.cbid = cbid;
  event.graph_id = graph_id_info.graph_id;
  // TODO(rahulnayar): Re-enable this check once the bug is fixed.
  // DCHECK_EQ(graph_id_info.node_id_map.size(), 1);
  event.graph_node_id = graph_id_info.node_id_map.begin()->first;
  event.cuda_graph_info.orig_graph_id = graph_id_info.orig_graph_id;
  event.cuda_graph_info.orig_graph_node_id =
      graph_id_info.node_id_map.begin()->second;
  VLOG(3) << "Observed CudaGraphNode API exit."
          << " name=" << cbdata->functionName;
  graph_id_info.node_id_map.clear();
}

void SetGenericEventUponApiExit(CuptiTracerEvent& event, uint32_t device_id,
                                CUpti_CallbackId cbid,
                                const CUpti_CallbackData* cbdata,
                                uint64_t start_time, uint64_t end_time) {
  event.type = CuptiTracerEventType::Generic;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.generic_info.cbid = cbid;
  VLOG(3) << "Observed generic API exit."
          << " name=" << cbdata->functionName;
}

static void SetCallbackEventUponApiExit(
    CuptiTracerEvent& event, CuptiInterface* cupti_interface,
    uint32_t device_id, CUpti_CallbackId cbid, const CUpti_CallbackData* cbdata,
    uint64_t start_tsc, uint64_t end_tsc,
    GuardedCallbackAnnotationsAndEvents& guarded_annotations_and_events,
    CuptiTracer* tracer) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:
#if CUDA_VERSION >= 11080  // CUDA 11.8
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernelEx:
#endif  // CUDA_VERSION >= 11080
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice:
      SetKernelEventUponApiExit(event, device_id, cbdata, start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync_v2:
      // This would be the place to populate the memcpy API activity's src and
      // dst memory kind by casting cbdata->functionParams. However, we are not
      // doing that because that will incur significant overhead to get the
      // memory aperture of each argument.
      SetNormalMemcpyEventUponApiExit(event, device_id, cbid, cbdata, start_tsc,
                                      end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync:
      SetP2PMemcpyEventUponApiExit(event, cupti_interface, device_id, cbid,
                                   cbdata, start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAlloc_v2:
      SetCuMemAllocEventUponApiExit(event, device_id, cbid, cbdata, start_tsc,
                                    end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocPitch_v2:
      SetCuMemAllocPitchEventUponApiExit(event, device_id, cbid, cbdata,
                                         start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocManaged:
      SetCuMemAllocManagedEventUponApiExit(event, device_id, cbid, cbdata,
                                           start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocHost_v2:
      SetCuMemAllocHostEventUponApiExit(event, device_id, cbid, cbdata,
                                        start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostAlloc:
      SetCuMemHostAllocEventUponApiExit(event, device_id, cbid, cbdata,
                                        start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemFree_v2:
      SetCuMemFreeEventUponApiExit(event, device_id, cbid, cbdata, start_tsc,
                                   end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemFreeHost:
      SetCuMemFreeHostEventUponApiExit(event, device_id, cbid, cbdata,
                                       start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostRegister_v2:
      SetCuMemHostRegisterEventUponApiExit(event, device_id, cbid, cbdata,
                                           start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostUnregister:
      SetCuMemHostUnregisterEventUponApiExit(event, device_id, cbid, cbdata,
                                             start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32Async:
      SetCuMemsetEventUponApiExit(event, device_id, cbid, cbdata, start_tsc,
                                  end_tsc);
      break;
#if CUDA_VERSION >= 11070  // CUDA 11.7
    case CUPTI_DRIVER_TRACE_CBID_cuGraphCreate:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiate:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch_ptsz:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphClone:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiate_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithFlags:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithParams:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithParams_ptsz:
      SetCudaGraphEventUponApiExit(event, cupti_interface, device_id, cbid,
                                   cbdata, start_tsc, end_tsc,
                                   guarded_annotations_and_events, tracer);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddKernelNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddKernelNode_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddMemcpyNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddMemsetNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddChildGraphNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddEmptyNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddHostNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddNode_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddEventRecordNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddEventWaitNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddExternalSemaphoresSignalNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddExternalSemaphoresWaitNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddMemAllocNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddMemFreeNode:
    case CUPTI_DRIVER_TRACE_CBID_cuGraphAddBatchMemOpNode:
      SetCudaGraphNodeEventUponApiExit(event, cupti_interface, device_id, cbid,
                                       cbdata, start_tsc, end_tsc);
      break;
#endif  // CUDA_VERSION >= 11070
    default:
      SetGenericEventUponApiExit(event, device_id, cbid, cbdata, start_tsc,
                                 end_tsc);
      break;
  }
}

absl::Status AddDriverApiCallbackEvent(
    CuptiTracer* tracer, CuptiInterface* cupti_interface, int device_id,
    uint64_t start_tsc, uint64_t end_tsc, CUpti_CallbackDomain domain,
    CUpti_CallbackId cbid, const CUpti_CallbackData* cbdata) {
  absl::string_view annotation = AnnotationStack::Get();
  absl::string_view nvtx_range = "";
  auto& guarded_annotations_and_events =
      PerThreadCallbackAnnotationsAndEvents::Get();
  if (tracer->TooManyCallbackEvents()) {
    guarded_annotations_and_events.IncNumDroppedEvents();
    return absl::OkStatus();
  }
  tracer->IncCallbackEventCount();
  absl::Span<const int64_t> range_ids = AnnotationStack::GetScopeRangeIds();
  guarded_annotations_and_events.AddScopeRangeIdSequence(range_ids);
  CuptiTracerEvent event{};
  event.correlation_id = cbdata->correlationId;
  event.annotation = annotation;
  event.nvtx_range = nvtx_range;
  event.scope_range_id = range_ids.empty() ? 0 : range_ids.back();
  SetCallbackEventUponApiExit(event, cupti_interface, device_id, cbid, cbdata,
                              start_tsc, end_tsc,
                              guarded_annotations_and_events, tracer);
  guarded_annotations_and_events.Push(*tracer, std::move(event));
  return absl::OkStatus();
}

// This hook uses cupti activity api to measure device side activities.
class CuptiDriverApiHookWithActivityApi : public CuptiDriverApiHook {
 public:
  CuptiDriverApiHookWithActivityApi(const CuptiTracerOptions& option,
                                    CuptiInterface* cupti_interface,
                                    CuptiTracer* tracer)
      : option_(option), cupti_interface_(cupti_interface), tracer_(tracer) {}

  absl::Status OnDriverApiEnter(int device_id, CUpti_CallbackDomain domain,
                                CUpti_CallbackId cbid,
                                const CUpti_CallbackData* cbdata) override {
    // Stash away the current Cupti timestamp into cbdata.
    *cbdata->correlationData =
        option_.required_callback_api_events ? CuptiTracer::GetTimestamp() : 0;
    return absl::OkStatus();
  }
  absl::Status OnDriverApiExit(int device_id, CUpti_CallbackDomain domain,
                               CUpti_CallbackId cbid,
                               const CUpti_CallbackData* cbdata) override {
    // Grab timestamp for API exit. API entry timestamp saved in cbdata.
    uint64_t end_tsc = CuptiTracer::GetTimestamp();
    uint64_t start_tsc = *cbdata->correlationData;
    TrackContext(cbid, cbdata->context);
    return AddDriverApiCallbackEvent(tracer_, cupti_interface_, device_id,
                                     start_tsc, end_tsc, domain, cbid, cbdata);
  }
  absl::Status SyncAndFlush() override {
    if (option_.sync_devices_before_stop) {
      CuptiApiTracingDisabler disabler;
      absl::MutexLock lock(&mutex_);
      for (auto& ctx : contexts_) {
        cuCtxPushCurrent(ctx);
        cuCtxSynchronize();  // Ignore error here for best effort.
        CUcontext current;
        cuCtxPopCurrent(&current);
      }
    }
    return absl::OkStatus();
  }

 private:
  void TrackContext(CUpti_CallbackId cbid, CUcontext ctx) {
    if (!option_.sync_devices_before_stop) return;
    if (ctx == nullptr) return;
    absl::MutexLock lock(&mutex_);
    if (cbid == CUPTI_DRIVER_TRACE_CBID_cuCtxDestroy_v2 ||
        cbid == CUPTI_DRIVER_TRACE_CBID_cuCtxDestroy) {
      contexts_.erase(ctx);
    } else {
      contexts_.emplace(ctx);
    }
  }

  const CuptiTracerOptions option_;
  CuptiInterface* cupti_interface_;
  CuptiTracer* tracer_;
  absl::Mutex mutex_;
  absl::flat_hash_set<CUcontext> contexts_ TF_GUARDED_BY(mutex_);

  CuptiDriverApiHookWithActivityApi(const CuptiDriverApiHookWithActivityApi&) =
      delete;
  void operator=(const CuptiDriverApiHookWithActivityApi&) = delete;
};

/*static*/ std::string ErrorWithHostname(absl::string_view error_message) {
  return absl::StrCat(tsl::port::Hostname(), ": ", error_message);
}

absl::Span<const uint32_t> GetCudaGraphTracingResourceCbids() {
#if CUDA_VERSION >= 11070
  static constexpr uint32_t res_cbids[] = {
      CUPTI_CBID_RESOURCE_GRAPH_CREATED, CUPTI_CBID_RESOURCE_GRAPH_CLONED,
      CUPTI_CBID_RESOURCE_GRAPHEXEC_CREATED,
      CUPTI_CBID_RESOURCE_GRAPHNODE_CREATED,
      CUPTI_CBID_RESOURCE_GRAPHNODE_CLONED};
  return absl::MakeSpan(res_cbids);
#else
  return absl::Span<const uint32_t>();
#endif
}

}  // namespace

CuptiTracer::CuptiTracer(CuptiInterface* cupti_interface)
    : num_gpus_(NumGpus()), cupti_interface_(cupti_interface) {}

/* static */ CuptiTracer* CuptiTracer::GetCuptiTracerSingleton() {
  static auto* singleton = new CuptiTracer(GetCuptiInterface());
  return singleton;
}

bool CuptiTracer::IsAvailable() const {
  return NumGpus() && !activity_tracing_enabled_ && !api_tracing_enabled_ &&
         !pm_sampling_enabled_;
}

int CuptiTracer::NumGpus() {
  static int num_gpus = []() -> int {
    if (cuInit(0) != CUDA_SUCCESS) {
      return 0;
    }
    int gpu_count;
    if (cuDeviceGetCount(&gpu_count) != CUDA_SUCCESS) {
      return 0;
    }
    LOG(INFO) << "Profiler found " << gpu_count << " GPUs";
    return gpu_count;
  }();
  return num_gpus;
}

absl::Status CuptiTracer::Enable(const CuptiTracerOptions& option,
                                 CuptiTraceCollector* collector) {
  option_ = option;
  collector_ = collector;

  // For nvtx tracking, utilize CUPTI activity marker and marker_data.
  if (option_->enable_nvtx_tracking) {
    std::vector<CUpti_ActivityKind>& activities = option_->activities_selected;
    if (std::find(activities.begin(), activities.end(),
                  CUPTI_ACTIVITY_KIND_MARKER) == activities.end()) {
      VLOG(1) << "Adding CUPTI_ACTIVITY_KIND_MARKER to activities:"
              << (int)CUPTI_ACTIVITY_KIND_MARKER;
      activities.push_back(CUPTI_ACTIVITY_KIND_MARKER);
    }
    // TODO: Add CUPTI_ACTIVITY_KIND_MARKER_DATA to activities after cupti
    // more detailed data could be provided by cupti.
  }

  cupti_driver_api_hook_ = std::make_unique<CuptiDriverApiHookWithActivityApi>(
      *option_, cupti_interface_, this);

  absl::Status status = EnableApiTracing();
  need_root_access_ |= status.code() == tsl::error::PERMISSION_DENIED;
  if (!status.ok()) {
    return status;
  }

  EnableActivityTracing().IgnoreError();
  tsl::profiler::AnnotationStack::Enable(true);

  if (option_->pm_sampler_options.enable) {
    TF_ASSIGN_OR_RETURN(
        pm_sampler_, CreatePmSampler(NumGpus(), option_->pm_sampler_options));

    TF_RETURN_IF_ERROR(pm_sampler_->StartSampler());

    pm_sampling_enabled_ = true;
  }

  return status;
}

void CuptiTracer::Disable() {
  if (pm_sampling_enabled_) {
    pm_sampler_->StopSampler().IgnoreError();
    pm_sampler_->Deinitialize().IgnoreError();

    pm_sampling_enabled_ = false;
  }

  DisableApiTracing().IgnoreError();
  DisableActivityTracing().IgnoreError();
  cupti_interface_->CleanUp();
  Finalize().IgnoreError();
  cupti_driver_api_hook_->SyncAndFlush().IgnoreError();

  collector_->SetTracingEndTimeNs(GetTimestamp());

  // The callback API events must be processed before activity API buffers
  // because the AnnotationMap is populated from the callback API events and
  // queried by the activity API events.
  collector_->OnTracerCollectedCallbackData(
      GatherCallbackAnnotationsAndEvents(/*stop_recording=*/true),
      IsCallbackApiEventsRequired());

  if (activity_buffers_) {
    auto cached_buffers = activity_buffers_->PopCachedBuffers();
    activity_buffers_.reset();
    collector_->OnTracerCachedActivityBuffers(std::move(cached_buffers));
  }

  if (cupti_dropped_activity_event_count_ > 0) {
    collector_->OnEventsDropped("Activity Event dropped by Cupti Lib:",
                                cupti_dropped_activity_event_count_);
  }
  if (num_activity_events_in_dropped_buffer_ > 0) {
    collector_->OnEventsDropped("Activity Event dropped in dropped buffer:",
                                num_activity_events_in_dropped_buffer_);
  }

  collector_->Flush();
  collector_ = nullptr;
  option_.reset();
  cupti_driver_api_hook_.reset();
  tsl::profiler::AnnotationStack::Enable(false);
}

std::vector<CUpti_driver_api_trace_cbid_enum>
CuptiTracer::CreateDefaultCallbackIds() {
  return {
      // KERNEL
      CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel,
#if CUDA_VERSION >= 11080  // CUDA 11.8
      CUPTI_DRIVER_TRACE_CBID_cuLaunchKernelEx,
#endif  // CUDA_VERSION >= 11080
      // MEMCPY
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync_v2,
      // MemAlloc
      CUPTI_DRIVER_TRACE_CBID_cuMemAlloc_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemAllocPitch_v2,
      // MemFree
      CUPTI_DRIVER_TRACE_CBID_cuMemFree_v2,
      // Memset
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD8_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD16_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD32_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32_v2,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD8Async,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD16Async,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD32Async,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8Async,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16Async,
      CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32Async,
      // GENERIC
      CUPTI_DRIVER_TRACE_CBID_cuStreamSynchronize,
#if CUDA_VERSION >= 12080  // CUDA 12.8
      CUPTI_DRIVER_TRACE_CBID_cuGraphCreate,
      CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiate,
      CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch,
      CUPTI_DRIVER_TRACE_CBID_cuGraphLaunch_ptsz,
      CUPTI_DRIVER_TRACE_CBID_cuGraphClone,
      CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiate_v2,
      CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithFlags,
      CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithParams,
      CUPTI_DRIVER_TRACE_CBID_cuGraphInstantiateWithParams_ptsz,
      CUPTI_DRIVER_TRACE_CBID_cuGraphAddMemcpyNode,
#endif  // CUDA_VERSION >= 12080
  };
}

absl::Status CuptiTracer::FlushEventsToCollector() {
  if (!api_tracing_enabled_ && !activity_tracing_enabled_) {
    return absl::OkStatus();
  }

  // Need get the cached activity buffers first, but send to collector after
  // the callback events are processed.
  std::list<CuptiActivityBufferManager::ActivityBufferAndSize> cached_buffers;
  if (activity_tracing_enabled_) {
    cached_buffers = activity_buffers_->PopCachedBuffers();
  }

  if (api_tracing_enabled_) {
    collector_->OnTracerCollectedCallbackData(
        GatherCallbackAnnotationsAndEvents(/*stop_recording=*/false),
        IsCallbackApiEventsRequired());
  }

  collector_->OnTracerCachedActivityBuffers(std::move(cached_buffers));
  return absl::OkStatus();
}

absl::Status CuptiTracer::SetActivityFlushPeriod(uint32_t period_ms) {
  if (activity_tracing_enabled_) {
    LOG(INFO) << "Set CUPTI activity flush period to " << period_ms << "ms.";
    RETURN_IF_CUPTI_ERROR(SetActivityFlushPeriod(period_ms));
  }
  return absl::OkStatus();
}

absl::Status CuptiTracer::FlushActivityBuffers() {
  // Not forced flush. Only flush completed activity buffers.
  RETURN_IF_CUPTI_ERROR(ActivityFlushAll(0));
  return absl::OkStatus();
}

// Need to trace graph ids from creation and instantiation.
absl::Status CuptiTracer::EnableApiTracing() {
  if (api_tracing_enabled_) return absl::OkStatus();

  PrepareCallbackStart();

  VLOG(1) << "Enable subscriber";
  // Subscribe can return CUPTI_ERROR_MAX_LIMIT_REACHED.
  // The application which calls CUPTI APIs cannot be used with Nvidia tools
  // like nvprof, Nvidia Visual Profiler, Nsight Compute, Nsight Systems.
  RETURN_IF_CUPTI_ERROR(
      Subscribe(&subscriber_, (CUpti_CallbackFunc)ApiCallback, this));
  api_tracing_enabled_ = true;

  absl::Span<const uint32_t> res_cbids = GetCudaGraphTracingResourceCbids();
  for (auto cbid : res_cbids) {
    RETURN_IF_CUPTI_ERROR(EnableCallback(1 /* ENABLE */, subscriber_,
                                         CUPTI_CB_DOMAIN_RESOURCE, cbid));
  }

  if (!option_->cbids_selected.empty()) {
    for (auto cbid : option_->cbids_selected) {
      RETURN_IF_CUPTI_ERROR(EnableCallback(1 /* ENABLE */, subscriber_,
                                           CUPTI_CB_DOMAIN_DRIVER_API, cbid));
    }
  } else {  // select all callback ids.
    RETURN_IF_CUPTI_ERROR(
        EnableDomain(1 /* ENABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API));
  }

  // There is no easy api to get the domain string from CUPTI_CB_DOMAIN_NVTX
  // callback. So we use ACTIVIY_MARKERS to get the domain/range_name strings,
  // and generate the related nvtx range event. So we do not need to use the
  // CUPTI_CB_DOMAIN_NVTX callback here.
  return absl::OkStatus();
}

absl::Status CuptiTracer::DisableApiTracing() {
  if (!api_tracing_enabled_) return absl::OkStatus();

  api_tracing_enabled_ = false;

  absl::Span<const uint32_t> res_cbids = GetCudaGraphTracingResourceCbids();
  for (auto cbid : res_cbids) {
    RETURN_IF_CUPTI_ERROR(EnableCallback(0 /* DISABLE */, subscriber_,
                                         CUPTI_CB_DOMAIN_RESOURCE, cbid));
  }

  if (!option_->cbids_selected.empty()) {
    for (auto cbid : option_->cbids_selected) {
      RETURN_IF_CUPTI_ERROR(EnableCallback(0 /* DISABLE */, subscriber_,
                                           CUPTI_CB_DOMAIN_DRIVER_API, cbid));
    }
  } else {
    RETURN_IF_CUPTI_ERROR(
        EnableDomain(0 /* DISABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API));
  }

  VLOG(1) << "Disable subscriber";
  RETURN_IF_CUPTI_ERROR(Unsubscribe(subscriber_));
  return absl::OkStatus();
}

absl::Status CuptiTracer::EnableActivityTracing() {
  if (activity_tracing_enabled_) return absl::OkStatus();
  PrepareActivityStart();
  if (!option_->activities_selected.empty()) {
    if (cupti_interface_->SetThreadIdType(
            CUPTI_ACTIVITY_THREAD_ID_TYPE_SYSTEM) != CUPTI_SUCCESS) {
      LOG(WARNING)
          << "Failed to set CUPTI activity thread id type to "
             "CUPTI_ACTIVITY_THREAD_ID_TYPE_SYSTEM, CUPTI reported thread id "
             "may be different from system thread id get with gettid()";
    };

    // Initialize callback functions for Cupti Activity API.
    VLOG(1) << "Registering CUPTI activity callbacks";
    if (auto err = cupti_interface_->ActivityUsePerThreadBuffer();
        err != CUPTI_SUCCESS) {
      LOG(WARNING) << "Fail to use per-thread activity buffer, cupti trace "
                      "overhead may be big. CUPTI ERROR CODE:"
                   << err;
    }
    RETURN_IF_CUPTI_ERROR(ActivityRegisterCallbacks(
        RequestCuptiActivityBuffer, ProcessCuptiActivityBuffer));
    VLOG(1) << "Enabling activity tracing for "
            << option_->activities_selected.size() << " activities";
    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Enabling activity tracing for: " << activity;
      if (activity == CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER) {
        ConfigureActivityUnifiedMemoryCounter(true);
      }
      RETURN_IF_CUPTI_ERROR(ActivityEnable(activity));
    }
  }
  activity_tracing_enabled_ = true;
  return absl::OkStatus();
}

absl::Status CuptiTracer::DisableActivityTracing() {
  if (activity_tracing_enabled_) {
    VLOG(1) << "Disabling activity tracing for "
            << option_->activities_selected.size() << " activities";
    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Disabling activity tracing for: " << activity;
      if (activity == CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER) {
        ConfigureActivityUnifiedMemoryCounter(false);
      }
      // TODO: b/422262733 - Temporarily skip calling disable because of the NV
      // bug (https://partners.nvidia.com/Bug/ViewBug/5350647). Re-enable after
      // the fix.
      if (activity == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) {
        VLOG(1) << "Skip disabling activity tracing for: " << activity
                << " due to deadlock";
        continue;
      }
      RETURN_IF_CUPTI_ERROR(ActivityDisable(activity));
    }
    option_->activities_selected.clear();

    VLOG(1) << "Flushing CUPTI activity buffer";
    RETURN_IF_CUPTI_ERROR(ActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED));
    LOG(INFO) << "CUPTI activity buffer flushed";
  }
  activity_tracing_enabled_ = false;
  return absl::OkStatus();
}

absl::Status CuptiTracer::Finalize() {
  if (option_->cupti_finalize) {
    VLOG(1) << "CuptiFinalize";
    RETURN_IF_CUPTI_ERROR(Finalize());
  }
  return absl::OkStatus();
}

/*static*/ uint64_t CuptiTracer::GetTimestamp() {
  uint64_t tsc;
  CuptiInterface* cupti_interface = GetCuptiInterface();
  if (cupti_interface && cupti_interface->GetTimestamp(&tsc) == CUPTI_SUCCESS) {
    return tsc;
  }
  // Return 0 on error. If an activity timestamp is 0, the activity will be
  // dropped during time normalization.
  return 0;
}

// Resource callback happens logically inside a driver API call's enter/exit.
// Some per-thread data structure to record the graph ids.
absl::Status CuptiTracer::HandleResourceCallback(
    CUpti_CallbackId cbid, const CUpti_CallbackData* cbdata) {
  auto* resource = reinterpret_cast<const CUpti_ResourceData*>(cbdata);
  auto* graph_data =
      reinterpret_cast<const CUpti_GraphData*>(resource->resourceDescriptor);
  GraphResourceCreationInfo& graph_id_info = GetGraphResourceCreationInfo();
  auto orig_graph_node = static_cast<CUgraphNode>(graph_data->originalNode);
  auto created_graph_node = static_cast<CUgraphNode>(graph_data->node);
  uint64_t orig_graph_node_id = 0;
  uint64_t graph_node_id = 0;
  switch (cbid) {
    case CUPTI_CBID_RESOURCE_GRAPH_CREATED:
      cupti_interface_->GetGraphId(graph_data->graph, &graph_id_info.graph_id);
      graph_id_info.orig_graph_id = 0;
      break;
    case CUPTI_CBID_RESOURCE_GRAPH_CLONED:
      cupti_interface_->GetGraphId(graph_data->graph, &graph_id_info.graph_id);
      cupti_interface_->GetGraphId(graph_data->originalGraph,
                                   &graph_id_info.orig_graph_id);
      break;
    case CUPTI_CBID_RESOURCE_GRAPHEXEC_CREATED:
      cupti_interface_->GetGraphExecId(graph_data->graphExec,
                                       &graph_id_info.graph_id);
      cupti_interface_->GetGraphId(graph_data->graph,
                                   &graph_id_info.orig_graph_id);
      break;
    case CUPTI_CBID_RESOURCE_GRAPHNODE_CREATED:
      cupti_interface_->GetGraphNodeId(created_graph_node, &graph_node_id);
      graph_id_info.node_id_map[graph_node_id] = 0;
      break;
    case CUPTI_CBID_RESOURCE_GRAPHNODE_CLONED:
      cupti_interface_->GetGraphNodeId(created_graph_node, &graph_node_id);
      cupti_interface_->GetGraphNodeId(orig_graph_node, &orig_graph_node_id);
      // Graph Node instance process, two graph nodes are first created this
      // will generate a two map entries linked to 0 node id. When graph node is
      // cloned/instantiated, the orig_graph_node_id will be populated.
      if (graph_id_info.node_id_map.contains(graph_node_id) &&
          graph_id_info.node_id_map[graph_node_id] > 0) {
        LOG_FIRST_N(ERROR, 10)
            << "Duplicate graph node id: " << graph_node_id
            << " graph_id: " << graph_id_info.graph_id
            << " orig_graph_id: " << graph_id_info.orig_graph_id
            << " orig_graph_node_id: "
            << graph_id_info.node_id_map[graph_node_id]
            << " ,inserting: " << orig_graph_node_id;
      }
      graph_id_info.node_id_map[graph_node_id] = orig_graph_node_id;
      break;
  }
  return absl::OkStatus();
}

absl::Status CuptiTracer::HandleDriverApiCallback(
    CUpti_CallbackId cbid, const CUpti_CallbackData* cbdata) {
  constexpr CUpti_CallbackDomain domain = CUPTI_CB_DOMAIN_DRIVER_API;
  if (internalCuCall) return absl::OkStatus();

  if (cbdata->context == nullptr) {
    // API callback is called before any CUDA context is created.
    // This is expected to be rare, and we ignore this case.
    VLOG(3) << "API callback received before creation of CUDA context\n";
    return absl::InternalError("cutpi callback without context");
  }

  // Grab a correct device ID.
  uint32_t device_id = -1;
  RETURN_IF_CUPTI_ERROR(GetDeviceId(cbdata->context, &device_id));
  if (device_id >= num_gpus_) {
    return absl::InternalError(absl::StrCat("Invalid device id:", device_id));
  }

  if (cbdata->callbackSite == CUPTI_API_ENTER) {
    TF_RETURN_IF_ERROR(cupti_driver_api_hook_->OnDriverApiEnter(
        device_id, domain, cbid, cbdata));
  } else if (cbdata->callbackSite == CUPTI_API_EXIT) {
    TF_RETURN_IF_ERROR(cupti_driver_api_hook_->OnDriverApiExit(
        device_id, domain, cbid, cbdata));
  }
  return absl::OkStatus();
}

absl::Status CuptiTracer::HandleCallback(CUpti_CallbackDomain domain,
                                         CUpti_CallbackId cbid,
                                         const CUpti_CallbackData* cbdata) {
  if (!api_tracing_enabled_) return absl::OkStatus();  // already unsubscribed.
  if (!cupti_driver_api_hook_)
    return absl::OkStatus();  // already unsubscribed.
  if (domain == CUPTI_CB_DOMAIN_DRIVER_API)
    return HandleDriverApiCallback(cbid, cbdata);
  if (domain == CUPTI_CB_DOMAIN_RESOURCE)
    return HandleResourceCallback(cbid, cbdata);
  return absl::OkStatus();
}

void CuptiTracer::ConfigureActivityUnifiedMemoryCounter(bool enable) {
  CUpti_ActivityUnifiedMemoryCounterConfig config[2];
  // By experiments, currently only measurements from these two activities are
  // trustworthy. Others like GPU page fault may be problematic.
  config[0].kind =
      CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_HTOD;
  config[1].kind =
      CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_DTOH;

  for (size_t i = 0; i < 2; i++) {
    config[i].enable = enable;
  }

  CUptiResult res;

  res = cupti_interface_->ActivityConfigureUnifiedMemoryCounter(config, 2);
  if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED) {
    LOG(ERROR) << "Unified memory is not supported on the "
                  "underlying platform.\n";
  } else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_DEVICE) {
    LOG(ERROR) << "Unified memory is not supported on the device.\n";
  } else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_NON_P2P_DEVICES) {
    LOG(ERROR) << "Unified memory is not supported on the "
                  "non-P2P multi-gpu setup.\n";
  } else if (res != CUPTI_SUCCESS) {
    const char* errstr = "";
    cuptiGetResultString(res, &errstr);
    LOG(ERROR) << "Error while enabling unified memory profiling: " << errstr;
  } else {
    VLOG(1) << "Configuring Unified memory profiling: " << res;
  }
}

void CuptiTracer::RequestActivityBuffer(uint8_t** buffer, size_t* size) {
  // TODO: b/422262733 - This check won't be needed anymore once the deadlock
  // bug is fixed.
  if (activity_buffers_ == nullptr) {
    LOG(WARNING) << "CUPTI activity buffer is not initialized.";
    *buffer = nullptr;
    *size = 0;
    return;
  }

  *buffer = activity_buffers_->GetOrCreateBuffer();
  if (*buffer == nullptr) {
    LOG(WARNING)
        << "CUPTI Buffer not allocated, activity records will be dropped";
    *size = 0;
    return;
  }
  *size = activity_buffers_->GetBufferSizeInBytes();
}

static size_t CountCuptiActivityEvent(uint8_t* buffer, size_t size) {
  size_t total_event_count = 0;
  if (size == 0 || buffer == nullptr) return total_event_count;
  CuptiInterface* cupti_interface = GetCuptiInterface();
  CUpti_Activity* record = nullptr;
  while (true) {
    if (cupti_interface->ActivityGetNextRecord(buffer, size, &record) ==
        CUPTI_SUCCESS) {
      ++total_event_count;
    } else {
      break;
    }
  }
  return total_event_count;
}

absl::Status CuptiTracer::ProcessActivityBuffer(CUcontext context,
                                                uint32_t stream_id,
                                                uint8_t* buffer, size_t size) {
  absl::Cleanup buffer_cleanup = [&]() {
    if (buffer) activity_buffers_->ReclaimBuffer(buffer);
  };
  if (size == 0 || buffer == nullptr) {
    return absl::OkStatus();
  }
  if (!activity_tracing_enabled_) {
    LOG(WARNING) << "CUPTI activity buffer is reclaimed after flush.";
    return absl::OkStatus();
  }
  if (cupti_interface_->Disabled()) return absl::InternalError("Disabled.");

  // Report dropped records.
  size_t dropped = 0;
  if (cupti_interface_->ActivityGetNumDroppedRecords(
          context, stream_id, &dropped) == CUPTI_SUCCESS) {
    cupti_dropped_activity_event_count_ += dropped;
  }

  size_t event_count_in_buffer = CountCuptiActivityEvent(buffer, size);
  auto max_activity_event_count =
      collector_->GetOptions().max_activity_api_events;
  if (max_activity_event_count > 0 &&
      num_activity_events_in_cached_buffer_ >= max_activity_event_count) {
    LOG_EVERY_N(WARNING, 10000)
        << "Already too many activity events, drop the buffer of " << size
        << "bytes of event to reuse. This warning is logged once per 10000 "
           "occurrences, the current count is "
        << COUNTER << ".";
    num_activity_events_in_dropped_buffer_ += event_count_in_buffer;
    // buffer will be return to the pool
    return absl::OkStatus();
  }
  num_activity_events_in_cached_buffer_ += event_count_in_buffer;

  // When cupti activity buffer is required to flush, save the buffer and its
  // valid size some where. All the saved activity buffer will be handled
  // after the profiling is stopped.
  VLOG(3) << "Caching CUPTI activity buffer of size:" << size;
  activity_buffers_->CacheCuptiFilledActivityBuffer(buffer, size);
  buffer = nullptr;  // So cleanup will not free it as it was saved already

  return absl::OkStatus();
}

/*static*/ std::string CuptiTracer::ErrorIfAny() {
  if (CuptiTracer::NumGpus() == 0) {
    return ErrorWithHostname("No GPU detected.");
  } else if (CuptiTracer::GetCuptiTracerSingleton()->NeedRootAccess()) {
    return ErrorWithHostname(
        "Insufficient privilege to run libcupti (you need root permission).");
  } else if (CuptiTracer::GetTimestamp() == 0) {
    return ErrorWithHostname(
        "Failed to load libcupti (is it installed and accessible?)");
  }
  return "";
}

std::vector<CallbackAnnotationsAndEvents>
CuptiTracer::GatherCallbackAnnotationsAndEvents(bool stop_recording) {
  // Note that it is OK to call PerThread<T>'s StartRecording() multiple times
  // without calling StopRecording().
  auto guarded_collection =
      stop_recording ? PerThreadCallbackAnnotationsAndEvents::StopRecording()
                     : PerThreadCallbackAnnotationsAndEvents::StartRecording();
  VLOG(3) << "Total grabbed per thread annotated events buffer: "
          << guarded_collection.size();

  std::vector<CallbackAnnotationsAndEvents> result;
  result.reserve(guarded_collection.size());
  for (auto& guarded_annotations_events : guarded_collection) {
    result.emplace_back(guarded_annotations_events->Consume());
  }
  return result;
}

void CuptiTracer::PrepareCallbackStart() {
  auto guarded_collection =
      PerThreadCallbackAnnotationsAndEvents::StartRecording();
  for (auto& guarded_annotations_events : guarded_collection) {
    guarded_annotations_events->Clear();
  }
  num_callback_events_ = 0;
}

void CuptiTracer::PrepareActivityStart() {
  activity_buffers_ =
      std::make_unique<CuptiActivityBufferManager>(kBufferSizeInBytes);
  cupti_dropped_activity_event_count_ = 0;
  num_activity_events_in_cached_buffer_ = 0;
  num_activity_events_in_dropped_buffer_ = 0;
}

bool CuptiTracer::TooManyCallbackEvents() const {
  if (collector_ != nullptr) {
    size_t count = num_callback_events_.load(std::memory_order_acquire);
    size_t max_events = collector_->GetOptions().max_callback_api_events;
    return max_events > 0 && count >= max_events;
  }
  return true;
}

bool CuptiTracer::TooManyAnnotationStrings(size_t count) const {
  if (collector_ != nullptr) {
    size_t max_strings = collector_->GetOptions().max_annotation_strings;
    return max_strings > 0 && count >= max_strings;
  }
  return true;
}

}  // namespace profiler
}  // namespace xla
