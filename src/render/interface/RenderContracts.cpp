#include "render/interface/RenderContracts.h"

#include <cctype>
#include <sstream>

namespace vkpt::render {
namespace {

std::string EscapeJson(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8u);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string Quote(std::string_view value) {
  return "\"" + EscapeJson(value) + "\"";
}

const char* Bool(bool value) {
  return value ? "true" : "false";
}

std::string SerializeStringArray(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << Quote(values[i]);
  }
  out << ']';
  return out.str();
}

std::string SerializeHandleArray(const std::vector<ResourceHandle>& values) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
  return out.str();
}

std::string SanitizePathSegment(std::string_view value, std::string_view fallback) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) || ch == '-' || ch == '_') {
      out.push_back(static_cast<char>(std::tolower(uch)));
    } else if (ch == '.' || ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      if (out.empty() || out.back() != '_') {
        out.push_back('_');
      }
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? std::string(fallback) : out;
}

void AppendPipelineFields(std::ostringstream& out, const PipelineDesc& desc) {
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"source_path\":" << Quote(desc.source_path) << ',';
  out << "\"source_format\":" << Quote(ShaderSourceFormatToString(desc.source_format)) << ',';
  out << "\"entry_point\":" << Quote(desc.entry_point) << ',';
  out << "\"backend_variant\":" << Quote(desc.backend_variant) << ',';
  out << "\"defines\":" << SerializeStringArray(desc.defines) << ',';
  out << "\"lifetime\":" << Quote(ResourceLifetimeToString(desc.lifetime));
}

}  // namespace

std::string_view BackendKindToString(BackendKind kind) {
  switch (kind) {
    case BackendKind::Null:
      return "null";
    case BackendKind::VulkanCompute:
      return "vulkan-compute";
    case BackendKind::VulkanRt:
      return "vulkan-rt";
    case BackendKind::D3d12:
      return "d3d12";
    case BackendKind::Metal:
      return "metal";
    case BackendKind::WebGpu:
      return "webgpu";
    case BackendKind::OpenGLExperimental:
      return "opengl-experimental";
    case BackendKind::Unknown:
      return "unknown";
    default:
      return "unknown";
  }
}

std::string_view AcceleratorKindToString(AcceleratorKind kind) {
  switch (kind) {
    case AcceleratorKind::Cpu:
      return "cpu";
    case AcceleratorKind::DiscreteGpu:
      return "discrete-gpu";
    case AcceleratorKind::IntegratedGpu:
      return "integrated-gpu";
    case AcceleratorKind::Warp:
      return "warp";
    case AcceleratorKind::VirtualGpu:
      return "virtual-gpu";
    case AcceleratorKind::Unknown:
    default:
      return "unknown";
  }
}

std::string_view PassTypeToString(PassType type) {
  switch (type) {
    case PassType::Upload:
      return "upload";
    case PassType::Compute:
      return "compute";
    case PassType::Raster:
      return "raster";
    case PassType::Copy:
      return "copy";
    case PassType::Resolve:
      return "resolve";
    case PassType::Readback:
      return "readback";
    case PassType::Present:
      return "present";
    case PassType::Debug:
      return "debug";
    default:
      return "unknown";
  }
}

std::string_view ResourceMemoryHintToString(ResourceMemoryHint hint) {
  switch (hint) {
    case ResourceMemoryHint::GpuOnly:
      return "gpu-only";
    case ResourceMemoryHint::CpuToGpu:
      return "cpu-to-gpu";
    case ResourceMemoryHint::GpuToCpu:
      return "gpu-to-cpu";
    case ResourceMemoryHint::CpuVisible:
      return "cpu-visible";
    case ResourceMemoryHint::Unknown:
    default:
      return "unknown";
  }
}

std::string_view ResourceLifetimeToString(ResourceLifetime lifetime) {
  switch (lifetime) {
    case ResourceLifetime::Transient:
      return "transient";
    case ResourceLifetime::Frame:
      return "frame";
    case ResourceLifetime::Persistent:
      return "persistent";
    case ResourceLifetime::External:
      return "external";
    case ResourceLifetime::Unknown:
    default:
      return "unknown";
  }
}

std::string_view ShaderSourceFormatToString(ShaderSourceFormat format) {
  switch (format) {
    case ShaderSourceFormat::Glsl:
      return "glsl";
    case ShaderSourceFormat::Spirv:
      return "spirv";
    case ShaderSourceFormat::Hlsl:
      return "hlsl";
    case ShaderSourceFormat::Dxil:
      return "dxil";
    case ShaderSourceFormat::Wgsl:
      return "wgsl";
    case ShaderSourceFormat::Msl:
      return "msl";
    case ShaderSourceFormat::MslAir:
      return "msl-air";
    case ShaderSourceFormat::Unknown:
    default:
      return "unknown";
  }
}

