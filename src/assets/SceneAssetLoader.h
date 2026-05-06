#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "scene/Scene.h"

namespace vkpt::assets {

struct SceneAssetExpansionStats {
  std::uint32_t imported_models = 0;
  std::uint32_t imported_textures = 0;
  std::uint32_t imported_materials = 0;
  std::uint32_t imported_geometry = 0;
  std::uint32_t imported_entities = 0;
  vkpt::core::StableId imported_root_entity = 0;
};

bool ExpandSceneAssetReferences(vkpt::scene::SceneDocument& document,
                                const std::filesystem::path& scene_path,
                                SceneAssetExpansionStats* stats = nullptr,
                                std::vector<std::string>* diagnostics = nullptr);

bool ImportSceneModelAsset(vkpt::scene::SceneDocument& document,
                           const std::filesystem::path& scene_path,
                           std::string_view model_uri,
                           const vkpt::scene::TransformComponent& root_transform,
                           SceneAssetExpansionStats* stats = nullptr,
                           std::vector<std::string>* diagnostics = nullptr);

}  // namespace vkpt::assets
