#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.h"
#include "core/Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

namespace {
void LogInfo(const std::string& msg) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "d3d12", msg);
}
void LogError(const std::string& msg) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "d3d12", msg);
}
} // namespace

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point)
    : m_hlslPath(std::move(hlsl_path)), m_entryPoint(std::move(entry_point)) {
  m_valid = init_device() && create_root_sig_and_pso();
  if (!m_valid) LogError("D3D12GpuPathTracer init failed: " + m_error);
}

D3D12GpuPathTracer::~D3D12GpuPathTracer() { shutdown(); }

// ============================================================================
// IPathTracer
// ============================================================================

bool D3D12GpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) return false;
  m_settings   = s;
  m_configured = true;
  m_filmPixels = s.width * s.height;
  m_film.resize(s.width, s.height);
  m_film.clear();
  m_counters          = {};
  m_hasScene          = false;
  m_sceneUploaded     = false;
  destroy_film_buffer();
  return create_film_buffer();
}

bool D3D12GpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::RTSceneData& scene) {
  if (!m_configured) return false;
  m_sceneData      = scene;
  m_hasScene       = true;
  m_sceneUploaded  = false;
  return true;
}

bool D3D12GpuPathTracer::build_or_update_acceleration() {
  if (!m_hasScene) return false;
  m_gpuVerts.clear();
  for (const auto& v : m_sceneData.vertices)
    { m_gpuVerts.push_back(v.x); m_gpuVerts.push_back(v.y); m_gpuVerts.push_back(v.z); }
  if (m_gpuVerts.empty()) m_gpuVerts.assign(3, 0.0f);

  m_gpuIdx = m_sceneData.indices;
  if (m_gpuIdx.empty()) m_gpuIdx.push_back(0u);

  m_gpuMats.clear();
  for (const auto& m : m_sceneData.materials) {
    m_gpuMats.push_back(m.albedo.x); m_gpuMats.push_back(m.albedo.y);
    m_gpuMats.push_back(m.albedo.z);
    m_gpuMats.push_back(m.emissive.x); m_gpuMats.push_back(m.emissive.y);
    m_gpuMats.push_back(m.emissive.z);
    m_gpuMats.push_back(m.roughness); m_gpuMats.push_back(0.0f);
  }
  if (m_gpuMats.empty()) m_gpuMats.assign(8, 0.0f);

  m_gpuInsts.clear();
  for (const auto& inst : m_sceneData.instances) {
    m_gpuInsts.push_back(inst.first_triangle);
    m_gpuInsts.push_back(inst.triangle_count);
    m_gpuInsts.push_back(inst.material_index);
    m_gpuInsts.push_back(0u);
  }
  if (m_gpuInsts.empty()) m_gpuInsts.assign(4, 0u);

  m_gpuLights.clear();
  for (const auto& lt : m_sceneData.lights) {
    m_gpuLights.push_back(lt.position.x); m_gpuLights.push_back(lt.position.y);
    m_gpuLights.push_back(lt.position.z);
    m_gpuLights.push_back(lt.color.x); m_gpuLights.push_back(lt.color.y);
    m_gpuLights.push_back(lt.color.z);
    m_gpuLights.push_back(lt.intensity); m_gpuLights.push_back(0.0f);
  }
  if (m_gpuLights.empty()) m_gpuLights.assign(8, 0.0f);

  destroy_scene_buffers();
  if (!upload_scene_buffers()) return false;
  m_sceneUploaded = true;
  return true;
}

