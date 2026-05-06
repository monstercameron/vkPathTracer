#include "scene/Scene.h"

#include "core/Logging.h"
#include "scene/SceneInternal.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

namespace vkpt::scene {

using namespace detail;

bool SceneWorld::set_transform(vkpt::core::StableId id,
                              const TransformComponent& transform,
                              TransformAuthority authority,
                              std::string_view writer,
                              vkpt::core::FrameIndex frame) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  const auto existing = m_transformAuthority.find(id);
  if (existing != m_transformAuthority.end() && existing->second.frame == frame) {
    // Same-frame writers arbitrate by authority rank, then stable writer name for deterministic ties.
    const auto previous_rank = authority_rank(existing->second.authority);
    const auto next_rank = authority_rank(authority);
    const std::string_view existing_writer(existing->second.writer);
    const bool writer_conflict = existing_writer != writer;
    const bool lower_authority = next_rank < previous_rank;
    const bool tie_loses = next_rank == previous_rank && writer_conflict && writer.compare(existing_writer) > 0;
    if (writer_conflict || lower_authority) {
      const auto selected = (lower_authority || tie_loses) ? existing->second.authority : authority;
      m_authority_conflicts.push_back(
          {id, existing->second.writer, std::string(writer), selected, frame});
      vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "scene", "transform authority conflict",
                                         {{"entity", std::to_string(id)},
                                          {"writer_a", existing->second.writer},
                                          {"writer_b", std::string(writer)},
                                          {"selected", std::string(to_string(selected))},
                                          {"frame", std::to_string(frame)}});
    }
    if (lower_authority || tie_loses) {
      return false;
    }
  }
  record->transform = transform;
  m_transformAuthority[id] = {authority, frame, std::string(writer)};
  mark_dirty_recursive(id);
  return true;
}

WorldTransform SceneWorld::compute_world_transform_unchecked(const EntityRecord* entity) const {
  WorldTransform out{};
  if (!entity || !entity->transform.has_value()) {
    out.world_matrix = identity_matrix();
    return out;
  }
  out = {};
  out.translation = entity->transform->translation;
  out.rotation = normalize_quat(entity->transform->rotation);
  out.scale = entity->transform->scale;
  out.world_matrix = make_transform_matrix(*entity->transform);
  out.dirty = false;
  auto get_parent_transform = [&](vkpt::core::StableId parent_id) -> WorldTransform {
    // Prefer cached parents but recurse for callers that need immediate, pre-recompute answers.
    WorldTransform parent{};
    parent.world_matrix = identity_matrix();
    const auto it = m_worldTransforms.find(parent_id);
    if (it != m_worldTransforms.end()) {
      return it->second;
    }
    const auto parentIt = m_entities.find(parent_id);
    if (parentIt == m_entities.end()) {
      return parent;
    }
    return compute_world_transform_unchecked(&parentIt->second);
  };
  if (entity->hierarchy && entity->hierarchy->parent != 0) {
    const auto parentId = entity->hierarchy->parent;
    const auto parentTransform = get_parent_transform(parentId);
    const auto localMatrix = out.world_matrix;
    out.world_matrix = multiply_matrix(parentTransform.world_matrix, localMatrix);
    out.translation = matrix_translation(out.world_matrix);
    out.rotation = multiply_quat(parentTransform.rotation, out.rotation);
    out.scale = {parentTransform.scale.x * out.scale.x,
                 parentTransform.scale.y * out.scale.y,
                 parentTransform.scale.z * out.scale.z};
  }
  return out;
}

void SceneWorld::recompute_world_transforms() {
  m_worldTransforms.clear();
  m_worldTransforms.reserve(m_entities_order.size());
  // Entity order is stable; parent recursion handles hierarchies even when parents appear later.
  for (auto id : m_entities_order) {
    auto* entity = get_entity(id);
    if (!entity || !entity->transform.has_value()) {
      continue;
    }
    if (entity->transform->dirty || !m_worldTransforms.contains(id)) {
      m_worldTransforms[id] = compute_world_transform_unchecked(entity);
      m_entities[id].transform->dirty = false;
    }
  }
}

