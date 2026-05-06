#include "scene/Scene.h"

#include "scene/Json.h"
#include "scene/SceneInternal.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vkpt::scene {

using namespace detail;

std::string SceneDocument::to_json(bool pretty) const {
  // Build the DOM explicitly so load/export share the same key names and default elision rules.
  auto bool_value = [](bool v) {
    JsonValue value;
    value.kind = JsonValue::Kind::Boolean;
    value.boolean = v;
    return value;
  };
  auto number_value = [](double v) {
    JsonValue value;
    value.kind = JsonValue::Kind::Number;
    value.number = v;
    return value;
  };
  auto string_value = [](std::string_view v) {
    JsonValue value;
    value.kind = JsonValue::Kind::String;
    value.string = std::string(v);
    return value;
  };
  auto array_value = []() {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    return value;
  };
  auto object_value = []() {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    return value;
  };
  auto vec3_value = [&](const Vec3& v) {
    JsonValue value = array_value();
    value.array.push_back(number_value(v.x));
    value.array.push_back(number_value(v.y));
    value.array.push_back(number_value(v.z));
    return value;
  };
  auto vec2_value = [&](const Vec2& v) {
    JsonValue value = array_value();
    value.array.push_back(number_value(v.u));
    value.array.push_back(number_value(v.v));
    return value;
  };
  auto quat_value = [&](const Quat& v) {
    JsonValue value = array_value();
    value.array.push_back(number_value(v.x));
    value.array.push_back(number_value(v.y));
    value.array.push_back(number_value(v.z));
    value.array.push_back(number_value(v.w));
    return value;
  };
  auto transform_value = [&](const TransformComponent& transform) {
    JsonValue value = object_value();
    value.object["translation"] = vec3_value(transform.translation);
    value.object["rotation"] = quat_value(transform.rotation);
    value.object["scale"] = vec3_value(transform.scale);
    return value;
  };
  auto camera_value = [&](const CameraComponent& camera) {
    JsonValue value = object_value();
    value.object["fov"] = number_value(camera.fov);
    value.object["near_plane"] = number_value(camera.near_plane);
    value.object["far_plane"] = number_value(camera.far_plane);
    value.object["focal_length_mm"] = number_value(camera.focal_length_mm);
    value.object["sensor_width_mm"] = number_value(camera.sensor_width_mm);
    value.object["sensor_height_mm"] = number_value(camera.sensor_height_mm);
    value.object["aperture_radius"] = number_value(camera.aperture_radius);
    value.object["focus_distance"] = number_value(camera.focus_distance);
    value.object["f_stop"] = number_value(camera.f_stop);
    value.object["shutter_seconds"] = number_value(camera.shutter_seconds);
    value.object["iso"] = number_value(camera.iso);
    value.object["exposure_compensation"] = number_value(camera.exposure_compensation);
    value.object["white_balance_kelvin"] = number_value(camera.white_balance_kelvin);
    value.object["iris_blade_count"] = number_value(static_cast<double>(camera.iris_blade_count));
    value.object["iris_rotation_degrees"] = number_value(camera.iris_rotation_degrees);
    value.object["iris_roundness"] = number_value(camera.iris_roundness);
    value.object["anamorphic_squeeze"] = number_value(camera.anamorphic_squeeze);
    return value;
  };

  JsonValue root;
  root.kind = JsonValue::Kind::Object;
  root.object["schema"] = string_value("1.0");

  JsonValue metadataNode;
  metadataNode.kind = JsonValue::Kind::Object;
  metadataNode.object["schema"] = string_value(metadata.schema);
  metadataNode.object["scene_name"] = string_value(metadata.scene_name);
  metadataNode.object["author"] = string_value(metadata.author);
  metadataNode.object["created"] = string_value(metadata.created);
  root.object["metadata"] = metadataNode;

  JsonValue assetsNode = array_value();
  assetsNode.array.reserve(assets.size());
  for (const auto& asset : assets) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(asset.id));
    item.object["type"] = string_value(asset.type);
    item.object["uri"] = string_value(asset.uri);
    if (!asset.name.empty()) {
      item.object["name"] = string_value(asset.name);
    }
    if (asset.parent != 0) {
      item.object["parent"] = number_value(static_cast<double>(asset.parent));
    }
    if (asset.sibling_order != 0) {
      item.object["sibling_order"] = number_value(static_cast<double>(asset.sibling_order));
    }
    if (asset.has_transform) {
      item.object["transform"] = transform_value(asset.transform);
    }
    if (asset.disable_imported_animation) {
      item.object["disable_imported_animation"] = bool_value(true);
    }
    assetsNode.array.push_back(std::move(item));
  }
  root.object["assets"] = std::move(assetsNode);

  JsonValue materialsNode = array_value();
  materialsNode.array.reserve(materials.size());
  for (const auto& material : materials) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(material.id));
    item.object["name"] = string_value(material.name);
    item.object["family"] = string_value(material.family);
    item.object["albedo"] = vec3_value(material.albedo);
    item.object["roughness"] = number_value(material.roughness);
    item.object["metallic"] = number_value(material.metallic);
    item.object["ior"] = number_value(material.ior);
    item.object["transmission"] = number_value(material.transmission);
    item.object["clearcoat"] = number_value(material.clearcoat);
    item.object["sheen"] = number_value(material.sheen);
    item.object["anisotropy"] = number_value(material.anisotropy);
    item.object["alpha"] = number_value(material.alpha);
    item.object["double_sided"] = bool_value(material.double_sided);
    if (!material.base_color_texture.empty()) {
      item.object["base_color_texture"] = string_value(material.base_color_texture);
    }
    if (!material.normal_texture.empty()) {
      item.object["normal_texture"] = string_value(material.normal_texture);
    }
    item.object["emission"] = vec3_value(material.emission);
    item.object["emission_intensity"] = number_value(material.emission_intensity);
    materialsNode.array.push_back(std::move(item));
  }
  root.object["materials"] = std::move(materialsNode);

  JsonValue geometryNode = array_value();
  geometryNode.array.reserve(geometry.size());
  for (const auto& geometry_entry : geometry) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(geometry_entry.id));
    item.object["primitive"] = string_value(geometry_entry.primitive);
    item.object["material_id"] = number_value(static_cast<double>(geometry_entry.material_id));
    if (geometry_entry.tessellation.enabled ||
        geometry_entry.tessellation.factor > 1u ||
        geometry_entry.tessellation.mode != "off") {
      JsonValue tessNode = object_value();
      tessNode.object["enabled"] = bool_value(geometry_entry.tessellation.enabled);
      tessNode.object["mode"] = string_value(geometry_entry.tessellation.mode);
      tessNode.object["factor"] = number_value(static_cast<double>(geometry_entry.tessellation.factor));
      tessNode.object["gpu"] = bool_value(geometry_entry.tessellation.gpu_preferred);
      tessNode.object["cache"] = bool_value(geometry_entry.tessellation.cache_generated_geometry);
      tessNode.object["displacement"] = bool_value(geometry_entry.tessellation.displacement);
      tessNode.object["projection"] = string_value(geometry_entry.tessellation.projection);
      item.object["tessellation"] = std::move(tessNode);
    }
    JsonValue tagsNode = array_value();
    tagsNode.array.reserve(geometry_entry.tags.size());
    for (const auto& tag : geometry_entry.tags) {
      tagsNode.array.push_back(string_value(tag));
    }
    item.object["tags"] = std::move(tagsNode);
    JsonValue verticesNode = array_value();
    verticesNode.array.reserve(geometry_entry.vertices.size());
    for (const auto& vertex : geometry_entry.vertices) {
      verticesNode.array.push_back(vec3_value(vertex));
    }
    item.object["vertices"] = std::move(verticesNode);
    if (!geometry_entry.texcoords.empty()) {
      JsonValue texcoordsNode = array_value();
      texcoordsNode.array.reserve(geometry_entry.texcoords.size());
      for (const auto& texcoord : geometry_entry.texcoords) {
        texcoordsNode.array.push_back(vec2_value(texcoord));
      }
      item.object["texcoords"] = std::move(texcoordsNode);
    }
    JsonValue indicesNode = array_value();
    indicesNode.array.reserve(geometry_entry.indices.size());
    for (const auto index : geometry_entry.indices) {
      indicesNode.array.push_back(number_value(static_cast<double>(index)));
    }
    item.object["indices"] = std::move(indicesNode);
    geometryNode.array.push_back(std::move(item));
  }
  root.object["geometry"] = std::move(geometryNode);

  JsonValue sdfNode = array_value();
  sdfNode.array.reserve(sdf_primitives.size());
  for (const auto& sdf : sdf_primitives) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(sdf.id));
    item.object["shape"] = string_value(sdf.shape);
    item.object["transform"] = transform_value(sdf.transform);
    JsonValue primitiveNode = object_value();
    primitiveNode.object["shape"] = string_value(sdf.primitive.shape);
    primitiveNode.object["radius"] = number_value(sdf.primitive.radius);
    primitiveNode.object["param_a"] = number_value(sdf.primitive.param_a);
    primitiveNode.object["param_b"] = number_value(sdf.primitive.param_b);
    item.object["primitive"] = std::move(primitiveNode);
    sdfNode.array.push_back(std::move(item));
  }
  if (!sdfNode.array.empty()) {
    root.object["sdf_primitives"] = std::move(sdfNode);
  }

  JsonValue particleNode = array_value();
  particleNode.array.reserve(particle_emitters.size());
  for (const auto& emitter : particle_emitters) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(emitter.id));
    item.object["name"] = string_value(emitter.name);
    item.object["type"] = string_value(emitter.type);
    item.object["enabled"] = bool_value(emitter.enabled);
    item.object["transform"] = transform_value(emitter.transform);
    item.object["bounds"] = vec3_value(emitter.bounds);
    item.object["velocity"] = vec3_value(emitter.velocity);
    item.object["velocity_jitter"] = vec3_value(emitter.velocity_jitter);
    item.object["wind"] = vec3_value(emitter.wind);
    item.object["material_id"] = number_value(static_cast<double>(emitter.material_id));
    item.object["count"] = number_value(static_cast<double>(emitter.count));
    item.object["seed"] = number_value(static_cast<double>(emitter.seed));
    item.object["time"] = number_value(emitter.time);
    item.object["lifetime"] = number_value(emitter.lifetime);
    item.object["radius"] = number_value(emitter.radius);
    item.object["length"] = number_value(emitter.length);
    item.object["turbulence"] = number_value(emitter.turbulence);
    item.object["gravity_scale"] = number_value(emitter.gravity_scale);
    item.object["drag"] = number_value(emitter.drag);
    item.object["bounce"] = number_value(emitter.bounce);
    item.object["collision_plane_y"] = number_value(emitter.collision_plane_y);
    item.object["vortex_strength"] = number_value(emitter.vortex_strength);
    particleNode.array.push_back(std::move(item));
  }
  if (!particleNode.array.empty()) {
    root.object["particle_emitters"] = std::move(particleNode);
  }

  JsonValue entitiesNode = array_value();
  entitiesNode.array.reserve(entities.size());
  for (const auto& entity : entities) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(entity.id));
    item.object["name"] = string_value(entity.name);
    if (!entity.visible) {
      item.object["visible"] = bool_value(false);
    }
    if (entity.has_transform) {
      item.object["transform"] = transform_value(entity.transform);
    }
    if (entity.has_camera) {
      item.object["camera"] = camera_value(entity.camera);
    }
    if (entity.has_light) {
      JsonValue lightNode = object_value();
      lightNode.object["type"] = string_value(entity.light.type);
      lightNode.object["color"] = vec3_value(entity.light.color);
      lightNode.object["intensity"] = number_value(entity.light.intensity);
      lightNode.object["radius"] = number_value(entity.light.radius);
      lightNode.object["direction"] = vec3_value(entity.light.direction);
      lightNode.object["beam_angle"] = number_value(entity.light.beam_angle_degrees);
      lightNode.object["blend"] = number_value(entity.light.blend);
      item.object["light"] = std::move(lightNode);
    }
    if (entity.has_mesh) {
      JsonValue meshNode = object_value();
      meshNode.object["mesh_id"] = number_value(static_cast<double>(entity.mesh.mesh_id));
      meshNode.object["material_id"] = number_value(static_cast<double>(entity.mesh.material_id));
      item.object["mesh"] = std::move(meshNode);
    }
    if (entity.has_sdf_primitive) {
      JsonValue sdfPrimitiveNode = object_value();
      sdfPrimitiveNode.object["shape"] = string_value(entity.sdf_primitive.shape);
      sdfPrimitiveNode.object["radius"] = number_value(entity.sdf_primitive.radius);
      sdfPrimitiveNode.object["param_a"] = number_value(entity.sdf_primitive.param_a);
      sdfPrimitiveNode.object["param_b"] = number_value(entity.sdf_primitive.param_b);
      item.object["sdf_primitive"] = std::move(sdfPrimitiveNode);
    }
    if (entity.has_hierarchy || entity.hierarchy.parent != 0 || entity.hierarchy.sibling_order != 0) {
      JsonValue hierarchyNode = object_value();
      hierarchyNode.object["parent"] = number_value(static_cast<double>(entity.hierarchy.parent));
      hierarchyNode.object["sibling_order"] = number_value(static_cast<double>(entity.hierarchy.sibling_order));
      item.object["hierarchy"] = std::move(hierarchyNode);
    }
    if (entity.material.material_id != 0) {
      JsonValue materialNode = object_value();
      materialNode.object["id"] = number_value(static_cast<double>(entity.material.material_id));
      item.object["material"] = std::move(materialNode);
    }
    if (entity.has_physics_body) {
      JsonValue physicsNode = object_value();
      physicsNode.object["enabled"] = bool_value(entity.physics_body.enabled);
      physicsNode.object["mass"] = number_value(entity.physics_body.mass);
      physicsNode.object["dynamic"] = bool_value(entity.physics_body.dynamic);
      physicsNode.object["body_type"] = string_value(entity.physics_body.dynamic ? "dynamic" : entity.physics_body.body_type);
      physicsNode.object["shape"] = string_value(entity.physics_body.shape);
      physicsNode.object["friction"] = number_value(entity.physics_body.friction);
      physicsNode.object["restitution"] = number_value(entity.physics_body.restitution);
      physicsNode.object["gravity_scale"] = number_value(entity.physics_body.gravity_scale);
      physicsNode.object["trigger"] = bool_value(entity.physics_body.trigger);
      physicsNode.object["allow_sleeping"] = bool_value(entity.physics_body.allow_sleeping);
      physicsNode.object["continuous_collision"] = bool_value(entity.physics_body.continuous_collision);
      item.object["physics"] = std::move(physicsNode);
    }
    if (!entity.animation.clip.empty()) {
      JsonValue animNode = object_value();
      animNode.object["clip"] = string_value(entity.animation.clip);
      animNode.object["looping"] = bool_value(entity.animation.looping);
      animNode.object["duration_seconds"] = number_value(entity.animation.duration_seconds);
      animNode.object["playback_speed"] = number_value(entity.animation.playback_speed);
      animNode.object["translation_amplitude"] = vec3_value(entity.animation.translation_amplitude);
      animNode.object["rotation_degrees"] = vec3_value(entity.animation.rotation_degrees);
      animNode.object["scale_amplitude"] = vec3_value(entity.animation.scale_amplitude);
      item.object["animation"] = std::move(animNode);
    }
    if (!entity.script.script.empty()) {
      JsonValue scriptNode = object_value();
      scriptNode.object["source"] = string_value(entity.script.script);
      scriptNode.object["language"] = string_value(entity.script.language);
      scriptNode.object["entry"] = string_value(entity.script.entry);
      scriptNode.object["enabled"] = bool_value(entity.script.enabled);
      scriptNode.object["reload_on_save"] = bool_value(entity.script.reload_on_save);
      item.object["script"] = std::move(scriptNode);
    }
    if (entity.has_benchmark_tag) {
      JsonValue benchmarkTagNode = object_value();
      benchmarkTagNode.object["enabled"] = bool_value(entity.benchmark_tag.enabled);
      item.object["benchmark"] = std::move(benchmarkTagNode);
    }
    entitiesNode.array.push_back(std::move(item));
  }
  root.object["entities"] = std::move(entitiesNode);

  JsonValue transformsNode = array_value();
  transformsNode.array.reserve(transforms.size());
  for (const auto& transform : transforms) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(transform.id));
    item.object["parent"] = number_value(static_cast<double>(transform.parent));
    item.object["transform"] = transform_value(transform.transform);
    transformsNode.array.push_back(std::move(item));
  }
  root.object["transforms"] = std::move(transformsNode);

  JsonValue camerasNode = array_value();
  camerasNode.array.reserve(cameras.size());
  for (const auto& camera : cameras) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(camera.id));
    JsonValue cameraNode = camera_value(camera.camera);
    for (auto& [key, value] : cameraNode.object) {
      item.object[key] = std::move(value);
    }
    camerasNode.array.push_back(std::move(item));
  }
  root.object["cameras"] = std::move(camerasNode);

  JsonValue lightsNode = array_value();
  lightsNode.array.reserve(lights.size());
  for (const auto& light : lights) {
    JsonValue item = object_value();
    item.object["id"] = number_value(static_cast<double>(light.id));
    item.object["type"] = string_value(light.light.type);
    item.object["color"] = vec3_value(light.light.color);
    item.object["intensity"] = number_value(light.light.intensity);
    item.object["radius"] = number_value(light.light.radius);
    item.object["direction"] = vec3_value(light.light.direction);
    item.object["beam_angle"] = number_value(light.light.beam_angle_degrees);
    item.object["blend"] = number_value(light.light.blend);
    lightsNode.array.push_back(std::move(item));
  }
  root.object["lights"] = std::move(lightsNode);

  JsonValue benchmarkNode;
  benchmarkNode.kind = JsonValue::Kind::Object;
  benchmarkNode.object["enabled"] = bool_value(benchmark.enabled);
  benchmarkNode.object["frame_target"] = number_value(static_cast<double>(benchmark.frame_target));
  benchmarkNode.object["warmup_frames"] = number_value(static_cast<double>(benchmark.warmup_frames));
  root.object["benchmark"] = benchmarkNode;

  return stringify(root, pretty);
}