std::string_view BackendSelectionSourceToString(BackendSelectionSource source) {
  switch (source) {
    case BackendSelectionSource::Explicit:
      return "explicit";
    case BackendSelectionSource::Config:
      return "config";
    case BackendSelectionSource::PlatformPreferred:
      return "platform-preferred";
    case BackendSelectionSource::FirstCompatible:
      return "first-compatible";
    case BackendSelectionSource::NullFallback:
      return "null-fallback";
    case BackendSelectionSource::None:
    default:
      return "none";
  }
}

std::string SerializeResourceBindingUsage(ResourceBindingUsage usage) {
  std::vector<std::string> flags;
  if (usage == ResourceBindingUsage::None) {
    flags.push_back("none");
  }
  if (HasUsage(usage, ResourceBindingUsage::Read)) {
    flags.push_back("read");
  }
  if (HasUsage(usage, ResourceBindingUsage::Write)) {
    flags.push_back("write");
  }
  if (HasUsage(usage, ResourceBindingUsage::Storage)) {
    flags.push_back("storage");
  }
  if (HasUsage(usage, ResourceBindingUsage::Uniform)) {
    flags.push_back("uniform");
  }
  return SerializeStringArray(flags);
}

std::string SerializeBufferDesc(const BufferDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"size_bytes\":" << desc.size_bytes << ',';
  out << "\"stride_bytes\":" << desc.stride_bytes << ',';
  out << "\"dynamic\":" << Bool(desc.dynamic) << ',';
  out << "\"usage\":" << SerializeResourceBindingUsage(desc.usage) << ',';
  out << "\"cpu_visible\":" << Bool(desc.cpu_visible) << ',';
  out << "\"memory_hint\":" << Quote(ResourceMemoryHintToString(desc.memory_hint)) << ',';
  out << "\"lifetime\":" << Quote(ResourceLifetimeToString(desc.lifetime));
  out << '}';
  return out.str();
}

std::string SerializeTextureDesc(const TextureDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"width\":" << desc.width << ',';
  out << "\"height\":" << desc.height << ',';
  out << "\"depth\":" << desc.depth << ',';
  out << "\"mip_levels\":" << desc.mip_levels << ',';
  out << "\"array_layers\":" << desc.array_layers << ',';
  out << "\"format\":" << Quote(desc.format) << ',';
  out << "\"usage\":" << SerializeResourceBindingUsage(desc.usage) << ',';
  out << "\"cube\":" << Bool(desc.cube) << ',';
  out << "\"memory_hint\":" << Quote(ResourceMemoryHintToString(desc.memory_hint)) << ',';
  out << "\"lifetime\":" << Quote(ResourceLifetimeToString(desc.lifetime));
  out << '}';
  return out.str();
}

std::string SerializeSamplerDesc(const SamplerDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"mag_filter\":" << Quote(desc.mag_filter) << ',';
  out << "\"min_filter\":" << Quote(desc.min_filter) << ',';
  out << "\"anisotropy_enable\":" << Bool(desc.anisotropy_enable) << ',';
  out << "\"max_anisotropy\":" << desc.max_anisotropy << ',';
  out << "\"lifetime\":" << Quote(ResourceLifetimeToString(desc.lifetime));
  out << '}';
  return out.str();
}

std::string SerializePipelineDesc(const PipelineDesc& desc) {
  std::ostringstream out;
  out << '{';
  AppendPipelineFields(out, desc);
  out << '}';
  return out.str();
}

std::string SerializeComputePipelineDesc(const ComputePipelineDesc& desc) {
  std::ostringstream out;
  out << '{';
  AppendPipelineFields(out, desc);
  out << ',';
  out << "\"subgroup_size\":" << desc.subgroup_size << ',';
  out << "\"workgroup_x\":" << desc.workgroup_x << ',';
  out << "\"workgroup_y\":" << desc.workgroup_y << ',';
  out << "\"workgroup_z\":" << desc.workgroup_z;
  out << '}';
  return out.str();
}

std::string SerializeRayTracingPipelineDesc(const RayTracingPipelineDesc& desc) {
  std::ostringstream out;
  out << '{';
  AppendPipelineFields(out, desc);
  out << ',';
  out << "\"hit_group\":" << Quote(desc.hit_group) << ',';
  out << "\"max_recursion_depth\":" << desc.max_recursion_depth;
  out << '}';
  return out.str();
}

