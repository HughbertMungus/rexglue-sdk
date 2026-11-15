/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <rex/assert.h>
#include <rex/platform/perf.h>
#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/deferred_command_list.h"
#include <rex/math.h>

namespace rex::gpu::d3d12 {

namespace {
// gpu.d3d12.command_census: commands replayed by type, and those identical to the previous command
// of the same type (root arguments: the previous one for the same root parameter, until the root
// signature or descriptor heaps change). Replay runs on one thread at a time.
struct CommandCensus {
  static constexpr u32 kTypes = 64;
  u64 count[kTypes] = {};
  u64 repeated[kTypes] = {};
  u64 bytes[kTypes] = {};
  std::vector<uintmax_t> last[kTypes];
  bool last_valid[kTypes] = {};
  u64 last_root[2][3][64] = {};  // [compute, graphics][table, cbv, constants hash][index]
  bool last_root_valid[2][3][64] = {};
  std::chrono::steady_clock::time_point next_report = std::chrono::steady_clock::now();
  u64 executes = 0;
};
CommandCensus g_command_census;

const char* CommandName(u32 command) {
  static const char* const kNames[] = {
      "ClearDepthStencilView", "ClearRenderTargetView", "ClearUAVUint", "CopyBufferRegion",
      "CopyResource", "CopyTexture", "CopyTextureRegion", "Dispatch", "DrawIndexedInstanced",
      "DrawInstanced", "BeginQuery", "EndQuery", "ResolveQueryData", "IASetIndexBuffer",
      "IASetPrimitiveTopology", "IASetVertexBuffers", "OMSetBlendFactor", "OMSetRenderTargets",
      "OMSetStencilRef", "ResourceBarrier", "RSSetScissorRect", "RSSetViewport",
      "SetComputeRoot32BitConstants", "SetGraphicsRoot32BitConstants", "SetComputeRootCBV",
      "SetGraphicsRootCBV", "SetComputeRootDescriptorTable", "SetGraphicsRootDescriptorTable",
      "SetComputeRootSRV", "SetGraphicsRootSRV", "SetComputeRootSignature",
      "SetGraphicsRootSignature", "SetComputeRootUAV", "SetGraphicsRootUAV", "SetDescriptorHeaps",
      "SetPipelineState", "SetPipelineStateHandle", "SetSamplePositions", "BeginDebugMarker",
      "EndDebugMarker", "InsertDebugMarker"};
  return command < std::size(kNames) ? kNames[command] : "?";
}

void CommandCensusReset(CommandCensus& c) {
  std::fill(std::begin(c.last_valid), std::end(c.last_valid), false);
  std::memset(c.last_root_valid, 0, sizeof(c.last_root_valid));
}

void CommandCensusRecord(CommandCensus& c, u32 command, const uintmax_t* args,
                         size_t args_elements, bool is_root, bool compute, u32 root_kind,
                         u32 root_index, u64 root_value) {
  if (command >= CommandCensus::kTypes) {
    return;
  }
  ++c.count[command];
  c.bytes[command] += args_elements * sizeof(uintmax_t);
  bool repeated;
  if (is_root) {
    u32 i = root_index & 63;
    repeated = c.last_root_valid[compute][root_kind][i] &&
               c.last_root[compute][root_kind][i] == root_value;
    c.last_root[compute][root_kind][i] = root_value;
    c.last_root_valid[compute][root_kind][i] = true;
  } else {
    std::vector<uintmax_t>& last = c.last[command];
    repeated = c.last_valid[command] && last.size() == args_elements &&
               std::equal(last.begin(), last.end(), args);
    last.assign(args, args + args_elements);
    c.last_valid[command] = true;
  }
  if (repeated) {
    ++c.repeated[command];
  }
}

void CommandCensusMaybeReport(CommandCensus& c) {
  auto now = std::chrono::steady_clock::now();
  if (now < c.next_report) {
    return;
  }
  c.next_report = now + std::chrono::seconds(10);
  u64 draws = c.count[static_cast<u32>(8)] + c.count[static_cast<u32>(9)];
  std::FILE* out = nullptr;
  if (draws && fopen_s(&out, "mp_cmdcensus.txt", "a") == 0 && out) {
    std::time_t t = std::time(nullptr);
    std::tm tm_local;
    localtime_s(&tm_local, &t);
    u64 total = 0;
    for (u64 n : c.count) total += n;
    std::fprintf(out, "%02d:%02d:%02d draws=%llu executes=%llu commands/draw=%.2f\n",
                 tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
                 static_cast<unsigned long long>(draws),
                 static_cast<unsigned long long>(c.executes), double(total) / draws);
    std::vector<std::pair<u64, u32>> order;
    for (u32 i = 0; i < CommandCensus::kTypes; ++i) {
      if (c.count[i]) order.emplace_back(c.count[i], i);
    }
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [n, i] : order) {
      std::fprintf(out, "  %-30s %8.3f/draw  repeated %5.1f%%  %6.1f B avg\n", CommandName(i),
                   double(n) / draws, 100.0 * c.repeated[i] / n, double(c.bytes[i]) / n);
    }
    std::fclose(out);
  }
  std::memset(c.count, 0, sizeof(c.count));
  std::memset(c.repeated, 0, sizeof(c.repeated));
  std::memset(c.bytes, 0, sizeof(c.bytes));
  c.executes = 0;
}
}  // namespace

