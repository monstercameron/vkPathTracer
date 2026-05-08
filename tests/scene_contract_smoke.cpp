#include "scene/Scene.h"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "scene_contract_smoke: " << message << "\n";
    return false;
  }
  return true;
}

bool ContainsId(const std::vector<vkpt::core::StableId>& ids,
                vkpt::core::StableId id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool WarningContains(const vkpt::core::Status& status,
                     std::string_view text) {
  for (const auto& warning : status.warnings) {
    if (warning.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool CustomFieldEquals(const vkpt::core::contracts::SubsystemStatus& status,
                       std::string_view key,
                       std::string_view value) {
  for (const auto& field : status.custom_fields) {
    if (field.name == key && field.value == value) {
      return true;
    }
  }
  return false;
}

bool CheckSceneWorldStatusAndDirtyEntities() {
  vkpt::scene::SceneWorld world;
  const auto determinism =
      vkpt::core::MakeDeterminismContext(true, 0x51CEu, 42u, "scene-contract");
  world.set_determinism(determinism);
  if (!Check(world.determinism_context() == determinism,
             "scene world should retain DeterminismContext")) {
    return false;
  }

  const auto empty = world.status();
  if (!Check(empty.lifecycle ==
                 vkpt::core::contracts::ComponentLifecycle::Uninitialized,
             "empty scene status should be uninitialized") ||
      !Check(empty.entity_count == 0u,
             "empty scene status should report zero entities") ||
      !Check(empty.deterministic &&
                 empty.determinism_base_seed == determinism.base_seed &&
                 empty.determinism_frame_index == determinism.frame_index &&
                 empty.determinism_scenario_id == determinism.scenario_id &&
                 empty.current_flow_id == determinism.frame_index,
             "scene status should expose determinism context and flow id")) {
    return false;
  }

  const auto missing = world.set_component(
      999u,
      vkpt::scene::ComponentKind::Transform,
      vkpt::scene::TransformComponent{});
  if (!Check(missing.is_error(),
             "set_component should return Status error for missing entity") ||
      !Check(missing.code == vkpt::core::StatusCode::InvalidArgument,
             "missing entity should be InvalidArgument")) {
    return false;
  }

  const auto parent = world.create_entity("parent");
  const auto child = world.create_entity("child");
  if (!Check(parent != 0u && child != 0u, "entities should be created")) {
    return false;
  }

  vkpt::scene::TransformComponent parent_transform;
  parent_transform.translation.x = 1.0f;
  vkpt::scene::TransformComponent child_transform;
  child_transform.translation.y = 2.0f;
  if (!Check(world.set_component(parent,
                                 vkpt::scene::ComponentKind::Transform,
                                 parent_transform),
             "parent transform set should succeed") ||
      !Check(world.set_component(child,
                                 vkpt::scene::ComponentKind::Transform,
                                 child_transform),
             "child transform set should succeed")) {
    return false;
  }

  auto dirty = world.dirty_entities();
  if (!Check(dirty.size() == 2u && ContainsId(dirty, parent) &&
                 ContainsId(dirty, child),
             "dirty_entities should report transform-dirty entities in the world")) {
    return false;
  }

  auto status = world.status();
  if (!Check(status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Ready,
             "non-empty scene status should be ready") ||
      !Check(status.entity_count == 2u && status.dirty_entity_count == 2u,
             "scene status should report entity and dirty counts")) {
    return false;
  }

  world.recompute_world_transforms();
  if (!Check(world.dirty_entities().empty(),
             "recompute_world_transforms should clear dirty entity query")) {
    return false;
  }

  const auto reparent = world.reparent_entity(child, parent, true);
  dirty = world.dirty_entities();
  if (!Check(reparent, "reparent_entity should return successful Status") ||
      !Check(dirty.size() == 1u && dirty.front() == child,
             "reparent_entity should dirty the moved child")) {
    return false;
  }

  const auto bad_reparent = world.reparent_entity(child, 123456u, true);
  if (!Check(bad_reparent.is_error(),
             "reparent_entity should reject a missing parent with Status")) {
    return false;
  }

  const auto mismatch = world.add_component(
      parent,
      vkpt::scene::ComponentKind::Camera,
      vkpt::scene::TransformComponent{});
  if (!Check(mismatch.is_error(),
             "add_component should reject mismatched component payloads")) {
    return false;
  }

  const auto destroyed = world.destroy_entity(child);
  if (!Check(destroyed, "destroy_entity should return successful Status") ||
      !Check(!world.entity_exists(child),
             "destroy_entity should remove the entity from the live world")) {
    return false;
  }

  const auto missing_destroy = world.destroy_entity(child);
  world.clear();
  const bool ok =
      Check(missing_destroy.is_error(),
            "destroy_entity should reject an already destroyed entity");
  return ok;
}

bool CheckTransformAuthorityStatusWarnings() {
  vkpt::scene::SceneWorld world;
  const auto entity = world.create_entity("controlled");
  vkpt::scene::TransformComponent transform;

  const auto physics = world.set_transform(
      entity,
      transform,
      vkpt::scene::TransformAuthority::PhysicsControlled,
      "physics",
      12u);
  if (!Check(physics, "initial transform authority write should succeed")) {
    return false;
  }

  transform.translation.x = 4.0f;
  const auto authored = world.set_transform(
      entity,
      transform,
      vkpt::scene::TransformAuthority::Authored,
      "scene",
      12u);
  if (!Check(authored.is_error(),
             "lower authority same-frame transform should be rejected") ||
      !Check(authored.code == vkpt::core::StatusCode::Busy,
             "authority rejection should report Busy") ||
      !Check(WarningContains(authored, "scene"),
             "authority rejection warning should include losing writer name")) {
    return false;
  }

  transform.translation.x = 8.0f;
  const auto benchmark = world.set_transform(
      entity,
      transform,
      vkpt::scene::TransformAuthority::BenchmarkFrozen,
      "benchmark",
      12u);
  if (!Check(benchmark,
             "higher authority same-frame transform should succeed") ||
      !Check(WarningContains(benchmark, "physics"),
             "authority acceptance warning should include losing writer name")) {
    return false;
  }

  const auto status = world.status();
  const auto report = vkpt::scene::EvaluateSceneWorldHealth(status);
  const auto probe = world.create_health_probe();
  const auto probe_report = probe->check();
  const auto generic = vkpt::scene::ToSubsystemStatus(status);
  const auto formatted = vkpt::scene::FormatSceneWorldStatus(status);
  return Check(world.authority_conflicts().size() == 2u,
               "authority conflicts should be recorded") &&
         Check(status.health == vkpt::core::contracts::SubsystemHealth::Degraded,
               "scene status should degrade when authority conflicts exist") &&
         Check(generic.name == "scene" &&
                   generic.status ==
                       vkpt::core::contracts::SubsystemHealth::Degraded,
               "scene status should convert to generic subsystem status") &&
         Check(CustomFieldEquals(generic, "deterministic", "false"),
               "generic scene status should expose deterministic=false") &&
         Check(report.status == vkpt::core::health::Status::Degraded &&
                   probe->name() == "scene" &&
                   probe_report.status == vkpt::core::health::Status::Degraded,
               "scene health probe should expose degraded authority conflicts") &&
         Check(formatted.find("authority_conflicts: 2") != std::string::npos,
               "formatted scene status should include authority conflict count");
}

bool CheckWorldCommandBufferFlowId() {
  vkpt::scene::SceneWorld world;
  vkpt::scene::WorldCommandBuffer commands;
  commands.set_flow_id(91u);
  commands.add_create_entity("flow_entity", 91001u);
  const auto replay = commands.replay(world);
  return Check(commands.flow_id() == 91u,
               "world command buffer should retain its flow id") &&
         Check(static_cast<bool>(replay) && world.entity_exists(91001u),
               "world command buffer replay should apply under the flow id");
}

}  // namespace

int main() {
  if (!CheckSceneWorldStatusAndDirtyEntities()) {
    return 1;
  }
  if (!CheckTransformAuthorityStatusWarnings()) {
    return 1;
  }
  if (!CheckWorldCommandBufferFlowId()) {
    return 1;
  }
  std::cout << "scene_contract_smoke: ok\n";
  return 0;
}
