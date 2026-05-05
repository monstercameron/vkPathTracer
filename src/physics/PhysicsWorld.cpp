#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

std::vector<PhysicsBodySync> BuildPhysicsBodySyncList(const vkpt::scene::SceneWorld& world) {
  std::vector<PhysicsBodySync> bodies;
  bodies.reserve(world.query(vkpt::scene::ComponentKind::PhysicsBody).size());
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
    return m_summary;
  }

  vkpt::core::Result<void> step_fixed(const PhysicsStepConfig& config) override {
    if (!std::isfinite(config.fixed_dt) || config.fixed_dt <= 0.0f || config.collision_steps <= 0) {
      return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
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
      m_writes.push_back({entity_id, record.sync.transform});
    }
    return vkpt::core::Result<void>::ok();
  }

  std::vector<PhysicsTransformWrite> extract_transform_writes() const override {
    return m_writes;
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
  std::unordered_map<vkpt::core::StableEntityId, BodyRecord> m_bodies;
  std::vector<PhysicsTransformWrite> m_writes;
};

class ThreadedPhysicsWorld final : public IPhysicsWorld {
 public:
  explicit ThreadedPhysicsWorld(std::unique_ptr<IPhysicsWorld> backend)
      : m_backend(std::move(backend)) {
    if (!m_backend) {
      throw std::invalid_argument("physics backend is null");
    }
    m_thread = std::thread([this]() {
      worker_loop();
    });
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this]() {
      return m_started;
    });
  }

  ~ThreadedPhysicsWorld() override {
    if (!m_thread.joinable()) {
      return;
    }
    if (std::this_thread::get_id() != m_worker_id) {
      run_on_worker([this]() {
        m_backend.reset();
      });
    } else {
      m_backend.reset();
      return;
    }
    {
      std::lock_guard lock(m_mutex);
      m_stopping = true;
    }
    m_cv.notify_one();
    m_thread.join();
  }

  PhysicsEngineInfo engine_info() const override {
    auto info = run_on_worker([this]() {
      return m_backend->engine_info();
    });
    info.runs_on_worker_thread = true;
    info.threading_model = "dedicated_worker";
    return info;
  }

  PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) override {
    auto bodies = BuildPhysicsBodySyncList(world);
    const auto ecs_entities = world.all_entities().size();
    return run_on_worker([this, bodies = std::move(bodies), ecs_entities]() mutable {
      return m_backend->sync_from_bodies(std::move(bodies), ecs_entities);
    });
  }

  PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies, std::size_t ecs_entities) override {
    return run_on_worker([this, bodies = std::move(bodies), ecs_entities]() mutable {
      return m_backend->sync_from_bodies(std::move(bodies), ecs_entities);
    });
  }

  vkpt::core::Result<void> step_fixed(const PhysicsStepConfig& config) override {
    return run_on_worker([this, config]() {
      return m_backend->step_fixed(config);
    });
  }

  std::vector<PhysicsTransformWrite> extract_transform_writes() const override {
    return run_on_worker([this]() {
      return m_backend->extract_transform_writes();
    });
  }

 private:
  template <typename Fn>
  auto run_on_worker(Fn&& fn) const -> std::invoke_result_t<Fn&> {
    using Result = std::invoke_result_t<Fn&>;
    if (std::this_thread::get_id() == m_worker_id) {
      return std::forward<Fn>(fn)();
    }

    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    {
      std::lock_guard lock(m_mutex);
      if (m_stopping) {
        throw std::runtime_error("physics worker is stopping");
      }
      m_jobs.emplace_back([task]() {
        (*task)();
      });
    }
    m_cv.notify_one();
    return future.get();
  }

  void worker_loop() {
    {
      std::lock_guard lock(m_mutex);
      m_worker_id = std::this_thread::get_id();
      m_started = true;
    }
    m_cv.notify_all();

    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() {
          return m_stopping || !m_jobs.empty();
        });
        if (m_stopping && m_jobs.empty()) {
          return;
        }
        job = std::move(m_jobs.front());
        m_jobs.pop_front();
      }
      job();
    }
  }

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_cv;
  mutable std::deque<std::function<void()>> m_jobs;
  mutable bool m_started = false;
  mutable bool m_stopping = false;
  std::thread::id m_worker_id;
  std::thread m_thread;
  std::unique_ptr<IPhysicsWorld> m_backend;
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

  PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) override {
    return sync_from_bodies(BuildPhysicsBodySyncList(world), world.all_entities().size());
  }

  PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies_to_sync, std::size_t ecs_entities) override {
    auto summary = BuildSyncSummary(bodies_to_sync, ecs_entities);
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
        continue;
      }

      destroy_body(sync.entity);
      const auto shape = CreateJoltShape(key);
      if (!shape.has_value()) {
        continue;
      }

      const auto motion_type = sync.body.dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
      const auto layer = sync.body.dynamic ? Layers::kDynamic : Layers::kStatic;
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

      const auto activation = sync.body.dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
      const auto body_id = bodies.CreateAndAddBody(settings, activation);
      if (!body_id.IsInvalid()) {
        m_bodies.emplace(sync.entity, BodyRecord{body_id, key});
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
    return m_summary;
  }

  vkpt::core::Result<void> step_fixed(const PhysicsStepConfig& config) override {
    if (!std::isfinite(config.fixed_dt) || config.fixed_dt <= 0.0f || config.collision_steps <= 0) {
      return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    if (!config.collision_detection_enabled) {
      step_without_collision_detection(config.fixed_dt);
      return vkpt::core::Result<void>::ok();
    }
    const auto error = m_system.Update(config.fixed_dt, config.collision_steps, m_temp_allocator.get(), m_job_system.get());
    if (error != JPH::EPhysicsUpdateError::None) {
      return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
    }
    return vkpt::core::Result<void>::ok();
  }

  std::vector<PhysicsTransformWrite> extract_transform_writes() const override {
    std::vector<PhysicsTransformWrite> writes;
    writes.reserve(m_bodies.size());
    const auto& lock_interface = m_system.GetBodyLockInterface();
    for (const auto& [entity_id, record] : m_bodies) {
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
      writes.push_back({entity_id, transform});
    }
    return writes;
  }

 private:
  struct BodyRecord {
    JPH::BodyID body_id;
    BodyRuntimeKey key;
    vkpt::scene::Vec3 velocity{};
  };

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
  PhysicsSyncSummary m_summary;
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
  return std::make_unique<ThreadedPhysicsWorld>(CreateBackendPhysicsWorld());
}

}  // namespace vkpt::physics
