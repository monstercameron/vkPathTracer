#include "app/ViewportInteraction.h"

#ifdef PT_ENABLE_QT

#include "core/Logging.h"
#include "gpu/D3D12GpuPathTracer.h"
#include "physics/PhysicsWorld.h"
#include "render/backends/BackendFactory.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace vkpt::app {

ViewportPickable::Triangle MakeViewportTriangle(const vkpt::pathtracer::Vec3& v0,
                                                const vkpt::pathtracer::Vec3& v1,
                                                const vkpt::pathtracer::Vec3& v2) {
  ViewportPickable::Triangle triangle{};
  triangle.v0 = v0;
  triangle.v1 = v1;
  triangle.v2 = v2;
  triangle.bounds_min = {
      std::min({v0.x, v1.x, v2.x}),
      std::min({v0.y, v1.y, v2.y}),
      std::min({v0.z, v1.z, v2.z})};
  triangle.bounds_max = {
      std::max({v0.x, v1.x, v2.x}),
      std::max({v0.y, v1.y, v2.y}),
      std::max({v0.z, v1.z, v2.z})};
  triangle.bounds_valid = true;
  return triangle;
}

vkpt::pathtracer::Vec3 PtAdd(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vkpt::pathtracer::Vec3 PtSub(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vkpt::pathtracer::Vec3 PtMul(const vkpt::pathtracer::Vec3& v, float scale) {
  return {v.x * scale, v.y * scale, v.z * scale};
}

float PtDot(const vkpt::pathtracer::Vec3& a, const vkpt::pathtracer::Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

vkpt::pathtracer::Vec3 PtCross(const vkpt::pathtracer::Vec3& a,
                               const vkpt::pathtracer::Vec3& b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}

float PtLength(const vkpt::pathtracer::Vec3& v) {
  return std::sqrt(std::max(0.0f, PtDot(v, v)));
}

vkpt::pathtracer::Vec3 PtNormalize(const vkpt::pathtracer::Vec3& v,
                                   vkpt::pathtracer::Vec3 fallback) {
  const float len = PtLength(v);
  if (len <= 1.0e-6f) {
    return fallback;
  }
  return PtMul(v, 1.0f / len);
}

vkpt::scene::Quat NormalizeQuat(vkpt::scene::Quat q) {
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 1.0e-6f) {
    return {};
  }
  const float inv = 1.0f / len;
  q.x *= inv;
  q.y *= inv;
  q.z *= inv;
  q.w *= inv;
  return q;
}

vkpt::scene::Quat QuatMultiply(const vkpt::scene::Quat& a, const vkpt::scene::Quat& b) {
  return NormalizeQuat({
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  });
}

vkpt::scene::Quat QuatFromAxisAngle(const vkpt::pathtracer::Vec3& axis, float radians) {
  const auto normalized = PtNormalize(axis, {0.0f, 1.0f, 0.0f});
  const float half = radians * 0.5f;
  const float s = std::sin(half);
  return NormalizeQuat({normalized.x * s, normalized.y * s, normalized.z * s, std::cos(half)});
}

vkpt::scene::Quat QuatFromCameraForwardUp(const vkpt::pathtracer::Vec3& forwardIn,
                                          const vkpt::pathtracer::Vec3& upIn) {
  const auto forward = PtNormalize(forwardIn, {0.0f, 0.0f, -1.0f});
  const auto right = PtNormalize(PtCross(forward, upIn), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});

  const float m00 = right.x;
  const float m01 = up.x;
  const float m02 = -forward.x;
  const float m10 = right.y;
  const float m11 = up.y;
  const float m12 = -forward.y;
  const float m20 = right.z;
  const float m21 = up.z;
  const float m22 = -forward.z;

  vkpt::scene::Quat q;
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  return NormalizeQuat(q);
}

vkpt::pathtracer::Vec3 RotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                         const vkpt::scene::Quat& rotation) {
  const auto q = NormalizeQuat(rotation);
  const vkpt::pathtracer::Vec3 qv{q.x, q.y, q.z};
  const auto t = PtMul(PtCross(qv, point), 2.0f);
  return PtAdd(PtAdd(point, PtMul(t, q.w)), PtCross(qv, t));
}

vkpt::pathtracer::Vec3 InverseRotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                                const vkpt::scene::Quat& rotation) {
  auto q = NormalizeQuat(rotation);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return RotatePointByQuat(point, q);
}

vkpt::pathtracer::Vec3 ApplySceneTransformToPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform) {
  const auto scaled = vkpt::pathtracer::Vec3{
      point.x * transform.scale.x,
      point.y * transform.scale.y,
      point.z * transform.scale.z};
  const auto rotated = RotatePointByQuat(scaled, transform.rotation);
  return PtAdd(rotated, {transform.translation.x, transform.translation.y, transform.translation.z});
}

vkpt::pathtracer::Vec3 InverseSceneTransformPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform) {
  const auto translated = PtSub(point, {transform.translation.x, transform.translation.y, transform.translation.z});
  const auto unrotated = InverseRotatePointByQuat(translated, transform.rotation);
  const auto safeScale = [](float value) {
    return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
  };
  return {
      unrotated.x / safeScale(transform.scale.x),
      unrotated.y / safeScale(transform.scale.y),
      unrotated.z / safeScale(transform.scale.z)};
}

float ClampFloat(float value, float min_value, float max_value) {
  return std::min(max_value, std::max(min_value, value));
}

float DegToRad(float degrees) {
  return degrees * (3.14159265358979323846f / 180.0f);
}

bool AnimationHasAuthoredMotion(const vkpt::scene::AnimationComponent& animation) {
  constexpr float kEpsilon = 1.0e-6f;
  return std::fabs(animation.translation_amplitude.x) > kEpsilon ||
         std::fabs(animation.translation_amplitude.y) > kEpsilon ||
         std::fabs(animation.translation_amplitude.z) > kEpsilon ||
         std::fabs(animation.rotation_degrees.x) > kEpsilon ||
         std::fabs(animation.rotation_degrees.y) > kEpsilon ||
         std::fabs(animation.rotation_degrees.z) > kEpsilon ||
         std::fabs(animation.scale_amplitude.x) > kEpsilon ||
         std::fabs(animation.scale_amplitude.y) > kEpsilon ||
         std::fabs(animation.scale_amplitude.z) > kEpsilon;
}

vkpt::scene::Quat QuatFromEulerDegrees(const vkpt::scene::Vec3& degrees) {
  const auto qx = QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, DegToRad(degrees.x));
  const auto qy = QuatFromAxisAngle({0.0f, 1.0f, 0.0f}, DegToRad(degrees.y));
  const auto qz = QuatFromAxisAngle({0.0f, 0.0f, 1.0f}, DegToRad(degrees.z));
  return QuatMultiply(qz, QuatMultiply(qy, qx));
}

