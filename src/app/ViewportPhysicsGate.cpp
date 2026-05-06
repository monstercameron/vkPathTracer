#include "app/ViewportInteraction.h"

#ifdef PT_ENABLE_QT

#include "core/Logging.h"
#include "gpu/D3D12GpuPathTracer.h"
#include "physics/PhysicsWorld.h"
#include "render/backends/BackendFactory.h"
#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace vkpt::app {

int RunDynamicPhysicsPerformanceGate(std::string scenePath, std::string backend,
                                     uint32_t width, uint32_t height,
                                     uint32_t frames) {
  if (scenePath.empty()) {
    scenePath = "assets/scenes/material_shader_physics_showcase.json";
  }
  width = std::max<uint32_t>(1u, width);
  height = std::max<uint32_t>(1u, height);
  frames = std::max<uint32_t>(1u, frames);
  backend =
      vkpt::render::NormalizeBackendName(backend.empty() ? "d3d12" : backend);

  const std::filesystem::path artifactPath =
      "artifacts/benchmarks/dynamic_physics_gate.json";
  std::error_code ec;
  std::filesystem::create_directories(artifactPath.parent_path(), ec);
  if (ec) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        "physics-gate",
        "failed to create artifact directory",
        {{"path", artifactPath.parent_path().string()}, {"error", ec.message()}});
  }

  bool passed = false;
  std::string failure;
  std::size_t dynamicInstances = 0u;
  std::size_t physicsDynamicBodies = 0u;
  std::size_t physicsWrites = 0u;
  uint32_t successfulUpdates = 0u;
  uint32_t rebuildCount = 0u;
  double physicsStepMs = 0.0;
  double transformPublishMs = 0.0;
  double renderMs = 0.0;
  uint64_t totalRays = 0u;

  auto writeArtifact = [&]() {
    // Always emit the artifact from both success and failure paths; CI consumes
    // the counters to distinguish setup failures from transform-update regressions.
    std::ofstream out(artifactPath.string());
    if (!out) {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          "physics-gate",
          "failed to open dynamic physics artifact",
          {{"path", artifactPath.string()}});
      return;
    }
    out << "{\n"
        << "  \"schema\": \"dynamic_physics_gate.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"scene\": \"" << vkpt::log::EscapeJson(scenePath) << "\",\n"
        << "  \"backend\": \"" << vkpt::log::EscapeJson(backend) << "\",\n"
        << "  \"resolution\": { \"width\": " << width
        << ", \"height\": " << height << " },\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"dynamic_instances\": " << dynamicInstances << ",\n"
        << "  \"physics_dynamic_bodies\": " << physicsDynamicBodies << ",\n"
        << "  \"physics_transform_writes\": " << physicsWrites << ",\n"
        << "  \"transform_update_successes\": " << successfulUpdates << ",\n"
        << "  \"full_rebuild_count\": " << rebuildCount << ",\n"
        << std::fixed << std::setprecision(4)
        << "  \"physics_step_ms\": " << physicsStepMs << ",\n"
        << "  \"transform_publish_ms\": " << transformPublishMs << ",\n"
        << "  \"tlas_update_ms\": " << transformPublishMs << ",\n"
        << "  \"render_ms\": " << renderMs << ",\n"
        << "  \"rays_per_second\": "
        << (renderMs > 0.0
                ? (static_cast<double>(totalRays) * 1000.0 / renderMs)
                : 0.0)
        << ",\n"
        << "  \"total_rays\": " << totalRays << ",\n"
        << "  \"failure\": \"" << vkpt::log::EscapeJson(failure) << "\"\n"
        << "}\n";
  };

  auto fail = [&](std::string message) {
    failure = std::move(message);
    passed = false;
    writeArtifact();
    std::cerr << "dynamic physics gate failed: " << failure << "\n";
    std::cerr << "artifact: " << artifactPath.string() << "\n";
    return 2;
  };

#ifndef PT_ENABLE_D3D12
  return fail("PT_ENABLE_D3D12 is not enabled in this build");
