#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <array>
#include <utility>
#include <variant>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/contracts/Result.h"
#include "core/Types.h"
#include "render/interface/ResourceRegistry.h"

namespace vkpt::render {

struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  std::size_t operator()(const std::string& value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  std::size_t operator()(const char* value) const noexcept {
    return std::hash<std::string_view>{}(value == nullptr ? std::string_view{} : std::string_view{value});
  }
};

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

enum class AcceleratorKind {
  Unknown,
  Cpu,
  DiscreteGpu,
  IntegratedGpu,
  Warp,
  VirtualGpu
};

enum class AcceleratorSelectionPreset {
  Auto,
  HighPerformance
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

enum class ResourceMemoryHint {
  Unknown,
  GpuOnly,
  CpuToGpu,
  GpuToCpu,
  CpuVisible
};

enum class ResourceLifetime {
  Unknown,
  Transient,
  Frame,
  Persistent,
  External
};

enum class ShaderSourceFormat {
  Unknown,
  Glsl,
  Spirv,
  Hlsl,
  Dxil,
  Wgsl,
  Msl,
  MslAir
};

enum class BackendSelectionSource {
  None,
  Explicit,
  Config,
  PlatformPreferred,
  FirstCompatible,
  NullFallback
};

enum class RenderContractState : std::uint8_t {
  Uninitialized,
  Ready,
  Recording,
  Built,
  Executing,
  ShuttingDown,
  Failed,
};

struct RenderStateTransitionContract {
  RenderContractState from = RenderContractState::Uninitialized;
  const char* interface_name = "";
  const char* operation = "";
  RenderContractState to = RenderContractState::Uninitialized;
  const char* result = "";
};

struct RenderInterfaceStandardContract {
  std::string schema_version = "render.interface.contract.v1";
  std::array<RenderStateTransitionContract, 8> state_machine{{
      {RenderContractState::Uninitialized,
       "IRenderBackend",
       "initialize",
       RenderContractState::Ready,
       "Status/Result success; capabilities and factories become queryable"},
      {RenderContractState::Ready,
       "IRenderBackend",
       "shutdown",
       RenderContractState::ShuttingDown,
       "no new devices, frame graphs, compilers, or caches may be requested"},
      {RenderContractState::Uninitialized,
       "IRenderDevice",
       "begin",
       RenderContractState::Recording,
       "command contexts and swapchain access are valid until end"},
      {RenderContractState::Recording,
       "IRenderDevice",
       "end",
       RenderContractState::Ready,
       "in-flight command recording is closed"},
      {RenderContractState::Uninitialized,
       "IFrameGraph",
       "build",
       RenderContractState::Built,
       "passes, dependencies, and resource hazards have been validated"},
      {RenderContractState::Built,
       "IFrameGraph",
       "execute",
       RenderContractState::Executing,
       "returns FrameGraphResult with per-pass Status entries"},
      {RenderContractState::Executing,
       "IFrameGraph",
       "execute_complete",
       RenderContractState::Built,
       "graph remains reusable for later frames"},
      {RenderContractState::Ready,
       "IRenderBackend",
       "create_frame_graph",
       RenderContractState::Ready,
       "factory returns a new unbuilt graph without mutating backend lifecycle"},
  }};
};

struct ShaderManifest {
  std::string shader_family;
  std::string entry_point;
  std::string backend;
  ShaderSourceFormat source_format = ShaderSourceFormat::Unknown;
  std::string source_path;
  std::string source_hash;
  std::string variant_hash;
  std::vector<std::string> defines;
  std::vector<std::string> feature_flags;
  std::vector<std::string> compiler_flags;
  uint32_t resource_layout_version = 0u;
  std::string artifact_path;
  std::string cache_key;
  std::string manifest_dump_path;
  std::string diagnostics_path;
  std::string compile_diagnostics;
  std::string validation_diagnostics;
  bool compile_success = false;
  bool validation_success = false;
};

/// Bitmask describing how a resource is bound during a pass or pipeline.
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
std::string_view AcceleratorKindToString(AcceleratorKind kind);
std::string_view AcceleratorSelectionPresetToString(AcceleratorSelectionPreset preset);

using ResourceHandle = vkpt::core::RuntimeHandle;

constexpr ResourceHandle kInvalidHandle = 0u;

/// Backend-independent buffer creation contract.
struct BufferDesc {
  std::string debug_label;
  std::uint64_t size_bytes = 0u;
  std::uint32_t stride_bytes = 0u;
  bool dynamic = false;
  ResourceBindingUsage usage = ResourceBindingUsage::None;
  bool cpu_visible = false;
  ResourceMemoryHint memory_hint = ResourceMemoryHint::Unknown;
  ResourceLifetime lifetime = ResourceLifetime::Unknown;
};

/// Backend-independent texture allocation contract.
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
  ResourceMemoryHint memory_hint = ResourceMemoryHint::Unknown;
  ResourceLifetime lifetime = ResourceLifetime::Unknown;
};

/// Immutable sampler state contract.
struct SamplerDesc {
  std::string debug_label;
  std::string mag_filter = "Nearest";
  std::string min_filter = "Nearest";
  bool anisotropy_enable = false;
  float max_anisotropy = 1.0f;
  ResourceLifetime lifetime = ResourceLifetime::Unknown;
};

/// Shared shader/pipeline identity used for cache keys and diagnostics.
struct PipelineDesc {
  std::string debug_label;
  std::string source_path;
  ShaderSourceFormat source_format = ShaderSourceFormat::Unknown;
  std::vector<std::string> defines;
  std::string entry_point = "main";
  std::string backend_variant;
  ResourceLifetime lifetime = ResourceLifetime::Unknown;
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

struct DescriptorBindingDesc {
  std::string debug_label;
  std::uint32_t set = 0u;
  std::uint32_t binding = 0u;
  std::uint32_t count = 1u;
  std::string resource_type = "unknown";
  ResourceBindingUsage usage = ResourceBindingUsage::None;
  bool bindless = false;
};

struct DescriptorLayoutDesc {
  std::vector<std::string> bindings;
  std::vector<DescriptorBindingDesc> structured_bindings;
  std::string debug_label;
};

struct ReadbackDesc {
  ResourceHandle source = kInvalidHandle;
  std::size_t byte_count = 0u;
  bool wait_for_idle = true;
};

/// Platform facts that influence backend selection and surface availability.
struct PlatformCapabilities {
  std::string platform_name = "unknown";
  std::string os = "unknown";
  std::string architecture = "unknown";
  bool headless = false;
  bool native_surface = false;
  bool browser_canvas = false;
  bool wasm = false;
  std::string notes;
};

struct CpuCapabilities {
  std::uint32_t logical_cores = 0u;
  std::uint32_t physical_cores = 0u;
  bool fma = false;
  bool atomics = true;
  std::string notes;
};

struct SimdCapabilities {
  bool sse2 = false;
  bool sse42 = false;
  bool avx = false;
  bool avx2 = false;
  bool avx512 = false;
  bool neon = false;
  bool sve = false;
  std::string best_mode = "scalar";
  std::string notes;
};

struct RayTracingCapabilities {
  bool hardware_pipeline = false;
  bool ray_query = false;
  bool acceleration_structures = false;
  bool inline_ray_tracing = false;
  std::uint32_t shader_group_handle_size = 0u;
  std::uint64_t max_acceleration_structure_size = 0u;
  std::uint32_t max_recursion_depth = 0u;
  std::string tier = "none";
  std::string unsupported_reason;
};

struct ShaderCapabilities {
  bool glsl = false;
  bool hlsl = false;
  bool wgsl = false;
  bool msl = false;
  bool spirv = false;
  bool dxil = false;
  bool subgroups = false;
  bool specialization_constants = false;
  std::string shader_model;
  std::vector<std::string> supported_source_formats;
  std::string notes;
};

struct TextureFormatCapabilities {
  bool rgba8_unorm = false;
  bool bgra8_unorm = false;
  bool rgba16_float = false;
  bool rgba32_float = false;
  bool depth32_float = false;
  bool storage_texture_formats = false;
  bool sampled_texture_formats = false;
  std::vector<std::string> guaranteed_formats;
  std::string notes;
};

struct MemoryBudgetCapabilities {
  bool budget_query = false;
  std::uint64_t dedicated_video_memory_bytes = 0u;
  std::uint64_t shared_system_memory_bytes = 0u;
  std::uint64_t current_budget_bytes = 0u;
  std::uint64_t current_usage_bytes = 0u;
  std::uint64_t max_buffer_size_bytes = 0u;
  std::uint64_t upload_alignment_bytes = 0u;
  std::uint64_t readback_alignment_bytes = 0u;
  std::string budget_unavailable_reason;
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
  PlatformCapabilities platform;
  CpuCapabilities cpu;
  SimdCapabilities simd;
  RayTracingCapabilities ray_tracing_caps;
  ShaderCapabilities shader;
  TextureFormatCapabilities texture_formats_caps;
  MemoryBudgetCapabilities memory_budget;
};

/// Discovered execution target used by multi-accelerator planners.
struct AcceleratorCapabilities {
  std::string id;
  std::string name = "unknown";
  AcceleratorKind accelerator_kind = AcceleratorKind::Unknown;
  BackendKind backend_kind = BackendKind::Unknown;
  bool available = false;
  bool hardware = false;
  bool cpu = false;
  bool d3d12 = false;
  bool compute = false;
  bool ray_tracing = false;
  bool presentation = false;
  bool warp = false;
  bool unified_memory = false;
  bool cache_coherent_uma = false;
  bool selected_by_default = false;
  std::uint32_t node_count = 1u;
  std::uint32_t vendor_id = 0u;
  std::uint32_t device_id = 0u;
  std::uint64_t dedicated_video_memory_bytes = 0u;
  std::uint64_t shared_system_memory_bytes = 0u;
  std::uint64_t current_budget_bytes = 0u;
  std::uint64_t current_usage_bytes = 0u;
  double estimated_rays_per_ms = 0.0;
  std::string adapter_luid;
  std::string notes;
  RenderBackendCapabilities backend_caps;
};

/// Input to the ray-budget planner for splitting path tracing around raster work.
struct RayBudgetRequest {
  AcceleratorSelectionPreset accelerator_preset = AcceleratorSelectionPreset::Auto;
  double polygon_frame_budget_ms = 16.6667;
  double reserved_polygon_ms = 5.0;
  double merge_budget_ms = 1.0;
  std::uint32_t width = 1280u;
  std::uint32_t height = 720u;
  std::uint32_t max_bounces = 6u;
  std::uint64_t min_rays_per_batch = 65536u;
  bool include_cpu = true;
  bool include_integrated_gpu = true;
  bool include_warp = false;
  bool require_ray_tracing = false;
};

/// Per-accelerator work assignment emitted by the ray-budget planner.
struct RayBudgetAssignment {
  std::string accelerator_id;
  std::string accelerator_name;
  AcceleratorKind accelerator_kind = AcceleratorKind::Unknown;
  BackendKind backend_kind = BackendKind::Unknown;
  std::string backend_name;
  bool active = false;
  bool uses_dxr = false;
  std::uint32_t worker_threads = 1u;
  std::uint64_t target_rays = 0u;
  double budget_ms = 0.0;
  double estimated_rays_per_ms = 0.0;
  std::string reason;
};

/// Complete planner result including diagnostics and estimated samples per pixel.
struct RayBudgetPlan {
  double polygon_frame_budget_ms = 0.0;
  double reserved_polygon_ms = 0.0;
  double merge_budget_ms = 0.0;
  double ray_budget_ms = 0.0;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint64_t total_target_rays = 0u;
  double estimated_samples_per_pixel = 0.0;
  std::vector<RayBudgetAssignment> assignments;
  std::vector<std::string> diagnostics;
};

/// Serializable renderer state intended for crash logs and diagnostics.
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

struct FrameContext {
  std::uint64_t frame_index = 0u;
  double delta_time_seconds = 0.0;
  double elapsed_time_seconds = 0.0;
  std::uint32_t viewport_width = 0u;
  std::uint32_t viewport_height = 0u;
  std::uint32_t swapchain_width = 0u;
  std::uint32_t swapchain_height = 0u;
  ResourceHandle color_target = kInvalidHandle;
  ResourceHandle depth_target = kInvalidHandle;
  ResourceHandle output_target = kInvalidHandle;
  ResourceHandle readback_target = kInvalidHandle;
  bool allow_gpu_timestamps = false;
  bool capture_debug_markers = false;
  bool present = false;
  std::string debug_label;
};

/// Serializable pass declaration consumed by IFrameGraph::build().
struct FrameGraphPassDesc {
  std::uint32_t id = 0u;
  std::string name;
  PassType type = PassType::Compute;
  std::vector<ResourceHandle> reads;
  std::vector<ResourceHandle> writes;
};

/// Declarative frame graph plus transient resource declarations.
struct FrameGraphDesc {
  std::string debug_label;
  std::vector<FrameGraphPassDesc> passes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> dependencies;
  std::vector<BufferDesc> transient_buffers;
  std::vector<TextureDesc> transient_textures;
  std::uint32_t target_width = 0u;
  std::uint32_t target_height = 0u;
  bool validate_hazards = true;
};

struct FrameGraphPassResult {
  std::uint32_t pass_id = 0u;
  std::string pass_name;
  vkpt::core::Status status = vkpt::core::Status::ok();
};

struct FrameGraphResult {
  vkpt::core::Status overall = vkpt::core::Status::ok();
  std::vector<FrameGraphPassResult> per_pass;

