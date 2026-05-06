#include "app/UiValidationInternal.h"

#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::app {

std::vector<vkpt::editor::SceneTreeEntityModel> BuildSceneTreeEntitiesFromWorld(
    const vkpt::scene::SceneWorld& world) {
  std::vector<vkpt::editor::SceneTreeEntityModel> models;
  models.reserve(world.all_entities().size());
  for (const auto id : world.all_entities()) {
    const auto* entity = world.get_entity(id);
    if (!entity) {
      continue;
    }
    vkpt::editor::SceneTreeEntityModel model;
    model.entity_id = id;
    model.parent_id = entity->hierarchy ? entity->hierarchy->parent : 0;
    model.sibling_order = entity->hierarchy ? entity->hierarchy->sibling_order : 0;
    model.name = entity->identity.name;
    if (entity->transform.has_value()) {
      model.component_badges.push_back("transform");
    }
    if (entity->mesh_renderer.has_value()) {
      model.component_badges.push_back("mesh");
    }
    if (entity->sdf_primitive.has_value()) {
      model.component_badges.push_back("sdf");
    }
    if (entity->material_override.has_value() ||
        (entity->mesh_renderer.has_value() && entity->mesh_renderer->material_id != 0)) {
      model.component_badges.push_back("material");
    }
    if (entity->camera.has_value()) {
      model.component_badges.push_back("camera");
    }
    if (entity->light.has_value()) {
      model.component_badges.push_back("light");
    }
    if (entity->script.has_value()) {
      model.component_badges.push_back("script");
    }
    if (entity->physics_body.has_value()) {
      model.component_badges.push_back("physics");
    }
    models.push_back(std::move(model));
  }
  return models;
}

