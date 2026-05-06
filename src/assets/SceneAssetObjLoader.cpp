#include "assets/SceneAssetLoaderInternal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/AssetImporters.h"

namespace vkpt::assets::scene_asset_detail {

inline bool ParseFloat(std::string_view text, float* out) {
  if (text.empty() || out == nullptr) {
    return false;
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  }

  float value = 0.0f;
  const auto* const first = text.data();
  const auto* const last = first + text.size();
  const auto parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

inline std::optional<std::int64_t> ParseInteger(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty()) {
      return {};
    }
  }
  std::int64_t value = 0;
  const auto* const first = text.data();
  const auto* const last = first + text.size();
  const auto parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    return {};
  }
  return value;
}

inline std::optional<bool> ParseBool(std::string_view text) {
  const auto value = detail::ToLower(text);
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return {};
}

inline std::string LastPathToken(const std::vector<std::string>& words) {
  if (words.size() < 2) {
    return {};
  }
  for (std::size_t i = 1; i < words.size(); ++i) {
    if (!words[i].empty() && words[i][0] != '-') {
      return words[i];
    }
    if (words[i] == "-bm" || words[i] == "-s" || words[i] == "-o") {
      ++i;
    }
  }
  return words.back();
}

inline std::vector<ObjMaterial> ParseMtl(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return {};
  }

  std::vector<ObjMaterial> out;
  ObjMaterial current;
  bool has_current = false;
  auto flush_current = [&]() {
    if (has_current) {
      out.push_back(std::move(current));
      current = ObjMaterial{};
      has_current = false;
    }
  };

  std::string raw_line;
  while (std::getline(file, raw_line)) {
    const auto line = detail::Trim(raw_line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto words = detail::SplitWords(line);
    if (words.empty()) {
      continue;
    }
    if (words[0] == "newmtl" && words.size() >= 2) {
      flush_current();
      current = ObjMaterial{};
      current.name = words[1];
      has_current = true;
      continue;
    }
    if (!has_current) {
      current = ObjMaterial{};
      has_current = true;
    }
    if (words[0] == "Kd" && words.size() >= 4) {
      ParseFloat(words[1], &current.albedo.x);
      ParseFloat(words[2], &current.albedo.y);
      ParseFloat(words[3], &current.albedo.z);
      current.albedo.x = Clamp(current.albedo.x, 0.0f, 1.0f);
      current.albedo.y = Clamp(current.albedo.y, 0.0f, 1.0f);
      current.albedo.z = Clamp(current.albedo.z, 0.0f, 1.0f);
    } else if (words[0] == "Ke" && words.size() >= 4) {
      ParseFloat(words[1], &current.emission.x);
      ParseFloat(words[2], &current.emission.y);
      ParseFloat(words[3], &current.emission.z);
      current.emission.x = Clamp(current.emission.x, 0.0f, 64.0f);
      current.emission.y = Clamp(current.emission.y, 0.0f, 64.0f);
      current.emission.z = Clamp(current.emission.z, 0.0f, 64.0f);
    } else if (words[0] == "Ns" && words.size() >= 2) {
      float ns = 0.0f;
      if (ParseFloat(words[1], &ns)) {
        current.roughness = Clamp(std::sqrt(2.0f / (std::max(1.0f, ns) + 2.0f)), 0.04f, 1.0f);
      }
    } else if ((words[0] == "family" || words[0] == "material_family") && words.size() >= 2) {
      current.family = words[1];
    } else if ((words[0] == "Pr" || words[0] == "roughness") && words.size() >= 2) {
      ParseFloat(words[1], &current.roughness);
      current.roughness = Clamp(current.roughness, 0.0f, 1.0f);
    } else if ((words[0] == "Pm" || words[0] == "metallic") && words.size() >= 2) {
      ParseFloat(words[1], &current.metallic);
      current.metallic = Clamp(current.metallic, 0.0f, 1.0f);
    } else if ((words[0] == "Ni" || words[0] == "ior") && words.size() >= 2) {
      ParseFloat(words[1], &current.ior);
      current.ior = Clamp(current.ior, 1.0f, 4.0f);
    } else if (words[0] == "transmission" && words.size() >= 2) {
      ParseFloat(words[1], &current.transmission);
      current.transmission = Clamp(current.transmission, 0.0f, 1.0f);
    } else if (words[0] == "clearcoat" && words.size() >= 2) {
      ParseFloat(words[1], &current.clearcoat);
      current.clearcoat = Clamp(current.clearcoat, 0.0f, 1.0f);
    } else if (words[0] == "sheen" && words.size() >= 2) {
      ParseFloat(words[1], &current.sheen);
      current.sheen = Clamp(current.sheen, 0.0f, 1.0f);
    } else if (words[0] == "anisotropy" && words.size() >= 2) {
      ParseFloat(words[1], &current.anisotropy);
      current.anisotropy = Clamp(current.anisotropy, -1.0f, 1.0f);
    } else if (words[0] == "emission_intensity" && words.size() >= 2) {
      ParseFloat(words[1], &current.emission_intensity);
      current.emission_intensity = Clamp(current.emission_intensity, 0.0f, 128.0f);
    } else if (words[0] == "double_sided" && words.size() >= 2) {
      if (const auto parsed = ParseBool(words[1])) {
        current.double_sided = *parsed;
      }
    } else if (words[0] == "d" && words.size() >= 2) {
      ParseFloat(words[1], &current.alpha);
      current.alpha = Clamp(current.alpha, 0.0f, 1.0f);
    } else if (words[0] == "Tr" && words.size() >= 2) {
      float tr = 0.0f;
      if (ParseFloat(words[1], &tr)) {
        current.alpha = Clamp(1.0f - tr, 0.0f, 1.0f);
      }
    } else if (words[0] == "map_Kd" || words[0] == "baseColorTexture") {
      current.base_color_texture = LastPathToken(words);
    } else if (words[0] == "map_Bump" || words[0] == "bump" || words[0] == "map_Kn" || words[0] == "map_Disp") {
      current.normal_texture = LastPathToken(words);
    } else if (words[0] == "map_Pr") {
      current.roughness_texture = LastPathToken(words);
    } else if (words[0] == "map_Pm") {
      current.metallic_texture = LastPathToken(words);
    }
  }
  flush_current();
  return out;
}

