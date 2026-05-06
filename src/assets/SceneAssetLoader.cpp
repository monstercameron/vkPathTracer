#include "assets/SceneAssetLoader.h"
#include "assets/SceneAssetLoaderInternal.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/AssetImporters.h"

namespace vkpt::assets {

namespace scene_asset_detail {

struct IdSets {
  std::unordered_set<vkpt::core::StableId> assets;
  std::unordered_set<vkpt::core::StableId> materials;
  std::unordered_set<vkpt::core::StableId> geometry;
  std::unordered_set<vkpt::core::StableId> entities;
  std::unordered_set<std::string> asset_uris;
};

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
  out.assets.reserve(document.assets.size());
  out.asset_uris.reserve(document.assets.size());
  out.materials.reserve(document.materials.size());
  out.geometry.reserve(document.geometry.size());
  out.entities.reserve(document.entities.size());
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
         type == "gltf" || type == "model/gltf" ||
         ext == ".obj" || ext == ".gltf";
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
  (void)scene_dir;
  // Texture assets are appended once by normalized URI; material bindings keep their resolved paths.
  const auto texture_uri = PathString(texture_path);
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

}  // namespace scene_asset_detail


bool ImportModelAsset(vkpt::scene::SceneDocument& document,
                      scene_asset_detail::IdSets& ids,
                      const std::filesystem::path& scene_dir,
                      const vkpt::scene::SceneAssetDefinition& asset,
                      const vkpt::scene::TransformComponent* root_transform_override,
                      SceneAssetExpansionStats& local_stats,
                      std::vector<std::string>* diagnostics) {
  using namespace scene_asset_detail;

  const auto model_path = ResolvePath(scene_dir, asset.uri);
  const auto ext = detail::ExtensionOf(model_path.string());
  ObjLoadResult loaded;
  if (ext == ".obj") {
    loaded = LoadObj(model_path, diagnostics);
  } else if (ext == ".gltf") {
    loaded = LoadGltf(model_path, diagnostics);
  } else {
    if (diagnostics) {
      diagnostics->push_back("unsupported scene model asset: " + asset.uri);
    }
    return false;
  }
  if (loaded.geometry.empty()) {
    if (diagnostics) {
      diagnostics->push_back("model produced no geometry: " + asset.uri);
    }
    return false;
  }

  std::unordered_map<std::string, vkpt::core::StableId> material_id_by_name;
  material_id_by_name.reserve(loaded.materials.size());
  document.materials.reserve(document.materials.size() + loaded.materials.size() + 1u);
  document.geometry.reserve(document.geometry.size() + loaded.geometry.size());
  document.entities.reserve(document.entities.size() + loaded.geometry.size() + 1u);
  std::uint64_t import_base = asset.id == 0 ? 900000u : asset.id * 1000u;
  std::uint32_t material_index = 0;
  // Imported material IDs are derived from the source asset ID to keep repeated imports predictable.
  for (const auto& material : loaded.materials) {
    vkpt::scene::SceneMaterialDefinition scene_material;
    scene_material.id = AllocateId(ids.materials, import_base + 100u + material_index);
    scene_material.name = material.name;
    scene_material.family = material.family.empty()
                                ? (material.metallic > 0.5f ? "metallic_pbr" : "diffuse")
                                : material.family;
    scene_material.albedo = material.albedo;
    scene_material.roughness = Clamp(material.roughness, 0.0f, 1.0f);
    scene_material.metallic = Clamp(material.metallic, 0.0f, 1.0f);
    scene_material.ior = Clamp(material.ior, 1.0f, 4.0f);
    scene_material.transmission = Clamp(material.transmission, 0.0f, 1.0f);
    scene_material.clearcoat = Clamp(material.clearcoat, 0.0f, 1.0f);
    scene_material.sheen = Clamp(material.sheen, 0.0f, 1.0f);
    scene_material.anisotropy = Clamp(material.anisotropy, -1.0f, 1.0f);
    scene_material.alpha = Clamp(material.alpha, 0.0f, 1.0f);
    scene_material.emission = material.emission;
    scene_material.emission_intensity = Clamp(material.emission_intensity, 0.0f, 128.0f);
    scene_material.double_sided = material.double_sided;
    if (!material.base_color_texture.empty() && IsTextureUri(material.base_color_texture)) {
      scene_material.base_color_texture =
          PathString(ResolvePath(model_path.parent_path(), material.base_color_texture));
    }
    if (!material.normal_texture.empty()) {
      scene_material.normal_texture =
          PathString(ResolvePath(model_path.parent_path(), material.normal_texture));
    }
    material_id_by_name[material.name] = scene_material.id;

    const std::array<std::string_view, 4> texture_slots = {
        std::string_view(material.base_color_texture),
        std::string_view(material.normal_texture),
        std::string_view(material.roughness_texture),
        std::string_view(material.metallic_texture),
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
  vkpt::core::StableId fallback_material_id = 0u;
  if (!loaded.materials.empty()) {
    const auto fallback_it = material_id_by_name.find(loaded.materials.front().name);
    if (fallback_it != material_id_by_name.end()) {
      fallback_material_id = fallback_it->second;
    }
  }
  if (fallback_material_id == 0u && !document.materials.empty()) {
    fallback_material_id = document.materials.front().id;
  }
  if (fallback_material_id == 0u) {
    vkpt::scene::SceneMaterialDefinition fallback_material;
    fallback_material.id = AllocateId(ids.materials, import_base + 99u);
    fallback_material.name = "asset_default";
    fallback_material.family = "diffuse";
    fallback_material_id = fallback_material.id;
    document.materials.push_back(std::move(fallback_material));
    ++local_stats.imported_materials;
  }

  vkpt::scene::SceneEntityDefinition root;
  root.id = AllocateId(ids.entities, import_base + 1u);
  root.name = asset.name.empty()
                  ? "Imported " + std::filesystem::path(asset.uri).stem().string()
                  : asset.name;
  root.has_transform = true;
  root.transform = loaded.has_root_transform ? loaded.root_transform : IdentityTransform();
  if (asset.has_transform) {
    root.transform = asset.transform;
  }
  if (root_transform_override != nullptr) {
    root.transform.translation = root_transform_override->translation;
    root.transform.dirty = true;
  }
  root.has_hierarchy = true;
  root.hierarchy.parent = asset.parent;
  root.hierarchy.sibling_order = asset.parent == 0
                                      ? static_cast<std::uint32_t>(document.entities.size())
                                      : asset.sibling_order;
  if (!asset.disable_imported_animation && !loaded.animation.clip.empty()) {
    root.animation = loaded.animation;
  }
  const auto root_id = root.id;
  document.entities.push_back(std::move(root));
  local_stats.imported_root_entity = root_id;
  ++local_stats.imported_entities;

  std::uint32_t geometry_index = 0;
  // Each geometry bucket becomes a child mesh entity under one imported root entity.
  for (const auto& bucket : loaded.geometry) {
    const auto material_it = material_id_by_name.find(bucket.material_name);
    const vkpt::core::StableId material_id =
        material_it == material_id_by_name.end() ? fallback_material_id : material_it->second;

    vkpt::scene::SceneGeometryDefinition geometry;
    geometry.id = AllocateId(ids.geometry, import_base + 1000u + geometry_index);
    geometry.primitive = "triangle";
    geometry.tags = {"asset_model", loaded.source_format, asset.uri, bucket.material_name};
    geometry.material_id = material_id;
    geometry.vertices = bucket.vertices;
    geometry.indices = bucket.indices;
    if (bucket.texcoords.size() == bucket.vertices.size()) {
      geometry.texcoords = bucket.texcoords;
    }
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
    entity.hierarchy.parent = root_id;
    entity.hierarchy.sibling_order = geometry_index;
    document.entities.push_back(std::move(entity));

    ++geometry_index;
    ++local_stats.imported_geometry;
    ++local_stats.imported_entities;
  }

  ++local_stats.imported_models;
  return true;
}

bool ExpandSceneAssetReferences(vkpt::scene::SceneDocument& document,
                                const std::filesystem::path& scene_path,
                                SceneAssetExpansionStats* stats,
                                std::vector<std::string>* diagnostics) {
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
    if (!ImportModelAsset(document, ids, scene_dir, asset, nullptr, local_stats, diagnostics)) {
      return false;
    }
  }

  if (stats) {
    *stats = local_stats;
  }
  return true;
}

bool ImportSceneModelAsset(vkpt::scene::SceneDocument& document,
                           const std::filesystem::path& scene_path,
                           std::string_view model_uri,
                           const vkpt::scene::TransformComponent& root_transform,
                           SceneAssetExpansionStats* stats,
                           std::vector<std::string>* diagnostics) {
  using namespace scene_asset_detail;

  auto ids = CollectIds(document);
  const auto asset_count = document.assets.size();
  const auto material_count = document.materials.size();
  const auto geometry_count = document.geometry.size();
  const auto entity_count = document.entities.size();
  vkpt::scene::SceneAssetDefinition asset;
  asset.id = AllocateId(ids.assets, 800000u + static_cast<vkpt::core::StableId>(document.assets.size()));
  asset.type = "model";
  asset.uri = std::string(model_uri);
  ids.asset_uris.insert(asset.uri);
  document.assets.push_back(asset);

  const auto scene_dir = scene_path.has_parent_path()
                             ? scene_path.parent_path()
                             : std::filesystem::current_path();
  SceneAssetExpansionStats local_stats{};
  if (!ImportModelAsset(document, ids, scene_dir, asset, &root_transform, local_stats, diagnostics)) {
    document.assets.resize(asset_count);
    document.materials.resize(material_count);
    document.geometry.resize(geometry_count);
    document.entities.resize(entity_count);
    return false;
  }
  if (stats) {
    *stats = local_stats;
  }
  return true;
}

}  // namespace vkpt::assets
