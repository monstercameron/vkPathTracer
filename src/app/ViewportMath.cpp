#include "app/ViewportInteractionInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <cmath>
#include <optional>

namespace vkpt::app {

vkpt::pathtracer::Vec3 PtAdd(const vkpt::pathtracer::Vec3 &a,
                             const vkpt::pathtracer::Vec3 &b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vkpt::pathtracer::Vec3 PtSub(const vkpt::pathtracer::Vec3 &a,
                             const vkpt::pathtracer::Vec3 &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vkpt::pathtracer::Vec3 PtMul(const vkpt::pathtracer::Vec3 &v, float scale) {
  return {v.x * scale, v.y * scale, v.z * scale};
}

float PtDot(const vkpt::pathtracer::Vec3 &a, const vkpt::pathtracer::Vec3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

vkpt::pathtracer::Vec3 PtCross(const vkpt::pathtracer::Vec3 &a,
                               const vkpt::pathtracer::Vec3 &b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}

float PtLength(const vkpt::pathtracer::Vec3 &v) {
  return std::sqrt(std::max(0.0f, PtDot(v, v)));
}

vkpt::pathtracer::Vec3 PtNormalize(const vkpt::pathtracer::Vec3 &v,
                                   vkpt::pathtracer::Vec3 fallback) {
  const float len = PtLength(v);
  if (len <= 1.0e-6f) {
    return fallback;
  }
  return PtMul(v, 1.0f / len);
}

vkpt::scene::Quat NormalizeQuat(vkpt::scene::Quat q) {
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 1.0e-6f) {
    return {};
  }
  const float inv = 1.0f / len;
  q.x *= inv;
  q.y *= inv;
  q.z *= inv;
  q.w *= inv;
  return q;
}

vkpt::scene::Quat QuatMultiply(const vkpt::scene::Quat &a,
                               const vkpt::scene::Quat &b) {
  return NormalizeQuat({
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  });
}

vkpt::scene::Quat QuatFromAxisAngle(const vkpt::pathtracer::Vec3 &axis,
                                    float radians) {
  const auto normalized = PtNormalize(axis, {0.0f, 1.0f, 0.0f});
  const float half = radians * 0.5f;
  const float s = std::sin(half);
  return NormalizeQuat(
      {normalized.x * s, normalized.y * s, normalized.z * s, std::cos(half)});
}

vkpt::scene::Quat
QuatFromCameraForwardUp(const vkpt::pathtracer::Vec3 &forwardIn,
                        const vkpt::pathtracer::Vec3 &upIn) {
  const auto forward = PtNormalize(forwardIn, {0.0f, 0.0f, -1.0f});
  const auto right = PtNormalize(PtCross(forward, upIn), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});

  const float m00 = right.x;
  const float m01 = up.x;
  const float m02 = -forward.x;
  const float m10 = right.y;
  const float m11 = up.y;
  const float m12 = -forward.y;
  const float m20 = right.z;
  const float m21 = up.z;
  const float m22 = -forward.z;

  vkpt::scene::Quat q;
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  return NormalizeQuat(q);
}

vkpt::pathtracer::Vec3 RotatePointByQuat(const vkpt::pathtracer::Vec3 &point,
                                         const vkpt::scene::Quat &rotation) {
  const auto q = NormalizeQuat(rotation);
  const vkpt::pathtracer::Vec3 qv{q.x, q.y, q.z};
  const auto t = PtMul(PtCross(qv, point), 2.0f);
  return PtAdd(PtAdd(point, PtMul(t, q.w)), PtCross(qv, t));
}

vkpt::pathtracer::Vec3
InverseRotatePointByQuat(const vkpt::pathtracer::Vec3 &point,
                         const vkpt::scene::Quat &rotation) {
  auto q = NormalizeQuat(rotation);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return RotatePointByQuat(point, q);
}

vkpt::pathtracer::Vec3
ApplySceneTransformToPoint(const vkpt::pathtracer::Vec3 &point,
                           const vkpt::scene::TransformComponent &transform) {
  const auto scaled = vkpt::pathtracer::Vec3{point.x * transform.scale.x,
                                             point.y * transform.scale.y,
                                             point.z * transform.scale.z};
  const auto rotated = RotatePointByQuat(scaled, transform.rotation);
  return PtAdd(rotated, {transform.translation.x, transform.translation.y,
                         transform.translation.z});
}

vkpt::pathtracer::Vec3
InverseSceneTransformPoint(const vkpt::pathtracer::Vec3 &point,
                           const vkpt::scene::TransformComponent &transform) {
  const auto translated =
      PtSub(point, {transform.translation.x, transform.translation.y,
                    transform.translation.z});
  const auto unrotated =
      InverseRotatePointByQuat(translated, transform.rotation);
  const auto safeScale = [](float value) {
    return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
  };
  return {unrotated.x / safeScale(transform.scale.x),
          unrotated.y / safeScale(transform.scale.y),
          unrotated.z / safeScale(transform.scale.z)};
}

float ClampFloat(float value, float min_value, float max_value) {
  return std::min(max_value, std::max(min_value, value));
}

float DegToRad(float degrees) {
  return degrees * (3.14159265358979323846f / 180.0f);
}

bool AnimationHasAuthoredMotion(
    const vkpt::scene::AnimationComponent &animation) {
  constexpr float kEpsilon = 1.0e-6f;
  return std::fabs(animation.translation_amplitude.x) > kEpsilon ||
         std::fabs(animation.translation_amplitude.y) > kEpsilon ||
         std::fabs(animation.translation_amplitude.z) > kEpsilon ||
         std::fabs(animation.rotation_degrees.x) > kEpsilon ||
         std::fabs(animation.rotation_degrees.y) > kEpsilon ||
         std::fabs(animation.rotation_degrees.z) > kEpsilon ||
         std::fabs(animation.scale_amplitude.x) > kEpsilon ||
         std::fabs(animation.scale_amplitude.y) > kEpsilon ||
         std::fabs(animation.scale_amplitude.z) > kEpsilon;
}

vkpt::scene::Quat QuatFromEulerDegrees(const vkpt::scene::Vec3 &degrees) {
  const auto qx = QuatFromAxisAngle({1.0f, 0.0f, 0.0f}, DegToRad(degrees.x));
  const auto qy = QuatFromAxisAngle({0.0f, 1.0f, 0.0f}, DegToRad(degrees.y));
  const auto qz = QuatFromAxisAngle({0.0f, 0.0f, 1.0f}, DegToRad(degrees.z));
  return QuatMultiply(qz, QuatMultiply(qy, qx));
}

vkpt::scene::TransformComponent
SampleAnimationTransform(const vkpt::scene::TransformComponent &base,
                         const vkpt::scene::AnimationComponent &animation,
                         float elapsed_seconds) {
  vkpt::scene::TransformComponent out = base;
  const float duration = std::max(1.0f / 60.0f, animation.duration_seconds);
  float local_time = elapsed_seconds * animation.playback_speed;
  if (animation.looping) {
    local_time = std::fmod(local_time, duration);
    if (local_time < 0.0f) {
      local_time += duration;
    }
  } else {
    local_time = ClampFloat(local_time, 0.0f, duration);
  }
  const float phase =
      duration > 0.0f ? ClampFloat(local_time / duration, 0.0f, 1.0f) : 0.0f;
  const float wave = std::sin(phase * 6.28318530717958647692f);

  out.translation.x += animation.translation_amplitude.x * wave;
  out.translation.y += animation.translation_amplitude.y * wave;
  out.translation.z += animation.translation_amplitude.z * wave;

  const vkpt::scene::Vec3 animated_degrees{animation.rotation_degrees.x * phase,
                                           animation.rotation_degrees.y * phase,
                                           animation.rotation_degrees.z *
                                               phase};
  out.rotation =
      QuatMultiply(QuatFromEulerDegrees(animated_degrees), base.rotation);

  out.scale.x = std::max(
      1.0e-5f, base.scale.x * (1.0f + animation.scale_amplitude.x * wave));
  out.scale.y = std::max(
      1.0e-5f, base.scale.y * (1.0f + animation.scale_amplitude.y * wave));
  out.scale.z = std::max(
      1.0e-5f, base.scale.z * (1.0f + animation.scale_amplitude.z * wave));
  out.dirty = true;
  return out;
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::scene::Vec3 &v) {
  return {v.x, v.y, v.z};
}

vkpt::scene::Vec3 ToSceneVec3(const vkpt::pathtracer::Vec3 &v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Quat4 ToPtQuat4(const vkpt::scene::Quat &q) {
  return {q.x, q.y, q.z, q.w};
}

vkpt::editor::Vec3 ToEditorVec3(const vkpt::pathtracer::Vec3 &v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::editor::Vec3 &v) {
  return {v.x, v.y, v.z};
}

void ExpandBounds(vkpt::editor::Bounds &bounds,
                  const vkpt::pathtracer::Vec3 &point) {
  if (!bounds.valid) {
    bounds.min = ToEditorVec3(point);
    bounds.max = ToEditorVec3(point);
    bounds.valid = true;
    return;
  }
  bounds.min.x = std::min(bounds.min.x, point.x);
  bounds.min.y = std::min(bounds.min.y, point.y);
  bounds.min.z = std::min(bounds.min.z, point.z);
  bounds.max.x = std::max(bounds.max.x, point.x);
  bounds.max.y = std::max(bounds.max.y, point.y);
  bounds.max.z = std::max(bounds.max.z, point.z);
}

vkpt::pathtracer::Vec3
TransformPointForPreview(const vkpt::scene::Vec3 &point,
                         const vkpt::scene::TransformComponent &transform) {
  return ApplySceneTransformToPoint({point.x, point.y, point.z}, transform);
}

std::optional<vkpt::scene::SceneWorld>
BuildSceneWorldSnapshot(const vkpt::scene::SceneDocument &document) {
  auto worldResult = document.to_world();
  if (!worldResult) {
    return std::nullopt;
  }
  auto world = std::move(worldResult.value());
  world.recompute_world_transforms();
  return world;
}

vkpt::scene::TransformComponent
ResolveEntityWorldTransform(const vkpt::scene::SceneEntityDefinition &entity,
                            const vkpt::scene::SceneWorld *world) {
  if (world != nullptr) {
    if (const auto *worldTransform = world->world_transform(entity.id)) {
      vkpt::scene::TransformComponent transform = *worldTransform;
      transform.dirty = entity.has_transform ? entity.transform.dirty : false;
      return transform;
    }
  }
  return entity.has_transform ? entity.transform
                              : vkpt::scene::TransformComponent{};
}

vkpt::scene::TransformComponent
TransformFromRtInstance(const vkpt::pathtracer::RTInstance &instance) {
  vkpt::scene::TransformComponent transform;
  transform.translation = {instance.translation.x, instance.translation.y,
                           instance.translation.z};
  transform.rotation = {instance.rotation.x, instance.rotation.y,
                        instance.rotation.z, instance.rotation.w};
  transform.scale = {instance.scale.x, instance.scale.y, instance.scale.z};
  transform.dirty = false;
  return transform;
}

vkpt::scene::Quat InverseQuat(vkpt::scene::Quat q) {
  q = NormalizeQuat(q);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return q;
}

float SafeTransformScaleDivisor(float value) {
  return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
}

vkpt::scene::TransformComponent ConvertWorldTransformToDocumentLocal(
    const vkpt::scene::SceneEntityDefinition &entity,
    const vkpt::scene::SceneWorld *currentWorld,
    const vkpt::scene::TransformComponent &worldTransform) {
  if (!entity.has_hierarchy || entity.hierarchy.parent == 0 ||
      currentWorld == nullptr) {
    return worldTransform;
  }

  const auto *parentWorld =
      currentWorld->world_transform(entity.hierarchy.parent);
  if (parentWorld == nullptr) {
    return worldTransform;
  }

  vkpt::scene::TransformComponent local = worldTransform;
  const auto delta = PtSub(ToPtVec3(worldTransform.translation),
                           ToPtVec3(parentWorld->translation));
  const auto unrotated = InverseRotatePointByQuat(delta, parentWorld->rotation);
  local.translation = {
      unrotated.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      unrotated.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      unrotated.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.rotation =
      QuatMultiply(InverseQuat(parentWorld->rotation), worldTransform.rotation);
  local.scale = {
      worldTransform.scale.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      worldTransform.scale.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      worldTransform.scale.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.dirty = true;
  return local;
}

} // namespace vkpt::app

#endif
