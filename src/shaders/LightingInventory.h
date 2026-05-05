#pragma once

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::shaders {

enum class LightFeature : std::uint8_t {
  Point,
  Spot,
  Directional,
  SphereArea,
  RectangleArea,
  DiskArea,
  LineArea,
  Portal,
  MeshEmissive,
  EnvironmentSky,
  OpenSky,
  BlackbodyEmitter,
  VisibleEmissiveObject,
  Unknown,
};

struct LightParameterDesc {
  std::string name;
  std::string unit;
  std::string semantic;
  bool required = true;
};

struct LightFeatureDescriptor {
  LightFeature feature = LightFeature::Unknown;
  std::string id;
  std::string display_name;
  std::string scene_type;
  std::string intensity_unit;
  bool physically_based = true;
  bool importance_sampled = false;
  bool visible_to_camera = false;
  std::vector<LightParameterDesc> parameters;
  std::string notes;
};

inline const char* ToString(LightFeature feature) {
  switch (feature) {
    case LightFeature::Point:
      return "Point";
    case LightFeature::Spot:
      return "Spot";
    case LightFeature::Directional:
      return "Directional";
    case LightFeature::SphereArea:
      return "SphereArea";
    case LightFeature::RectangleArea:
      return "RectangleArea";
    case LightFeature::DiskArea:
      return "DiskArea";
    case LightFeature::LineArea:
      return "LineArea";
    case LightFeature::Portal:
      return "Portal";
    case LightFeature::MeshEmissive:
      return "MeshEmissive";
    case LightFeature::EnvironmentSky:
      return "EnvironmentSky";
    case LightFeature::OpenSky:
      return "OpenSky";
    case LightFeature::BlackbodyEmitter:
      return "BlackbodyEmitter";
    case LightFeature::VisibleEmissiveObject:
      return "VisibleEmissiveObject";
    case LightFeature::Unknown:
    default:
      return "Unknown";
  }
}

inline std::string EscapeLightingJson(std::string_view text) {
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

inline const std::vector<LightFeatureDescriptor>& GetLightingFeatureInventory() {
  static const std::vector<LightFeatureDescriptor> s_inventory = {
      {LightFeature::Point,
       "point",
       "Point",
       "point",
       "candela",
       true,
       false,
       false,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "candela", "luminous intensity", true},
        {"radius", "meter", "soft shadow radius", false},
        {"falloff", "inverse_square", "distance attenuation", false}},
       "Core punctual light; radius is a soft-shadow approximation until emitter geometry sampling lands."},
      {LightFeature::Spot,
       "spot",
       "Spot",
       "spot",
       "candela",
       true,
       false,
       false,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "candela", "luminous intensity", true},
        {"radius", "meter", "source radius", false},
        {"direction", "unit_vector", "spot direction", true},
        {"beam_angle", "degree", "inner cone angle", true},
        {"blend", "unit_interval", "edge softness", false}},
       "Spot light descriptor maps authored cone controls into renderer falloff terms."},
      {LightFeature::Directional,
       "directional",
       "Directional",
       "directional",
       "lux",
       true,
       false,
       false,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "lux", "illuminance", true},
        {"direction", "unit_vector", "incoming light direction", true}},
       "Directional light for sun-like distant illumination."},
      {LightFeature::SphereArea,
       "sphere_area",
       "Sphere Area",
       "sphere_area",
       "lumen",
       true,
       true,
       true,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "lumen", "radiant flux fallback", true},
        {"radius", "meter", "sphere radius", true},
        {"center", "meter", "world-space center", true}},
       "Area emitter declaration for next-event estimation and visible light geometry."},
      {LightFeature::RectangleArea,
       "rectangle_area",
       "Rectangle Area",
       "rectangle_area",
       "lumen",
       true,
       true,
       true,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "lumen", "radiant flux fallback", true},
        {"width", "meter", "emitter width", true},
        {"height", "meter", "emitter height", true},
        {"normal", "unit_vector", "surface normal", true},
        {"u_axis", "unit_vector", "local U axis", true},
        {"v_axis", "unit_vector", "local V axis", true}},
       "Rectangular emitter descriptor for softbox and window-light tests."},
      {LightFeature::DiskArea,
       "disk_area",
       "Disk Area",
       "disk_area",
       "lumen",
       true,
       true,
       true,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "lumen", "radiant flux fallback", true},
        {"radius", "meter", "disk radius", true},
        {"normal", "unit_vector", "surface normal", true},
        {"center", "meter", "world-space center", true}},
       "Disk emitter descriptor for circular panels and aperture-like sources."},
      {LightFeature::LineArea,
       "line_area",
       "Line Area",
       "line_area",
       "lumen",
       true,
       true,
       true,
       {{"color", "linear_rgb", "radiant color", true},
        {"intensity", "lumen", "radiant flux fallback", true},
        {"start", "meter", "segment start", true},
        {"end", "meter", "segment end", true},
        {"radius", "meter", "tube radius", false}},
       "Tube/line emitter descriptor; renderer may approximate as a thin cylinder."},
      {LightFeature::Portal,
       "portal",
       "Portal",
       "portal",
       "relative",
       false,
       true,
       false,
       {{"color", "linear_rgb", "tint", false},
        {"intensity", "relative", "sampling weight", true},
        {"width", "meter", "portal width", true},
        {"height", "meter", "portal height", true},
        {"normal", "unit_vector", "portal normal", true},
        {"forward", "unit_vector", "environment lookup direction", true}},
       "Sampling aid for environment lighting; does not emit by itself unless paired with an environment."},
      {LightFeature::MeshEmissive,
       "mesh_emissive",
       "Mesh Emissive",
       "mesh_emissive",
       "W sr-1 m-2",
       true,
       true,
       true,
       {{"mesh_id", "asset_id", "emitter mesh asset", true},
        {"color", "linear_rgb", "radiance color", true},
        {"intensity", "W sr-1 m-2", "radiance scale", true},
        {"unit_scale", "meter", "mesh unit conversion", false}},
       "Connects material emission to explicit light sampling inventory."},
      {LightFeature::EnvironmentSky,
       "environment_sky",
       "Environment Sky",
       "environment_sky",
       "cd m-2",
       true,
       true,
       false,
       {{"color", "linear_rgb", "sky tint", false},
        {"intensity", "cd m-2", "environment luminance", true},
        {"turbidity", "unitless", "atmosphere turbidity", false},
        {"elevation", "degree", "sun elevation", false}},
       "Procedural sky/environment light descriptor."},
      {LightFeature::OpenSky,
       "open_sky",
       "Open Sky",
       "open_sky",
       "cd m-2",
       true,
       true,
       false,
       {{"intensity", "cd m-2", "sky luminance", true},
        {"sun_color", "linear_rgb", "sun tint", false},
        {"sun_elevation", "degree", "sun elevation", false}},
       "Simpler sky model for benchmark scenes where full atmosphere is not needed."},
      {LightFeature::BlackbodyEmitter,
       "blackbody_emitter",
       "Blackbody Emitter",
       "blackbody_emitter",
       "watt",
       true,
       true,
       true,
       {{"temperature_k", "kelvin", "blackbody temperature", true},
        {"intensity", "watt", "radiant power", true},
        {"scale", "unitless", "emission multiplier", false}},
       "Physical color-temperature hook for hot emitters and fire/plasma materials."},
      {LightFeature::VisibleEmissiveObject,
       "visible_emissive_object",
       "Visible Emissive Object",
       "visible_emissive_object",
       "candela",
       true,
       true,
       true,
       {{"object_id", "entity_id", "visible source entity", true},
        {"color", "linear_rgb", "emission color", true},
        {"intensity", "candela", "luminous intensity fallback", true},
        {"mask_threshold", "unit_interval", "visibility mask threshold", false}},
       "Bridge descriptor between visible emissive materials and the light sampling table."},
  };
  return s_inventory;
}

