#include "render/DebugViews.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace vkpt::render {

namespace {

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
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
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string CommandIdFor(const DebugViewDescriptor& descriptor) {
  if (!descriptor.command_id.empty()) {
    return descriptor.command_id;
  }
  return "render.debug_view." + descriptor.view_id;
}

}  // namespace

const char* ToString(DebugViewId id) {
  switch (id) {
    case DebugViewId::Beauty:            return "Beauty";
    case DebugViewId::Albedo:            return "Albedo";
    case DebugViewId::Normals:           return "Normals";
    case DebugViewId::Depth:             return "Depth";
    case DebugViewId::WorldPosition:     return "WorldPosition";
    case DebugViewId::MaterialId:        return "MaterialId";
    case DebugViewId::ObjectId:          return "ObjectId";
    case DebugViewId::UV:                return "UV";
    case DebugViewId::Roughness:         return "Roughness";
    case DebugViewId::Metallic:          return "Metallic";
    case DebugViewId::Emission:          return "Emission";
    case DebugViewId::Throughput:        return "Throughput";
    case DebugViewId::SampleCount:       return "SampleCount";
    case DebugViewId::Variance:          return "Variance";
    case DebugViewId::BvhDepth:          return "BvhDepth";
    case DebugViewId::SdfDistance:       return "SdfDistance";
    case DebugViewId::LightContribution: return "LightContribution";
    case DebugViewId::DenoisedOutput:    return "DenoisedOutput";
    case DebugViewId::DifferenceHeatmap: return "DifferenceHeatmap";
    case DebugViewId::NanInfHighlight:   return "NanInfHighlight";
    default:                             return "Unknown";
  }
}

const char* ToString(DebugBackendRequirement req) {
  switch (req) {
    case DebugBackendRequirement::Any:     return "any";
    case DebugBackendRequirement::CpuOnly: return "cpu_only";
    case DebugBackendRequirement::GpuOnly: return "gpu_only";
    case DebugBackendRequirement::RtOnly:  return "rt_only";
    default:                               return "any";
  }
}

const char* ToString(DebugViewChannel channel) {
  switch (channel) {
    case DebugViewChannel::Beauty:       return "beauty";
    case DebugViewChannel::Geometry:     return "geometry";
    case DebugViewChannel::Material:     return "material";
    case DebugViewChannel::PathState:    return "path_state";
    case DebugViewChannel::Acceleration: return "acceleration";
    case DebugViewChannel::Sdf:          return "sdf";
    case DebugViewChannel::Lighting:     return "lighting";
    case DebugViewChannel::PostProcess:  return "post_process";
    case DebugViewChannel::Validation:   return "validation";
    default:                             return "beauty";
  }
}

const std::vector<DebugViewDescriptor>& GetDebugViewRegistry() {
  static const std::vector<DebugViewDescriptor> s_registry = {
    {DebugViewId::Beauty,            "beauty",             "Beauty",              true,  DebugBackendRequirement::Any,     "Full path-traced image", true, DebugViewChannel::Beauty,       false, "render.debug_view.beauty",             "beauty"},
    {DebugViewId::Albedo,            "albedo",             "Albedo",              false, DebugBackendRequirement::CpuOnly, "Surface albedo at first hit", true, DebugViewChannel::Material,     false, "render.debug_view.albedo",             "debug.albedo"},
    {DebugViewId::Normals,           "normals",            "Normals",             true,  DebugBackendRequirement::CpuOnly, "World-space shading normal as RGB", true, DebugViewChannel::Geometry,     false, "render.debug_view.normals",            "debug.normals"},
    {DebugViewId::Depth,             "depth",              "Depth",               true,  DebugBackendRequirement::CpuOnly, "Linear depth from camera", true, DebugViewChannel::Geometry,     false, "render.debug_view.depth",              "debug.depth"},
    {DebugViewId::WorldPosition,     "world_position",     "World Position",      false, DebugBackendRequirement::CpuOnly, "World-space hit position", true, DebugViewChannel::Geometry,     false, "render.debug_view.world_position",     "debug.world_position"},
    {DebugViewId::MaterialId,        "material_id",        "Material ID",         false, DebugBackendRequirement::CpuOnly, "Material index as false-color", true, DebugViewChannel::Material,     false, "render.debug_view.material_id",        "debug.material_id"},
    {DebugViewId::ObjectId,          "object_id",          "Object ID",           false, DebugBackendRequirement::CpuOnly, "Instance index as false-color", true, DebugViewChannel::Geometry,     false, "render.debug_view.object_id",          "debug.object_id"},
    {DebugViewId::UV,                "uv",                 "UV",                  false, DebugBackendRequirement::CpuOnly, "Texture coordinates at first hit", true, DebugViewChannel::Geometry,     false, "render.debug_view.uv",                 "debug.uv"},
    {DebugViewId::Roughness,         "roughness",          "Roughness",           false, DebugBackendRequirement::CpuOnly, "Material roughness value", true, DebugViewChannel::Material,     false, "render.debug_view.roughness",          "debug.roughness"},
    {DebugViewId::Metallic,          "metallic",           "Metallic",            false, DebugBackendRequirement::CpuOnly, "Material metallic value", true, DebugViewChannel::Material,     false, "render.debug_view.metallic",           "debug.metallic"},
    {DebugViewId::Emission,          "emission",           "Emission",            true,  DebugBackendRequirement::CpuOnly, "Emissive radiance contribution", true, DebugViewChannel::Material,     false, "render.debug_view.emission",           "debug.emission"},
    {DebugViewId::Throughput,        "throughput",         "Throughput",          true,  DebugBackendRequirement::CpuOnly, "Path throughput at each bounce", true, DebugViewChannel::PathState,    false, "render.debug_view.throughput",         "debug.throughput"},
    {DebugViewId::SampleCount,       "sample_count",       "Sample Count",        true,  DebugBackendRequirement::Any,     "Number of valid samples per pixel", true, DebugViewChannel::PathState,    false, "render.debug_view.sample_count",       "debug.sample_count"},
    {DebugViewId::Variance,          "variance",           "Variance",            false, DebugBackendRequirement::CpuOnly, "Per-pixel luminance variance", true, DebugViewChannel::PathState,    false, "render.debug_view.variance",           "debug.variance"},
    {DebugViewId::BvhDepth,          "bvh_depth",          "BVH Depth",           false, DebugBackendRequirement::CpuOnly, "BVH traversal depth", true, DebugViewChannel::Acceleration, false, "render.debug_view.bvh_depth",          "debug.bvh_depth"},
    {DebugViewId::SdfDistance,       "sdf_distance",       "SDF Distance",        false, DebugBackendRequirement::CpuOnly, "Signed distance at hit point", true, DebugViewChannel::Sdf,          false, "render.debug_view.sdf_distance",       "debug.sdf_distance"},
    {DebugViewId::LightContribution, "light_contribution", "Light Contribution",  false, DebugBackendRequirement::CpuOnly, "Next-event direct light contribution", true, DebugViewChannel::Lighting,     false, "render.debug_view.light_contribution", "debug.light_contribution"},
    {DebugViewId::DenoisedOutput,    "denoised_output",    "Denoised Output",     false, DebugBackendRequirement::Any,     "Post-denoising buffer", true, DebugViewChannel::PostProcess,  false, "render.debug_view.denoised_output",    "debug.denoised_output"},
    {DebugViewId::DifferenceHeatmap, "difference_heatmap", "Difference Heatmap",  false, DebugBackendRequirement::Any,     "Error map vs reference image", true, DebugViewChannel::Validation,   false, "render.debug_view.difference_heatmap", "debug.difference_heatmap"},
    {DebugViewId::NanInfHighlight,   "nan_inf_highlight",  "NaN/Inf Highlight",   true,  DebugBackendRequirement::Any,     "Flags non-finite samples as magenta", true, DebugViewChannel::Validation,   false, "render.debug_view.nan_inf_highlight",  "debug.nan_inf_highlight"},
  };
  return s_registry;
}