std::string SerializeDescriptorBindingDesc(const DescriptorBindingDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"set\":" << desc.set << ',';
  out << "\"binding\":" << desc.binding << ',';
  out << "\"count\":" << desc.count << ',';
  out << "\"resource_type\":" << Quote(desc.resource_type) << ',';
  out << "\"usage\":" << SerializeResourceBindingUsage(desc.usage) << ',';
  out << "\"bindless\":" << Bool(desc.bindless);
  out << '}';
  return out.str();
}

std::string SerializeDescriptorLayoutDesc(const DescriptorLayoutDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"bindings\":" << SerializeStringArray(desc.bindings) << ',';
  out << "\"structured_bindings\":[";
  for (std::size_t i = 0; i < desc.structured_bindings.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeDescriptorBindingDesc(desc.structured_bindings[i]);
  }
  out << "]}";
  return out.str();
}

std::string SerializeReadbackDesc(const ReadbackDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"source\":" << desc.source << ',';
  out << "\"byte_count\":" << desc.byte_count << ',';
  out << "\"wait_for_idle\":" << Bool(desc.wait_for_idle);
  out << '}';
  return out.str();
}

std::string SerializePlatformCapabilities(const PlatformCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"platform_name\":" << Quote(caps.platform_name) << ',';
  out << "\"os\":" << Quote(caps.os) << ',';
  out << "\"architecture\":" << Quote(caps.architecture) << ',';
  out << "\"headless\":" << Bool(caps.headless) << ',';
  out << "\"native_surface\":" << Bool(caps.native_surface) << ',';
  out << "\"browser_canvas\":" << Bool(caps.browser_canvas) << ',';
  out << "\"wasm\":" << Bool(caps.wasm) << ',';
  out << "\"notes\":" << Quote(caps.notes);
  out << '}';
  return out.str();
}

std::string SerializeCpuCapabilities(const CpuCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"logical_cores\":" << caps.logical_cores << ',';
  out << "\"physical_cores\":" << caps.physical_cores << ',';
  out << "\"fma\":" << Bool(caps.fma) << ',';
  out << "\"atomics\":" << Bool(caps.atomics) << ',';
  out << "\"notes\":" << Quote(caps.notes);
  out << '}';
  return out.str();
}

std::string SerializeSimdCapabilities(const SimdCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"sse2\":" << Bool(caps.sse2) << ',';
  out << "\"sse42\":" << Bool(caps.sse42) << ',';
  out << "\"avx\":" << Bool(caps.avx) << ',';
  out << "\"avx2\":" << Bool(caps.avx2) << ',';
  out << "\"avx512\":" << Bool(caps.avx512) << ',';
  out << "\"neon\":" << Bool(caps.neon) << ',';
  out << "\"sve\":" << Bool(caps.sve) << ',';
  out << "\"best_mode\":" << Quote(caps.best_mode) << ',';
  out << "\"notes\":" << Quote(caps.notes);
  out << '}';
  return out.str();
}

std::string SerializeRayTracingCapabilities(const RayTracingCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"hardware_pipeline\":" << Bool(caps.hardware_pipeline) << ',';
  out << "\"ray_query\":" << Bool(caps.ray_query) << ',';
  out << "\"acceleration_structures\":" << Bool(caps.acceleration_structures) << ',';
  out << "\"inline_ray_tracing\":" << Bool(caps.inline_ray_tracing) << ',';
  out << "\"shader_group_handle_size\":" << caps.shader_group_handle_size << ',';
  out << "\"max_acceleration_structure_size\":" << caps.max_acceleration_structure_size << ',';
  out << "\"max_recursion_depth\":" << caps.max_recursion_depth << ',';
  out << "\"tier\":" << Quote(caps.tier) << ',';
  out << "\"unsupported_reason\":" << Quote(caps.unsupported_reason);
  out << '}';
  return out.str();
}

std::string SerializeShaderCapabilities(const ShaderCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"glsl\":" << Bool(caps.glsl) << ',';
  out << "\"hlsl\":" << Bool(caps.hlsl) << ',';
  out << "\"wgsl\":" << Bool(caps.wgsl) << ',';
  out << "\"msl\":" << Bool(caps.msl) << ',';
  out << "\"spirv\":" << Bool(caps.spirv) << ',';
  out << "\"dxil\":" << Bool(caps.dxil) << ',';
  out << "\"subgroups\":" << Bool(caps.subgroups) << ',';
  out << "\"specialization_constants\":" << Bool(caps.specialization_constants) << ',';
  out << "\"shader_model\":" << Quote(caps.shader_model) << ',';
  out << "\"supported_source_formats\":" << SerializeStringArray(caps.supported_source_formats) << ',';
  out << "\"notes\":" << Quote(caps.notes);
  out << '}';
  return out.str();
}

