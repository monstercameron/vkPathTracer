#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "scene/Scene.h"

namespace vkpt::physics {

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
  bool collision_detection_enabled = true;
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
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::TransformComponent transform;
};

struct PhysicsBodySync {
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::PhysicsBodyComponent body;
  vkpt::scene::TransformComponent transform;
};

class IPhysicsWorld {
 public:
  virtual ~IPhysicsWorld() = default;

  virtual PhysicsEngineInfo engine_info() const = 0;
  virtual PhysicsSyncSummary sync_from_scene_world(const vkpt::scene::SceneWorld& world) = 0;
  virtual PhysicsSyncSummary sync_from_bodies(std::vector<PhysicsBodySync> bodies, std::size_t ecs_entities) = 0;
  virtual vkpt::core::Result<void> step_fixed(const PhysicsStepConfig& config) = 0;
  virtual std::vector<PhysicsTransformWrite> extract_transform_writes() const = 0;
};

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld();
PhysicsEngineInfo GetCompiledPhysicsEngineInfo();

}  // namespace vkpt::physics