bool IsDebugViewAvailable(DebugViewId id) {
  for (const auto& d : GetDebugViewRegistry()) {
    if (d.id == id) return d.available;
  }
  return false;
}

const DebugViewDescriptor* FindDebugView(std::string_view view_id) {
  for (const auto& d : GetDebugViewRegistry()) {
    if (d.view_id == view_id) return &d;
  }
  return nullptr;
}

std::string MakeDebugViewCommandPayloadJson(const DebugViewDescriptor& descriptor) {
  std::ostringstream out;
  out << "{";
  out << "\"type\":\"SetDebugView\",";
  out << "\"command_id\":\"" << EscapeJson(CommandIdFor(descriptor)) << "\",";
  out << "\"view_id\":\"" << EscapeJson(descriptor.view_id) << "\",";
  out << "\"debug_view\":\"" << ToString(descriptor.id) << "\",";
  out << "\"output_buffer\":\"" << EscapeJson(descriptor.output_buffer) << "\",";
  out << "\"accumulation_reset_required\":"
      << (descriptor.accumulation_reset_required ? "true" : "false");
  out << "}";
  return out.str();
}

std::vector<DebugViewCommandDescriptor> BuildDebugViewCommandDescriptors() {
  std::vector<DebugViewCommandDescriptor> commands;
  const auto& registry = GetDebugViewRegistry();
  commands.reserve(registry.size());
  for (const auto& descriptor : registry) {
    DebugViewCommandDescriptor command;
    command.command_id = CommandIdFor(descriptor);
    command.debug_view = descriptor.id;
    command.view_id = descriptor.view_id;
    command.display_name = descriptor.display_name;
    command.enabled = descriptor.declared;
    command.undoable = false;
    command.accumulation_reset_required = descriptor.accumulation_reset_required;
    command.command_payload_json = MakeDebugViewCommandPayloadJson(descriptor);
    commands.push_back(std::move(command));
  }
  return commands;
}

const DebugViewCommandDescriptor* FindDebugViewCommand(std::string_view command_id) {
  static const std::vector<DebugViewCommandDescriptor> s_commands = BuildDebugViewCommandDescriptors();
  for (const auto& command : s_commands) {
    if (command.command_id == command_id) {
      return &command;
    }
  }
  return nullptr;
}

std::string SerializeDebugViewRegistry() {
  const auto& reg = GetDebugViewRegistry();
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < reg.size(); ++i) {
    const auto& d = reg[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"id\":\"" << ToString(d.id) << "\",";
    out << "\"view_id\":\"" << d.view_id << "\",";
    out << "\"display_name\":\"" << d.display_name << "\",";
    out << "\"declared\":" << (d.declared ? "true" : "false") << ",";
    out << "\"available\":" << (d.available ? "true" : "false") << ",";
    out << "\"backend_requirement\":\"" << ToString(d.backend_requirement) << "\",";
    out << "\"channel\":\"" << ToString(d.channel) << "\",";
    out << "\"command_id\":\"" << EscapeJson(CommandIdFor(d)) << "\",";
    out << "\"output_buffer\":\"" << EscapeJson(d.output_buffer) << "\",";
    out << "\"accumulation_reset_required\":" << (d.accumulation_reset_required ? "true" : "false") << ",";
    out << "\"notes\":\"" << EscapeJson(d.notes) << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

}  // namespace vkpt::render