bool CheckEcsSceneTreeContracts(std::string* detail) {
  auto fail = [&](std::string_view reason) {
    if (detail) {
      *detail = std::string(reason);
    }
    return false;
  };
  auto set_detail = [&](std::string_view text) {
    if (detail) {
      *detail = std::string(text);
    }
  };

  vkpt::scene::SceneDocument doc;
  doc.metadata.schema = "1.0";
  doc.metadata.scene_name = "ecs-tree-smoke";
  vkpt::scene::SceneEntityDefinition rootDef;
  rootDef.id = 1;
  rootDef.name = "Root";
  rootDef.has_physics_body = true;
  rootDef.physics_body.enabled = true;
  rootDef.physics_body.dynamic = true;
  rootDef.physics_body.body_type = "dynamic";
  rootDef.physics_body.shape = "box";
  rootDef.physics_body.mass = 2.0f;
  vkpt::scene::SceneEntityDefinition firstChild;
  firstChild.id = 2;
  firstChild.name = "First";
  firstChild.has_hierarchy = true;
  firstChild.hierarchy.parent = 1;
  firstChild.hierarchy.sibling_order = 1;
  vkpt::scene::SceneEntityDefinition secondChild;
  secondChild.id = 3;
  secondChild.name = "Second";
  secondChild.has_hierarchy = true;
  secondChild.hierarchy.parent = 1;
  secondChild.hierarchy.sibling_order = 0;
  doc.entities = {rootDef, firstChild, secondChild};
  const auto serialized = doc.to_json(true);
  auto reloaded = vkpt::scene::SceneDocument::load_from_text(serialized);
  if (!reloaded) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to parse");
  }
  auto loadedWorld = reloaded.value().to_world();
  if (!loadedWorld) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to build ECS world");
  }
  const auto persistedChildren = loadedWorld.value().children_of(1);
  if (persistedChildren != std::vector<vkpt::core::StableId>({3, 2})) {
    return fail("sibling_order did not persist through save/load");
  }
  auto physics = vkpt::physics::CreatePhysicsWorld();
  const auto physicsInfo = physics->engine_info();
  if (!physicsInfo.runs_on_worker_thread || physicsInfo.threading_model != "dedicated_worker") {
    return fail("physics world is not running through the dedicated worker thread");
  }
  const auto persistedPhysics = physics->sync_from_scene_world(loadedWorld.value());
  if (persistedPhysics.physics_components != 1u || persistedPhysics.enabled_bodies != 1u ||
      persistedPhysics.dynamic_bodies != 1u) {
    return fail("physics body did not persist through JSON and ECS sync");
  }

  vkpt::scene::SceneWorld world;
  const auto root = world.create_entity("Root", 10);
  const auto childA = world.create_entity("Child A", 11);
  const auto childB = world.create_entity("Child B", 12);
  vkpt::scene::TransformComponent rootTransform;
  rootTransform.translation = {10.0f, 0.0f, 0.0f};
  vkpt::scene::TransformComponent childTransform;
  childTransform.translation = {1.0f, 0.0f, 0.0f};
  if (!world.set_transform(root, rootTransform) ||
      !world.set_transform(childA, childTransform) ||
      !world.set_hierarchy_parent(childA, root, 0) ||
      !world.set_hierarchy_parent(childB, root, 1)) {
    return fail("failed to build ECS hierarchy fixture");
  }
  vkpt::scene::MeshRendererComponent mesh;
  mesh.mesh_id = 101;
  mesh.material_id = 201;
  world.set_component(childB, vkpt::scene::ComponentKind::MeshRenderer, mesh);
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.mass = 1.5f;
  world.set_component(childA, vkpt::scene::ComponentKind::PhysicsBody, body);
  const auto livePhysics = physics->sync_from_scene_world(world);
  if (livePhysics.enabled_bodies != 1u || livePhysics.dynamic_bodies != 1u) {
    return fail("physics world did not sync enabled ECS physics body");
  }
  vkpt::physics::PhysicsStepConfig physicsStep;
  physicsStep.fixed_dt = 1.0f / 60.0f;
  if (!physics->step_fixed(physicsStep)) {
    return fail("physics fixed step rejected a valid timestep");
  }
  if (physicsInfo.available && physics->extract_transform_writes().empty()) {
    return fail("Jolt physics backend did not publish transform writes");
  }

  world.recompute_world_transforms();
  const auto* before = world.world_transform(childA);
  if (!before || std::abs(before->translation.x - 11.0f) > 0.001f) {
    return fail("hierarchy fixture world transform was invalid before reparent");
  }

  vkpt::scene::WorldCommandBuffer commands;
  commands.add_reorder_sibling(childB, 0, childA);
  commands.add_reparent_entity(childA, 0, true);
  commands.add_create_entity("Camera Child", 13, root);
  commands.add_set_component(13, vkpt::scene::ComponentKind::Camera, vkpt::scene::CameraComponent{});
  if (!commands.replay(world)) {
    return fail("WorldCommandBuffer failed to replay reparent/reorder/create-child commands");
  }
  world.recompute_world_transforms();
  const auto rootChildren = world.children_of(root);
  if (rootChildren != std::vector<vkpt::core::StableId>({childB, 13})) {
    return fail("reorder/create-child commands produced nondeterministic child order");
  }
  const auto* after = world.world_transform(childA);
  if (!after || std::abs(after->translation.x - 11.0f) > 0.001f) {
    return fail("preserve-world reparent changed the child world transform");
  }

  vkpt::editor::SelectionState selection = vkpt::editor::CreateDefaultSelectionState();
  selection.selected_entity_ids = {childA};
  selection.hovered_entity = 13;
  const auto treeRows = vkpt::editor::BuildSceneTreeRows(
      BuildSceneTreeEntitiesFromWorld(world), selection, 13);
  if (treeRows.size() != 4) {
    return fail("scene tree row builder returned the wrong visible row count");
  }
  if (treeRows[0].entity_id != root || treeRows[0].depth != 0 || !treeRows[0].has_children) {
    return fail("scene tree rows did not expose root hierarchy state");
  }
  if (treeRows[1].entity_id != childB || treeRows[1].depth != 1 ||
      std::find(treeRows[1].component_badges.begin(), treeRows[1].component_badges.end(), "mesh") ==
          treeRows[1].component_badges.end()) {
    return fail("scene tree rows did not expose ordered child badges");
  }
  if (treeRows[2].entity_id != 13 || treeRows[2].depth != 1 || !treeRows[2].hovered ||
      treeRows[2].icon != "camera") {
    return fail("scene tree rows did not expose hover/camera state");
  }
  if (treeRows[3].entity_id != childA || !treeRows[3].selected || treeRows[3].depth != 0) {
    return fail("scene tree rows did not reflect selection or reparent-to-root");
  }
  set_detail("ECS tree rows, worker-thread physics sync, hierarchy command replay, preserve-world reparent, and sibling_order JSON roundtrip pass");
  return true;
}

void RunUiSceneTreeSmokeChecks(const UiSmokeCheckFn& check_true) {
  std::string ecs_tree_detail;
  check_true("ecs scene tree integration", CheckEcsSceneTreeContracts(&ecs_tree_detail));
}

}  // namespace vkpt::app