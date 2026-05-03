#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <variant>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "render/interface/ResourceRegistry.h"

namespace vkpt::render {

enum class BackendKind {
  Null,
  VulkanCompute,
  VulkanRt,
  D3d12,
  Metal,
  WebGpu,
  OpenGLExperimental,
  Unknown
};

enum class PassType {
  Upload,
  Compute,
  Raster,
  Copy,
  Resolve,
  Readback,
  Present,
  Debug
};

struct ShaderManifest {
  std::string shader_family;
  std::string entry_point;
  std::string backend;
  std::string source_hash;
  std::string variant_hash;
  std::vector<std::string> defines;
  std::vector<std::string> feature_flags;
  std::vector<std::string> compiler_flags;
  uint32_t resource_layout_version = 0u;
  std::string artifact_path;
  std::string compile_diagnostics;
  bool compile_success = false;
};

enum class ResourceBindingUsage : std::uint32_t {
  None = 0u,
  Read = 1u << 0u,
  Write = 1u << 1u,
  Storage = 1u << 2u,
  Uniform = 1u << 3u
};

inline ResourceBindingUsage operator|(ResourceBindingUsage lhs, ResourceBindingUsage rhs) {
  return static_cast<ResourceBindingUsage>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline ResourceBindingUsage operator&(ResourceBindingUsage lhs, ResourceBindingUsage rhs) {
  return static_cast<ResourceBindingUsage>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

inline ResourceBindingUsage& operator|=(ResourceBindingUsage& lhs, ResourceBindingUsage rhs) {
  lhs = lhs | rhs;
  return lhs;
}

inline bool HasUsage(ResourceBindingUsage value, ResourceBindingUsage flag) {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0u;
}

std::string_view BackendKindToString(BackendKind kind);

using ResourceHandle = vkpt::core::RuntimeHandle;

constexpr ResourceHandle kInvalidHandle = 0u;

struct BufferDesc {
  std::string debug_label;
  std::uint64_t size_bytes = 0u;
  std::uint32_t stride_bytes = 0u;
  bool dynamic = false;
  ResourceBindingUsage usage = ResourceBindingUsage::None;
  bool cpu_visible = false;
};

struct TextureDesc {
  std::string debug_label;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t depth = 1u;
  std::uint32_t mip_levels = 1u;
  std::uint32_t array_layers = 1u;
  std::string format = "RGBA16F";
  ResourceBindingUsage usage = ResourceBindingUsage::None;
  bool cube = false;
};

struct SamplerDesc {
  std::string debug_label;
  std::string mag_filter = "Nearest";
  std::string min_filter = "Nearest";
  bool anisotropy_enable = false;
  float max_anisotropy = 1.0f;
};

struct PipelineDesc {
  std::string debug_label;
  std::string source_path;
  std::vector<std::string> defines;
  std::string entry_point = "main";
  std::string backend_variant;
};

struct ComputePipelineDesc : public PipelineDesc {
  std::uint32_t subgroup_size = 32u;
  std::uint32_t workgroup_x = 8u;
  std::uint32_t workgroup_y = 8u;
  std::uint32_t workgroup_z = 1u;
};

struct RayTracingPipelineDesc : public PipelineDesc {
  std::string hit_group;
  std::uint32_t max_recursion_depth = 0u;
};

struct DescriptorLayoutDesc {
  std::vector<std::string> bindings;
  std::string debug_label;
};

struct ReadbackDesc {
  ResourceHandle source = kInvalidHandle;
  std::size_t byte_count = 0u;
  bool wait_for_idle = true;
};

struct RenderBackendCapabilities {
  std::string backend_name = "unknown";
  bool compute = false;
  bool storage_buffers = false;
  bool storage_textures = false;
  bool timestamp_queries = false;
  std::string timestamp_fallback_reason;
  bool subgroups = false;
  bool descriptor_indexing = false;
  bool bindless_like_resources = false;
  bool texture_formats = true;
  bool ray_tracing = false;
  bool ray_query = false;
  bool ray_query_supported = false;
  bool acceleration_structure_supported = false;
  std::uint32_t shader_group_handle_size = 0u;
  std::uint64_t max_as_size = 0u;
  bool presentation = false;
  bool readback = false;
  bool is_simulated = false;
  bool supports_present = false;
  bool supports_multiqueue = false;
  std::uint32_t max_workgroup_size_x = 0u;
  std::uint32_t max_workgroup_size_y = 0u;
  std::uint32_t max_workgroup_size_z = 0u;
  std::uint64_t max_buffer_alignment = 0u;
  std::string memory_model = "unknown";
  std::string notes;
};

struct RenderCrashState {
  std::string selected_backend;
  std::string device_name;
  RenderBackendCapabilities capabilities;
  std::vector<ResourceLeaseInfo> live_resources;
  uint64_t last_frame_index = 0u;
  std::string last_frame_stage;
  std::string last_pass_name;
  std::string last_shader_variant;
  std::string last_error;
};

struct RenderResource {
  ResourceHandle handle = kInvalidHandle;
  std::string label;
  std::uint64_t size_bytes = 0u;
  std::uint64_t version = 0u;
};

struct RenderPassCommandContext {
  std::string name;
  PassType type = PassType::Compute;
  ResourceHandle pass_id = kInvalidHandle;
};

class IRenderCommandContext {
 public:
  virtual ~IRenderCommandContext() = default;

  virtual bool begin_frame() = 0;
  virtual bool end_frame() = 0;
  virtual bool begin_pass(PassType type, std::string_view label) = 0;
  virtual bool end_pass() = 0;
  virtual bool dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
  virtual bool copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) = 0;
  virtual bool barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) = 0;
};

class IRenderResourceAllocator {
 public:
  virtual ~IRenderResourceAllocator() = default;