  /// Preferred naming for new call sites; fields remain for source compatibility.
  const vkpt::core::Status& overall_status() const noexcept { return overall; }
  vkpt::core::Status& overall_status() noexcept { return overall; }
  const std::vector<FrameGraphPassResult>& pass_results() const noexcept {
    return per_pass;
  }
  std::vector<FrameGraphPassResult>& pass_results() noexcept { return per_pass; }
  bool is_ok() const noexcept { return overall.is_ok(); }
  bool is_error() const noexcept { return overall.is_error(); }
  operator bool() const noexcept { return is_ok(); }
};

/// Backend advertised to the selection policy before an instance is created.
struct BackendCandidateDesc {
  std::string name;
  BackendKind kind = BackendKind::Unknown;
  std::uint32_t selection_priority = 0u;
  bool compiled = false;
  bool available = false;
  bool adapter_skeleton = false;
  bool experimental = false;
  bool supports_compute = false;
  bool supports_presentation = false;
  bool supports_ray_tracing = false;
  std::string unavailable_reason;
};

/// Constraints and preferences used by SelectBackend().
struct BackendSelectionRequest {
  std::string explicit_backend;
  std::string config_backend;
  std::string platform_preferred_backend;
  bool require_compute = true;
  bool require_presentation = false;
  bool require_ray_tracing = false;
  bool allow_adapter_skeleton = true;
  bool allow_experimental = false;
  bool allow_null_fallback = false;
};

/// Backend selection result, including rejected candidates for diagnostics.
struct BackendSelectionDecision {
  std::string selected_backend;
  BackendKind selected_kind = BackendKind::Unknown;
  BackendSelectionSource source = BackendSelectionSource::None;
  bool selected = false;
  std::string reason;
  std::vector<BackendCandidateDesc> candidates;
  std::vector<std::string> diagnostics;
};

/// Minimal command recording interface consumed by frame-graph execution.
///
/// IRenderCommandContext state machine contract:
///
/// state\method      begin_frame  end_frame   begin_pass   end_pass    dispatch  copy_buffer_to_texture  barrier
/// Uninitialized     ->Recording  error       illegal      illegal     illegal   illegal                 illegal
/// Recording         noop         ->Submitted ->InPass     illegal     illegal   ok                      ok
/// InPass            illegal      illegal     illegal      ->Recording ok        ok                      ok
/// Submitted         ->Recording  noop        illegal      illegal     illegal   illegal                 illegal
/// Failed            error        error       error        error       error     error                   error
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

/// Backend-owned resource allocator contract for buffers, textures, uploads, and readback.
///
/// IRenderResourceAllocator state machine contract:
///
/// state\method     create_buffer  destroy_buffer  create_texture  destroy_texture  upload_data  readback
/// Uninitialized    illegal        noop            illegal         noop             illegal      illegal
/// Ready            ok             ok              ok              ok               ok           ok
/// ShuttingDown     illegal        ok              illegal         ok               illegal      illegal
/// Failed           error          noop            error           noop             error        error
///
/// destroy_*() against an unknown handle is noop; against a live handle in
/// Ready/ShuttingDown it is ok and idempotent.
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

/// Presentation surface contract; headless backends may expose a zero-sized stub.
class IRenderSwapchain {
 public:
  virtual ~IRenderSwapchain() = default;
  virtual bool present() = 0;
  virtual std::uint32_t width() const = 0;
  virtual std::uint32_t height() const = 0;
  virtual bool resize(std::uint32_t width, std::uint32_t height) = 0;
};

/// Logical render device lifetime and command-context factory.
///
/// IRenderDevice state machine contract:
///
/// state\method     begin      end        create_command_context  swapchain
/// Uninitialized    ->Recording noop       error                   ok|null
/// Recording        noop       ->Ready     ok                      ok|null
/// Ready            ->Recording noop       ok                      ok|null
/// Failed           error      noop       error                   ok|null
///
/// begin()/end() are serialized lifecycle calls owned by the render backend.
class IRenderDevice {
 public:
  virtual ~IRenderDevice() = default;