const WorldTransform* SceneWorld::world_transform(vkpt::core::StableId id) const {
  const auto it = m_worldTransforms.find(id);
  if (it == m_worldTransforms.end()) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<WorldAuthorityConflict>& SceneWorld::authority_conflicts() const {
  return m_authority_conflicts;
}

bool SceneWorld::has_authority(vkpt::core::StableId id,
                               TransformAuthority authority,
                               std::string_view writer,
                               vkpt::core::FrameIndex frame) const {
  const auto existing = m_transformAuthority.find(id);
  if (existing == m_transformAuthority.end() || existing->second.frame != frame) {
    return true;
  }
  const auto previous_rank = authority_rank(existing->second.authority);
  const auto next_rank = authority_rank(authority);
  if (next_rank != previous_rank) {
    return next_rank > previous_rank;
  }
  const std::string_view existing_writer(existing->second.writer);
  return existing_writer == writer || writer.compare(existing_writer) < 0;
}

uint32_t SceneWorld::kind_mask(ComponentKind kind) const {
  return component_kind_mask(kind);
}

SceneSnapshot SceneWorld::build_snapshot() const {
  SceneSnapshot out;
  out.benchmark = {};
  out.entity_ids.reserve(m_entities_order.size());
  out.renderables.reserve(m_entities_order.size());
  out.lights.reserve(m_entities_order.size());
  out.materials.reserve(m_entities_order.size());
  out.asset_refs.reserve(m_entities_order.size());
  std::unordered_set<vkpt::core::StableId> snapshot_material_ids;
  snapshot_material_ids.reserve(m_entities_order.size());
  std::string blob = "scene:";
  for (const auto id : m_entities_order) {
    const auto* entity = get_entity(id);
    if (!entity) {
      continue;
    }
    out.entity_ids.push_back(id);
    blob += "e" + std::to_string(id) + ":" + entity->identity.name + ";";
    // Resolve lazily so snapshots work even when callers forgot to rebuild the transform cache.
    auto resolve_world = [&]() {
      WorldTransform world{};
      world.world_matrix = identity_matrix();
      if (const auto* wt = world_transform(id)) {
        world = *wt;
      } else {
        world = compute_world_transform_unchecked(entity);
      }
      return world;
    };
    if (entity->mesh_renderer.has_value()) {
      const auto world = resolve_world();
      out.renderables.push_back(SceneSnapshot::RenderableObject{
          id, entity->mesh_renderer->mesh_id, entity->mesh_renderer->material_id, world});
      blob += "r" + std::to_string(entity->mesh_renderer->mesh_id) + ":" +
              std::to_string(entity->mesh_renderer->material_id) + ";";
      blob += "t" + std::to_string(world.translation.x) + "," + std::to_string(world.translation.y) + "," +
              std::to_string(world.translation.z) + ";";
    }
    if (entity->light.has_value()) {
      const auto world = resolve_world();
      out.lights.push_back(
          SceneSnapshot::LightObject{id, *entity->light, world});
      blob += "l" + entity->light->type + ":" + std::to_string(entity->light->intensity) + ";";
    }
    if (entity->material_override.has_value() &&
        snapshot_material_ids.insert(entity->material_override->material_id).second) {
      SceneSnapshot::MaterialObject material;
      material.id = entity->material_override->material_id;
      out.materials.push_back(material);
      blob += "mat" + std::to_string(material.id) + ";";
    }
  }
  for (const auto id : m_entities_order) {
    const auto* cameraEnt = get_entity(id);
    if (cameraEnt && cameraEnt->camera.has_value()) {
      out.camera = SceneCameraDefinition{id, *cameraEnt->camera};
      blob += "c" + std::to_string(id) + ":" + camera_hash_blob(*cameraEnt->camera) + ";";
      break;
    }
  }
  out.scene_hash = hash_scene_blob(blob);
  return out;
}

RenderSceneProxy SceneWorld::extract_render_scene(vkpt::core::FrameIndex frame) const {
  RenderSceneProxy proxy;
  const auto snapshot = build_snapshot();
  proxy.scene_hash = snapshot.scene_hash;
  proxy.frame = frame;
  proxy.benchmark = snapshot.benchmark;
  proxy.renderables.reserve(snapshot.renderables.size());
  proxy.lights.reserve(snapshot.lights.size());
  proxy.materials.reserve(snapshot.materials.size());

  for (const auto& renderable : snapshot.renderables) {
    // Render proxies carry only backend-facing fields and the resolved world-space transform.
    RenderSceneProxy::Renderable out;
    out.entity_id = renderable.entity_id;
    out.geometry_id = renderable.mesh_id;
    out.material_id = renderable.material_id;
    if (const auto* wt = world_transform(renderable.entity_id)) {
      out.world_matrix = wt->world_matrix;
      out.translation = wt->translation;
      out.scale = wt->scale;
    } else {
      out.world_matrix = make_transform_matrix(renderable.transform);
      out.translation = renderable.transform.translation;
      out.scale = renderable.transform.scale;
    }
    proxy.renderables.push_back(out);
  }

  for (const auto& light : snapshot.lights) {
    RenderSceneProxy::Light out;
    out.entity_id = light.entity_id;
    out.type = light.light.type;
    out.color = light.light.color;
    out.intensity = light.light.intensity;
    out.radius = light.light.radius;
    if (const auto* wt = world_transform(light.entity_id)) {
      out.world_matrix = wt->world_matrix;
      out.position = wt->translation;
    } else {
      out.world_matrix = make_transform_matrix(light.transform);
      out.position = light.transform.translation;
    }
    proxy.lights.push_back(out);
  }

  for (const auto& material : snapshot.materials) {
    proxy.materials.push_back(RenderSceneProxy::Material{
        material.id,
        material.material.albedo,
        material.material.roughness,
        material.material.emission,
        material.material.emission_intensity});
  }

  if (snapshot.camera) {
    RenderSceneProxy::Camera camera;
    camera.entity_id = snapshot.camera->id;
    camera.fov = snapshot.camera->camera.fov;
    camera.near_plane = snapshot.camera->camera.near_plane;
    camera.far_plane = snapshot.camera->camera.far_plane;
    camera.focal_length_mm = snapshot.camera->camera.focal_length_mm;
    camera.sensor_width_mm = snapshot.camera->camera.sensor_width_mm;
    camera.sensor_height_mm = snapshot.camera->camera.sensor_height_mm;
    camera.aperture_radius = snapshot.camera->camera.aperture_radius;
    camera.focus_distance = snapshot.camera->camera.focus_distance;
    camera.f_stop = snapshot.camera->camera.f_stop;
    camera.shutter_seconds = snapshot.camera->camera.shutter_seconds;
    camera.iso = snapshot.camera->camera.iso;
    camera.exposure_compensation = snapshot.camera->camera.exposure_compensation;
    camera.white_balance_kelvin = snapshot.camera->camera.white_balance_kelvin;
    camera.iris_blade_count = snapshot.camera->camera.iris_blade_count;
    camera.iris_rotation_degrees = snapshot.camera->camera.iris_rotation_degrees;
    camera.iris_roundness = snapshot.camera->camera.iris_roundness;
    camera.anamorphic_squeeze = snapshot.camera->camera.anamorphic_squeeze;
    if (const auto* wt = world_transform(camera.entity_id)) {
      camera.world_matrix = wt->world_matrix;
      camera.position = wt->translation;
    } else if (const auto* entity = get_entity(camera.entity_id); entity && entity->transform) {
      camera.world_matrix = make_transform_matrix(*entity->transform);
      camera.position = entity->transform->translation;
    } else {
      camera.world_matrix = identity_matrix();
    }
    proxy.camera = camera;
  }

  return proxy;
}

}  // namespace vkpt::scene
