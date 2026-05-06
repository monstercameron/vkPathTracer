#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/backends/FrameGraph.h"
#include "render/interface/RenderContracts.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::render {

/// Vulkan compute compiler facade; currently validates GLSL compute metadata.
class VulkanShaderCompiler final : public IShaderCompiler {
 public:
  bool supports_feature(std::string_view feature) const override;
  bool compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) override;
};

/// In-memory cache for Vulkan skeleton shader artifacts and manifests.
class VulkanShaderCache final : public IShaderCache {
 public:
  bool query(std::string_view key, std::string& binary) override;
  bool store(std::string_view key, const std::string& binary) override;
  bool invalidate(std::string_view key) override;
  std::string explain_miss(std::string_view key) const override;
  std::vector<CachedManifest> dump_manifest() const override;

 private:
  std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_entries;
};

/// Simulated Vulkan resource allocator backed by CPU memory.
class VulkanResourceAllocator final : public IRenderResourceAllocator {
 public:
  ResourceHandle create_buffer(const BufferDesc& desc) override;
  bool destroy_buffer(ResourceHandle handle) override;
  ResourceHandle create_texture(const TextureDesc& desc) override;
  bool destroy_texture(ResourceHandle handle) override;
  bool upload_data(ResourceHandle target, const void* data, std::size_t byte_count) override;
  bool readback(ResourceHandle source, void* out_data, std::size_t out_size) const override;

 private:
  struct ResourceRecord {
    std::string label;
    std::vector<std::uint8_t> data;
    bool is_texture = false;
    std::uint64_t version = 0u;
    std::size_t size_bytes = 0u;
  };

  ResourceHandle next_handle();
  bool copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count);

  ResourceHandle m_nextHandle = 1u;
  std::unordered_map<ResourceHandle, ResourceRecord> m_resources;
};

/// Command context matching the Vulkan backend contract without native Vk objects.
class VulkanCommandContext final : public IRenderCommandContext {
 public:
  bool begin_frame() override;
  bool end_frame() override;
  bool begin_pass(PassType type, std::string_view label) override;
  bool end_pass() override;
  bool dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  bool copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) override;
  bool barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) override;
};

/// Headless Vulkan swapchain placeholder.
class VulkanSwapchain final : public IRenderSwapchain {
 public:
  explicit VulkanSwapchain(std::uint32_t width = 0u, std::uint32_t height = 0u);
  bool present() override;
  std::uint32_t width() const override;
  std::uint32_t height() const override;
  bool resize(std::uint32_t width, std::uint32_t height) override;

 private:
  std::uint32_t m_width = 0u;
  std::uint32_t m_height = 0u;
};

/// Simulated Vulkan device; command contexts are only created while begun.
class VulkanDevice final : public IRenderDevice {
 public:
  explicit VulkanDevice(std::unique_ptr<VulkanResourceAllocator> allocator, std::unique_ptr<VulkanSwapchain> swapchain);
  bool begin() override;
  bool end() override;
  std::unique_ptr<IRenderCommandContext> create_command_context() override;
  IRenderSwapchain* swapchain() const override;
  IRenderResourceAllocator* allocator();

 private:
  std::unique_ptr<VulkanResourceAllocator> m_allocator;
  std::unique_ptr<VulkanSwapchain> m_swapchain;
  bool m_running = false;
};

/// Vulkan compute backend skeleton used before native VkDevice integration.
class VulkanComputeBackend final : public IRenderBackend {
 public:
  bool initialize() override;
  bool shutdown() override;
  BackendKind kind() const override;
  std::string name() const override;
  RenderBackendCapabilities capabilities() const override;
  std::unique_ptr<IRenderDevice> create_device() override;
  IShaderCompiler* compiler() override;
  IShaderCache* shader_cache() override;
  std::unique_ptr<IFrameGraph> create_frame_graph() override;

 private:
  bool m_initialized = false;
  bool m_simulated = true;
  std::unique_ptr<VulkanShaderCompiler> m_compiler;
  std::unique_ptr<VulkanShaderCache> m_cache;
};

/// Execute a minimal graph/allocator smoke test against a Vulkan-compatible backend.
bool RunVulkanComputeSmoke(vkpt::render::IRenderBackend& backend);

/// Simulated Vulkan BVH/pathtrace pass result.
struct VulkanBVHPassResult {
  bool success = false;
  uint32_t vertex_buffer_count = 0;   // vertices uploaded
  uint32_t index_buffer_count  = 0;   // indices uploaded
  uint32_t instance_count      = 0;   // BLAS instances
  uint32_t bvh_node_estimate   = 0;   // SAH-estimated node count
  std::string error;
};

/// Upload RTSceneData, simulate BVH build, and execute a pathtrace graph.
///
/// No Vulkan SDK objects are required in this stub; all operations are performed
/// through the simulated allocator and frame-graph contracts.
VulkanBVHPassResult RunVulkanBVHPass(
    vkpt::render::VulkanComputeBackend& backend,
    const vkpt::pathtracer::RTSceneData& scene,
    uint32_t width,
    uint32_t height);

}  // namespace vkpt::render

