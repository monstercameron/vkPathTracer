#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracerInternal.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace vkpt::gpu {

namespace {

vkpt::core::Status D3D12InitFailure(
    std::string message,
    vkpt::core::StatusCode code = vkpt::core::StatusCode::InternalError) {
  return vkpt::core::Status::error(code, std::move(message));
}

}  // namespace

void D3D12GpuPathTracer::shutdown() {
  if (!m_device) return;
  EmitGpuBackendStopped("d3d12", introspect());
  // Stop GPU work before releasing resources; mapped upload/readback pointers
  // remain valid only while their owning committed resources are alive.
  (void)wait_for_gpu();
  destroy_film_buffer();
  destroy_scene_buffers();
  if (m_uploadPtr && m_uploadBuf) {
    m_uploadBuf->Unmap(0, nullptr);
    m_uploadPtr = nullptr;
  }
  m_uploadBuf.Reset();
  destroy_dxr_resources();
  m_dxrCmdList.Reset();
  m_dxrCmdAllocator.Reset();
  m_dxrCmdQueue.Reset();
  m_device5.Reset();
  destroy_frame_contexts();
  m_cmdList.Reset();
  m_cmdAllocator.Reset();
  m_pso.Reset();
  m_tonemapPso.Reset();
  m_denoisePso.Reset();
  m_guidePso.Reset();
  m_temporalPso.Reset();
  m_rootSig.Reset();
  if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
  if (m_timelineFenceEvent) { CloseHandle(m_timelineFenceEvent); m_timelineFenceEvent = nullptr; }
  m_timelineFence.Reset();
  m_cmdQueue.Reset();
  m_device.Reset();
  m_factory.Reset();
  m_valid = false;
  m_dxrRuntimeObjectsReady = false;
  m_usingDxrDispatch = false;
  m_dxrAccelReady    = false;
  m_dxrPipelineReady = false;
}

// ============================================================================
// Init helpers
// ============================================================================

std::vector<D3D12GpuPathTracer::AdapterDescriptor>
D3D12GpuPathTracer::EnumerateAdapters() {
  std::vector<AdapterDescriptor> out;
  UINT flags = 0;
#if defined(_DEBUG)
  flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)))) {
    return out;
  }
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    AdapterDescriptor info;
    info.index = i;
    info.description = std::wstring(desc.Description);
    info.dedicated_vram_bytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
    info.shared_system_bytes = static_cast<std::uint64_t>(desc.SharedSystemMemory);
    info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    // Cheap probe: try to create a non-realized ID3D12Device, then a real
    // device only if needed for a DXR feature query. The non-realized create
    // is the same call init_device() uses to skip unusable adapters.
    const bool creates = SUCCEEDED(D3D12CreateDevice(adapter.Get(),
                                                     D3D_FEATURE_LEVEL_11_0,
                                                     __uuidof(ID3D12Device),
                                                     nullptr));
    if (creates && !info.is_software) {
      Microsoft::WRL::ComPtr<ID3D12Device> probeDevice;
      if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&probeDevice)))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(probeDevice->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
          info.dxr_supported =
              opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
        }
      }
    }
    out.push_back(std::move(info));
  }
  return out;
}

