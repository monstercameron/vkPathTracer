#pragma once

#include "scene/Json.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace vkpt::scene::detail {

inline std::uint32_t component_kind_mask(ComponentKind kind) {
  return 1u << static_cast<std::size_t>(kind);
}

inline uint8_t authority_rank(TransformAuthority authority) {
  switch (authority) {
    case TransformAuthority::BenchmarkFrozen:
      return 5;
    case TransformAuthority::PhysicsControlled:
      return 4;
    case TransformAuthority::AnimationControlled:
      return 3;
    case TransformAuthority::ScriptControlled:
      return 2;
    case TransformAuthority::EditorControlled:
      return 1;
    case TransformAuthority::Authored:
    default:
      return 0;
  }
}

inline void read_camera_component(const JsonValue& object, CameraComponent& camera) {
  read_float(object, "fov", camera.fov);
  read_float(object, "near_plane", camera.near_plane);
  read_float(object, "far_plane", camera.far_plane);
  read_float(object, "focal_length_mm", camera.focal_length_mm);
  read_float(object, "sensor_width_mm", camera.sensor_width_mm);
  read_float(object, "sensor_height_mm", camera.sensor_height_mm);
  read_float(object, "aperture_radius", camera.aperture_radius);
  read_float(object, "focus_distance", camera.focus_distance);
  read_float(object, "f_stop", camera.f_stop);
  read_float(object, "shutter_seconds", camera.shutter_seconds);
  read_float(object, "iso", camera.iso);
  read_float(object, "exposure_compensation", camera.exposure_compensation);
  read_float(object, "white_balance_kelvin", camera.white_balance_kelvin);
  read_u32(object, "iris_blade_count", camera.iris_blade_count);
  read_float(object, "iris_rotation_degrees", camera.iris_rotation_degrees);
  read_float(object, "iris_roundness", camera.iris_roundness);
  read_float(object, "anamorphic_squeeze", camera.anamorphic_squeeze);
}

inline bool valid_camera_values(const CameraComponent& camera) {
  return std::isfinite(camera.fov) && camera.fov > 0.0f &&
         std::isfinite(camera.near_plane) && camera.near_plane > 0.0f &&
         std::isfinite(camera.far_plane) && camera.far_plane > camera.near_plane &&
         std::isfinite(camera.focal_length_mm) && camera.focal_length_mm > 0.0f &&
         std::isfinite(camera.sensor_width_mm) && camera.sensor_width_mm > 0.0f &&
         std::isfinite(camera.sensor_height_mm) && camera.sensor_height_mm > 0.0f &&
         std::isfinite(camera.aperture_radius) && camera.aperture_radius >= 0.0f &&
         std::isfinite(camera.focus_distance) && camera.focus_distance >= 0.0f &&
         std::isfinite(camera.f_stop) && camera.f_stop >= 0.0f &&
         std::isfinite(camera.shutter_seconds) && camera.shutter_seconds > 0.0f &&
         std::isfinite(camera.iso) && camera.iso > 0.0f &&
         std::isfinite(camera.exposure_compensation) &&
         std::isfinite(camera.white_balance_kelvin) &&
         camera.white_balance_kelvin >= 1000.0f &&
         camera.white_balance_kelvin <= 40000.0f &&
         camera.iris_blade_count <= 64u &&
         std::isfinite(camera.iris_rotation_degrees) &&
         std::isfinite(camera.iris_roundness) &&
         camera.iris_roundness >= 0.0f &&
         camera.iris_roundness <= 1.0f &&
         std::isfinite(camera.anamorphic_squeeze) &&
         camera.anamorphic_squeeze > 0.0f;
}

