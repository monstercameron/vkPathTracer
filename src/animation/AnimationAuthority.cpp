#include "animation/AnimationAuthority.h"

#include <cstddef>

namespace vkpt::animation {

namespace {

bool MatchesJointCount(const std::vector<vkpt::scene::Mat4>& mats,
                       std::size_t joint_count) {
  return !mats.empty() && mats.size() == joint_count;
}

}  // namespace

AuthorityResolution resolve_authority(
    const Skeleton& skeleton,
    bool has_animation_active,
    bool has_ragdoll_active,
    const std::vector<vkpt::scene::Mat4>& animation_matrices,
    const std::vector<vkpt::scene::Mat4>& ragdoll_matrices,
    const std::vector<vkpt::scene::Mat4>& bind_world_matrices) {
  AuthorityResolution out;
  if (skeleton.joints.empty()) {
    out.source = JointAuthority::None;
    return out;
  }

  const std::size_t joint_count = skeleton.joints.size();

  if (has_ragdoll_active && MatchesJointCount(ragdoll_matrices, joint_count)) {
    out.source = JointAuthority::Ragdoll;
    out.joint_world_matrices = ragdoll_matrices;
    return out;
  }
  if (has_animation_active &&
      MatchesJointCount(animation_matrices, joint_count)) {
    out.source = JointAuthority::Animation;
    out.joint_world_matrices = animation_matrices;
    return out;
  }
  out.source = JointAuthority::BindPose;
  if (MatchesJointCount(bind_world_matrices, joint_count)) {
    out.joint_world_matrices = bind_world_matrices;
  } else {
    // Fallback: caller didn't supply a bind set. Synthesize identity per joint
    // so downstream code never sees an empty cache once we've claimed
    // BindPose authority.
    out.joint_world_matrices.assign(joint_count, vkpt::scene::Mat4{});
    for (auto& m : out.joint_world_matrices) {
      m.values[0] = 1.0f;
      m.values[5] = 1.0f;
      m.values[10] = 1.0f;
      m.values[15] = 1.0f;
    }
  }
  return out;
}

}  // namespace vkpt::animation
