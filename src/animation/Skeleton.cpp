#include "animation/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkpt::animation {

namespace {

using vkpt::scene::Mat4;
using vkpt::scene::Quat;
using vkpt::scene::TransformComponent;
using vkpt::scene::Vec3;

Mat4 IdentityMat4() {
  Mat4 m{};
  m.values[0] = 1.0f;
  m.values[5] = 1.0f;
  m.values[10] = 1.0f;
  m.values[15] = 1.0f;
  return m;
}

// Column-major 4x4 multiply (lhs * rhs), matching the convention of the rest of
// the engine's Mat4 and the glTF inverse-bind storage layout.
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

// Compose a 4x4 column-major TRS matrix from the engine TransformComponent. We
// implement this locally rather than reaching into the renderer math because
// SkeletonComponent must compile in the asset/loader layer with no engine math
// dependency.
Mat4 ComposeTRS(const TransformComponent& t) {
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
  // column 0
  m.values[0] = r00 * t.scale.x;
  m.values[1] = r10 * t.scale.x;
  m.values[2] = r20 * t.scale.x;
  m.values[3] = 0.0f;
  // column 1
  m.values[4] = r01 * t.scale.y;
  m.values[5] = r11 * t.scale.y;
  m.values[6] = r21 * t.scale.y;
  m.values[7] = 0.0f;
  // column 2
  m.values[8] = r02 * t.scale.z;
  m.values[9] = r12 * t.scale.z;
  m.values[10] = r22 * t.scale.z;
  m.values[11] = 0.0f;
  // column 3 (translation)
  m.values[12] = t.translation.x;
  m.values[13] = t.translation.y;
  m.values[14] = t.translation.z;
  m.values[15] = 1.0f;
  return m;
}

}  // namespace

bool validate(const Skeleton& s, std::vector<std::string>* issues) {
  bool ok = true;
  auto report = [&](std::string message) {
    ok = false;
    if (issues != nullptr) {
      issues->push_back(std::move(message));
    }
  };

  if (s.joints.empty()) {
    report("skeleton has zero joints");
    return ok;
  }

  const auto count = static_cast<std::int32_t>(s.joints.size());
  std::int32_t root_count = 0;
  for (std::int32_t i = 0; i < count; ++i) {
    const auto& joint = s.joints[static_cast<std::size_t>(i)];
    if (joint.parent_index < -1 || joint.parent_index >= count) {
      report("joint " + std::to_string(i) + " has out-of-range parent");
      continue;
    }
    if (joint.parent_index == -1) {
      ++root_count;
    } else if (joint.parent_index == i) {
      report("joint " + std::to_string(i) + " references itself as parent");
    }
    for (float v : joint.inverse_bind.values) {
      if (!std::isfinite(v)) {
        report("joint " + std::to_string(i) + " has non-finite inverse-bind value");
        break;
      }
    }
  }
  if (root_count != 1) {
    report("skeleton expects exactly one root joint, found " + std::to_string(root_count));
  }

  if (s.root_index < 0 || s.root_index >= count) {
    report("skeleton root_index out of range");
  } else if (s.joints[static_cast<std::size_t>(s.root_index)].parent_index != -1) {
    report("skeleton root_index does not point to a parent-less joint");
  }

  // Cycle detection via DFS. Visit each joint, walking parent chain; bail if we
  // exceed `count` steps or revisit the start.
  for (std::int32_t i = 0; i < count; ++i) {
    std::int32_t cursor = s.joints[static_cast<std::size_t>(i)].parent_index;
    std::int32_t steps = 0;
    while (cursor != -1) {
      if (cursor < 0 || cursor >= count) {
        break;
      }
      if (cursor == i) {
        report("joint " + std::to_string(i) + " is part of a cycle");
        break;
      }
      ++steps;
      if (steps > count) {
        report("joint " + std::to_string(i) + " parent chain exceeds joint count");
        break;
      }
      cursor = s.joints[static_cast<std::size_t>(cursor)].parent_index;
    }
  }

  return ok;
}

std::vector<Mat4> compute_bind_world_matrices(const Skeleton& s) {
  const std::size_t count = s.joints.size();
  std::vector<Mat4> world(count, IdentityMat4());
  if (count == 0u) {
    return world;
  }

  // Topological order: parents must precede children. glTF often emits joints in
  // an order where a child can appear before its parent, so we sort by the
  // longest path back to a root.
  std::vector<std::int32_t> depth(count, 0);
  std::vector<std::size_t> order(count);
  for (std::size_t i = 0; i < count; ++i) {
    order[i] = i;
    std::int32_t cursor = s.joints[i].parent_index;
    std::int32_t steps = 0;
    while (cursor != -1 && steps <= static_cast<std::int32_t>(count)) {
      ++steps;
      if (cursor < 0 || cursor >= static_cast<std::int32_t>(count)) {
        break;
      }
      cursor = s.joints[static_cast<std::size_t>(cursor)].parent_index;
    }
    depth[i] = steps;
  }
  std::sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });

  for (const auto idx : order) {
    const auto& joint = s.joints[idx];
    const Mat4 local = ComposeTRS(joint.bind_local);
    if (joint.parent_index < 0 ||
        static_cast<std::size_t>(joint.parent_index) >= count) {
      world[idx] = local;
    } else {
      world[idx] = MulMat4(world[static_cast<std::size_t>(joint.parent_index)], local);
    }
  }
  return world;
}

}  // namespace vkpt::animation
