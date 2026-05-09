#include "scene/Scene.h"

#include "core/Logging.h"
#include "assets/SceneAssetLoader.h"
#include "scene/Json.h"
#include "scene/SceneInternal.h"

#include <algorithm>
#include <cctype>
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

namespace {

std::string LowerCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char ch : text) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

std::string ExtensionOf(std::string_view uri) {
  auto ext = std::filesystem::path(std::string(uri)).extension().generic_string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext;
}

bool IsStandaloneImageLikeAsset(const SceneAssetDefinition& asset) {
  const auto type = LowerCopy(asset.type);
  const auto ext = ExtensionOf(asset.uri);
  const bool imageExt = ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                        ext == ".tga" || ext == ".exr" || ext == ".hdr";
  return imageExt &&
         (type == "texture" || type == "image" || type == "environment/hdri" ||
          type == "environment_hdri" || type == "hdri" || type == "environment" ||
          type == "environment/sky" || type == "environment_sky");
}

void ReadScriptComponent(const JsonValue& object, ScriptComponent& script) {
  if (object.kind != JsonValue::Kind::Object) {
    return;
  }
  // `path` is a legacy alias. Export normalizes the field back to `source`.
  read_string(object, "source", script.script);
  if (script.script.empty()) {
    read_string(object, "path", script.script);
  }
  read_string(object, "language", script.language);
  read_string(object, "entry", script.entry);
  read_string(object, "module", script.module_id);
  read_string(object, "module_id", script.module_id);
  read_bool(object, "enabled", script.enabled);
  read_bool(object, "reload_on_save", script.reload_on_save);
  if (const auto paramsNode = object.object.find("params");
      paramsNode != object.object.end() &&
      paramsNode->second.kind == JsonValue::Kind::Object) {
    script.params.clear();
    for (const auto& [key, value] : paramsNode->second.object) {
      if (value.kind == JsonValue::Kind::String) {
        script.params[key] = value.string;
      } else if (value.kind == JsonValue::Kind::Number) {
        script.params[key] = std::to_string(value.number);
      } else if (value.kind == JsonValue::Kind::Boolean) {
        script.params[key] = value.boolean ? "true" : "false";
      } else {
        script.params[key] = stringify(value, false);
      }
    }
  }
}

void ResolveStandaloneAssetUris(SceneDocument& document, const std::filesystem::path& scene_path) {
  const auto sceneDir = scene_path.has_parent_path()
                            ? scene_path.parent_path()
                            : std::filesystem::current_path();
  for (auto& asset : document.assets) {
    if (asset.uri.empty() || !IsStandaloneImageLikeAsset(asset)) {
      continue;
    }
    const std::filesystem::path requested{asset.uri};
    if (requested.is_absolute()) {
      asset.uri = requested.lexically_normal().generic_string();
      continue;
    }
    std::error_code ec;
    const auto sceneRelative = (sceneDir / requested).lexically_normal();
    if (std::filesystem::exists(sceneRelative, ec) && !ec) {
      asset.uri = sceneRelative.generic_string();
      continue;
    }
    ec.clear();
    const auto cwdRelative = (std::filesystem::current_path() / requested).lexically_normal();
    if (std::filesystem::exists(cwdRelative, ec) && !ec) {
      asset.uri = cwdRelative.generic_string();
    }
  }
}

}  // namespace

