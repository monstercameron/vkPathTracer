#include "scene/Scene.h"

#include "core/Logging.h"
#include "assets/SceneAssetLoader.h"
#include "scene/Json.h"
#include "scene/SceneInternal.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vkpt::scene {

using namespace detail;

vkpt::core::Result<SceneDocument> SceneDocument::load_from_text(std::string_view text) {
  const auto root = JsonParser::parse(text);
  if (!root || root->kind != JsonValue::Kind::Object) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  SceneDocument doc;
  std::unordered_set<std::uint64_t> usedIds;
  const auto& rootObj = *root;
  auto require_kind_if_present = [&](std::string_view key, JsonValue::Kind kind) {
    const auto it = rootObj.object.find(std::string(key));
    return it == rootObj.object.end() || it->second.kind == kind;
  };
  if (!require_kind_if_present("schema", JsonValue::Kind::String) ||
      !require_kind_if_present("metadata", JsonValue::Kind::Object) ||
      !require_kind_if_present("assets", JsonValue::Kind::Array) ||
      !require_kind_if_present("materials", JsonValue::Kind::Array) ||
      !require_kind_if_present("geometry", JsonValue::Kind::Array) ||
      !require_kind_if_present("sdf_primitives", JsonValue::Kind::Array) ||
      !require_kind_if_present("entities", JsonValue::Kind::Array) ||
      !require_kind_if_present("transforms", JsonValue::Kind::Array) ||
      !require_kind_if_present("cameras", JsonValue::Kind::Array) ||
      !require_kind_if_present("lights", JsonValue::Kind::Array) ||
      !require_kind_if_present("benchmark", JsonValue::Kind::Object)) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  read_string(rootObj, "schema", doc.metadata.schema);

  if (const auto metadataNode = rootObj.object.find("metadata"); metadataNode != rootObj.object.end()) {
    read_string(metadataNode->second, "schema", doc.metadata.schema);
    read_string(metadataNode->second, "scene_name", doc.metadata.scene_name);
    read_string(metadataNode->second, "author", doc.metadata.author);
    read_string(metadataNode->second, "created", doc.metadata.created);
  }

  if (const auto assetsNode = rootObj.object.find("assets"); assetsNode != rootObj.object.end() &&
      assetsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : assetsNode->second.array) {
      SceneAssetDefinition asset;
      read_u64(item, "id", asset.id);
      read_string(item, "type", asset.type);
      read_string(item, "uri", asset.uri);
      if (asset.id == 0) {
        asset.id = allocate_id(usedIds);
      }
      usedIds.insert(asset.id);
      doc.assets.push_back(std::move(asset));
    }
  }

  usedIds.clear();
  if (const auto materialsNode = rootObj.object.find("materials"); materialsNode != rootObj.object.end() &&
      materialsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : materialsNode->second.array) {
      SceneMaterialDefinition material;
      read_u64(item, "id", material.id);
      read_string(item, "name", material.name);
      read_string(item, "family", material.family);
      if (material.id == 0) {
        material.id = allocate_id(usedIds);
      }
      usedIds.insert(material.id);
      ApplyMaterialFamilyPreset(material, SceneMaterialPresetPolicy::Override);
      if (item.object.contains("albedo")) {
        read_vec3(item, "albedo", material.albedo);
      }
      if (item.object.contains("emission")) {
        read_vec3(item, "emission", material.emission);
      }
      if (item.object.contains("emission_intensity")) {
        read_float(item, "emission_intensity", material.emission_intensity);
      }
      read_float(item, "roughness", material.roughness);
      read_float(item, "metallic", material.metallic);
      read_float(item, "ior", material.ior);
      read_float(item, "transmission", material.transmission);
      read_float(item, "clearcoat", material.clearcoat);
      read_float(item, "sheen", material.sheen);
      read_float(item, "anisotropy", material.anisotropy);
      read_float(item, "alpha", material.alpha);
      read_bool(item, "double_sided", material.double_sided);
      read_string(item, "base_color_texture", material.base_color_texture);
      read_string(item, "normal_texture", material.normal_texture);
      doc.materials.push_back(std::move(material));
    }
  }

  if (const auto geometryNode = rootObj.object.find("geometry"); geometryNode != rootObj.object.end() &&
      geometryNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : geometryNode->second.array) {
      SceneGeometryDefinition geometry;
      read_u64(item, "id", geometry.id);
      read_string(item, "primitive", geometry.primitive);
      read_u64(item, "material_id", geometry.material_id);
      read_vec3_list(item, "vertices", geometry.vertices);
      read_vec2_list(item, "texcoords", geometry.texcoords);
      read_u32_list(item, "indices", geometry.indices);
      if (const auto tessNode = item.object.find("tessellation");
          tessNode != item.object.end() && tessNode->second.kind == JsonValue::Kind::Object) {
        read_bool(tessNode->second, "enabled", geometry.tessellation.enabled);
        read_string(tessNode->second, "mode", geometry.tessellation.mode);
        read_u32(tessNode->second, "factor", geometry.tessellation.factor);
        read_bool(tessNode->second, "gpu", geometry.tessellation.gpu_preferred);
        read_bool(tessNode->second, "cache", geometry.tessellation.cache_generated_geometry);
        read_bool(tessNode->second, "displacement", geometry.tessellation.displacement);
        read_string(tessNode->second, "projection", geometry.tessellation.projection);
        if (geometry.tessellation.enabled && geometry.tessellation.mode == "off") {
          geometry.tessellation.mode = "uniform";
        }
      }
      const auto tags = item.object.find("tags");
      if (tags != item.object.end() && tags->second.kind == JsonValue::Kind::Array) {
        for (const auto& t : tags->second.array) {
          if (t.kind == JsonValue::Kind::String) {
            geometry.tags.push_back(t.string);
          }
        }
      }
      doc.geometry.push_back(std::move(geometry));
    }
  }

  usedIds.clear();
  if (const auto sdfNode = rootObj.object.find("sdf_primitives"); sdfNode != rootObj.object.end() &&
      sdfNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : sdfNode->second.array) {
      SceneSdfPrimitiveDefinition primitive;
      read_u64(item, "id", primitive.id);
      read_string(item, "shape", primitive.shape);
      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        primitive.transform = read_transform(transformNode->second);
      }
      if (const auto primitiveNode = item.object.find("primitive"); primitiveNode != item.object.end()) {
        read_string(primitiveNode->second, "shape", primitive.primitive.shape);
        read_float(primitiveNode->second, "radius", primitive.primitive.radius);
        read_float(primitiveNode->second, "param_a", primitive.primitive.param_a);
        read_float(primitiveNode->second, "param_b", primitive.primitive.param_b);
      }
      if (primitive.id == 0) {
        primitive.id = allocate_id(usedIds);
      }
      usedIds.insert(primitive.id);
      doc.sdf_primitives.push_back(std::move(primitive));
    }
  }

  usedIds.clear();
  if (const auto entitiesNode = rootObj.object.find("entities"); entitiesNode != rootObj.object.end() &&
      entitiesNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : entitiesNode->second.array) {
      SceneEntityDefinition entity;
      read_u64(item, "id", entity.id);
      if (entity.id == 0) {
        entity.id = allocate_id(usedIds);
      }
      usedIds.insert(entity.id);
      read_string(item, "name", entity.name);

      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        entity.has_transform = true;
        entity.transform = read_transform(transformNode->second);
      }
      if (const auto cameraNode = item.object.find("camera"); cameraNode != item.object.end()) {
        entity.has_camera = true;
        read_camera_component(cameraNode->second, entity.camera);
      }
      if (const auto lightNode = item.object.find("light"); lightNode != item.object.end()) {
        entity.has_light = true;
        read_string(lightNode->second, "type", entity.light.type);
        read_vec3(lightNode->second, "color", entity.light.color);
        read_float(lightNode->second, "intensity", entity.light.intensity);
        read_float(lightNode->second, "radius", entity.light.radius);
        read_vec3(lightNode->second, "direction", entity.light.direction);
        read_float(lightNode->second, "beam_angle", entity.light.beam_angle_degrees);
        read_float(lightNode->second, "blend", entity.light.blend);
      }
      if (const auto meshNode = item.object.find("mesh"); meshNode != item.object.end()) {
        entity.has_mesh = true;
        read_u64(meshNode->second, "mesh_id", entity.mesh.mesh_id);
        read_u64(meshNode->second, "material_id", entity.mesh.material_id);
      }
      if (const auto sdfNode = item.object.find("sdf_primitive"); sdfNode != item.object.end()) {
        entity.has_sdf_primitive = true;
        read_string(sdfNode->second, "shape", entity.sdf_primitive.shape);
        read_float(sdfNode->second, "radius", entity.sdf_primitive.radius);
        read_float(sdfNode->second, "param_a", entity.sdf_primitive.param_a);
        read_float(sdfNode->second, "param_b", entity.sdf_primitive.param_b);
      }
      if (const auto hierarchyNode = item.object.find("hierarchy"); hierarchyNode != item.object.end()) {
        entity.has_hierarchy = true;
        read_u64(hierarchyNode->second, "parent", entity.hierarchy.parent);
        read_u32(hierarchyNode->second, "sibling_order", entity.hierarchy.sibling_order);
      }
      if (const auto materialNode = item.object.find("material"); materialNode != item.object.end()) {
        read_u64(materialNode->second, "id", entity.material.material_id);
      }
      if (const auto physicsNode = item.object.find("physics"); physicsNode != item.object.end()) {
        entity.has_physics_body = true;
        read_bool(physicsNode->second, "enabled", entity.physics_body.enabled);
        read_float(physicsNode->second, "mass", entity.physics_body.mass);
        read_bool(physicsNode->second, "dynamic", entity.physics_body.dynamic);
        read_string(physicsNode->second, "body_type", entity.physics_body.body_type);
        read_string(physicsNode->second, "shape", entity.physics_body.shape);
        read_float(physicsNode->second, "friction", entity.physics_body.friction);
        read_float(physicsNode->second, "restitution", entity.physics_body.restitution);
        read_float(physicsNode->second, "gravity_scale", entity.physics_body.gravity_scale);
        read_bool(physicsNode->second, "trigger", entity.physics_body.trigger);
        read_bool(physicsNode->second, "allow_sleeping", entity.physics_body.allow_sleeping);
        read_bool(physicsNode->second, "continuous_collision", entity.physics_body.continuous_collision);
        if (entity.physics_body.body_type == "dynamic") {
          entity.physics_body.dynamic = true;
        } else if (entity.physics_body.body_type == "static" || entity.physics_body.body_type == "kinematic") {
          entity.physics_body.dynamic = false;
        } else {
          entity.physics_body.body_type = entity.physics_body.dynamic ? "dynamic" : "static";
        }
      }
      if (const auto animNode = item.object.find("animation"); animNode != item.object.end()) {
        read_string(animNode->second, "clip", entity.animation.clip);
        read_bool(animNode->second, "looping", entity.animation.looping);
        read_float(animNode->second, "duration_seconds", entity.animation.duration_seconds);
        read_float(animNode->second, "duration", entity.animation.duration_seconds);
        read_float(animNode->second, "playback_speed", entity.animation.playback_speed);
        read_float(animNode->second, "speed", entity.animation.playback_speed);
        read_vec3(animNode->second, "translation_amplitude", entity.animation.translation_amplitude);
        read_vec3(animNode->second, "rotation_degrees", entity.animation.rotation_degrees);
        read_vec3(animNode->second, "scale_amplitude", entity.animation.scale_amplitude);
      }
      if (const auto scriptNode = item.object.find("script"); scriptNode != item.object.end()) {
        read_string(scriptNode->second, "source", entity.script.script);
        if (entity.script.script.empty()) {
          read_string(scriptNode->second, "path", entity.script.script);
        }
        read_string(scriptNode->second, "language", entity.script.language);
        read_string(scriptNode->second, "entry", entity.script.entry);
        read_bool(scriptNode->second, "enabled", entity.script.enabled);
        read_bool(scriptNode->second, "reload_on_save", entity.script.reload_on_save);
      }
      if (const auto benchmarkNode = item.object.find("benchmark"); benchmarkNode != item.object.end()) {
        entity.has_benchmark_tag = true;
        read_bool(benchmarkNode->second, "enabled", entity.benchmark_tag.enabled);
      }
      doc.entities.push_back(std::move(entity));
    }
  }

  if (const auto transformsNode = rootObj.object.find("transforms"); transformsNode != rootObj.object.end() &&
      transformsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : transformsNode->second.array) {
      SceneTransformEntry entry;
      read_u64(item, "id", entry.id);
      read_u64(item, "parent", entry.parent);
      if (const auto transform = item.object.find("transform"); transform != item.object.end()) {
        entry.transform = read_transform(transform->second);
      }
      doc.transforms.push_back(std::move(entry));
    }
  }

  if (const auto camerasNode = rootObj.object.find("cameras"); camerasNode != rootObj.object.end() &&
      camerasNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : camerasNode->second.array) {
      SceneCameraDefinition cam;
      read_u64(item, "id", cam.id);
      read_camera_component(item, cam.camera);
      doc.cameras.push_back(std::move(cam));
    }
  }

  if (const auto lightsNode = rootObj.object.find("lights"); lightsNode != rootObj.object.end() &&
      lightsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : lightsNode->second.array) {
      SceneLightDefinition light;
      read_u64(item, "id", light.id);
      read_string(item, "type", light.light.type);
      read_vec3(item, "color", light.light.color);
      read_float(item, "intensity", light.light.intensity);
      read_float(item, "radius", light.light.radius);
      read_vec3(item, "direction", light.light.direction);
      read_float(item, "beam_angle", light.light.beam_angle_degrees);
      read_float(item, "blend", light.light.blend);
      doc.lights.push_back(std::move(light));
    }
  }

  if (const auto benchmarkNode = rootObj.object.find("benchmark"); benchmarkNode != rootObj.object.end()) {
    read_bool(benchmarkNode->second, "enabled", doc.benchmark.enabled);
    read_u32(benchmarkNode->second, "frame_target", doc.benchmark.frame_target);
    read_u32(benchmarkNode->second, "warmup_frames", doc.benchmark.warmup_frames);
  }

  if (!doc.validate(nullptr)) {
    doc.parse_result = SceneSchemaError::ValidationFailure;
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  doc.parse_result = SceneSchemaError::Ok;
  return vkpt::core::Result<SceneDocument>::ok(std::move(doc));
}