bool D3D12GpuPathTracer::reset_accumulation() {
  if (!m_configured || !m_filmBuf) return false;
  // Zero the film on GPU
  wait_for_gpu();
  const auto res = m_cmdAllocator->Reset();
  if (FAILED(res)) { m_error = "cmd allocator reset failed"; return false; }
  const auto res2 = m_cmdList->Reset(m_cmdAllocator.Get(), nullptr);
  if (FAILED(res2)) { m_error = "cmd list reset failed"; return false; }

  // Transition film to unordered-access
  D3D12_RESOURCE_BARRIER rb{};
  rb.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb.Transition.pResource   = m_filmBuf.Get();
  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  // Actually no, we need to clear it. Let's use a compute clear or copy from zeroed upload.
  // Simpler: allocate new zeroed upload, copy to film
  // Or: use ClearUnorderedAccessViewUint with a descriptor (requires GPU descriptor heap)
  // Easiest: create a small zeroed upload buffer and copy subresource
  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  // We already have m_uploadBuf with m_uploadPtr zeroed in create_film_buffer
  // Actually, let's use a Clear buffer via descriptor.
  // Simplest approach that works: allocate a temp upload, copy zeros to film
  {
    Microsoft::WRL::ComPtr<ID3D12Resource> zeroBuf;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC   rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = filmSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count = 1;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zeroBuf)))) {
      m_error = "zeroBuf create failed"; return false;
    }
    void* p = nullptr;
    zeroBuf->Map(0, nullptr, &p);
    std::memset(p, 0, static_cast<size_t>(filmSize));
    zeroBuf->Unmap(0, nullptr);
    m_cmdList->CopyBufferRegion(m_filmBuf.Get(), 0, zeroBuf.Get(), 0, filmSize);
  }
  m_cmdList->Close();
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  wait_for_gpu();

  m_film.clear();
  m_counters = {};
  return true;
}

