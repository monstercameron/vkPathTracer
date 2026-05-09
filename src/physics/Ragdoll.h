#pragma once

// Phase 2 RAG01-04: minimal Jolt ragdoll bridge.
//
// Spec note: the original brief asked for a wrapper around JPH::Ragdoll +
// RagdollSettings. After surveying the Jolt 5.5 API surface we deliberately
// took the documented "hard fallback": build the ragdoll out of plain
// JPH::Body + per-joint TwoBodyConstraints (Hinge / SwingTwist / Fixed) and
// keep the higher-level JPH::Ragdoll class out of this translation unit.
// The contract Ragdoll::read_joint_world_matrices() (one transform per
// skeleton joint) still satisfies the demo, so the cut is invisible to the
// rest of the engine.

#include <cstddef>
#include <memory>
#include <vector>

#include "animation/Skeleton.h"
#include "scene/SceneTypes.h"

#ifdef PT_ENABLE_JOLT
namespace JPH {
class PhysicsSystem;
}
#endif

namespace vkpt::physics {

struct RagdollConfig {
  float capsule_radius_scale = 0.05f;  // fraction of bone length
  float spine_capsule_radius = 0.10f;  // larger; covers torso
  float head_capsule_radius = 0.12f;
  float density = 1000.0f;
  bool self_collision = false;  // off by default; on causes jitter
};

class Ragdoll {
 public:
  Ragdoll();
  ~Ragdoll();

  Ragdoll(const Ragdoll&) = delete;
  Ragdoll& operator=(const Ragdoll&) = delete;
  Ragdoll(Ragdoll&&) noexcept;
  Ragdoll& operator=(Ragdoll&&) noexcept;

  /// Build a ragdoll from a Skeleton. Capsule per bone (parent->child),
  /// constraints derived by bone name (case-insensitive substring match):
  ///   shoulder.{l,r}, thigh.{l,r}  -> SwingTwist (cone-twist) constraint
  ///   arm.{l,r}, leg.{l,r}         -> Hinge (single-axis bend)
  ///   head, body, hand.{l,r}, foot.{l,r} -> Fixed (no rotation; rigid attach)
  /// Returns false on Jolt API failure or if the skeleton fails validation.
  bool build(const vkpt::animation::Skeleton& skeleton,
             const vkpt::scene::Mat4& spawn_world_transform,
             const RagdollConfig& config = {});

#ifdef PT_ENABLE_JOLT
  /// Add the ragdoll's bodies + constraints to a Jolt world. Caller retains
  /// ownership of the world. add_to_world() can only be called once per
  /// world; calling it again is a no-op.
  void add_to_world(JPH::PhysicsSystem& world);
  void remove_from_world(JPH::PhysicsSystem& world);
#endif

  /// Read current per-joint world transforms. Output size equals
  /// skeleton.joints.size(). Joint i's transform is the body whose bone ends
  /// at joint i (i.e. the parent->joint capsule). Leaf joints inherit their
  /// parent body's transform plus the bind-relative offset.
  std::vector<vkpt::scene::Mat4> read_joint_world_matrices() const;

  /// Phase 5 RAG06: spawn ragdoll bodies at the supplied pose (typically the
  /// animation pose just before activation). Each Jolt body is teleported to
  /// align with the bone (parent->joint axis) defined by the supplied
  /// matrices. If `prev_joint_world_matrices` is non-null and its size matches
  /// the skeleton, linear+angular velocities are derived from the delta over
  /// `dt` and stamped on each body so the ragdoll inherits the animation's
  /// momentum. Returns false if the ragdoll isn't built / added to a world,
  /// or if `joint_world_matrices` size is mismatched.
  bool seed_pose_from_skeleton(
      const std::vector<vkpt::scene::Mat4>& joint_world_matrices,
      const std::vector<vkpt::scene::Mat4>* prev_joint_world_matrices = nullptr,
      float dt = 1.0f / 60.0f);

  /// Phase 5 RAG07: apply a world-space impulse to the body that owns the
  /// joint at `joint_index`. Returns false if the index is out of range or
  /// no body owns that joint (e.g. root joint with no body, or ragdoll not
  /// added to a world). The impulse is applied at the body's center of mass.
  bool apply_impulse_to_joint(std::int32_t joint_index,
                              vkpt::scene::Vec3 impulse);

  /// Diagnostic accessors.
  std::size_t body_count() const noexcept;
  std::size_t constraint_count() const noexcept;
  std::size_t joint_count() const noexcept;
  bool is_built() const noexcept;
  bool is_added_to_world() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace vkpt::physics