vkpt::core::Result<SceneDocument> SceneDocument::load_from_file(std::string_view path) {
  std::ifstream file{std::string(path)};
  if (!file) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::IOError);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  auto loaded = load_from_text(buffer.str());
  if (!loaded) {
    return vkpt::core::Result<SceneDocument>::error(loaded.error());
  }
  auto document = std::move(loaded.value());
  std::vector<std::string> asset_diagnostics;
  vkpt::assets::SceneAssetExpansionStats expansion_stats{};
  if (!vkpt::assets::ExpandSceneAssetReferences(document,
                                                std::filesystem::path(std::string(path)),
                                                &expansion_stats,
                                                &asset_diagnostics)) {
    for (const auto& diagnostic : asset_diagnostics) {
      vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "asset_import", diagnostic);
    }
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  if (expansion_stats.imported_models > 0u) {
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                      "asset_import",
                                      "scene assets expanded",
                                      {
                                          {"models", std::to_string(expansion_stats.imported_models)},
                                          {"textures", std::to_string(expansion_stats.imported_textures)},
                                          {"materials", std::to_string(expansion_stats.imported_materials)},
                                          {"geometry", std::to_string(expansion_stats.imported_geometry)},
                                          {"entities", std::to_string(expansion_stats.imported_entities)},
                                      });
  }
  if (!document.validate(nullptr)) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  return vkpt::core::Result<SceneDocument>::ok(std::move(document));
}

