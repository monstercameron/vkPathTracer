#include "shaders/ShaderFamilyManifest.h"

#include <sstream>

namespace vkpt::shaders {

const char* ToString(ShaderFamily family) {
  switch (family) {
    case ShaderFamily::RayGeneration:       return "RayGeneration";
    case ShaderFamily::CameraSampling:      return "CameraSampling";
    case ShaderFamily::BvhTraversal:        return "BvhTraversal";
    case ShaderFamily::SdfIntersection:     return "SdfIntersection";
    case ShaderFamily::TriangleIntersection:return "TriangleIntersection";
    case ShaderFamily::MaterialEvaluation:  return "MaterialEvaluation";
    case ShaderFamily::BsdfSampling:        return "BsdfSampling";
    case ShaderFamily::LightSampling:       return "LightSampling";
    case ShaderFamily::ShadowRayTesting:    return "ShadowRayTesting";
    case ShaderFamily::FilmAccumulation:    return "FilmAccumulation";
    case ShaderFamily::Resolve:             return "Resolve";
    case ShaderFamily::Denoise:             return "Denoise";
    case ShaderFamily::DebugVisualization:  return "DebugVisualization";
    case ShaderFamily::EditorOverlay:       return "EditorOverlay";
    default:                                return "Unknown";
  }
}

const char* ToString(ShaderStage stage) {
  switch (stage) {
    case ShaderStage::Compute:    return "Compute";
    case ShaderStage::RayGen:     return "RayGen";
    case ShaderStage::ClosestHit: return "ClosestHit";
    case ShaderStage::AnyHit:     return "AnyHit";
    case ShaderStage::Miss:       return "Miss";
    case ShaderStage::Callable:   return "Callable";
    case ShaderStage::Vertex:     return "Vertex";
    case ShaderStage::Fragment:   return "Fragment";
    default:                      return "Unknown";
  }
}

const std::vector<ShaderFamilyDescriptor>& GetShaderFamilyManifest() {
  static const std::vector<ShaderFamilyDescriptor> s_manifest = {
    {ShaderFamily::RayGeneration,        "ray_generation",        "Ray Generation",        ShaderStage::RayGen,     true,  "CPU scalar: main render loop generates primary rays"},
    {ShaderFamily::CameraSampling,       "camera_sampling",       "Camera Sampling",       ShaderStage::Compute,    true,  "CPU scalar: camera_rays() builds stratified samples"},
    {ShaderFamily::BvhTraversal,         "bvh_traversal",         "BVH Traversal",         ShaderStage::Compute,    true,  "CPU scalar: intersect_scene() traverses AABB BVH"},
    {ShaderFamily::SdfIntersection,      "sdf_intersection",      "SDF Intersection",      ShaderStage::Compute,    true,  "CPU scalar: sphere/box/capsule/torus/plane SDF march"},
    {ShaderFamily::TriangleIntersection, "triangle_intersection", "Triangle Intersection", ShaderStage::ClosestHit, true,  "CPU scalar: Möller–Trumbore per-triangle test"},
    {ShaderFamily::MaterialEvaluation,   "material_evaluation",   "Material Evaluation",   ShaderStage::ClosestHit, true,  "CPU scalar: evaluate_bsdf() Lambertian BRDF"},
    {ShaderFamily::BsdfSampling,         "bsdf_sampling",         "BSDF Sampling",         ShaderStage::Compute,    true,  "CPU scalar: cosine-weighted hemisphere sampling"},
    {ShaderFamily::LightSampling,        "light_sampling",        "Light Sampling",        ShaderStage::Compute,    false, "NEE light selection; not yet a stand-alone pipeline stage"},
    {ShaderFamily::ShadowRayTesting,     "shadow_ray_testing",    "Shadow Ray Testing",    ShaderStage::AnyHit,     false, "Shadow rays cast by NEE; piggy-backs intersect_scene"},
    {ShaderFamily::FilmAccumulation,     "film_accumulation",     "Film Accumulation",     ShaderStage::Compute,    true,  "CPU scalar: FilmBuffer::add_sample()"},
    {ShaderFamily::Resolve,              "resolve",               "Resolve",               ShaderStage::Compute,    true,  "CPU scalar: ApplyFilmResolve() tone-map + gamma"},
    {ShaderFamily::Denoise,              "denoise",               "Denoise",               ShaderStage::Compute,    true,  "D3D12 GPU spatial denoise with albedo/normal/depth guides"},
    {ShaderFamily::DebugVisualization,   "debug_visualization",   "Debug Visualization",   ShaderStage::Fragment,   false, "Not yet implemented"},
    {ShaderFamily::EditorOverlay,        "editor_overlay",        "Editor Overlay",        ShaderStage::Vertex,     false, "Not yet implemented"},
  };
  return s_manifest;
}

std::string SerializeShaderFamilyManifest() {
  const auto& manifest = GetShaderFamilyManifest();
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < manifest.size(); ++i) {
    const auto& d = manifest[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"family\":\"" << ToString(d.family) << "\",";
    out << "\"id\":\"" << d.id << "\",";
    out << "\"display_name\":\"" << d.display_name << "\",";
    out << "\"primary_stage\":\"" << ToString(d.primary_stage) << "\",";
    out << "\"implemented\":" << (d.implemented ? "true" : "false") << ",";
    out << "\"notes\":\"" << d.notes << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

}  // namespace vkpt::shaders