vkpt::scene::TransformComponent SampleAnimationTransform(
    const vkpt::scene::TransformComponent& base,
    const vkpt::scene::AnimationComponent& animation,
    float elapsed_seconds) {
  vkpt::scene::TransformComponent out = base;
  const float duration = std::max(1.0f / 60.0f, animation.duration_seconds);
  float local_time = elapsed_seconds * animation.playback_speed;
  if (animation.looping) {
    local_time = std::fmod(local_time, duration);
    if (local_time < 0.0f) {
      local_time += duration;
    }
  } else {
    local_time = ClampFloat(local_time, 0.0f, duration);
  }
  const float phase = duration > 0.0f ? ClampFloat(local_time / duration, 0.0f, 1.0f) : 0.0f;
  const float wave = std::sin(phase * 6.28318530717958647692f);

  out.translation.x += animation.translation_amplitude.x * wave;
  out.translation.y += animation.translation_amplitude.y * wave;
  out.translation.z += animation.translation_amplitude.z * wave;

  const vkpt::scene::Vec3 animated_degrees{
      animation.rotation_degrees.x * phase,
      animation.rotation_degrees.y * phase,
      animation.rotation_degrees.z * phase};
  out.rotation = QuatMultiply(QuatFromEulerDegrees(animated_degrees), base.rotation);

  out.scale.x = std::max(1.0e-5f, base.scale.x * (1.0f + animation.scale_amplitude.x * wave));
  out.scale.y = std::max(1.0e-5f, base.scale.y * (1.0f + animation.scale_amplitude.y * wave));
  out.scale.z = std::max(1.0e-5f, base.scale.z * (1.0f + animation.scale_amplitude.z * wave));
  out.dirty = true;
  return out;
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::scene::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::scene::Vec3 ToSceneVec3(const vkpt::pathtracer::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Quat4 ToPtQuat4(const vkpt::scene::Quat& q) {
  return {q.x, q.y, q.z, q.w};
}

vkpt::editor::Vec3 ToEditorVec3(const vkpt::pathtracer::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::editor::Vec3& v) {
  return {v.x, v.y, v.z};
}

int RunDynamicPhysicsPerformanceGate(std::string scenePath,
                                     std::string backend,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t frames) {
  if (scenePath.empty()) {
    scenePath = "assets/scenes/material_shader_physics_showcase.json";
  }
  width = std::max<uint32_t>(1u, width);
  height = std::max<uint32_t>(1u, height);
  frames = std::max<uint32_t>(1u, frames);
  backend = vkpt::render::NormalizeBackendName(backend.empty() ? "d3d12" : backend);

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
        << "  \"resolution\": { \"width\": " << width << ", \"height\": " << height << " },\n"
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
        << (renderMs > 0.0 ? (static_cast<double>(totalRays) * 1000.0 / renderMs) : 0.0) << ",\n"
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
    const auto& instance = rtScene.instances[i];
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
      return fail("DXR backend requested but this D3D12 device reports no DXR support");
    }
  } else if (backend != "d3d12") {
    return fail("dynamic physics gate supports only d3d12 and d3d12-dxr backends");
  }
  if (!tracer->configure(settings) ||
      !tracer->load_scene_snapshot(rtScene) ||
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
        std::chrono::steady_clock::now() - physicsStart).count();
    if (!stepResult) {
      return fail("physics step failed");
    }

    const auto writes = physics->extract_transform_writes();
    physicsWrites += writes.size();
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
    updates.reserve(writes.size());
    for (const auto& write : writes) {
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
        std::chrono::steady_clock::now() - publishStart).count();
    if (!updated) {
      ++rebuildCount;
      return fail("transform-only update failed; backend would require full scene rebuild");
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
        std::chrono::steady_clock::now() - renderStart).count();
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

void ExpandBounds(vkpt::editor::Bounds& bounds, const vkpt::pathtracer::Vec3& point) {
  if (!bounds.valid) {
    bounds.min = ToEditorVec3(point);
    bounds.max = ToEditorVec3(point);
    bounds.valid = true;
    return;
  }
  bounds.min.x = std::min(bounds.min.x, point.x);
  bounds.min.y = std::min(bounds.min.y, point.y);
  bounds.min.z = std::min(bounds.min.z, point.z);
  bounds.max.x = std::max(bounds.max.x, point.x);
  bounds.max.y = std::max(bounds.max.y, point.y);
  bounds.max.z = std::max(bounds.max.z, point.z);
}

vkpt::pathtracer::Vec3 TransformPointForPreview(const vkpt::scene::Vec3& point,
                                                const vkpt::scene::TransformComponent& transform) {
  return ApplySceneTransformToPoint({point.x, point.y, point.z}, transform);
}

std::optional<vkpt::scene::SceneWorld> BuildSceneWorldSnapshot(
    const vkpt::scene::SceneDocument& document) {
  auto worldResult = document.to_world();
  if (!worldResult) {
    return std::nullopt;
  }
  auto world = std::move(worldResult.value());
  world.recompute_world_transforms();
  return world;
}

vkpt::scene::TransformComponent ResolveEntityWorldTransform(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* world) {
  if (world != nullptr) {
    if (const auto* worldTransform = world->world_transform(entity.id)) {
      vkpt::scene::TransformComponent transform = *worldTransform;
      transform.dirty = entity.has_transform ? entity.transform.dirty : false;
      return transform;
    }
  }
  return entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
}

vkpt::scene::TransformComponent TransformFromRtInstance(
    const vkpt::pathtracer::RTInstance& instance) {
  vkpt::scene::TransformComponent transform;
  transform.translation = {instance.translation.x, instance.translation.y, instance.translation.z};
  transform.rotation = {instance.rotation.x, instance.rotation.y, instance.rotation.z, instance.rotation.w};
  transform.scale = {instance.scale.x, instance.scale.y, instance.scale.z};
  transform.dirty = false;
  return transform;
}

vkpt::scene::Quat InverseQuat(vkpt::scene::Quat q) {
  q = NormalizeQuat(q);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return q;
}

float SafeTransformScaleDivisor(float value) {
  return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
}

vkpt::scene::TransformComponent ConvertWorldTransformToDocumentLocal(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* currentWorld,
    const vkpt::scene::TransformComponent& worldTransform) {
  if (!entity.has_hierarchy || entity.hierarchy.parent == 0 || currentWorld == nullptr) {
    return worldTransform;
  }

  const auto* parentWorld = currentWorld->world_transform(entity.hierarchy.parent);
  if (parentWorld == nullptr) {
    return worldTransform;
  }

  vkpt::scene::TransformComponent local = worldTransform;
  const auto delta = PtSub(ToPtVec3(worldTransform.translation),
                           ToPtVec3(parentWorld->translation));
  const auto unrotated = InverseRotatePointByQuat(delta, parentWorld->rotation);
  local.translation = {
      unrotated.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      unrotated.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      unrotated.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.rotation = QuatMultiply(InverseQuat(parentWorld->rotation), worldTransform.rotation);
  local.scale = {
      worldTransform.scale.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      worldTransform.scale.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      worldTransform.scale.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.dirty = true;
  return local;
}

std::string PickableLabel(std::string_view name, vkpt::core::StableId id) {
  if (!name.empty()) {
    return std::string(name);
  }
  return "entity " + std::to_string(id);
}

void AddSdfPickable(std::vector<ViewportPickable>& pickables,
                    vkpt::core::StableId id,
                    std::string label,
                    std::string_view shape,
                    const vkpt::scene::TransformComponent& transform,
                    const vkpt::scene::SdfPrimitiveComponent& primitive) {
  const auto center = ToPtVec3(transform.translation);
  const auto scale = ToPtVec3(transform.scale);
  const float radius = std::max(0.05f, primitive.radius);
  vkpt::pathtracer::Vec3 extent{
      std::max(0.05f, std::fabs(scale.x) * radius),
      std::max(0.05f, std::fabs(scale.y) * radius),
      std::max(0.05f, std::fabs(scale.z) * radius),
  };
  if (shape == "box" || shape == "rounded_box") {
    extent = {
        std::max(0.05f, std::fabs(scale.x)),
        std::max(0.05f, std::fabs(scale.y)),
        std::max(0.05f, std::fabs(scale.z)),
    };
  } else if (shape == "torus") {
    const float major = std::max(0.05f, primitive.param_a);
    const float minor = std::max(0.02f, radius);
    const float torusExtent = major + minor;
    extent = {
        std::max(0.05f, std::fabs(scale.x) * torusExtent),
        std::max(0.05f, std::fabs(scale.y) * minor),
        std::max(0.05f, std::fabs(scale.z) * torusExtent),
    };
  } else if (shape == "capsule") {
    const float halfHeight = std::max(0.0f, primitive.param_a);
    extent = {
        std::max(0.05f, std::fabs(scale.x) * radius),
        std::max(0.05f, std::fabs(scale.y) * (halfHeight + radius)),
        std::max(0.05f, std::fabs(scale.z) * radius),
    };
  } else if (shape == "plane") {
    return;
  }

  vkpt::editor::Bounds bounds{};
  ExpandBounds(bounds, PtSub(center, extent));
  ExpandBounds(bounds, PtAdd(center, extent));
  if (bounds.valid) {
    ViewportPickable pickable{};
    pickable.entity_id = id;
    pickable.bounds = bounds;
    pickable.label = std::move(label);
    pickables.push_back(std::move(pickable));
  }
}

void AddSdfPickable(std::vector<ViewportPickable>& pickables,
                    const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
  const std::string shape = primitive.shape.empty()
      ? (primitive.primitive.shape.empty() ? std::string("sphere") : primitive.primitive.shape)
      : primitive.shape;
  AddSdfPickable(pickables,
                 primitive.id,
                 "sdf " + std::to_string(primitive.id),
                 shape,
                 primitive.transform,
                 primitive.primitive);
}

std::vector<ViewportPickable> BuildViewportPickables(const vkpt::scene::SceneDocument& document,
                                                     const vkpt::pathtracer::RTSceneData& scene) {
  std::vector<ViewportPickable> pickables;
  const auto worldSnapshot = BuildSceneWorldSnapshot(document);
  const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneGeometryDefinition*> geometryById;
  for (const auto& geometry : document.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  struct MeshPickableRef {
    vkpt::core::StableId entity_id = 0;
    vkpt::core::StableId mesh_id = 0;
    std::string label;
  };
  std::vector<MeshPickableRef> meshRefs;
  meshRefs.reserve(document.entities.size());
  for (const auto& entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto* geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty() || geometry->indices.empty()) {
      continue;
    }
    meshRefs.push_back({
        entity.id,
        entity.mesh.mesh_id,
        PickableLabel(entity.name, entity.id)});
  }

  auto appendEntitySdfPickables = [&]() {
    for (const auto& entity : document.entities) {
      if (!entity.has_sdf_primitive) {
        continue;
      }
      const auto transform = ResolveEntityWorldTransform(entity, world);
      const std::string shape = entity.sdf_primitive.shape.empty()
          ? std::string("sphere")
          : entity.sdf_primitive.shape;
      AddSdfPickable(pickables,
                     entity.id,
                     PickableLabel(entity.name, entity.id),
                     shape,
                     transform,
                     entity.sdf_primitive);
    }
  };

  if (!meshRefs.empty() && !scene.instances.empty()) {
    const std::size_t count = std::min(meshRefs.size(), scene.instances.size());
    for (std::size_t instanceIndex = 0; instanceIndex < count; ++instanceIndex) {
      const auto& instance = scene.instances[instanceIndex];
      const auto& meshRef = meshRefs[instanceIndex];
      if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
        const auto geometryIt = geometryById.find(meshRef.mesh_id);
        if (geometryIt == geometryById.end() || geometryIt->second == nullptr) {
          continue;
        }
        const auto* geometry = geometryIt->second;
        const auto transform = TransformFromRtInstance(instance);
        vkpt::editor::Bounds bounds{};
        for (const auto& vertex : geometry->vertices) {
          ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
        }
        if (!bounds.valid) {
          continue;
        }
        ViewportPickable pickable{};
        pickable.entity_id = meshRef.entity_id;
        pickable.bounds = bounds;
        pickable.label = meshRef.label;
        pickable.require_triangle_hit = true;
        for (std::size_t index = 0; index + 2u < geometry->indices.size(); index += 3u) {
          const auto i0 = geometry->indices[index + 0u];
          const auto i1 = geometry->indices[index + 1u];
          const auto i2 = geometry->indices[index + 2u];
          if (i0 >= geometry->vertices.size() ||
              i1 >= geometry->vertices.size() ||
              i2 >= geometry->vertices.size()) {
            continue;
          }
          pickable.triangles.push_back(MakeViewportTriangle(
              TransformPointForPreview(geometry->vertices[i0], transform),
              TransformPointForPreview(geometry->vertices[i1], transform),
              TransformPointForPreview(geometry->vertices[i2], transform)));
        }
        if (!pickable.triangles.empty()) {
          pickables.push_back(std::move(pickable));
        }
        continue;
      }
      vkpt::editor::Bounds bounds{};
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        for (uint32_t corner = 0u; corner < 3u; ++corner) {
          const uint32_t vertexIndex = scene.indices[base + corner];
          if (vertexIndex < scene.vertices.size()) {
            ExpandBounds(bounds, scene.vertices[vertexIndex]);
          }
        }
      }
      if (!bounds.valid) {
        continue;
      }

      ViewportPickable pickable{};
      pickable.entity_id = meshRef.entity_id;
      pickable.bounds = bounds;
      pickable.label = meshRef.label;
      pickable.require_triangle_hit = true;
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() || i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(scene.vertices[i0],
                                                          scene.vertices[i1],
                                                          scene.vertices[i2]));
      }
      if (!pickable.triangles.empty()) {
        pickables.push_back(std::move(pickable));
      }
    }

    appendEntitySdfPickables();
    for (const auto& primitive : document.sdf_primitives) {
      AddSdfPickable(pickables, primitive);
    }
    if (!pickables.empty()) {
      return pickables;
    }
  }

  for (const auto& entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto* geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty()) {
      continue;
    }
    const auto transform = ResolveEntityWorldTransform(entity, world);
    vkpt::editor::Bounds bounds{};
    for (const auto& vertex : geometry->vertices) {
      ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
    }
    if (bounds.valid) {
      ViewportPickable pickable{};
      pickable.entity_id = entity.id;
      pickable.bounds = bounds;
      pickable.label = PickableLabel(entity.name, entity.id);
      pickable.require_triangle_hit = true;
      for (std::size_t index = 0; index + 2u < geometry->indices.size(); index += 3u) {
        const auto i0 = geometry->indices[index + 0u];
        const auto i1 = geometry->indices[index + 1u];
        const auto i2 = geometry->indices[index + 2u];
        if (i0 >= geometry->vertices.size() ||
            i1 >= geometry->vertices.size() ||
            i2 >= geometry->vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(
            TransformPointForPreview(geometry->vertices[i0], transform),
            TransformPointForPreview(geometry->vertices[i1], transform),
            TransformPointForPreview(geometry->vertices[i2], transform)));
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
    }
  }

  appendEntitySdfPickables();
  for (const auto& primitive : document.sdf_primitives) {
    AddSdfPickable(pickables, primitive);
  }

  if (!pickables.empty()) {
    return pickables;
  }

  for (std::size_t instanceIndex = 0; instanceIndex < scene.instances.size(); ++instanceIndex) {
    const auto& instance = scene.instances[instanceIndex];
    vkpt::editor::Bounds bounds{};
    for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
      const uint32_t base = (instance.first_triangle + triangle) * 3u;
      if (base + 2u >= scene.indices.size()) {
        continue;
      }
      for (uint32_t corner = 0u; corner < 3u; ++corner) {
        const uint32_t vertexIndex = scene.indices[base + corner];
        if (vertexIndex < scene.vertices.size()) {
          ExpandBounds(bounds, scene.vertices[vertexIndex]);
        }
      }
    }
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(instanceIndex + 1u);
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "instance " + std::to_string(id);
      pickable.require_triangle_hit = true;
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() || i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(scene.vertices[i0],
                                                          scene.vertices[i1],
                                                          scene.vertices[i2]));
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
    }
  }

  for (std::size_t primitiveIndex = 0; primitiveIndex < scene.sdf_primitives.size(); ++primitiveIndex) {
    const auto& primitive = scene.sdf_primitives[primitiveIndex];
    const float radius = std::max(0.05f, primitive.radius);
    const vkpt::pathtracer::Vec3 extent{
        std::max(0.05f, std::fabs(primitive.scale.x) * radius),
        std::max(0.05f, std::fabs(primitive.scale.y) * radius),
        std::max(0.05f, std::fabs(primitive.scale.z) * radius),
    };
    vkpt::editor::Bounds bounds{};
    ExpandBounds(bounds, PtSub(primitive.position, extent));
    ExpandBounds(bounds, PtAdd(primitive.position, extent));
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(pickables.size() + 1u);
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "sdf " + std::to_string(id);
      pickables.push_back(std::move(pickable));
    }
  }

  return pickables;
}