inline const LightFeatureDescriptor* FindLightFeature(std::string_view id) {
  for (const auto& desc : GetLightingFeatureInventory()) {
    if (desc.id == id || desc.scene_type == id) {
      return &desc;
    }
  }
  return nullptr;
}

inline std::string SerializeLightingFeatureInventory() {
  const auto& inventory = GetLightingFeatureInventory();
  std::ostringstream out;
  out << "{\"schema\":\"vkpt.lighting_inventory.v1\",\"features\":[";
  for (std::size_t i = 0; i < inventory.size(); ++i) {
    if (i > 0) out << ",";
    const auto& desc = inventory[i];
    out << "{";
    out << "\"feature\":\"" << ToString(desc.feature) << "\",";
    out << "\"id\":\"" << EscapeLightingJson(desc.id) << "\",";
    out << "\"display_name\":\"" << EscapeLightingJson(desc.display_name) << "\",";
    out << "\"scene_type\":\"" << EscapeLightingJson(desc.scene_type) << "\",";
    out << "\"intensity_unit\":\"" << EscapeLightingJson(desc.intensity_unit) << "\",";
    out << "\"physically_based\":" << (desc.physically_based ? "true" : "false") << ",";
    out << "\"importance_sampled\":" << (desc.importance_sampled ? "true" : "false") << ",";
    out << "\"visible_to_camera\":" << (desc.visible_to_camera ? "true" : "false") << ",";
    out << "\"parameters\":[";
    for (std::size_t j = 0; j < desc.parameters.size(); ++j) {
      if (j > 0) out << ",";
      const auto& param = desc.parameters[j];
      out << "{";
      out << "\"name\":\"" << EscapeLightingJson(param.name) << "\",";
      out << "\"unit\":\"" << EscapeLightingJson(param.unit) << "\",";
      out << "\"semantic\":\"" << EscapeLightingJson(param.semantic) << "\",";
      out << "\"required\":" << (param.required ? "true" : "false");
      out << "}";
    }
    out << "],\"notes\":\"" << EscapeLightingJson(desc.notes) << "\"";
    out << "}";
  }
  out << "]}";
  return out.str();
}

}  // namespace vkpt::shaders