inline std::string camera_hash_blob(const CameraComponent& camera) {
  std::ostringstream blob;
  blob << std::to_string(camera.fov) << ':'
       << std::to_string(camera.near_plane) << ':'
       << std::to_string(camera.far_plane) << ':'
       << std::to_string(camera.focal_length_mm) << ':'
       << std::to_string(camera.sensor_width_mm) << ':'
       << std::to_string(camera.sensor_height_mm) << ':'
       << std::to_string(camera.aperture_radius) << ':'
       << std::to_string(camera.focus_distance) << ':'
       << std::to_string(camera.f_stop) << ':'
       << std::to_string(camera.shutter_seconds) << ':'
       << std::to_string(camera.iso) << ':'
       << std::to_string(camera.exposure_compensation) << ':'
       << std::to_string(camera.white_balance_kelvin) << ':'
       << std::to_string(camera.iris_blade_count) << ':'
       << std::to_string(camera.iris_rotation_degrees) << ':'
       << std::to_string(camera.iris_roundness) << ':'
       << std::to_string(camera.anamorphic_squeeze);
  return blob.str();
}

inline TransformComponent read_transform(const JsonValue& object) {
  TransformComponent transform;
  read_vec3(object, "translation", transform.translation);
  read_quat(object, "rotation", transform.rotation);
  read_vec3(object, "scale", transform.scale);
  return transform;
}

inline Mat4 identity_matrix() {
  Mat4 out{};
  out.values[0] = 1.0f;
  out.values[5] = 1.0f;
  out.values[10] = 1.0f;
  out.values[15] = 1.0f;
  return out;
}