std::string SerializeTextureFormatCapabilities(const TextureFormatCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"rgba8_unorm\":" << Bool(caps.rgba8_unorm) << ',';
  out << "\"bgra8_unorm\":" << Bool(caps.bgra8_unorm) << ',';
  out << "\"rgba16_float\":" << Bool(caps.rgba16_float) << ',';
  out << "\"rgba32_float\":" << Bool(caps.rgba32_float) << ',';
  out << "\"depth32_float\":" << Bool(caps.depth32_float) << ',';
  out << "\"storage_texture_formats\":" << Bool(caps.storage_texture_formats) << ',';
  out << "\"sampled_texture_formats\":" << Bool(caps.sampled_texture_formats) << ',';
  out << "\"guaranteed_formats\":" << SerializeStringArray(caps.guaranteed_formats) << ',';
  out << "\"notes\":" << Quote(caps.notes);
  out << '}';
  return out.str();
}

std::string SerializeMemoryBudgetCapabilities(const MemoryBudgetCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"budget_query\":" << Bool(caps.budget_query) << ',';
  out << "\"dedicated_video_memory_bytes\":" << caps.dedicated_video_memory_bytes << ',';
  out << "\"shared_system_memory_bytes\":" << caps.shared_system_memory_bytes << ',';
  out << "\"current_budget_bytes\":" << caps.current_budget_bytes << ',';
  out << "\"current_usage_bytes\":" << caps.current_usage_bytes << ',';
  out << "\"max_buffer_size_bytes\":" << caps.max_buffer_size_bytes << ',';
  out << "\"upload_alignment_bytes\":" << caps.upload_alignment_bytes << ',';
  out << "\"readback_alignment_bytes\":" << caps.readback_alignment_bytes << ',';
  out << "\"budget_unavailable_reason\":" << Quote(caps.budget_unavailable_reason);
  out << '}';
  return out.str();
}

std::string SerializeBackendCapabilities(const RenderBackendCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"backend_name\":" << Quote(caps.backend_name) << ',';
  out << "\"compute\":" << Bool(caps.compute) << ',';
  out << "\"storage_buffers\":" << Bool(caps.storage_buffers) << ',';
  out << "\"storage_textures\":" << Bool(caps.storage_textures) << ',';
  out << "\"timestamp_queries\":" << Bool(caps.timestamp_queries) << ',';
  out << "\"timestamp_fallback_reason\":" << Quote(caps.timestamp_fallback_reason) << ',';
  out << "\"subgroups\":" << Bool(caps.subgroups) << ',';
  out << "\"descriptor_indexing\":" << Bool(caps.descriptor_indexing) << ',';
  out << "\"bindless_like_resources\":" << Bool(caps.bindless_like_resources) << ',';
  out << "\"texture_formats\":" << Bool(caps.texture_formats) << ',';
  out << "\"ray_tracing\":" << Bool(caps.ray_tracing) << ',';
  out << "\"ray_query\":" << Bool(caps.ray_query) << ',';
  out << "\"ray_query_supported\":" << Bool(caps.ray_query_supported) << ',';
  out << "\"acceleration_structure_supported\":" << Bool(caps.acceleration_structure_supported) << ',';
  out << "\"shader_group_handle_size\":" << caps.shader_group_handle_size << ',';
  out << "\"max_as_size\":" << caps.max_as_size << ',';
  out << "\"presentation\":" << Bool(caps.presentation) << ',';
  out << "\"readback\":" << Bool(caps.readback) << ',';
  out << "\"is_simulated\":" << Bool(caps.is_simulated) << ',';
  out << "\"supports_present\":" << Bool(caps.supports_present) << ',';
  out << "\"supports_multiqueue\":" << Bool(caps.supports_multiqueue) << ',';
  out << "\"max_workgroup_size_x\":" << caps.max_workgroup_size_x << ',';
  out << "\"max_workgroup_size_y\":" << caps.max_workgroup_size_y << ',';
  out << "\"max_workgroup_size_z\":" << caps.max_workgroup_size_z << ',';
  out << "\"max_buffer_alignment\":" << caps.max_buffer_alignment << ',';
  out << "\"memory_model\":" << Quote(caps.memory_model) << ',';
  out << "\"notes\":" << Quote(caps.notes) << ',';
  out << "\"platform\":" << SerializePlatformCapabilities(caps.platform) << ',';
  out << "\"cpu\":" << SerializeCpuCapabilities(caps.cpu) << ',';
  out << "\"simd\":" << SerializeSimdCapabilities(caps.simd) << ',';
  out << "\"ray_tracing_capabilities\":" << SerializeRayTracingCapabilities(caps.ray_tracing_caps) << ',';
  out << "\"shader_capabilities\":" << SerializeShaderCapabilities(caps.shader) << ',';
  out << "\"texture_format_capabilities\":" << SerializeTextureFormatCapabilities(caps.texture_formats_caps) << ',';
  out << "\"memory_budget\":" << SerializeMemoryBudgetCapabilities(caps.memory_budget);
  out << '}';
  return out.str();
}

