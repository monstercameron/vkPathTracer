#pragma once

// Phase 3 ANI03: clip sampler. Evaluates an AnimationClip at time t to produce
// per-joint local transforms (TRS) and composes those into world-space
// matrices via the same parent-chain walk used by the bind-pose helper in
// animation/Skeleton.h.
//
// Authority is *not* arbitrated here — the sampler simply produces a pose.
// Phase 5 (RAG05) will combine sampler output with ragdoll output per-entity
// per-frame and decide which one wins for the cached joint_world_matrices
// field on EntityRecord.

#include <cstdint>
#include <vector>

#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"
#include "scene/SceneTypes.h"

namespace vkpt::scene {
class SceneWorld;
}  // namespace vkpt::scene

namespace vkpt::animation {

struct JointLocalTransform {
  vkpt::scene::Vec3 translation{0.0f, 0.0f, 0.0f};
  vkpt::scene::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
  vkpt::scene::Vec3 scale{1.0f, 1.0f, 1.0f};
};

/// Evaluate the clip at time `time_seconds`, clamped to [0, duration].
/// Joints that have no track (or have an empty track) fall back to
/// `skeleton.joints[i].bind_local`. The returned vector is exactly
/// `skeleton.joints.size()` long.
std::vector<JointLocalTransform> evaluate(const AnimationClip& clip,
                                          const Skeleton& skeleton,
                                          float time_seconds);

/// Compose local TRS transforms into world-space column-major matrices. Same
/// algorithm as Skeleton::compute_bind_world_matrices but with arbitrary
/// locals.
std::vector<vkpt::scene::Mat4> compose_world_matrices(
    const Skeleton& skeleton,
    const std::vector<JointLocalTransform>& locals);

/// Iterate every entity in `world` that has both a Skeleton and an
/// AnimationComponent. Advance the component's time by `dt * speed`, wrap if
/// `loop`, then sample + compose and write into the entity's
/// joint_world_matrices cache. SimWorker integration is deferred — Phase 3
/// tests call this directly. Phase 5 (RAG05) takes ownership of the
/// authority arbitration.
void tick_animations(vkpt::scene::SceneWorld& world, float dt);

}  // namespace vkpt::animation