inline Quat normalize_quat(Quat q) {
  const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (len_sq <= 0.0f || !std::isfinite(len_sq)) {
    return {};
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  q.x *= inv_len;
  q.y *= inv_len;
  q.z *= inv_len;
  q.w *= inv_len;
  return q;
}

inline Quat multiply_quat(const Quat& lhs, const Quat& rhs) {
  return normalize_quat({
      lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
      lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
      lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z});
}

inline Mat4 multiply_matrix(const Mat4& lhs, const Mat4& rhs) {
  Mat4 out{};
  for (std::size_t col = 0u; col < 4u; ++col) {
    for (std::size_t row = 0u; row < 4u; ++row) {
      float value = 0.0f;
      for (std::size_t k = 0u; k < 4u; ++k) {
        value += lhs.values[k * 4u + row] * rhs.values[col * 4u + k];
      }
      out.values[col * 4u + row] = value;
    }
  }
  return out;
}

inline Mat4 make_transform_matrix(const TransformComponent& transform) {
  const auto q = normalize_quat(transform.rotation);
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;

  Mat4 out = identity_matrix();
  out.values[0] = (1.0f - 2.0f * (yy + zz)) * transform.scale.x;
  out.values[1] = (2.0f * (xy + wz)) * transform.scale.x;
  out.values[2] = (2.0f * (xz - wy)) * transform.scale.x;

  out.values[4] = (2.0f * (xy - wz)) * transform.scale.y;
  out.values[5] = (1.0f - 2.0f * (xx + zz)) * transform.scale.y;
  out.values[6] = (2.0f * (yz + wx)) * transform.scale.y;

  out.values[8] = (2.0f * (xz + wy)) * transform.scale.z;
  out.values[9] = (2.0f * (yz - wx)) * transform.scale.z;
  out.values[10] = (1.0f - 2.0f * (xx + yy)) * transform.scale.z;

  out.values[12] = transform.translation.x;
  out.values[13] = transform.translation.y;
  out.values[14] = transform.translation.z;
  return out;
}

inline Vec3 matrix_translation(const Mat4& matrix) {
  return {matrix.values[12], matrix.values[13], matrix.values[14]};
}

inline std::optional<Mat4> inverse_affine_matrix(const Mat4& matrix) {
  const float a00 = matrix.values[0];
  const float a01 = matrix.values[4];
  const float a02 = matrix.values[8];
  const float a10 = matrix.values[1];
  const float a11 = matrix.values[5];
  const float a12 = matrix.values[9];
  const float a20 = matrix.values[2];
  const float a21 = matrix.values[6];
  const float a22 = matrix.values[10];

  const float c00 = a11 * a22 - a12 * a21;
  const float c01 = a02 * a21 - a01 * a22;
  const float c02 = a01 * a12 - a02 * a11;
  const float det = a00 * c00 + a10 * c01 + a20 * c02;
  if (!std::isfinite(det) || std::abs(det) <= 1.0e-8f) {
    return std::nullopt;
  }
  const float inv_det = 1.0f / det;

  Mat4 out = identity_matrix();
  out.values[0] = c00 * inv_det;
  out.values[4] = c01 * inv_det;
  out.values[8] = c02 * inv_det;
  out.values[1] = (a12 * a20 - a10 * a22) * inv_det;
  out.values[5] = (a00 * a22 - a02 * a20) * inv_det;
  out.values[9] = (a02 * a10 - a00 * a12) * inv_det;
  out.values[2] = (a10 * a21 - a11 * a20) * inv_det;
  out.values[6] = (a01 * a20 - a00 * a21) * inv_det;
  out.values[10] = (a00 * a11 - a01 * a10) * inv_det;

  const Vec3 t = matrix_translation(matrix);
  out.values[12] = -(out.values[0] * t.x + out.values[4] * t.y + out.values[8] * t.z);
  out.values[13] = -(out.values[1] * t.x + out.values[5] * t.y + out.values[9] * t.z);
  out.values[14] = -(out.values[2] * t.x + out.values[6] * t.y + out.values[10] * t.z);
  return out;
}

inline float column_length(const Mat4& matrix, std::size_t column) {
  const std::size_t base = column * 4u;
  return std::sqrt(matrix.values[base + 0u] * matrix.values[base + 0u] +
                   matrix.values[base + 1u] * matrix.values[base + 1u] +
                   matrix.values[base + 2u] * matrix.values[base + 2u]);
}

inline Quat quat_from_rotation_matrix(const Mat4& matrix) {
  const float m00 = matrix.values[0];
  const float m01 = matrix.values[4];
  const float m02 = matrix.values[8];
  const float m10 = matrix.values[1];
  const float m11 = matrix.values[5];
  const float m12 = matrix.values[9];
  const float m20 = matrix.values[2];
  const float m21 = matrix.values[6];
  const float m22 = matrix.values[10];
  const float trace = m00 + m11 + m22;
  Quat q{};
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
  return normalize_quat(q);
}

inline TransformComponent transform_from_matrix(const Mat4& matrix) {
  TransformComponent out;
  out.translation = matrix_translation(matrix);
  out.scale = {
      column_length(matrix, 0u),
      column_length(matrix, 1u),
      column_length(matrix, 2u)};
  Mat4 rotation = matrix;
  for (std::size_t column = 0u; column < 3u; ++column) {
    const float scale = column == 0u ? out.scale.x : (column == 1u ? out.scale.y : out.scale.z);
    if (scale > 1.0e-8f && std::isfinite(scale)) {
      const std::size_t base = column * 4u;
      rotation.values[base + 0u] /= scale;
      rotation.values[base + 1u] /= scale;
      rotation.values[base + 2u] /= scale;
    }
  }
  rotation.values[12] = 0.0f;
  rotation.values[13] = 0.0f;
  rotation.values[14] = 0.0f;
  out.rotation = quat_from_rotation_matrix(rotation);
  out.dirty = true;
  return out;
}

inline bool finite_vec3(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool finite_quat(const Quat& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

inline bool valid_transform_values(const TransformComponent& transform) {
  return finite_vec3(transform.translation) && finite_vec3(transform.scale) && finite_quat(transform.rotation) &&
         transform.scale.x != 0.0f && transform.scale.y != 0.0f && transform.scale.z != 0.0f;
}

inline vkpt::core::Hash256 hash_scene_blob(std::string_view blob) {
  constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
  constexpr std::uint64_t kFNVPrime = 1099511628211ull;
  std::uint64_t hash = kFNVOffset;
  vkpt::core::Hash256 out{};
  for (unsigned char ch : blob) {
    hash ^= ch;
    hash *= kFNVPrime;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((hash >> ((i % 8) * 8)) & 0xffu);
  }
  return out;
}

inline std::uint64_t allocate_id(const std::unordered_set<vkpt::core::StableId>& used) {
  std::uint64_t candidate = 1;
  while (used.contains(candidate)) {
    ++candidate;
  }
  return candidate;
}


}  // namespace vkpt::scene::detail