#else
  auto parseResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!parseResult) {
    return fail("scene parse failed");
  }
  auto document = parseResult.value();
  auto worldResult = document.to_world();
  if (!worldResult) {
    return fail("scene ECS conversion failed");
  }
  auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
  if (!sceneResult) {
    return fail("scene RT conversion failed");
  }
  auto rtScene = sceneResult.value();

  // Transform writes arrive by ECS entity id, while D3D12 updates require the
  // compact RT instance index. Build the bridge once before timing the loop.
  std::unordered_map<vkpt::core::StableId, uint32_t> instanceByEntity;
  instanceByEntity.reserve(rtScene.instances.size());
  for (uint32_t i = 0u; i < rtScene.instances.size(); ++i) {
    const auto &instance = rtScene.instances[i];
    if (instance.entity_id != 0u) {
      instanceByEntity[instance.entity_id] = i;
    }
    if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      ++dynamicInstances;
    }
  }
  if (dynamicInstances == 0u) {
    return fail("scene has no dynamic RT instances");
  }

  auto physics = vkpt::physics::CreatePhysicsWorld();
  auto syncSummary = physics->sync_from_scene_world(worldResult.value());
  physicsDynamicBodies = syncSummary.dynamic_bodies;
  if (physicsDynamicBodies == 0u) {
    return fail("scene has no dynamic physics bodies");
  }

  vkpt::pathtracer::RenderSettings settings{};
  settings.width = width;
  settings.height = height;
  settings.spp = 1u;
  settings.max_depth = 3u;
  settings.seed = 0xD4D4D4D4ull;
  settings.enable_nee = true;
  settings.enable_mis = true;

  const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
      PT_SHADER_HLSL_PATH;
#else
      "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
  auto tracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
  if (!tracer->is_valid()) {
    return fail("D3D12 tracer init failed: " + tracer->last_error());
  }
  if (backend == "d3d12-dxr") {
    tracer->set_prefer_dxr(true);
    if (!tracer->dxr_supported()) {
      return fail(
          "DXR backend requested but this D3D12 device reports no DXR support");
    }
  } else if (backend != "d3d12") {
    return fail(
        "dynamic physics gate supports only d3d12 and d3d12-dxr backends");
  }
  if (!tracer->configure(settings) || !tracer->load_scene_snapshot(rtScene) ||
      !tracer->build_or_update_acceleration() ||
      !tracer->reset_accumulation()) {
    return fail("D3D12 scene preparation failed: " + tracer->last_error());
  }

  vkpt::physics::PhysicsStepConfig physicsConfig{};
  physicsConfig.fixed_dt = 1.0f / 60.0f;
  physicsConfig.collision_steps = 6;
  physicsConfig.deterministic = true;
  physicsConfig.collision_detection_enabled = true;

  uint32_t revision = 1u;
  auto lastCounters = tracer->read_counters();
  for (uint32_t frame = 0u; frame < frames; ++frame) {
    const auto physicsStart = std::chrono::steady_clock::now();
    const auto stepResult = physics->step_fixed(physicsConfig);
    physicsStepMs += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - physicsStart)
                         .count();
    if (!stepResult) {
      return fail("physics step failed");
    }

    const auto writes = physics->extract_transform_writes();
    physicsWrites += writes.size();
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
    updates.reserve(writes.size());
    for (const auto &write : writes) {
      const auto found = instanceByEntity.find(write.entity);
      if (found == instanceByEntity.end()) {
        continue;
      }
      vkpt::pathtracer::RTInstanceTransformUpdate update{};
      update.entity_id = write.entity;
      update.instance_index = found->second;
      update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                     vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                     vkpt::pathtracer::kRTInstanceFlagTransformDirty;
      update.transform_revision = revision++;
      update.translation = ToPtVec3(write.transform.translation);
      update.rotation = ToPtQuat4(write.transform.rotation);
      update.scale = ToPtVec3(write.transform.scale);
      updates.push_back(update);
    }
    if (updates.empty()) {
      continue;
    }

    const auto publishStart = std::chrono::steady_clock::now();
    const bool updated = tracer->update_instance_transforms(updates);
    transformPublishMs += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - publishStart)
                              .count();
    if (!updated) {
      // This gate is specifically for the dynamic-transform path; a fallback
      // rebuild would hide the regression it is meant to catch.
      ++rebuildCount;
      return fail("transform-only update failed; backend would require full "
                  "scene rebuild");
    }
    ++successfulUpdates;

    if (!tracer->reset_accumulation()) {
      return fail("accumulation reset failed after dynamic transform update");
    }
    const auto renderStart = std::chrono::steady_clock::now();
    if (!tracer->render_sample_batch(0, settings.height, frame, frame)) {
      return fail("render_sample_batch failed after dynamic transform update");
    }
    renderMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - renderStart)
                    .count();
    const auto counters = tracer->read_counters();
    if (counters.rays >= lastCounters.rays) {
      totalRays += counters.rays - lastCounters.rays;
    }
    lastCounters = counters;
  }

  if (successfulUpdates == 0u) {
    return fail("physics produced no mappable dynamic transform updates");
  }
  physicsStepMs /= static_cast<double>(frames);
  transformPublishMs /= static_cast<double>(successfulUpdates);
  renderMs = std::max(0.0001, renderMs);
  passed = rebuildCount == 0u && totalRays > 0u;
  if (!passed) {
    failure = "gate counters did not meet pass criteria";
  }
  writeArtifact();
  std::cout << "dynamic physics gate: " << (passed ? "ok" : "fail") << "\n";
  std::cout << "artifact: " << artifactPath.string() << "\n";
  std::cout << "dynamic_instances: " << dynamicInstances << "\n";
  std::cout << "physics_dynamic_bodies: " << physicsDynamicBodies << "\n";
  std::cout << "transform_updates: " << successfulUpdates << "\n";
  std::cout << "full_rebuild_count: " << rebuildCount << "\n";
  std::cout << "rays_per_second: "
            << (static_cast<double>(totalRays) * 1000.0 / renderMs) << "\n";
  return passed ? 0 : 2;
