#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"
#include "scene/Scene.h"

namespace vkpt::assets::scene_asset_detail {

struct ObjMaterial {
  std::string name = "obj_default";
  std::string family;
  vkpt::scene::Vec3 albedo{0.75f, 0.75f, 0.75f};
  vkpt::scene::Vec3 emission{0.0f, 0.0f, 0.0f};
  float roughness = 0.85f;
  float metallic = 0.0f;
  float ior = 1.5f;
  float transmission = 0.0f;
  float clearcoat = 0.0f;
  float sheen = 0.0f;
  float anisotropy = 0.0f;
  float alpha = 1.0f;
  float emission_intensity = 0.0f;
  bool double_sided = true;
  std::string base_color_texture;
  std::string normal_texture;
  std::string roughness_texture;
  std::string metallic_texture;
};

// Phase 4 SKN01: per-vertex skinning attributes. Filled only when the source
// primitive has both `JOINTS_0` and `WEIGHTS_0` attributes AND the parent node
// references a skin. `joint_indices` is parallel to `vertices` (one VEC4 of
// skeleton-local joint indices per vertex), `joint_weights` likewise. Indices
// are remapped through the skin's `joints[]` table so they are joint-local.
// Weights are normalized to sum=1 with epsilon guard.
struct ObjGeometryBucket {
  std::string material_name = "obj_default";
  std::vector<vkpt::scene::Vec3> vertices;
  std::vector<vkpt::scene::Vec2> texcoords;
  std::vector<std::uint32_t> indices;
  // Phase 4 SKN01: per-vertex skinning attributes (parallel to `vertices`).
  // Empty when the primitive is not skinned.
  std::vector<std::array<std::uint32_t, 4>> joint_indices;
  std::vector<std::array<float, 4>> joint_weights;
};

struct ObjLoadResult {
  // Shared intermediate used by OBJ and glTF expansion before SceneDocument IDs are allocated.
  std::vector<ObjMaterial> materials;
  std::vector<ObjGeometryBucket> geometry;
  std::vector<std::string> texture_uris;
  std::string source_format = "obj";
  bool has_root_transform = false;
  vkpt::scene::TransformComponent root_transform;
  // Phase 1 ANI01: skeletal hierarchy if the source asset declared a skin.
  std::optional<vkpt::animation::Skeleton> skeleton;
  // Phase 3 ANI02/06: authored animation clips that target the skeleton's
  // joints. Filled by the glTF loader from the top-level "animations" array.
  std::vector<vkpt::animation::AnimationClip> animation_clips;
};

std::string PathString(const std::filesystem::path& path);
std::filesystem::path ResolvePath(const std::filesystem::path& base_dir, std::string_view uri);
float Clamp(float value, float lo, float hi);
vkpt::scene::TransformComponent IdentityTransform();

ObjLoadResult LoadObj(const std::filesystem::path& obj_path,
                      std::vector<std::string>* diagnostics = nullptr);
ObjLoadResult LoadGltf(const std::filesystem::path& gltf_path,
                       std::vector<std::string>* diagnostics = nullptr);

}  // namespace vkpt::assets::scene_asset_detail