std::string SerializeAcceleratorCapabilities(const AcceleratorCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"id\":" << Quote(caps.id) << ',';
  out << "\"name\":" << Quote(caps.name) << ',';
  out << "\"accelerator_kind\":" << Quote(AcceleratorKindToString(caps.accelerator_kind)) << ',';
  out << "\"backend_kind\":" << Quote(BackendKindToString(caps.backend_kind)) << ',';
  out << "\"available\":" << Bool(caps.available) << ',';
  out << "\"hardware\":" << Bool(caps.hardware) << ',';
  out << "\"cpu\":" << Bool(caps.cpu) << ',';
  out << "\"d3d12\":" << Bool(caps.d3d12) << ',';
  out << "\"compute\":" << Bool(caps.compute) << ',';
  out << "\"ray_tracing\":" << Bool(caps.ray_tracing) << ',';
  out << "\"presentation\":" << Bool(caps.presentation) << ',';
  out << "\"warp\":" << Bool(caps.warp) << ',';
  out << "\"unified_memory\":" << Bool(caps.unified_memory) << ',';
  out << "\"cache_coherent_uma\":" << Bool(caps.cache_coherent_uma) << ',';
  out << "\"selected_by_default\":" << Bool(caps.selected_by_default) << ',';
  out << "\"node_count\":" << caps.node_count << ',';
  out << "\"vendor_id\":" << caps.vendor_id << ',';
  out << "\"device_id\":" << caps.device_id << ',';
  out << "\"dedicated_video_memory_bytes\":" << caps.dedicated_video_memory_bytes << ',';
  out << "\"shared_system_memory_bytes\":" << caps.shared_system_memory_bytes << ',';
  out << "\"current_budget_bytes\":" << caps.current_budget_bytes << ',';
  out << "\"current_usage_bytes\":" << caps.current_usage_bytes << ',';
  out << "\"estimated_rays_per_ms\":" << caps.estimated_rays_per_ms << ',';
  out << "\"adapter_luid\":" << Quote(caps.adapter_luid) << ',';
  out << "\"notes\":" << Quote(caps.notes) << ',';
  out << "\"backend_capabilities\":" << SerializeBackendCapabilities(caps.backend_caps);
  out << '}';
  return out.str();
}

std::string SerializeRayBudgetRequest(const RayBudgetRequest& request) {
  std::ostringstream out;
  out << '{';
  out << "\"polygon_frame_budget_ms\":" << request.polygon_frame_budget_ms << ',';
  out << "\"reserved_polygon_ms\":" << request.reserved_polygon_ms << ',';
  out << "\"merge_budget_ms\":" << request.merge_budget_ms << ',';
  out << "\"width\":" << request.width << ',';
  out << "\"height\":" << request.height << ',';
  out << "\"max_bounces\":" << request.max_bounces << ',';
  out << "\"min_rays_per_batch\":" << request.min_rays_per_batch << ',';
  out << "\"include_cpu\":" << Bool(request.include_cpu) << ',';
  out << "\"include_integrated_gpu\":" << Bool(request.include_integrated_gpu) << ',';
  out << "\"include_warp\":" << Bool(request.include_warp) << ',';
  out << "\"require_ray_tracing\":" << Bool(request.require_ray_tracing);
  out << '}';
  return out.str();
}

std::string SerializeRayBudgetAssignment(const RayBudgetAssignment& assignment) {
  std::ostringstream out;
  out << '{';
  out << "\"accelerator_id\":" << Quote(assignment.accelerator_id) << ',';
  out << "\"accelerator_name\":" << Quote(assignment.accelerator_name) << ',';
  out << "\"accelerator_kind\":" << Quote(AcceleratorKindToString(assignment.accelerator_kind)) << ',';
  out << "\"backend_kind\":" << Quote(BackendKindToString(assignment.backend_kind)) << ',';
  out << "\"backend_name\":" << Quote(assignment.backend_name) << ',';
  out << "\"active\":" << Bool(assignment.active) << ',';
  out << "\"uses_dxr\":" << Bool(assignment.uses_dxr) << ',';
  out << "\"worker_threads\":" << assignment.worker_threads << ',';
  out << "\"target_rays\":" << assignment.target_rays << ',';
  out << "\"budget_ms\":" << assignment.budget_ms << ',';
  out << "\"estimated_rays_per_ms\":" << assignment.estimated_rays_per_ms << ',';
  out << "\"reason\":" << Quote(assignment.reason);
  out << '}';
  return out.str();
}

