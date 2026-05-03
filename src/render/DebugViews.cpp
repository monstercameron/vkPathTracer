#include "render/DebugViews.h"

#include <algorithm>
#include <sstream>

namespace vkpt::render {

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

const std::vector<DebugViewDescriptor>& GetDebugViewRegistry() {
  static const std::vector<DebugViewDescriptor> s_registry = {
    {DebugViewId::Beauty,            "beauty",             "Beauty",              true,  DebugBackendRequirement::Any,     "Full path-traced image"},
    {DebugViewId::Albedo,            "albedo",             "Albedo",              false, DebugBackendRequirement::CpuOnly, "Surface albedo at first hit"},
    {DebugViewId::Normals,           "normals",            "Normals",             true,  DebugBackendRequirement::CpuOnly, "World-space shading normal as RGB"},
    {DebugViewId::Depth,             "depth",              "Depth",               true,  DebugBackendRequirement::CpuOnly, "Linear depth from camera"},
    {DebugViewId::WorldPosition,     "world_position",     "World Position",      false, DebugBackendRequirement::CpuOnly, "World-space hit position"},
    {DebugViewId::MaterialId,        "material_id",        "Material ID",         false, DebugBackendRequirement::CpuOnly, "Material index as false-colour"},
    {DebugViewId::ObjectId,          "object_id",          "Object ID",           false, DebugBackendRequirement::CpuOnly, "Instance index as false-colour"},
    {DebugViewId::UV,                "uv",                 "UV",                  false, DebugBackendRequirement::CpuOnly, "Texture coordinates at first hit"},
    {DebugViewId::Roughness,         "roughness",          "Roughness",           false, DebugBackendRequirement::CpuOnly, "Material roughness value"},
    {DebugViewId::Metallic,          "metallic",           "Metallic",            false, DebugBackendRequirement::CpuOnly, "Material metallic value"},
    {DebugViewId::Emission,          "emission",           "Emission",            true,  DebugBackendRequirement::CpuOnly, "Emissive radiance contribution"},
    {DebugViewId::Throughput,        "throughput",         "Throughput",          true,  DebugBackendRequirement::CpuOnly, "Path throughput at each bounce"},
    {DebugViewId::SampleCount,       "sample_count",       "Sample Count",        true,  DebugBackendRequirement::Any,     "Number of valid samples per pixel"},
    {DebugViewId::Variance,          "variance",           "Variance",            false, DebugBackendRequirement::CpuOnly, "Per-pixel luminance variance"},
    {DebugViewId::BvhDepth,          "bvh_depth",          "BVH Depth",           false, DebugBackendRequirement::CpuOnly, "BVH traversal depth"},
    {DebugViewId::SdfDistance,       "sdf_distance",       "SDF Distance",        false, DebugBackendRequirement::CpuOnly, "Signed distance at hit point"},
    {DebugViewId::LightContribution, "light_contribution", "Light Contribution",  false, DebugBackendRequirement::CpuOnly, "NEE direct light contribution"},
    {DebugViewId::DenoisedOutput,    "denoised_output",    "Denoised Output",     false, DebugBackendRequirement::Any,     "Post-denoising buffer"},
    {DebugViewId::DifferenceHeatmap, "difference_heatmap", "Difference Heatmap",  false, DebugBackendRequirement::Any,     "Error map vs reference image"},
    {DebugViewId::NanInfHighlight,   "nan_inf_highlight",  "NaN/Inf Highlight",   true,  DebugBackendRequirement::Any,     "Flags non-finite samples as magenta"},
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
    out << "\"available\":" << (d.available ? "true" : "false") << ",";
    out << "\"backend_requirement\":\"" << ToString(d.backend_requirement) << "\",";
    out << "\"notes\":\"" << d.notes << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

}  // namespace vkpt::render
