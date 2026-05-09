#pragma once

// Phase 3 ANI02: animation clip storage. A clip is a list of joint tracks
// authored in glTF terms — each track holds keyframed translation, rotation,
// and/or scale samples for a single joint, indexed against a Skeleton's joint
// array. The runtime sampler in AnimationSampler.h consumes this data; the
// glTF importer in src/assets/SceneAssetGltfLoader.cpp produces it.
//
// Design notes:
// - Cubic spline tangents are interleaved per glTF VEC3/VEC4 layout with three
//   values per keyframe (in-tangent, value, out-tangent). For Phase 3 we only
//   store the value channel and fall back to Linear with a once-per-clip
//   diagnostic — see AnimationSampler.cpp.
// - Empty tracks (no keys at all) are filtered out by the importer; the
//   sampler still tolerates them defensively and falls back to bind_local.

#include <cstdint>
#include <string>
#include <vector>

#include "scene/SceneTypes.h"

namespace vkpt::animation {

enum class Interpolation : std::uint8_t {
  Step,
  Linear,
  CubicSpline,
};

struct TranslationKey {
  float time = 0.0f;
  vkpt::scene::Vec3 value{};
};

struct RotationKey {
  float time = 0.0f;
  vkpt::scene::Quat value{0.0f, 0.0f, 0.0f, 1.0f};
};

struct ScaleKey {
  float time = 0.0f;
  vkpt::scene::Vec3 value{1.0f, 1.0f, 1.0f};
};

struct JointTrack {
  std::int32_t joint_index = -1;  // index into Skeleton::joints; -1 means unused
  Interpolation interpolation = Interpolation::Linear;
  std::vector<TranslationKey> translation;
  std::vector<RotationKey> rotation;
  std::vector<ScaleKey> scale;
};

struct AnimationClip {
  std::string name;
  float duration_seconds = 0.0f;
  std::vector<JointTrack> tracks;  // sparse — not all joints have tracks
};

/// Validation: duration > 0, every track's joint_index in [0, skeleton_joint_count),
/// key times non-decreasing per channel. Returns true on success; appends one
/// or more human-readable strings to `issues` per failure if provided.
bool validate(const AnimationClip& clip,
              std::int32_t skeleton_joint_count,
              std::vector<std::string>* issues = nullptr);

}  // namespace vkpt::animation
