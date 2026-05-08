#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/sync/SpscRing.h"
#include "physics/PhysicsWorld.h"
#include "scene/SceneTypes.h"

namespace vkpt::physics {

struct PhysicsApplyForceCmd {
  std::uint64_t request_id = 0;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 force{};
  vkpt::scene::Vec3 point{};
  bool impulse = false;
};

struct PhysicsSetKinematicCmd {
  std::uint64_t request_id = 0;
  vkpt::core::StableEntityId entity = 0;
  bool kinematic = false;
};

struct PhysicsRaycastCmd {
  std::uint64_t request_id = 0;
  vkpt::scene::Vec3 origin{};
  vkpt::scene::Vec3 direction{0.0f, -1.0f, 0.0f};
  float max_distance = 1000.0f;
};

struct PhysicsAddBodyCmd {
  std::uint64_t request_id = 0;
  PhysicsBodySync body;
};

struct PhysicsRemoveBodyCmd {
  std::uint64_t request_id = 0;
  vkpt::core::StableEntityId entity = 0;
};

struct PhysicsSetGravityCmd {
  std::uint64_t request_id = 0;
  vkpt::scene::Vec3 gravity{0.0f, -9.81f, 0.0f};
};

struct PhysicsSyncBodiesCmd {
  std::uint64_t request_id = 0;
  std::vector<PhysicsBodySync> bodies;
  std::size_t ecs_entities = 0u;
};

struct PhysicsStepFixedCmd {
  std::uint64_t request_id = 0;
  PhysicsStepConfig config;
  std::uint64_t flow_id = 0;
};

struct PhysicsShutdownCmd {
  std::uint64_t request_id = 0;
};

struct PhysicsCmd {
  using Payload = std::variant<
      PhysicsApplyForceCmd,
      PhysicsSetKinematicCmd,
      PhysicsRaycastCmd,
      PhysicsAddBodyCmd,
      PhysicsRemoveBodyCmd,
      PhysicsSetGravityCmd,
      PhysicsSyncBodiesCmd,
      PhysicsStepFixedCmd,
      PhysicsShutdownCmd>;

  Payload payload;
};

struct PhysicsContactEventDelta {
  std::uint64_t request_id = 0;
  std::uint64_t flow_id = 0;
  vkpt::core::StableEntityId entity_a = 0;
  vkpt::core::StableEntityId entity_b = 0;
};

struct PhysicsRaycastResultDelta {
  std::uint64_t request_id = 0;
  bool hit = false;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{};
  vkpt::scene::Vec3 normal{0.0f, 1.0f, 0.0f};
  float distance = 0.0f;
};

struct PhysicsSleepStateChangedDelta {
  std::uint64_t request_id = 0;
  vkpt::core::StableEntityId entity = 0;
  bool sleeping = false;
};

struct PhysicsStepCompletedDelta {
  std::uint64_t request_id = 0;
  bool ok = true;
  vkpt::core::ErrorCode error = vkpt::core::ErrorCode::Ok;
  std::uint64_t step_us = 0;
  std::size_t contact_count = 0;
  std::uint64_t flow_id = 0;
  std::uint32_t solver_iterations = 0;
  std::size_t active_bodies = 0;
};

struct PhysicsSyncSummaryDelta {
  std::uint64_t request_id = 0;
  PhysicsSyncSummary summary;
};

struct PhysicsDelta {
  using Payload = std::variant<
      PhysicsTransformWrite,
      PhysicsContactEventDelta,
      PhysicsRaycastResultDelta,
      PhysicsSleepStateChangedDelta,
      PhysicsStepCompletedDelta,
      PhysicsSyncSummaryDelta>;

  Payload payload;
};

using PhysicsCmdRing = vkpt::core::sync::SpscRing<PhysicsCmd>;
using PhysicsDeltaRing = vkpt::core::sync::SpscRing<PhysicsDelta>;

}  // namespace vkpt::physics