bool D3D12GpuPathTracer::render_sample_batch(uint32_t, uint32_t,
    uint32_t sample_idx, uint32_t frame_idx) {
  if (!m_valid || !m_configured || !m_sceneUploaded) return false;

  const auto& sc = m_sceneData;

  // Camera basis
  auto norm3 = [](float x, float y, float z, float* out) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0]=x/l; out[1]=y/l; out[2]=z/l;
  };
  float fwd[3]; norm3(sc.camera_target.x - sc.camera_position.x,
    sc.camera_target.y - sc.camera_position.y,
    sc.camera_target.z - sc.camera_position.z, fwd);
  float rt[3] = {fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
    fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
    fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x};
  float rn[3]; norm3(rt[0],rt[1],rt[2],rn);
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
    rn[2]*fwd[0]-rn[0]*fwd[2], rn[0]*fwd[1]-rn[1]*fwd[0],un);

  PathTraceConstants pc{};
  pc.camera_pos_x = sc.camera_position.x;
  pc.camera_pos_y = sc.camera_position.y;
  pc.camera_pos_z = sc.camera_position.z;
  pc.fov_tan_half = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_fwd_x=fwd[0]; pc.cam_fwd_y=fwd[1]; pc.cam_fwd_z=fwd[2];
  pc.aspect = static_cast<float>(m_settings.width) /
              std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right_x=rn[0]; pc.cam_right_y=rn[1]; pc.cam_right_z=rn[2];
  pc.pad0 = 0.0f;
  pc.cam_up_x=un[0]; pc.cam_up_y=un[1]; pc.cam_up_z=un[2];
  pc.sample_index = sample_idx;
  pc.num_insts    = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats     = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights   = static_cast<uint32_t>(sc.lights.size());
  pc.width        = m_settings.width;
  pc.height       = m_settings.height;
  pc.base_seed    = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                    ^ (frame_idx * 2654435761u);
  pc.env_r = sc.environment_color.x;
  pc.env_g = sc.environment_color.y;
  pc.env_b = sc.environment_color.z;
  pc.max_depth_f = static_cast<float>(std::max(1u, m_settings.max_depth));

  // Reset command list
  if (FAILED(m_cmdAllocator->Reset())) { m_error = "allocator reset"; return false; }
  if (FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), m_pso.Get()))) { m_error = "cmd list reset"; return false; }

  // Upload constants
  m_cmdList->SetComputeRootSignature(m_rootSig.Get());

  // Create descriptor heap for SRVs/UAVs
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 6;
  heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
  if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)))) {
    m_error = "srv heap"; return false;
  }
  UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();

  // Bind SRVs (t0-t4)
  auto makeSrv = [&](int slot, ID3D12Resource* buf, UINT64 size, DXGI_FORMAT fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = static_cast<UINT>(size / 4u);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(slot) * inc;
    m_device->CreateShaderResourceView(buf, &srv, h);
  };
  makeSrv(0, m_vertBuf.Get(),  m_gpuVerts.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(1, m_idxBuf.Get(),   m_gpuIdx.size()  * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(2, m_matBuf.Get(),   m_gpuMats.size()  * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(3, m_instBuf.Get(),  m_gpuInsts.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(4, m_ltBuf.Get(),    m_gpuLights.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);

  // Bind UAV (u0) — film buffer as float4
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(m_filmPixels);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(5) * inc;
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &uav, h);
  }

  m_cmdList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());
  m_cmdList->SetComputeRootDescriptorTable(1, gpuHandle);

  // Root constants (b0) — 28 DWORDs = 112 bytes
  m_cmdList->SetComputeRoot32BitConstants(0, 28, &pc, 0);

  // Transition film to UAV if needed
  D3D12_RESOURCE_BARRIER rb{};
  rb.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb.Transition.pResource   = m_filmBuf.Get();
  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_cmdList->ResourceBarrier(1, &rb);

  m_cmdList->Dispatch((m_settings.width + 7u) / 8u,
                      (m_settings.height + 7u) / 8u, 1u);

  // Transition film back to common for readback
  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
  m_cmdList->ResourceBarrier(1, &rb);

  // Copy film to readback buffer
  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  m_cmdList->CopyBufferRegion(m_filmReadbackBuf.Get(), 0, m_filmBuf.Get(), 0, filmSize);

  // Return film to COMMON state for next frame
  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  m_cmdList->ResourceBarrier(1, &rb);

  m_cmdList->Close();
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  wait_for_gpu();

  // Rebuild CPU FilmBuffer from readback
  const float* src = reinterpret_cast<const float*>(m_filmReadbackPtr);
  m_film.resize(m_settings.width, m_settings.height);
  m_film.clear();
  for (uint32_t y = 0; y < m_settings.height; ++y) {
    for (uint32_t x = 0; x < m_settings.width; ++x) {
      const uint32_t p   = (y * m_settings.width + x) * 4u;
      const float    cnt = std::max(1.0f, src[p + 3u]);
      const vkpt::pathtracer::Vec3 avg{src[p]/cnt, src[p+1]/cnt, src[p+2]/cnt};
      m_film.add_sample(x, y, avg);
    }
  }
  m_counters.samples += static_cast<uint64_t>(m_settings.width) * m_settings.height;
  m_counters.rays    += m_counters.samples;
  return true;
}

vkpt::pathtracer::FilmLdr D3D12GpuPathTracer::resolve_ldr() const {
  return m_film.resolve_ldr();
}
vkpt::pathtracer::FilmHdr D3D12GpuPathTracer::resolve_hdr() const {
  return m_film.resolve_hdr();
}
vkpt::pathtracer::SampleCounters D3D12GpuPathTracer::read_counters() const {
  return m_counters;
}

void D3D12GpuPathTracer::shutdown() {
  if (!m_device) return;
  wait_for_gpu();
  destroy_film_buffer();
  destroy_scene_buffers();
  m_uploadBuf.Reset();
  m_cmdList.Reset();
  m_cmdAllocator.Reset();
  m_pso.Reset();
  m_rootSig.Reset();
  if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
  m_cmdQueue.Reset();
  m_device.Reset();
  m_factory.Reset();
  m_valid = false;
  m_uploadPtr = nullptr;
}

// ============================================================================
// Init helpers
// ============================================================================

