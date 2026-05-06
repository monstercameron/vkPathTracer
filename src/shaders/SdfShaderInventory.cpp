#include "shaders/SdfShaderInventory.h"

#include <sstream>

namespace vkpt::shaders {

const char* ToString(SdfFeature feature) {
  switch (feature) {
    case SdfFeature::Sphere:            return "Sphere";
    case SdfFeature::Box:               return "Box";
    case SdfFeature::RoundedBox:        return "RoundedBox";
    case SdfFeature::Cylinder:          return "Cylinder";
    case SdfFeature::Cone:              return "Cone";
    case SdfFeature::Frustum:           return "Frustum";
    case SdfFeature::Capsule:           return "Capsule";
    case SdfFeature::Ellipsoid:         return "Ellipsoid";
    case SdfFeature::Torus:             return "Torus";
    case SdfFeature::Disk:              return "Disk";
    case SdfFeature::Plane:             return "Plane";
    case SdfFeature::Triangle:          return "Triangle";
    case SdfFeature::Wedge:             return "Wedge";
    case SdfFeature::Prism:             return "Prism";
    case SdfFeature::Metaballs:         return "Metaballs";
    case SdfFeature::MandelbulbFractal: return "MandelbulbFractal";
    case SdfFeature::CsgUnion:          return "CsgUnion";
    case SdfFeature::CsgIntersection:   return "CsgIntersection";
    case SdfFeature::CsgSubtraction:    return "CsgSubtraction";
    case SdfFeature::SmoothBlend:       return "SmoothBlend";
    case SdfFeature::NoiseDeformation:  return "NoiseDeformation";
    default:                            return "Unknown";
  }
}

const std::vector<SdfFeatureDescriptor>& GetSdfFeatureInventory() {
  // Central SDF feature list for UI/reporting; "implemented" means callable by
  // the current path tracer, not merely planned in scene metadata.
  static const std::vector<SdfFeatureDescriptor> s_inventory = {
    {SdfFeature::Sphere,            "sphere",             "Sphere",             true,  "intersect_sphere() — analytic ray-sphere"},
    {SdfFeature::Box,               "box",                "Box",                true,  "intersect_box() — slab-test AABB"},
    {SdfFeature::RoundedBox,        "rounded_box",        "Rounded Box",        true,  "intersect_rounded_box() — chamfered AABB"},
    {SdfFeature::Cylinder,          "cylinder",           "Cylinder",           false, "Not yet implemented"},
    {SdfFeature::Cone,              "cone",               "Cone",               false, "Not yet implemented"},
    {SdfFeature::Frustum,           "frustum",            "Frustum",            false, "Not yet implemented"},
    {SdfFeature::Capsule,           "capsule",            "Capsule",            true,  "intersect_capsule() — segment + sphere end caps"},
    {SdfFeature::Ellipsoid,         "ellipsoid",          "Ellipsoid",          false, "Not yet implemented"},
    {SdfFeature::Torus,             "torus",              "Torus",              true,  "intersect_torus() — quartic intersection"},
    {SdfFeature::Disk,              "disk",               "Disk",               false, "Not yet implemented"},
    {SdfFeature::Plane,             "plane",              "Plane",              true,  "intersect_plane() — infinite half-space"},
    {SdfFeature::Triangle,          "triangle",           "Triangle",           false, "SDF triangle not yet; triangle mesh uses Möller–Trumbore"},
    {SdfFeature::Wedge,             "wedge",              "Wedge",              false, "Not yet implemented"},
    {SdfFeature::Prism,             "prism",              "Prism",              false, "Not yet implemented"},
    {SdfFeature::Metaballs,         "metaballs",          "Metaballs",          false, "Iso-surface marching not yet implemented"},
    {SdfFeature::MandelbulbFractal, "mandelbulb_fractal", "Mandelbulb Fractal", false, "Iterative SDF fractal not yet implemented"},
    {SdfFeature::CsgUnion,          "csg_union",          "CSG Union",          false, "min() combinator — not yet hooked to scene graph"},
    {SdfFeature::CsgIntersection,   "csg_intersection",   "CSG Intersection",   false, "max() combinator — not yet implemented"},
    {SdfFeature::CsgSubtraction,    "csg_subtraction",    "CSG Subtraction",    false, "max(a,-b) combinator — not yet implemented"},
    {SdfFeature::SmoothBlend,       "smooth_blend",       "Smooth Blend",       false, "smin() polynomial blend — not yet implemented"},
    {SdfFeature::NoiseDeformation,  "noise_deformation",  "Noise Deformation",  false, "Perlin/Simplex displacement — not yet implemented"},
  };
  return s_inventory;
}

std::string SerializeSdfFeatureInventory() {
  const auto& inventory = GetSdfFeatureInventory();
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < inventory.size(); ++i) {
    const auto& d = inventory[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"feature\":\"" << ToString(d.feature) << "\",";
    out << "\"id\":\"" << d.id << "\",";
    out << "\"display_name\":\"" << d.display_name << "\",";
    out << "\"implemented\":" << (d.implemented ? "true" : "false") << ",";
    out << "\"notes\":\"" << d.notes << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

}  // namespace vkpt::shaders
