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
  out << "\"subgroups\":" << (caps.subgroups ? "true" : "false") << ',';
  out << "\"descriptor_indexing\":" << (caps.descriptor_indexing ? "true" : "false") << ',';
  out << "\"bindless_like_resources\":" << (caps.bindless_like_resources ? "true" : "false") << ',';
  out << "\"ray_tracing\":" << (caps.ray_tracing ? "true" : "false") << ',';
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

void AppendDiagnosticField(std::vector<std::string>& diagnostics, const std::string& entry) {
  diagnostics.push_back(entry);
}

}  // namespace vkpt::render

