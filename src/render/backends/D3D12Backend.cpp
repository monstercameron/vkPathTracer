#include "render/backends/D3D12Backend.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <random>
#include <cmath>

namespace vkpt::render {

bool D3D12ShaderCompiler::supports_feature(std::string_view feature) const {
  return feature == "compute" || feature == "storage-buffers" || feature == "storage-textures"
      || feature == "hlsl";
}

bool D3D12ShaderCompiler::compile_compute_shader(const ComputePipelineDesc& desc,
                                                  std::string& out_artifact,
                                                  std::string* diagnostics) {
  if (desc.source_path.empty()) {
    if (diagnostics) {
      *diagnostics = "missing source path";
    }
    return false;
  }
  std::string defines;
  for (const auto& define : desc.defines) {
    if (!defines.empty()) {
      defines.push_back(',');
    }
    defines += define;
  }
  out_artifact = "d3d12-compute:" + desc.source_path + ":" + desc.entry_point + ":" + defines;
  return true;
}

bool D3D12ShaderCache::query(std::string_view key, std::string& binary) {
  const auto it = m_entries.find(std::string(key));
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
  const auto it = m_entries.find(std::string(key));
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
  const auto bytes = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.width)) *
                     static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.height)) *
                     static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.array_layers));
  record.size_bytes = bytes * 4u;
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

bool D3D12Backend::initialize() {
  if (m_initialized) {
    return true;
  }
  m_compiler = std::make_unique<D3D12ShaderCompiler>();
  m_cache = std::make_unique<D3D12ShaderCache>();
  m_initialized = true;
  return true;
}

bool D3D12Backend::shutdown() {
  m_initialized = false;
  m_compiler.reset();
  m_cache.reset();
  return true;
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
  caps.notes = "No external Direct3D 12 SDK required in this gate; simulated backend path.";
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

bool RunD3D12ComputeSmoke(vkpt::render::IRenderBackend& backend) {
  auto device = backend.create_device();
  if (!device || !device->begin()) {
    return false;
  }
  auto* renderDevice = dynamic_cast<D3D12Device*>(device.get());
  if (!renderDevice) {
    device->end();
    return false;
  }
  auto* allocator = static_cast<D3D12ResourceAllocator*>(renderDevice->allocator());
  if (!allocator) {
    device->end();
    return false;
  }

  BufferDesc patternDesc;
  patternDesc.debug_label = "smoke_compute_pattern";
  patternDesc.size_bytes = 64u;
  patternDesc.usage = ResourceBindingUsage::Storage | ResourceBindingUsage::Read | ResourceBindingUsage::Write;
  const auto sourceBuffer = allocator->create_buffer(patternDesc);
  if (sourceBuffer == kInvalidHandle) {
    device->end();
    return false;
  }

  std::array<std::uint32_t, 16> pattern{};
  std::mt19937 rng(456u);
  for (auto& v : pattern) {
    v = rng();
  }
  if (!allocator->upload_data(sourceBuffer, pattern.data(), pattern.size() * sizeof(std::uint32_t))) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }

  FrameGraphDesc graphDesc;
  graphDesc.debug_label = "d3d12_compute_smoke";
  graphDesc.passes = {
      {0u, "write", PassType::Copy, {}, {sourceBuffer}},
      {1u, "compute", PassType::Compute, {sourceBuffer}, {sourceBuffer}},
      {2u, "readback", PassType::Readback, {sourceBuffer}, {}},
  };
  graphDesc.dependencies = {{0u, 1u}, {1u, 2u}};
  auto frameGraph = backend.create_frame_graph();
  std::vector<std::string> buildDiagnostics;
  if (!frameGraph->build(graphDesc, &buildDiagnostics)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  FrameContext frame;
  frame.frame_index = 0u;
  frame.debug_label = graphDesc.debug_label;
  auto context = device->create_command_context();
  if (!context || !frameGraph->validate(nullptr)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  if (!frameGraph->execute(*context, frame)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  allocator->destroy_buffer(sourceBuffer);
  device->end();
  return true;
}

}  // namespace vkpt::render
