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

class D3D12ShaderCompiler final : public IShaderCompiler {
 public:
  bool supports_feature(std::string_view feature) const override;
  bool compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) override;
};

class D3D12ShaderCache final : public IShaderCache {
 public:
  bool query(std::string_view key, std::string& binary) override;
  bool store(std::string_view key, const std::string& binary) override;
  bool invalidate(std::string_view key) override;
  std::string explain_miss(std::string_view key) const override;
  std::vector<CachedManifest> dump_manifest() const override;

 private:
  std::unordered_map<std::string, std::string> m_entries;
};

class D3D12ResourceAllocator final : public IRenderResourceAllocator {
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

class D3D12CommandContext final : public IRenderCommandContext {
 public:
  bool begin_frame() override;
  bool end_frame() override;
  bool begin_pass(PassType type, std::string_view label) override;
  bool end_pass() override;
  bool dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  bool copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) override;
  bool barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) override;
};

class D3D12Swapchain final : public IRenderSwapchain {
 public:
  explicit D3D12Swapchain(std::uint32_t width = 0u, std::uint32_t height = 0u);
  bool present() override;
  std::uint32_t width() const override;
  std::uint32_t height() const override;
  bool resize(std::uint32_t width, std::uint32_t height) override;

 private:
  std::uint32_t m_width = 0u;
  std::uint32_t m_height = 0u;
};

class D3D12Device final : public IRenderDevice {
 public:
  explicit D3D12Device(std::unique_ptr<D3D12ResourceAllocator> allocator, std::unique_ptr<D3D12Swapchain> swapchain);
  bool begin() override;
  bool end() override;
  std::unique_ptr<IRenderCommandContext> create_command_context() override;
  IRenderSwapchain* swapchain() const override;
  IRenderResourceAllocator* allocator();

 private:
  std::unique_ptr<D3D12ResourceAllocator> m_allocator;
  std::unique_ptr<D3D12Swapchain> m_swapchain;
  bool m_running = false;
};

class D3D12Backend final : public IRenderBackend {
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
  std::unique_ptr<D3D12ShaderCompiler> m_compiler;
  std::unique_ptr<D3D12ShaderCache> m_cache;
};

std::vector<AcceleratorCapabilities> EnumerateD3D12Accelerators(bool include_cpu = true,
                                                                bool include_warp = false);
RayBudgetPlan BuildD3D12RayBudgetPlan(const RayBudgetRequest& request);
bool RunD3D12ComputeSmoke(vkpt::render::IRenderBackend& backend);

}  // namespace vkpt::render