std::string SerializeRayBudgetPlan(const RayBudgetPlan& plan) {
  std::ostringstream out;
  out << '{';
  out << "\"polygon_frame_budget_ms\":" << plan.polygon_frame_budget_ms << ',';
  out << "\"reserved_polygon_ms\":" << plan.reserved_polygon_ms << ',';
  out << "\"merge_budget_ms\":" << plan.merge_budget_ms << ',';
  out << "\"ray_budget_ms\":" << plan.ray_budget_ms << ',';
  out << "\"width\":" << plan.width << ',';
  out << "\"height\":" << plan.height << ',';
  out << "\"total_target_rays\":" << plan.total_target_rays << ',';
  out << "\"estimated_samples_per_pixel\":" << plan.estimated_samples_per_pixel << ',';
  out << "\"diagnostics\":" << SerializeStringArray(plan.diagnostics) << ',';
  out << "\"assignments\":[";
  for (std::size_t i = 0; i < plan.assignments.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeRayBudgetAssignment(plan.assignments[i]);
  }
  out << "]}";
  return out.str();
}

std::string MakeShaderManifestHash(std::string_view source_text) {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffset;
  for (unsigned char ch : source_text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kPrime;
  }
  const char hex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 8u; ++i) {
    const auto byte = static_cast<unsigned int>((hash >> (i * 8u)) & 0xffu);
    out[(i * 2u)] = hex[(byte >> 4u) & 0xfu];
    out[(i * 2u) + 1u] = hex[byte & 0xfu];
  }
  return out;
}

std::string BuildShaderCacheKey(std::string_view backend, const ShaderManifest& manifest) {
  std::string key;
  key.reserve(192);
  key += std::string(backend);
  key += '|';
  key += manifest.shader_family;
  key += '|';
  key += manifest.entry_point;
  key += "|fmt:";
  key += std::string(ShaderSourceFormatToString(manifest.source_format));
  key += "|path:";
  key += manifest.source_path;
  key += "|src:";
  key += manifest.source_hash;
  key += "|var:";
  key += manifest.variant_hash;
  key += '|';
  for (const auto& define : manifest.defines) {
    key += define;
    key += ',';
  }
  key += '|';
  for (const auto& feature : manifest.feature_flags) {
    key += feature;
    key += ',';
  }
  key += '|';
  for (const auto& flag : manifest.compiler_flags) {
    key += flag;
    key += ',';
  }
  key += "|rl:";
  key += std::to_string(manifest.resource_layout_version);
  return key;
}

std::string BuildShaderManifestDumpPath(std::string_view dump_root, const ShaderManifest& manifest) {
  const auto backend = SanitizePathSegment(manifest.backend.empty() ? "unknown" : manifest.backend, "unknown");
  const auto family = SanitizePathSegment(manifest.shader_family.empty() ? "shader" : manifest.shader_family, "shader");
  const auto entry = SanitizePathSegment(manifest.entry_point.empty() ? "entry" : manifest.entry_point, "entry");
  const auto variant = SanitizePathSegment(
      manifest.variant_hash.empty() ? (manifest.source_hash.empty() ? "default" : manifest.source_hash) : manifest.variant_hash,
      "default");
  const std::string file = backend + "_" + family + "_" + entry + "_" + variant + ".shader_manifest.json";
  if (dump_root.empty()) {
    return file;
  }
  std::string root(dump_root);
  while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
    root.pop_back();
  }
  return root.empty() ? file : (root + "/" + file);
}

