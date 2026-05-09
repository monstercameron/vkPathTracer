#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "core/Logging.h"
#include "core/metrics/Metrics.h"
#include "physics/Channels.h"

#ifdef PT_ENABLE_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#endif

#ifndef PT_JOLT_GIT_TAG_STRING
#define PT_JOLT_GIT_TAG_STRING "disabled"
#endif

namespace vkpt::physics {

namespace {

void LogPhysicsException(std::string_view operation, std::string_view error) noexcept {
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error,
        "physics",
        "physics worker exception",
        {{"operation", std::string(operation)}, {"error", std::string(error)}});
  } catch (...) {
  }
}

void LogPhysicsException(std::string_view operation) noexcept {
  LogPhysicsException(operation, "non-standard exception");
}

}  // namespace
namespace {

std::vector<PhysicsBodySync> BuildPhysicsBodySyncList(const vkpt::scene::SceneWorld& world) {
  std::vector<PhysicsBodySync> bodies;
  bodies.reserve(world.all_entities().size());
  for (const auto entity_id : world.all_entities()) {
    const auto* entity = world.get_entity(entity_id);
    if (entity == nullptr || !entity->physics_body.has_value()) {
      continue;
    }

    PhysicsBodySync sync;
    sync.entity = entity_id;
    sync.body = *entity->physics_body;
    if (entity->transform.has_value()) {
      sync.transform = *entity->transform;
    }
    if (const auto* world_transform = world.world_transform(entity_id)) {
      sync.transform.translation = world_transform->translation;
      sync.transform.rotation = world_transform->rotation;
      sync.transform.scale = world_transform->scale;
      sync.transform.dirty = true;
    }
    bodies.push_back(std::move(sync));
  }
  return bodies;
}

PhysicsSyncSummary BuildSyncSummary(const std::vector<PhysicsBodySync>& bodies, std::size_t ecs_entities) {
  PhysicsSyncSummary summary;
  summary.ecs_entities = ecs_entities;
  for (const auto& sync : bodies) {
    ++summary.physics_components;
    const auto& body = sync.body;
    if (!body.enabled) {
      ++summary.disabled_bodies;
      continue;
    }
    ++summary.enabled_bodies;
    if (body.dynamic) {
      ++summary.dynamic_bodies;
    } else {
      ++summary.static_bodies;
    }
  }
  return summary;
}

const char* ErrorCodeName(vkpt::core::ErrorCode code) noexcept {
  switch (code) {
    case vkpt::core::ErrorCode::Ok:
      return "ok";
    case vkpt::core::ErrorCode::InvalidArgument:
      return "invalid_argument";
    case vkpt::core::ErrorCode::NotFound:
      return "not_found";
    case vkpt::core::ErrorCode::IOError:
      return "io_error";
    case vkpt::core::ErrorCode::Unsupported:
      return "unsupported";
    case vkpt::core::ErrorCode::Timeout:
      return "timeout";
    case vkpt::core::ErrorCode::Internal:
      return "internal";
    case vkpt::core::ErrorCode::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

void SetPhysicsBodyCountMetrics(const PhysicsSyncSummary& summary) {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  registry.gauge("vkp.physics.body_count.ecs_entities").set(static_cast<double>(summary.ecs_entities));
  registry.gauge("vkp.physics.body_count.components").set(static_cast<double>(summary.physics_components));
  registry.gauge("vkp.physics.body_count.enabled").set(static_cast<double>(summary.enabled_bodies));
  registry.gauge("vkp.physics.body_count.disabled").set(static_cast<double>(summary.disabled_bodies));
  registry.gauge("vkp.physics.body_count.dynamic").set(static_cast<double>(summary.dynamic_bodies));
  registry.gauge("vkp.physics.body_count.static").set(static_cast<double>(summary.static_bodies));
  registry.gauge("vkp.physics.body_count.backend").set(static_cast<double>(summary.backend_bodies));
  registry.gauge("vkp.physics.body_count").set(static_cast<double>(summary.enabled_bodies));
}

void SetPhysicsStatusBodyCounts(PhysicsStatus& status, const PhysicsSyncSummary& summary) {
  status.body_counts["ecs_entities"] = summary.ecs_entities;
  status.body_counts["components"] = summary.physics_components;
  status.body_counts["enabled"] = summary.enabled_bodies;
  status.body_counts["disabled"] = summary.disabled_bodies;
  status.body_counts["dynamic"] = summary.dynamic_bodies;
  status.body_counts["static"] = summary.static_bodies;
  status.body_counts["backend"] = summary.backend_bodies;
}

void RecordPhysicsSyncTelemetry(const PhysicsSyncSummary& summary) {
  SetPhysicsBodyCountMetrics(summary);
  // Sync drift = backend disagrees with what we asked it to sync. Comparing
  // enabled_bodies against ecs_entities is wrong: most entities don't have
  // physics components, so the inequality is the steady-state, not a drift,
  // and the warning floods the log on every sync (multiple per second on
  // both UI and worker threads in 1000+ entity scenes).
  if (summary.enabled_bodies != summary.backend_bodies) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        "physics",
        "physics.sync_drift",
        {{"enabled_bodies", std::to_string(summary.enabled_bodies)},
         {"backend_bodies", std::to_string(summary.backend_bodies)},
         {"ecs_entities", std::to_string(summary.ecs_entities)}});
  }
}

void RecordPhysicsStepTelemetry(std::uint64_t step_us, std::size_t contact_count) {
  VKP_METRIC_OBSERVE("vkp.physics.step_us", step_us);
  VKP_METRIC_OBSERVE("vkp.physics.contacts_per_step", contact_count);
}

void LogPhysicsStepFailed(std::uint64_t frame_idx,
                          std::string_view error_message,
                          std::size_t body_count) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Error,
      "physics",
      "physics.step_failed",
      {{"frame_idx", std::to_string(frame_idx)},
       {"error_message", std::string(error_message)},
       {"body_count", std::to_string(body_count)}},
      frame_idx);
}

std::uint64_t CurrentFlowId(const vkpt::core::contracts::IFlowSource* flow_source) noexcept {
  return flow_source == nullptr ? 0u : flow_source->current_flow_id();
}

enum class PhysicsLifecycleState : std::uint8_t {
  Constructed,
  Synced,
  Stepping,
  Failed,
  ShuttingDown,
};

vkpt::core::contracts::ComponentLifecycle ToComponentLifecycle(PhysicsLifecycleState state) noexcept {
  using vkpt::core::contracts::ComponentLifecycle;
  switch (state) {
    case PhysicsLifecycleState::Constructed:
      return ComponentLifecycle::Uninitialized;
    case PhysicsLifecycleState::Synced:
      return ComponentLifecycle::Ready;
    case PhysicsLifecycleState::Stepping:
      return ComponentLifecycle::Busy;
    case PhysicsLifecycleState::Failed:
      return ComponentLifecycle::Failed;
    case PhysicsLifecycleState::ShuttingDown:
      return ComponentLifecycle::ShuttingDown;
  }
  return ComponentLifecycle::Failed;
}

bool CanStep(PhysicsLifecycleState state) noexcept {
  return state == PhysicsLifecycleState::Synced || state == PhysicsLifecycleState::Stepping;
}

void AssertCanStep(PhysicsLifecycleState state) {
#ifndef NDEBUG
  assert(CanStep(state) && "IPhysicsWorld::step_fixed requires sync_from_scene_world/sync_from_bodies first");
#else
  (void)state;
#endif
}

std::uint64_t SteadyNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void MarkPhysicsTick(PhysicsStatus& status, PhysicsLifecycleState lifecycle) {
  status.lifecycle = ToComponentLifecycle(lifecycle);
  status.last_tick_ns = SteadyNowNs();
  ++status.ticks_total;
}

void MarkPhysicsError(PhysicsStatus& status,
                      PhysicsLifecycleState lifecycle,
                      std::string error) {
  status.lifecycle = ToComponentLifecycle(lifecycle);
  status.last_error = std::move(error);
  status.last_tick_ns = SteadyNowNs();
  ++status.errors_total;
}

std::unordered_map<std::string, std::size_t> BodyCountsMap(const PhysicsSyncSummary& summary) {
  return {
      {"ecs_entities", summary.ecs_entities},
      {"components", summary.physics_components},
      {"enabled", summary.enabled_bodies},
      {"disabled", summary.disabled_bodies},
      {"dynamic", summary.dynamic_bodies},
      {"static", summary.static_bodies},
      {"backend", summary.backend_bodies},
  };
}

PhysicsStepStats MakeStepStats(std::uint64_t step_us,
                               std::uint32_t solver_iterations,
                               std::size_t contact_count,
                               const PhysicsSyncSummary& summary) {
  PhysicsStepStats stats;
  stats.step_us = step_us;
  stats.solver_iterations = solver_iterations;
  stats.contact_count = contact_count;
  stats.active_bodies = summary.backend_bodies == 0u ? summary.enabled_bodies : summary.backend_bodies;
  return stats;
}