ViewportRay BuildViewportRay(const ViewportCameraPose& camera,
                             float x,
                             float y,
                             float width,
                             float height,
                             float renderAspect) {
  const float safeWidth = std::max(1.0f, width);
  const float safeHeight = std::max(1.0f, height);
  const float safeAspect = std::max(0.01f, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));
  const float nx = ((x + 0.5f) / safeWidth * 2.0f - 1.0f) * safeAspect * tanHalfFov;
  const float ny = (1.0f - (y + 0.5f) / safeHeight * 2.0f) * tanHalfFov;
  return {camera.position, PtNormalize(PtAdd(PtAdd(forward, PtMul(right, nx)), PtMul(up, ny)))};
}

bool IntersectBounds(const ViewportRay& ray, const vkpt::editor::Bounds& bounds, float& t_near) {
  if (!bounds.valid) {
    return false;
  }
  const float minValues[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
  const float maxValues[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
  const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  float tMin = 1.0e-4f;
  float tMax = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(direction[axis]) <= 1.0e-6f) {
      if (origin[axis] < minValues[axis] || origin[axis] > maxValues[axis]) {
        return false;
      }
      continue;
    }
    const float invD = 1.0f / direction[axis];
    float t0 = (minValues[axis] - origin[axis]) * invD;
    float t1 = (maxValues[axis] - origin[axis]) * invD;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax) {
      return false;
    }
  }
  t_near = tMin;
  return true;
}

