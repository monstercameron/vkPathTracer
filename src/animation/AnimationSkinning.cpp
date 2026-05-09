#include "animation/AnimationSkinning.h"

#include <cstddef>

namespace vkpt::animation {
namespace {

using vkpt::scene::Mat4;

Mat4 IdentityMat4() {
  Mat4 m{};
  m.values[0] = 1.0f;
  m.values[5] = 1.0f;
  m.values[10] = 1.0f;
  m.values[15] = 1.0f;
  return m;
}

// Column-major 4x4 multiply matching the convention in Skeleton.cpp /
// AnimationSampler.cpp (both store column vectors at offsets [c*4 + r]).
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

}  // namespace

std::vector<Mat4> compute_skinning_matrices(
    const Skeleton& skeleton,
    const std::vector<Mat4>& joint_world_matrices) {
  const std::size_t count = skeleton.joints.size();
  std::vector<Mat4> out(count, IdentityMat4());
  if (count == 0u || joint_world_matrices.size() != count) {
    return out;
  }
  for (std::size_t j = 0; j < count; ++j) {
    out[j] = MulMat4(joint_world_matrices[j], skeleton.joints[j].inverse_bind);
  }
  return out;
}

}  // namespace vkpt::animation
