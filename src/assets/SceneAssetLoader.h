#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/AssetImporters.h"
#include "scene/Scene.h"

namespace vkpt::assets {

struct SceneAssetExpansionStats {
  std::uint32_t imported_models = 0;
  std::uint32_t imported_textures = 0;
  std::uint32_t imported_materials = 0;
  std::uint32_t imported_geometry = 0;
  std::uint32_t imported_entities = 0;
};

namespace scene_asset_detail {

struct IdSets {
  std::unordered_set<vkpt::core::StableId> assets;
  std::unordered_set<vkpt::core::StableId> materials;
  std::unordered_set<vkpt::core::StableId> geometry;
  std::unordered_set<vkpt::core::StableId> entities;
  std::unordered_set<std::string> asset_uris;
};

struct ObjMaterial {
  std::string name = "obj_default";
  vkpt::scene::Vec3 albedo{0.75f, 0.75f, 0.75f};
  float roughness = 0.85f;
  float metallic = 0.0f;
  float alpha = 1.0f;
  std::string base_color_texture;
  std::string normal_texture;
  std::string roughness_texture;
  std::string metallic_texture;
};

struct ObjGeometryBucket {
  std::string material_name = "obj_default";
  std::vector<vkpt::scene::Vec3> vertices;
  std::vector<std::uint32_t> indices;
};

struct ObjLoadResult {
  std::vector<ObjMaterial> materials;
  std::vector<ObjGeometryBucket> geometry;
  std::vector<std::string> texture_uris;
};

inline std::string PathString(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

inline std::filesystem::path ResolvePath(const std::filesystem::path& base_dir, std::string_view uri) {
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

inline std::string UriRelativeTo(const std::filesystem::path& base_dir, const std::filesystem::path& path) {
  std::error_code ec;
  auto relative = std::filesystem::relative(path, base_dir, ec);
  if (!ec && !relative.empty()) {
    return PathString(relative);
  }
  return PathString(path);
}

inline bool ParseFloat(std::string_view text, float* out) {
  if (text.empty() || out == nullptr) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const auto value = std::stof(std::string(text), &consumed);
    if (consumed == 0 || !std::isfinite(value)) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

inline float Clamp(float value, float lo, float hi) {
  if (!std::isfinite(value)) {
    return lo;
  }
  return std::min(hi, std::max(lo, value));
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
      out.push_back(current);
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
    } else if (words[0] == "Ns" && words.size() >= 2) {
      float ns = 0.0f;
      if (ParseFloat(words[1], &ns)) {
        current.roughness = Clamp(std::sqrt(2.0f / (std::max(1.0f, ns) + 2.0f)), 0.04f, 1.0f);
      }
    } else if ((words[0] == "Pr" || words[0] == "roughness") && words.size() >= 2) {
      ParseFloat(words[1], &current.roughness);
      current.roughness = Clamp(current.roughness, 0.0f, 1.0f);
    } else if ((words[0] == "Pm" || words[0] == "metallic") && words.size() >= 2) {
      ParseFloat(words[1], &current.metallic);
      current.metallic = Clamp(current.metallic, 0.0f, 1.0f);
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
    } else if (words[0] == "map_Bump" || words[0] == "bump" || words[0] == "map_Kn") {
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
  try {
    const int value = std::stoi(std::string(index_text));
    int resolved = value > 0 ? value - 1 : static_cast<int>(position_count) + value;
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= position_count) {
      return {};
    }
    return static_cast<std::uint32_t>(resolved);
  } catch (...) {
    return {};
  }
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

inline ObjLoadResult LoadObj(const std::filesystem::path& obj_path,
                             std::vector<std::string>* diagnostics = nullptr) {
  ObjLoadResult result;
  std::ifstream file(obj_path);
  if (!file) {
    if (diagnostics) {
      diagnostics->push_back("model load failed: " + PathString(obj_path));
    }
    return result;
  }

  std::vector<vkpt::scene::Vec3> positions;
  std::vector<std::filesystem::path> material_libraries;
  std::string current_material = "obj_default";
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
    } else if (words[0] == "v" && words.size() >= 4) {
      vkpt::scene::Vec3 v{};
      if (ParseFloat(words[1], &v.x) && ParseFloat(words[2], &v.y) && ParseFloat(words[3], &v.z)) {
        positions.push_back(v);
      }
    } else if (words[0] == "f" && words.size() >= 4) {
      auto& bucket = FindOrCreateBucket(result.geometry, current_material);
      std::vector<std::uint32_t> local_indices;
      local_indices.reserve(words.size() - 1u);
      for (std::size_t i = 1; i < words.size(); ++i) {
        const auto source_index = ResolveObjPositionIndex(words[i], positions.size());
        if (!source_index) {
          continue;
        }
        bucket.vertices.push_back(positions[*source_index]);
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

inline vkpt::core::StableId AllocateId(std::unordered_set<vkpt::core::StableId>& used,
                                       vkpt::core::StableId preferred) {
  vkpt::core::StableId candidate = preferred == 0 ? 1 : preferred;
  while (used.contains(candidate)) {
    ++candidate;
    if (candidate == 0) {
      candidate = 1;
    }
  }
  used.insert(candidate);
  return candidate;
}

inline IdSets CollectIds(const vkpt::scene::SceneDocument& document) {
  IdSets out;
  for (const auto& asset : document.assets) {
    out.assets.insert(asset.id);
    if (!asset.uri.empty()) {
      out.asset_uris.insert(asset.uri);
    }
  }
  for (const auto& material : document.materials) {
    out.materials.insert(material.id);
  }
  for (const auto& geometry : document.geometry) {
    out.geometry.insert(geometry.id);
  }
  for (const auto& entity : document.entities) {
    out.entities.insert(entity.id);
  }
  return out;
}

inline bool IsModelAsset(const vkpt::scene::SceneAssetDefinition& asset) {
  const auto type = detail::ToLower(asset.type);
  const auto ext = detail::ExtensionOf(asset.uri);
  return type == "model" || type == "mesh" || type == "obj" || type == "model/obj" ||
         ext == ".obj";
}

inline bool IsTextureUri(std::string_view uri) {
  const auto ext = detail::ExtensionOf(uri);
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

inline bool AppendTextureAsset(vkpt::scene::SceneDocument& document,
                               IdSets& ids,
                               const std::filesystem::path& scene_dir,
                               const std::filesystem::path& texture_path,
                               std::string_view binding_context,
                               vkpt::core::StableId preferred_id,
                               std::vector<std::string>* diagnostics) {
  const auto texture_uri = UriRelativeTo(scene_dir, texture_path);
  if (ids.asset_uris.contains(texture_uri)) {
    return true;
  }

  TextureMetadataImporter importer;
  AssetImportSource source;
  source.uri = texture_path.string();
  source.root_directory = texture_path.parent_path().string();
  source.binding_context = std::string(binding_context);
  AssetImportOptions options;
  options.metadata_only = false;
  const auto validation = importer.validate_source(source, options);
  if (!validation.valid) {
    if (diagnostics) {
      diagnostics->push_back("texture load failed: " + texture_uri + " (" + validation.reason + ")");
    }
    return false;
  }
  const auto imported = importer.import_source(source, options);
  if (!imported.success) {
    if (diagnostics) {
      diagnostics->push_back("texture import failed: " + texture_uri);
    }
    return false;
  }

  vkpt::scene::SceneAssetDefinition texture_asset;
  texture_asset.id = AllocateId(ids.assets, preferred_id);
  texture_asset.type = "texture";
  texture_asset.uri = texture_uri;
  document.assets.push_back(std::move(texture_asset));
  ids.asset_uris.insert(texture_uri);
  return true;
}

inline vkpt::scene::TransformComponent IdentityTransform() {
  vkpt::scene::TransformComponent transform;
  transform.translation = {0.0f, 0.0f, 0.0f};
  transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  transform.scale = {1.0f, 1.0f, 1.0f};
  transform.dirty = true;
  return transform;
}

}  // namespace scene_asset_detail

inline bool ExpandSceneAssetReferences(vkpt::scene::SceneDocument& document,
                                       const std::filesystem::path& scene_path,
                                       SceneAssetExpansionStats* stats = nullptr,
                                       std::vector<std::string>* diagnostics = nullptr) {
  using namespace scene_asset_detail;

  const auto scene_dir = scene_path.has_parent_path()
                             ? scene_path.parent_path()
                             : std::filesystem::current_path();
  auto ids = CollectIds(document);
  SceneAssetExpansionStats local_stats{};
  const auto original_assets = document.assets;

  for (const auto& asset : original_assets) {
    if (!IsModelAsset(asset)) {
      continue;
    }

    const auto model_path = ResolvePath(scene_dir, asset.uri);
    const auto ext = detail::ExtensionOf(model_path.string());
    if (ext != ".obj") {
      if (diagnostics) {
        diagnostics->push_back("unsupported scene model asset: " + asset.uri);
      }
      return false;
    }
    auto loaded = LoadObj(model_path, diagnostics);
    if (loaded.geometry.empty()) {
      if (diagnostics) {
        diagnostics->push_back("model produced no geometry: " + asset.uri);
      }
      return false;
    }

    std::unordered_map<std::string, vkpt::core::StableId> material_id_by_name;
    std::uint64_t import_base = asset.id == 0 ? 900000u : asset.id * 1000u;
    std::uint32_t material_index = 0;
    for (const auto& material : loaded.materials) {
      vkpt::scene::SceneMaterialDefinition scene_material;
      scene_material.id = AllocateId(ids.materials, import_base + 100u + material_index);
      scene_material.name = material.name;
      scene_material.family = material.metallic > 0.5f ? "metallic_pbr" : "diffuse";
      scene_material.albedo = material.albedo;
      scene_material.roughness = Clamp(material.roughness, 0.0f, 1.0f);
      scene_material.metallic = Clamp(material.metallic, 0.0f, 1.0f);
      scene_material.alpha = Clamp(material.alpha, 0.0f, 1.0f);
      scene_material.double_sided = true;
      material_id_by_name[material.name] = scene_material.id;

      const std::vector<std::string> texture_slots = {
          material.base_color_texture,
          material.normal_texture,
          material.roughness_texture,
          material.metallic_texture,
      };
      std::uint32_t texture_slot = 0;
      for (const auto& texture_uri : texture_slots) {
        if (!texture_uri.empty() && IsTextureUri(texture_uri)) {
          const auto texture_path = ResolvePath(model_path.parent_path(), texture_uri);
          if (!AppendTextureAsset(document,
                                  ids,
                                  scene_dir,
                                  texture_path,
                                  material.name,
                                  import_base + 500u + material_index * 16u + texture_slot,
                                  diagnostics)) {
            return false;
          }
          ++local_stats.imported_textures;
        }
        ++texture_slot;
      }

      document.materials.push_back(std::move(scene_material));
      ++material_index;
      ++local_stats.imported_materials;
    }

    vkpt::scene::SceneEntityDefinition root;
    root.id = AllocateId(ids.entities, import_base + 1u);
    root.name = "Imported " + std::filesystem::path(asset.uri).stem().string();
    root.has_transform = true;
    root.transform = IdentityTransform();
    root.has_hierarchy = true;
    root.hierarchy.parent = 0;
    root.hierarchy.sibling_order = static_cast<std::uint32_t>(document.entities.size());
    document.entities.push_back(root);
    ++local_stats.imported_entities;

    std::uint32_t geometry_index = 0;
    for (const auto& bucket : loaded.geometry) {
      const auto material_it = material_id_by_name.find(bucket.material_name);
      const vkpt::core::StableId material_id =
          material_it == material_id_by_name.end() ? document.materials.front().id : material_it->second;

      vkpt::scene::SceneGeometryDefinition geometry;
      geometry.id = AllocateId(ids.geometry, import_base + 1000u + geometry_index);
      geometry.primitive = "triangle";
      geometry.tags = {"asset_model", "obj", asset.uri, bucket.material_name};
      geometry.material_id = material_id;
      geometry.vertices = bucket.vertices;
      geometry.indices = bucket.indices;
      document.geometry.push_back(std::move(geometry));

      vkpt::scene::SceneEntityDefinition entity;
      entity.id = AllocateId(ids.entities, import_base + 10000u + geometry_index);
      entity.name = std::filesystem::path(asset.uri).stem().string() + "_" + bucket.material_name;
      entity.has_transform = true;
      entity.transform = IdentityTransform();
      entity.has_mesh = true;
      entity.mesh.mesh_id = document.geometry.back().id;
      entity.mesh.material_id = material_id;
      entity.has_hierarchy = true;
      entity.hierarchy.parent = root.id;
      entity.hierarchy.sibling_order = geometry_index;
      document.entities.push_back(std::move(entity));

      ++geometry_index;
      ++local_stats.imported_geometry;
      ++local_stats.imported_entities;
    }

    ++local_stats.imported_models;
  }

  if (stats) {
    *stats = local_stats;
  }
  return true;
}

}  // namespace vkpt::assets