PhysicsStepSnapshot MakeStepSnapshot(std::uint64_t generation,
                                     std::uint64_t request_id,
                                     std::uint64_t flow_id,
                                     const PhysicsSyncSummary& summary,
                                     PhysicsStepStats stats,
                                     std::vector<PhysicsTransformWrite> writes) {
  PhysicsStepSnapshot snapshot;
  snapshot.generation = generation;
  snapshot.wall_time_ns = SteadyNowNs();
  snapshot.request_id = request_id;
  snapshot.flow_id = flow_id;
  snapshot.complete = true;
  snapshot.transform_writes = std::move(writes);
  snapshot.body_counts = BodyCountsMap(summary);
  snapshot.stats = stats;
  return snapshot;
}

float FallbackAbsScale(float value) {
  return std::max(std::abs(value), 0.001f);
}

float FallbackBodyRadius(const PhysicsBodySync& sync) {
  const auto& scale = sync.transform.scale;
  if (sync.body.shape == "sphere") {
    return std::max({FallbackAbsScale(scale.x), FallbackAbsScale(scale.y), FallbackAbsScale(scale.z)});
  }
  return std::max({FallbackAbsScale(scale.x), FallbackAbsScale(scale.y), FallbackAbsScale(scale.z)}) * 0.5f;
}

float FallbackBodyHalfHeight(const PhysicsBodySync& sync) {
  if (sync.body.shape == "sphere") {
    return FallbackBodyRadius(sync);
  }
  return FallbackAbsScale(sync.transform.scale.y) * 0.5f;
}

bool FallbackNearlyEqual(float lhs, float rhs, float epsilon = 0.0001f) {
  return std::abs(lhs - rhs) <= epsilon;
}

bool FallbackNearlyEqual(const vkpt::scene::Vec3& lhs,
                         const vkpt::scene::Vec3& rhs,
                         float epsilon = 0.0001f) {
  return FallbackNearlyEqual(lhs.x, rhs.x, epsilon) &&
         FallbackNearlyEqual(lhs.y, rhs.y, epsilon) &&
         FallbackNearlyEqual(lhs.z, rhs.z, epsilon);
}