std::string SceneDocument::export_hash_hex() const {
  const auto hash = snapshot().scene_hash;
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(hash.size() * 2);
  for (const auto byte : hash) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

SceneSnapshot SceneDocument::snapshot() const {
  SceneSnapshot out;
  if (auto world = to_world()) {
    // Prefer ECS extraction for hierarchy-aware transforms, then add document-only sections below.
    out = world.value().build_snapshot();
  }

  std::string blob = "scene:" + metadata.schema + ":" + metadata.scene_name + ";";
  out.entity_ids.clear();
  out.renderables.clear();
  out.lights.clear();
  out.entity_ids.reserve(entities.size());
  out.renderables.reserve(entities.size());
  out.lights.reserve(entities.size() + lights.size());
  std::unordered_map<vkpt::core::StableId, const SceneEntityDefinition*> entityById;
  entityById.reserve(entities.size());
  for (const auto& entity : entities) {
    entityById.emplace(entity.id, &entity);
  }
  std::unordered_map<vkpt::core::StableId, bool> visiblePathCache;
  visiblePathCache.reserve(entities.size());
  auto entity_visible_path = [&](const SceneEntityDefinition& entity) {
    if (const auto cached = visiblePathCache.find(entity.id);
        cached != visiblePathCache.end()) {
      return cached->second;
    }
    bool visible = true;
    const auto* current = &entity;
    std::unordered_set<vkpt::core::StableId> visited;
    visited.reserve(8u);
    for (std::size_t depth = 0u;
         current != nullptr && depth <= entities.size() &&
         visited.insert(current->id).second;
         ++depth) {
      if (!current->visible) {
        visible = false;
        break;
      }
      if (current->hierarchy.parent == 0u) {
        break;
      }
      const auto parentIt = entityById.find(current->hierarchy.parent);
      current = parentIt == entityById.end() ? nullptr : parentIt->second;
    }
    visiblePathCache.emplace(entity.id, visible);
    return visible;
  };
  for (const auto& entity : entities) {
    const bool entityVisible = entity_visible_path(entity);
    out.entity_ids.push_back(entity.id);
    blob += "e" + std::to_string(entity.id) + ":" + entity.name + ";";
    if (!entityVisible) {
      blob += "hidden;";
    }
    if (entityVisible && entity.has_mesh) {
      out.renderables.push_back({entity.id, entity.mesh.mesh_id, entity.mesh.material_id, entity.transform});
      blob += "m" + std::to_string(entity.mesh.mesh_id) + ":" + std::to_string(entity.mesh.material_id) + ";";
    }
    if (entityVisible && entity.has_light) {
      out.lights.push_back({entity.id, entity.light, entity.transform});
      blob += "l" + entity.light.type + ":" + std::to_string(entity.light.intensity) + ":" +
              std::to_string(entity.light.beam_angle_degrees) + ":" +
              std::to_string(entity.light.direction.x) + "," +
              std::to_string(entity.light.direction.y) + "," +
              std::to_string(entity.light.direction.z) + ";";
    }
    if (entityVisible && entity.has_sdf_primitive) {
      blob += "sdfEntity" + std::to_string(entity.id) + ":" + entity.sdf_primitive.shape + ":" +
              std::to_string(entity.sdf_primitive.radius) + ";";
    }
    if (entityVisible && entity.has_camera && !out.camera) {
      out.camera = SceneCameraDefinition{entity.id, entity.camera};
      blob += "c" + std::to_string(entity.id) + ":" + camera_hash_blob(entity.camera) + ";";
    }
    if (entity.has_physics_body) {
      blob += "p" + std::to_string(entity.id) + ":" +
              (entity.physics_body.enabled ? "1" : "0") + ":" +
              (entity.physics_body.dynamic ? "dynamic" : "static") + ":" +
              entity.physics_body.shape + ":" +
              std::to_string(entity.physics_body.mass) + ";";
    }
  }
  out.materials.clear();
  out.materials.reserve(materials.size());
  for (const auto& material : materials) {
    out.materials.push_back({material.id, material});
    blob += "mat" + std::to_string(material.id) + ":" + material.name + ":" +
            material.family + ":" + std::to_string(material.roughness) + ":" +
            std::to_string(material.metallic) + ":" + std::to_string(material.transmission) + ":" +
            std::to_string(material.clearcoat) + ":" + std::to_string(material.sheen) + ":" +
            std::to_string(material.anisotropy) + ":" + std::to_string(material.alpha) + ":" +
            (material.double_sided ? "2s" : "1s") + ";";
  }
  out.asset_refs.clear();
  out.asset_refs.reserve(assets.size());
  for (const auto& asset : assets) {
    out.asset_refs.push_back(asset.uri);
    blob += "a" + std::to_string(asset.id) + ":" + asset.uri + ":" +
            asset.name + ":p" + std::to_string(asset.parent) + ":o" +
            std::to_string(asset.sibling_order) + ";";
  }
  for (const auto& camera : cameras) {
    if (!out.camera) {
      out.camera = camera;
    }
    blob += "c" + std::to_string(camera.id) + ":" + camera_hash_blob(camera.camera) + ";";
  }
  for (const auto& geometry_entry : geometry) {
    blob += "g" + std::to_string(geometry_entry.id) + ":" + geometry_entry.primitive + ":" +
            std::to_string(geometry_entry.material_id) + ":" +
            std::to_string(geometry_entry.vertices.size()) + ":" +
            std::to_string(geometry_entry.indices.size()) + ":" +
            (geometry_entry.tessellation.enabled ? "tess1" : "tess0") + ":" +
            geometry_entry.tessellation.mode + ":" +
            std::to_string(geometry_entry.tessellation.factor) + ":" +
            (geometry_entry.tessellation.gpu_preferred ? "gpu" : "cpu") + ":" +
            (geometry_entry.tessellation.cache_generated_geometry ? "cache" : "nocache") + ":" +
            (geometry_entry.tessellation.displacement ? "disp" : "nodisp") + ":" +
            geometry_entry.tessellation.projection + ";";
  }
  for (const auto& transform : transforms) {
    blob += "x" + std::to_string(transform.id) + ":" + std::to_string(transform.parent) + ":" +
            std::to_string(transform.transform.translation.x) + "," +
            std::to_string(transform.transform.translation.y) + "," +
            std::to_string(transform.transform.translation.z) + ";";
  }
  for (const auto& light : lights) {
    blob += "L" + std::to_string(light.id) + ":" + light.light.type + ":" +
            std::to_string(light.light.intensity) + ":" +
            std::to_string(light.light.beam_angle_degrees) + ":" +
            std::to_string(light.light.direction.x) + "," +
            std::to_string(light.light.direction.y) + "," +
            std::to_string(light.light.direction.z) + ";";
  }
  for (const auto& sdf : sdf_primitives) {
    blob += "sdf" + std::to_string(sdf.id) + ":" + sdf.shape + ":" +
            std::to_string(sdf.primitive.radius) + ";";
  }
  for (const auto& emitter : particle_emitters) {
    blob += "pt" + std::to_string(emitter.id) + ":" + emitter.type + ":" +
            std::to_string(emitter.enabled ? 1 : 0) + ":" +
            std::to_string(emitter.count) + ":" +
            std::to_string(emitter.seed) + ":" +
            std::to_string(emitter.time) + ":" +
            std::to_string(emitter.lifetime) + ":" +
            std::to_string(emitter.radius) + ":" +
            std::to_string(emitter.length) + ":" +
            std::to_string(emitter.gravity_scale) + ":" +
            std::to_string(emitter.drag) + ":" +
            std::to_string(emitter.bounce) + ":" +
            std::to_string(emitter.vortex_strength) + ":" +
            std::to_string(emitter.bounds.x) + "," +
            std::to_string(emitter.bounds.y) + "," +
            std::to_string(emitter.bounds.z) + ";";
  }
  out.benchmark = benchmark;
  if (out.benchmark.enabled) {
    blob += "b";
    blob += std::to_string(out.benchmark.frame_target);
    blob += std::to_string(out.benchmark.warmup_frames);
  }
  out.scene_hash = hash_scene_blob(blob);
  return out;
}

RenderSceneProxy SceneDocument::extract_render_scene(vkpt::core::FrameIndex frame) const {
  RenderSceneProxy proxy;
  const auto snap = snapshot();
  proxy.scene_hash = snap.scene_hash;
  proxy.frame = frame;
  proxy.benchmark = benchmark;

  if (auto loaded = to_world()) {
    // World extraction resolves hierarchy and transform authority; document material data is merged afterward.
    proxy = loaded.value().extract_render_scene(frame);
    proxy.scene_hash = snap.scene_hash;
    proxy.benchmark = benchmark;
  }

  proxy.materials.clear();
  proxy.materials.reserve(materials.size());
  for (const auto& material : materials) {
    proxy.materials.push_back(RenderSceneProxy::Material{
        material.id,
        material.albedo,
        material.roughness,
        material.emission,
        material.emission_intensity});
  }

  if (!proxy.camera && snap.camera) {
    RenderSceneProxy::Camera camera;
    camera.entity_id = snap.camera->id;
    camera.fov = snap.camera->camera.fov;
    camera.near_plane = snap.camera->camera.near_plane;
    camera.far_plane = snap.camera->camera.far_plane;
    camera.focal_length_mm = snap.camera->camera.focal_length_mm;
    camera.sensor_width_mm = snap.camera->camera.sensor_width_mm;
    camera.sensor_height_mm = snap.camera->camera.sensor_height_mm;
    camera.aperture_radius = snap.camera->camera.aperture_radius;
    camera.focus_distance = snap.camera->camera.focus_distance;
    camera.f_stop = snap.camera->camera.f_stop;
    camera.shutter_seconds = snap.camera->camera.shutter_seconds;
    camera.iso = snap.camera->camera.iso;
    camera.exposure_compensation = snap.camera->camera.exposure_compensation;
    camera.white_balance_kelvin = snap.camera->camera.white_balance_kelvin;
    camera.iris_blade_count = snap.camera->camera.iris_blade_count;
    camera.iris_rotation_degrees = snap.camera->camera.iris_rotation_degrees;
    camera.iris_roundness = snap.camera->camera.iris_roundness;
    camera.anamorphic_squeeze = snap.camera->camera.anamorphic_squeeze;
    camera.world_matrix = identity_matrix();
    proxy.camera = camera;
  }

  return proxy;
}

}  // namespace vkpt::scene
