#include "render/backends/D3D12Backend.h"
#include "cpu/CpuFeatures.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <cmath>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#if defined(_WIN32) && defined(PT_ENABLE_D3D12)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace vkpt::render {

namespace {

std::uint32_t DetectLogicalCores() {
  const auto count = std::thread::hardware_concurrency();
  return count == 0u ? 1u : count;
}

std::string SimdName(const vkpt::cpu::CpuFeatureSet& features) {
  const auto dispatch = vkpt::cpu::BuildSimdDispatchInfo(features);
  return vkpt::cpu::ToString(dispatch.preferred);
}

RenderBackendCapabilities MakeCpuBackendCaps(const vkpt::cpu::CpuFeatureSet& features,
                                             std::uint32_t logical_cores) {
  RenderBackendCapabilities caps;
  caps.backend_name = SimdName(features) == "scalar" ? "cpu-scalar" : "cpu-simd";
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = false;
  caps.timestamp_queries = true;
  caps.subgroups = false;
  caps.descriptor_indexing = false;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = false;
  caps.supports_present = false;
  caps.supports_multiqueue = logical_cores > 1u;
  caps.max_workgroup_size_x = logical_cores;
  caps.max_workgroup_size_y = 1u;
  caps.max_workgroup_size_z = 1u;
  caps.memory_model = "system-memory";
  caps.notes = "CPU ray generation path; not a D3D12 device.";
  caps.platform.platform_name = "cpu";
  caps.platform.headless = true;
  caps.cpu.logical_cores = logical_cores;
  caps.cpu.fma = features.fma;
  caps.cpu.notes = "Runtime CPU capability probe.";
  caps.simd.sse2 = features.sse2;
  caps.simd.sse42 = features.sse4_2;
  caps.simd.avx = features.avx;
  caps.simd.avx2 = features.avx2;
  caps.simd.avx512 = features.avx512f;
  caps.simd.neon = features.neon;
  caps.simd.sve = features.sve;
  caps.simd.best_mode = SimdName(features);
  caps.shader.supported_source_formats = {"cpp"};
  caps.shader.notes = "Native CPU kernels.";
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.rgba32_float = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "RGBA16F", "RGBA32F"};
  caps.memory_budget.budget_query = false;
  caps.memory_budget.shared_system_memory_bytes = 0u;
  caps.memory_budget.budget_unavailable_reason = "No process-wide RAM budget probe is wired into the render contracts.";
  return caps;
}

double EstimateCpuRaysPerMs(const vkpt::cpu::CpuFeatureSet& features, std::uint32_t logical_cores) {
  double vector_weight = 1.0;
  if (features.avx512f) {
    vector_weight = 6.0;
  } else if (features.avx2) {
    vector_weight = 4.0;
  } else if (features.avx) {
    vector_weight = 2.5;
  } else if (features.sse4_2 || features.neon) {
    vector_weight = 1.75;
  }
  return static_cast<double>(std::max<std::uint32_t>(1u, logical_cores)) * vector_weight * 5500.0;
}

AcceleratorCapabilities MakeCpuAccelerator() {
  const auto features = vkpt::cpu::QueryCpuFeatures();
  const auto logical_cores = DetectLogicalCores();
  AcceleratorCapabilities accel;
  accel.id = "cpu:process";
  accel.name = "CPU";
  accel.accelerator_kind = AcceleratorKind::Cpu;
  accel.backend_kind = BackendKind::Unknown;
  accel.available = true;
  accel.hardware = true;
  accel.cpu = true;
  accel.compute = true;
  accel.ray_tracing = false;
  accel.presentation = false;
  accel.node_count = logical_cores;
  accel.shared_system_memory_bytes = 0u;
  accel.estimated_rays_per_ms = EstimateCpuRaysPerMs(features, logical_cores);
  accel.notes = "CPU path tracer worker candidate; keep thread count capped so polygon/raster work keeps cores.";
  accel.backend_caps = MakeCpuBackendCaps(features, logical_cores);
  return accel;
}