bool IntersectFrontFacingTriangle(const ViewportRay& ray,
                                  const ViewportPickable::Triangle& triangle,
                                  float maxDistance,
                                  float& t_out) {
  constexpr float kEpsilon = 1.0e-6f;
  const auto edge1 = PtSub(triangle.v1, triangle.v0);
  const auto edge2 = PtSub(triangle.v2, triangle.v0);
  const auto pvec = PtCross(ray.direction, edge2);
  const float det = PtDot(edge1, pvec);
  if (det <= kEpsilon) {
    return false;
  }

  const float invDet = 1.0f / det;
  const auto tvec = PtSub(ray.origin, triangle.v0);
  const float u = PtDot(tvec, pvec) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const auto qvec = PtCross(tvec, edge1);
  const float v = PtDot(ray.direction, qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float t = PtDot(edge2, qvec) * invDet;
  if (t <= kEpsilon || t >= maxDistance) {
    return false;
  }
  t_out = t;
  return true;
}

bool IntersectTriangleDoubleSided(const ViewportRay& ray,
                                  const ViewportPickable::Triangle& triangle,
                                  float maxDistance,
                                  float& t_out,
                                  vkpt::pathtracer::Vec3& normal_out) {
  constexpr float kEpsilon = 1.0e-6f;
  const auto edge1 = PtSub(triangle.v1, triangle.v0);
  const auto edge2 = PtSub(triangle.v2, triangle.v0);
  const auto pvec = PtCross(ray.direction, edge2);
  const float det = PtDot(edge1, pvec);
  if (std::fabs(det) <= kEpsilon) {
    return false;
  }

  const float invDet = 1.0f / det;
  const auto tvec = PtSub(ray.origin, triangle.v0);
  const float u = PtDot(tvec, pvec) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const auto qvec = PtCross(tvec, edge1);
  const float v = PtDot(ray.direction, qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float t = PtDot(edge2, qvec) * invDet;
  if (t <= kEpsilon || t >= maxDistance) {
    return false;
  }

  auto normal = PtNormalize(PtCross(edge1, edge2), {0.0f, 1.0f, 0.0f});
  if (PtDot(normal, ray.direction) > 0.0f) {
    normal = PtMul(normal, -1.0f);
  }
  t_out = t;
  normal_out = normal;
  return true;
}

bool BoundsOverlapsAabb(const vkpt::editor::Bounds& bounds,
                        const vkpt::pathtracer::Vec3& queryMin,
                        const vkpt::pathtracer::Vec3& queryMax) {
  if (!bounds.valid) {
    return true;
  }
  return bounds.max.x >= queryMin.x && bounds.min.x <= queryMax.x &&
         bounds.max.y >= queryMin.y && bounds.min.y <= queryMax.y &&
         bounds.max.z >= queryMin.z && bounds.min.z <= queryMax.z;
}

bool TriangleOverlapsAabb(const ViewportPickable::Triangle& triangle,
                          const vkpt::pathtracer::Vec3& queryMin,
                          const vkpt::pathtracer::Vec3& queryMax) {
  if (triangle.bounds_valid) {
    return triangle.bounds_max.x >= queryMin.x && triangle.bounds_min.x <= queryMax.x &&
           triangle.bounds_max.y >= queryMin.y && triangle.bounds_min.y <= queryMax.y &&
           triangle.bounds_max.z >= queryMin.z && triangle.bounds_min.z <= queryMax.z;
  }
  const float minX = std::min({triangle.v0.x, triangle.v1.x, triangle.v2.x});
  const float minY = std::min({triangle.v0.y, triangle.v1.y, triangle.v2.y});
  const float minZ = std::min({triangle.v0.z, triangle.v1.z, triangle.v2.z});
  const float maxX = std::max({triangle.v0.x, triangle.v1.x, triangle.v2.x});
  const float maxY = std::max({triangle.v0.y, triangle.v1.y, triangle.v2.y});
  const float maxZ = std::max({triangle.v0.z, triangle.v1.z, triangle.v2.z});
  return maxX >= queryMin.x && minX <= queryMax.x &&
         maxY >= queryMin.y && minY <= queryMax.y &&
         maxZ >= queryMin.z && minZ <= queryMax.z;
}

FpsCollisionHit TraceFpsGround(const std::vector<ViewportPickable>& pickables,
                               const vkpt::pathtracer::Vec3& origin,
                               float maxDistance,
                               float minWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const ViewportRay ray{origin, {0.0f, -1.0f, 0.0f}};
  constexpr float kQueryPad = 0.08f;
  const vkpt::pathtracer::Vec3 queryMin{
      origin.x - kQueryPad,
      origin.y - maxDistance - kQueryPad,
      origin.z - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{
      origin.x + kQueryPad,
      origin.y + kQueryPad,
      origin.z + kQueryPad};
  for (const auto& pickable : pickables) {
    if (pickable.triangles.empty()) {
      continue;
    }
    if (!BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    float boundsDistance = 0.0f;
    if (pickable.bounds.valid &&
        (!IntersectBounds(ray, pickable.bounds, boundsDistance) || boundsDistance >= bestDistance)) {
      continue;
    }
    for (const auto& triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      float distance = 0.0f;
      vkpt::pathtracer::Vec3 normal{};
      if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance, normal)) {
        continue;
      }
      if (std::fabs(normal.y) < minWalkableNormalY) {
        continue;
      }
      best.hit = true;
      best.distance = distance;
      best.position = PtAdd(origin, PtMul(ray.direction, distance));
      best.normal = normal;
      bestDistance = distance;
    }
  }
  return best;
}

FpsCollisionHit TraceFpsWall(const std::vector<ViewportPickable>& pickables,
                             const vkpt::pathtracer::Vec3& origin,
                             const vkpt::pathtracer::Vec3& direction,
                             float maxDistance,
                             float maxWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const ViewportRay ray{origin, PtNormalize(direction, {1.0f, 0.0f, 0.0f})};
  constexpr float kQueryPad = 0.08f;
  const auto end = PtAdd(origin, PtMul(ray.direction, maxDistance));
  const vkpt::pathtracer::Vec3 queryMin{
      std::min(origin.x, end.x) - kQueryPad,
      std::min(origin.y, end.y) - kQueryPad,
      std::min(origin.z, end.z) - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{
      std::max(origin.x, end.x) + kQueryPad,
      std::max(origin.y, end.y) + kQueryPad,
      std::max(origin.z, end.z) + kQueryPad};
  for (const auto& pickable : pickables) {
    if (pickable.triangles.empty()) {
      continue;
    }
    if (!BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    float boundsDistance = 0.0f;
    if (pickable.bounds.valid &&
        (!IntersectBounds(ray, pickable.bounds, boundsDistance) || boundsDistance >= bestDistance)) {
      continue;
    }
    for (const auto& triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      float distance = 0.0f;
      vkpt::pathtracer::Vec3 normal{};
      if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance, normal)) {
        continue;
      }
      if (std::fabs(normal.y) > maxWalkableNormalY) {
        continue;
      }
      best.hit = true;
      best.distance = distance;
      best.position = PtAdd(origin, PtMul(ray.direction, distance));
      best.normal = normal;
      bestDistance = distance;
    }
  }
  return best;
}

FpsCollisionHit TraceFpsBodyWall(const std::vector<ViewportPickable>& pickables,
                                 const vkpt::pathtracer::Vec3& feetPosition,
                                 const vkpt::pathtracer::Vec3& direction,
                                 const std::array<float, 3>& probeHeights,
                                 float maxDistance,
                                 float maxWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const auto rayDirection = PtNormalize(direction, {1.0f, 0.0f, 0.0f});
  constexpr float kQueryPad = 0.08f;
  const auto end = PtAdd(feetPosition, PtMul(rayDirection, maxDistance));
  const auto minmaxHeight = std::minmax_element(probeHeights.begin(), probeHeights.end());
  const float minHeight = minmaxHeight.first == probeHeights.end() ? 0.0f : *minmaxHeight.first;
  const float maxHeight = minmaxHeight.second == probeHeights.end() ? 0.0f : *minmaxHeight.second;
  const vkpt::pathtracer::Vec3 queryMin{
      std::min(feetPosition.x, end.x) - kQueryPad,
      feetPosition.y + minHeight - kQueryPad,
      std::min(feetPosition.z, end.z) - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{
      std::max(feetPosition.x, end.x) + kQueryPad,
      feetPosition.y + maxHeight + kQueryPad,
      std::max(feetPosition.z, end.z) + kQueryPad};

  for (const auto& pickable : pickables) {
    if (pickable.triangles.empty() || !BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    for (const auto& triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      for (const float height : probeHeights) {
        const ViewportRay ray{PtAdd(feetPosition, {0.0f, height, 0.0f}), rayDirection};
        float distance = 0.0f;
        vkpt::pathtracer::Vec3 normal{};
        if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance, normal)) {
          continue;
        }
        if (std::fabs(normal.y) > maxWalkableNormalY) {
          continue;
        }
        best.hit = true;
        best.distance = distance;
        best.position = PtAdd(ray.origin, PtMul(ray.direction, distance));
        best.normal = normal;
        bestDistance = distance;
      }
    }
  }
  return best;
}

vkpt::pathtracer::Vec3 ResolveFpsHorizontalDeltaForPlayer(
    const std::vector<ViewportPickable>& pickables,
    const vkpt::pathtracer::Vec3& feetPosition,
    const vkpt::pathtracer::Vec3& desiredDelta,
    float radius,
    float skin,
    float eyeHeight) {
  vkpt::pathtracer::Vec3 resolved{};
  vkpt::pathtracer::Vec3 remaining = desiredDelta;
  for (int iteration = 0; iteration < 3; ++iteration) {
    const float remainingDistance = PtLength(remaining);
    if (remainingDistance <= 1.0e-5f) {
      break;
    }
    const auto direction = PtNormalize(remaining, {1.0f, 0.0f, 0.0f});
    const float traceDistance = remainingDistance + radius + skin;
    const std::array<float, 3> probeHeights{
        std::max(radius, skin),
        std::max(radius, eyeHeight * 0.55f),
        std::max(radius, eyeHeight - radius * 0.5f)};
    const auto nearest = TraceFpsBodyWall(pickables,
                                          PtAdd(feetPosition, resolved),
                                          direction,
                                          probeHeights,
                                          traceDistance,
                                          0.62f);
    if (!nearest.hit || nearest.distance > traceDistance) {
      resolved = PtAdd(resolved, remaining);
      break;
    }

    const float allowedDistance =
        ClampFloat(nearest.distance - radius - skin, 0.0f, remainingDistance);
    resolved = PtAdd(resolved, PtMul(direction, allowedDistance));
    auto slide = PtMul(direction, remainingDistance - allowedDistance);
    auto wallNormal = vkpt::pathtracer::Vec3{nearest.normal.x, 0.0f, nearest.normal.z};
    wallNormal = PtNormalize(wallNormal, {});
    if (PtLength(wallNormal) <= 1.0e-5f) {
      break;
    }
    const float intoWall = PtDot(slide, wallNormal);
    if (intoWall < 0.0f) {
      slide = PtSub(slide, PtMul(wallNormal, intoWall));
    }
    remaining = slide;
  }
  return resolved;
}

FpsMovementResult SolveFpsMovement(const std::vector<ViewportPickable>& pickables,
                                   const FpsMovementRequest& request) {
  auto player = request.player;
  const auto eye_position = [](const FpsPlayerState& state) {
    return PtAdd(state.feet_position, {0.0f, state.eye_height, 0.0f});
  };

  const auto oldEye = eye_position(player);
  const auto oldFeet = player.feet_position;
  const bool oldGrounded = player.grounded;
  const bool oldCrouching = player.crouching;
  const bool oldRunning = player.running;
  const float oldEyeHeight = player.eye_height;

  player.crouching = request.crouching;
  player.running = request.running && !player.crouching;

  const float targetEyeHeight =
      player.crouching ? request.tuning.crouch_eye_height : request.tuning.stand_eye_height;
  const float crouchBlend = ClampFloat(request.dt_seconds * 12.0f, 0.0f, 1.0f);
  player.eye_height = player.eye_height + (targetEyeHeight - player.eye_height) * crouchBlend;

  if (player.jump_queued && player.grounded) {
    player.velocity.y = request.tuning.jump_speed;
    player.grounded = false;
  }
  player.jump_queued = false;

  if (PtLength(request.wish_move) > 1.0e-5f) {
    float speed = request.tuning.walk_speed;
    if (player.crouching) {
      speed = request.tuning.crouch_speed;
    } else if (player.running) {
      speed = request.tuning.run_speed;
    }
    if (!player.grounded) {
      speed *= request.tuning.air_control_scale;
    }
    const auto desiredDelta = PtMul(PtNormalize(request.wish_move), speed * request.dt_seconds);
    const auto horizontalDelta =
        ResolveFpsHorizontalDeltaForPlayer(pickables,
                                           player.feet_position,
                                           desiredDelta,
                                           request.tuning.radius,
                                           request.tuning.skin,
                                           player.eye_height);
    player.feet_position = PtAdd(player.feet_position, horizontalDelta);
    player.current_speed = PtLength(horizontalDelta) / std::max(request.dt_seconds, 1.0e-5f);
  } else {
    player.current_speed = 0.0f;
  }

  if (!player.grounded) {
    player.velocity.y -= request.tuning.gravity * request.dt_seconds;
  } else if (player.velocity.y < 0.0f) {
    player.velocity.y = 0.0f;
  }
  player.feet_position.y += player.velocity.y * request.dt_seconds;

  const float fallDistance = std::max(0.0f, oldFeet.y - player.feet_position.y);
  const float groundProbeStartY =
      std::max(oldFeet.y, player.feet_position.y) +
      request.tuning.step_height + request.tuning.skin;
  const float verticalProbe =
      (groundProbeStartY - player.feet_position.y) +
      request.tuning.step_height + request.tuning.skin * 2.0f;
  const auto groundOrigin = vkpt::pathtracer::Vec3{
      player.feet_position.x,
      groundProbeStartY,
      player.feet_position.z};
  const auto ground = TraceFpsGround(pickables, groundOrigin, verticalProbe, 0.62f);
  const bool sweptThroughGround =
      ground.hit && ground.position.y >= player.feet_position.y - request.tuning.skin * 2.0f &&
      ground.position.y <= oldFeet.y + request.tuning.step_height + fallDistance + request.tuning.skin;
  if (ground.hit &&
      player.velocity.y <= 0.0f &&
      (player.feet_position.y <= ground.position.y + request.tuning.step_height + request.tuning.skin ||
       sweptThroughGround)) {
    player.feet_position.y = ground.position.y;
    player.velocity.y = 0.0f;
    player.grounded = true;
  } else {
    player.grounded = false;
  }

  FpsMovementResult result{};
  result.sequence = request.sequence;
  result.collision_revision = request.collision_revision;
  result.player = player;
  const auto newEye = eye_position(player);
  result.pose_changed =
      PtLength(PtSub(newEye, oldEye)) > 1.0e-4f ||
      std::fabs(player.eye_height - oldEyeHeight) > 1.0e-4f;
  result.state_changed =
      oldGrounded != player.grounded ||
      oldCrouching != player.crouching ||
      oldRunning != player.running;
  return result;
}

FpsCollisionWorker::FpsCollisionWorker()
    : m_pickables(std::make_shared<const std::vector<ViewportPickable>>()) {
  m_thread = std::jthread([this](std::stop_token stop) {
    run(stop);
  });
}

FpsCollisionWorker::~FpsCollisionWorker() {
  stop();
}

void FpsCollisionWorker::stop() {
  if (!m_thread.joinable()) {
    return;
  }
  m_thread.request_stop();
  m_cv.notify_all();
  m_thread.join();
}

void FpsCollisionWorker::set_pickables(std::vector<ViewportPickable> pickables) {
  std::scoped_lock lock(m_mutex);
  m_pickables = std::make_shared<const std::vector<ViewportPickable>>(std::move(pickables));
  ++m_revision;
  m_pending.reset();
  m_result.reset();
}

std::uint64_t FpsCollisionWorker::collision_revision() const {
  std::scoped_lock lock(m_mutex);
  return m_revision;
}

void FpsCollisionWorker::submit(FpsMovementRequest request) {
  {
    std::scoped_lock lock(m_mutex);
    request.collision_revision = m_revision;
    m_pending = request;
  }
  m_cv.notify_one();
}

bool FpsCollisionWorker::has_work() const {
  std::scoped_lock lock(m_mutex);
  return m_busy || m_pending.has_value();
}

void FpsCollisionWorker::discard_pending_results() {
  std::scoped_lock lock(m_mutex);
  m_pending.reset();
  m_result.reset();
}

std::optional<FpsMovementResult> FpsCollisionWorker::take_latest_result() {
  std::scoped_lock lock(m_mutex);
  if (!m_result) {
    return std::nullopt;
  }
  auto result = *m_result;
  m_result.reset();
  return result;
}

void FpsCollisionWorker::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    FpsMovementRequest request{};
    std::shared_ptr<const std::vector<ViewportPickable>> pickables;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [&]() {
        return stop.stop_requested() || m_pending.has_value();
      });
      if (stop.stop_requested()) {
        break;
      }
      request = *m_pending;
      m_pending.reset();
      pickables = m_pickables;
      m_busy = true;
    }

    static const auto kEmptyPickables = std::vector<ViewportPickable>{};
    const auto& collisionPickables = pickables ? *pickables : kEmptyPickables;
    auto result = SolveFpsMovement(collisionPickables, request);

    {
      std::scoped_lock lock(m_mutex);
      m_result = result;
      m_busy = false;
    }
  }
}
bool IntersectPickableForSelection(const ViewportRay& ray,
                                   const ViewportPickable& pickable,
                                   float maxDistance,
                                   float& t_out) {
  float boundsDistance = 0.0f;
  if (!IntersectBounds(ray, pickable.bounds, boundsDistance) || boundsDistance >= maxDistance) {
    return false;
  }

  if (pickable.triangles.empty()) {
    if (pickable.require_triangle_hit) {
      return false;
    }
    t_out = boundsDistance;
    return true;
  }

  bool hit = false;
  float bestTriangleDistance = maxDistance;
  for (const auto& triangle : pickable.triangles) {
    float triangleDistance = 0.0f;
    if (!IntersectFrontFacingTriangle(ray, triangle, bestTriangleDistance, triangleDistance)) {
      continue;
    }
    bestTriangleDistance = triangleDistance;
    hit = true;
  }
  if (!hit) {
    return false;
  }
  t_out = bestTriangleDistance;
  return true;
}

