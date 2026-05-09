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