vkpt::scene::Vec3 FallbackAdd(const vkpt::scene::Vec3& lhs, const vkpt::scene::Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

vkpt::scene::Vec3 FallbackSub(const vkpt::scene::Vec3& lhs, const vkpt::scene::Vec3& rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

vkpt::scene::Vec3 FallbackMul(const vkpt::scene::Vec3& value, float scalar) {
  return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float FallbackDot(const vkpt::scene::Vec3& lhs, const vkpt::scene::Vec3& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

float FallbackLength(const vkpt::scene::Vec3& value) {
  return std::sqrt(std::max(0.0f, FallbackDot(value, value)));
}

vkpt::scene::Vec3 FallbackNormalize(const vkpt::scene::Vec3& value,
                                    const vkpt::scene::Vec3& fallback = {0.0f, 1.0f, 0.0f}) {
  const float len = FallbackLength(value);
  if (len <= 0.000001f) {
    return fallback;
  }
  return FallbackMul(value, 1.0f / len);
}

class NullPhysicsWorld final : public IPhysicsWorld {
 public:
  PhysicsEngineInfo engine_info() const override {
    return GetCompiledPhysicsEngineInfo();
  }

  void set_flow_source(const vkpt::core::contracts::IFlowSource* flow_source) override {
    m_flowSource = flow_source;
  }

  PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) override {
    return sync_from_bodies(BuildPhysicsBodySyncList(world), world.all_entities().size());
  }

  PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies, std::size_t ecs_entities) override {
    m_summary = BuildSyncSummary(bodies, ecs_entities);
    std::unordered_set<vkpt::core::StableEntityId> seen;
    seen.reserve(bodies.size());

    for (auto& sync : bodies) {
      seen.insert(sync.entity);
      if (!sync.body.enabled) {
        m_bodies.erase(sync.entity);
        continue;
      }

      auto existing = m_bodies.find(sync.entity);
      if (existing == m_bodies.end()) {
        BodyRecord record;
        record.sync = std::move(sync);
        record.last_published_transform = record.sync.transform;
        const auto entity_id = record.sync.entity;
        m_bodies.emplace(entity_id, std::move(record));
        continue;
      }

      if (!FallbackNearlyEqual(existing->second.sync.transform.translation,
                               sync.transform.translation,
                               0.01f)) {
        existing->second.velocity = {};
        existing->second.asleep = false;
      }
      existing->second.sync = std::move(sync);
    }

    std::vector<vkpt::core::StableEntityId> stale;
    for (const auto& [entity_id, _] : m_bodies) {
      if (!seen.contains(entity_id)) {
        stale.push_back(entity_id);
      }
    }
    for (const auto entity_id : stale) {
      m_bodies.erase(entity_id);
    }

    m_summary.backend_bodies = m_bodies.size();
    m_status.backend = engine_info().engine_name;
    m_status.current_flow_id = CurrentFlowId(m_flowSource);
    SetPhysicsStatusBodyCounts(m_status, m_summary);
    m_status.last_error.clear();
    MarkPhysicsTick(m_status, PhysicsLifecycleState::Synced);
    RecordPhysicsSyncTelemetry(m_summary);
    m_lifecycle = PhysicsLifecycleState::Synced;
    return m_summary;
  }

  vkpt::core::Result<PhysicsStepStats> step_fixed(const PhysicsStepConfig& config) override {
    AssertCanStep(m_lifecycle);
    if (!CanStep(m_lifecycle)) {
      MarkPhysicsError(m_status, PhysicsLifecycleState::Failed, "not_synced");
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    m_lifecycle = PhysicsLifecycleState::Stepping;
    const auto step_start = std::chrono::steady_clock::now();
    m_status.fixed_dt_ms = static_cast<double>(config.fixed_dt) * 1000.0;
    m_status.current_flow_id = CurrentFlowId(m_flowSource);
    const auto request_id = m_nextRequestId++;
    if (!std::isfinite(config.fixed_dt) || config.fixed_dt <= 0.0f || config.collision_steps <= 0) {
      MarkPhysicsError(m_status, PhysicsLifecycleState::Failed, "invalid_argument");
      LogPhysicsStepFailed(m_status.current_flow_id, m_status.last_error, m_summary.backend_bodies);
      m_lifecycle = PhysicsLifecycleState::Failed;
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    m_status.last_error.clear();
    m_lastContactCount = 0u;
    m_writes.clear();

    std::vector<vkpt::core::StableEntityId> ids;
    ids.reserve(m_bodies.size());
    for (const auto& [entity_id, record] : m_bodies) {
      if (record.sync.body.enabled) {
        ids.push_back(entity_id);
      }
    }
    std::sort(ids.begin(), ids.end());

    const int substeps = std::max(1, config.collision_steps);
    const float dt = config.fixed_dt / static_cast<float>(substeps);
    // The fallback backend is intentionally simple but deterministic: stable
    // entity ordering keeps headless tests reproducible without Jolt.
    for (int step = 0; step < substeps; ++step) {
      integrate_dynamic_bodies(ids, dt);
      if (config.collision_detection_enabled) {
        solve_ground_contacts(ids);
        solve_sphere_contacts(ids);
      }
    }

    for (auto& [entity_id, record] : m_bodies) {
      if (!record.sync.body.dynamic || !record.sync.body.enabled) {
        continue;
      }
      if (FallbackNearlyEqual(record.last_published_transform.translation,
                              record.sync.transform.translation,
                              0.0005f) &&
          FallbackNearlyEqual(record.last_published_transform.scale,
                              record.sync.transform.scale,
                              0.0005f)) {
        continue;
      }
      record.sync.transform.dirty = true;
      record.last_published_transform = record.sync.transform;
      m_writes.push_back({entity_id, record.sync.transform, request_id});
    }
    const auto step_end = std::chrono::steady_clock::now();
    m_status.last_step_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(step_end - step_start).count());
    m_status.last_contacts_per_step = m_lastContactCount;
    RecordPhysicsStepTelemetry(m_status.last_step_us, m_lastContactCount);
    const auto stats = MakeStepStats(m_status.last_step_us,
                                     static_cast<std::uint32_t>(std::max(1, config.collision_steps)),
                                     m_lastContactCount,
                                     m_summary);
    m_lastStepSnapshot = MakeStepSnapshot(++m_stepGeneration,
                                          request_id,
                                          m_status.current_flow_id,
                                          m_summary,
                                          stats,
                                          m_writes);
    m_lifecycle = PhysicsLifecycleState::Synced;
    MarkPhysicsTick(m_status, m_lifecycle);
    return vkpt::core::Result<PhysicsStepStats>::ok(stats);
  }

  PhysicsStepSnapshot step_snapshot() const override {
    return m_lastStepSnapshot;
  }

  PhysicsStatus status() const override {
    return m_status;
  }

 private:
  struct BodyRecord {
    PhysicsBodySync sync;
    vkpt::scene::Vec3 velocity{};
    vkpt::scene::TransformComponent last_published_transform;
    bool asleep = false;
  };

  float ground_plane_y() const {
    bool found = false;
    float ground_y = 0.0f;
    for (const auto& [_, record] : m_bodies) {
      const auto& body = record.sync.body;
      if (!body.enabled || body.dynamic || body.trigger) {
        continue;
      }
      if (record.sync.transform.translation.y >= ground_y || !found) {
        ground_y = record.sync.transform.translation.y;
        found = true;
      }
    }
    return found ? ground_y : 0.0f;
  }

  void integrate_dynamic_bodies(const std::vector<vkpt::core::StableEntityId>& ids, float dt) {
    for (const auto entity_id : ids) {
      auto it = m_bodies.find(entity_id);
      if (it == m_bodies.end()) {
        continue;
      }
      auto& record = it->second;
      const auto& body = record.sync.body;
      if (!body.dynamic || body.trigger) {
        continue;
      }
      if (record.asleep && body.allow_sleeping) {
        continue;
      }
      record.velocity.y -= 9.81f * body.gravity_scale * dt;
      record.sync.transform.translation =
          FallbackAdd(record.sync.transform.translation, FallbackMul(record.velocity, dt));
      record.sync.transform.dirty = true;
    }
  }

  void solve_ground_contacts(const std::vector<vkpt::core::StableEntityId>& ids) {
    const float ground_y = ground_plane_y();
    for (const auto entity_id : ids) {
      auto it = m_bodies.find(entity_id);
      if (it == m_bodies.end()) {
        continue;
      }
      auto& record = it->second;
      const auto& body = record.sync.body;
      if (!body.dynamic || body.trigger) {
        continue;
      }
      const float half_height = FallbackBodyHalfHeight(record.sync);
      const float min_center_y = ground_y + half_height;
      if (record.sync.transform.translation.y >= min_center_y) {
        record.asleep = false;
        continue;
      }

      ++m_lastContactCount;
      record.sync.transform.translation.y = min_center_y;
      if (record.velocity.y < 0.0f) {
        record.velocity.y = -record.velocity.y * std::max(0.0f, body.restitution);
      }
      const float ground_friction = std::max(0.0f, body.friction);
      const float lateral_damp = std::max(0.0f, 1.0f - ground_friction * 0.18f);
      record.velocity.x *= lateral_damp;
      record.velocity.z *= lateral_damp;
      if (std::abs(record.velocity.y) < 0.04f) {
        record.velocity.y = 0.0f;
      }
      if (body.allow_sleeping && FallbackLength(record.velocity) < 0.05f) {
        record.velocity = {};
        record.asleep = true;
      }
      record.sync.transform.dirty = true;
    }
  }

  void solve_sphere_contacts(const std::vector<vkpt::core::StableEntityId>& ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
      auto lhs_it = m_bodies.find(ids[i]);
      if (lhs_it == m_bodies.end()) {
        continue;
      }
      auto& lhs = lhs_it->second;
      if (!lhs.sync.body.dynamic || lhs.sync.body.trigger || lhs.sync.body.shape != "sphere") {
        continue;
      }
      for (std::size_t j = i + 1; j < ids.size(); ++j) {
        auto rhs_it = m_bodies.find(ids[j]);
        if (rhs_it == m_bodies.end()) {
          continue;
        }
        auto& rhs = rhs_it->second;
        if (!rhs.sync.body.dynamic || rhs.sync.body.trigger || rhs.sync.body.shape != "sphere") {
          continue;
        }

        const float lhs_radius = FallbackBodyRadius(lhs.sync);
        const float rhs_radius = FallbackBodyRadius(rhs.sync);
        const float min_dist = lhs_radius + rhs_radius;
        const auto delta = FallbackSub(rhs.sync.transform.translation, lhs.sync.transform.translation);
        const float dist = FallbackLength(delta);
        if (dist >= min_dist || dist <= 0.000001f) {
          continue;
        }

        ++m_lastContactCount;
        const auto normal = FallbackNormalize(delta, {1.0f, 0.0f, 0.0f});
        const float penetration = min_dist - dist;
        const float lhs_inv_mass = 1.0f / std::max(0.001f, lhs.sync.body.mass);
        const float rhs_inv_mass = 1.0f / std::max(0.001f, rhs.sync.body.mass);
        const float inv_sum = std::max(0.001f, lhs_inv_mass + rhs_inv_mass);
        lhs.sync.transform.translation =
            FallbackAdd(lhs.sync.transform.translation,
                        FallbackMul(normal, -penetration * (lhs_inv_mass / inv_sum)));
        rhs.sync.transform.translation =
            FallbackAdd(rhs.sync.transform.translation,
                        FallbackMul(normal, penetration * (rhs_inv_mass / inv_sum)));

        const auto relative_velocity = FallbackSub(rhs.velocity, lhs.velocity);
        const float normal_speed = FallbackDot(relative_velocity, normal);
        if (normal_speed < 0.0f) {
          const float restitution = std::min(lhs.sync.body.restitution, rhs.sync.body.restitution);
          const float impulse = -(1.0f + restitution) * normal_speed / inv_sum;
          lhs.velocity = FallbackAdd(lhs.velocity, FallbackMul(normal, -impulse * lhs_inv_mass));
          rhs.velocity = FallbackAdd(rhs.velocity, FallbackMul(normal, impulse * rhs_inv_mass));
        }

        lhs.asleep = false;
        rhs.asleep = false;
        lhs.sync.transform.dirty = true;
        rhs.sync.transform.dirty = true;
      }
    }
  }

  PhysicsSyncSummary m_summary;
  PhysicsStatus m_status;
  PhysicsStepSnapshot m_lastStepSnapshot;
  const vkpt::core::contracts::IFlowSource* m_flowSource = nullptr;
  std::unordered_map<vkpt::core::StableEntityId, BodyRecord> m_bodies;
  std::vector<PhysicsTransformWrite> m_writes;
  std::size_t m_lastContactCount = 0u;
  std::uint64_t m_stepGeneration = 0u;
  std::uint64_t m_nextRequestId = 1u;
  PhysicsLifecycleState m_lifecycle = PhysicsLifecycleState::Constructed;
};

class ThreadedPhysicsWorld final : public IPhysicsWorld {
 private:
  struct WorkerState {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable bool started = false;
    mutable bool stopping = false;
    std::thread::id worker_id;
    std::unique_ptr<IPhysicsWorld> backend;
    PhysicsEngineInfo cached_info;
    PhysicsCmdRing cmd_ring{1024u};
    mutable PhysicsDeltaRing delta_ring{4096u};
    std::atomic<std::uint64_t> submitted_steps{0u};
    std::atomic<std::uint64_t> completed_steps{0u};
    std::vector<PhysicsBodySync> known_bodies;
    std::size_t ecs_entities = 0u;
    vkpt::scene::Vec3 gravity{0.0f, -9.81f, 0.0f};
    mutable std::mutex status_mutex;
    PhysicsStatus status;
    PhysicsStepSnapshot latest_snapshot;
    PhysicsStepSnapshot pending_snapshot;
    std::uint64_t step_generation = 0u;
  };

  struct PendingStep {
    PhysicsStepConfig config;
    float remaining_dt = 0.0f;
    std::uint64_t request_id = 0;
    std::uint64_t flow_id = 0;
    std::uint64_t step_us = 0;
    std::size_t contact_count = 0;
    std::uint32_t solver_iterations = 0;
  };

 public:
  // Precondition (enforced by CreatePhysicsWorld factory): `backend` must be non-null.
  // Construction skips initialization when backend is null so the worker thread is
  // never started; callers should obtain ThreadedPhysicsWorld via CreatePhysicsWorld
  // which validates the backend up-front and reports failure via the factory's
  // Result-returning interface rather than constructor throw.
  explicit ThreadedPhysicsWorld(std::unique_ptr<IPhysicsWorld> backend)
      : m_state(std::make_shared<WorkerState>()) {
    if (!backend) {
      m_state->status.lifecycle = ToComponentLifecycle(PhysicsLifecycleState::Failed);
      m_state->status.last_error = "physics backend is null";
      m_lifecycle.store(PhysicsLifecycleState::Failed, std::memory_order_release);
      return;
    }
    m_state->cached_info = backend->engine_info();
    m_state->cached_info.runs_on_worker_thread = true;
    m_state->cached_info.threading_model = "dedicated_worker";
    m_state->status.backend = m_state->cached_info.engine_name;
    m_state->status.lifecycle = ToComponentLifecycle(PhysicsLifecycleState::Constructed);
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Info,
          "physics",
          "config",
          {{"backend", m_state->cached_info.engine_name},
           {"threading_model", m_state->cached_info.threading_model},
           {"cmd_ring_capacity", "1024"},
           {"delta_ring_capacity", "4096"}});
    } catch (...) {
    }
    m_state->backend = std::move(backend);
    m_thread = std::thread([state = m_state]() {
      worker_loop(state);
    });
    std::unique_lock lock(m_state->mutex);
    m_state->cv.wait(lock, [state = m_state]() {
      return state->started;
    });
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Info,
          "physics",
          "started",
          {{"backend", m_state->cached_info.engine_name}});
    } catch (...) {
    }
  }

  ~ThreadedPhysicsWorld() noexcept override {
    m_lifecycle.store(PhysicsLifecycleState::ShuttingDown, std::memory_order_release);
    {
      std::scoped_lock lock(m_state->status_mutex);
      m_state->status.lifecycle = ToComponentLifecycle(PhysicsLifecycleState::ShuttingDown);
    }
    if (!m_thread.joinable()) {
      return;
    }
    const auto state = m_state;
    if (std::this_thread::get_id() != state->worker_id) {
      enqueue_command(PhysicsCmd{PhysicsShutdownCmd{next_request_id()}});
      {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
      }
      state->cv.notify_one();
      m_thread.join();
    } else {
      state->backend.reset();
      {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
      }
      state->cv.notify_one();
      m_thread.detach();
    }
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Info,
          "physics",
          "stopped",
          {{"backend", state->cached_info.engine_name}});
    } catch (...) {
    }
  }

  PhysicsEngineInfo engine_info() const override {
    return m_state->cached_info;
  }

  void set_flow_source(const vkpt::core::contracts::IFlowSource* flow_source) override {
    m_flowSource.store(flow_source, std::memory_order_release);
  }

  PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) override {
    auto bodies = BuildPhysicsBodySyncList(world);
    const auto ecs_entities = world.all_entities().size();
    return sync_from_bodies(std::move(bodies), ecs_entities);
  }

  PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies, std::size_t ecs_entities) override {
    auto summary = BuildSyncSummary(bodies, ecs_entities);
    enqueue_command(PhysicsCmd{PhysicsSyncBodiesCmd{next_request_id(), std::move(bodies), ecs_entities}});
    m_latestSummary = summary;
    // Do NOT call RecordPhysicsSyncTelemetry here — the summary at this
    // point reflects the request, not the backend. backend_bodies is 0
    // until the worker thread processes the enqueued command and pushes
    // the real summary back via PhysicsSyncSummaryDelta. Telemetry is
    // recorded on the worker side (sync_backend_from_known_bodies) where
    // the numbers match reality. Logging here was producing
    // physics.sync_drift warnings every sync (multiple per second on
    // 1000+ entity scenes) with bogus backend_bodies=0.
    {
      std::scoped_lock lock(m_state->status_mutex);
      m_state->status.backend = m_state->cached_info.engine_name;
      m_state->status.current_flow_id = current_flow_id();
      SetPhysicsStatusBodyCounts(m_state->status, summary);
      m_state->status.last_error.clear();
      MarkPhysicsTick(m_state->status, PhysicsLifecycleState::Synced);
    }
    m_lifecycle.store(PhysicsLifecycleState::Synced, std::memory_order_release);
    return summary;
  }

  vkpt::core::Result<PhysicsStepStats> step_fixed(const PhysicsStepConfig& config) override {
    const auto lifecycle = m_lifecycle.load(std::memory_order_acquire);
    AssertCanStep(lifecycle);
    if (!CanStep(lifecycle)) {
      {
        std::scoped_lock lock(m_state->status_mutex);
        MarkPhysicsError(m_state->status,
                         PhysicsLifecycleState::Failed,
                         lifecycle == PhysicsLifecycleState::Constructed
                             ? "not_synced"
                             : "illegal_state");
      }
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    const auto flow_id = current_flow_id();
    {
      std::scoped_lock lock(m_state->status_mutex);
      m_state->status.fixed_dt_ms = static_cast<double>(config.fixed_dt) * 1000.0;
      m_state->status.current_flow_id = flow_id;
    }
    if (!std::isfinite(config.fixed_dt) || config.fixed_dt <= 0.0f || config.collision_steps <= 0) {
      {
        std::scoped_lock lock(m_state->status_mutex);
        MarkPhysicsError(m_state->status, PhysicsLifecycleState::Failed, "invalid_argument");
      }
      LogPhysicsStepFailed(flow_id, "invalid_argument", m_latestSummary.backend_bodies);
      m_lifecycle.store(PhysicsLifecycleState::Failed, std::memory_order_release);
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    const auto request_id = next_request_id();
    if (!enqueue_command(PhysicsCmd{PhysicsStepFixedCmd{request_id, config, flow_id}})) {
      {
        std::scoped_lock lock(m_state->status_mutex);
        MarkPhysicsError(m_state->status, PhysicsLifecycleState::Failed, "internal");
      }
      LogPhysicsStepFailed(flow_id, "internal", m_latestSummary.backend_bodies);
      m_lifecycle.store(PhysicsLifecycleState::Failed, std::memory_order_release);
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::Internal);
    }
    m_state->submitted_steps.fetch_add(1u, std::memory_order_release);
    m_lifecycle.store(PhysicsLifecycleState::Stepping, std::memory_order_release);
    {
      std::scoped_lock lock(m_state->status_mutex);
      m_state->status.lifecycle = ToComponentLifecycle(PhysicsLifecycleState::Stepping);
    }
    auto stats = MakeStepStats(0u,
                               static_cast<std::uint32_t>(std::max(1, config.collision_steps)),
                               0u,
                               m_latestSummary);
    return vkpt::core::Result<PhysicsStepStats>::ok(stats);
  }

  PhysicsStepSnapshot step_snapshot() const override {
    auto latest_generation = [&]() {
      std::scoped_lock lock(m_state->status_mutex);
      return m_state->latest_snapshot.generation;
    };
    auto ensure_pending_snapshot = [&](std::uint64_t request_id, std::uint64_t flow_id = 0u) {
      if (m_state->pending_snapshot.request_id != request_id) {
        m_state->pending_snapshot = {};
        m_state->pending_snapshot.request_id = request_id;
        m_state->pending_snapshot.flow_id = flow_id;
        m_state->pending_snapshot.body_counts = BodyCountsMap(m_latestSummary);
      } else if (m_state->pending_snapshot.flow_id == 0u) {
        m_state->pending_snapshot.flow_id = flow_id;
      }
    };
    auto drain = [&]() {
      PhysicsDelta delta;
      while (m_state->delta_ring.try_pop(delta)) {
        std::visit([&](auto&& payload) {
          using T = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<T, PhysicsTransformWrite>) {
            std::scoped_lock lock(m_state->status_mutex);
            ensure_pending_snapshot(payload.request_id);
            m_state->pending_snapshot.transform_writes.push_back(std::move(payload));
          } else if constexpr (std::is_same_v<T, PhysicsContactEventDelta>) {
            std::scoped_lock lock(m_state->status_mutex);
            ensure_pending_snapshot(payload.request_id, payload.flow_id);
            m_state->pending_snapshot.contact_events.push_back(PhysicsContactEvent{
                payload.request_id,
                payload.flow_id,
                payload.entity_a,
                payload.entity_b});
          } else if constexpr (std::is_same_v<T, PhysicsSyncSummaryDelta>) {
            m_latestSummary = payload.summary;
          } else if constexpr (std::is_same_v<T, PhysicsStepCompletedDelta>) {
            m_state->completed_steps.fetch_add(1u, std::memory_order_release);
            {
              std::scoped_lock lock(m_state->status_mutex);
              ensure_pending_snapshot(payload.request_id, payload.flow_id);
              m_state->status.last_step_us = payload.step_us;
              m_state->status.last_contacts_per_step = payload.contact_count;
              m_state->status.current_flow_id = payload.flow_id;
              m_state->status.last_error = payload.ok ? std::string{} : ErrorCodeName(payload.error);
              if (payload.ok) {
                MarkPhysicsTick(m_state->status, PhysicsLifecycleState::Synced);
              } else {
                MarkPhysicsError(m_state->status,
                                 PhysicsLifecycleState::Failed,
                                 ErrorCodeName(payload.error));
              }
              auto stats = MakeStepStats(payload.step_us,
                                         payload.solver_iterations,
                                         payload.contact_count,
                                         m_latestSummary);
              if (payload.active_bodies != 0u) {
                stats.active_bodies = payload.active_bodies;
              }
              m_state->pending_snapshot.generation = ++m_state->step_generation;
              m_state->pending_snapshot.wall_time_ns = SteadyNowNs();
              m_state->pending_snapshot.request_id = payload.request_id;
              m_state->pending_snapshot.flow_id = payload.flow_id;
              m_state->pending_snapshot.complete = payload.ok;
              m_state->pending_snapshot.body_counts = BodyCountsMap(m_latestSummary);
              m_state->pending_snapshot.stats = stats;
              m_state->latest_snapshot = std::move(m_state->pending_snapshot);
              m_state->pending_snapshot = {};
            }
            m_lifecycle.store(payload.ok ? PhysicsLifecycleState::Synced
                                         : PhysicsLifecycleState::Failed,
                              std::memory_order_release);
            if (!payload.ok) {
              LogPhysicsStepFailed(payload.flow_id,
                                   ErrorCodeName(payload.error),
                                   m_latestSummary.backend_bodies);
              LogPhysicsException("step_fixed", "physics worker reported step failure");
            }
          }
        }, std::move(delta.payload));
      }
    };
    drain();
    if (latest_generation() == 0u &&
        m_state->completed_steps.load(std::memory_order_acquire) <
            m_state->submitted_steps.load(std::memory_order_acquire)) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
      do {
        std::this_thread::yield();
        drain();
      } while (latest_generation() == 0u &&
               m_state->completed_steps.load(std::memory_order_acquire) <
                   m_state->submitted_steps.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline);
    }
    std::scoped_lock lock(m_state->status_mutex);
    return m_state->latest_snapshot;
  }

  PhysicsStatus status() const override {
    std::scoped_lock lock(m_state->status_mutex);
    auto out = m_state->status;
    out.lifecycle = ToComponentLifecycle(m_lifecycle.load(std::memory_order_acquire));
    return out;
  }

 private:
  std::uint64_t current_flow_id() const noexcept {
    return CurrentFlowId(m_flowSource.load(std::memory_order_acquire));
  }

  std::uint64_t next_request_id() const {
    return m_nextRequestId.fetch_add(1u, std::memory_order_relaxed);
  }

  bool enqueue_command(PhysicsCmd cmd) const {
    const auto state = m_state;
    if (std::this_thread::get_id() == state->worker_id) {
      return false;
    }
    if (!state->cmd_ring.try_push(std::move(cmd))) {
      try {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Warning,
            "physics",
            "physics command dropped",
            {{"cmd_ring_depth", std::to_string(state->cmd_ring.depth())}});
      } catch (...) {
      }
      return false;
    }
    state->cv.notify_one();
    return true;
  }

  static void push_delta(const std::shared_ptr<WorkerState>& state, PhysicsDelta delta) {
    if (!state->delta_ring.try_push(std::move(delta))) {
      try {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Warning,
            "physics",
            "physics delta dropped",
            {{"delta_ring_depth", std::to_string(state->delta_ring.depth())}});
      } catch (...) {
      }
    }
  }

  static PhysicsRaycastResultDelta raycast_known_bodies(const WorkerState& state,
                                                        const PhysicsRaycastCmd& cmd) {
    PhysicsRaycastResultDelta result;
    result.request_id = cmd.request_id;
    const auto direction = FallbackNormalize(cmd.direction, {0.0f, -1.0f, 0.0f});
    const float max_distance = std::max(0.0f, cmd.max_distance);
    float best_t = max_distance;
    for (const auto& body : state.known_bodies) {
      if (!body.body.enabled) {
        continue;
      }
      const auto center = body.transform.translation;
      const float radius = FallbackBodyRadius(body);
      const auto oc = FallbackSub(cmd.origin, center);
      const float b = FallbackDot(oc, direction);
      const float c = FallbackDot(oc, oc) - radius * radius;
      const float discriminant = b * b - c;
      if (discriminant < 0.0f) {
        continue;
      }
      const float t = -b - std::sqrt(discriminant);
      if (t < 0.0f || t > best_t) {
        continue;
      }
      best_t = t;
      result.hit = true;
      result.entity = body.entity;
      result.distance = t;
      result.position = FallbackAdd(cmd.origin, FallbackMul(direction, t));
      result.normal = FallbackNormalize(FallbackSub(result.position, center));
    }
    return result;
  }

  static void sync_backend_from_known_bodies(const std::shared_ptr<WorkerState>& state,
                                             std::uint64_t request_id) {
    const auto summary = state->backend->sync_from_bodies(state->known_bodies, state->ecs_entities);
    {
      std::scoped_lock lock(state->status_mutex);
      state->status.backend = state->cached_info.engine_name;
      SetPhysicsStatusBodyCounts(state->status, summary);
    }
    push_delta(state, PhysicsDelta{PhysicsSyncSummaryDelta{request_id, summary}});
  }

  static void process_command(const std::shared_ptr<WorkerState>& state,
                              PhysicsCmd cmd,
                              std::deque<PendingStep>& pending_steps,
                              bool& shutdown) {
    std::visit([&](auto&& payload) {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, PhysicsSyncBodiesCmd>) {
        state->known_bodies = std::move(payload.bodies);
        state->ecs_entities = payload.ecs_entities;
        sync_backend_from_known_bodies(state, payload.request_id);
      } else if constexpr (std::is_same_v<T, PhysicsAddBodyCmd>) {
        auto existing = std::find_if(state->known_bodies.begin(),
                                     state->known_bodies.end(),
                                     [&](const PhysicsBodySync& body) {
                                       return body.entity == payload.body.entity;
                                     });
        if (existing == state->known_bodies.end()) {
          state->known_bodies.push_back(std::move(payload.body));
          state->ecs_entities = std::max(state->ecs_entities, state->known_bodies.size());
        } else {
          *existing = std::move(payload.body);
        }
        sync_backend_from_known_bodies(state, payload.request_id);
      } else if constexpr (std::is_same_v<T, PhysicsRemoveBodyCmd>) {
        state->known_bodies.erase(
            std::remove_if(state->known_bodies.begin(),
                           state->known_bodies.end(),
                           [&](const PhysicsBodySync& body) {
                             return body.entity == payload.entity;
                           }),
            state->known_bodies.end());
        sync_backend_from_known_bodies(state, payload.request_id);
      } else if constexpr (std::is_same_v<T, PhysicsSetKinematicCmd>) {
        for (auto& body : state->known_bodies) {
          if (body.entity != payload.entity) {
            continue;
          }
          body.body.dynamic = !payload.kinematic;
          body.body.body_type = payload.kinematic ? "kinematic" : "dynamic";
        }
        sync_backend_from_known_bodies(state, payload.request_id);
      } else if constexpr (std::is_same_v<T, PhysicsSetGravityCmd>) {
        state->gravity = payload.gravity;
        push_delta(state, PhysicsDelta{PhysicsStepCompletedDelta{payload.request_id}});
      } else if constexpr (std::is_same_v<T, PhysicsRaycastCmd>) {
        push_delta(state, PhysicsDelta{raycast_known_bodies(*state, payload)});
      } else if constexpr (std::is_same_v<T, PhysicsStepFixedCmd>) {
        pending_steps.push_back(
            PendingStep{payload.config, payload.config.fixed_dt, payload.request_id, payload.flow_id});
      } else if constexpr (std::is_same_v<T, PhysicsShutdownCmd>) {
        shutdown = true;
      } else if constexpr (std::is_same_v<T, PhysicsApplyForceCmd>) {
        // The backend interface does not expose force application yet. The
        // command is still accepted by the channel so callers can migrate to
        // request-id flow without blocking RPCs.
        push_delta(state, PhysicsDelta{PhysicsStepCompletedDelta{payload.request_id}});
      }
    }, std::move(cmd.payload));
  }

  static void run_pending_substeps(const std::shared_ptr<WorkerState>& state,
                                   std::deque<PendingStep>& pending_steps) {
    constexpr float kTargetSubstepDt = 1.0f / 240.0f;
    while (!pending_steps.empty()) {
      auto& pending = pending_steps.front();
      if (!std::isfinite(pending.remaining_dt) || pending.remaining_dt <= 0.0f) {
        push_delta(state,
                   PhysicsDelta{PhysicsStepCompletedDelta{
                       pending.request_id,
                       true,
                       vkpt::core::ErrorCode::Ok,
                       pending.step_us,
                       pending.contact_count,
                       pending.flow_id,
                       pending.solver_iterations,
                       state->known_bodies.size()}});
        pending_steps.pop_front();
        continue;
      }
      PhysicsStepConfig substep = pending.config;
      substep.fixed_dt = pending.config.deterministic
                             ? pending.remaining_dt
                             : std::min(kTargetSubstepDt, pending.remaining_dt);
      const auto result = state->backend->step_fixed(substep);
      const auto backend_status = state->backend->status();
      auto update_worker_status = [&](bool ok) {
        std::scoped_lock lock(state->status_mutex);
        state->status.fixed_dt_ms = static_cast<double>(pending.config.fixed_dt) * 1000.0;
        state->status.last_step_us = pending.step_us;
        state->status.last_contacts_per_step = pending.contact_count;
        state->status.current_flow_id = pending.flow_id;
        if (ok) {
          state->status.last_error.clear();
        } else if (!backend_status.last_error.empty()) {
          state->status.last_error = backend_status.last_error;
        }
      };
      if (result) {
        pending.step_us += result.value().step_us;
        pending.contact_count += result.value().contact_count;
        pending.solver_iterations += result.value().solver_iterations;
        const auto backend_snapshot = state->backend->step_snapshot();
        for (auto write : backend_snapshot.transform_writes) {
          write.request_id = pending.request_id;
          push_delta(state, PhysicsDelta{std::move(write)});
        }
        pending.remaining_dt -= substep.fixed_dt;
        update_worker_status(true);
        if (pending.remaining_dt <= 0.000001f) {
          push_delta(state,
                     PhysicsDelta{PhysicsStepCompletedDelta{
                         pending.request_id,
                         true,
                         vkpt::core::ErrorCode::Ok,
                         pending.step_us,
                         pending.contact_count,
                         pending.flow_id,
                         pending.solver_iterations,
                         state->known_bodies.size()}});
          pending_steps.pop_front();
        }
      } else {
        pending.step_us += backend_status.last_step_us;
        pending.contact_count += backend_status.last_contacts_per_step;
        update_worker_status(false);
        LogPhysicsStepFailed(pending.flow_id,
                             ErrorCodeName(result.error()),
                             backend_status.body_counts.contains("backend")
                                 ? backend_status.body_counts.at("backend")
                                 : state->known_bodies.size());
        push_delta(state,
                   PhysicsDelta{PhysicsStepCompletedDelta{
                       pending.request_id,
                       false,
                       result.error(),
                       pending.step_us,
                       pending.contact_count,
                       pending.flow_id,
                       pending.solver_iterations,
                       state->known_bodies.size()}});
        pending_steps.pop_front();
      }
    }
  }

  static void worker_loop(const std::shared_ptr<WorkerState>& state) {
    {
      std::lock_guard lock(state->mutex);
      state->worker_id = std::this_thread::get_id();
      state->started = true;
    }
    state->cv.notify_all();

    std::deque<PendingStep> pending_steps;
    for (;;) {
      PhysicsCmd cmd;
      {
        std::unique_lock lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::milliseconds(1), [state]() {
          return state->stopping || state->cmd_ring.depth() > 0u;
        });
        if (state->stopping && state->cmd_ring.depth() == 0u && pending_steps.empty()) {
          state->backend.reset();
          return;
        }
        if (!state->cmd_ring.try_pop(cmd)) {
          run_pending_substeps(state, pending_steps);
          continue;
        }
      }
      try {
        bool shutdown = false;
        process_command(state, std::move(cmd), pending_steps, shutdown);
        PhysicsCmd next_cmd;
        while (!shutdown && state->cmd_ring.try_pop(next_cmd)) {
          process_command(state, std::move(next_cmd), pending_steps, shutdown);
        }
        run_pending_substeps(state, pending_steps);
        if (shutdown) {
          state->backend.reset();
          return;
        }
      } catch (const std::exception& ex) {
        LogPhysicsException("worker_loop_job", ex.what());
      } catch (...) {
        LogPhysicsException("worker_loop_job");
      }
    }
  }

  std::shared_ptr<WorkerState> m_state;
  std::thread m_thread;
  mutable PhysicsSyncSummary m_latestSummary;
  mutable std::atomic<std::uint64_t> m_nextRequestId{1u};
  mutable std::atomic<PhysicsLifecycleState> m_lifecycle{PhysicsLifecycleState::Constructed};
  std::atomic<const vkpt::core::contracts::IFlowSource*> m_flowSource{nullptr};
};