std::optional<ViewportPickResult> PickViewportObject(const std::vector<ViewportPickable>& pickables,
                                                     const ViewportCameraPose& camera,
                                                     float x,
                                                     float y,
                                                     float width,
                                                     float height,
                                                     float renderAspect) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  std::optional<ViewportPickResult> best;
  for (const auto& pickable : pickables) {
    const float maxDistance = best ? best->distance : std::numeric_limits<float>::infinity();
    float distance = 0.0f;
    if (!IntersectPickableForSelection(ray, pickable, maxDistance, distance)) {
      continue;
    }
    if (!best || distance < best->distance) {
      best = ViewportPickResult{pickable.entity_id, pickable.bounds, pickable.label, distance};
    }
  }
  return best;
}

std::vector<vkpt::pathtracer::Vec3> BoundsCorners(const vkpt::editor::Bounds& bounds) {
  return {
      {bounds.min.x, bounds.min.y, bounds.min.z},
      {bounds.max.x, bounds.min.y, bounds.min.z},
      {bounds.min.x, bounds.max.y, bounds.min.z},
      {bounds.max.x, bounds.max.y, bounds.min.z},
      {bounds.min.x, bounds.min.y, bounds.max.z},
      {bounds.max.x, bounds.min.y, bounds.max.z},
      {bounds.min.x, bounds.max.y, bounds.max.z},
      {bounds.max.x, bounds.max.y, bounds.max.z},
  };
}

