#include "app/ViewportInteraction.h"

#ifdef PT_ENABLE_QT

#include "core/Logging.h"
#include "gpu/D3D12GpuPathTracer.h"
#include "physics/PhysicsWorld.h"
#include "render/backends/BackendFactory.h"

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
    std::ofstream out(artifactPath.string());
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

} // namespace vkpt::app

#endif
