#pragma once

// Phase 4 ANI04: skinning-matrix compute. Per frame, for each skinned mesh
// `final_skinning_matrix[i] = joint_world_matrices[i] * inverse_bind[i]`.
// The output is uploaded to a per-mesh GPU buffer (D3D12 / Vulkan), which the
// vertex-skinning compute shader (SKN03) then consumes alongside per-vertex
// JOINTS_0 / WEIGHTS_0 to deform bind-pose vertices into world-space skinned
// vertices for the BVH refit pass.
//
// At bind pose, joint_world_matrices[i] is the bind world matrix for joint i,
// and inverse_bind[i] is its inverse. Their product is the identity (within
// floating-point error), which is the smoke verification used in
// `tests/skinning_pixel_hash_smoke.cpp` and exercised below.

#include <cstdint>
#include <vector>

#include "animation/Skeleton.h"
#include "scene/SceneTypes.h"

namespace vkpt::animation {

/// CPU compute: returns `joint_world_matrices[i] * inverse_bind[i]` for every
/// joint in `skeleton`. Output size equals `skeleton.joints.size()`.
///
/// Inputs:
///   - `skeleton`: provides `inverse_bind` matrices (per joint) and the joint
///     count.
///   - `joint_world_matrices`: produced by the animation sampler or by the
///     ragdoll bridge. Must be exactly `skeleton.joints.size()` long; if it
///     is empty or wrong-sized, the output is filled with identity.
///
/// Pre-conditions:
///   - All inputs are finite. Non-finite inputs propagate to the output.
///
/// Smoke property:
///   - When `joint_world_matrices == compute_bind_world_matrices(skeleton)` —
///     i.e. the skeleton is at bind pose — every output matrix equals the
///     identity within 1e-4 component-wise.
std::vector<vkpt::scene::Mat4> compute_skinning_matrices(
    const Skeleton& skeleton,
    const std::vector<vkpt::scene::Mat4>& joint_world_matrices);

}  // namespace vkpt::animation