constexpr std::array<std::pair<int, int>, 12> kViewportBoundsEdges{{
    {0, 1}, {1, 3}, {3, 2}, {2, 0},
    {4, 5}, {5, 7}, {7, 6}, {6, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
}};

struct ProjectedViewportPoint {
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.0f;
};

std::optional<ProjectedViewportPoint> ProjectWorldPointToOverlay(
    const vkpt::pathtracer::Vec3& point,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect) {
  if (width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float aspect = std::max(0.01f, renderAspect);
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));

  const auto rel = PtSub(point, camera.position);
  const float depth = PtDot(rel, forward);
  if (depth <= 1.0e-4f) {
    return std::nullopt;
  }
  const float cameraX = PtDot(rel, right);
  const float cameraY = PtDot(rel, up);
  const float ndcX = cameraX / (depth * tanHalfFov * aspect);
  const float ndcY = cameraY / (depth * tanHalfFov);
  return ProjectedViewportPoint{
      (ndcX + 1.0f) * 0.5f * width,
      (1.0f - ndcY) * 0.5f * height,
      depth};
}

void AddProjectedOverlayLine(vkpt::platform::QtSelectionOverlayBox& box,
                             const ProjectedViewportPoint& a,
                             const ProjectedViewportPoint& b,
                             OverlayColor color,
                             float lineWidth) {
  box.lines.push_back(vkpt::platform::QtSelectionOverlayBox::Line{
      a.x, a.y, b.x, b.y, color.r, color.g, color.b, color.a, lineWidth});
}

void AddWorldOverlayLine(vkpt::platform::QtSelectionOverlayBox& box,
                         const ViewportCameraPose& camera,
                         float width,
                         float height,
                         float renderAspect,
                         const vkpt::pathtracer::Vec3& a,
                         const vkpt::pathtracer::Vec3& b,
                         OverlayColor color,
                         float lineWidth) {
  const auto projectedA = ProjectWorldPointToOverlay(a, camera, width, height, renderAspect);
  const auto projectedB = ProjectWorldPointToOverlay(b, camera, width, height, renderAspect);
  if (!projectedA || !projectedB) {
    return;
  }
  AddProjectedOverlayLine(box, *projectedA, *projectedB, color, lineWidth);
}

void AddWorldOverlayPoint(vkpt::platform::QtSelectionOverlayBox& box,
                          const ViewportCameraPose& camera,
                          float width,
                          float height,
                          float renderAspect,
                          const vkpt::pathtracer::Vec3& point,
                          OverlayColor color,
                          float radius,
                          std::string label) {
  const auto projected = ProjectWorldPointToOverlay(point, camera, width, height, renderAspect);
  if (!projected) {
    return;
  }
  box.points.push_back(vkpt::platform::QtSelectionOverlayBox::Point{
      projected->x, projected->y, radius, color.r, color.g, color.b, color.a, std::move(label)});
}

void AddGizmoCornerArc(vkpt::platform::QtSelectionOverlayBox& box,
                       const ViewportCameraPose& camera,
                       float width,
                       float height,
                       float renderAspect,
                       const vkpt::pathtracer::Vec3& corner,
                       const vkpt::pathtracer::Vec3& axisA,
                       const vkpt::pathtracer::Vec3& axisB,
                       float radius,
                       OverlayColor color,
                       float lineWidth = 1.0f) {
  if (radius <= 1.0e-4f) {
    return;
  }
  constexpr int kArcSegments = 14;
  constexpr float kHalfPi = 1.57079632679489661923f;
  auto previous = ProjectedViewportPoint{};
  bool previousValid = false;
  for (int segment = 0; segment <= kArcSegments; ++segment) {
    const float t = (static_cast<float>(segment) / static_cast<float>(kArcSegments)) * kHalfPi;
    const auto world = PtAdd(corner,
                             PtAdd(PtMul(axisA, std::cos(t) * radius),
                                   PtMul(axisB, std::sin(t) * radius)));
    const auto projected = ProjectWorldPointToOverlay(world, camera, width, height, renderAspect);
    if (projected && previousValid) {
      AddProjectedOverlayLine(box, previous, *projected, color, lineWidth);
    }
    if (projected) {
      previous = *projected;
      previousValid = true;
    } else {
      previousValid = false;
    }
  }
}