#ifdef PT_ENABLE_JOLT

namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kDynamic = 1;
constexpr JPH::ObjectLayer kCount = 2;
}  // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kDynamic(1);
constexpr JPH::uint kCount = 2;
}  // namespace BroadPhaseLayers

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
 public:
  BroadPhaseLayerInterfaceImpl() {
    m_layers[Layers::kStatic] = BroadPhaseLayers::kStatic;
    m_layers[Layers::kDynamic] = BroadPhaseLayers::kDynamic;
  }

  JPH::uint GetNumBroadPhaseLayers() const override {
    return BroadPhaseLayers::kCount;
  }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return m_layers[layer];
  }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
      case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::kStatic):
        return "static";
      case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::kDynamic):
        return "dynamic";
      default:
        return "unknown";
    }
  }
#endif

 private:
  JPH::BroadPhaseLayer m_layers[Layers::kCount];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase_layer) const override {
    if (layer == Layers::kStatic) {
      return broad_phase_layer == BroadPhaseLayers::kDynamic;
    }
    if (layer == Layers::kDynamic) {
      return true;
    }
    return false;
  }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer lhs, JPH::ObjectLayer rhs) const override {
    if (lhs == Layers::kStatic && rhs == Layers::kStatic) {
      return false;
    }
    return lhs < Layers::kCount && rhs < Layers::kCount;
  }
};

