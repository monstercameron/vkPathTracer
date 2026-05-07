#include "scene/Scene.h"

#include "scene/Json.h"
#include "scene/SceneInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <cstring>

namespace vkpt::scene {

using namespace detail;

std::string NormalizeMaterialFamilyId(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9')) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    } else if (uc == '_' || uc == '-' || std::isspace(uc) != 0) {
      if (!out.empty() && out.back() != '_') {
        out.push_back('_');
      }
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out.empty() ? "diffuse" : out;
}

void ApplyMaterialFamilyPreset(SceneMaterialDefinition& material,
                               SceneMaterialPresetPolicy policy) {
  const std::string family = NormalizeMaterialFamilyId(
      material.family.empty() ? material.name : material.family);
  material.family = family;

  const bool force = policy == SceneMaterialPresetPolicy::Override;
  auto near_value = [](float a, float b) {
    return std::fabs(a - b) <= 1.0e-5f;
  };
  auto set_if_generic = [&](float& target, float value, float generic) {
    if (force || !std::isfinite(target) || near_value(target, generic)) {
      target = value;
    }
  };
  auto set_color_if_dark = [&](Vec3& target, Vec3 value) {
    if (force || (target.x <= 0.0f && target.y <= 0.0f && target.z <= 0.0f)) {
      target = value;
    }
  };

  set_if_generic(material.alpha, 1.0f, 1.0f);
  set_if_generic(material.transmission, 0.0f, 0.0f);
  set_if_generic(material.clearcoat, 0.0f, 0.0f);
  set_if_generic(material.sheen, 0.0f, 0.0f);
  set_if_generic(material.anisotropy, 0.0f, 0.0f);

  if (family == "mirror") {
    set_if_generic(material.roughness, 0.0f, 1.0f);
    set_if_generic(material.metallic, 1.0f, 0.0f);
  } else if (family == "glossy" || family == "specular" ||
             family == "normal_mapped_pbr" || family == "plastic") {
    set_if_generic(material.roughness, 0.18f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
  } else if (family == "metallic_pbr" ||
             family == "ggx_rough_conductor" ||
             family == "anisotropic_ggx" ||
             family == "brushed_metal" ||
             family == "ground_metal") {
    set_if_generic(material.roughness, family == "ggx_rough_conductor" ? 0.35f : 0.24f, 1.0f);
    set_if_generic(material.metallic, 1.0f, 0.0f);
    set_if_generic(material.anisotropy, family == "brushed_metal" ? 0.65f : 0.0f, 0.0f);
  } else if (family == "dielectric_glass" ||
             family == "ggx_rough_dielectric" ||
             family == "spectral_glass_approx" ||
             family == "resin" ||
             family == "epoxy" ||
             family == "gemstone" ||
             family == "ice_crystal" ||
             family == "frosted_acrylic" ||
             family == "translucent_polymer") {
    set_if_generic(material.roughness, 0.02f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.ior,
                   family == "gemstone" ? 1.75f : (family == "ice_crystal" ? 1.31f : 1.5f),
                   1.5f);
    set_if_generic(material.transmission, 1.0f, 0.0f);
    set_if_generic(material.alpha, 0.35f, 1.0f);
  } else if (family == "frosted_glass" || family == "dirty_glass") {
    set_if_generic(material.roughness, 0.48f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.ior, 1.45f, 1.5f);
    set_if_generic(material.transmission, 0.85f, 0.0f);
    set_if_generic(material.alpha, 0.5f, 1.0f);
  } else if (family == "clearcoat" ||
             family == "paint" ||
             family == "car_paint" ||
             family == "porcelain_ceramic" ||
             family == "energy_conserving_layered") {
    set_if_generic(material.roughness, 0.22f, 1.0f);
    set_if_generic(material.metallic, family == "car_paint" ? 0.25f : 0.0f, 0.0f);
    set_if_generic(material.clearcoat, 1.0f, 0.0f);
  } else if (family == "velvet" || family == "fabric_cloth" || family == "hair_fur_lobes") {
    set_if_generic(material.roughness, 0.86f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.sheen, family == "velvet" ? 0.85f : 0.62f, 0.0f);
  } else if (family == "toon_surface" || family == "stylized_diffuse" || family == "xray") {
    set_if_generic(material.roughness, 1.0f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
  } else if (family == "subsurface_approx" ||
             family == "skin" ||
             family == "wax" ||
             family == "marble_scattering") {
    set_if_generic(material.roughness, family == "marble_scattering" ? 0.72f : 0.62f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.sheen, family == "skin" ? 0.2f : 0.12f, 0.0f);
    set_if_generic(material.alpha, 0.92f, 1.0f);
  } else if (family == "volumetric_shafts" ||
             family == "volumetric_medium" ||
             family == "smoke" ||
             family == "chromatic_dust") {
    set_if_generic(material.roughness, 1.0f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.alpha, 0.45f, 1.0f);
    set_if_generic(material.transmission, 0.12f, 0.0f);
  } else if (family == "emissive" ||
             family == "environment_emissive" ||
             family == "blackbody_emission" ||
             family == "fire_plasma" ||
             family == "fire_sparkle_emission" ||
             family == "light_emitting_textile" ||
             family == "bokeh_motion_blur_stress") {
    set_if_generic(material.roughness, 1.0f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_color_if_dark(material.emission,
                      {std::max(0.2f, material.albedo.x),
                       std::max(0.2f, material.albedo.y),
                       std::max(0.2f, material.albedo.z)});
    set_if_generic(material.emission_intensity,
                   family == "blackbody_emission" ? 5.0f : 3.0f,
                   0.0f);
  } else if (family == "thin_film_iridescent" ||
             family == "holographic_coating" ||
             family == "pearl_lustre" ||
             family == "diffraction_grating") {
    set_if_generic(material.roughness, 0.2f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.clearcoat, 0.8f, 0.0f);
    set_if_generic(material.sheen, 0.35f, 0.0f);
  } else if (family == "alpha_mask") {
    set_if_generic(material.roughness, 0.8f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.alpha, 0.5f, 1.0f);
  } else if (family == "wet_surface" || family == "water_fluid_surface") {
    set_if_generic(material.roughness, 0.08f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.clearcoat, 0.9f, 0.0f);
    set_if_generic(material.transmission, family == "water_fluid_surface" ? 0.55f : 0.0f, 0.0f);
  } else if (family == "retroreflector" || family == "caustics_inspired_response") {
    set_if_generic(material.roughness, 0.12f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.clearcoat, 0.75f, 0.0f);
  } else if (family == "plastic" || family == "rubber") {
    set_if_generic(material.roughness, family == "rubber" ? 0.55f : 0.38f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.ior, family == "rubber" ? 1.35f : 1.5f, 1.5f);
  } else if (family == "wood" || family == "oak" || family == "walnut" || family == "parquet" ||
             family == "sandalwood" || family == "pine" || family == "teak" ||
             family == "mahogany" || family == "cedar") {
    const bool polished = family == "parquet" || family == "teak";
    const bool roughWood = family == "walnut" || family == "mahogany" || family == "cedar";
    set_if_generic(material.roughness, polished ? 0.28f : (roughWood ? 0.62f : 0.46f), 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
    set_if_generic(material.clearcoat, polished ? 0.55f : (roughWood ? 0.08f : 0.24f), 0.0f);
    set_if_generic(material.anisotropy, polished ? 0.28f : 0.18f, 0.0f);
  } else if (family == "stone" ||
             family == "concrete" ||
             family == "plaster" ||
             family == "sand" ||
             family == "mud" ||
             family == "terra_earth" ||
             family == "charcoal" ||
             family == "cardboard" ||
             family == "paper") {
    set_if_generic(material.roughness, 0.82f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
  } else {
    set_if_generic(material.roughness, 0.85f, 1.0f);
    set_if_generic(material.metallic, 0.0f, 0.0f);
  }

  const bool emissiveFamily =
      family == "emissive" ||
      family == "environment_emissive" ||
      family == "blackbody_emission" ||
      family == "fire_plasma" ||
      family == "fire_sparkle_emission" ||
      family == "light_emitting_textile" ||
      family == "bokeh_motion_blur_stress";
  if (force && !emissiveFamily) {
    material.emission = {0.0f, 0.0f, 0.0f};
    material.emission_intensity = 0.0f;
  }
}

std::string_view to_string(ComponentKind kind) {
  switch (kind) {
    case ComponentKind::Identity:
      return "Identity";
    case ComponentKind::Transform:
      return "Transform";
    case ComponentKind::Hierarchy:
      return "Hierarchy";
    case ComponentKind::Camera:
      return "Camera";
    case ComponentKind::Light:
      return "Light";
    case ComponentKind::MeshRenderer:
      return "MeshRenderer";
    case ComponentKind::SdfPrimitive:
      return "SDFPrimitive";
    case ComponentKind::MaterialOverride:
      return "MaterialOverride";
    case ComponentKind::PhysicsBody:
      return "PhysicsBody";
    case ComponentKind::Script:
      return "Script";
    case ComponentKind::BenchmarkTag:
      return "BenchmarkTag";
    default:
      return "Unknown";
  }
}

std::string_view to_string(TransformAuthority authority) {
  switch (authority) {
    case TransformAuthority::BenchmarkFrozen:
      return "BenchmarkFrozen";
    case TransformAuthority::PhysicsControlled:
      return "PhysicsControlled";
    case TransformAuthority::ScriptControlled:
      return "ScriptControlled";
    case TransformAuthority::EditorControlled:
      return "EditorControlled";
    case TransformAuthority::Authored:
      return "Authored";
    default:
      return "Authored";
  }
}

vkpt::core::Result<SceneWorld> SceneDocument::to_world() const {
  SceneWorld world;
  for (const auto& entity : entities) {
    const auto id = world.create_entity(entity.name, entity.id);
    if (!id) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
  }
  for (const auto& entity : entities) {
    const auto id = entity.id;
    if (entity.has_transform && !world.set_component(id, ComponentKind::Transform, entity.transform)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_camera && !world.set_component(id, ComponentKind::Camera, entity.camera)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_light && !world.set_component(id, ComponentKind::Light, entity.light)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_mesh && !world.set_component(id, ComponentKind::MeshRenderer, entity.mesh)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_sdf_primitive && !world.set_component(id, ComponentKind::SdfPrimitive, entity.sdf_primitive)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.material.material_id != 0 && !world.set_component(id, ComponentKind::MaterialOverride, entity.material)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_hierarchy && !world.set_hierarchy_parent(id, entity.hierarchy.parent, entity.hierarchy.sibling_order)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    if (entity.has_physics_body && !world.set_component(id, ComponentKind::PhysicsBody, entity.physics_body)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (!entity.script.script.empty() && !world.set_component(id, ComponentKind::Script, entity.script)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_benchmark_tag && !world.set_component(id, ComponentKind::BenchmarkTag, entity.benchmark_tag)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
  }
  for (const auto& cam : cameras) {
    if (!world.set_component(cam.id, ComponentKind::Camera, cam.camera)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
  }
  for (const auto& light : lights) {
    if (!world.set_component(light.id, ComponentKind::Light, light.light)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
  }
  for (const auto& transform : transforms) {
    if (transform.id != 0) {
      if (transform.parent != 0) {
        if (!world.set_hierarchy_parent(transform.id, transform.parent)) {
          return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
        }
      }
      if (!world.set_transform(transform.id, transform.transform, TransformAuthority::Authored, "document", 0)) {
        return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
      }
    }
  }
  world.recompute_world_transforms();
  return vkpt::core::Result<SceneWorld>::ok(std::move(world));
}

vkpt::core::Result<void> SceneDocument::apply_to_world(SceneWorld& world) const {
  auto loaded = to_world();
  if (!loaded) {
    return vkpt::core::Result<void>::error(loaded.error());
  }
  world = std::move(loaded.value());
  return vkpt::core::Result<void>::ok();
}

vkpt::core::Result<SceneDocument> JsonSceneLoader::load_document_from_text(std::string_view text) {
  return SceneDocument::load_from_text(text);
}

vkpt::core::Result<SceneDocument> JsonSceneLoader::load_document_from_file(std::string_view path) {
  return SceneDocument::load_from_file(path);
}

SceneRuntime::SceneRuntime(SceneWorld world) : m_world(std::move(world)) {}

IEcsWorld& SceneRuntime::world() {
  return m_world;
}

const IEcsWorld& SceneRuntime::world() const {
  return m_world;
}

SceneWorld& SceneRuntime::scene_world() {
  return m_world;
}

const SceneWorld& SceneRuntime::scene_world() const {
  return m_world;
}

vkpt::core::Result<void> SceneRuntime::load_document(const SceneDocument& document) {
  return document.apply_to_world(m_world);
}

SceneSnapshot SceneRuntime::snapshot() const {
  return m_world.build_snapshot();
}

RenderSceneProxy SceneRuntime::extract_render_scene(vkpt::core::FrameIndex frame) const {
  return m_world.extract_render_scene(frame);
}

FrameLifecycleController& SceneRuntime::frame_lifecycle() {
  return m_frameLifecycle;
}

const FrameLifecycleController& SceneRuntime::frame_lifecycle() const {
  return m_frameLifecycle;
}

}  // namespace vkpt::scene
