/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "gpu/xenos/shared_memory.h"
#include <rex/memory.h>
#include "gpu/xenos/host/d3d12/d3d12_api.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor;

class D3D12SharedMemory : public SharedMemory {
 public:
  D3D12SharedMemory(D3D12CommandProcessor& command_processor, IPhysicalMemory& memory,
                    IPhysicalWriteWatch& watch);
  ~D3D12SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  // Async upload copies (gpu.d3d12.upload_copy_workers, bo2-bisect perf/0027, 0032, 0041). The
  // ticket is the number of copies queued so far (command thread); WaitForUploads(ticket) returns
  // once all of them are done, which a command list reading the upload buffers and every
  // guest-visible write need first. Waiters copy queued jobs themselves instead of idling.
  u64 UploadTicket() const { return copy_enqueued_; }
  bool UploadsDone(u64 ticket) const {
    return copy_completed_.load(std::memory_order_acquire) >= ticket;
  }
  void WaitForUploads(u64 ticket);

  /**
   * Frame start (gpu.d3d12.upload_prefetch_frames, bo2-bisect perf/0029, 0042): uploads again in
   * one batch, with one buffer transition, the parts of the ranges uploaded on demand in the last
   * frames that are invalid again, so the frame's draws find them valid instead of each forcing a
   * copy destination transition mid frame.
   */
  void PrefetchHotRanges();

  ID3D12Resource* GetBuffer() const { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return buffer_gpu_address_; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  // RequestRange can leave the buffer in copy destination state, so it precedes UseFor*.

  /** Makes the buffer usable for vertices, indices and texture untiling. */
  void UseForReading() {
    // Vertex fetch is also allowed in pixel shaders.
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  /** Makes the buffer usable for texture tiling after a resolve. */
  void UseForWriting() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  /** Makes the buffer usable as a source for copy commands. */
  void UseAsCopySource() { CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE); }
  /**
   * Marks UAV writes to the buffer by draws or dispatches, so the next UseForWriting issues a
   * UAV barrier ordering overlapping UAV writes and reads.
   */
  void MarkUAVWritesCommitNeeded() {
    if (buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      buffer_uav_writes_commit_needed_ = true;
    }
  }

  void WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  /**
   * Writes a typed SRV descriptor. The smallest supported format is 32 bit, from
   * D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP.
   *
   * @param handle Destination CPU descriptor.
   * @param element_size_bytes_pow2 Log2 of the element size in bytes, 2 to 4.
   */
  void WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 element_size_bytes_pow2);
  void WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 element_size_bytes_pow2);

 protected:
  bool AllocateSparseHostGPUMemoryRange(u32 offset_allocations, u32 length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<u32, u32>>& upload_page_ranges) override;

 private:
  D3D12CommandProcessor& command_processor_;

  struct CopyJob {
    void* dst;
    const void* src;
    size_t size;
    u64 seq;
  };
  /** Jobs may finish out of order; copy_completed_ is the contiguous finished prefix. */
  static constexpr u64 kCopyDoneRing = u64{1} << 16;
  std::vector<u8> copy_done_ = std::vector<u8>(kCopyDoneRing, 0);
  u64 copy_prefix_ = 0;  // Guarded by copy_mutex_.
  void FinishCopyLocked(u64 seq);
  void CopyThreadMain(int index);
  void StopCopyThreads();
  static constexpr int kMaxCopyThreads = 4;
  std::thread copy_threads_[kMaxCopyThreads];
  std::condition_variable copy_worker_cvs_[kMaxCopyThreads];
  std::atomic<int> copy_workers_active_{0};
  u32 copy_wake_next_ = 0;
  std::mutex copy_mutex_;
  std::condition_variable copy_done_cv_;
  std::deque<CopyJob> copy_queue_;
  bool copy_stop_ = false;
  u64 copy_enqueued_ = 0;  // Command thread only.
  std::atomic<u64> copy_completed_{0};

  static constexpr size_t kHotFrames = 8;
  /** Pages (first, count) uploaded on demand per frame, a ring over kHotFrames frames. */
  std::vector<std::pair<u32, u32>> hot_ranges_[kHotFrames];
  size_t hot_frame_ = 0;
  bool prefetching_ = false;

  /** The 512 MB tiled buffer. */
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS buffer_gpu_address_ = 0;
  std::vector<ID3D12Heap*> buffer_tiled_heaps_;
  D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool buffer_uav_writes_commit_needed_ = false;
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);

  /** Views in the non shader visible descriptor heap, copied for binding. */
  enum class BufferDescriptorIndex : u8 {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE buffer_descriptor_heap_start_;

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> upload_buffer_pool_;
};

}  // namespace rex::gpu::d3d12