struct JoltRuntime {
  JoltRuntime() {
    JPH::RegisterDefaultAllocator();
    if (JPH::Factory::sInstance == nullptr) {
      JPH::Factory::sInstance = new JPH::Factory();
    }
    JPH::RegisterTypes();
  }

  ~JoltRuntime() {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
};

void EnsureJoltRuntime() {
  static JoltRuntime runtime;
  (void)runtime;
}

float PositiveScale(float value) {
  return std::max(std::abs(value), 0.1f);
}

JPH::Quat ToJoltQuat(const vkpt::scene::Quat& value) {
  return JPH::Quat(value.x, value.y, value.z, value.w);
}

vkpt::scene::Quat FromJoltQuat(JPH::QuatArg value) {
  return vkpt::scene::Quat{value.GetX(), value.GetY(), value.GetZ(), value.GetW()};
}

bool NearlyEqual(float lhs, float rhs) {
  return std::abs(lhs - rhs) <= 0.0001f;
}

bool NearlyEqual(const vkpt::scene::Vec3& lhs, const vkpt::scene::Vec3& rhs) {
  return NearlyEqual(lhs.x, rhs.x) && NearlyEqual(lhs.y, rhs.y) && NearlyEqual(lhs.z, rhs.z);
}

bool NearlyEqual(const vkpt::scene::Quat& lhs, const vkpt::scene::Quat& rhs, float epsilon = 0.0005f) {
  const float dot = lhs.x * rhs.x +
                    lhs.y * rhs.y +
                    lhs.z * rhs.z +
                    lhs.w * rhs.w;
  return std::abs(1.0f - std::abs(dot)) <= epsilon;
}

bool NearlyEqual(JPH::RVec3Arg lhs, const vkpt::scene::Vec3& rhs, float epsilon = 0.0005f) {
  return std::abs(static_cast<float>(lhs.GetX()) - rhs.x) <= epsilon &&
         std::abs(static_cast<float>(lhs.GetY()) - rhs.y) <= epsilon &&
         std::abs(static_cast<float>(lhs.GetZ()) - rhs.z) <= epsilon;
}

bool NearlyEqual(JPH::QuatArg lhs, const vkpt::scene::Quat& rhs, float epsilon = 0.0005f) {
  const float dot = lhs.GetX() * rhs.x +
                    lhs.GetY() * rhs.y +
                    lhs.GetZ() * rhs.z +
                    lhs.GetW() * rhs.w;
  return std::abs(1.0f - std::abs(dot)) <= epsilon;
}

struct BodyRuntimeKey {
  bool dynamic = false;
  bool kinematic = false;
  bool trigger = false;
  bool allow_sleeping = true;
  bool continuous_collision = false;
  std::string shape = "box";
  float mass = 1.0f;
  float friction = 0.5f;
  float restitution = 0.0f;
  float gravity_scale = 1.0f;
  vkpt::scene::Vec3 scale{1.0f, 1.0f, 1.0f};
};

bool SameBodyRuntimeKey(const BodyRuntimeKey& lhs, const BodyRuntimeKey& rhs) {
  return lhs.dynamic == rhs.dynamic &&
         lhs.kinematic == rhs.kinematic &&
         lhs.trigger == rhs.trigger &&
         lhs.allow_sleeping == rhs.allow_sleeping &&
         lhs.continuous_collision == rhs.continuous_collision &&
         lhs.shape == rhs.shape &&
         NearlyEqual(lhs.mass, rhs.mass) &&
         NearlyEqual(lhs.friction, rhs.friction) &&
         NearlyEqual(lhs.restitution, rhs.restitution) &&
         NearlyEqual(lhs.gravity_scale, rhs.gravity_scale) &&
         NearlyEqual(lhs.scale, rhs.scale);
}

BodyRuntimeKey MakeBodyRuntimeKey(const PhysicsBodySync& sync) {
  BodyRuntimeKey key;
  key.dynamic = sync.body.dynamic;
  key.kinematic = !sync.body.dynamic && sync.body.body_type == "kinematic";
  key.trigger = sync.body.trigger;
  key.allow_sleeping = sync.body.allow_sleeping;
  key.continuous_collision = sync.body.continuous_collision;
  key.shape = sync.body.shape.empty() ? "box" : sync.body.shape;
  key.mass = sync.body.mass;
  key.friction = sync.body.friction;
  key.restitution = sync.body.restitution;
  key.gravity_scale = sync.body.gravity_scale;
  key.scale = sync.transform.scale;
  return key;
}

std::optional<JPH::ShapeRefC> CreateJoltShape(const BodyRuntimeKey& key) {
  const auto sx = PositiveScale(key.scale.x);
  const auto sy = PositiveScale(key.scale.y);
  const auto sz = PositiveScale(key.scale.z);

  if (key.shape == "sphere") {
    JPH::SphereShapeSettings settings(std::max({sx, sy, sz}));
    const auto result = settings.Create();
    if (!result.HasError()) {
      return result.Get();
    }
  } else if (key.shape == "capsule") {
    const float radius = std::max(0.05f, std::min(sx, sz) * 0.5f);
    const float half_height = std::max(0.05f, (sy * 0.5f) - radius);
    JPH::CapsuleShapeSettings settings(half_height, radius);
    const auto result = settings.Create();
    if (!result.HasError()) {
      return result.Get();
    }
  } else if (key.shape == "cylinder") {
    const float radius = std::max(0.05f, std::max(sx, sz) * 0.5f);
    const float half_height = std::max(0.05f, sy * 0.5f);
    JPH::CylinderShapeSettings settings(half_height, radius);
    const auto result = settings.Create();
    if (!result.HasError()) {
      return result.Get();
    }
  }

  JPH::BoxShapeSettings settings(JPH::Vec3(sx * 0.5f, sy * 0.5f, sz * 0.5f));
  const auto result = settings.Create();
  if (result.HasError()) {
    return std::nullopt;
  }
  return result.Get();
}

JPH::uint ConservativeJoltWorkerCount() {
  const auto hardware = std::max(1u, std::thread::hardware_concurrency());
  if (hardware <= 4u) {
    return 1u;
  }
  return std::min(2u, std::max(1u, hardware / 4u));
}

class JoltPhysicsWorld final : public IPhysicsWorld {
 public:
  JoltPhysicsWorld() {
    EnsureJoltRuntime();
    m_backendWorkerThreads = ConservativeJoltWorkerCount();
    m_temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(16u * 1024u * 1024u);
    m_job_system = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        static_cast<int>(m_backendWorkerThreads));
    m_system.Init(65536u, 0u, 65536u, 10240u, m_broad_phase_layers, m_object_vs_broad_phase_filter,
                  m_object_layer_pair_filter);
  }