#if defined(_WIN32) && defined(PT_ENABLE_D3D12)
double EstimateD3D12RaysPerMs(const AcceleratorCapabilities& accel) {
  if (!accel.available || !accel.compute) {
    return 0.0;
  }
  if (accel.warp) {
    return 25000.0;
  }
  double base = 180000.0;
  if (accel.accelerator_kind == AcceleratorKind::DiscreteGpu) {
    base = 850000.0;
    const double vram_gb =
        static_cast<double>(accel.dedicated_video_memory_bytes) / static_cast<double>(1024ull * 1024ull * 1024ull);
    base *= 1.0 + std::min(0.65, std::max(0.0, vram_gb) / 32.0);
  } else if (accel.accelerator_kind == AcceleratorKind::IntegratedGpu) {
    base = accel.cache_coherent_uma ? 260000.0 : 210000.0;
  } else if (accel.accelerator_kind == AcceleratorKind::VirtualGpu) {
    base = 120000.0;
  }
  if (accel.ray_tracing) {
    base *= 1.45;
  }
  if (accel.backend_caps.subgroups) {
    base *= 1.08;
  }
  return base;
}

std::string WStringToUtf8(const wchar_t* src) {
  if (!src) {
    return {};
  }
  const int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::string FormatHr(HRESULT hr) {
  std::ostringstream ss;
  ss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
     << static_cast<std::uint32_t>(hr);
  return ss.str();
}

std::string FormatLuid(LUID luid) {
  std::ostringstream ss;
  ss << std::hex << std::nouppercase << static_cast<std::uint32_t>(luid.HighPart) << ':'
     << static_cast<std::uint32_t>(luid.LowPart);
  return ss.str();
}

std::string DxrTierName(D3D12_RAYTRACING_TIER tier) {
  if (tier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
    return "none";
  }
  if (tier > D3D12_RAYTRACING_TIER_1_0) {
    return "1.1+";
  }
  if (tier == D3D12_RAYTRACING_TIER_1_0) {
    return "1.0";
  }
  return "unknown";
}

bool IsMicrosoftBasicAdapter(const DXGI_ADAPTER_DESC1& desc) {
  if (desc.VendorId == 0x1414u) {
    return true;
  }
  const auto name = WStringToUtf8(desc.Description);
  return name.find("Microsoft Basic Render Driver") != std::string::npos;
}

bool QueryD3D12Adapter(IDXGIAdapter1* adapter,
                       std::uint32_t ordinal,
                       bool warp,
                       AcceleratorCapabilities& out) {
  if (!adapter) {
    return false;
  }

  DXGI_ADAPTER_DESC1 desc{};
  if (FAILED(adapter->GetDesc1(&desc))) {
    return false;
  }

  const bool software = ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) || IsMicrosoftBasicAdapter(desc);
  const std::string luid = FormatLuid(desc.AdapterLuid);
  // Use the adapter LUID as the stable planner id so repeated probes can be
  // correlated with persisted budget diagnostics.
  out.id = (warp ? "d3d12:warp:" : "d3d12:") + luid;
  out.name = WStringToUtf8(desc.Description);
  out.backend_kind = BackendKind::D3d12;
  out.d3d12 = true;
  out.warp = warp || software;
  out.vendor_id = desc.VendorId;
  out.device_id = desc.DeviceId;
  out.adapter_luid = luid;
  out.dedicated_video_memory_bytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
  out.shared_system_memory_bytes = static_cast<std::uint64_t>(desc.SharedSystemMemory);
  out.notes = "DXGI adapter ordinal " + std::to_string(ordinal);

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  const HRESULT create_hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(create_hr) || !device) {
    out.available = false;
    out.hardware = !out.warp;
    out.compute = false;
    out.ray_tracing = false;
    out.accelerator_kind = out.warp ? AcceleratorKind::Warp : AcceleratorKind::Unknown;
    out.notes += "; D3D12CreateDevice failed hr=" + FormatHr(create_hr);
    return true;
  }

  out.available = true;
  out.hardware = !out.warp;
  out.compute = true;
  out.presentation = false;
  out.node_count = std::max<UINT>(1u, device->GetNodeCount());

  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  architecture.NodeIndex = 0u;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)))) {
    out.unified_memory = architecture.UMA != FALSE;
    out.cache_coherent_uma = architecture.CacheCoherentUMA != FALSE;
  }

  if (out.warp) {
    out.accelerator_kind = AcceleratorKind::Warp;
  } else if (out.unified_memory || desc.DedicatedVideoMemory == 0u) {
    out.accelerator_kind = AcceleratorKind::IntegratedGpu;
  } else {
    out.accelerator_kind = AcceleratorKind::DiscreteGpu;
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  const bool options_ok =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  const bool options1_ok =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  const bool options5_ok =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));

  out.ray_tracing = options5_ok && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;

  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO memory_info{};
    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_info))) {
      out.current_budget_bytes = static_cast<std::uint64_t>(memory_info.Budget);
      out.current_usage_bytes = static_cast<std::uint64_t>(memory_info.CurrentUsage);
    }
  }

  RenderBackendCapabilities caps;
  caps.backend_name = out.ray_tracing ? "d3d12-dxr" : "d3d12-compute";
  if (out.warp) {
    caps.backend_name = "d3d12-warp";
  }
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = true;
  caps.subgroups = options1_ok && options1.WaveOps != FALSE;
  caps.descriptor_indexing = options_ok && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
  caps.bindless_like_resources = options_ok && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
  caps.texture_formats = true;
  caps.ray_tracing = out.ray_tracing;
  caps.ray_query = out.ray_tracing && options5.RaytracingTier > D3D12_RAYTRACING_TIER_1_0;
  caps.ray_query_supported = caps.ray_query;
  caps.acceleration_structure_supported = out.ray_tracing;
  caps.shader_group_handle_size = out.ray_tracing ? D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES : 0u;
  caps.max_as_size = out.ray_tracing ? std::numeric_limits<std::uint64_t>::max() : 0u;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = false;
  caps.supports_present = false;
  caps.supports_multiqueue = true;
  caps.max_workgroup_size_x = 1024u;
  caps.max_workgroup_size_y = 1024u;
  caps.max_workgroup_size_z = 64u;
  caps.max_buffer_alignment = 256u;
  caps.memory_model = out.unified_memory ? "uma" : "dedicated-vram";
  caps.notes = "Native D3D12 capability probe for multi-accelerator ray planning.";
  caps.platform.platform_name = "d3d12";
  caps.platform.headless = true;
  caps.ray_tracing_caps.hardware_pipeline = out.ray_tracing;
  caps.ray_tracing_caps.acceleration_structures = out.ray_tracing;
  caps.ray_tracing_caps.inline_ray_tracing = caps.ray_query;
  caps.ray_tracing_caps.ray_query = caps.ray_query;
  caps.ray_tracing_caps.shader_group_handle_size = caps.shader_group_handle_size;
  caps.ray_tracing_caps.max_acceleration_structure_size = caps.max_as_size;
  caps.ray_tracing_caps.max_recursion_depth = out.ray_tracing ? 31u : 0u;
  caps.ray_tracing_caps.tier = options5_ok ? DxrTierName(options5.RaytracingTier) : "not-probed";
  if (!out.ray_tracing) {
    caps.ray_tracing_caps.unsupported_reason =
        options5_ok ? "D3D12_OPTIONS5 reports no DXR tier" : "D3D12_OPTIONS5 probe failed";
  }
  caps.shader.hlsl = true;
  caps.shader.dxil = true;
  caps.shader.subgroups = caps.subgroups;
  caps.shader.shader_model = "sm6";
  caps.shader.supported_source_formats = {"hlsl", "dxil"};
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.bgra8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.rgba32_float = true;
  caps.texture_formats_caps.depth32_float = true;
  caps.texture_formats_caps.storage_texture_formats = true;
  caps.texture_formats_caps.sampled_texture_formats = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "BGRA8", "RGBA16F", "RGBA32F", "D32F"};
  caps.memory_budget.budget_query = out.current_budget_bytes > 0u;
  caps.memory_budget.dedicated_video_memory_bytes = out.dedicated_video_memory_bytes;
  caps.memory_budget.shared_system_memory_bytes = out.shared_system_memory_bytes;
  caps.memory_budget.current_budget_bytes = out.current_budget_bytes;
  caps.memory_budget.current_usage_bytes = out.current_usage_bytes;
  caps.memory_budget.max_buffer_size_bytes = std::numeric_limits<std::uint64_t>::max();
  caps.memory_budget.upload_alignment_bytes = 256u;
  caps.memory_budget.readback_alignment_bytes = 256u;
  if (!caps.memory_budget.budget_query) {
    caps.memory_budget.budget_unavailable_reason = "IDXGIAdapter3 memory budget query unavailable or returned zero.";
  }

  out.backend_caps = std::move(caps);
  out.estimated_rays_per_ms = EstimateD3D12RaysPerMs(out);
  return true;
}