  virtual bool begin() = 0;
  virtual bool end() = 0;
  virtual std::unique_ptr<IRenderCommandContext> create_command_context() = 0;
  virtual IRenderSwapchain* swapchain() const = 0;
};

/// Shader compiler facade used by backend skeletons and future native compilers.
///
/// IShaderCompiler state machine contract:
///
/// state\method     supports_feature  compile_compute_shader
/// Uninitialized    ok                illegal
/// Ready            ok                ok
/// ShuttingDown     ok                illegal
/// Failed           ok                error
///
/// Compiler lifetime is bound to its owning backend: the pointer returned by
/// IRenderBackend::compiler() is valid only between successful initialize()
/// and the start of shutdown().
class IShaderCompiler {
 public:
  virtual ~IShaderCompiler() = default;
  virtual bool supports_feature(std::string_view feature) const = 0;
  /// Thread-safety: implementations must be reentrant for concurrent compile
  /// requests once their owning backend has completed initialize().
  virtual vkpt::core::Status compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) = 0;
};

struct CachedManifest {
  std::string manifest_text;
  std::string backend;
  std::string cache_key;
  std::string artifact_path;
  std::string manifest_dump_path;
  bool compile_success = false;
};

/// IShaderCache state machine contract:
///
/// state\method     query    store    invalidate  explain_miss  dump_manifest
/// Uninitialized    miss     illegal  noop        ok            empty
/// Ready            ok       ok       ok          ok            ok
/// ShuttingDown     ok       illegal  noop        ok            ok
/// Failed           miss     error    noop        ok            empty
///
/// Cache lifetime is bound to its owning backend (see IRenderBackend grid).
/// `query` returns false on miss in any state; `store` only mutates in Ready.
class IShaderCache {
 public:
  virtual ~IShaderCache() = default;
  /// Thread-safety: this interface does not require internal locking. Callers
  /// sharing one cache across threads must serialize query/store/invalidate/
  /// dump calls, and no cache method may race backend shutdown().
  virtual bool query(std::string_view key, std::string& binary) = 0;
  virtual bool store(std::string_view key, const std::string& binary) = 0;
  virtual bool invalidate(std::string_view key) = 0;
  virtual std::string explain_miss(std::string_view key) const = 0;
  virtual std::vector<CachedManifest> dump_manifest() const = 0;
};

/// Declarative render pass graph with dependency and resource-hazard validation.
///
/// IFrameGraph state machine contract:
///
/// state\method  add_pass  build     add_dependency  validate  execute  passes/dependencies
/// Empty         ok        ->Built   ok              ok        error    ok
/// Built         ok        ->Built   ok              ok        ok       ok
/// Executing     illegal   illegal   illegal         ok        illegal  ok
/// Failed        ok        ->Built   ok              ok        error    ok
///
/// execute() reports failures through FrameGraphResult::overall_status() and
/// per-pass Status entries; bool is reserved for predicates only.
class IFrameGraph {
 public:
  /// Concrete pass record after a graph has assigned dense ids.
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
  virtual bool build(const FrameGraphDesc& desc, std::vector<std::string>* diagnostics = nullptr) = 0;
  virtual bool add_dependency(std::uint32_t from, std::uint32_t to) = 0;
  virtual bool validate(std::vector<std::string>* diagnostics) const = 0;
  virtual FrameGraphResult execute(
      IRenderCommandContext& context,
      const std::vector<std::uint32_t>* execution_order = nullptr) const = 0;
  virtual FrameGraphResult execute(
      IRenderCommandContext& context,
      const FrameContext& frame,
      const std::vector<std::uint32_t>* execution_order = nullptr) const = 0;
  virtual const std::vector<Pass>& passes() const = 0;
  virtual const std::vector<std::pair<std::uint32_t, std::uint32_t>>& dependencies() const = 0;
};

/// Backend module contract for initialization, device creation, compiler/cache, and graphs.
///
/// IRenderBackend state machine contract:
///
/// state\method      initialize  shutdown      capabilities  create_device  compiler/cache  create_frame_graph
/// Uninitialized     ->Ready     noop          ok            error          null/error       error
/// Ready             noop        ->ShuttingDown ok           ok             ok               ok
/// ShuttingDown      error       noop          ok            error          null/error       error
/// Failed            error       noop          ok            error          null/error       error
///
/// Implementations should expose failure details through Status/Result-returning
/// factory helpers when available; legacy bool/null returns are compatibility
/// shims and must also update last_error().
class IRenderBackend {
 public:
  virtual ~IRenderBackend() = default;
  virtual vkpt::core::Status initialize() = 0;
  virtual vkpt::core::Status shutdown() = 0;
  virtual BackendKind kind() const = 0;
  virtual std::string name() const = 0;
  virtual RenderBackendCapabilities capabilities() const = 0;
  virtual std::unique_ptr<IRenderDevice> create_device() = 0;
  /// Thread-safety: compiler()/shader_cache() pointer retrieval is stable only
  /// after initialize() succeeds and before shutdown() begins. initialize(),
  /// shutdown(), create_device(), and create_frame_graph() are lifecycle calls
  /// the owner must serialize. The returned compiler is reentrant; the returned
  /// cache follows IShaderCache's externally serialized mutation contract.
  virtual IShaderCompiler* compiler() = 0;
  virtual IShaderCache* shader_cache() = 0;
  virtual std::unique_ptr<IFrameGraph> create_frame_graph() = 0;
  virtual const ResourceLifetimeRegistry* resource_registry() const { return nullptr; }
  virtual std::string last_error() const { return {}; }
};

std::string MakeShaderManifestHash(std::string_view source_text);
std::string BuildShaderCacheKey(std::string_view backend, const ShaderManifest& manifest);
std::string BuildShaderManifestDumpPath(std::string_view dump_root, const ShaderManifest& manifest);
std::string SerializeShaderManifest(const ShaderManifest& manifest);
std::string SerializeCachedManifest(const CachedManifest& manifest);
std::string SerializeShaderCacheDump(const std::vector<CachedManifest>& manifests);
RenderCrashState BuildRendererCrashState(const IRenderBackend& backend,
                                        uint64_t frame_index,
                                        const std::string& frame_stage,
                                        const std::string& last_pass = {},
                                        const std::string& last_shader_variant = {},
                                        const std::string& extra_error = {});
std::string SerializeRenderCrashState(const RenderCrashState& state);
std::string SerializeBackendCapabilities(const RenderBackendCapabilities& caps);
std::string SerializeAcceleratorCapabilities(const AcceleratorCapabilities& caps);
std::string SerializeRayBudgetRequest(const RayBudgetRequest& request);
std::string SerializeRayBudgetAssignment(const RayBudgetAssignment& assignment);
std::string SerializeRayBudgetPlan(const RayBudgetPlan& plan);
std::string SerializePlatformCapabilities(const PlatformCapabilities& caps);
std::string SerializeCpuCapabilities(const CpuCapabilities& caps);
std::string SerializeSimdCapabilities(const SimdCapabilities& caps);
std::string SerializeRayTracingCapabilities(const RayTracingCapabilities& caps);
std::string SerializeShaderCapabilities(const ShaderCapabilities& caps);
std::string SerializeTextureFormatCapabilities(const TextureFormatCapabilities& caps);
std::string SerializeMemoryBudgetCapabilities(const MemoryBudgetCapabilities& caps);
std::string SerializeResourceBindingUsage(ResourceBindingUsage usage);
std::string SerializeBufferDesc(const BufferDesc& desc);
std::string SerializeTextureDesc(const TextureDesc& desc);
std::string SerializeSamplerDesc(const SamplerDesc& desc);
std::string SerializePipelineDesc(const PipelineDesc& desc);
std::string SerializeComputePipelineDesc(const ComputePipelineDesc& desc);
std::string SerializeRayTracingPipelineDesc(const RayTracingPipelineDesc& desc);
std::string SerializeDescriptorBindingDesc(const DescriptorBindingDesc& desc);
std::string SerializeDescriptorLayoutDesc(const DescriptorLayoutDesc& desc);
std::string SerializeReadbackDesc(const ReadbackDesc& desc);
std::string SerializeFrameContext(const FrameContext& context);
std::string SerializeFrameGraphDesc(const FrameGraphDesc& desc);
std::string SerializeBackendCandidateDesc(const BackendCandidateDesc& desc);
std::string SerializeBackendSelectionDecision(const BackendSelectionDecision& decision);
std::string_view PassTypeToString(PassType type);
std::string_view ResourceMemoryHintToString(ResourceMemoryHint hint);
std::string_view ResourceLifetimeToString(ResourceLifetime lifetime);
std::string_view ShaderSourceFormatToString(ShaderSourceFormat format);
std::string_view BackendSelectionSourceToString(BackendSelectionSource source);
void AppendDiagnosticField(std::vector<std::string>& diagnostics, const std::string& entry);

inline RenderInterfaceStandardContract BuildStandardRenderInterfaceContract() {
  return {};
}

inline bool ValidateStandardRenderInterfaceContract(
    const RenderInterfaceStandardContract& contract,
    std::vector<std::string>* diagnostics = nullptr) {
  if (diagnostics) {
    diagnostics->clear();
  }
  bool ok = true;
  auto require = [&](bool condition, const char* message) {
    if (!condition) {
      ok = false;
      if (diagnostics) {
        diagnostics->push_back(message);
      }
    }
  };

  require(contract.schema_version == "render.interface.contract.v1",
          "unexpected render interface contract schema version");
  require(contract.state_machine.size() == 8u,
          "render interface state machine must publish eight standard transitions");
  require(contract.state_machine[0].from == RenderContractState::Uninitialized &&
              std::string_view(contract.state_machine[0].interface_name) == "IRenderBackend" &&
              std::string_view(contract.state_machine[0].operation) == "initialize" &&
              contract.state_machine[0].to == RenderContractState::Ready,
          "render interface contract missing backend initialize transition");
  require(contract.state_machine[2].from == RenderContractState::Uninitialized &&
              std::string_view(contract.state_machine[2].interface_name) == "IRenderDevice" &&
              std::string_view(contract.state_machine[2].operation) == "begin" &&
              contract.state_machine[2].to == RenderContractState::Recording,
          "render interface contract missing device begin transition");
  require(contract.state_machine[4].from == RenderContractState::Uninitialized &&
              std::string_view(contract.state_machine[4].interface_name) == "IFrameGraph" &&
              std::string_view(contract.state_machine[4].operation) == "build" &&
              contract.state_machine[4].to == RenderContractState::Built,
          "render interface contract missing frame graph build transition");
  require(contract.state_machine[5].from == RenderContractState::Built &&
              std::string_view(contract.state_machine[5].interface_name) == "IFrameGraph" &&
              std::string_view(contract.state_machine[5].operation) == "execute" &&
              contract.state_machine[5].to == RenderContractState::Executing,
          "render interface contract missing frame graph execute transition");
  return ok;
}

}  // namespace vkpt::render
