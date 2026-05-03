#include "render/backends/VulkanBackend.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <random>

namespace vkpt::render {

bool VulkanShaderCompiler::supports_feature(std::string_view feature) const {
  return feature == "compute" || feature == "storage-buffers" || feature == "storage-textures" || feature == "subgroups";
}

bool VulkanShaderCompiler::compile_compute_shader(const ComputePipelineDesc& desc,
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
  out_artifact = "vulkan-compute:" + desc.source_path + ":" + desc.entry_point + ":" + defines;
  return true;
}

bool VulkanShaderCache::query(std::string_view key, std::string& binary) {
  const auto it = m_entries.find(std::string(key));
  if (it == m_entries.end()) {
    return false;
  }
  binary = it->second;
  return true;
}

bool VulkanShaderCache::store(std::string_view key, const std::string& binary) {
  m_entries[std::string(key)] = binary;
  return true;
}

bool VulkanShaderCache::invalidate(std::string_view key) {
  const auto it = m_entries.find(std::string(key));
  if (it == m_entries.end()) {
    return false;
  }
  m_entries.erase(it);
  return true;
}

std::string VulkanShaderCache::explain_miss(std::string_view key) const {
  return "vulkan shader cache miss: " + std::string(key);
}

std::vector<CachedManifest> VulkanShaderCache::dump_manifest() const {
  std::vector<CachedManifest> out;
  out.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    out.push_back({entry.second, "vulkan"});
  }
  return out;
}

bool VulkanCommandContext::begin_frame() {
  return true;
}

bool VulkanCommandContext::end_frame() {
  return true;
}

bool VulkanCommandContext::begin_pass(PassType type, std::string_view label) {
  (void)type;
  (void)label;
  return true;
}

bool VulkanCommandContext::end_pass() {
  return true;
}

bool VulkanCommandContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  (void)x;
  (void)y;
  (void)z;
  return true;
}

bool VulkanCommandContext::copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) {
  (void)source_buffer;
  (void)target_texture;
  return true;
}

bool VulkanCommandContext::barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) {
  (void)resource;
  (void)usage_before;
  (void)usage_after;
  return true;
}

VulkanSwapchain::VulkanSwapchain(std::uint32_t width, std::uint32_t height) : m_width(width), m_height(height) {}

bool VulkanSwapchain::present() {
  return true;
}

std::uint32_t VulkanSwapchain::width() const {
  return m_width;
}

std::uint32_t VulkanSwapchain::height() const {
  return m_height;
}

bool VulkanSwapchain::resize(std::uint32_t width, std::uint32_t height) {
  m_width = width;
  m_height = height;
  return true;
}

ResourceHandle VulkanResourceAllocator::next_handle() {
  return m_nextHandle++;
}

