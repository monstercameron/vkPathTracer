#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/contracts/IFlowSource.h"
#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/health/Health.h"
#include "core/Types.h"
#include "core/contracts/Result.h"
#include "scene/Scene.h"

namespace vkpt::physics {

inline constexpr std::string_view kPhysicsSubsystemName = "physics";
inline constexpr std::string_view kPhysicsStatusTypeName = "PhysicsStatus";
inline constexpr std::string_view kPhysicsStepSnapshotContractName =
    "physics.step_snapshot.v1";

struct PhysicsNamingContract {
  std::string_view subsystem_name = kPhysicsSubsystemName;
  std::string_view status_type_name = kPhysicsStatusTypeName;
  std::string_view step_snapshot_contract = kPhysicsStepSnapshotContractName;
  std::string_view health_probe_name = kPhysicsSubsystemName;
  std::string_view lifecycle_field_name = "lifecycle";
  std::string_view last_error_field_name = "last_error";
  std::string_view flow_field_name = "current_flow_id";
  std::string_view sequencing_field_name = "request_id";
};

inline constexpr PhysicsNamingContract kPhysicsNamingContract{};

[[nodiscard]] inline constexpr std::string_view PhysicsSubsystemName() noexcept {
  return kPhysicsSubsystemName;
}

struct PhysicsEngineInfo {
  bool available = false;
  bool compiled_with_jolt = false;
  bool runs_on_worker_thread = false;
  std::size_t backend_worker_threads = 0;
  std::string engine_name = "none";
  std::string engine_version = "disabled";
  std::string threading_model = "caller";
};

struct PhysicsStepConfig {
  float fixed_dt = 1.0f / 60.0f;
  int collision_steps = 1;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  bool collision_detection_enabled = true;

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("physics", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                              determinism_base_seed,
                                              determinism_frame_index,
                                              determinism_scenario_id);
  }
};

struct PhysicsSyncSummary {
  std::size_t ecs_entities = 0;
  std::size_t physics_components = 0;
  std::size_t enabled_bodies = 0;
  std::size_t disabled_bodies = 0;
  std::size_t dynamic_bodies = 0;
  std::size_t static_bodies = 0;
  std::size_t backend_bodies = 0;
};

struct PhysicsTransformWrite {
  // Canonical sequencing key for physics outputs. Writes produced by the same
  // accepted physics command carry the same non-zero request_id, and request_id
  // values are monotonically increasing for one IPhysicsWorld instance.
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::TransformComponent transform;
  std::uint64_t request_id = 0;
};

struct PhysicsBodySync {
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::PhysicsBodyComponent body;
  vkpt::scene::TransformComponent transform;
};

struct PhysicsStatus {
  std::string name = std::string(kPhysicsSubsystemName);
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  std::string backend;
  std::uint64_t last_tick_ns = 0;
  std::uint64_t ticks_total = 0;
  std::uint64_t errors_total = 0;
  double fixed_dt_ms = 0.0;
  std::unordered_map<std::string, std::size_t> body_counts;
  std::uint64_t last_step_us = 0;
  std::size_t last_contacts_per_step = 0;
  std::uint64_t current_flow_id = 0;
  std::string last_error;
};

using PhysicsWorldStatus = PhysicsStatus;

struct PhysicsStepStats {
  std::uint64_t step_us = 0;
  std::uint32_t solver_iterations = 0;
  std::size_t contact_count = 0;
  std::size_t active_bodies = 0;
};

struct PhysicsContactEvent {
  std::uint64_t request_id = 0;
  std::uint64_t flow_id = 0;
  vkpt::core::StableEntityId entity_a = 0;
  vkpt::core::StableEntityId entity_b = 0;
};

struct PhysicsStepSnapshot {
  std::uint64_t generation = 0;
  std::uint64_t wall_time_ns = 0;
  std::uint64_t request_id = 0;
  std::uint64_t flow_id = 0;
  bool complete = false;
  std::vector<PhysicsTransformWrite> transform_writes;
  std::vector<PhysicsContactEvent> contact_events;
  std::unordered_map<std::string, std::size_t> body_counts;
  PhysicsStepStats stats;
  // Phase 2 RAG02: per-entity joint-world matrices, parallel to that
  // entity's skeleton.joints. Empty for entities with no active ragdoll.
  // Additive — old consumers ignore this map.
  std::unordered_map<vkpt::core::StableEntityId, std::vector<vkpt::scene::Mat4>>
      joint_world_matrices;
};

using PhysicsWorldStepSnapshot = PhysicsStepSnapshot;

// IPhysicsWorld lifecycle contract:
//
// state\method       engine_info status set_flow_source sync_from_* step_fixed step_snapshot
// Constructed        ok          ok     ok              ->Synced    error      empty
// Synced             ok          ok     ok              ok          ok         latest
// Stepping           ok          ok     ok              ok          ok         latest-complete
// Failed             ok          ok     ok              ->Synced    error      latest
// ShuttingDown       ok          ok     noop            illegal     illegal    latest
//
// A sync call is required before the first step. State is retained after sync:
// callers do not need to sync before every step unless ECS physics bodies or
// authored transforms changed. Debug builds assert this ordering in each impl.
class IPhysicsWorld {
 public:
  virtual ~IPhysicsWorld() = default;

  virtual PhysicsEngineInfo engine_info() const = 0;
  virtual void set_flow_source(const vkpt::core::contracts::IFlowSource* flow_source) {
    (void)flow_source;
  }
  virtual PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) = 0;
  virtual PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies, std::size_t ecs_entities) = 0;
  virtual vkpt::core::Result<PhysicsStepStats> step_fixed(const PhysicsStepConfig& config) = 0;
  virtual PhysicsStepSnapshot step_snapshot() const = 0;
  virtual PhysicsStatus status() const = 0;
  virtual std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const;
};

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld();
vkpt::core::Result<std::unique_ptr<IPhysicsWorld>> CreatePhysicsWorldResult();
PhysicsEngineInfo GetCompiledPhysicsEngineInfo();
vkpt::core::health::Report EvaluatePhysicsHealth(const PhysicsStatus& status);

}  // namespace vkpt::physics