inline std::optional<std::uint32_t> ResolveObjPositionIndex(std::string_view token, std::size_t position_count) {
  const auto slash = token.find('/');
  const auto index_text = slash == std::string_view::npos ? token : token.substr(0, slash);
  if (index_text.empty()) {
    return {};
  }
  const auto value = ParseInteger(index_text);
  if (!value) {
    return {};
  }
  const auto count = static_cast<std::int64_t>(position_count);
  const auto resolved = *value > 0 ? *value - 1 : count + *value;
  if (resolved < 0 ||
      static_cast<std::uint64_t>(resolved) >= position_count ||
      static_cast<std::uint64_t>(resolved) > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return static_cast<std::uint32_t>(resolved);
}

inline std::optional<std::uint32_t> ResolveObjTexcoordIndex(std::string_view token, std::size_t texcoord_count) {
  const auto first_slash = token.find('/');
  if (first_slash == std::string_view::npos) {
    return {};
  }
  const auto second_slash = token.find('/', first_slash + 1u);
  const auto index_text = second_slash == std::string_view::npos
                              ? token.substr(first_slash + 1u)
                              : token.substr(first_slash + 1u, second_slash - first_slash - 1u);
  if (index_text.empty()) {
    return {};
  }
  const auto value = ParseInteger(index_text);
  if (!value) {
    return {};
  }
  const auto count = static_cast<std::int64_t>(texcoord_count);
  const auto resolved = *value > 0 ? *value - 1 : count + *value;
  if (resolved < 0 ||
      static_cast<std::uint64_t>(resolved) >= texcoord_count ||
      static_cast<std::uint64_t>(resolved) > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  return static_cast<std::uint32_t>(resolved);
}

inline ObjGeometryBucket& FindOrCreateBucket(std::vector<ObjGeometryBucket>& buckets, std::string_view material_name) {
  const std::string key = material_name.empty() ? "obj_default" : std::string(material_name);
  for (auto& bucket : buckets) {
    if (bucket.material_name == key) {
      return bucket;
    }
  }
  ObjGeometryBucket bucket;
  bucket.material_name = key;
  buckets.push_back(std::move(bucket));
  return buckets.back();
}

ObjLoadResult LoadObj(const std::filesystem::path& obj_path,
                        std::vector<std::string>* diagnostics) {
  ObjLoadResult result;
  std::ifstream file(obj_path);
  if (!file) {
    if (diagnostics) {
      diagnostics->push_back("model load failed: " + PathString(obj_path));
    }
    return result;
  }

  std::vector<vkpt::scene::Vec3> positions;
  std::vector<vkpt::scene::Vec2> texcoords;
  std::vector<std::filesystem::path> material_libraries;
  std::string current_material = "obj_default";
  ObjGeometryBucket* current_bucket = nullptr;
  std::vector<std::uint32_t> local_indices;
  bool saw_faces = false;

  std::string raw_line;
  while (std::getline(file, raw_line)) {
    const auto line = detail::Trim(raw_line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto words = detail::SplitWords(line);
    if (words.empty()) {
      continue;
    }
    if (words[0] == "mtllib" && words.size() >= 2) {
      for (std::size_t i = 1; i < words.size(); ++i) {
        material_libraries.push_back((obj_path.parent_path() / words[i]).lexically_normal());
      }
    } else if (words[0] == "usemtl" && words.size() >= 2) {
      current_material = words[1];
      current_bucket = nullptr;
    } else if (words[0] == "v" && words.size() >= 4) {
      vkpt::scene::Vec3 v{};
      if (ParseFloat(words[1], &v.x) && ParseFloat(words[2], &v.y) && ParseFloat(words[3], &v.z)) {
        positions.push_back(v);
      }
    } else if (words[0] == "vt" && words.size() >= 3) {
      vkpt::scene::Vec2 uv{};
      if (ParseFloat(words[1], &uv.u) && ParseFloat(words[2], &uv.v)) {
        texcoords.push_back(uv);
      }
    } else if (words[0] == "f" && words.size() >= 4) {
      if (current_bucket == nullptr) {
        current_bucket = &FindOrCreateBucket(result.geometry, current_material);
      }
      auto& bucket = *current_bucket;
      local_indices.clear();
      if (local_indices.capacity() < words.size() - 1u) {
        local_indices.reserve(words.size() - 1u);
      }
      for (std::size_t i = 1; i < words.size(); ++i) {
        const auto source_index = ResolveObjPositionIndex(words[i], positions.size());
        if (!source_index) {
          continue;
        }
        bucket.vertices.push_back(positions[*source_index]);
        if (const auto uv_index = ResolveObjTexcoordIndex(words[i], texcoords.size())) {
          bucket.texcoords.push_back(texcoords[*uv_index]);
        } else {
          bucket.texcoords.push_back({});
        }
        local_indices.push_back(static_cast<std::uint32_t>(bucket.vertices.size() - 1u));
      }
      if (local_indices.size() >= 3u) {
        saw_faces = true;
        for (std::size_t i = 1; i + 1u < local_indices.size(); ++i) {
          bucket.indices.push_back(local_indices[0]);
          bucket.indices.push_back(local_indices[i]);
          bucket.indices.push_back(local_indices[i + 1u]);
        }
      }
    }
  }

  std::unordered_set<std::string> material_names;
  material_names.reserve(material_libraries.size() + result.geometry.size());
  for (const auto& library : material_libraries) {
    auto parsed = ParseMtl(library);
    for (auto& material : parsed) {
      material_names.insert(material.name);
      if (!material.base_color_texture.empty()) {
        result.texture_uris.push_back(material.base_color_texture);
      }
      if (!material.normal_texture.empty()) {
        result.texture_uris.push_back(material.normal_texture);
      }
      if (!material.roughness_texture.empty()) {
        result.texture_uris.push_back(material.roughness_texture);
      }
      if (!material.metallic_texture.empty()) {
        result.texture_uris.push_back(material.metallic_texture);
      }
      result.materials.push_back(std::move(material));
    }
  }
  for (const auto& bucket : result.geometry) {
    if (!material_names.contains(bucket.material_name)) {
      ObjMaterial fallback;
      fallback.name = bucket.material_name;
      result.materials.push_back(std::move(fallback));
      material_names.insert(bucket.material_name);
    }
  }
  if (result.materials.empty()) {
    result.materials.push_back(ObjMaterial{});
  }

  std::sort(result.texture_uris.begin(), result.texture_uris.end());
  result.texture_uris.erase(std::unique(result.texture_uris.begin(), result.texture_uris.end()),
                            result.texture_uris.end());

  result.geometry.erase(std::remove_if(result.geometry.begin(), result.geometry.end(),
                                       [](const ObjGeometryBucket& bucket) {
                                         return bucket.vertices.empty() ||
                                                bucket.indices.empty() ||
                                                (bucket.indices.size() % 3u) != 0u;
                                       }),
                        result.geometry.end());
  if (!saw_faces && diagnostics) {
    diagnostics->push_back("OBJ has no renderable faces: " + PathString(obj_path));
  }
  return result;
}

}  // namespace vkpt::assets::scene_asset_detail
