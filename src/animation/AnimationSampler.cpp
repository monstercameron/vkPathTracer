#include "animation/AnimationSampler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "core/log/Log.h"
#include "scene/SceneWorld.h"

namespace vkpt::animation {

namespace {

using vkpt::scene::Mat4;
using vkpt::scene::Quat;
using vkpt::scene::Vec3;

Mat4 IdentityMat4() {
  Mat4 m{};
  m.values[0] = 1.0f;
  m.values[5] = 1.0f;
  m.values[10] = 1.0f;
  m.values[15] = 1.0f;
  return m;
}

Mat4 MulMat4(const Mat4& lhs, const Mat4& rhs) {
  Mat4 out{};
  for (std::size_t col = 0; col < 4u; ++col) {
    for (std::size_t row = 0; row < 4u; ++row) {
      float sum = 0.0f;
      for (std::size_t k = 0; k < 4u; ++k) {
        sum += lhs.values[k * 4u + row] * rhs.values[col * 4u + k];
      }
      out.values[col * 4u + row] = sum;
    }
  }
  return out;
}

Mat4 ComposeTRS(const JointLocalTransform& t) {
  const Quat& q = t.rotation;
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;

  const float r00 = 1.0f - 2.0f * (yy + zz);
  const float r01 = 2.0f * (xy - wz);
  const float r02 = 2.0f * (xz + wy);
  const float r10 = 2.0f * (xy + wz);
  const float r11 = 1.0f - 2.0f * (xx + zz);
  const float r12 = 2.0f * (yz - wx);
  const float r20 = 2.0f * (xz - wy);
  const float r21 = 2.0f * (yz + wx);
  const float r22 = 1.0f - 2.0f * (xx + yy);

  Mat4 m{};
  m.values[0] = r00 * t.scale.x;
  m.values[1] = r10 * t.scale.x;
  m.values[2] = r20 * t.scale.x;
  m.values[3] = 0.0f;
  m.values[4] = r01 * t.scale.y;
  m.values[5] = r11 * t.scale.y;
  m.values[6] = r21 * t.scale.y;
  m.values[7] = 0.0f;
  m.values[8] = r02 * t.scale.z;
  m.values[9] = r12 * t.scale.z;
  m.values[10] = r22 * t.scale.z;
  m.values[11] = 0.0f;
  m.values[12] = t.translation.x;
  m.values[13] = t.translation.y;
  m.values[14] = t.translation.z;
  m.values[15] = 1.0f;
  return m;
}

Vec3 LerpVec3(const Vec3& a, const Vec3& b, float t) {
  return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
              a.z + (b.z - a.z) * t};
}

Quat NormalizeQuat(const Quat& q) {
  const float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!std::isfinite(lenSq) || lenSq <= 0.0f) {
    return Quat{0.0f, 0.0f, 0.0f, 1.0f};
  }
  const float inv = 1.0f / std::sqrt(lenSq);
  return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Spherical linear interpolation along the shortest arc.
Quat SlerpQuat(Quat a, Quat b, float t) {
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (dot < 0.0f) {
    b = Quat{-b.x, -b.y, -b.z, -b.w};
    dot = -dot;
  }
  // Use linear interpolation for nearly-parallel rotations to avoid
  // singularities; renormalize at the end either way.
  if (dot > 0.9995f) {
    Quat r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
           a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    return NormalizeQuat(r);
  }
  const float clamped = std::clamp(dot, -1.0f, 1.0f);
  const float theta_0 = std::acos(clamped);
  const float theta = theta_0 * t;
  const float sin_theta = std::sin(theta);
  const float sin_theta_0 = std::sin(theta_0);
  const float s0 = std::cos(theta) - clamped * sin_theta / sin_theta_0;
  const float s1 = sin_theta / sin_theta_0;
  return Quat{s0 * a.x + s1 * b.x, s0 * a.y + s1 * b.y,
              s0 * a.z + s1 * b.z, s0 * a.w + s1 * b.w};
}

// Locate the segment [keys[i], keys[i+1]] containing `t`. Returns:
//   {i, alpha}  with i in [0, keys.size()-1) and alpha in [0,1] for Linear
//   {i, 0}      held value (Step or before-first/after-last)
// Empty keys -> {-1, 0}.
struct KeySegment {
  std::int32_t i = -1;
  float alpha = 0.0f;
};

template <typename Key>
KeySegment FindSegment(const std::vector<Key>& keys, float t,
                       Interpolation interp) {
  if (keys.empty()) {
    return {};
  }
  if (t <= keys.front().time) {
    return {0, 0.0f};
  }
  if (t >= keys.back().time) {
    return {static_cast<std::int32_t>(keys.size()) - 1, 0.0f};
  }
  // Binary search the largest index with time <= t.
  std::size_t lo = 0;
  std::size_t hi = keys.size() - 1;
  while (lo + 1 < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (keys[mid].time <= t) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const std::size_t i0 = lo;
  const std::size_t i1 = lo + 1;
  if (interp == Interpolation::Step) {
    return {static_cast<std::int32_t>(i0), 0.0f};
  }
  const float dt = keys[i1].time - keys[i0].time;
  float alpha = 0.0f;
  if (dt > 0.0f) {
    alpha = std::clamp((t - keys[i0].time) / dt, 0.0f, 1.0f);
  }
  return {static_cast<std::int32_t>(i0), alpha};
}

JointLocalTransform BindLocalToTransform(const vkpt::scene::TransformComponent& src) {
  JointLocalTransform out;
  out.translation = src.translation;
  out.rotation = src.rotation;
  out.scale = src.scale;
  return out;
}

}  // namespace

std::vector<JointLocalTransform> evaluate(const AnimationClip& clip,
                                          const Skeleton& skeleton,
                                          float time_seconds) {
  std::vector<JointLocalTransform> out(skeleton.joints.size());
  for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
    out[j] = BindLocalToTransform(skeleton.joints[j].bind_local);
  }
  if (skeleton.joints.empty()) {
    return out;
  }