bool D3D12GpuPathTracer::init_device() {
#if defined(_DEBUG)
  Microsoft::WRL::ComPtr<ID3D12Debug> dbg;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
    dbg->EnableDebugLayer();
#endif

  UINT flags = 0;
#if defined(_DEBUG)
  flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory)))) {
    m_error = "CreateDXGIFactory2"; return false;
  }

  // Enumerate adapters, pick the one with the most dedicated VRAM
  Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
  SIZE_T bestVram = 0;
  std::string bestName;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (m_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), nullptr))) continue;
    if (desc.DedicatedVideoMemory > bestVram) {
      bestVram = desc.DedicatedVideoMemory;
      chosen = adapter;
      int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
      bestName.resize(static_cast<size_t>(len > 0 ? len - 1 : 0));
      if (len > 0) WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
          &bestName[0], len, nullptr, nullptr);
      if (!bestName.empty() && bestName.back() == '\0') bestName.pop_back();
    }
  }
  if (!chosen) {
    m_factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen));
    DXGI_ADAPTER_DESC1 desc{};
    chosen->GetDesc1(&desc);
    bestVram = 0;
    int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    bestName.resize(static_cast<size_t>(len > 0 ? len - 1 : 0));
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
        &bestName[0], len, nullptr, nullptr);
    if (!bestName.empty() && bestName.back() == '\0') bestName.pop_back();
  }
  m_gpuName = bestName;
  m_vramMb  = static_cast<uint32_t>(bestVram / (1024u * 1024u));
  std::ostringstream sel;
  sel << "Selected GPU: " << m_gpuName << "  VRAM=" << m_vramMb << " MB";
  LogInfo(sel.str());

  if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(&m_device)))) {
    m_error = "D3D12CreateDevice"; return false;
  }

  // Command queue
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)))) {
    m_error = "CreateCommandQueue"; return false;
  }

  // Command allocator + list
  if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
      IID_PPV_ARGS(&m_cmdAllocator)))) {
    m_error = "CreateCommandAllocator"; return false;
  }
  if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
      m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)))) {
    m_error = "CreateCommandList"; return false;
  }
  m_cmdList->Close();

  // Fence
  if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(&m_fence)))) {
    m_error = "CreateFence"; return false;
  }
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent) { m_error = "CreateEventW"; return false; }

  // Upload buffer — 64 MB
  m_uploadSize = 64ull * 1024 * 1024;
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = m_uploadSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadBuf)))) {
    m_error = "upload buffer"; return false;
  }
  m_uploadBuf->Map(0, nullptr, &m_uploadPtr);

  LogInfo("D3D12 device init success");
  return true;
}

bool D3D12GpuPathTracer::create_root_sig_and_pso() {
  // Root sig: param0 = 25 32-bit constants (b0), param1 = descriptor table
  D3D12_ROOT_PARAMETER params[2]{};
  // Root constants (b0)
  params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace  = 0;
  params[0].Constants.Num32BitValues = 28;
  params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
  // Descriptor table (t0-t4 SRV + u0 UAV)
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors     = 5;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace      = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors     = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace      = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 5;
  params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges   = ranges;
  params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 2;
  rsd.pParameters   = params;
  rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(hr)) {
    m_error = "SerializeRootSignature";
    if (err) LogError(static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(),
      sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)))) {
    m_error = "CreateRootSignature"; return false;
  }

  // Load HLSL source and compile at runtime
  std::ifstream f(m_hlslPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) { m_error = "Cannot open HLSL: " + m_hlslPath; return false; }
  const auto sz = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::string src(sz, '\0');
  f.read(&src[0], static_cast<std::streamsize>(sz));
  f.close();

  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT compileHr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, m_entryPoint.c_str(), "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(compileHr)) {
    m_error = "D3DCompile failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature     = m_rootSig.Get();
  pso.CS.pShaderBytecode  = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength   = csBlob->GetBufferSize();
  if (FAILED(m_device->CreateComputePipelineState(&pso,
      IID_PPV_ARGS(&m_pso)))) {
    m_error = "CreateComputePipelineState"; return false;
  }
  LogInfo("D3D12 compute PSO created from " + m_hlslPath);
  return true;
}

