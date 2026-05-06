#include "assets/SceneAssetLoaderInternal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

namespace vkpt::assets::scene_asset_detail {

std::string PathString(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

std::filesystem::path ResolvePath(const std::filesystem::path& base_dir, std::string_view uri) {
  const std::filesystem::path requested{std::string(uri)};
  if (requested.is_absolute()) {
    return requested.lexically_normal();
  }
  const auto scene_relative = (base_dir / requested).lexically_normal();
  if (std::filesystem::exists(scene_relative)) {
    return scene_relative;
  }
  const auto cwd_relative = (std::filesystem::current_path() / requested).lexically_normal();
  if (std::filesystem::exists(cwd_relative)) {
    return cwd_relative;
  }
  return scene_relative;
}

float Clamp(float value, float lo, float hi) {
  if (!std::isfinite(value)) {
    return lo;
  }
  return std::min(hi, std::max(lo, value));
}

vkpt::scene::TransformComponent IdentityTransform() {
  vkpt::scene::TransformComponent transform;
  transform.translation = {0.0f, 0.0f, 0.0f};
  transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  transform.scale = {1.0f, 1.0f, 1.0f};
  transform.dirty = true;
  return transform;
}

}  // namespace vkpt::assets::scene_asset_detail
