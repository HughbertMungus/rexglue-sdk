/**
 * @file        gpu/xenos/host/d3d12/d3d12_settings.h
 * @brief       Settings of the Direct3D 12 driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace rex::gpu::d3d12 {

/** The gpu.d3d12 settings. The D3D12 graphics driver owns one. */
class D3D12Settings : public SettingOwner {
 public:
  D3D12Settings() : SettingOwner("gpu.d3d12") {}

  REX_SETTING(i32, adapter, -1, "Adapter index, -1 picks the first hardware adapter, -2 picks WARP",
              Range{-2, 15}, kStartupOnly);
  REX_SETTING(bool, debug_markers, false,
              "Insert debug markers into GPU command streams for tools like PIX and RenderDoc. "
              "Turns on when RenderDoc is detected");
  REX_SETTING(bool, bindless, true, "Use bindless resources where available", kRestart);
  REX_SETTING(bool, readback_memexport, false,
              "Read data written by memory export in shaders on the CPU");
  REX_SETTING(bool, readback_resolve, false, "Read render to texture results on the CPU");
  REX_SETTING(bool, async_replay, false,
              "Record, execute and signal closed submissions on a separate GPU Replay thread "
              "instead of the GPU command thread");
  REX_SETTING(i32, upload_copy_workers, 0,
              "Threads that copy guest memory into upload buffers off the GPU command thread, 0 "
              "copies inline. Command lists and guest-visible writes wait for the queued copies",
              Range{0, 4});
  REX_SETTING(i32, upload_prefetch_frames, 0,
              "At frame start, upload again in one batch the parts of the ranges uploaded on demand "
              "in this many previous frames that are invalid again, 0 turns it off",
              Range{0, 7});
  REX_SETTING(i32, texture_recycle_pool_mb, 0,
              "Keep evicted texture resources up to this many MB for reuse by textures of the same "
              "description, instead of creating and destroying them, 0 turns it off",
              Range{0, 2048});
  REX_SETTING(bool, fetch_dedup, false,
              "Skip texture rebinds and the fetch constant buffer update for fetch constant writes "
              "that do not change the value");
  REX_SETTING(bool, defer_guest_stores, false,
              "With upload copy workers, queue fence and scratch stores behind the pending copies "
              "instead of waiting for them");
  REX_SETTING(i32, gpu_profile_interval, 0,
              "Time the GPU work of every Nth frame by category (draws, render target transfers, "
              "resolves, texture loads, uploads, present) with timestamp queries; the fps log "
              "(REX_FPS_LOG=1) gets gp_* fields in ms per profiled frame. 0 turns it off",
              Range{0, 1000});
  REX_SETTING(bool, rt_transfer_census, false,
              "Count render target ownership transfers by kind and by source -> destination render "
              "target, one line a second in mp_rtx.txt (needs REX_FPS_LOG=1)");
  REX_SETTING(bool, submit_on_primary_buffer_end, true,
              "Submit command list when PM4 primary buffer ends");
  REX_SETTING(bool, dxbc_disasm, false, "Dump DXBC disassembly");
  REX_SETTING(bool, dxbc_disasm_dxilconv, false, "Dump DXIL conversion disassembly");
  REX_SETTING(i32, pipeline_creation_threads, -1,
              "Number of pipeline creation threads, -1 for auto", Range{-1, 32}, kRestart);
  REX_SETTING(bool, tessellation_wireframe, false, "Render tessellation as wireframe");
  REX_SETTING(bool, native_stencil_value_output, true, "Enable native stencil value output");
  REX_SETTING(bool, use_fuzzy_alpha_epsilon, false,
              "Use approximate compare for alpha test values to prevent flickering on NVIDIA "
              "graphics cards");
  REX_SETTING(bool, dxbc_switch, true, "Use switch statements in DXBC");
  REX_SETTING(bool, dxbc_source_map, false, "Generate source maps for DXBC");
  REX_SETTING(bool, draw_resolution_scaled_texture_offsets, true,
              "Scale texture offsets with draw resolution");
  REX_SETTING(bool, native_stencil_value_output_intel, false,
              "Native stencil value output for Intel");
  REX_SETTING(std::string, render_target_path, "", "Render target implementation path",
              kStartupOnly);
  REX_SETTING(bool, tiled_shared_memory, true, "Use tiled shared memory", kRestart);
  REX_SETTING(bool, allow_tearing, true, "Allow variable refresh rate and tearing");
  REX_SETTING(bool, debug, false, "Enable Direct3D 12 and DXGI debug layer", kStartupOnly);
  REX_SETTING(bool, break_on_error, false, "Break on Direct3D 12 validation errors");
  REX_SETTING(bool, break_on_warning, false, "Break on Direct3D 12 validation warnings");
  REX_SETTING(i32, queue_priority, 1,
              "Graphics command queue priority, 0 for normal, 1 for high, 2 for realtime",
              Range{0, 2});
};

}  // namespace rex::gpu::d3d12