std::string SerializeShaderManifest(const ShaderManifest& manifest) {
  std::ostringstream out;
  out << '{';
  out << "\"shader_family\":" << Quote(manifest.shader_family) << ',';
  out << "\"entry_point\":" << Quote(manifest.entry_point) << ',';
  out << "\"backend\":" << Quote(manifest.backend) << ',';
  out << "\"source_format\":" << Quote(ShaderSourceFormatToString(manifest.source_format)) << ',';
  out << "\"source_path\":" << Quote(manifest.source_path) << ',';
  out << "\"source_hash\":" << Quote(manifest.source_hash) << ',';
  out << "\"variant_hash\":" << Quote(manifest.variant_hash) << ',';
  out << "\"resource_layout_version\":" << manifest.resource_layout_version << ',';
  out << "\"artifact_path\":" << Quote(manifest.artifact_path) << ',';
  out << "\"cache_key\":" << Quote(manifest.cache_key) << ',';
  out << "\"manifest_dump_path\":" << Quote(manifest.manifest_dump_path) << ',';
  out << "\"diagnostics_path\":" << Quote(manifest.diagnostics_path) << ',';
  out << "\"compile_success\":" << Bool(manifest.compile_success) << ',';
  out << "\"validation_success\":" << Bool(manifest.validation_success) << ',';
  out << "\"compile_diagnostics\":" << Quote(manifest.compile_diagnostics) << ',';
  out << "\"validation_diagnostics\":" << Quote(manifest.validation_diagnostics) << ',';
  out << "\"defines\":" << SerializeStringArray(manifest.defines) << ',';
  out << "\"feature_flags\":" << SerializeStringArray(manifest.feature_flags) << ',';
  out << "\"compiler_flags\":" << SerializeStringArray(manifest.compiler_flags);
  out << '}';
  return out.str();
}

std::string SerializeCachedManifest(const CachedManifest& manifest) {
  std::ostringstream out;
  out << '{';
  out << "\"backend\":" << Quote(manifest.backend) << ',';
  out << "\"cache_key\":" << Quote(manifest.cache_key) << ',';
  out << "\"artifact_path\":" << Quote(manifest.artifact_path) << ',';
  out << "\"manifest_dump_path\":" << Quote(manifest.manifest_dump_path) << ',';
  out << "\"compile_success\":" << Bool(manifest.compile_success) << ',';
  out << "\"manifest_text\":" << Quote(manifest.manifest_text);
  out << '}';
  return out.str();
}

std::string SerializeShaderCacheDump(const std::vector<CachedManifest>& manifests) {
  std::ostringstream out;
  out << "{\"manifests\":[";
  for (std::size_t i = 0; i < manifests.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeCachedManifest(manifests[i]);
  }
  out << "]}";
  return out.str();
}

RenderCrashState BuildRendererCrashState(const IRenderBackend& backend,
                                        uint64_t frame_index,
                                        const std::string& frame_stage,
                                        const std::string& last_pass,
                                        const std::string& last_shader_variant,
                                        const std::string& extra_error) {
  RenderCrashState out;
  out.selected_backend = BackendKindToString(backend.kind());
  out.device_name = backend.name();
  out.capabilities = backend.capabilities();
  if (const auto* registry = backend.resource_registry()) {
    out.live_resources = registry->snapshot();
  }
  out.last_frame_index = frame_index;
  out.last_frame_stage = frame_stage;
  out.last_pass_name = last_pass;
  out.last_shader_variant = last_shader_variant;
  out.last_error = extra_error.empty() ? backend.last_error() : extra_error;
  return out;
}

std::string SerializeRenderCrashState(const RenderCrashState& state) {
  std::ostringstream out;
  out << '{';
  out << "\"selected_backend\":" << Quote(state.selected_backend) << ',';
  out << "\"device_name\":" << Quote(state.device_name) << ',';
  out << "\"last_frame_index\":" << state.last_frame_index << ',';
  out << "\"last_frame_stage\":" << Quote(state.last_frame_stage) << ',';
  out << "\"last_pass_name\":" << Quote(state.last_pass_name) << ',';
  out << "\"last_shader_variant\":" << Quote(state.last_shader_variant) << ',';
  out << "\"last_error\":" << Quote(state.last_error) << ',';
  out << "\"capabilities\":" << SerializeBackendCapabilities(state.capabilities) << ',';
  out << "\"live_resources\":[";
  for (std::size_t i = 0; i < state.live_resources.size(); ++i) {
    const auto& resource = state.live_resources[i];
    if (i > 0u) {
      out << ',';
    }
    out << '{';
    out << "\"handle\":" << resource.handle << ',';
    out << "\"label\":" << Quote(resource.label) << ',';
    out << "\"kind\":" << Quote(resource.kind) << ',';
    out << "\"size_bytes\":" << resource.size_bytes << ',';
    out << "\"ref_count\":" << resource.ref_count << ',';
    out << "\"acquired_frame\":" << resource.acquired_frame << ',';
    out << "\"last_access_frame\":" << resource.last_access_frame << ',';
    out << "\"versions\":" << resource.versions;
    out << '}';
  }
  out << "]}";
  return out.str();
}

