#include "render/backends/NullBackend.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace vkpt::render {

bool NullShaderCompiler::supports_feature(std::string_view feature) const {
  return feature == "compute" || feature == "storage-buffers" || feature == "storage-textures";
}

vkpt::core::Status NullShaderCompiler::compile_compute_shader(const ComputePipelineDesc& desc,
                                                              std::string& out_artifact,
                                                              std::string* diagnostics) {
  if (desc.entry_point.empty()) {
    if (diagnostics) {
      *diagnostics = "missing entry point";
    }
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                     "missing entry point");
  }
  std::string defines;
  for (const auto& define : desc.defines) {
    if (!defines.empty()) {
      defines.push_back(',');
    }
    defines += define;
  }
  out_artifact = "null-compute:" + desc.debug_label + ":" + desc.entry_point + ":" + defines;
  return vkpt::core::Status::ok();
}

bool NullShaderCache::query(std::string_view key, std::string& binary) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  binary = it->second;
  return true;
}

bool NullShaderCache::store(std::string_view key, const std::string& binary) {
  m_entries[std::string(key)] = binary;
  return true;
}

bool NullShaderCache::invalidate(std::string_view key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  m_entries.erase(it);
  return true;
}

std::string NullShaderCache::explain_miss(std::string_view key) const {
  return "null shader cache miss: " + std::string(key);
}

std::vector<CachedManifest> NullShaderCache::dump_manifest() const {
  std::vector<CachedManifest> out;
  out.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    ShaderManifest manifest;
    manifest.shader_family = "cached";
    manifest.entry_point = "main";
    manifest.backend = "null";
    manifest.source_format = ShaderSourceFormat::Unknown;
    manifest.source_hash = MakeShaderManifestHash(entry.first);
    manifest.variant_hash = MakeShaderManifestHash(entry.second);
    manifest.cache_key = entry.first;
    manifest.artifact_path = entry.second;
    manifest.manifest_dump_path = BuildShaderManifestDumpPath("shader_cache", manifest);
    manifest.compile_success = true;
    manifest.validation_success = true;
    out.push_back({SerializeShaderManifest(manifest),
                   "null",
                   manifest.cache_key,
                   manifest.artifact_path,
                   manifest.manifest_dump_path,
                   manifest.compile_success});
  }
  return out;
}

bool NullCommandContext::begin_pass(PassType type, std::string_view label) {
  (void)type;
  (void)label;
  return true;
}

bool NullCommandContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  (void)x;
  (void)y;
  (void)z;
  return true;
}

bool NullCommandContext::copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) {
  (void)source_buffer;
  (void)target_texture;
  return true;
}

bool NullCommandContext::barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) {
  (void)resource;
  (void)usage_before;
  (void)usage_after;
  return true;
}

NullSwapchain::NullSwapchain(std::uint32_t width, std::uint32_t height) : m_width(width), m_height(height) {}

std::uint32_t NullSwapchain::width() const {
  return m_width;
}

std::uint32_t NullSwapchain::height() const {
  return m_height;
}

bool NullSwapchain::resize(std::uint32_t width, std::uint32_t height) {
  m_width = width;
  m_height = height;
  return true;
}

ResourceHandle NullResourceAllocator::next_handle() {
  return m_nextHandle++;
}

