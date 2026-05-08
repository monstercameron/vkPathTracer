#include "physics/PhysicsWorld.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "physics_contract_smoke: " << message << "\n";
    return false;
  }
  return true;
}

class TestFlowSource final : public vkpt::core::contracts::IFlowSource {
 public:
  explicit TestFlowSource(std::uint64_t flow_id) : flow_id_(flow_id) {}
  std::uint64_t current_flow_id() const noexcept override { return flow_id_; }

 private:
  std::uint64_t flow_id_ = 0u;
};

vkpt::scene::SceneWorld BuildPhysicsProbeWorld() {
  vkpt::scene::SceneWorld world;

  const auto ground = world.create_entity("ground", 50100u);
  vkpt::scene::TransformComponent ground_transform;
  ground_transform.translation = {0.0f, 0.0f, 0.0f};
  ground_transform.scale = {10.0f, 0.1f, 10.0f};
  vkpt::scene::PhysicsBodyComponent ground_body;
  ground_body.enabled = true;
  ground_body.dynamic = false;
  ground_body.body_type = "static";
  ground_body.shape = "box";
  ground_body.mass = 1.0f;
  world.set_component(ground, vkpt::scene::ComponentKind::Transform, ground_transform);
  world.set_component(ground, vkpt::scene::ComponentKind::PhysicsBody, ground_body);

  const auto ball = world.create_entity("ball", 50101u);
  vkpt::scene::TransformComponent ball_transform;
  ball_transform.translation = {0.0f, 2.0f, 0.0f};
  ball_transform.scale = {0.5f, 0.5f, 0.5f};
  vkpt::scene::PhysicsBodyComponent ball_body;
  ball_body.enabled = true;
  ball_body.dynamic = true;
  ball_body.body_type = "dynamic";
  ball_body.shape = "sphere";
  ball_body.mass = 1.0f;
  ball_body.allow_sleeping = false;
  world.set_component(ball, vkpt::scene::ComponentKind::Transform, ball_transform);
  world.set_component(ball, vkpt::scene::ComponentKind::PhysicsBody, ball_body);

  world.recompute_world_transforms();
  return world;
}

vkpt::physics::PhysicsStepSnapshot WaitForSnapshot(vkpt::physics::IPhysicsWorld& physics,
                                                   std::uint64_t min_generation) {
  for (int spin = 0; spin < 200; ++spin) {
    auto snapshot = physics.step_snapshot();
    if (snapshot.complete && snapshot.generation > min_generation) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return physics.step_snapshot();
}

bool CheckPhysicsNamingContract() {
  static_assert(std::is_same_v<vkpt::physics::PhysicsWorldStatus,
                               vkpt::physics::PhysicsStatus>);
  static_assert(std::is_same_v<vkpt::physics::PhysicsWorldStepSnapshot,
                               vkpt::physics::PhysicsStepSnapshot>);

  vkpt::physics::PhysicsWorldStatus status;
  return Check(vkpt::physics::PhysicsSubsystemName() == "physics",
               "physics subsystem should expose a stable canonical name") &&
         Check(std::string_view(status.name) ==
                   vkpt::physics::kPhysicsSubsystemName,
               "physics status should carry the canonical subsystem name") &&
         Check(vkpt::physics::kPhysicsNamingContract.status_type_name ==
                   "PhysicsStatus",
               "physics naming contract should expose the status type name") &&
         Check(vkpt::physics::kPhysicsNamingContract.health_probe_name ==
                   vkpt::physics::kPhysicsSubsystemName,
               "physics health probe name should match the subsystem name") &&
         Check(vkpt::physics::kPhysicsNamingContract.sequencing_field_name ==
                   "request_id",
               "physics naming contract should pin request_id sequencing") &&
         Check(vkpt::physics::kPhysicsNamingContract.step_snapshot_contract ==
                   vkpt::physics::kPhysicsStepSnapshotContractName,
               "physics step snapshot contract should be source-proofable");
}

}  // namespace