#endif
}

int RunThirdPersonScriptPerformanceGate(std::string scenePath,
                                        std::string backend,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t frames) {
  if (scenePath.empty()) {
    scenePath = "assets/scenes/third_person_action_demo.json";
  }
  width = std::max<uint32_t>(1u, width);
  height = std::max<uint32_t>(1u, height);
  frames = std::max<uint32_t>(1u, frames);
  backend =
      vkpt::render::NormalizeBackendName(backend.empty() ? "d3d12" : backend);

  const std::filesystem::path artifactPath =
      "artifacts/benchmarks/third_person_script_gate.json";
  std::error_code ec;
  std::filesystem::create_directories(artifactPath.parent_path(), ec);

  bool passed = false;
  std::string failure;
  std::size_t dynamicInstances = 0u;
  std::size_t totalCommands = 0u;
  std::size_t totalTransformUpdates = 0u;
  uint32_t successfulUpdates = 0u;
  uint32_t rebuildCount = 0u;
  double scriptDispatchMs = 0.0;
  double commandReplayMs = 0.0;
  double transformCollectMs = 0.0;
  double transformPublishMs = 0.0;
  double fullRebuildMs = 0.0;
  double cameraUpdateMs = 0.0;
  double renderMs = 0.0;
  uint64_t totalRays = 0u;

  auto writeArtifact = [&]() {
    std::ofstream out(artifactPath.string());
    if (!out) {
      return;
    }
    const double frameCount = static_cast<double>(frames);
    const double updateCount = static_cast<double>(std::max<uint32_t>(1u, successfulUpdates));
    out << "{\n"
        << "  \"schema\": \"third_person_script_gate.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"scene\": \"" << vkpt::log::EscapeJson(scenePath) << "\",\n"
        << "  \"backend\": \"" << vkpt::log::EscapeJson(backend) << "\",\n"
        << "  \"resolution\": { \"width\": " << width
        << ", \"height\": " << height << " },\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"dynamic_instances\": " << dynamicInstances << ",\n"
        << "  \"commands_total\": " << totalCommands << ",\n"
        << "  \"transform_updates_total\": " << totalTransformUpdates << ",\n"
        << "  \"transform_update_successes\": " << successfulUpdates << ",\n"
        << "  \"full_rebuild_count\": " << rebuildCount << ",\n"
        << std::fixed << std::setprecision(4)
        << "  \"script_dispatch_ms_avg\": " << (scriptDispatchMs / frameCount) << ",\n"
        << "  \"command_replay_ms_avg\": " << (commandReplayMs / frameCount) << ",\n"
        << "  \"transform_collect_ms_avg\": " << (transformCollectMs / frameCount) << ",\n"
        << "  \"transform_publish_ms_avg\": " << (transformPublishMs / updateCount) << ",\n"
        << "  \"camera_update_ms_avg\": " << (cameraUpdateMs / frameCount) << ",\n"
        << "  \"render_ms_total\": " << renderMs << ",\n"
        << "  \"render_ms_avg\": " << (renderMs / frameCount) << ",\n"
        << "  \"full_rebuild_ms_total\": " << fullRebuildMs << ",\n"
        << "  \"rays_per_second\": "
        << (renderMs > 0.0
                ? (static_cast<double>(totalRays) * 1000.0 / renderMs)
                : 0.0)
        << ",\n"
        << "  \"total_rays\": " << totalRays << ",\n"
        << "  \"failure\": \"" << vkpt::log::EscapeJson(failure) << "\"\n"
        << "}\n";
  };

  auto fail = [&](std::string message) {
    failure = std::move(message);
    passed = false;
    writeArtifact();
    std::cerr << "third-person script gate failed: " << failure << "\n";
    std::cerr << "artifact: " << artifactPath.string() << "\n";
    return 2;
  };

#ifndef PT_ENABLE_D3D12
  return fail("PT_ENABLE_D3D12 is not enabled in this build");
#else
#ifndef PT_ENABLE_LUA
  return fail("PT_ENABLE_LUA is not enabled in this build");
#else
  auto parseResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!parseResult) {
    return fail("scene parse failed");
  }
  auto document = parseResult.value();
  auto worldResult = document.to_world();
  if (!worldResult) {
    return fail("scene ECS conversion failed");
  }
  auto world = std::move(worldResult.value());
  world.recompute_world_transforms();
  auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
  if (!sceneResult) {
    return fail("scene RT conversion failed");
  }
  auto rtScene = sceneResult.value();

  std::unordered_map<vkpt::core::StableId, uint32_t> instanceByEntity;
  instanceByEntity.reserve(rtScene.instances.size());
  for (uint32_t i = 0u; i < rtScene.instances.size(); ++i) {
    const auto& instance = rtScene.instances[i];
    if (instance.entity_id != 0u) {
      instanceByEntity[instance.entity_id] = i;
    }
    if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      ++dynamicInstances;
    }
  }
  if (dynamicInstances == 0u) {
    return fail("third-person scene has no dynamic RT instances");
  }

  auto runtime = vkpt::scripting::CreateScriptRuntime();
  const auto bindingSummary = runtime->reload_bindings(world);
  if (bindingSummary.runnable_count == 0u) {
    return fail("third-person scene has no runnable Lua scripts");
  }

  vkpt::pathtracer::RenderSettings settings{};
  settings.width = width;
  settings.height = height;
  settings.spp = 1u;
  settings.max_depth = 3u;
  settings.seed = 0x7357A11Cull;
  settings.enable_nee = true;
  settings.enable_mis = true;

  const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
      PT_SHADER_HLSL_PATH;