vkpt::core::Status D3D12GpuPathTracer::init_device() {
#if defined(_DEBUG)
  Microsoft::WRL::ComPtr<ID3D12Debug> dbg;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
    dbg->EnableDebugLayer();
#endif

  UINT flags = 0;
#if defined(_DEBUG)
  flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  const HRESULT createFactoryHr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
  if (FAILED(createFactoryHr)) {
    m_error = "CreateDXGIFactory2 hr=" + FormatHr(createFactoryHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  // Enumerate adapters. Two selection modes are supported:
  //   - When `m_adapterIndex` is set: bind to the matching adapter from
  //     EnumAdapters1, so RenderCoordinator can hand each tracer instance a
  //     specific GPU. We still log the full enumeration for diagnostics.
  //   - When unset: legacy "max VRAM among non-software D3D12-capable
  //     adapters" selection is preserved exactly so single-GPU callers see
  //     byte-identical behavior.
  Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
  SIZE_T bestVram = 0;
  std::string bestName;
  int adapterCount = 0;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> indexedAdapter;
  std::string indexedName;
  SIZE_T indexedVram = 0;
  bool indexedSoftware = false;
  bool indexedCreates = false;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (m_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    const std::string name = WStringToUtf8(desc.Description);
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    const bool creates = SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), nullptr));
    std::ostringstream a;
    a << "adapter[" << i << "] " << name
      << " vram=" << (static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull))
      << "MB software=" << (software ? "true" : "false")
      << " create_ok=" << (creates ? "true" : "false");
    LogDebug(a.str());
    if (m_adapterIndex.has_value() && i == *m_adapterIndex) {
      indexedAdapter = adapter;
      indexedName = name;
      indexedVram = desc.DedicatedVideoMemory;
      indexedSoftware = software;
      indexedCreates = creates;
    }
    if (software || !creates) continue;
    ++adapterCount;
    if (desc.DedicatedVideoMemory > bestVram) {
      bestVram = desc.DedicatedVideoMemory;
      chosen = adapter;
      bestName = name;
    }
  }
  if (m_adapterIndex.has_value()) {
    if (!indexedAdapter) {
      m_error = "adapter index " + std::to_string(*m_adapterIndex) +
                " out of range";
      return D3D12InitFailure(m_error, vkpt::core::StatusCode::NotReady);
    }
    if (indexedSoftware || !indexedCreates) {
      m_error = "adapter index " + std::to_string(*m_adapterIndex) +
                " is not a usable D3D12 device (software=" +
                (indexedSoftware ? "true" : "false") +
                ", create_ok=" + (indexedCreates ? "true" : "false") + ")";
      return D3D12InitFailure(m_error, vkpt::core::StatusCode::NotReady);
    }
    chosen = indexedAdapter;
    bestVram = indexedVram;
    bestName = indexedName;
    LogInfo("D3D12 explicit adapter selection index=" +
            std::to_string(*m_adapterIndex) + " name=" + bestName);
  }
  if (!chosen) {
    m_factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen));
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(chosen->GetDesc1(&desc))) {
      m_error = "EnumWarpAdapter returned invalid adapter";
      return D3D12InitFailure(m_error, vkpt::core::StatusCode::NotReady);
    }
    bestVram = 0;
    bestName = WStringToUtf8(desc.Description);
    LogDebug("falling back to WARP adapter");
  }
  if (chosen && adapterCount == 0) {
    LogDebug("adapter enum had no non-software D3D12 devices");
  }
  m_gpuName = bestName;
  m_vramMb  = static_cast<uint32_t>(bestVram / (1024u * 1024u));
  std::ostringstream sel;
  sel << "Selected GPU: " << m_gpuName << "  VRAM=" << m_vramMb << " MB";
  LogInfo(sel.str());

  const HRESULT createDeviceHr = D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(&m_device));
  if (FAILED(createDeviceHr)) {
    m_error = "D3D12CreateDevice hr=" + FormatHr(createDeviceHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  // Probe DXR capability for migration planning and benchmark telemetry.
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
  const HRESULT opts5Hr = m_device->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
  if (SUCCEEDED(opts5Hr)) {
    m_dxrTier = opts5.RaytracingTier;
    m_dxrSupported = (m_dxrTier >= D3D12_RAYTRACING_TIER_1_0);
  } else {
    m_dxrTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    m_dxrSupported = false;
  }
  {
    std::ostringstream dxr;
    dxr << "DXR support=" << (m_dxrSupported ? "yes" : "no")
        << " tier=" << dxr_tier_string();
    LogInfo(dxr.str());
  }

  // The compute path can run on a compute-capable queue, but the command list
  // type must remain consistent across queue, allocator, and command list.
  const D3D12_COMMAND_LIST_TYPE commandListType = SelectComputeCommandListType();
  LogInfo(std::string("compute command queue type=") + CommandListTypeName(commandListType));
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type     = commandListType;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  const HRESULT createQueueHr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue));
  if (FAILED(createQueueHr)) {
    m_error = "CreateCommandQueue hr=" + FormatHr(createQueueHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  // Command allocator + list
  const HRESULT createAllocHr = m_device->CreateCommandAllocator(commandListType,
      IID_PPV_ARGS(&m_cmdAllocator));
  if (FAILED(createAllocHr)) {
    m_error = "CreateCommandAllocator hr=" + FormatHr(createAllocHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }
  const HRESULT createListHr = m_device->CreateCommandList(0, commandListType,
      m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
  if (FAILED(createListHr)) {
    m_error = "CreateCommandList hr=" + FormatHr(createListHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }
  const HRESULT closeListHr = m_cmdList->Close();
  if (FAILED(closeListHr)) {
    m_error = "CreateCommandList initial close hr=" + FormatHr(closeListHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  // Fence
  const HRESULT createFenceHr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(&m_fence));
  if (FAILED(createFenceHr)) {
    m_error = "CreateFence hr=" + FormatHr(createFenceHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent) {
    m_error = "CreateEventW";
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  // Persistent upload heap. Scene uploads suballocate from this mapped buffer
  // and copy into default-heap resources on the GPU queue.
  m_uploadSize = 256ull * 1024 * 1024;
  const D3D12_HEAP_PROPERTIES hp = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = m_uploadSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  const HRESULT createUploadHr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadBuf));
  if (FAILED(createUploadHr)) {
    m_error = "upload buffer hr=" + FormatHr(createUploadHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error, vkpt::core::StatusCode::AllocFailed);
  }
  const HRESULT mapUploadHr = m_uploadBuf->Map(0, nullptr, &m_uploadPtr);
  if (FAILED(mapUploadHr)) {
    m_error = "upload buffer map hr=" + FormatHr(mapUploadHr);
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }
  LogDebug("D3D12 device init success upload_heap=" + std::to_string(m_uploadSize));

  if (!init_frame_contexts()) {
    LogError("init_device: " + m_error);
    return D3D12InitFailure(m_error);
  }

  LogInfo("D3D12 device init success");
  return vkpt::core::Status::ok("D3D12 device initialized");
}

bool D3D12GpuPathTracer::init_frame_contexts() {
  // The render-tile path uses a small ring of (allocator, command list)
  // contexts plus a shared timeline fence. With kD3D12FrameCount in flight,
  // the CPU records frame N+1 while frame N is still executing on the GPU,
  // amortizing fence-wait latency that previously serialized every tile.
  const D3D12_COMMAND_LIST_TYPE commandListType = SelectComputeCommandListType();
  for (size_t i = 0; i < m_frameCtx.size(); ++i) {
    auto& ctx = m_frameCtx[i];
    const HRESULT createAllocHr = m_device->CreateCommandAllocator(
        commandListType, IID_PPV_ARGS(&ctx.allocator));
    if (FAILED(createAllocHr)) {
      m_error = "CreateCommandAllocator(frame=" + std::to_string(i) +
                ") hr=" + FormatHr(createAllocHr);
      return false;
    }
    const HRESULT createListHr = m_device->CreateCommandList(
        0, commandListType, ctx.allocator.Get(), nullptr,
        IID_PPV_ARGS(&ctx.cmdList));
    if (FAILED(createListHr)) {
      m_error = "CreateCommandList(frame=" + std::to_string(i) +
                ") hr=" + FormatHr(createListHr);
      return false;
    }
    const HRESULT closeHr = ctx.cmdList->Close();
    if (FAILED(closeHr)) {
      m_error = "frame cmd list initial close hr=" + FormatHr(closeHr);
      return false;
    }
    ctx.fence_value = 0;
    ctx.in_flight = false;
  }
  const HRESULT createTimelineHr = m_device->CreateFence(
      0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_timelineFence));
  if (FAILED(createTimelineHr)) {
    m_error = "CreateFence(timeline) hr=" + FormatHr(createTimelineHr);
    return false;
  }
  m_timelineFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_timelineFenceEvent) {
    m_error = "CreateEventW(timeline)";
    return false;
  }
  m_nextFenceValue = 1;
  m_frameRingIdx = 0;
  return true;
}

void D3D12GpuPathTracer::destroy_frame_contexts() {
  for (auto& ctx : m_frameCtx) {
    ctx.cmdList.Reset();
    ctx.allocator.Reset();
    ctx.fence_value = 0;
    ctx.in_flight = false;
  }
}

vkpt::core::Status D3D12GpuPathTracer::init_dxr_runtime_objects() {
  if (!m_dxrSupported || !m_device5) {
    m_error = "DXR runtime requested before ID3D12Device5 support is available";
    return D3D12InitFailure(m_error, vkpt::core::StatusCode::Unsupported);
  }

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  const HRESULT createQueueHr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_dxrCmdQueue));
  if (FAILED(createQueueHr)) {
    m_error = "CreateCommandQueue(DIRECT) hr=" + FormatHr(createQueueHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }

  const HRESULT createAllocHr = m_device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_dxrCmdAllocator));
  if (FAILED(createAllocHr)) {
    m_error = "CreateCommandAllocator(DIRECT) hr=" + FormatHr(createAllocHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }

  const HRESULT createListHr = m_device5->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_dxrCmdAllocator.Get(), nullptr,
      IID_PPV_ARGS(&m_dxrCmdList));
  if (FAILED(createListHr)) {
    m_error = "CreateCommandList4(DIRECT) hr=" + FormatHr(createListHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }
  const HRESULT closeDxrListHr = m_dxrCmdList->Close();
  if (FAILED(closeDxrListHr)) {
    m_error = "CreateCommandList4(DIRECT) initial close hr=" + FormatHr(closeDxrListHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }
  m_dxrRuntimeObjectsReady = true;

  // Keep a DXR fence even though current DispatchRays uses the compute queue;
  // it preserves the split-queue path for future experiments and diagnostics.
  const HRESULT createDxrFenceHr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(&m_dxrFence));
  if (FAILED(createDxrFenceHr)) {
    m_error = "CreateFence(DXR) hr=" + FormatHr(createDxrFenceHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }
  m_dxrFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_dxrFenceEvent) {
    m_error = "CreateEventW(DXR)";
    LogError("init_dxr_runtime_objects: " + m_error);
    return D3D12InitFailure(m_error);
  }

  std::ostringstream st;
  st << "DXR runtime objects ready tier=" << dxr_tier_string();
  LogInfo(st.str());
  return vkpt::core::Status::ok("D3D12 DXR runtime initialized");
}

bool D3D12GpuPathTracer::wait_for_gpu() {
  // Fence every submitted batch before CPU readback or resource reuse. This is
  // conservative but keeps persistent mapped upload/readback buffers simple.
  // Also drains any in-flight per-frame ring submissions so subsequent cold-path
  // operations can safely reuse m_cmdAllocator/m_cmdList.
  ++m_fenceValue;
  const HRESULT signalHr = m_cmdQueue->Signal(m_fence.Get(), m_fenceValue);
  if (FAILED(signalHr)) {
    m_error = "Signal failed hr=" + FormatHr(signalHr);
    LogError("wait_for_gpu: " + m_error);
    return false;
  }
  if (m_fence->GetCompletedValue() < m_fenceValue) {
    const HRESULT setEvHr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    if (FAILED(setEvHr)) {
      m_error = "SetEventOnCompletion failed hr=" + FormatHr(setEvHr);
      LogError("wait_for_gpu: " + m_error);
      return false;
    }
    const DWORD waitRes = WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    if (waitRes != WAIT_OBJECT_0) {
      m_error = "WaitForSingleObjectEx failed code=" + std::to_string(waitRes);
      LogError("wait_for_gpu: " + m_error);
      return false;
    }
  }
  // Drain the timeline fence too. After wait_for_gpu the caller assumes both
  // m_cmdList and any frame-context submission have retired so allocator
  // resets are safe.
  if (m_timelineFence) {
    const UINT64 latest = m_nextFenceValue == 0u ? 0u : (m_nextFenceValue - 1u);
    if (latest > 0u && m_timelineFence->GetCompletedValue() < latest) {
      if (!wait_for_fence(latest)) {
        return false;
      }
    }
    for (auto& ctx : m_frameCtx) {
      ctx.in_flight = false;
    }
  }
  return true;
}

bool D3D12GpuPathTracer::wait_for_fence(UINT64 target) {
  if (!m_timelineFence || target == 0u) {
    return true;
  }
  if (m_timelineFence->GetCompletedValue() >= target) {
    return true;
  }
  const HRESULT setEvHr =
      m_timelineFence->SetEventOnCompletion(target, m_timelineFenceEvent);
  if (FAILED(setEvHr)) {
    m_error = "wait_for_fence SetEventOnCompletion hr=" + FormatHr(setEvHr);
    LogError("wait_for_fence: " + m_error);
    return false;
  }
  const DWORD waitRes =
      WaitForSingleObjectEx(m_timelineFenceEvent, INFINITE, FALSE);
  if (waitRes != WAIT_OBJECT_0) {
    m_error = "wait_for_fence wait code=" + std::to_string(waitRes);
    LogError("wait_for_fence: " + m_error);
    return false;
  }
  return true;
}

// ============================================================================
// DXR: compile DXIL, create global root sig, build pipeline, build AS, dispatch
// ============================================================================

bool D3D12GpuPathTracer::wait_for_dxr_gpu() {
  // Mirrors wait_for_gpu() for the optional direct DXR queue.
  ++m_dxrFenceValue;
  const HRESULT signalHr = m_dxrCmdQueue->Signal(m_dxrFence.Get(), m_dxrFenceValue);
  if (FAILED(signalHr)) {
    m_error = "dxr Signal hr=" + FormatHr(signalHr);
    LogError("wait_for_dxr_gpu: " + m_error);
    return false;
  }
  if (m_dxrFence->GetCompletedValue() < m_dxrFenceValue) {
    m_dxrFence->SetEventOnCompletion(m_dxrFenceValue, m_dxrFenceEvent);
    if (WaitForSingleObjectEx(m_dxrFenceEvent, INFINITE, FALSE) != WAIT_OBJECT_0) {
      m_error = "dxr WaitForSingleObjectEx failed";
      LogError("wait_for_dxr_gpu: " + m_error);
      return false;
    }
  }
  return true;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