void PushUniqueD3D12Accelerator(std::vector<AcceleratorCapabilities>& out, AcceleratorCapabilities accel) {
  const auto duplicate = std::any_of(out.begin(), out.end(), [&](const AcceleratorCapabilities& existing) {
    return existing.d3d12 && accel.d3d12 &&
           existing.adapter_luid == accel.adapter_luid &&
           existing.accelerator_kind == accel.accelerator_kind;
  });
  if (!duplicate) {
    out.push_back(std::move(accel));
  }
}

void EnumerateNativeD3D12(std::vector<AcceleratorCapabilities>& out, bool include_warp) {
  UINT factory_flags = 0u;
#if defined(_DEBUG)
  factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

  Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
  const HRESULT factory_hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory4));
  if (FAILED(factory_hr) || !factory4) {
    return;
  }

  Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
  factory4.As(&factory6);

  std::uint32_t ordinal = 0u;
  if (factory6) {
    for (UINT i = 0u;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const HRESULT hr = factory6->EnumAdapterByGpuPreference(
          i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
      if (hr == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(hr)) {
        continue;
      }
      AcceleratorCapabilities accel;
      if (QueryD3D12Adapter(adapter.Get(), ordinal++, false, accel)) {
        if (!accel.warp || include_warp) {
          PushUniqueD3D12Accelerator(out, std::move(accel));
        }
      }
    }
  } else {
    for (UINT i = 0u;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      const HRESULT hr = factory4->EnumAdapters1(i, &adapter);
      if (hr == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(hr)) {
        continue;
      }
      AcceleratorCapabilities accel;
      if (QueryD3D12Adapter(adapter.Get(), ordinal++, false, accel)) {
        if (!accel.warp || include_warp) {
          PushUniqueD3D12Accelerator(out, std::move(accel));
        }
      }
    }
  }

  if (include_warp) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> warp_adapter;
    if (SUCCEEDED(factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter)))) {
      AcceleratorCapabilities accel;
      if (QueryD3D12Adapter(warp_adapter.Get(), ordinal, true, accel)) {
        PushUniqueD3D12Accelerator(out, std::move(accel));
      }
    }
  }
}