DeferredCommandList::DeferredCommandList(const D3D12CommandProcessor& command_processor,
                                         size_t initial_size_bytes)
    : command_processor_(command_processor) {
  command_stream_.Reserve(initial_size_bytes);
}

void DeferredCommandList::Reset() {
  command_stream_.Clear();
}

void DeferredCommandList::Execute(ID3D12GraphicsCommandList* command_list,
                                  ID3D12GraphicsCommandList1* command_list_1) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES
  const uintmax_t* stream = command_stream_.Data();
  size_t stream_remaining = command_stream_.Size();
  ID3D12PipelineState* current_pipeline_state = nullptr;
  const bool census = command_processor_.GetD3D12Settings().command_census.Get();
  if (census) {
    CommandCensusReset(g_command_census);
    ++g_command_census.executes;
  }
  while (stream_remaining != 0) {
    const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;
    if (census) {
      bool is_root = false, compute = false;
      u32 root_kind = 0, root_index = 0;
      u64 root_value = 0;
      switch (header.command) {
        case Command::kD3DSetComputeRootDescriptorTable:
        case Command::kD3DSetGraphicsRootDescriptorTable: {
          auto& a = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
          is_root = true;
          compute = header.command == Command::kD3DSetComputeRootDescriptorTable;
          root_kind = 0;
          root_index = a.root_parameter_index;
          root_value = a.base_descriptor.ptr;
        } break;
        case Command::kD3DSetComputeRootConstantBufferView:
        case Command::kD3DSetGraphicsRootConstantBufferView:
        case Command::kD3DSetComputeRootShaderResourceView:
        case Command::kD3DSetGraphicsRootShaderResourceView:
        case Command::kD3DSetComputeRootUnorderedAccessView:
        case Command::kD3DSetGraphicsRootUnorderedAccessView: {
          auto& a = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
          is_root = true;
          compute = header.command == Command::kD3DSetComputeRootConstantBufferView ||
                    header.command == Command::kD3DSetComputeRootShaderResourceView ||
                    header.command == Command::kD3DSetComputeRootUnorderedAccessView;
          root_kind = 1;
          root_index = a.root_parameter_index;
          root_value = a.buffer_location;
        } break;
        case Command::kD3DSetComputeRootSignature:
        case Command::kD3DSetGraphicsRootSignature:
        case Command::kSetDescriptorHeaps:
          std::memset(g_command_census.last_root_valid, 0,
                      sizeof(g_command_census.last_root_valid));
          break;
        default:
          break;
      }
      CommandCensusRecord(g_command_census, static_cast<u32>(header.command), stream,
                          header.arguments_size_elements, is_root, compute, root_kind,
                          root_index, root_value);
    }
    switch (header.command) {
      case Command::kD3DClearDepthStencilView: {
        auto& args = *reinterpret_cast<const ClearDepthStencilViewHeader*>(stream);
        command_list->ClearDepthStencilView(
            args.depth_stencil_view, args.clear_flags, args.depth, args.stencil, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearRenderTargetView: {
        auto& args = *reinterpret_cast<const ClearRenderTargetViewHeader*>(stream);
        command_list->ClearRenderTargetView(
            args.render_target_view, args.color_rgba, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearUnorderedAccessViewUint: {
        auto& args = *reinterpret_cast<const ClearUnorderedAccessViewHeader*>(stream);
        command_list->ClearUnorderedAccessViewUint(
            args.view_gpu_handle_in_current_heap, args.view_cpu_handle, args.resource,
            args.values_uint, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DCopyBufferRegion: {
        auto& args = *reinterpret_cast<const D3DCopyBufferRegionArguments*>(stream);
        command_list->CopyBufferRegion(args.dst_buffer, args.dst_offset, args.src_buffer,
                                       args.src_offset, args.num_bytes);
      } break;
      case Command::kD3DCopyResource: {
        auto& args = *reinterpret_cast<const D3DCopyResourceArguments*>(stream);
        command_list->CopyResource(args.dst_resource, args.src_resource);
      } break;
      case Command::kCopyTexture: {
        auto& args = *reinterpret_cast<const CopyTextureArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, 0, 0, 0, &args.src, nullptr);
      } break;
      case Command::kD3DCopyTextureRegion: {
        auto& args = *reinterpret_cast<const D3DCopyTextureRegionArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, args.dst_x, args.dst_y, args.dst_z, &args.src,
                                        args.has_src_box ? &args.src_box : nullptr);
      } break;
      case Command::kD3DDispatch: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDispatchArguments*>(stream);
          command_list->Dispatch(args.thread_group_count_x, args.thread_group_count_y,
                                 args.thread_group_count_z);
        }
      } break;
      case Command::kD3DDrawIndexedInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawIndexedInstancedArguments*>(stream);
          command_list->DrawIndexedInstanced(args.index_count_per_instance, args.instance_count,
                                             args.start_index_location, args.base_vertex_location,
                                             args.start_instance_location);
        }
      } break;
      case Command::kD3DDrawInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawInstancedArguments*>(stream);
          command_list->DrawInstanced(args.vertex_count_per_instance, args.instance_count,
                                      args.start_vertex_location, args.start_instance_location);
        }
      } break;
      case Command::kD3DBeginQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->BeginQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DEndQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->EndQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DResolveQueryData: {
        auto& args = *reinterpret_cast<const D3DResolveQueryDataArguments*>(stream);
        command_list->ResolveQueryData(args.query_heap, args.type, args.start_index,
                                       args.num_queries, args.destination_buffer,
                                       args.aligned_destination_buffer_offset);
      } break;
      case Command::kD3DIASetIndexBuffer: {
        auto view = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(stream);
        command_list->IASetIndexBuffer(view->Format != DXGI_FORMAT_UNKNOWN ? view : nullptr);
      } break;
      case Command::kD3DIASetPrimitiveTopology: {
        command_list->IASetPrimitiveTopology(
            *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY*>(stream));
      } break;
      case Command::kD3DIASetVertexBuffers: {
        static_assert(alignof(D3D12_VERTEX_BUFFER_VIEW) <= alignof(uintmax_t));
        auto& args = *reinterpret_cast<const D3DIASetVertexBuffersHeader*>(stream);
        command_list->IASetVertexBuffers(args.start_slot, args.num_views,
                                         reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(
                                             reinterpret_cast<const u8*>(stream) +
                                             rex::RoundUpPow2(sizeof(D3DIASetVertexBuffersHeader),
                                                              alignof(D3D12_VERTEX_BUFFER_VIEW))));
      } break;
      case Command::kD3DOMSetBlendFactor: {
        command_list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(stream));
      } break;
      case Command::kD3DOMSetRenderTargets: {
        auto& args = *reinterpret_cast<const D3DOMSetRenderTargetsArguments*>(stream);
        command_list->OMSetRenderTargets(
            args.num_render_target_descriptors, args.render_target_descriptors,
            args.rts_single_handle_to_descriptor_range ? TRUE : FALSE,
            args.depth_stencil ? &args.depth_stencil_descriptor : nullptr);
      } break;
      case Command::kD3DOMSetStencilRef: {
        command_list->OMSetStencilRef(*reinterpret_cast<const UINT*>(stream));
      } break;
      case Command::kD3DResourceBarrier: {
        static_assert(alignof(D3D12_RESOURCE_BARRIER) <= alignof(uintmax_t));
        command_list->ResourceBarrier(
            *reinterpret_cast<const UINT*>(stream),
            reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(
                reinterpret_cast<const u8*>(stream) +
                rex::RoundUpPow2(sizeof(UINT), alignof(D3D12_RESOURCE_BARRIER))));
      } break;
      case Command::kRSSetScissorRect: {
        command_list->RSSetScissorRects(1, reinterpret_cast<const D3D12_RECT*>(stream));
      } break;
      case Command::kRSSetViewport: {
        command_list->RSSetViewports(1, reinterpret_cast<const D3D12_VIEWPORT*>(stream));
      } break;
      case Command::kD3DSetComputeRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetComputeRoot32BitConstants(args->root_parameter_index,
                                                   args->num_32bit_values_to_set, args + 1,
                                                   args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetGraphicsRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetGraphicsRoot32BitConstants(args->root_parameter_index,
                                                    args->num_32bit_values_to_set, args + 1,
                                                    args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetComputeRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootConstantBufferView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootConstantBufferView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetComputeRootDescriptorTable(args.root_parameter_index,
                                                    args.base_descriptor);
      } break;
      case Command::kD3DSetGraphicsRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetGraphicsRootDescriptorTable(args.root_parameter_index,
                                                     args.base_descriptor);
      } break;
      case Command::kD3DSetComputeRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootShaderResourceView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootShaderResourceView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootSignature: {
        command_list->SetComputeRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetGraphicsRootSignature: {
        command_list->SetGraphicsRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetComputeRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootUnorderedAccessView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootUnorderedAccessView(args.root_parameter_index,
                                                         args.buffer_location);
      } break;
      case Command::kSetDescriptorHeaps: {
        auto& args = *reinterpret_cast<const SetDescriptorHeapsArguments*>(stream);
        UINT num_descriptor_heaps = 0;
        ID3D12DescriptorHeap* descriptor_heaps[2];
        if (args.cbv_srv_uav_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.cbv_srv_uav_descriptor_heap;
        }
        if (args.sampler_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.sampler_descriptor_heap;
        }
        command_list->SetDescriptorHeaps(num_descriptor_heaps, descriptor_heaps);
      } break;
      case Command::kD3DSetPipelineState: {
        current_pipeline_state = *reinterpret_cast<ID3D12PipelineState* const*>(stream);
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kSetPipelineStateHandle: {
        current_pipeline_state =
            command_processor_.GetD3D12PipelineByHandle(*reinterpret_cast<void* const*>(stream));
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kD3DSetSamplePositions: {
        if (command_list_1 != nullptr) {
          auto& args = *reinterpret_cast<const D3DSetSamplePositionsArguments*>(stream);
          command_list_1->SetSamplePositions(
              args.num_samples_per_pixel, args.num_pixels,
              (args.num_samples_per_pixel && args.num_pixels)
                  ? const_cast<D3D12_SAMPLE_POSITION*>(args.sample_positions)
                  : nullptr);
        }
      } break;
      case Command::kBeginDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(reinterpret_cast<const u8*>(stream) +
                                                               sizeof(DebugMarkerHeader));
        command_list->BeginEvent(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      case Command::kEndDebugMarker: {
        command_list->EndEvent();
      } break;
      case Command::kInsertDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(reinterpret_cast<const u8*>(stream) +
                                                               sizeof(DebugMarkerHeader));
        command_list->SetMarker(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      default:
        REX_UNHANDLED_CASE(header.command);
        break;
    }
    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }
  if (census) {
    CommandCensusMaybeReport(g_command_census);
  }
}

void* DeferredCommandList::WriteCommand(Command command, size_t arguments_size_bytes) {
  if (gpu_prof_ && gpu_prof_->active) {
    switch (command) {
      case Command::kD3DClearDepthStencilView:
      case Command::kD3DClearRenderTargetView:
      case Command::kD3DClearUnorderedAccessViewUint:
      case Command::kD3DCopyBufferRegion:
      case Command::kD3DCopyResource:
      case Command::kCopyTexture:
      case Command::kD3DCopyTextureRegion:
      case Command::kD3DDispatch:
      case Command::kD3DDrawIndexedInstanced:
      case Command::kD3DDrawInstanced:
        if (gpu_prof_->category != gpu_prof_->last_marked && gpu_prof_->count < GpuProf::kMax - 2) {
          u32 index = gpu_prof_->count++;
          gpu_prof_->cats[index] = gpu_prof_->category;
          gpu_prof_->last_marked = gpu_prof_->category;
          D3DEndQuery(gpu_prof_->heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
        }
        break;
      default:
        break;
    }
  }
  return command_stream_.Write(command, arguments_size_bytes);
}

}  // namespace rex::gpu::d3d12
