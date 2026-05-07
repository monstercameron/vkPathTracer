#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

struct ObjGeometryBucket {
  std::string material_name = "obj_default";
  std::vector<vkpt::scene::Vec3> vertices;
  std::vector<vkpt::scene::Vec2> texcoords;
  std::vector<std::uint32_t> indices;
};

struct ObjLoadResult {
  // Shared intermediate used by OBJ and glTF expansion before SceneDocument IDs are allocated.
  std::vector<ObjMaterial> materials;
  std::vector<ObjGeometryBucket> geometry;
  std::vector<std::string> texture_uris;
  std::string source_format = "obj";
  bool has_root_transform = false;
  vkpt::scene::TransformComponent root_transform;
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