  const float duration = std::max(clip.duration_seconds, 0.0f);
  const float t =
      std::isfinite(time_seconds) ? std::clamp(time_seconds, 0.0f, duration) : 0.0f;

  // Cubic spline support is intentionally deferred — log once per sample call
  // when we encounter a CubicSpline track and process it as Linear. The clip
  // import filter bumps the diagnostic at the import site as well, so this is
  // a safety net.
  static thread_local std::unordered_set<const AnimationClip*> warned_clips;
  bool warned_this_call = false;

  for (const auto& track : clip.tracks) {
    if (track.joint_index < 0 ||
        static_cast<std::size_t>(track.joint_index) >= skeleton.joints.size()) {
      continue;
    }
    auto& dst = out[static_cast<std::size_t>(track.joint_index)];
    Interpolation effective = track.interpolation;
    if (effective == Interpolation::CubicSpline) {
      if (!warned_this_call && warned_clips.insert(&clip).second) {
        VKP_LOG(Warn, "animation",
                "cubic_spline_fallback",
                "clip", clip.name,
                "joint", track.joint_index);
      }
      warned_this_call = true;
      effective = Interpolation::Linear;
    }

    if (!track.translation.empty()) {
      const auto seg = FindSegment(track.translation, t, effective);
      if (seg.i >= 0) {
        const auto& a = track.translation[static_cast<std::size_t>(seg.i)];
        if (effective == Interpolation::Step ||
            seg.alpha == 0.0f ||
            static_cast<std::size_t>(seg.i) + 1 >= track.translation.size()) {
          dst.translation = a.value;
        } else {
          const auto& b = track.translation[static_cast<std::size_t>(seg.i) + 1u];
          dst.translation = LerpVec3(a.value, b.value, seg.alpha);
        }
      }
    }
    if (!track.rotation.empty()) {
      const auto seg = FindSegment(track.rotation, t, effective);
      if (seg.i >= 0) {
        const auto& a = track.rotation[static_cast<std::size_t>(seg.i)];
        if (effective == Interpolation::Step ||
            seg.alpha == 0.0f ||
            static_cast<std::size_t>(seg.i) + 1 >= track.rotation.size()) {
          dst.rotation = NormalizeQuat(a.value);
        } else {
          const auto& b = track.rotation[static_cast<std::size_t>(seg.i) + 1u];
          dst.rotation = SlerpQuat(NormalizeQuat(a.value),
                                   NormalizeQuat(b.value), seg.alpha);
        }
      }
    }
    if (!track.scale.empty()) {
      const auto seg = FindSegment(track.scale, t, effective);
      if (seg.i >= 0) {
        const auto& a = track.scale[static_cast<std::size_t>(seg.i)];
        if (effective == Interpolation::Step ||
            seg.alpha == 0.0f ||
            static_cast<std::size_t>(seg.i) + 1 >= track.scale.size()) {
          dst.scale = a.value;
        } else {
          const auto& b = track.scale[static_cast<std::size_t>(seg.i) + 1u];
          dst.scale = LerpVec3(a.value, b.value, seg.alpha);
        }
      }
    }
  }

