#pragma once

// RAG-MAIN-LOOP: a self-contained ragdoll world that lives alongside the
// engine's ThreadedPhysicsWorld. We deliberately do NOT pierce the threaded
// physics worker — instead, RagdollWorld owns its own JPH::PhysicsSystem
// (or, when built without PT_ENABLE_JOLT, no-ops gracefully). This keeps
// the live qt main loop free of races while exposing the same set of
// operations the hero_hit_demo_smoke uses (build, seed_pose, apply_impulse,
// step, read joint matrices).
//
// The qt loop drives one tick per frame (after the main physics step) and
// queries read_joint_world_matrices() per active handle. Authority resolver
// (animation/AnimationAuthority.cpp) decides which side feeds
// EntityRecord::joint_world_matrices on a given frame.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "animation/Skeleton.h"
#include "core/Types.h"
#include "physics/Ragdoll.h"
#include "scene/SceneTypes.h"

namespace vkpt::physics {

// Opaque handle returned by attach_ragdoll(). Stable for the lifetime of
// the entity's ragdoll.
struct RagdollHandle {
  std::uint64_t id = 0u;
  bool valid() const noexcept { return id != 0u; }
  bool operator==(const RagdollHandle& other) const noexcept {
    return id == other.id;
  }
  bool operator!=(const RagdollHandle& other) const noexcept {
    return !(*this == other);
  }
};

class RagdollWorld {
 public:
  RagdollWorld();
  ~RagdollWorld();

  RagdollWorld(const RagdollWorld&) = delete;
  RagdollWorld& operator=(const RagdollWorld&) = delete;
  RagdollWorld(RagdollWorld&&) noexcept;
  RagdollWorld& operator=(RagdollWorld&&) noexcept;

  // Whether the underlying Jolt world is available. False on builds
  // compiled without PT_ENABLE_JOLT — all attach_/seed_/step_ methods
  // become no-ops in that case.
  bool is_available() const noexcept;

  // Build + add a ragdoll for `entity`. Returns an invalid handle on
  // failure. The entity may already have an attached ragdoll; callers
  // should detach() the old one first.
  RagdollHandle attach_ragdoll(vkpt::core::StableEntityId entity,
                               const vkpt::animation::Skeleton& skeleton,
                               const vkpt::scene::Mat4& spawn_world,
                               const RagdollConfig& config = {});

  // Detach + drop a previously attached ragdoll. Safe to call with an
  // invalid or already-detached handle.
  void detach_ragdoll(RagdollHandle handle);

  // Returns the latest per-joint world matrices snapshot. Output size
  // equals skeleton.joints.size() of the originally-attached skeleton, or
  // 0 if the handle is invalid.
  std::vector<vkpt::scene::Mat4> read_ragdoll_joint_matrices(
      RagdollHandle handle) const;

  // Seed an attached ragdoll with the supplied pose. Typically called once
  // when the RagdollComponent.active flag flips false->true so the bodies
  // inherit the animation pose + momentum.
  bool seed_ragdoll_pose(
      RagdollHandle handle,
      const std::vector<vkpt::scene::Mat4>& joint_world_matrices,
      const std::vector<vkpt::scene::Mat4>* prev_joint_world_matrices = nullptr,
      float dt = 1.0f / 60.0f);

  // Apply a world-space impulse to the body that owns `joint_index`.
  bool apply_ragdoll_impulse(RagdollHandle handle,
                             std::int32_t joint_index,
                             vkpt::scene::Vec3 impulse);

  // Step the underlying Jolt world by `dt`. The qt loop calls this once
  // per frame after the main physics step has completed. No-op when no
  // ragdolls are attached or PT_ENABLE_JOLT is off.
  void step(float dt);

  // Diagnostic accessors.
  std::size_t active_ragdoll_count() const noexcept;
  std::size_t total_steps() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace vkpt::physics