#endif  // defined(_WIN32) && defined(PT_ENABLE_D3D12)

void MarkDefaultAccelerator(std::vector<AcceleratorCapabilities>& accelerators) {
  auto mark_first = [&](AcceleratorKind kind) -> bool {
    for (auto& accel : accelerators) {
      if (accel.available && accel.compute && accel.accelerator_kind == kind) {
        accel.selected_by_default = true;
        return true;
      }
    }
    return false;
  };
  if (mark_first(AcceleratorKind::DiscreteGpu)) {
    return;
  }
  if (mark_first(AcceleratorKind::IntegratedGpu)) {
    return;
  }
  if (mark_first(AcceleratorKind::Cpu)) {
    return;
  }
  mark_first(AcceleratorKind::Warp);
}

}  // namespace

bool D3D12ShaderCompiler::supports_feature(std::string_view feature) const {
  return feature == "compute" || feature == "storage-buffers" || feature == "storage-textures"
      || feature == "hlsl";
}

vkpt::core::Status D3D12ShaderCompiler::compile_compute_shader(const ComputePipelineDesc& desc,
                                                                std::string& out_artifact,
                                                                std::string* diagnostics) {
  if (desc.source_path.empty()) {
    if (diagnostics) {
      *diagnostics = "missing source path";
    }
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                     "missing source path");
  }
  std::string defines;
  for (const auto& define : desc.defines) {
    if (!defines.empty()) {
      defines.push_back(',');
    }
    defines += define;
  }
  out_artifact = "d3d12-compute:" + desc.source_path + ":" + desc.entry_point + ":" + defines;
  return vkpt::core::Status::ok();
}