bool NullResourceAllocator::copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count) {
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

ResourceHandle NullResourceAllocator::create_buffer(const BufferDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = false;
  record.size_bytes = desc.size_bytes;
  record.data.assign(static_cast<std::size_t>(desc.size_bytes), 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool NullResourceAllocator::destroy_buffer(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

ResourceHandle NullResourceAllocator::create_texture(const TextureDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = true;
  // Store simulated textures as tightly packed RGBA8 bytes; format semantics are
  // reported through capabilities rather than modeled in this allocator.
  const auto width = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.width));
  const auto height = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.height));
  constexpr std::size_t kBytesPerTexel = 4u;
  if (height > std::numeric_limits<std::size_t>::max() / width) {
    return kInvalidHandle;
  }
  const auto texels = width * height;
  if (texels > std::numeric_limits<std::size_t>::max() / kBytesPerTexel) {
    return kInvalidHandle;
  }
  const auto bytes = texels * kBytesPerTexel;
  record.size_bytes = bytes;
  record.data.assign(bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool NullResourceAllocator::destroy_texture(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

bool NullResourceAllocator::upload_data(ResourceHandle target, const void* data, std::size_t byte_count) {
  return copy_or_fill_resource(target, data, byte_count);
}

bool NullResourceAllocator::readback(ResourceHandle source, void* out_data, std::size_t out_size) const {
  const auto it = m_resources.find(source);
  if (it == m_resources.end()) {
    return false;
  }
  if (!out_data) {
    return false;
  }
  const std::size_t to_copy = std::min(out_size, it->second.size_bytes);
  std::memcpy(out_data, it->second.data.data(), to_copy);
  return true;
}

std::size_t NullResourceAllocator::live_resource_count() const {
  return m_resources.size();
}

NullDevice::NullDevice(std::unique_ptr<NullResourceAllocator> allocator, std::unique_ptr<NullSwapchain> swapchain)
    : m_allocator(std::move(allocator)), m_swapchain(std::move(swapchain)) {}

bool NullDevice::begin() {
  m_running = true;
  return true;
}

bool NullDevice::end() {
  m_running = false;
  return true;
}

std::unique_ptr<IRenderCommandContext> NullDevice::create_command_context() {
  if (!m_running) {
    return nullptr;
  }
  return std::make_unique<NullCommandContext>();
}

IRenderSwapchain* NullDevice::swapchain() const {
  return m_swapchain.get();
}

IRenderResourceAllocator* NullDevice::allocator() {
  return m_allocator.get();
}

vkpt::core::Status NullBackend::initialize() {
  if (m_initialized) {
    return vkpt::core::Status::ok();
  }
  m_compiler = std::make_unique<NullShaderCompiler>();
  m_cache = std::make_unique<NullShaderCache>();
  m_initialized = true;
  return vkpt::core::Status::ok();
}

vkpt::core::Status NullBackend::shutdown() {
  m_initialized = false;
  m_compiler.reset();
  m_cache.reset();
  return vkpt::core::Status::ok();
}

BackendKind NullBackend::kind() const {
  return BackendKind::Null;
}

std::string NullBackend::name() const {
  return "null";
}

RenderBackendCapabilities NullBackend::capabilities() const {
  RenderBackendCapabilities caps;
  // The null backend advertises only contracts that can be honored in CPU memory.
  caps.backend_name = name();
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = false;
  caps.timestamp_fallback_reason = "null backend has no GPU timestamp queue; use CPU frame timing";
  caps.subgroups = false;
  caps.descriptor_indexing = false;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.ray_query = false;
  caps.ray_query_supported = false;
  caps.acceleration_structure_supported = false;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = true;
  caps.supports_present = false;
  caps.supports_multiqueue = false;
  caps.max_workgroup_size_x = 64u;
  caps.max_workgroup_size_y = 1u;
  caps.max_workgroup_size_z = 1u;
  caps.max_buffer_alignment = 256u;
  caps.memory_model = "null";
  caps.notes = "Simulated backend for lifecycle and diagnostics.";
  caps.platform.platform_name = "headless";
  caps.platform.headless = true;
  caps.platform.notes = "No native renderer surface is created.";
  caps.ray_tracing_caps.unsupported_reason = "null backend has no hardware ray tracing device";
  caps.shader.supported_source_formats = {"none"};
  caps.shader.notes = "Accepts synthetic compute descriptors for lifecycle tests.";
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.storage_texture_formats = true;
  caps.texture_formats_caps.sampled_texture_formats = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "RGBA16F"};
  caps.texture_formats_caps.notes = "Formats are simulated in CPU memory.";
  caps.memory_budget.upload_alignment_bytes = 256u;
  caps.memory_budget.readback_alignment_bytes = 256u;
  caps.memory_budget.max_buffer_size_bytes = 256ull * 1024ull * 1024ull;
  caps.memory_budget.budget_unavailable_reason = "null backend does not own GPU memory";
  return caps;
}

std::unique_ptr<IRenderDevice> NullBackend::create_device() {
  if (!m_initialized) {
    return {};
  }
  auto allocator = std::make_unique<NullResourceAllocator>();
  auto swapchain = std::make_unique<NullSwapchain>();
  return std::make_unique<NullDevice>(std::move(allocator), std::move(swapchain));
}

IShaderCompiler* NullBackend::compiler() {
  return m_compiler.get();
}

IShaderCache* NullBackend::shader_cache() {
  return m_cache.get();
}

std::unique_ptr<IFrameGraph> NullBackend::create_frame_graph() {
  return std::make_unique<FrameGraph>();
}

}  // namespace vkpt::render