bool SceneDocument::validate(std::vector<std::string>* issues) const {
  bool ok = true;
  auto report = [&](const std::string& message) {
    ok = false;
    if (issues) {
      issues->push_back(message);
    }
  };
  std::unordered_set<vkpt::core::StableId> entityIds;
  std::unordered_set<vkpt::core::StableId> materialIds;
  std::unordered_set<vkpt::core::StableId> geometryIds;
  std::unordered_set<vkpt::core::StableId> assetIds;
  std::unordered_set<vkpt::core::StableId> sdfIds;
  std::unordered_map<vkpt::core::StableId, vkpt::core::StableId> parentByEntity;

  if (metadata.schema.empty()) {
    report("metadata schema is empty");
  } else if (metadata.schema != "1.0") {
    report("unsupported schema " + metadata.schema);
  }

  for (const auto& asset : assets) {
    if (asset.id == 0) {
      report("asset id is zero");
    }
    if (!assetIds.insert(asset.id).second) {
      report("duplicate asset id " + std::to_string(asset.id));
    }
    if (asset.type.empty()) {
      report("asset type is empty for " + std::to_string(asset.id));
    }
    if (asset.uri.empty()) {
      report("asset uri is empty for " + std::to_string(asset.id));
    }
  }

  for (const auto& material : materials) {
    if (material.id == 0) {
      report("material id is zero");
    }
    if (!materialIds.insert(material.id).second) {
      report("duplicate material id " + std::to_string(material.id));
    }
    if (!finite_vec3(material.albedo) || !finite_vec3(material.emission)) {
      report("material contains non-finite color " + std::to_string(material.id));
    }
    if (material.family.empty()) {
      report("material family is empty " + std::to_string(material.id));
    }
    if (!std::isfinite(material.roughness) || material.roughness < 0.0f || material.roughness > 1.0f) {
      report("material roughness out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.metallic) || material.metallic < 0.0f || material.metallic > 1.0f) {
      report("material metallic out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.ior) || material.ior <= 0.0f) {
      report("material ior out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.transmission) || material.transmission < 0.0f || material.transmission > 1.0f) {
      report("material transmission out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.clearcoat) || material.clearcoat < 0.0f || material.clearcoat > 1.0f) {
      report("material clearcoat out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.sheen) || material.sheen < 0.0f || material.sheen > 1.0f) {
      report("material sheen out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.anisotropy) || material.anisotropy < -1.0f || material.anisotropy > 1.0f) {
      report("material anisotropy out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.alpha) || material.alpha < 0.0f || material.alpha > 1.0f) {
      report("material alpha out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.emission_intensity) || material.emission_intensity < 0.0f) {
      report("material emission intensity out of range " + std::to_string(material.id));
    }
  }

  for (const auto& geometry_entry : geometry) {
    if (geometry_entry.id == 0) {
      report("geometry id is zero");
    }
    if (!geometryIds.insert(geometry_entry.id).second) {
      report("duplicate geometry id " + std::to_string(geometry_entry.id));
    }
    if (geometry_entry.primitive.empty()) {
      report("geometry primitive is empty " + std::to_string(geometry_entry.id));
    }
    if (geometry_entry.material_id != 0 && !materialIds.empty() && !materialIds.contains(geometry_entry.material_id)) {
      report("geometry references missing material " + std::to_string(geometry_entry.material_id));
    }
    if (geometry_entry.primitive == "triangle") {
      if (geometry_entry.vertices.empty()) {
        report("triangle geometry has no vertices " + std::to_string(geometry_entry.id));
      }
      if (geometry_entry.indices.empty() || geometry_entry.indices.size() % 3u != 0u) {
        report("triangle geometry indices are not triangles " + std::to_string(geometry_entry.id));
      }
      for (const auto index : geometry_entry.indices) {
        if (index >= geometry_entry.vertices.size()) {
          report("geometry index out of range " + std::to_string(geometry_entry.id));
          break;
        }
      }
    }
    for (const auto& vertex : geometry_entry.vertices) {
      if (!finite_vec3(vertex)) {
        report("geometry contains non-finite vertex " + std::to_string(geometry_entry.id));
        break;
      }
    }
    if (geometry_entry.tessellation.enabled) {
      if (geometry_entry.tessellation.mode != "uniform") {
        report("geometry tessellation mode is unsupported " + std::to_string(geometry_entry.id));
      }
      if (geometry_entry.tessellation.projection != "none" &&
          geometry_entry.tessellation.projection != "sphere") {
        report("geometry tessellation projection is unsupported " + std::to_string(geometry_entry.id));
      }
      if (geometry_entry.tessellation.factor < 1u || geometry_entry.tessellation.factor > 64u) {
        report("geometry tessellation factor out of range " + std::to_string(geometry_entry.id));
      }
      if (geometry_entry.indices.empty() || geometry_entry.indices.size() % 3u != 0u) {
        report("geometry tessellation requires triangle indices " + std::to_string(geometry_entry.id));
      }
    }
  }

  for (const auto& sdf : sdf_primitives) {
    if (sdf.id == 0) {
      report("sdf primitive id is zero");
    }
    if (!sdfIds.insert(sdf.id).second) {
      report("duplicate sdf primitive id " + std::to_string(sdf.id));
    }
    if (sdf.shape.empty()) {
      report("sdf primitive shape is empty " + std::to_string(sdf.id));
    }
    if (!valid_transform_values(sdf.transform)) {
      report("sdf primitive has invalid transform " + std::to_string(sdf.id));
    }
    if (!std::isfinite(sdf.primitive.radius) || sdf.primitive.radius < 0.0f) {
      report("sdf primitive radius is invalid " + std::to_string(sdf.id));
    }
  }

  for (const auto& entity : entities) {
    if (entity.id == 0) {
      report("entity id is zero");
    }
    if (!entityIds.insert(entity.id).second) {
      report("duplicate entity id " + std::to_string(entity.id));
    }
    if (entity.hierarchy.parent != 0 && entity.id == entity.hierarchy.parent) {
      report("entity has self parent " + std::to_string(entity.id));
    }
    if (entity.hierarchy.parent != 0) {
      parentByEntity[entity.id] = entity.hierarchy.parent;
    }
    if (entity.has_transform && !valid_transform_values(entity.transform)) {
      report("entity has invalid transform " + std::to_string(entity.id));
    }
    if (entity.has_mesh) {
      if (!geometryIds.empty() && !geometryIds.contains(entity.mesh.mesh_id)) {
        report("entity references missing geometry " + std::to_string(entity.mesh.mesh_id));
      }
      if (entity.mesh.material_id != 0 && !materialIds.empty() && !materialIds.contains(entity.mesh.material_id)) {
        report("entity references missing material " + std::to_string(entity.mesh.material_id));
      }
    }
    if (entity.has_sdf_primitive) {
      if (entity.sdf_primitive.shape.empty()) {
        report("entity sdf primitive shape is empty " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.sdf_primitive.radius) || entity.sdf_primitive.radius < 0.0f) {
        report("entity sdf primitive radius is invalid " + std::to_string(entity.id));
      }
    }
    if (entity.material.material_id != 0 && !materialIds.empty() && !materialIds.contains(entity.material.material_id)) {
      report("entity material override references missing material " + std::to_string(entity.material.material_id));
    }
    if (entity.has_physics_body) {
      if (entity.physics_body.shape.empty()) {
        report("entity physics shape is empty " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.physics_body.mass) || entity.physics_body.mass <= 0.0f) {
        report("entity physics mass is invalid " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.physics_body.friction) || entity.physics_body.friction < 0.0f) {
        report("entity physics friction is invalid " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.physics_body.restitution) || entity.physics_body.restitution < 0.0f) {
        report("entity physics restitution is invalid " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.physics_body.gravity_scale)) {
        report("entity physics gravity scale is invalid " + std::to_string(entity.id));
      }
    }
    if (entity.has_camera && !valid_camera_values(entity.camera)) {
      report("entity camera has invalid clip/fov " + std::to_string(entity.id));
    }
    if (entity.has_light &&
        (!finite_vec3(entity.light.color) || !std::isfinite(entity.light.intensity) || entity.light.intensity < 0.0f ||
         !std::isfinite(entity.light.radius) || entity.light.radius < 0.0f ||
         !finite_vec3(entity.light.direction) || !std::isfinite(entity.light.beam_angle_degrees) ||
         entity.light.beam_angle_degrees <= 0.0f || !std::isfinite(entity.light.blend) ||
         entity.light.blend < 0.0f)) {
      report("entity light has invalid values " + std::to_string(entity.id));
    }
    if (!entity.animation.clip.empty() &&
        (!std::isfinite(entity.animation.duration_seconds) ||
         entity.animation.duration_seconds <= 0.0f ||
         !std::isfinite(entity.animation.playback_speed) ||
         !finite_vec3(entity.animation.translation_amplitude) ||
         !finite_vec3(entity.animation.rotation_degrees) ||
         !finite_vec3(entity.animation.scale_amplitude))) {
      report("entity animation has invalid values " + std::to_string(entity.id));
    }
  }
  for (const auto& transform : transforms) {
    if (!entityIds.contains(transform.id)) {
      report("transform references missing entity " + std::to_string(transform.id));
    }
    if (transform.parent != 0) {
      if (!entityIds.contains(transform.parent)) {
        report("transform references missing parent " + std::to_string(transform.parent));
      }
      parentByEntity[transform.id] = transform.parent;
    }
    if (!valid_transform_values(transform.transform)) {
      report("transform has invalid values " + std::to_string(transform.id));
    }
  }
  for (const auto& light : lights) {
    if (!entityIds.contains(light.id)) {
      report("light references missing entity " + std::to_string(light.id));
    }
    if (!finite_vec3(light.light.color) || !std::isfinite(light.light.intensity) || light.light.intensity < 0.0f ||
        !std::isfinite(light.light.radius) || light.light.radius < 0.0f ||
        !finite_vec3(light.light.direction) || !std::isfinite(light.light.beam_angle_degrees) ||
        light.light.beam_angle_degrees <= 0.0f || !std::isfinite(light.light.blend) ||
        light.light.blend < 0.0f) {
      report("light has invalid values " + std::to_string(light.id));
    }
  }
  for (const auto& cam : cameras) {
    if (!entityIds.contains(cam.id)) {
      report("camera references missing entity " + std::to_string(cam.id));
    }
    if (!valid_camera_values(cam.camera)) {
      report("camera has invalid clip/fov " + std::to_string(cam.id));
    }
  }

  for (const auto& [child, parent] : parentByEntity) {
    if (parent != 0 && !entityIds.contains(parent)) {
      report("hierarchy references missing parent " + std::to_string(parent));
    }
    std::unordered_set<vkpt::core::StableId> visited;
    auto current = child;
    while (parentByEntity.contains(current)) {
      if (!visited.insert(current).second) {
        report("hierarchy cycle includes entity " + std::to_string(child));
        break;
      }
      current = parentByEntity[current];
      if (current == 0) {
        break;
      }
    }
  }

  if (benchmark.enabled && benchmark.frame_target != 0 && benchmark.warmup_frames > benchmark.frame_target) {
    report("benchmark warmup_frames exceeds frame_target");
  }
  return ok;
}

bool SceneDocument::has_section(std::string_view name) const {
  return name == "schema" || name == "metadata" || name == "assets" || name == "materials" ||
      name == "geometry" || name == "sdf_primitives" || name == "entities" ||
      name == "transforms" || name == "cameras" || name == "lights" || name == "benchmark";
}

}  // namespace vkpt::scene
