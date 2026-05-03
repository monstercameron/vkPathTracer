#include "render/interface/RenderContracts.h"

#include <iomanip>
#include <sstream>

namespace vkpt::render {

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

std::string SerializeBackendCapabilities(const RenderBackendCapabilities& caps) {
  std::ostringstream out;
  out << '{';
  out << "\"backend_name\":\"" << caps.backend_name << "\",";
  out << "\"compute\":" << (caps.compute ? "true" : "false") << ',';
  out << "\"storage_buffers\":" << (caps.storage_buffers ? "true" : "false") << ',';
  out << "\"storage_textures\":" << (caps.storage_textures ? "true" : "false") << ',';
  out << "\"timestamp_queries\":" << (caps.timestamp_queries ? "true" : "false") << ',';
  out << "\"timestamp_fallback_reason\":\"" << caps.timestamp_fallback_reason << "\",";
  out << "\"subgroups\":" << (caps.subgroups ? "true" : "false") << ',';
  out << "\"descriptor_indexing\":" << (caps.descriptor_indexing ? "true" : "false") << ',';
  out << "\"bindless_like_resources\":" << (caps.bindless_like_resources ? "true" : "false") << ',';
  out << "\"ray_tracing\":" << (caps.ray_tracing ? "true" : "false") << ',';
  out << "\"ray_query\":" << (caps.ray_query ? "true" : "false") << ',';
  out << "\"ray_query_supported\":" << (caps.ray_query_supported ? "true" : "false") << ',';
  out << "\"acceleration_structure_supported\":" << (caps.acceleration_structure_supported ? "true" : "false") << ',';
  out << "\"shader_group_handle_size\":" << caps.shader_group_handle_size << ',';
  out << "\"max_as_size\":" << caps.max_as_size << ',';
  out << "\"presentation\":" << (caps.presentation ? "true" : "false") << ',';
  out << "\"readback\":" << (caps.readback ? "true" : "false") << ',';
  out << "\"is_simulated\":" << (caps.is_simulated ? "true" : "false") << ',';
  out << "\"supports_present\":" << (caps.supports_present ? "true" : "false") << ',';
  out << "\"supports_multiqueue\":" << (caps.supports_multiqueue ? "true" : "false") << ',';
  out << "\"max_workgroup_size_x\":" << caps.max_workgroup_size_x << ',';
  out << "\"max_workgroup_size_y\":" << caps.max_workgroup_size_y << ',';
  out << "\"max_workgroup_size_z\":" << caps.max_workgroup_size_z << ',';
  out << "\"max_buffer_alignment\":" << caps.max_buffer_alignment << ',';
  out << "\"memory_model\":\"" << caps.memory_model << "\",";
  out << "\"notes\":\"" << caps.notes << "\"}";
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
    const auto nibble = static_cast<unsigned int>((hash >> (i * 8u)) & 0xffu);
    out[(i * 2u)] = hex[(nibble >> 4u) & 0xfu];
    out[(i * 2u) + 1u] = hex[nibble & 0xfu];
  }
  return out;
}

std::string BuildShaderCacheKey(std::string_view backend, const ShaderManifest& manifest) {
  std::string key;
  key.reserve(128);
  key += std::string(backend);
  key += "|";
  key += manifest.shader_family;
  key += "|";
  key += manifest.entry_point;
  key += "|src:";
  key += manifest.source_hash;
  key += "|var:";
  key += manifest.variant_hash;
  key += "|";
  for (const auto& define : manifest.defines) {
    key += define;
    key += ",";
  }
  key += "|";
  for (const auto& feature : manifest.feature_flags) {
    key += feature;
    key += ",";
  }
  key += "|";
  for (const auto& flag : manifest.compiler_flags) {
    key += flag;
    key += ",";
  }
  key += "|rl:";
  key += std::to_string(manifest.resource_layout_version);
  return key;
}

std::string SerializeShaderManifest(const ShaderManifest& manifest) {
  std::ostringstream out;
  out << "{";
  out << "\"shader_family\":\"" << manifest.shader_family << "\",";
  out << "\"entry_point\":\"" << manifest.entry_point << "\",";
  out << "\"backend\":\"" << manifest.backend << "\",";
  out << "\"source_hash\":\"" << manifest.source_hash << "\",";
  out << "\"variant_hash\":\"" << manifest.variant_hash << "\",";
  out << "\"resource_layout_version\":" << manifest.resource_layout_version << ",";
  out << "\"artifact_path\":\"" << manifest.artifact_path << "\",";
  out << "\"compile_success\":" << (manifest.compile_success ? "true" : "false") << ",";
  out << "\"compile_diagnostics\":\"" << manifest.compile_diagnostics << "\",";
  out << "\"defines\":[";
  for (std::size_t i = 0; i < manifest.defines.size(); ++i) {
    if (i > 0u) {
      out << ",";
    }
    out << "\"" << manifest.defines[i] << "\"";
  }
  out << "],\"feature_flags\":[";
  for (std::size_t i = 0; i < manifest.feature_flags.size(); ++i) {
    if (i > 0u) {
      out << ",";
    }
    out << "\"" << manifest.feature_flags[i] << "\"";
  }
  out << "],\"compiler_flags\":[";
  for (std::size_t i = 0; i < manifest.compiler_flags.size(); ++i) {
    if (i > 0u) {
      out << ",";
    }
    out << "\"" << manifest.compiler_flags[i] << "\"";
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
  out << "{";
  out << "\"selected_backend\":\"" << state.selected_backend << "\",";
  out << "\"device_name\":\"" << state.device_name << "\",";
  out << "\"last_frame_index\":" << state.last_frame_index << ",";
  out << "\"last_frame_stage\":\"" << state.last_frame_stage << "\",";
  out << "\"last_pass_name\":\"" << state.last_pass_name << "\",";
  out << "\"last_shader_variant\":\"" << state.last_shader_variant << "\",";
  out << "\"last_error\":\"" << state.last_error << "\",";
  out << "\"capabilities\":" << SerializeBackendCapabilities(state.capabilities) << ",";
  out << "\"live_resources\":[";
  for (std::size_t i = 0; i < state.live_resources.size(); ++i) {
    const auto& resource = state.live_resources[i];
    if (i) {
      out << ",";
    }
    out << "{";
    out << "\"handle\":" << resource.handle << ",";
    out << "\"label\":\"" << resource.label << "\",";
    out << "\"kind\":\"" << resource.kind << "\",";
    out << "\"size_bytes\":" << resource.size_bytes << ",";
    out << "\"ref_count\":" << resource.ref_count << ",";
    out << "\"acquired_frame\":" << resource.acquired_frame << ",";
    out << "\"last_access_frame\":" << resource.last_access_frame << ",";
    out << "\"versions\":" << resource.versions;
    out << "}";
  }
  out << "]}";
  return out.str();
}

void AppendDiagnosticField(std::vector<std::string>& diagnostics, const std::string& entry) {
  diagnostics.push_back(entry);
}

}  // namespace vkpt::render