#else
      "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
  auto tracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
  if (!tracer->is_valid()) {
    return fail("D3D12 tracer init failed: " + tracer->last_error());
  }
  if (backend == "d3d12-dxr") {
    tracer->set_prefer_dxr(true);
    if (!tracer->dxr_supported()) {
      return fail("DXR backend requested but this D3D12 device reports no DXR support");
    }
  } else if (backend != "d3d12") {
    return fail("third-person script gate supports only d3d12 and d3d12-dxr backends");
  }
  if (!tracer->configure(settings) || !tracer->load_scene_snapshot(rtScene) ||
      !tracer->build_or_update_acceleration() ||
      !tracer->reset_accumulation()) {
    return fail("D3D12 scene preparation failed: " + tracer->last_error());
  }

  auto collectSubtree = [&](const std::unordered_set<vkpt::core::StableId>& roots) {
    // Script commands often move a parent node; descendants inherit the world
    // transform change and need matching RT instance updates.
    std::unordered_set<vkpt::core::StableId> ids;
    ids.reserve(roots.size());
    std::vector<vkpt::core::StableId> stack;
    stack.reserve(roots.size());
    for (const auto id : roots) {
      if (ids.insert(id).second) {
        stack.push_back(id);
      }
    }
    while (!stack.empty()) {
      const auto parent = stack.back();
      stack.pop_back();
      for (const auto child : world.children_of(parent)) {
        if (ids.insert(child).second) {
          stack.push_back(child);
        }
      }
    }
    return ids;
  };

  auto buildCameraState = [&]() {
    auto state = vkpt::pathtracer::ExtractCameraState(rtScene);
    vkpt::core::StableId cameraEntity = 9101u;
    if (world.get_entity(cameraEntity) == nullptr) {
      const auto cameras = world.query(vkpt::scene::ComponentKind::Camera);
      cameraEntity = cameras.empty() ? 0u : cameras.front();
    }
    if (cameraEntity != 0u) {
      if (const auto* entity = world.get_entity(cameraEntity)) {
        if (entity->camera) {
          const auto& camera = *entity->camera;
          state.fov_deg = camera.fov;
          state.focal_length_mm = camera.focal_length_mm;
          state.sensor_width_mm = camera.sensor_width_mm;
          state.sensor_height_mm = camera.sensor_height_mm;
          state.aperture_radius = camera.aperture_radius;
          state.focus_distance = camera.focus_distance;
          state.f_stop = camera.f_stop;
          state.shutter_seconds = camera.shutter_seconds;
          state.iso = camera.iso;
          state.exposure_compensation = camera.exposure_compensation;
          state.white_balance_kelvin = camera.white_balance_kelvin;
          state.iris_blade_count = camera.iris_blade_count;
          state.iris_rotation_degrees = camera.iris_rotation_degrees;
          state.iris_roundness = camera.iris_roundness;
          state.anamorphic_squeeze = camera.anamorphic_squeeze;
        }
      }
      if (const auto* transform = world.world_transform(cameraEntity)) {
        state.position = ToPtVec3(transform->translation);
        const auto forward = PtNormalize(
            RotatePointByQuat({0.0f, 0.0f, -1.0f}, transform->rotation),
            {0.0f, 0.0f, -1.0f});
        state.target = PtAdd(state.position, forward);
        state.up = PtNormalize(
            RotatePointByQuat({0.0f, 1.0f, 0.0f}, transform->rotation),
            {0.0f, 1.0f, 0.0f});
      }
    }
    return state;
  };

  auto lastCounters = tracer->read_counters();
  uint32_t revision = 1u;
  for (uint32_t frame = 0u; frame < frames; ++frame) {
    vkpt::scene::WorldCommandBuffer commands;
    vkpt::scripting::ScriptExecutionContext context;
    context.frame = frame;
    context.delta_seconds = 1.0 / 60.0;
    context.input.active_keys = {'W'};
    context.input.mouse_delta_x = 5.0f;
    context.input.mouse_delta_y = 0.0f;

    const auto scriptStart = std::chrono::steady_clock::now();
    const auto dispatch = runtime->dispatch_hook(
        world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        context,
        commands);
    scriptDispatchMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - scriptStart)
                            .count();
    if (!dispatch.diagnostics.empty()) {
      return fail("Lua dispatch reported diagnostics");
    }
    totalCommands += commands.commands().size();

    std::unordered_set<vkpt::core::StableId> touchedTransforms;
    for (const auto& command : commands.commands()) {
      if (const auto* setTransform =
              std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(
                  &command.payload)) {
        touchedTransforms.insert(setTransform->id);
      }
    }

    const auto replayStart = std::chrono::steady_clock::now();
    if (!commands.replay(world)) {
      return fail("WorldCommandBuffer replay failed");
    }
    world.recompute_world_transforms();
    commandReplayMs += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - replayStart)
                           .count();

    const auto collectStart = std::chrono::steady_clock::now();
    const auto affected = collectSubtree(touchedTransforms);
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
    updates.reserve(affected.size());
    for (const auto entityId : affected) {
      const auto found = instanceByEntity.find(entityId);
      if (found == instanceByEntity.end()) {
        continue;
      }
      const auto* transform = world.world_transform(entityId);
      if (transform == nullptr) {
        continue;
      }
      vkpt::pathtracer::RTInstanceTransformUpdate update{};
      update.entity_id = entityId;
      update.instance_index = found->second;
      update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                     vkpt::pathtracer::kRTInstanceFlagTransformDirty;
      update.transform_revision = revision++;
      update.translation = ToPtVec3(transform->translation);
      update.rotation = ToPtQuat4(transform->rotation);
      update.scale = ToPtVec3(transform->scale);
      updates.push_back(update);
    }
    transformCollectMs += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - collectStart)
                              .count();
    totalTransformUpdates += updates.size();
    if (updates.empty()) {
      return fail("script movement produced no render instance transform updates");
    }
    vkpt::pathtracer::ApplyInstanceTransformUpdates(
        rtScene,
        updates,
        vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly);

    const auto publishStart = std::chrono::steady_clock::now();
    const bool updated = tracer->update_instance_transforms(updates);
    transformPublishMs += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - publishStart)
                              .count();
    if (!updated) {
      ++rebuildCount;
      const auto rebuildStart = std::chrono::steady_clock::now();
      if (!tracer->load_scene_snapshot(rtScene) ||
          !tracer->build_or_update_acceleration()) {
        return fail("transform update failed and scene rebuild failed");
      }
      fullRebuildMs += std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - rebuildStart)
                           .count();
    } else {
      ++successfulUpdates;
    }

    const auto cameraState = buildCameraState();
    vkpt::pathtracer::ApplyCameraState(rtScene, cameraState);
    const auto cameraStart = std::chrono::steady_clock::now();
    if (!tracer->update_camera_state(cameraState)) {
      return fail("D3D12 camera state update failed");
    }
    cameraUpdateMs += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - cameraStart)
                          .count();

    if (!tracer->reset_accumulation()) {
      return fail("accumulation reset failed after script transform update");
    }
    const auto renderStart = std::chrono::steady_clock::now();
    if (!tracer->render_sample_batch(0, settings.height, frame, frame)) {
      return fail("render_sample_batch failed after script transform update");
    }
    renderMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - renderStart)
                    .count();
    const auto counters = tracer->read_counters();
    if (counters.rays >= lastCounters.rays) {
      totalRays += counters.rays - lastCounters.rays;
    }
    lastCounters = counters;
  }

  if (successfulUpdates == 0u) {
    return fail("script movement produced no successful transform updates");
  }
  passed = rebuildCount == 0u && totalRays > 0u;
  if (!passed) {
    failure = "gate counters did not meet pass criteria";
  }
  writeArtifact();
  std::cout << "third-person script gate: " << (passed ? "ok" : "fail") << "\n";
  std::cout << "artifact: " << artifactPath.string() << "\n";
  std::cout << "dynamic_instances: " << dynamicInstances << "\n";
  std::cout << "transform_updates: " << successfulUpdates << "\n";
  std::cout << "full_rebuild_count: " << rebuildCount << "\n";
  std::cout << "script_dispatch_ms_avg: " << (scriptDispatchMs / static_cast<double>(frames)) << "\n";
  std::cout << "transform_publish_ms_avg: "
            << (transformPublishMs / static_cast<double>(successfulUpdates)) << "\n";
  std::cout << "render_ms_avg: " << (renderMs / static_cast<double>(frames)) << "\n";
  return passed ? 0 : 2;
#endif
#endif
}

} // namespace vkpt::app

#endif
