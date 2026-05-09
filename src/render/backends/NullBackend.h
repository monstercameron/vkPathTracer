#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "render/backends/FrameGraph.h"
#include "render/interface/RenderContracts.h"

namespace vkpt::render {

/// Shader compiler stub that validates synthetic compute descriptors.
class NullShaderCompiler final : public IShaderCompiler {
 public:
  bool supports_feature(std::string_view feature) const override;
  vkpt::core::Status compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) override;
};

/// In-memory shader cache used by the null backend for lifecycle tests.
class NullShaderCache final : public IShaderCache {
 public:
  bool query(std::string_view key, std::string& binary) override;
  bool store(std::string_view key, const std::string& binary) override;
  bool invalidate(std::string_view key) override;
  std::string explain_miss(std::string_view key) const override;
  std::vector<CachedManifest> dump_manifest() const override;

 private:
  std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_entries;
};

/// Command context that accepts commands without touching GPU state.
class NullCommandContext final : public IRenderCommandContext {
 public:
  bool begin_frame() override { return true; }
  bool end_frame() override { return true; }
  bool begin_pass(PassType type, std::string_view label) override;
  bool end_pass() override { return true; }
  bool dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  bool copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) override;
  bool barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) override;
};

/// Headless swapchain placeholder for code paths that expect presentation hooks.
class NullSwapchain final : public IRenderSwapchain {
 public:
  explicit NullSwapchain(std::uint32_t width = 0u, std::uint32_t height = 0u);
  bool present() override { return true; }
  std::uint32_t width() const override;
  std::uint32_t height() const override;
  bool resize(std::uint32_t width, std::uint32_t height) override;

 private:
  std::uint32_t m_width = 0u;
  std::uint32_t m_height = 0u;
};

/// CPU-memory resource allocator used by the null backend.
class NullResourceAllocator final : public IRenderResourceAllocator {
 public:
  ResourceHandle create_buffer(const BufferDesc& desc) override;
  bool destroy_buffer(ResourceHandle handle) override;
  ResourceHandle create_texture(const TextureDesc& desc) override;
  bool destroy_texture(ResourceHandle handle) override;
  bool upload_data(ResourceHandle target, const void* data, std::size_t byte_count) override;
  bool readback(ResourceHandle source, void* out_data, std::size_t out_size) const override;

  std::size_t live_resource_count() const;

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

/// Null render device with explicit begin/end lifetime gating command contexts.
class NullDevice final : public IRenderDevice {
 public:
  explicit NullDevice(std::unique_ptr<NullResourceAllocator> allocator,
                      std::unique_ptr<NullSwapchain> swapchain);
  bool begin() override;
  bool end() override;
  std::unique_ptr<IRenderCommandContext> create_command_context() override;
  IRenderSwapchain* swapchain() const override;
  IRenderResourceAllocator* allocator();

 private:
  std::unique_ptr<NullResourceAllocator> m_allocator;
  std::unique_ptr<NullSwapchain> m_swapchain;
  bool m_running = false;
};

/// Simulated backend for tests, diagnostics, and fallback selection.
class NullBackend final : public IRenderBackend {
 public:
  vkpt::core::Status initialize() override;
  vkpt::core::Status shutdown() override;
  BackendKind kind() const override;
  std::string name() const override;
  RenderBackendCapabilities capabilities() const override;
  std::unique_ptr<IRenderDevice> create_device() override;
  IShaderCompiler* compiler() override;
  IShaderCache* shader_cache() override;
  std::unique_ptr<IFrameGraph> create_frame_graph() override;

 private:
  bool m_initialized = false;
  NullResourceAllocator m_allocator;
  std::unique_ptr<NullShaderCompiler> m_compiler;
  std::unique_ptr<NullShaderCache> m_cache;
};

}  // namespace vkpt::render

