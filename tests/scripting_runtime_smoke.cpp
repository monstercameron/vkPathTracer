#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"
#include "pathtracer/SceneConversion.h"
#include "physics/PhysicsWorld.h"

#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "scripting runtime smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

bool PathExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

std::filesystem::path FindRepoFile(const std::filesystem::path& relative_path) {
  auto current = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = current / relative_path;
    if (PathExists(candidate)) {
      return candidate;
    }
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }
  return relative_path;
}

vkpt::scene::Vec3 RotateByQuat(const vkpt::scene::Vec3& value, const vkpt::scene::Quat& rotation) {
  const float len_sq = rotation.x * rotation.x +
                       rotation.y * rotation.y +
                       rotation.z * rotation.z +
                       rotation.w * rotation.w;
  if (len_sq <= 1.0e-8f) {
    return value;
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  const float qx = rotation.x * inv_len;
  const float qy = rotation.y * inv_len;
  const float qz = rotation.z * inv_len;
  const float qw = rotation.w * inv_len;
  const auto cross = [](const vkpt::scene::Vec3& a, const vkpt::scene::Vec3& b) {
    return vkpt::scene::Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
  };
  const vkpt::scene::Vec3 qv{qx, qy, qz};
  const auto t = cross(qv, value);
  const vkpt::scene::Vec3 two_t{t.x * 2.0f, t.y * 2.0f, t.z * 2.0f};
  const auto qv_cross_t = cross(qv, two_t);
  return {
      value.x + two_t.x * qw + qv_cross_t.x,
      value.y + two_t.y * qw + qv_cross_t.y,
      value.z + two_t.z * qw + qv_cross_t.z,
  };
}

[[maybe_unused]] bool CameraLooksAtHeroCenter(const vkpt::scene::TransformComponent& camera,
                                              const vkpt::scene::TransformComponent& hero) {
  constexpr float kTargetY = 1.35f;
  const auto forward = RotateByQuat({0.0f, 0.0f, -1.0f}, camera.rotation);
  const vkpt::scene::Vec3 to_target{
      hero.translation.x - camera.translation.x,
      hero.translation.y + kTargetY - camera.translation.y,
      hero.translation.z - camera.translation.z,
  };
  const float forward_len =
      std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
  const float target_len =
      std::sqrt(to_target.x * to_target.x + to_target.y * to_target.y + to_target.z * to_target.z);
  if (forward_len <= 1.0e-6f || target_len <= 1.0e-6f) {
    return false;
  }
  const float dot = forward.x * to_target.x + forward.y * to_target.y + forward.z * to_target.z;
  return dot / (forward_len * target_len) > 0.995f;
}

float DistanceXZ(const vkpt::scene::TransformComponent& lhs,
                 const vkpt::scene::TransformComponent& rhs) {
  const float dx = lhs.translation.x - rhs.translation.x;
  const float dz = lhs.translation.z - rhs.translation.z;
  return std::sqrt(dx * dx + dz * dz);
}

const vkpt::scene::WorldCommandBuffer::SetTransformCommand* FindSetTransform(
    const vkpt::scene::WorldCommandBuffer& commands,
    vkpt::core::StableId entity_id) {
  for (const auto& command : commands.commands()) {
    if (const auto* set_transform =
            std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(&command.payload);
        set_transform != nullptr && set_transform->id == entity_id) {
      return set_transform;
    }
  }
  return nullptr;
}

}  // namespace