vkpt::platform::QtViewportCursor CursorForGizmoHit(const ViewportGizmoHit& hit) {
  switch (hit.kind) {
    case ViewportGizmoDragKind::Translate:
    case ViewportGizmoDragKind::FreeformTranslate:
      return vkpt::platform::QtViewportCursor::Translate;
    case ViewportGizmoDragKind::Rotate:
      return vkpt::platform::QtViewportCursor::Rotate;
    case ViewportGizmoDragKind::ScaleAxis:
      return vkpt::platform::QtViewportCursor::Scale;
    case ViewportGizmoDragKind::None:
    default:
      return vkpt::platform::QtViewportCursor::Default;
  }
}

float ScreenDistance(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

float ScreenDistanceToSegment(float px,
                              float py,
                              const ProjectedViewportPoint& a,
                              const ProjectedViewportPoint& b,
                              float* tangentX = nullptr,
                              float* tangentY = nullptr) {
  const float vx = b.x - a.x;
  const float vy = b.y - a.y;
  const float lenSq = vx * vx + vy * vy;
  if (lenSq <= 1.0e-6f) {
    if (tangentX != nullptr) {
      *tangentX = 1.0f;
    }
    if (tangentY != nullptr) {
      *tangentY = 0.0f;
    }
    return ScreenDistance(px, py, a.x, a.y);
  }
  const float t = ClampFloat(((px - a.x) * vx + (py - a.y) * vy) / lenSq, 0.0f, 1.0f);
  const float closestX = a.x + vx * t;
  const float closestY = a.y + vy * t;
  const float len = std::sqrt(lenSq);
  if (tangentX != nullptr) {
    *tangentX = vx / len;
  }
  if (tangentY != nullptr) {
    *tangentY = vy / len;
  }
  return ScreenDistance(px, py, closestX, closestY);
}

bool SameGizmoHandle(const std::optional<ViewportGizmoHit>& a,
                     const std::optional<ViewportGizmoHit>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  if (!a) {
    return true;
  }
  return a->kind == b->kind && a->axis_index == b->axis_index;
}

bool IsHoveredGizmoHandle(const std::optional<ViewportGizmoHit>& hover,
                          ViewportGizmoDragKind kind,
                          int axisIndex) {
  return hover && hover->kind == kind && hover->axis_index == axisIndex;
}

std::optional<vkpt::pathtracer::Vec3> ScreenPointOnCameraPlane(
    const ViewportCameraPose& camera,
    float x,
    float y,
    float width,
    float height,
    float renderAspect,
    const vkpt::pathtracer::Vec3& planePoint) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const float denom = PtDot(ray.direction, forward);
  if (std::fabs(denom) <= 1.0e-5f) {
    return std::nullopt;
  }
  const float t = PtDot(PtSub(planePoint, ray.origin), forward) / denom;
  if (t <= 1.0e-5f) {
    return std::nullopt;
  }
  return PtAdd(ray.origin, PtMul(ray.direction, t));
}

std::optional<ViewportGizmoHit> PickSelectionGizmoHandle(const vkpt::editor::Bounds& bounds,
                                                         const ViewportCameraPose& camera,
                                                         float width,
                                                         float height,
                                                         float renderAspect,
                                                         vkpt::editor::GizmoMode mode,
                                                         float mouseX,
                                                         float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const auto center = PtMul(PtAdd(min, max), 0.5f);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return std::nullopt;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const auto projectedAnchor = ProjectWorldPointToOverlay(anchor, camera, width, height, renderAspect);
  if (!projectedAnchor) {
    return std::nullopt;
  }

  const bool anchorMinX = std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY = std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ = std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
  };
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  std::optional<ViewportGizmoHit> best;
  float bestDistance = std::numeric_limits<float>::infinity();
  int bestPriority = -1;
  constexpr float kTranslateHitRadius = 11.0f;
  constexpr float kRotateHitRadius = 10.0f;
  constexpr float kScaleHitRadius = 12.0f;
  const auto accept_hit = [&](float distance, int priority, ViewportGizmoHit hit) {
    constexpr float kTieBreakPixels = 0.75f;
    if (distance + kTieBreakPixels < bestDistance ||
        (std::fabs(distance - bestDistance) <= kTieBreakPixels && priority > bestPriority)) {
      bestDistance = distance;
      bestPriority = priority;
      best = hit;
    }
  };

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  const auto consider_axis_line = [&](const CornerAxis& axis) {
    if (!drawTranslate || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    float tangentX = 1.0f;
    float tangentY = 0.0f;
    const float distance = ScreenDistanceToSegment(mouseX,
                                                   mouseY,
                                                   *projectedAnchor,
                                                   *projectedEnd,
                                                   &tangentX,
                                                   &tangentY);
    if (distance > kTranslateHitRadius) {
      return;
    }
    const float screenPixels = ScreenDistance(projectedAnchor->x,
                                              projectedAnchor->y,
                                              projectedEnd->x,
                                              projectedEnd->y);
    accept_hit(distance, 10, ViewportGizmoHit{
        ViewportGizmoDragKind::Translate,
        axis.axis,
        center,
        tangentX,
        tangentY,
        std::max(1.0f, screenPixels / std::max(axis.length, 1.0e-4f)),
        axis.length,
        axis.axis_index});
  };

  const auto consider_axis_endpoint = [&](const CornerAxis& axis) {
    if (!drawScale || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    const float distance = ScreenDistance(mouseX, mouseY, projectedEnd->x, projectedEnd->y);
    if (distance > kScaleHitRadius) {
      return;
    }
    float sx = projectedEnd->x - projectedAnchor->x;
    float sy = projectedEnd->y - projectedAnchor->y;
    float pixels = std::sqrt(sx * sx + sy * sy);
    if (pixels <= 1.0e-3f) {
      sx = 1.0f;
      sy = 0.0f;
      pixels = 1.0f;
    }
    accept_hit(distance, 30, ViewportGizmoHit{
        ViewportGizmoDragKind::ScaleAxis,
        axis.axis,
        anchor,
        sx / pixels,
        sy / pixels,
        std::max(1.0f, pixels / std::max(axis.length, 1.0e-4f)),
        axis.length,
        axis.axis_index});
  };

  for (const auto& axis : axes) {
    consider_axis_line(axis);
    consider_axis_endpoint(axis);
  }

  const auto consider_arc = [&](const vkpt::pathtracer::Vec3& axis,
                                int axisIndex,
                                const vkpt::pathtracer::Vec3& axisA,
                                const vkpt::pathtracer::Vec3& axisB,
                                float radius) {
    if (!drawRotate || radius <= 1.0e-4f) {
      return;
    }
    constexpr int kArcSegments = 14;
    constexpr float kHalfPi = 1.57079632679489661923f;
    auto previous = ProjectedViewportPoint{};
    bool previousValid = false;
    for (int segment = 0; segment <= kArcSegments; ++segment) {
      const float t = (static_cast<float>(segment) / static_cast<float>(kArcSegments)) * kHalfPi;
      const auto world = PtAdd(anchor,
                               PtAdd(PtMul(axisA, std::cos(t) * radius),
                                     PtMul(axisB, std::sin(t) * radius)));
      const auto projected = ProjectWorldPointToOverlay(world, camera, width, height, renderAspect);
      if (projected && previousValid) {
        float tangentX = 1.0f;
        float tangentY = 0.0f;
        const float distance = ScreenDistanceToSegment(mouseX, mouseY, previous, *projected, &tangentX, &tangentY);
        if (distance <= kRotateHitRadius) {
          accept_hit(distance, 20, ViewportGizmoHit{
              ViewportGizmoDragKind::Rotate,
              axis,
              center,
              tangentX,
              tangentY,
              1.0f,
              std::max(radius, 1.0e-4f),
              axisIndex});
        }
      }
      if (projected) {
        previous = *projected;
        previousValid = true;
      } else {
        previousValid = false;
      }
    }
  };

  consider_arc(axes[0].axis, 0, axes[1].axis, axes[2].axis, std::min(yLength, zLength));
  consider_arc(axes[1].axis, 1, axes[0].axis, axes[2].axis, std::min(xLength, zLength));
  consider_arc(axes[2].axis, 2, axes[0].axis, axes[1].axis, std::min(xLength, yLength));
  return best;
}

std::optional<ViewportGizmoHit> PickSelectionBoundsFreeform(const vkpt::editor::Bounds& bounds,
                                                            const ViewportCameraPose& camera,
                                                            float width,
                                                            float height,
                                                            float renderAspect,
                                                            vkpt::editor::GizmoMode mode,
                                                            float mouseX,
                                                            float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projected{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projected[i] = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
  }

  float bestDistance = 10.0f;
  for (const auto [a, b] : kViewportBoundsEdges) {
    const auto& pa = projected[static_cast<std::size_t>(a)];
    const auto& pb = projected[static_cast<std::size_t>(b)];
    if (!pa || !pb) {
      continue;
    }
    const float distance = ScreenDistanceToSegment(mouseX, mouseY, *pa, *pb);
    bestDistance = std::min(bestDistance, distance);
  }
  if (bestDistance > 9.5f) {
    return std::nullopt;
  }

  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  return ViewportGizmoHit{
      ViewportGizmoDragKind::FreeformTranslate,
      {},
      PtMul(PtAdd(min, max), 0.5f),
      1.0f,
      0.0f,
      1.0f,
      1.0f,
      -1};
}

void AddSelectionGizmo(vkpt::platform::QtSelectionOverlayBox& box,
                       const vkpt::editor::Bounds& bounds,
                       const ViewportCameraPose& camera,
                       float width,
                       float height,
                       float renderAspect,
                       vkpt::editor::GizmoMode mode,
                       const std::optional<ViewportGizmoHit>& hover) {
  if (mode == vkpt::editor::GizmoMode::None || !bounds.valid) {
    return;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const bool anchorMinX = std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY = std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ = std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
    OverlayColor color{};
  };
  constexpr OverlayColor kX{245u, 76u, 76u, 170u};
  constexpr OverlayColor kY{84u, 214u, 112u, 170u};
  constexpr OverlayColor kZ{74u, 144u, 255u, 170u};
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX,
       kX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY,
       kY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ,
       kZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  constexpr OverlayColor kCorner{255u, 255u, 255u, 170u};
  const auto highlight_color = [](OverlayColor color) {
    color.a = 255u;
    color.r = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.r) + 22));
    color.g = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.g) + 22));
    color.b = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.b) + 22));
    return color;
  };

  if (drawTranslate) {
    for (const auto& axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Translate, axis.axis_index);
      AddWorldOverlayLine(box,
                          camera,
                          width,
                          height,
                          renderAspect,
                          anchor,
                          axis.endpoint,
                          hovered ? highlight_color(axis.color) : axis.color,
                          hovered ? 2.4f : 1.35f);
    }
  }

  if (drawScale) {
    for (const auto& axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::ScaleAxis, axis.axis_index);
      AddWorldOverlayPoint(box,
                           camera,
                           width,
                           height,
                           renderAspect,
                           axis.endpoint,
                           hovered ? highlight_color(axis.color) : axis.color,
                           hovered ? 4.5f : 3.1f);
    }
  }

  if (drawRotate) {
    const bool hoverX = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 0);
    const bool hoverY = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 1);
    const bool hoverZ = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 2);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[1].axis, axes[2].axis,
                      std::min(yLength, zLength), hoverX ? highlight_color(kX) : kX, hoverX ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[0].axis, axes[2].axis,
                      std::min(xLength, zLength), hoverY ? highlight_color(kY) : kY, hoverY ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[0].axis, axes[1].axis,
                      std::min(xLength, yLength), hoverZ ? highlight_color(kZ) : kZ, hoverZ ? 2.3f : 1.1f);
  }

  if (drawTranslate || drawRotate || drawScale) {
    AddWorldOverlayPoint(box, camera, width, height, renderAspect, anchor, kCorner, 2.8f);
  }
}

