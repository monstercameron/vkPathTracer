#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scene/SceneTypes.h"

namespace vkpt::animation {

/// One skeletal joint. `parent_index` is a joint-local index into `Skeleton::joints`,
/// not a glTF node index. -1 marks the root.
struct Joint {
  std::string name;
  std::int32_t parent_index = -1;
  vkpt::scene::Mat4 inverse_bind{};
  vkpt::scene::TransformComponent bind_local{};
};

/// Authored bind-pose skeleton. The joints array is the canonical ordering used
/// by SKN01/ANI02 to index per-vertex weights and animation tracks.
struct Skeleton {
  std::vector<Joint> joints;
  std::int32_t root_index = -1;
};

/// Returns true when the skeleton is acyclic, single-rooted, every parent index
/// refers to a valid earlier joint after topological sort, and inverse-bind
/// matrices contain only finite values.
bool validate(const Skeleton& s, std::vector<std::string>* issues = nullptr);

/// Compose parent-chain matrices for each joint at bind pose. Output ordering
/// matches `s.joints`. glTF does not guarantee topologically sorted joint order,
/// so this routine first walks parents-before-children before composing.
std::vector<vkpt::scene::Mat4> compute_bind_world_matrices(const Skeleton& s);

}  // namespace vkpt::animation