bool D3D12ShaderCache::query(std::string_view key, std::string& binary) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  binary = it->second;
  return true;
}

bool D3D12ShaderCache::store(std::string_view key, const std::string& binary) {
  m_entries[std::string(key)] = binary;
  return true;
}

bool D3D12ShaderCache::invalidate(std::string_view key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  m_entries.erase(it);
  return true;
}

std::string D3D12ShaderCache::explain_miss(std::string_view key) const {
  return "d3d12 shader cache miss: " + std::string(key);
}

std::vector<CachedManifest> D3D12ShaderCache::dump_manifest() const {
  std::vector<CachedManifest> out;
  out.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    ShaderManifest manifest;
    manifest.shader_family = "cached";
    manifest.entry_point = "main";
    manifest.backend = "d3d12";
    manifest.source_format = ShaderSourceFormat::Hlsl;
    manifest.source_hash = MakeShaderManifestHash(entry.first);
    manifest.variant_hash = MakeShaderManifestHash(entry.second);
    manifest.cache_key = entry.first;
    manifest.artifact_path = entry.second;
    manifest.manifest_dump_path = BuildShaderManifestDumpPath("shader_cache", manifest);
    manifest.compile_success = true;
    manifest.validation_success = true;
    out.push_back({SerializeShaderManifest(manifest),
                   "d3d12",
                   manifest.cache_key,
                   manifest.artifact_path,
                   manifest.manifest_dump_path,
                   manifest.compile_success});
  }
  return out;
}

bool D3D12CommandContext::begin_frame() {
  return true;
}

bool D3D12CommandContext::end_frame() {
  return true;
}

bool D3D12CommandContext::begin_pass(PassType type, std::string_view label) {
  (void)type;
  (void)label;
  return true;
}

bool D3D12CommandContext::end_pass() {
  return true;
}

bool D3D12CommandContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  (void)x;
  (void)y;
  (void)z;
  return true;
}

bool D3D12CommandContext::copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) {
  (void)source_buffer;
  (void)target_texture;
  return true;
}

bool D3D12CommandContext::barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) {
  (void)resource;
  (void)usage_before;
  (void)usage_after;
  return true;
}

D3D12Swapchain::D3D12Swapchain(std::uint32_t width, std::uint32_t height) : m_width(width), m_height(height) {}

bool D3D12Swapchain::present() {
  return true;
}

std::uint32_t D3D12Swapchain::width() const {
  return m_width;
}

std::uint32_t D3D12Swapchain::height() const {
  return m_height;
}

bool D3D12Swapchain::resize(std::uint32_t width, std::uint32_t height) {
  m_width = width;
  m_height = height;
  return true;
}