int main() {
  using vkpt::core::contracts::ComponentLifecycle;
  using vkpt::core::health::Status;

  if (!CheckPhysicsNamingContract()) {
    return 1;
  }

  auto world = BuildPhysicsProbeWorld();
  auto physics = vkpt::physics::CreatePhysicsWorld();
  TestFlowSource flow_source(4242u);
  physics->set_flow_source(&flow_source);

  const auto probe = physics->create_health_probe();
  if (!Check(static_cast<bool>(probe), "physics health probe should be created") ||
      !Check(probe->name() == "physics", "physics health probe should be named physics") ||
      !Check(physics->status().lifecycle == ComponentLifecycle::Uninitialized,
             "physics status should start Uninitialized before sync")) {
    return 1;
  }

  const auto sync = physics->sync_from_scene_world(world);
  if (!Check(sync.enabled_bodies == 2u && sync.dynamic_bodies == 1u,
             "sync should expose enabled and dynamic physics bodies") ||
      !Check(physics->status().lifecycle == ComponentLifecycle::Ready,
             "physics status should become Ready after sync") ||
      !Check(probe->check().status == Status::Ok,
             "physics health probe should report ok after sync")) {
    return 1;
  }

  vkpt::physics::PhysicsStepConfig step;
  step.fixed_dt = 1.0f / 60.0f;
  step.collision_steps = 2;
  step.deterministic = true;

  const auto accepted = physics->step_fixed(step);
  if (!Check(static_cast<bool>(accepted), "valid step should be accepted") ||
      !Check(accepted.value().solver_iterations == 2u,
             "accepted step stats should echo requested solver iterations") ||
      !Check(accepted.value().active_bodies == 2u,
             "accepted step stats should expose active body count")) {
    return 1;
  }

  const auto first = WaitForSnapshot(*physics, 0u);
  const auto first_status = physics->status();
  if (!Check(first.complete, "step snapshot should complete") ||
      !Check(first.generation == 1u, "first completed step snapshot should be generation 1") ||
      !Check(first.request_id != 0u, "step snapshot should carry a non-zero request_id") ||
      !Check(first.flow_id == 4242u, "step snapshot should carry the flow id") ||
      !Check(first.stats.step_us > 0u, "step snapshot should include measured step_us") ||
      !Check(first.stats.solver_iterations >= 2u,
             "step snapshot should include solver iteration stats") ||
      !Check(first.stats.active_bodies == 2u,
             "step snapshot should include active body stats") ||
      !Check(first.body_counts.at("enabled") == 2u,
             "step snapshot should include body count map") ||
      !Check(!first.transform_writes.empty(),
             "step snapshot should include transform writes") ||
      !Check(first_status.lifecycle == ComponentLifecycle::Ready,
             "physics status should return to Ready after completed step") ||
      !Check(first_status.ticks_total >= 1u && first_status.last_tick_ns != 0u,
             "physics status should expose tick counters")) {
    return 1;
  }
  for (const auto& write : first.transform_writes) {
    if (!Check(write.request_id == first.request_id,
               "all transform writes should share the snapshot request_id")) {
      return 1;
    }
  }

  const auto second_accepted = physics->step_fixed(step);
  const auto second = WaitForSnapshot(*physics, first.generation);
  if (!Check(static_cast<bool>(second_accepted), "second step should be accepted") ||
      !Check(second.generation == first.generation + 1u,
             "step snapshot generations should increase monotonically") ||
      !Check(second.request_id > first.request_id,
             "step request_id values should increase monotonically")) {
    return 1;
  }

  vkpt::physics::PhysicsStepConfig invalid_step;
  invalid_step.fixed_dt = 0.0f;
  invalid_step.collision_steps = 1;
  const auto invalid = physics->step_fixed(invalid_step);
  const auto failed_status = physics->status();
  if (!Check(!static_cast<bool>(invalid), "invalid step should fail") ||
      !Check(failed_status.last_error == "invalid_argument",
             "invalid step should update PhysicsStatus last_error") ||
      !Check(failed_status.lifecycle == ComponentLifecycle::Failed,
             "invalid step should move PhysicsStatus lifecycle to Failed") ||
      !Check(failed_status.errors_total >= 1u,
             "invalid step should increment PhysicsStatus errors_total") ||
      !Check(probe->check().status == Status::Failed,
             "physics health probe should fail when the physics lifecycle fails")) {
    return 1;
  }

  return 0;
}