std::optional<vkpt::platform::QtSelectionOverlayBox> ProjectBoundsToOverlay(
    const vkpt::editor::Bounds& bounds,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect,
    std::string label,
    bool primary,
    vkpt::editor::GizmoMode gizmoMode,
    const std::optional<ViewportGizmoHit>& hover) {
  if (!bounds.valid || width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  constexpr std::array<std::pair<int, int>, 12> kBoundsEdges{{
      {0, 1}, {1, 3}, {3, 2}, {2, 0},
      {4, 5}, {5, 7}, {7, 6}, {6, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  }};
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projectedCorners{};
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  bool anyProjected = false;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projectedCorners[i] = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (!projectedCorners[i]) {
      continue;
    }
    minX = std::min(minX, projectedCorners[i]->x);
    minY = std::min(minY, projectedCorners[i]->y);
    maxX = std::max(maxX, projectedCorners[i]->x);
    maxY = std::max(maxY, projectedCorners[i]->y);
    anyProjected = true;
  }
  if (!anyProjected) {
    return std::nullopt;
  }
  constexpr OverlayColor kPrimaryBox{255u, 214u, 64u, 245u};
  constexpr OverlayColor kSecondaryBox{102u, 204u, 255u, 230u};
  vkpt::platform::QtSelectionOverlayBox box{};
  box.label = std::move(label);
  box.primary = primary;
  auto boxColor = primary ? kPrimaryBox : kSecondaryBox;
  if (primary && hover && hover->kind == ViewportGizmoDragKind::FreeformTranslate) {
    boxColor = {255u, 244u, 164u, 255u};
  }
  for (const auto [a, b] : kBoundsEdges) {
    if (!projectedCorners[static_cast<std::size_t>(a)] ||
        !projectedCorners[static_cast<std::size_t>(b)]) {
      continue;
    }
    AddProjectedOverlayLine(box,
                            *projectedCorners[static_cast<std::size_t>(a)],
                            *projectedCorners[static_cast<std::size_t>(b)],
                            boxColor,
                            primary
                                ? (hover && hover->kind == ViewportGizmoDragKind::FreeformTranslate ? 2.8f : 2.0f)
                                : 1.5f);
  }
  if (primary) {
    AddSelectionGizmo(box, bounds, camera, width, height, renderAspect, gizmoMode, hover);
  }

  const float margin = 4.0f;
  minX = ClampFloat(minX - margin, -width, width * 2.0f);
  minY = ClampFloat(minY - margin, -height, height * 2.0f);
  maxX = ClampFloat(maxX + margin, -width, width * 2.0f);
  maxY = ClampFloat(maxY + margin, -height, height * 2.0f);
  if (maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }
  box.x = minX;
  box.y = minY;
  box.width = maxX - minX;
  box.height = maxY - minY;
  return box;
}

std::vector<vkpt::platform::QtSelectionOverlayBox> BuildSelectionOverlayBoxes(
    const vkpt::editor::SelectionState& selection,
    const std::vector<ViewportPickable>& pickables,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect,
    vkpt::editor::GizmoMode gizmoMode,
    const std::optional<ViewportGizmoHit>& activeHover) {
  std::vector<vkpt::platform::QtSelectionOverlayBox> boxes;
  for (const auto selectedId : selection.selected_entity_ids) {
    auto bounds = std::optional<vkpt::editor::Bounds>{};
    std::string label = "entity " + std::to_string(selectedId);
    for (const auto& item : selection.per_item_bounds) {
      if (item.entity_id == selectedId && item.bounds.valid) {
        bounds = item.bounds;
        break;
      }
    }
    for (const auto& pickable : pickables) {
      if (pickable.entity_id == selectedId) {
        if (!bounds) {
          bounds = pickable.bounds;
        }
        label = pickable.label;
        break;
      }
    }
    if (!bounds) {
      continue;
    }
    auto projected = ProjectBoundsToOverlay(*bounds,
                                            camera,
                                            width,
                                            height,
                                            renderAspect,
                                            label,
                                            selectedId == selection.active_primary_entity,
                                            selectedId == selection.active_primary_entity
                                                ? gizmoMode
                                                : vkpt::editor::GizmoMode::None,
                                            selectedId == selection.active_primary_entity
                                                ? activeHover
                                                : std::optional<ViewportGizmoHit>{});
    if (projected) {
      boxes.push_back(std::move(*projected));
    }
  }
  return boxes;
}

void RebuildSelectionBounds(vkpt::editor::SelectionState& selection,
                            const std::vector<ViewportPickable>& pickables) {
  selection.per_item_bounds.clear();
  selection.aggregate_bounds = {};
  for (const auto selectedId : selection.selected_entity_ids) {
    const auto it = std::find_if(pickables.begin(), pickables.end(),
                                 [selectedId](const ViewportPickable& pickable) {
                                   return pickable.entity_id == selectedId;
                                 });
    if (it == pickables.end() || !it->bounds.valid) {
      continue;
    }
    selection.per_item_bounds.push_back({selectedId, it->bounds});
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.min));
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.max));
  }
}

}  // namespace vkpt::app

#endif