ResourceHandle D3D12ResourceAllocator::next_handle() {
  return m_nextHandle++;
}

bool D3D12ResourceAllocator::copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count) {
  auto it = m_resources.find(handle);
  if (it == m_resources.end()) {
    return false;
  }
  const std::size_t to_copy = std::min(byte_count, it->second.size_bytes);
  if (data) {
    std::memcpy(it->second.data.data(), data, to_copy);
  } else {
    std::fill(it->second.data.begin(), it->second.data.begin() + static_cast<std::ptrdiff_t>(to_copy), 0u);
  }
  it->second.version++;
  return true;
}

ResourceHandle D3D12ResourceAllocator::create_buffer(const BufferDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = false;
  record.size_bytes = static_cast<std::size_t>(desc.size_bytes);
  record.data.assign(record.size_bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool D3D12ResourceAllocator::destroy_buffer(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

ResourceHandle D3D12ResourceAllocator::create_texture(const TextureDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = true;
  const auto width = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.width));
  const auto height = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.height));
  const auto layers = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.array_layers));
  constexpr std::size_t kBytesPerTexel = 4u;
  if (height > std::numeric_limits<std::size_t>::max() / width) {
    return kInvalidHandle;
  }
  const auto pixels = width * height;
  if (layers > std::numeric_limits<std::size_t>::max() / pixels) {
    return kInvalidHandle;
  }
  const auto texels = pixels * layers;
  if (texels > std::numeric_limits<std::size_t>::max() / kBytesPerTexel) {
    return kInvalidHandle;
  }
  record.size_bytes = texels * kBytesPerTexel;
  record.data.assign(record.size_bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool D3D12ResourceAllocator::destroy_texture(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

bool D3D12ResourceAllocator::upload_data(ResourceHandle target, const void* data, std::size_t byte_count) {
  return copy_or_fill_resource(target, data, byte_count);
}

bool D3D12ResourceAllocator::readback(ResourceHandle source, void* out_data, std::size_t out_size) const {
  const auto it = m_resources.find(source);
  if (it == m_resources.end()) {
    return false;
  }
  if (!out_data) {
    return false;
  }
  const auto to_copy = std::min(out_size, it->second.size_bytes);
  std::memcpy(out_data, it->second.data.data(), to_copy);
  return true;
}

D3D12Device::D3D12Device(std::unique_ptr<D3D12ResourceAllocator> allocator, std::unique_ptr<D3D12Swapchain> swapchain)
    : m_allocator(std::move(allocator)), m_swapchain(std::move(swapchain)) {}

bool D3D12Device::begin() {
  m_running = true;
  return true;
}

bool D3D12Device::end() {
  m_running = false;
  return true;
}

std::unique_ptr<IRenderCommandContext> D3D12Device::create_command_context() {
  if (!m_running) {
    return nullptr;
  }
  return std::make_unique<D3D12CommandContext>();
}

IRenderSwapchain* D3D12Device::swapchain() const {
  return m_swapchain.get();
}

IRenderResourceAllocator* D3D12Device::allocator() {
  return m_allocator.get();
}

vkpt::core::Status D3D12Backend::initialize() {
  if (m_initialized) {
    return vkpt::core::Status::ok();
  }
  m_compiler = std::make_unique<D3D12ShaderCompiler>();
  m_cache = std::make_unique<D3D12ShaderCache>();
  m_initialized = true;
  return vkpt::core::Status::ok();
}

vkpt::core::Status D3D12Backend::shutdown() {
  m_initialized = false;
  m_compiler.reset();
  m_cache.reset();
  return vkpt::core::Status::ok();
}

BackendKind D3D12Backend::kind() const {
  return BackendKind::D3d12;
}

std::string D3D12Backend::name() const {
  return "d3d12-compute (simulated)";
}

RenderBackendCapabilities D3D12Backend::capabilities() const {
  RenderBackendCapabilities caps;
  caps.backend_name = name();
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = false;
  caps.timestamp_fallback_reason =
      "simulated D3D12 adapter does not create a timestamp query heap; use CPU frame timing";
  caps.subgroups = false;
  caps.descriptor_indexing = false;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.ray_query = false;
  caps.ray_query_supported = false;
  caps.acceleration_structure_supported = false;
  caps.shader_group_handle_size = 0u;
  caps.max_as_size = 0u;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = true;
  caps.supports_present = false;
  caps.supports_multiqueue = false;
  caps.max_workgroup_size_x = 1024u;
  caps.max_workgroup_size_y = 1024u;
  caps.max_workgroup_size_z = 64u;
  caps.max_buffer_alignment = 256u;
  caps.memory_model = "simulated-hlsl-sm6.6";
  caps.notes =
      "No external Direct3D 12 SDK required in this gate; simulated backend path. "
      "Native accelerator probing is exposed through EnumerateD3D12Accelerators.";
  caps.platform.platform_name = "headless-d3d12-sim";
  caps.platform.headless = true;
  caps.platform.notes = "No ID3D12Device, IDXGISwapChain, or native handles are exposed.";
  caps.ray_tracing_caps.tier = "not-probed";
  caps.ray_tracing_caps.unsupported_reason =
      "DXR tier is not probed by the simulated adapter because no native D3D12 device is created";
  caps.shader.hlsl = true;
  caps.shader.supported_source_formats = {"hlsl"};
  caps.shader.shader_model = "sm6-contract";
  caps.shader.notes = "HLSL compile contract only; DXIL generation is not linked in this skeleton.";
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.bgra8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.rgba32_float = true;
  caps.texture_formats_caps.storage_texture_formats = true;
  caps.texture_formats_caps.sampled_texture_formats = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "BGRA8", "RGBA16F", "RGBA32F"};
  caps.memory_budget.upload_alignment_bytes = 256u;
  caps.memory_budget.readback_alignment_bytes = 256u;
  caps.memory_budget.max_buffer_size_bytes = 1024ull * 1024ull * 1024ull;
  caps.memory_budget.budget_unavailable_reason =
      "IDXGIAdapter3 memory budget is not available because the simulated adapter does not create a DXGI adapter";
  return caps;
}

std::unique_ptr<IRenderDevice> D3D12Backend::create_device() {
  if (!m_initialized) {
    return {};
  }
  auto allocator = std::make_unique<D3D12ResourceAllocator>();
  auto swapchain = std::make_unique<D3D12Swapchain>(0u, 0u);
  return std::make_unique<D3D12Device>(std::move(allocator), std::move(swapchain));
}

IShaderCompiler* D3D12Backend::compiler() {
  return m_compiler.get();
}

IShaderCache* D3D12Backend::shader_cache() {
  return m_cache.get();
}

std::unique_ptr<IFrameGraph> D3D12Backend::create_frame_graph() {
  return std::make_unique<FrameGraph>();
}

std::vector<AcceleratorCapabilities> EnumerateD3D12Accelerators(bool include_cpu, bool include_warp) {
  std::vector<AcceleratorCapabilities> accelerators;

#if defined(_WIN32) && defined(PT_ENABLE_D3D12)
  // Native DXGI/D3D12 probing is optional; non-Windows or disabled builds still
  // report CPU planning candidates through the same contract.
  EnumerateNativeD3D12(accelerators, include_warp);
#else
  (void)include_warp;
#endif

  if (include_cpu) {
    accelerators.push_back(MakeCpuAccelerator());
  }

#if !(defined(_WIN32) && defined(PT_ENABLE_D3D12))
  for (auto& accel : accelerators) {
    if (accel.cpu) {
      accel.notes += " D3D12 adapter probing is unavailable in this build.";
    }
  }
#endif

  MarkDefaultAccelerator(accelerators);
  return accelerators;
}

}  // namespace vkpt::render