bool VulkanResourceAllocator::copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count) {
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

ResourceHandle VulkanResourceAllocator::create_buffer(const BufferDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = false;
  record.size_bytes = static_cast<std::size_t>(desc.size_bytes);
  record.data.assign(record.size_bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool VulkanResourceAllocator::destroy_buffer(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

ResourceHandle VulkanResourceAllocator::create_texture(const TextureDesc& desc) {
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

bool VulkanResourceAllocator::destroy_texture(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

bool VulkanResourceAllocator::upload_data(ResourceHandle target, const void* data, std::size_t byte_count) {
  return copy_or_fill_resource(target, data, byte_count);
}

bool VulkanResourceAllocator::readback(ResourceHandle source, void* out_data, std::size_t out_size) const {
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

VulkanDevice::VulkanDevice(std::unique_ptr<VulkanResourceAllocator> allocator, std::unique_ptr<VulkanSwapchain> swapchain)
    : m_allocator(std::move(allocator)), m_swapchain(std::move(swapchain)) {}

bool VulkanDevice::begin() {
  m_running = true;
  return true;
}

bool VulkanDevice::end() {
  m_running = false;
  return true;
}

std::unique_ptr<IRenderCommandContext> VulkanDevice::create_command_context() {
  if (!m_running) {
    return nullptr;
  }
  return std::make_unique<VulkanCommandContext>();
}

IRenderSwapchain* VulkanDevice::swapchain() const {
  return m_swapchain.get();
}

IRenderResourceAllocator* VulkanDevice::allocator() {
  return m_allocator.get();
}

bool VulkanComputeBackend::initialize() {
  if (m_initialized) {
    return true;
  }
  m_compiler = std::make_unique<VulkanShaderCompiler>();
  m_cache = std::make_unique<VulkanShaderCache>();
  m_initialized = true;
  return true;
}

bool VulkanComputeBackend::shutdown() {
  m_initialized = false;
  m_compiler.reset();
  m_cache.reset();
  return true;
}

BackendKind VulkanComputeBackend::kind() const {
  return m_simulated ? BackendKind::VulkanCompute : BackendKind::VulkanRt;
}

std::string VulkanComputeBackend::name() const {
  return m_simulated ? "vulkan-compute (simulated)" : "vulkan";
}

RenderBackendCapabilities VulkanComputeBackend::capabilities() const {
  RenderBackendCapabilities caps;
  caps.backend_name = name();
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = true;
  caps.subgroups = true;
  caps.descriptor_indexing = true;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.presentation = true;
  caps.readback = true;
  caps.is_simulated = true;
  caps.supports_present = true;
  caps.supports_multiqueue = false;
  caps.max_workgroup_size_x = 256u;
  caps.max_workgroup_size_y = 256u;
  caps.max_workgroup_size_z = 64u;
  caps.max_buffer_alignment = 256u;
  caps.memory_model = "simulated-glsl";
  caps.notes = "No external Vulkan SDK required in this gate; simulated backend path.";
  return caps;
}

std::unique_ptr<IRenderDevice> VulkanComputeBackend::create_device() {
  if (!m_initialized) {
    return {};
  }
  auto allocator = std::make_unique<VulkanResourceAllocator>();
  auto swapchain = std::make_unique<VulkanSwapchain>(0u, 0u);
  return std::make_unique<VulkanDevice>(std::move(allocator), std::move(swapchain));
}

IShaderCompiler* VulkanComputeBackend::compiler() {
  return m_compiler.get();
}

IShaderCache* VulkanComputeBackend::shader_cache() {
  return m_cache.get();
}

std::unique_ptr<IFrameGraph> VulkanComputeBackend::create_frame_graph() {
  return std::make_unique<FrameGraph>();
}

bool RunVulkanComputeSmoke(vkpt::render::IRenderBackend& backend) {
  auto device = backend.create_device();
  if (!device || !device->begin()) {
    return false;
  }
  auto* renderDevice = dynamic_cast<VulkanDevice*>(device.get());
  if (!renderDevice) {
    device->end();
    return false;
  }
  auto* allocator = static_cast<VulkanResourceAllocator*>(renderDevice->allocator());
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
  std::mt19937 rng(123u);
  for (auto& v : pattern) {
    v = rng();
  }
  if (!allocator->upload_data(sourceBuffer, pattern.data(), pattern.size() * sizeof(std::uint32_t))) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }

  auto frameGraph = backend.create_frame_graph();
  const auto writePass = frameGraph->add_pass("write", PassType::Copy, {}, {sourceBuffer});
  const auto computePass = frameGraph->add_pass("compute", PassType::Compute, {sourceBuffer}, {sourceBuffer});
  const auto readbackPass = frameGraph->add_pass("readback", PassType::Readback, {sourceBuffer}, {});
  frameGraph->add_dependency(writePass, computePass);
  frameGraph->add_dependency(computePass, readbackPass);
  auto context = device->create_command_context();
  if (!context || !frameGraph->validate(nullptr)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  if (!frameGraph->execute(*context)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  allocator->destroy_buffer(sourceBuffer);
  device->end();
  return true;
}

}  // namespace vkpt::render