  virtual ResourceHandle create_buffer(const BufferDesc& desc) = 0;
  virtual bool destroy_buffer(ResourceHandle handle) = 0;
  virtual ResourceHandle create_texture(const TextureDesc& desc) = 0;
  virtual bool destroy_texture(ResourceHandle handle) = 0;
  virtual bool upload_data(ResourceHandle target, const void* data, std::size_t byte_count) = 0;
  virtual bool readback(ResourceHandle source, void* out_data, std::size_t out_size) const = 0;
};

class IRenderSwapchain {
 public:
  virtual ~IRenderSwapchain() = default;
  virtual bool present() = 0;
  virtual std::uint32_t width() const = 0;
  virtual std::uint32_t height() const = 0;
  virtual bool resize(std::uint32_t width, std::uint32_t height) = 0;
};

class IRenderDevice {
 public:
  virtual ~IRenderDevice() = default;

  virtual bool begin() = 0;
  virtual bool end() = 0;
  virtual std::unique_ptr<IRenderCommandContext> create_command_context() = 0;
  virtual IRenderSwapchain* swapchain() const = 0;
};

class IShaderCompiler {
 public:
  virtual ~IShaderCompiler() = default;
  virtual bool supports_feature(std::string_view feature) const = 0;
  virtual bool compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) = 0;
};

struct CachedManifest {
  std::string manifest_text;
  std::string backend;
};

class IShaderCache {
 public:
  virtual ~IShaderCache() = default;
  virtual bool query(std::string_view key, std::string& binary) = 0;
  virtual bool store(std::string_view key, const std::string& binary) = 0;
  virtual bool invalidate(std::string_view key) = 0;
  virtual std::string explain_miss(std::string_view key) const = 0;
  virtual std::vector<CachedManifest> dump_manifest() const = 0;
};

class IFrameGraph {
 public:
  struct Pass {
    std::uint32_t id = 0u;
    std::string name;
    PassType type = PassType::Compute;
    std::vector<ResourceHandle> reads;
    std::vector<ResourceHandle> writes;
  };

  virtual ~IFrameGraph() = default;
  virtual std::uint32_t add_pass(std::string_view name,
                                 PassType type,
                                 std::vector<ResourceHandle> reads,
                                 std::vector<ResourceHandle> writes) = 0;
  virtual bool add_dependency(std::uint32_t from, std::uint32_t to) = 0;
  virtual bool validate(std::vector<std::string>* diagnostics) const = 0;
  virtual bool execute(IRenderCommandContext& context,
                       const std::vector<std::uint32_t>* execution_order = nullptr) const = 0;
  virtual const std::vector<Pass>& passes() const = 0;
  virtual const std::vector<std::pair<std::uint32_t, std::uint32_t>>& dependencies() const = 0;
};

class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;
  virtual bool initialize() = 0;
  virtual bool shutdown() = 0;
  virtual BackendKind kind() const = 0;
  virtual std::string name() const = 0;
  virtual RenderBackendCapabilities capabilities() const = 0;
  virtual std::unique_ptr<IRenderDevice> create_device() = 0;
  virtual IShaderCompiler* compiler() = 0;
  virtual IShaderCache* shader_cache() = 0;
  virtual std::unique_ptr<IFrameGraph> create_frame_graph() = 0;
  virtual const ResourceLifetimeRegistry* resource_registry() const { return nullptr; }
  virtual std::string last_error() const { return {}; }
};

std::string MakeShaderManifestHash(std::string_view source_text);
std::string BuildShaderCacheKey(std::string_view backend, const ShaderManifest& manifest);
std::string SerializeShaderManifest(const ShaderManifest& manifest);
RenderCrashState BuildRendererCrashState(const IRenderBackend& backend,
                                        uint64_t frame_index,
                                        const std::string& frame_stage,
                                        const std::string& last_pass = {},
                                        const std::string& last_shader_variant = {},
                                        const std::string& extra_error = {});
std::string SerializeRenderCrashState(const RenderCrashState& state);
std::string SerializeBackendCapabilities(const RenderBackendCapabilities& caps);
void AppendDiagnosticField(std::vector<std::string>& diagnostics, const std::string& entry);

}  // namespace vkpt::render