  return out;
}

std::vector<vkpt::scene::Mat4> compose_world_matrices(
    const Skeleton& skeleton,
    const std::vector<JointLocalTransform>& locals) {
  const std::size_t count = skeleton.joints.size();
  std::vector<Mat4> world(count, IdentityMat4());
  if (count == 0u || locals.size() != count) {
    return world;
  }

  // Topological order: parents must precede children. Mirrors the algorithm
  // in Skeleton::compute_bind_world_matrices.
  std::vector<std::int32_t> depth(count, 0);
  std::vector<std::size_t> order(count);
  for (std::size_t i = 0; i < count; ++i) {
    order[i] = i;
    std::int32_t cursor = skeleton.joints[i].parent_index;
    std::int32_t steps = 0;
    while (cursor != -1 && steps <= static_cast<std::int32_t>(count)) {
      ++steps;
      if (cursor < 0 || cursor >= static_cast<std::int32_t>(count)) {
        break;
      }
      cursor = skeleton.joints[static_cast<std::size_t>(cursor)].parent_index;
    }
    depth[i] = steps;
  }
  std::sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });

  for (const auto idx : order) {
    const auto& joint = skeleton.joints[idx];
    const Mat4 local = ComposeTRS(locals[idx]);
    if (joint.parent_index < 0 ||
        static_cast<std::size_t>(joint.parent_index) >= count) {
      world[idx] = local;
    } else {
      world[idx] =
          MulMat4(world[static_cast<std::size_t>(joint.parent_index)], local);
    }
  }
  return world;
}

void tick_animations(vkpt::scene::SceneWorld& world, float dt) {
  if (!std::isfinite(dt)) {
    return;
  }
  for (const auto entity_id : world.all_entities()) {
    auto* entity = world.get_entity(entity_id);
    if (entity == nullptr || !entity->skeleton.has_value() ||
        !entity->animation.has_value() || entity->clips.empty()) {
      continue;
    }
    auto& anim = *entity->animation;
    if (anim.paused) {
      continue;
    }
    if (anim.clip_index < 0 ||
        static_cast<std::size_t>(anim.clip_index) >= entity->clips.size()) {
      continue;
    }
    const auto& clip = entity->clips[static_cast<std::size_t>(anim.clip_index)];
    if (clip.duration_seconds <= 0.0f) {
      continue;
    }
    anim.time_seconds += dt * anim.speed;
    if (anim.loop) {
      anim.time_seconds = std::fmod(anim.time_seconds, clip.duration_seconds);
      if (anim.time_seconds < 0.0f) {
        anim.time_seconds += clip.duration_seconds;
      }
    } else {
      anim.time_seconds =
          std::clamp(anim.time_seconds, 0.0f, clip.duration_seconds);
    }
    const auto locals = evaluate(clip, *entity->skeleton, anim.time_seconds);
    entity->joint_world_matrices = compose_world_matrices(*entity->skeleton, locals);
  }
}

}  // namespace vkpt::animation