  ~JoltPhysicsWorld() override {
    destroy_all_bodies();
  }

  PhysicsEngineInfo engine_info() const override {
    auto info = GetCompiledPhysicsEngineInfo();
    info.backend_worker_threads = m_backendWorkerThreads;
    return info;
  }

  void set_flow_source(const vkpt::core::contracts::IFlowSource* flow_source) override {
    m_flowSource = flow_source;
  }

  PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) override {
    return sync_from_bodies(BuildPhysicsBodySyncList(world), world.all_entities().size());
  }

  PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies_to_sync, std::size_t ecs_entities) override {
    auto summary = BuildSyncSummary(bodies_to_sync, ecs_entities);
    m_writes.clear();
    auto& bodies = m_system.GetBodyInterface();
    std::unordered_set<vkpt::core::StableEntityId> seen;
    seen.reserve(bodies_to_sync.size());

    for (const auto& sync : bodies_to_sync) {
      seen.insert(sync.entity);
      if (!sync.body.enabled) {
        destroy_body(sync.entity);
        continue;
      }

      const auto key = MakeBodyRuntimeKey(sync);
      if (const auto existing = m_bodies.find(sync.entity);
          existing != m_bodies.end() && SameBodyRuntimeKey(existing->second.key, key)) {
        const JPH::RVec3 target_position(sync.transform.translation.x,
                                         sync.transform.translation.y,
                                         sync.transform.translation.z);
        const JPH::Quat target_rotation = ToJoltQuat(sync.transform.rotation);
        JPH::RVec3 current_position;
        JPH::Quat current_rotation;
        bodies.GetPositionAndRotation(existing->second.body_id, current_position, current_rotation);
        const bool pose_changed =
            !NearlyEqual(current_position, sync.transform.translation) ||
            !NearlyEqual(current_rotation, sync.transform.rotation);
        if (key.kinematic && pose_changed) {
          bodies.MoveKinematic(existing->second.body_id,
                               target_position,
                               target_rotation,
                               1.0f / 60.0f);
          bodies.ActivateBody(existing->second.body_id);
          existing->second.last_published_transform = sync.transform;
          continue;
        }
        const auto activation =
            key.dynamic && pose_changed ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        bodies.SetPositionAndRotationWhenChanged(existing->second.body_id,
                                                 target_position,
                                                 target_rotation,
                                                 activation);
        if (key.dynamic && pose_changed) {
          bodies.SetLinearAndAngularVelocity(existing->second.body_id,
                                             JPH::Vec3::sZero(),
                                             JPH::Vec3::sZero());
          bodies.ResetSleepTimer(existing->second.body_id);
          bodies.ActivateBody(existing->second.body_id);
        }
        if (pose_changed) {
          existing->second.last_published_transform = sync.transform;
        }
        continue;
      }

      destroy_body(sync.entity);
      const auto shape = CreateJoltShape(key);
      if (!shape.has_value()) {
        continue;
      }

      const auto motion_type = sync.body.dynamic
          ? JPH::EMotionType::Dynamic
          : (key.kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static);
      const auto layer = (sync.body.dynamic || key.kinematic) ? Layers::kDynamic : Layers::kStatic;
      JPH::BodyCreationSettings settings(
          *shape,
          JPH::RVec3(sync.transform.translation.x, sync.transform.translation.y, sync.transform.translation.z),
          ToJoltQuat(sync.transform.rotation),
          motion_type,
          layer);
      settings.mUserData = static_cast<JPH::uint64>(sync.entity);
      settings.mIsSensor = sync.body.trigger;
      settings.mFriction = sync.body.friction;
      settings.mRestitution = sync.body.restitution;
      settings.mGravityFactor = sync.body.gravity_scale;
      settings.mAllowSleeping = sync.body.allow_sleeping;
      settings.mMotionQuality = sync.body.continuous_collision ? JPH::EMotionQuality::LinearCast
                                                               : JPH::EMotionQuality::Discrete;
      if (sync.body.dynamic && sync.body.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = sync.body.mass;
      }

      const auto activation = (sync.body.dynamic || key.kinematic)
          ? JPH::EActivation::Activate
          : JPH::EActivation::DontActivate;
      const auto body_id = bodies.CreateAndAddBody(settings, activation);
      if (!body_id.IsInvalid()) {
        m_bodies.emplace(sync.entity, BodyRecord{body_id, key, {}, sync.transform});
      }
    }

    std::vector<vkpt::core::StableEntityId> stale;
    for (const auto& [entity_id, _] : m_bodies) {
      if (!seen.contains(entity_id)) {
        stale.push_back(entity_id);
      }
    }
    for (const auto entity_id : stale) {
      destroy_body(entity_id);
    }

    summary.backend_bodies = m_bodies.size();
    m_summary = summary;
    m_status.backend = engine_info().engine_name;
    m_status.current_flow_id = CurrentFlowId(m_flowSource);
    SetPhysicsStatusBodyCounts(m_status, m_summary);
    m_status.last_error.clear();
    MarkPhysicsTick(m_status, PhysicsLifecycleState::Synced);
    RecordPhysicsSyncTelemetry(m_summary);
    m_lifecycle = PhysicsLifecycleState::Synced;
    return m_summary;
  }

  vkpt::core::Result<PhysicsStepStats> step_fixed(const PhysicsStepConfig& config) override {
    AssertCanStep(m_lifecycle);
    if (!CanStep(m_lifecycle)) {
      MarkPhysicsError(m_status, PhysicsLifecycleState::Failed, "not_synced");
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    m_lifecycle = PhysicsLifecycleState::Stepping;
    const auto step_start = std::chrono::steady_clock::now();
    m_status.fixed_dt_ms = static_cast<double>(config.fixed_dt) * 1000.0;
    m_status.current_flow_id = CurrentFlowId(m_flowSource);
    const auto request_id = m_nextRequestId++;
    m_writes.clear();
    if (!std::isfinite(config.fixed_dt) || config.fixed_dt <= 0.0f || config.collision_steps <= 0) {
      MarkPhysicsError(m_status, PhysicsLifecycleState::Failed, "invalid_argument");
      LogPhysicsStepFailed(m_status.current_flow_id, m_status.last_error, m_summary.backend_bodies);
      m_lifecycle = PhysicsLifecycleState::Failed;
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    if (!config.collision_detection_enabled) {
      step_without_collision_detection(config.fixed_dt);
      collect_changed_transform_writes(request_id);
      const auto step_end = std::chrono::steady_clock::now();
      m_status.last_step_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(step_end - step_start).count());
      m_status.last_contacts_per_step = 0u;
      m_status.last_error.clear();
      RecordPhysicsStepTelemetry(m_status.last_step_us, 0u);
      const auto stats = MakeStepStats(m_status.last_step_us,
                                       static_cast<std::uint32_t>(std::max(1, config.collision_steps)),
                                       0u,
                                       m_summary);
      m_lastStepSnapshot = MakeStepSnapshot(++m_stepGeneration,
                                            request_id,
                                            m_status.current_flow_id,
                                            m_summary,
                                            stats,
                                            m_writes);
      m_lifecycle = PhysicsLifecycleState::Synced;
      MarkPhysicsTick(m_status, m_lifecycle);
      return vkpt::core::Result<PhysicsStepStats>::ok(stats);
    }
    const auto error = m_system.Update(config.fixed_dt, config.collision_steps, m_temp_allocator.get(), m_job_system.get());
    if (error != JPH::EPhysicsUpdateError::None) {
      MarkPhysicsError(m_status, PhysicsLifecycleState::Failed, "internal");
      LogPhysicsStepFailed(m_status.current_flow_id, m_status.last_error, m_summary.backend_bodies);
      m_lifecycle = PhysicsLifecycleState::Failed;
      return vkpt::core::Result<PhysicsStepStats>::error(vkpt::core::ErrorCode::Internal);
    }
    collect_changed_transform_writes(request_id);
    const auto step_end = std::chrono::steady_clock::now();
    m_status.last_step_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(step_end - step_start).count());
    m_status.last_contacts_per_step = 0u;
    m_status.last_error.clear();
    RecordPhysicsStepTelemetry(m_status.last_step_us, 0u);
    const auto stats = MakeStepStats(m_status.last_step_us,
                                     static_cast<std::uint32_t>(std::max(1, config.collision_steps)),
                                     0u,
                                     m_summary);
    m_lastStepSnapshot = MakeStepSnapshot(++m_stepGeneration,
                                          request_id,
                                          m_status.current_flow_id,
                                          m_summary,
                                          stats,
                                          m_writes);
    m_lifecycle = PhysicsLifecycleState::Synced;
    MarkPhysicsTick(m_status, m_lifecycle);
    return vkpt::core::Result<PhysicsStepStats>::ok(stats);
  }

  PhysicsStepSnapshot step_snapshot() const override {
    return m_lastStepSnapshot;
  }

  PhysicsStatus status() const override {
    return m_status;
  }

 private:
  struct BodyRecord {
    JPH::BodyID body_id;
    BodyRuntimeKey key;
    vkpt::scene::Vec3 velocity{};
    vkpt::scene::TransformComponent last_published_transform;
  };

  void collect_changed_transform_writes(std::uint64_t request_id) {
    m_writes.clear();
    const auto& lock_interface = m_system.GetBodyLockInterface();
    for (auto& [entity_id, record] : m_bodies) {
      if (!record.key.dynamic && !record.key.kinematic) {
        continue;
      }
      JPH::BodyLockRead lock(lock_interface, record.body_id);
      if (!lock.Succeeded()) {
        continue;
      }
      const auto& body = lock.GetBody();
      const auto position = body.GetPosition();
      const auto rotation = body.GetRotation();

      vkpt::scene::TransformComponent transform;
      transform.translation = {static_cast<float>(position.GetX()),
                               static_cast<float>(position.GetY()),
                               static_cast<float>(position.GetZ())};
      transform.rotation = FromJoltQuat(rotation);
      transform.scale = record.key.scale;
      transform.dirty = true;
      if (NearlyEqual(record.last_published_transform.translation,
                      transform.translation) &&
          NearlyEqual(record.last_published_transform.rotation,
                      transform.rotation) &&
          NearlyEqual(record.last_published_transform.scale,
                      transform.scale)) {
        continue;
      }
      record.last_published_transform = transform;
      m_writes.push_back({entity_id, transform, request_id});
    }
  }

  void step_without_collision_detection(float dt) {
    auto& body_interface = m_system.GetBodyInterface();
    const auto& lock_interface = m_system.GetBodyLockInterface();
    for (auto& [_, record] : m_bodies) {
      if (!record.key.dynamic) {
        continue;
      }

      JPH::RVec3 position;
      JPH::Quat rotation;
      {
        JPH::BodyLockRead lock(lock_interface, record.body_id);
        if (!lock.Succeeded()) {
          continue;
        }
        const auto& body = lock.GetBody();
        position = body.GetPosition();
        rotation = body.GetRotation();
      }

      record.velocity.y -= 9.81f * record.key.gravity_scale * dt;
      const JPH::RVec3 next_position(
          position.GetX() + record.velocity.x * dt,
          position.GetY() + record.velocity.y * dt,
          position.GetZ() + record.velocity.z * dt);
      body_interface.SetPositionAndRotation(
          record.body_id,
          next_position,
          rotation,
          JPH::EActivation::Activate);
    }
  }

  void destroy_body(vkpt::core::StableEntityId entity_id) {
    const auto it = m_bodies.find(entity_id);
    if (it == m_bodies.end()) {
      return;
    }
    auto& bodies = m_system.GetBodyInterface();
    if (!it->second.body_id.IsInvalid()) {
      bodies.RemoveBody(it->second.body_id);
      bodies.DestroyBody(it->second.body_id);
    }
    m_bodies.erase(it);
  }

  void destroy_all_bodies() {
    auto& bodies = m_system.GetBodyInterface();
    for (const auto& [_, record] : m_bodies) {
      if (!record.body_id.IsInvalid()) {
        bodies.RemoveBody(record.body_id);
        bodies.DestroyBody(record.body_id);
      }
    }
    m_bodies.clear();
  }

  BroadPhaseLayerInterfaceImpl m_broad_phase_layers;
  ObjectVsBroadPhaseLayerFilterImpl m_object_vs_broad_phase_filter;
  ObjectLayerPairFilterImpl m_object_layer_pair_filter;
  JPH::PhysicsSystem m_system;
  std::unique_ptr<JPH::TempAllocatorImpl> m_temp_allocator;
  std::unique_ptr<JPH::JobSystemThreadPool> m_job_system;
  JPH::uint m_backendWorkerThreads = 1;
  std::unordered_map<vkpt::core::StableEntityId, BodyRecord> m_bodies;
  std::vector<PhysicsTransformWrite> m_writes;
  PhysicsSyncSummary m_summary;
  PhysicsStatus m_status;
  PhysicsStepSnapshot m_lastStepSnapshot;
  const vkpt::core::contracts::IFlowSource* m_flowSource = nullptr;
  std::uint64_t m_stepGeneration = 0u;
  std::uint64_t m_nextRequestId = 1u;
  PhysicsLifecycleState m_lifecycle = PhysicsLifecycleState::Constructed;
};