bool D3D12GpuPathTracer::create_film_buffer() {
  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);

  // Default heap (GPU-visible UAV)
  D3D12_HEAP_PROPERTIES defhp{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = filmSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_filmBuf)))) {
    m_error = "film buf"; return false;
  }

  // Readback buffer
  D3D12_HEAP_PROPERTIES rdhp{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC   rd2{};
  rd2.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd2.Width            = filmSize;
  rd2.Height           = 1;
  rd2.DepthOrArraySize = 1;
  rd2.MipLevels        = 1;
  rd2.Format           = DXGI_FORMAT_UNKNOWN;
  rd2.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd2.SampleDesc.Count = 1;
  if (FAILED(m_device->CreateCommittedResource(&rdhp, D3D12_HEAP_FLAG_NONE, &rd2,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_filmReadbackBuf)))) {
    m_error = "film readback buf"; return false;
  }
  m_filmReadbackBuf->Map(0, nullptr, &m_filmReadbackPtr);
  return true;
}

bool D3D12GpuPathTracer::upload_scene_buffers() {
  wait_for_gpu();
  if (FAILED(m_cmdAllocator->Reset())) return false;
  if (FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) return false;

  struct UploadInfo { UINT64 size; ID3D12Resource** dstBuf; const void* data; };
  UINT64 offset = 0;
  auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };

  auto stage = [&](const void* data, UINT64 size, ID3D12Resource** dst) {
    if (offset + size > m_uploadSize) return false;
    std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + offset, data, static_cast<size_t>(size));

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC  rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=size; rd.Height=1;
    rd.DepthOrArraySize=1; rd.MipLevels=1; rd.Format=DXGI_FORMAT_UNKNOWN;
    rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.SampleDesc.Count=1;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(dst))))
      return false;
    m_cmdList->CopyBufferRegion(*dst, 0, m_uploadBuf.Get(), offset, size);
    offset = align(offset + size);
    return true;
  };

  if (!stage(m_gpuVerts.data(),  m_gpuVerts.size()  * sizeof(float), &m_vertBuf)) return false;
  if (!stage(m_gpuIdx.data(),    m_gpuIdx.size()    * sizeof(uint32_t), &m_idxBuf)) return false;
  if (!stage(m_gpuMats.data(),   m_gpuMats.size()   * sizeof(float), &m_matBuf)) return false;
  if (!stage(m_gpuInsts.data(),  m_gpuInsts.size()  * sizeof(uint32_t), &m_instBuf)) return false;
  if (!stage(m_gpuLights.data(), m_gpuLights.size() * sizeof(float), &m_ltBuf)) return false;

  // Transition all to non-pixel shader resource
  D3D12_RESOURCE_BARRIER barriers[5]{};
  ID3D12Resource* bufs[] = {m_vertBuf.Get(), m_idxBuf.Get(), m_matBuf.Get(),
                            m_instBuf.Get(), m_ltBuf.Get()};
  for (int i = 0; i < 5; ++i) {
    barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[i].Transition.pResource=bufs[i];
    barriers[i].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[i].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  m_cmdList->ResourceBarrier(5, barriers);
  m_cmdList->Close();
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  wait_for_gpu();
  return true;
}

void D3D12GpuPathTracer::destroy_scene_buffers() {
  m_vertBuf.Reset(); m_idxBuf.Reset(); m_matBuf.Reset();
  m_instBuf.Reset(); m_ltBuf.Reset();
  m_sceneUploaded = false;
}

void D3D12GpuPathTracer::destroy_film_buffer() {
  if (m_filmReadbackPtr && m_filmReadbackBuf) m_filmReadbackBuf->Unmap(0, nullptr);
  m_filmReadbackPtr = nullptr;
  m_filmReadbackBuf.Reset();
  m_filmBuf.Reset();
}

void D3D12GpuPathTracer::wait_for_gpu() {
  ++m_fenceValue;
  m_cmdQueue->Signal(m_fence.Get(), m_fenceValue);
  if (m_fence->GetCompletedValue() < m_fenceValue) {
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
  }
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
