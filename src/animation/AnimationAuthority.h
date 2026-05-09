#pragma once

// Phase 5 RAG05: per-entity authority arbitration. Decides which source owns
// the entity's joint cache this frame:
//   - Ragdoll wins when active.
//   - Else Animation wins when a clip is selected and not paused.
//   - Else BindPose (skeleton present, no other source).
//   - Else None (no skeleton at all).
//
// The arbiter is intentionally pure: callers provide the candidate matrices
// per source and the arbiter copies the winning set into the resolution. No
// side effects on world/components.

#include <cstdint>
#include <vector>

#include "animation/Skeleton.h"
#include "scene/SceneTypes.h"

namespace vkpt::animation {

enum class JointAuthority : std::uint8_t {
  None,        // no skeleton, joint cache untouched
  BindPose,    // skeleton present but no animation/ragdoll active — use bind pose
  Animation,   // sampler output wins
  Ragdoll,     // physics ragdoll output wins
};

struct AuthorityResolution {
  JointAuthority source = JointAuthority::None;
  std::vector<vkpt::scene::Mat4> joint_world_matrices;
};

/// Resolve per-entity authority. The skeleton must be non-empty for any
/// non-None result. If `has_ragdoll_active` is true, ragdoll wins (its
/// matrices are used). Else if `has_animation_active` is true, animation wins.
/// Else BindPose. The supplied bind_world_matrices vector should typically be
/// the result of `compute_bind_world_matrices(skeleton)`. Empty source vectors
/// degrade gracefully — if the winning source's vector is empty / mismatched,
/// the arbiter falls back to bind pose.
AuthorityResolution resolve_authority(
    const Skeleton& skeleton,
    bool has_animation_active,
    bool has_ragdoll_active,
    const std::vector<vkpt::scene::Mat4>& animation_matrices,
    const std::vector<vkpt::scene::Mat4>& ragdoll_matrices,
    const std::vector<vkpt::scene::Mat4>& bind_world_matrices);

}  // namespace vkpt::animation