#endif

}  // namespace

PhysicsEngineInfo GetCompiledPhysicsEngineInfo() {
#ifdef PT_ENABLE_JOLT
  return PhysicsEngineInfo{true, true, false, 0u, "Jolt Physics", PT_JOLT_GIT_TAG_STRING, "caller"};
#else
  return PhysicsEngineInfo{true, false, false, 0u, "Basic Physics", "internal", "caller"};
#endif
}

std::unique_ptr<IPhysicsWorld> CreateBackendPhysicsWorld() {
#ifdef PT_ENABLE_JOLT
  return std::make_unique<JoltPhysicsWorld>();
#else
  return std::make_unique<NullPhysicsWorld>();
#endif
}

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld() {
  auto backend = CreateBackendPhysicsWorld();
  if (!backend) {
    // Returning null instead of constructing a Failed-state world keeps callers
    // (which already check for null in many sites) on a single error path.
    return nullptr;
  }
  return std::make_unique<ThreadedPhysicsWorld>(std::move(backend));
}

vkpt::core::Result<std::unique_ptr<IPhysicsWorld>> CreatePhysicsWorldResult() {
  auto backend = CreateBackendPhysicsWorld();
  if (!backend) {
    return vkpt::core::Result<std::unique_ptr<IPhysicsWorld>>::error(
        vkpt::core::ErrorCode::Internal);
  }
  std::unique_ptr<IPhysicsWorld> world =
      std::make_unique<ThreadedPhysicsWorld>(std::move(backend));
  return vkpt::core::Result<std::unique_ptr<IPhysicsWorld>>::ok(std::move(world));
}

vkpt::core::health::Report EvaluatePhysicsHealth(const PhysicsStatus& status) {
  using vkpt::core::contracts::ComponentLifecycle;
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;

  if (status.lifecycle == ComponentLifecycle::Failed) {
    return Report{Status::Failed,
                  status.last_error.empty() ? "physics world failed" : status.last_error};
  }
  if (status.lifecycle == ComponentLifecycle::ShuttingDown) {
    return Report{Status::Ok, "shutting_down"};
  }
  if (!status.last_error.empty()) {
    return Report{Status::Degraded, status.last_error};
  }
  if (status.backend.empty()) {
    return Report{Status::Degraded, "physics backend is not reported"};
  }
  return Report{Status::Ok, "ok"};
}

std::shared_ptr<vkpt::core::health::IHealthProbe> IPhysicsWorld::create_health_probe() const {
  return std::make_shared<vkpt::core::health::FunctionProbe>(
      std::string(kPhysicsSubsystemName),
      [this]() {
        return EvaluatePhysicsHealth(status());
      });
}

}  // namespace vkpt::physics