int RunScriptingRuntimeSmoke() {
  vkpt::scene::SceneWorld world;

  const auto moving = world.create_entity("scripted_mover", 101);
  vkpt::scene::ScriptComponent mover_script;
  mover_script.script = "scripts/move.lua";
  mover_script.language = "lua";
  mover_script.entry = "default";
  mover_script.enabled = true;
  if (!world.set_component(moving, vkpt::scene::ComponentKind::Script, mover_script)) {
    std::cerr << "scripting runtime smoke failed: could not attach mover script\n";
    return 1;
  }

  const auto dormant = world.create_entity("dormant_script", 102);
  vkpt::scene::ScriptComponent dormant_script;
  dormant_script.script = "scripts/dormant.lua";
  dormant_script.enabled = false;
  if (!world.set_component(dormant, vkpt::scene::ComponentKind::Script, dormant_script)) {
    std::cerr << "scripting runtime smoke failed: could not attach dormant script\n";
    return 1;
  }

  auto runtime = vkpt::scripting::CreateScriptRuntime();
  const auto binding_summary = runtime->reload_bindings(world);
  if (!Check(binding_summary.binding_count == 2u, "expected two script bindings") ||
      !Check(binding_summary.runnable_count == 1u, "expected one runnable script") ||
      !Check(binding_summary.disabled_count == 1u, "expected one disabled script")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  if (!Check(binding_summary.execution_available, "Lua runtime should execute when PT_ENABLE_LUA is enabled")) {
    return 1;
  }
#else
  if (!Check(!binding_summary.execution_available, "stub runtime should not execute Lua yet")) {
    return 1;
  }
#endif

  vkpt::scene::WorldCommandBuffer commands;
  vkpt::scripting::ScriptExecutionContext context;
  context.frame = 7;
  context.delta_seconds = 1.0 / 60.0;
  const auto dispatch_summary = runtime->dispatch_hook(
      world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context, commands);

  if (!Check(dispatch_summary.binding_count == 2u, "dispatch should see two bindings") ||
      !Check(dispatch_summary.runnable_count == 1u, "dispatch should see one runnable script") ||
      !Check(dispatch_summary.hook_call_count == 0u, "missing smoke script should not call hooks") ||
      !Check(dispatch_summary.command_count_after == 0u, "missing smoke script should not emit commands") ||
      !Check(!dispatch_summary.diagnostics.empty(), "dispatch should report the unavailable or missing smoke script")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  if (!Check(dispatch_summary.skipped_count == 1u, "Lua dispatch should only skip the disabled script")) {
    return 1;
  }
#else
  if (!Check(dispatch_summary.skipped_count == 2u, "stub runtime should skip runnable and disabled scripts")) {
    return 1;
  }
#endif

  const auto demo_scene_path = FindRepoFile("assets/scenes/ecs_lifecycle_scripting_demo.json");
  if (!Check(PathExists(demo_scene_path), "demo lifecycle scene should exist")) {
    return 1;
  }

  auto demo_document_result = vkpt::scene::SceneDocument::load_from_file(demo_scene_path.string());
  if (!Check(static_cast<bool>(demo_document_result), "demo lifecycle scene should load")) {
    return 1;
  }
  std::vector<std::string> scene_issues;
  if (!Check(demo_document_result.value().validate(&scene_issues), "demo lifecycle scene should validate")) {
    for (const auto& issue : scene_issues) {
      std::cerr << "  scene issue: " << issue << "\n";
    }
    return 1;
  }

  auto demo_world_result = demo_document_result.value().to_world();
  if (!Check(static_cast<bool>(demo_world_result), "demo lifecycle scene should convert to world")) {
    return 1;
  }

  auto demo_runtime = vkpt::scripting::CreateScriptRuntime();
  const auto demo_binding_summary = demo_runtime->reload_bindings(demo_world_result.value());
  if (!Check(demo_binding_summary.binding_count == 4u, "demo scene should expose four script bindings") ||
      !Check(demo_binding_summary.runnable_count == 3u, "demo scene should expose three runnable scripts") ||
      !Check(demo_binding_summary.disabled_count == 1u, "demo scene should expose one disabled script")) {
    return 1;
  }

  for (const auto& binding : demo_runtime->bindings()) {
    const auto source_path = FindRepoFile(binding.source);
    if (!Check(PathExists(source_path), "demo script source should exist: " + binding.source)) {
      return 1;
    }
  }

  const std::array hooks = {
      vkpt::scripting::ScriptLifecycleHook::OnLoad,
      vkpt::scripting::ScriptLifecycleHook::OnSpawn,
      vkpt::scripting::ScriptLifecycleHook::OnEnable,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      vkpt::scripting::ScriptLifecycleHook::OnLateUpdate,
      vkpt::scripting::ScriptLifecycleHook::OnDisable,
      vkpt::scripting::ScriptLifecycleHook::OnDestroy,
      vkpt::scripting::ScriptLifecycleHook::OnUnload,
  };

  for (const auto hook : hooks) {
    vkpt::scene::WorldCommandBuffer lifecycle_commands;
    vkpt::scripting::ScriptExecutionContext lifecycle_context;
    lifecycle_context.frame = 17;
    lifecycle_context.elapsed_seconds = 0.25;
    lifecycle_context.delta_seconds = 1.0 / 60.0;

    const auto lifecycle_summary =
        demo_runtime->dispatch_hook(demo_world_result.value(), hook, lifecycle_context, lifecycle_commands);
    if (!Check(lifecycle_summary.binding_count == 4u, "lifecycle dispatch should see four bindings") ||
        !Check(lifecycle_summary.runnable_count == 3u, "lifecycle dispatch should see three runnable scripts")) {
      return 1;
    }
#ifdef PT_ENABLE_LUA
    if (!Check(lifecycle_summary.skipped_count == 1u, "Lua lifecycle dispatch should only skip disabled bindings")) {
      return 1;
    }
#else
    if (!Check(lifecycle_summary.skipped_count == 4u, "no-Lua lifecycle dispatch should skip all demo bindings") ||
        !Check(lifecycle_summary.hook_call_count == 0u, "no-Lua lifecycle dispatch should call no hooks") ||
        !Check(lifecycle_summary.command_count_after == 0u, "no-Lua lifecycle dispatch should emit no commands") ||
        !Check(lifecycle_summary.diagnostics.size() == 3u,
               "no-Lua lifecycle dispatch should diagnose the three runnable scripts")) {
      return 1;
    }
#endif
  }

  const auto third_person_scene_path = FindRepoFile("assets/scenes/third_person_action_demo.json");
  if (!Check(PathExists(third_person_scene_path), "third-person scripted scene should exist")) {
    return 1;
  }
  auto third_person_document_result =
      vkpt::scene::SceneDocument::load_from_file(third_person_scene_path.string());
  if (!Check(static_cast<bool>(third_person_document_result), "third-person scripted scene should load")) {
    return 1;
  }
  std::vector<std::string> third_person_issues;
  if (!Check(third_person_document_result.value().validate(&third_person_issues),
             "third-person scripted scene should validate")) {
    for (const auto& issue : third_person_issues) {
      std::cerr << "  scene issue: " << issue << "\n";
    }
    return 1;
  }
  vkpt::core::StableId imported_hero_model_id = 0u;
  std::size_t imported_hero_mesh_children = 0u;
  std::unordered_set<vkpt::core::StableId> imported_hero_mesh_child_ids;
  std::size_t third_person_static_physics = 0u;
  std::size_t third_person_dynamic_physics = 0u;
  std::size_t third_person_ball_meshes = 0u;
  std::size_t third_person_ball_geometry_vertices = 0u;
  bool has_hero_root_physics = false;
  bool has_hero_physics_capsule = false;
  for (const auto& geometry : third_person_document_result.value().geometry) {
    if (geometry.id == 9302u) {
      third_person_ball_geometry_vertices = geometry.vertices.size();
    }
  }
  for (const auto& entity : third_person_document_result.value().entities) {
    if (entity.name == "Hero Character Model") {
      imported_hero_model_id = entity.id;
      if (!Check(entity.has_transform, "third-person hero model should have an import transform") ||
          !Check(entity.has_hierarchy && entity.hierarchy.parent == 9110u,
                 "third-person hero model should be parented under the Lua player root") ||
          !Check(entity.animation.clip.empty(),
                 "third-person skinned hero should not receive fake procedural root animation") ||
          !Check(std::abs(entity.transform.scale.x - 0.31f) < 0.001f,
                 "third-person hero model import scale should fit the playable camera")) {
        return 1;
      }
    }
    if (entity.has_physics_body && entity.physics_body.enabled) {
      if (entity.physics_body.dynamic) {
        ++third_person_dynamic_physics;
      } else {
        ++third_person_static_physics;
      }
    }
    if (entity.id == 9110u &&
        entity.has_physics_body &&
        entity.physics_body.enabled &&
        !entity.physics_body.dynamic &&
        entity.physics_body.body_type == "kinematic" &&
        entity.physics_body.shape == "capsule") {
      has_hero_root_physics = true;
    }
    if (entity.name == "Hero Physics Capsule" &&
        entity.has_physics_body &&
        entity.physics_body.body_type == "kinematic" &&
        entity.physics_body.shape == "capsule" &&
        entity.has_hierarchy &&
        entity.hierarchy.parent == 9110u) {
      has_hero_physics_capsule = true;
    }
    if (entity.name.rfind("Physics Ball ", 0) == 0 &&
        entity.has_mesh &&
        entity.mesh.mesh_id == 9302u &&
        entity.has_physics_body &&
        entity.physics_body.dynamic &&
        entity.physics_body.shape == "sphere") {
      ++third_person_ball_meshes;
    }
  }
  for (const auto& entity : third_person_document_result.value().entities) {
    if (entity.has_hierarchy && entity.hierarchy.parent == imported_hero_model_id && entity.has_mesh) {
      ++imported_hero_mesh_children;
      imported_hero_mesh_child_ids.insert(entity.id);
    }
  }
  if (!Check(imported_hero_model_id != 0u, "third-person scene should import the low-poly hero model") ||
      !Check(imported_hero_mesh_children >= 7u,
             "third-person imported hero should preserve its material mesh buckets")) {
    return 1;
  }
  if (!Check(has_hero_root_physics, "third-person hero root ECS entity should be directly collidable") ||
      !Check(has_hero_physics_capsule, "third-person hero should have a child physics capsule") ||
      !Check(third_person_static_physics >= 5u,
             "third-person scene should have static floor/cover/player colliders") ||
      !Check(third_person_dynamic_physics >= 4u,
             "third-person scene should have dynamic lightweight physics balls") ||
      !Check(third_person_ball_meshes >= 4u,
             "third-person physics balls should be visible sphere mesh entities") ||
      !Check(third_person_ball_geometry_vertices >= 40u,
             "third-person physics balls should use a rounded lightweight sphere mesh")) {
    return 1;
  }
  auto third_person_rt_scene_result =
      vkpt::pathtracer::BuildSceneDataFromDocument(third_person_document_result.value());
  if (!Check(static_cast<bool>(third_person_rt_scene_result),
             "third-person scene should convert to RT scene data")) {
    return 1;
  }
  std::size_t dynamic_hero_instances = 0u;
  for (const auto& instance : third_person_rt_scene_result.value().instances) {
    if (!imported_hero_mesh_child_ids.contains(instance.entity_id)) {
      continue;
    }
    ++dynamic_hero_instances;
    if (!Check(instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform),
               "scripted third-person hero meshes should be dynamic transform instances") ||
        !Check(instance.local_vertex_count > 0u && instance.local_index_count > 0u,
               "scripted third-person hero meshes should keep local geometry for cheap transform updates")) {
      return 1;
    }
  }
  if (!Check(dynamic_hero_instances == imported_hero_mesh_child_ids.size(),
             "third-person RT scene should include every imported hero mesh as a dynamic instance")) {
    return 1;
  }
  auto third_person_world_result = third_person_document_result.value().to_world();
  if (!Check(static_cast<bool>(third_person_world_result), "third-person scripted scene should convert to world")) {
    return 1;
  }
  auto third_person_runtime = vkpt::scripting::CreateScriptRuntime();
  const auto third_person_summary =
      third_person_runtime->reload_bindings(third_person_world_result.value());
  if (!Check(third_person_summary.binding_count == 1u, "third-person scene should expose the player script binding") ||
      !Check(third_person_summary.runnable_count == 1u, "third-person scene should expose one runnable player script")) {
    return 1;
  }
  for (const auto& binding : third_person_runtime->bindings()) {
    const auto source_path = FindRepoFile(binding.source);
    if (!Check(PathExists(source_path), "third-person script source should exist: " + binding.source)) {
      return 1;
    }
  }

  vkpt::scene::WorldCommandBuffer third_person_commands;
  vkpt::scripting::ScriptExecutionContext third_person_context;
  third_person_context.frame = 3;
  third_person_context.delta_seconds = 1.0 / 60.0;
  third_person_context.input.active_keys = {'W'};
  const auto third_person_dispatch = third_person_runtime->dispatch_hook(
      third_person_world_result.value(),
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      third_person_context,
      third_person_commands);
  if (!Check(third_person_dispatch.runnable_count == 1u, "third-person dispatch should see the player script")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  bool moved_hero_root = false;
  bool moved_camera = false;
  bool animated_hero_model = false;
  for (const auto& command : third_person_commands.commands()) {
    if (const auto* set_transform =
            std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(&command.payload)) {
      if (set_transform->id == 9110u && set_transform->transform.translation.z < -0.0001f) {
        moved_hero_root = true;
      }
      if (set_transform->id == 9101u) {
        moved_camera = true;
      }
      if (set_transform->id == imported_hero_model_id) {
        animated_hero_model = true;
      }
    }
  }
  if (!Check(third_person_dispatch.hook_call_count == 1u, "Lua third-person dispatch should call the player update hook") ||
      !Check(third_person_dispatch.command_count_after >= 3u,
             "Lua third-person dispatch should emit gameplay transform commands without per-frame light rebuilds") ||
      !Check(third_person_dispatch.diagnostics.empty(), "Lua third-person dispatch should not report diagnostics") ||
      !Check(moved_hero_root, "W input should move the hero root forward") ||
      !Check(animated_hero_model, "W input should drive the visible imported hero pose") ||
      !Check(moved_camera, "third-person script should move the action camera")) {
    return 1;
  }

  vkpt::scene::WorldCommandBuffer third_person_strafe_commands;
  third_person_context.input.active_keys = {'D'};
  const auto third_person_strafe_dispatch = third_person_runtime->dispatch_hook(
      third_person_world_result.value(),
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      third_person_context,
      third_person_strafe_commands);
  bool strafed_right = false;
  bool camera_locked = false;
  bool have_strafe_hero = false;
  bool have_strafe_camera = false;
  vkpt::scene::TransformComponent strafe_hero_transform;
  vkpt::scene::TransformComponent strafe_camera_transform;
  for (const auto& command : third_person_strafe_commands.commands()) {
    const auto* set_transform =
        std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(&command.payload);
    if (set_transform == nullptr) {
      continue;
    }
    if (set_transform->id == 9110u &&
        set_transform->transform.translation.x > 0.0001f &&
        std::abs(set_transform->transform.translation.z) < 0.0001f) {
      strafed_right = true;
      strafe_hero_transform = set_transform->transform;
      have_strafe_hero = true;
    }
    if (set_transform->id == 9101u &&
        set_transform->transform.translation.x > 0.0001f &&
        set_transform->transform.translation.z > 4.9f) {
      camera_locked = true;
      strafe_camera_transform = set_transform->transform;
      have_strafe_camera = true;
    }
  }
  if (!Check(third_person_strafe_dispatch.hook_call_count == 1u,
             "Lua third-person strafe dispatch should call the player update hook") ||
      !Check(third_person_strafe_dispatch.command_count_after >= 3u,
             "D input should emit strafe transform commands") ||
      !Check(strafed_right, "D input should move the hero right without forward drift") ||
      !Check(camera_locked, "D input should keep the action camera locked behind the hero") ||
      !Check(have_strafe_hero && have_strafe_camera &&
                 CameraLooksAtHeroCenter(strafe_camera_transform, strafe_hero_transform),
             "D input camera should remain centered on the hero")) {
    return 1;
  }

  vkpt::scene::WorldCommandBuffer third_person_mouse_commands;
  third_person_context.input.active_keys = {'W'};
  third_person_context.input.mouse_delta_x = 100.0f;
  third_person_context.input.mouse_delta_y = 0.0f;
  const auto third_person_mouse_dispatch = third_person_runtime->dispatch_hook(
      third_person_world_result.value(),
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      third_person_context,
      third_person_mouse_commands);
  bool mouse_steered_hero = false;
  bool mouse_steered_movement = false;
  bool mouse_steered_camera = false;
  bool have_mouse_hero = false;
  bool have_mouse_camera = false;
  vkpt::scene::TransformComponent mouse_hero_transform;
  vkpt::scene::TransformComponent mouse_camera_transform;
  for (const auto& command : third_person_mouse_commands.commands()) {
    const auto* set_transform =
        std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(&command.payload);
    if (set_transform == nullptr) {
      continue;
    }
    if (set_transform->id == 9110u) {
      mouse_hero_transform = set_transform->transform;
      have_mouse_hero = true;
      if (set_transform->transform.rotation.y < -0.01f) {
        mouse_steered_hero = true;
      }
      if (set_transform->transform.translation.x < -0.0001f &&
          set_transform->transform.translation.z < -0.0001f) {
        mouse_steered_movement = true;
      }
    }
    if (set_transform->id == 9101u && set_transform->transform.translation.x > 0.1f) {
      mouse_steered_camera = true;
      mouse_camera_transform = set_transform->transform;
      have_mouse_camera = true;
    }
  }
  if (!Check(third_person_mouse_dispatch.hook_call_count == 1u,
             "Lua third-person mouse dispatch should call the player update hook") ||
      !Check(third_person_mouse_dispatch.command_count_after >= 3u,
             "mouse look with W should emit gameplay transform commands") ||
      !Check(mouse_steered_hero, "mouse look should rotate the hero") ||
      !Check(mouse_steered_movement, "mouse look should steer W movement direction") ||
      !Check(mouse_steered_camera, "mouse look should rotate the chase camera around the hero") ||
      !Check(have_mouse_hero && have_mouse_camera &&
                 CameraLooksAtHeroCenter(mouse_camera_transform, mouse_hero_transform),
             "mouse look camera should remain centered on the hero")) {
    return 1;
  }

  auto static_collision_world_result = third_person_document_result.value().to_world();
  if (!Check(static_cast<bool>(static_collision_world_result),
             "third-person static collision test should convert scene to world")) {
    return 1;
  }
  auto& static_collision_world = static_collision_world_result.value();
  auto* static_collision_hero = static_collision_world.get_entity(9110u);
  if (!Check(static_collision_hero != nullptr && static_collision_hero->transform.has_value(),
             "third-person static collision test should find the hero transform")) {
    return 1;
  }
  auto static_collision_transform = *static_collision_hero->transform;
  static_collision_transform.translation.x = -3.0f;
  static_collision_transform.translation.y = 0.0f;
  static_collision_transform.translation.z = -0.92f;
  static_collision_world.set_transform(9110u,
                                       static_collision_transform,
                                       vkpt::scene::TransformAuthority::ScriptControlled,
                                       "static_collision_test",
                                       23u);
  static_collision_world.recompute_world_transforms();
  vkpt::scene::WorldCommandBuffer static_collision_commands;
  vkpt::scripting::ScriptExecutionContext static_collision_context;
  static_collision_context.frame = 23u;
  static_collision_context.delta_seconds = 0.05;
  static_collision_context.elapsed_seconds = 0.38;
  static_collision_context.input.active_keys = {'W'};
  const auto static_collision_dispatch = third_person_runtime->dispatch_hook(
      static_collision_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      static_collision_context,
      static_collision_commands);
  const auto* static_collision_hero_move =
      FindSetTransform(static_collision_commands, 9110u);
  if (!Check(static_collision_dispatch.hook_call_count == 1u,
             "Lua third-person static collision dispatch should call the player update hook") ||
      !Check(static_collision_hero_move != nullptr,
             "static collision should still emit the hero transform") ||
      !Check(static_collision_hero_move->transform.translation.z > -0.94f,
             "static box collision should keep the hero outside cover")) {
    return 1;
  }

  auto ball_collision_world_result = third_person_document_result.value().to_world();
  if (!Check(static_cast<bool>(ball_collision_world_result),
             "third-person ball collision test should convert scene to world")) {
    return 1;
  }
  auto& ball_collision_world = ball_collision_world_result.value();
  auto* ball_collision_hero = ball_collision_world.get_entity(9110u);
  if (!Check(ball_collision_hero != nullptr && ball_collision_hero->transform.has_value(),
             "third-person ball collision test should find the hero transform")) {
    return 1;
  }
  auto ball_collision_transform = *ball_collision_hero->transform;
  ball_collision_transform.translation.x = -0.13f;
  ball_collision_transform.translation.y = 0.0f;
  ball_collision_transform.translation.z = -0.65f;
  ball_collision_world.set_transform(9110u,
                                     ball_collision_transform,
                                     vkpt::scene::TransformAuthority::ScriptControlled,
                                     "ball_collision_test",
                                     24u);
  ball_collision_world.recompute_world_transforms();
  vkpt::scene::WorldCommandBuffer ball_collision_commands;
  vkpt::scripting::ScriptExecutionContext ball_collision_context;
  ball_collision_context.frame = 24u;
  ball_collision_context.delta_seconds = 0.05;
  ball_collision_context.elapsed_seconds = 0.40;
  ball_collision_context.input.active_keys = {'A'};
  const auto ball_collision_dispatch = third_person_runtime->dispatch_hook(
      ball_collision_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      ball_collision_context,
      ball_collision_commands);
  const auto* ball_collision_hero_move =
      FindSetTransform(ball_collision_commands, 9110u);
  const auto* yellow_ball_push = FindSetTransform(ball_collision_commands, 9131u);
  if (!Check(ball_collision_dispatch.hook_call_count == 1u,
             "Lua third-person ball collision dispatch should call the player update hook") ||
      !Check(ball_collision_hero_move != nullptr,
             "ball collision should still emit the hero transform") ||
      !Check(yellow_ball_push != nullptr,
             "hero overlap should push the nearby dynamic ball from Lua") ||
      !Check(yellow_ball_push->transform.translation.x < -0.80f,
             "dynamic ball collision should move the ball away from the hero") ||
      !Check(DistanceXZ(ball_collision_hero_move->transform, yellow_ball_push->transform) > 0.66f,
             "hero and pushed ball should end the Lua frame separated")) {
    return 1;
  }

#ifdef PT_ENABLE_JOLT
  auto physics_collision_world_result = third_person_document_result.value().to_world();
  if (!Check(static_cast<bool>(physics_collision_world_result),
             "third-person physics collision test should convert scene to world")) {
    return 1;
  }
  auto& physics_collision_world = physics_collision_world_result.value();
  physics_collision_world.recompute_world_transforms();
  auto physics = vkpt::physics::CreatePhysicsWorld();
  auto physics_summary = physics->sync_from_scene_world(physics_collision_world);
  if (!Check(physics_summary.dynamic_bodies >= 4u,
             "third-person physics collision test should sync dynamic balls")) {
    return 1;
  }
  const auto* physics_ball = physics_collision_world.get_entity(9131u);
  auto* physics_hero = physics_collision_world.get_entity(9110u);
  if (!Check(physics_ball != nullptr && physics_ball->transform.has_value() &&
                 physics_hero != nullptr && physics_hero->transform.has_value(),
             "third-person physics collision test should find hero and ball transforms")) {
    return 1;
  }
  const auto initial_physics_ball_transform = *physics_ball->transform;
  auto physics_hero_transform = *physics_hero->transform;
  physics_hero_transform.translation.x = initial_physics_ball_transform.translation.x + 0.08f;
  physics_hero_transform.translation.y = 0.0f;
  physics_hero_transform.translation.z = initial_physics_ball_transform.translation.z;
  physics_collision_world.set_transform(9110u,
                                        physics_hero_transform,
                                        vkpt::scene::TransformAuthority::ScriptControlled,
                                        "jolt_collision_test",
                                        25u);
  physics_collision_world.recompute_world_transforms();
  physics->sync_from_scene_world(physics_collision_world);
  vkpt::physics::PhysicsStepConfig physics_step{};
  physics_step.fixed_dt = 1.0f / 60.0f;
  physics_step.collision_steps = 8;
  physics_step.collision_detection_enabled = true;
  vkpt::scene::TransformComponent latest_ball_transform = initial_physics_ball_transform;
  for (int frame = 0; frame < 18; ++frame) {
    const auto step_result = physics->step_fixed(physics_step);
    if (!Check(static_cast<bool>(step_result),
               "third-person physics collision test should step Jolt")) {
      return 1;
    }
    for (const auto& write : physics->extract_transform_writes()) {
      if (write.entity == 9131u) {
        latest_ball_transform = write.transform;
      }
    }
  }
  if (!Check(DistanceXZ(initial_physics_ball_transform, latest_ball_transform) > 0.04f,
             "Jolt kinematic hero body should physically push the dynamic ball")) {
    return 1;
  }
#endif
#else
  if (!Check(third_person_dispatch.command_count_after == 0u, "no-Lua third-person dispatch should emit no commands") ||
      !Check(third_person_dispatch.diagnostics.size() == 1u,
             "no-Lua third-person dispatch should diagnose the runnable player script")) {
    return 1;
  }
#endif

  std::cout << "scripting runtime smoke: ok\n";
  return 0;
}

int main() {
  try {
    return RunScriptingRuntimeSmoke();
  } catch (const std::bad_alloc& ex) {
    std::cerr << "scripting runtime smoke failed: out of memory: "
              << ex.what() << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "scripting runtime smoke failed: unhandled exception: "
              << ex.what() << "\n";
  } catch (...) {
    std::cerr << "scripting runtime smoke failed: unhandled non-standard exception\n";
  }
  return 1;
}