std::string SerializeFrameContext(const FrameContext& context) {
  std::ostringstream out;
  out << '{';
  out << "\"frame_index\":" << context.frame_index << ',';
  out << "\"delta_time_seconds\":" << context.delta_time_seconds << ',';
  out << "\"elapsed_time_seconds\":" << context.elapsed_time_seconds << ',';
  out << "\"viewport_width\":" << context.viewport_width << ',';
  out << "\"viewport_height\":" << context.viewport_height << ',';
  out << "\"swapchain_width\":" << context.swapchain_width << ',';
  out << "\"swapchain_height\":" << context.swapchain_height << ',';
  out << "\"color_target\":" << context.color_target << ',';
  out << "\"depth_target\":" << context.depth_target << ',';
  out << "\"output_target\":" << context.output_target << ',';
  out << "\"readback_target\":" << context.readback_target << ',';
  out << "\"allow_gpu_timestamps\":" << Bool(context.allow_gpu_timestamps) << ',';
  out << "\"capture_debug_markers\":" << Bool(context.capture_debug_markers) << ',';
  out << "\"present\":" << Bool(context.present) << ',';
  out << "\"debug_label\":" << Quote(context.debug_label);
  out << '}';
  return out.str();
}

std::string SerializeFrameGraphDesc(const FrameGraphDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"debug_label\":" << Quote(desc.debug_label) << ',';
  out << "\"target_width\":" << desc.target_width << ',';
  out << "\"target_height\":" << desc.target_height << ',';
  out << "\"validate_hazards\":" << Bool(desc.validate_hazards) << ',';
  out << "\"transient_buffers\":[";
  for (std::size_t i = 0; i < desc.transient_buffers.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeBufferDesc(desc.transient_buffers[i]);
  }
  out << "],\"transient_textures\":[";
  for (std::size_t i = 0; i < desc.transient_textures.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeTextureDesc(desc.transient_textures[i]);
  }
  out << "],\"passes\":[";
  for (std::size_t i = 0; i < desc.passes.size(); ++i) {
    const auto& pass = desc.passes[i];
    if (i > 0u) {
      out << ',';
    }
    out << '{';
    out << "\"id\":" << pass.id << ',';
    out << "\"name\":" << Quote(pass.name) << ',';
    out << "\"type\":" << Quote(PassTypeToString(pass.type)) << ',';
    out << "\"reads\":" << SerializeHandleArray(pass.reads) << ',';
    out << "\"writes\":" << SerializeHandleArray(pass.writes);
    out << '}';
  }
  out << "],\"dependencies\":[";
  for (std::size_t i = 0; i < desc.dependencies.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << "{\"from\":" << desc.dependencies[i].first << ",\"to\":" << desc.dependencies[i].second << '}';
  }
  out << "]}";
  return out.str();
}

std::string SerializeBackendCandidateDesc(const BackendCandidateDesc& desc) {
  std::ostringstream out;
  out << '{';
  out << "\"name\":" << Quote(desc.name) << ',';
  out << "\"kind\":" << Quote(BackendKindToString(desc.kind)) << ',';
  out << "\"selection_priority\":" << desc.selection_priority << ',';
  out << "\"compiled\":" << Bool(desc.compiled) << ',';
  out << "\"available\":" << Bool(desc.available) << ',';
  out << "\"adapter_skeleton\":" << Bool(desc.adapter_skeleton) << ',';
  out << "\"experimental\":" << Bool(desc.experimental) << ',';
  out << "\"supports_compute\":" << Bool(desc.supports_compute) << ',';
  out << "\"supports_presentation\":" << Bool(desc.supports_presentation) << ',';
  out << "\"supports_ray_tracing\":" << Bool(desc.supports_ray_tracing) << ',';
  out << "\"unavailable_reason\":" << Quote(desc.unavailable_reason);
  out << '}';
  return out.str();
}

std::string SerializeBackendSelectionDecision(const BackendSelectionDecision& decision) {
  std::ostringstream out;
  out << '{';
  out << "\"selected\":" << Bool(decision.selected) << ',';
  out << "\"selected_backend\":" << Quote(decision.selected_backend) << ',';
  out << "\"selected_kind\":" << Quote(BackendKindToString(decision.selected_kind)) << ',';
  out << "\"source\":" << Quote(BackendSelectionSourceToString(decision.source)) << ',';
  out << "\"reason\":" << Quote(decision.reason) << ',';
  out << "\"diagnostics\":" << SerializeStringArray(decision.diagnostics) << ',';
  out << "\"candidates\":[";
  for (std::size_t i = 0; i < decision.candidates.size(); ++i) {
    if (i > 0u) {
      out << ',';
    }
    out << SerializeBackendCandidateDesc(decision.candidates[i]);
  }
  out << "]}";
  return out.str();
}

void AppendDiagnosticField(std::vector<std::string>& diagnostics, const std::string& entry) {
  diagnostics.push_back(entry);
}

}  // namespace vkpt::render