vkpt::core::Result<SceneDocument> SceneDocument::load_from_text(std::string_view text) {
  const auto root = JsonParser::parse(text);
  if (!root || root->kind != JsonValue::Kind::Object) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  SceneDocument doc;
  std::unordered_set<std::uint64_t> usedIds;
  const auto& rootObj = *root;
  // Sparse documents are allowed, but sections that are present must have the expected JSON kind.
  auto require_kind_if_present = [&](std::string_view key, JsonValue::Kind kind) {
    const auto it = rootObj.object.find(key);
    return it == rootObj.object.end() || it->second.kind == kind;
  };
  if (!require_kind_if_present("schema", JsonValue::Kind::String) ||
      !require_kind_if_present("metadata", JsonValue::Kind::Object) ||
      !require_kind_if_present("assets", JsonValue::Kind::Array) ||
      !require_kind_if_present("materials", JsonValue::Kind::Array) ||
      !require_kind_if_present("geometry", JsonValue::Kind::Array) ||
      !require_kind_if_present("sdf_primitives", JsonValue::Kind::Array) ||
      !require_kind_if_present("particle_emitters", JsonValue::Kind::Array) ||
      !require_kind_if_present("entities", JsonValue::Kind::Array) ||
      !require_kind_if_present("transforms", JsonValue::Kind::Array) ||
      !require_kind_if_present("cameras", JsonValue::Kind::Array) ||
      !require_kind_if_present("lights", JsonValue::Kind::Array) ||
      !require_kind_if_present("scene_script", JsonValue::Kind::Object) ||
      !require_kind_if_present("performance_culling", JsonValue::Kind::Object) ||
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

  if (const auto scriptNode = rootObj.object.find("scene_script"); scriptNode != rootObj.object.end()) {
    doc.has_scene_script = true;
    ReadScriptComponent(scriptNode->second, doc.scene_script);
  }

  if (const auto assetsNode = rootObj.object.find("assets"); assetsNode != rootObj.object.end() &&
      assetsNode->second.kind == JsonValue::Kind::Array) {
    doc.assets.reserve(assetsNode->second.array.size());
    usedIds.reserve(assetsNode->second.array.size());
    for (const auto& item : assetsNode->second.array) {
      SceneAssetDefinition asset;
      read_u64(item, "id", asset.id);
      read_string(item, "type", asset.type);
      read_string(item, "uri", asset.uri);
      read_string(item, "name", asset.name);
      read_u64(item, "parent", asset.parent);
      read_u32(item, "sibling_order", asset.sibling_order);
      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        asset.has_transform = true;
        asset.transform = read_transform(transformNode->second);
      }
      if (asset.id == 0) {
        // Authored assets may omit IDs; allocate locally before later reference validation.
        asset.id = allocate_id(usedIds);
      }
      usedIds.insert(asset.id);
      doc.assets.push_back(std::move(asset));
    }
  }

  usedIds.clear();
  if (const auto materialsNode = rootObj.object.find("materials"); materialsNode != rootObj.object.end() &&
      materialsNode->second.kind == JsonValue::Kind::Array) {
    doc.materials.reserve(materialsNode->second.array.size());
    usedIds.reserve(materialsNode->second.array.size());
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
      if (item.object.find("albedo") != item.object.end()) {
        read_vec3(item, "albedo", material.albedo);
      }
      if (item.object.find("emission") != item.object.end()) {
        read_vec3(item, "emission", material.emission);
      }
      if (item.object.find("emission_intensity") != item.object.end()) {
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
    doc.geometry.reserve(geometryNode->second.array.size());
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
    doc.sdf_primitives.reserve(sdfNode->second.array.size());
    usedIds.reserve(sdfNode->second.array.size());
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
  if (const auto particlesNode = rootObj.object.find("particle_emitters");
      particlesNode != rootObj.object.end() &&
      particlesNode->second.kind == JsonValue::Kind::Array) {
    doc.particle_emitters.reserve(particlesNode->second.array.size());
    usedIds.reserve(particlesNode->second.array.size());
    for (const auto& item : particlesNode->second.array) {
      SceneParticleEmitterDefinition emitter;
      read_u64(item, "id", emitter.id);
      read_string(item, "name", emitter.name);
      read_string(item, "type", emitter.type);
      read_bool(item, "enabled", emitter.enabled);
      read_u64(item, "material_id", emitter.material_id);
      read_u32(item, "count", emitter.count);
      read_u32(item, "seed", emitter.seed);
      read_float(item, "time", emitter.time);
      read_float(item, "lifetime", emitter.lifetime);
      read_float(item, "radius", emitter.radius);
      read_float(item, "length", emitter.length);
      read_float(item, "turbulence", emitter.turbulence);
      read_float(item, "gravity_scale", emitter.gravity_scale);
      read_float(item, "drag", emitter.drag);
      read_float(item, "bounce", emitter.bounce);
      read_float(item, "collision_plane_y", emitter.collision_plane_y);
      read_float(item, "vortex_strength", emitter.vortex_strength);
      read_vec3(item, "bounds", emitter.bounds);
      read_vec3(item, "velocity", emitter.velocity);
      read_vec3(item, "velocity_jitter", emitter.velocity_jitter);
      read_vec3(item, "wind", emitter.wind);
      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        emitter.transform = read_transform(transformNode->second);
      }
      if (emitter.id == 0) {
        emitter.id = allocate_id(usedIds);
      }
      usedIds.insert(emitter.id);
      doc.particle_emitters.push_back(std::move(emitter));
    }
  }

  usedIds.clear();
  if (const auto entitiesNode = rootObj.object.find("entities"); entitiesNode != rootObj.object.end() &&
      entitiesNode->second.kind == JsonValue::Kind::Array) {
    doc.entities.reserve(entitiesNode->second.array.size());
    usedIds.reserve(entitiesNode->second.array.size());
    for (const auto& item : entitiesNode->second.array) {
      SceneEntityDefinition entity;
      read_u64(item, "id", entity.id);
      if (entity.id == 0) {
        entity.id = allocate_id(usedIds);
      }
      usedIds.insert(entity.id);
      read_string(item, "name", entity.name);
      read_bool(item, "visible", entity.visible);

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
      if (const auto scriptNode = item.object.find("script"); scriptNode != item.object.end()) {
        ReadScriptComponent(scriptNode->second, entity.script);
      }
      if (const auto listenerNode = item.object.find("audio_listener"); listenerNode != item.object.end()) {
        entity.has_audio_listener = true;
        read_bool(listenerNode->second, "enabled", entity.audio_listener.enabled);
        read_bool(listenerNode->second, "primary", entity.audio_listener.primary);
      }
      if (const auto emitterNode = item.object.find("audio_emitter"); emitterNode != item.object.end()) {
        entity.has_audio_emitter = true;
        read_string(emitterNode->second, "event", entity.audio_emitter.event);
        read_string(emitterNode->second, "bus", entity.audio_emitter.bus);
        read_bool(emitterNode->second, "enabled", entity.audio_emitter.enabled);
        read_bool(emitterNode->second, "autoplay", entity.audio_emitter.autoplay);
        read_bool(emitterNode->second, "loop", entity.audio_emitter.loop);
        read_bool(emitterNode->second, "spatial", entity.audio_emitter.spatial);
        read_float(emitterNode->second, "volume", entity.audio_emitter.volume);
        read_float(emitterNode->second, "pitch", entity.audio_emitter.pitch);
        read_float(emitterNode->second, "min_distance", entity.audio_emitter.min_distance);
        read_float(emitterNode->second, "max_distance", entity.audio_emitter.max_distance);
      }
      if (const auto panelNode = item.object.find("ui_panel"); panelNode != item.object.end()) {
        entity.has_ui_panel = true;
        read_ui_panel_component(panelNode->second, entity.ui_panel);
      }
      if (const auto benchmarkNode = item.object.find("benchmark"); benchmarkNode != item.object.end()) {
        entity.has_benchmark_tag = true;
        read_bool(benchmarkNode->second, "enabled", entity.benchmark_tag.enabled);
      }
      if (const auto skeletonIt = item.object.find("skeleton");
          skeletonIt != item.object.end() &&
          skeletonIt->second.kind == JsonValue::Kind::Object) {
        // Phase 1 ANI01 read path. We deliberately accept missing fields rather
        // than reject so partial scenes keep loading; validate() is the gate.
        entity.has_skeleton = true;
        const auto& skeletonNode = skeletonIt->second;
        std::int64_t root_index_signed = -1;
        if (const auto rootIt = skeletonNode.object.find("root_index");
            rootIt != skeletonNode.object.end() &&
            rootIt->second.kind == JsonValue::Kind::Number) {
          root_index_signed = static_cast<std::int64_t>(rootIt->second.number);
        }
        entity.skeleton.root_index = static_cast<std::int32_t>(root_index_signed);
        if (const auto jointsIt = skeletonNode.object.find("joints");
            jointsIt != skeletonNode.object.end() &&
            jointsIt->second.kind == JsonValue::Kind::Array) {
          entity.skeleton.joints.clear();
          entity.skeleton.joints.reserve(jointsIt->second.array.size());
          for (const auto& jointNode : jointsIt->second.array) {
            vkpt::animation::Joint joint;
            read_string(jointNode, "name", joint.name);
            std::int64_t parent = -1;
            if (const auto parentIt = jointNode.object.find("parent");
                parentIt != jointNode.object.end() &&
                parentIt->second.kind == JsonValue::Kind::Number) {
              parent = static_cast<std::int64_t>(parentIt->second.number);
            }
            joint.parent_index = static_cast<std::int32_t>(parent);
            if (const auto ibmIt = jointNode.object.find("inverse_bind");
                ibmIt != jointNode.object.end() &&
                ibmIt->second.kind == JsonValue::Kind::Array &&
                ibmIt->second.array.size() == 16u) {
              for (std::size_t k = 0; k < 16u; ++k) {
                if (ibmIt->second.array[k].kind == JsonValue::Kind::Number) {
                  joint.inverse_bind.values[k] =
                      static_cast<float>(ibmIt->second.array[k].number);
                }
              }
            }
            if (const auto localIt = jointNode.object.find("bind_local");
                localIt != jointNode.object.end() &&
                localIt->second.kind == JsonValue::Kind::Object) {
              joint.bind_local = read_transform(localIt->second);
            }
            entity.skeleton.joints.push_back(std::move(joint));
          }
        }
      }
      doc.entities.push_back(std::move(entity));
    }
  }

  if (const auto transformsNode = rootObj.object.find("transforms"); transformsNode != rootObj.object.end() &&
      transformsNode->second.kind == JsonValue::Kind::Array) {
    doc.transforms.reserve(transformsNode->second.array.size());
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
    doc.cameras.reserve(camerasNode->second.array.size());
    for (const auto& item : camerasNode->second.array) {
      SceneCameraDefinition cam;
      read_u64(item, "id", cam.id);
      read_camera_component(item, cam.camera);
      doc.cameras.push_back(std::move(cam));
    }
  }

  if (const auto lightsNode = rootObj.object.find("lights"); lightsNode != rootObj.object.end() &&
      lightsNode->second.kind == JsonValue::Kind::Array) {
    doc.lights.reserve(lightsNode->second.array.size());
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

  if (const auto cullingNode = rootObj.object.find("performance_culling");
      cullingNode != rootObj.object.end()) {
    read_bool(cullingNode->second, "enabled", doc.performance_culling.enabled);
    read_bool(cullingNode->second, "frustum", doc.performance_culling.frustum);
    read_bool(cullingNode->second, "distance", doc.performance_culling.distance);
    read_bool(cullingNode->second, "cull_dynamic", doc.performance_culling.cull_dynamic);
    read_float(cullingNode->second, "max_distance", doc.performance_culling.max_distance);
    read_float(cullingNode->second, "frustum_padding", doc.performance_culling.frustum_padding);
    read_float(cullingNode->second, "aspect_ratio", doc.performance_culling.aspect_ratio);
    read_float(cullingNode->second, "min_instance_radius",
               doc.performance_culling.min_instance_radius);
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
  // Disk loads expand relative asset references before the second validation pass.
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
  ResolveStandaloneAssetUris(document, std::filesystem::path(std::string(path)));
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
  entityIds.reserve(entities.size());
  materialIds.reserve(materials.size());
  geometryIds.reserve(geometry.size());
  assetIds.reserve(assets.size());
  sdfIds.reserve(sdf_primitives.size());
  parentByEntity.reserve(entities.size() + transforms.size());

  // Lookup sets make the later cross-section checks deterministic and order independent.
  if (metadata.schema.empty()) {
    report("metadata schema is empty");
  } else if (metadata.schema != "1.0") {
    report("unsupported schema " + metadata.schema);
  }
  if (has_scene_script && (!scene_script.script.empty() || !scene_script.params.empty())) {
    if (!scene_script.language.empty() && scene_script.language != "lua") {
      report("scene script language is unsupported");
    }
    for (const auto& [key, value] : scene_script.params) {
      if (key.empty()) {
        report("scene script parameter key is empty");
      }
      if (value.size() > 4096u) {
        report("scene script parameter value is too large");
      }
    }
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
    if (asset.has_transform && !valid_transform_values(asset.transform)) {
      report("asset transform has invalid values " + std::to_string(asset.id));
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

  std::unordered_set<vkpt::core::StableId> particleIds;
  particleIds.reserve(particle_emitters.size());
  for (const auto& emitter : particle_emitters) {
    if (emitter.id == 0) {
      report("particle emitter id is zero");
    }
    if (!particleIds.insert(emitter.id).second) {
      report("duplicate particle emitter id " + std::to_string(emitter.id));
    }
    const std::string type = LowerCopy(emitter.type);
    if (type != "rain" && type != "smoke") {
      report("particle emitter type is unsupported " + std::to_string(emitter.id));
    }
    if (!valid_transform_values(emitter.transform) ||
        !finite_vec3(emitter.bounds) ||
        !finite_vec3(emitter.velocity) ||
        !finite_vec3(emitter.velocity_jitter) ||
        !finite_vec3(emitter.wind)) {
      report("particle emitter contains invalid vectors " + std::to_string(emitter.id));
    }
    if (emitter.bounds.x < 0.0f || emitter.bounds.y < 0.0f || emitter.bounds.z < 0.0f) {
      report("particle emitter bounds are negative " + std::to_string(emitter.id));
    }
    if (emitter.count > 20000u) {
      report("particle emitter count exceeds limit " + std::to_string(emitter.id));
    }
    if (!std::isfinite(emitter.time) ||
        !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0.0f ||
        !std::isfinite(emitter.radius) || emitter.radius <= 0.0f ||
        !std::isfinite(emitter.length) || emitter.length < 0.0f ||
        !std::isfinite(emitter.turbulence) || emitter.turbulence < 0.0f ||
        !std::isfinite(emitter.gravity_scale) ||
        !std::isfinite(emitter.drag) || emitter.drag < 0.0f ||
        !std::isfinite(emitter.bounce) || emitter.bounce < 0.0f || emitter.bounce > 1.0f ||
        !std::isfinite(emitter.collision_plane_y) ||
        !std::isfinite(emitter.vortex_strength)) {
      report("particle emitter scalar values are invalid " + std::to_string(emitter.id));
    }
    if (emitter.material_id != 0 && !materialIds.empty() && !materialIds.contains(emitter.material_id)) {
      report("particle emitter references missing material " + std::to_string(emitter.material_id));
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
    if (!entity.script.script.empty() || !entity.script.params.empty()) {
      if (!entity.script.language.empty() && entity.script.language != "lua") {
        report("entity script language is unsupported " + std::to_string(entity.id));
      }
      for (const auto& [key, value] : entity.script.params) {
        if (key.empty()) {
          report("entity script parameter key is empty " + std::to_string(entity.id));
        }
        if (value.size() > 4096u) {
          report("entity script parameter value is too large " + std::to_string(entity.id));
        }
      }
    }
    if (entity.has_audio_emitter) {
      if (entity.audio_emitter.event.empty()) {
        report("entity audio emitter event is empty " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.audio_emitter.volume) || entity.audio_emitter.volume < 0.0f) {
        report("entity audio emitter volume is invalid " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.audio_emitter.pitch) ||
          entity.audio_emitter.pitch <= 0.0f ||
          entity.audio_emitter.pitch > 4.0f) {
        report("entity audio emitter pitch is invalid " + std::to_string(entity.id));
      }
      if (!std::isfinite(entity.audio_emitter.min_distance) ||
          !std::isfinite(entity.audio_emitter.max_distance) ||
          entity.audio_emitter.min_distance < 0.0f ||
          entity.audio_emitter.max_distance <= entity.audio_emitter.min_distance) {
        report("entity audio emitter distance range is invalid " + std::to_string(entity.id));
      }
    }
    if (entity.has_ui_panel) {
      if (!valid_ui_panel_values(entity.ui_panel)) {
        report("entity ui panel has invalid values " + std::to_string(entity.id));
      }
    }
    if (entity.has_light &&
        (!finite_vec3(entity.light.color) || !std::isfinite(entity.light.intensity) || entity.light.intensity < 0.0f ||
         !std::isfinite(entity.light.radius) || entity.light.radius < 0.0f ||
         !finite_vec3(entity.light.direction) || !std::isfinite(entity.light.beam_angle_degrees) ||
         entity.light.beam_angle_degrees <= 0.0f || !std::isfinite(entity.light.blend) ||
         entity.light.blend < 0.0f)) {
      report("entity light has invalid values " + std::to_string(entity.id));
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
    // Walk parent links for each child; a repeated node marks a hierarchy cycle.
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

  for (const auto& asset : assets) {
    if (asset.parent != 0 && !entityIds.contains(asset.parent)) {
      report("asset import references missing parent " + std::to_string(asset.parent));
    }
  }

  if (benchmark.enabled && benchmark.frame_target != 0 && benchmark.warmup_frames > benchmark.frame_target) {
    report("benchmark warmup_frames exceeds frame_target");
  }
  if (!std::isfinite(performance_culling.max_distance) ||
      performance_culling.max_distance < 0.0f) {
    report("performance_culling max_distance is invalid");
  }
  if (!std::isfinite(performance_culling.frustum_padding) ||
      performance_culling.frustum_padding <= 0.0f) {
    report("performance_culling frustum_padding is invalid");
  }
  if (!std::isfinite(performance_culling.aspect_ratio) ||
      performance_culling.aspect_ratio <= 0.0f) {
    report("performance_culling aspect_ratio is invalid");
  }
  if (!std::isfinite(performance_culling.min_instance_radius) ||
      performance_culling.min_instance_radius < 0.0f) {
    report("performance_culling min_instance_radius is invalid");
  }
  return ok;
}

bool SceneDocument::has_section(std::string_view name) const {
  return name == "schema" || name == "metadata" || name == "assets" || name == "materials" ||
      name == "geometry" || name == "sdf_primitives" || name == "particle_emitters" || name == "entities" ||
      name == "transforms" || name == "cameras" || name == "lights" || name == "scene_script" ||
      name == "benchmark" || name == "performance_culling";
}

}  // namespace vkpt::scene
