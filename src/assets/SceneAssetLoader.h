#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

inline vkpt::scene::TransformComponent IdentityTransform();

struct IdSets {
  std::unordered_set<vkpt::core::StableId> assets;
  std::unordered_set<vkpt::core::StableId> materials;
  std::unordered_set<vkpt::core::StableId> geometry;
  std::unordered_set<vkpt::core::StableId> entities;
  std::unordered_set<std::string> asset_uris;
};

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
  std::vector<ObjMaterial> materials;
  std::vector<ObjGeometryBucket> geometry;
  std::vector<std::string> texture_uris;
  std::string source_format = "obj";
  bool has_root_transform = false;
  vkpt::scene::TransformComponent root_transform;
  vkpt::scene::AnimationComponent animation;
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
  try {
    const int value = std::stoi(std::string(index_text));
    int resolved = value > 0 ? value - 1 : static_cast<int>(texcoord_count) + value;
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= texcoord_count) {
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
  std::vector<vkpt::scene::Vec2> texcoords;
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
    } else if (words[0] == "vt" && words.size() >= 3) {
      vkpt::scene::Vec2 uv{};
      if (ParseFloat(words[1], &uv.u) && ParseFloat(words[2], &uv.v)) {
        texcoords.push_back(uv);
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

inline const vkpt::scene::JsonValue* JsonMember(const vkpt::scene::JsonValue& object,
                                                std::string_view key) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return nullptr;
  }
  const auto it = object.object.find(std::string(key));
  return it == object.object.end() ? nullptr : &it->second;
}

inline std::optional<std::size_t> JsonIndexMember(const vkpt::scene::JsonValue& object,
                                                  std::string_view key) {
  const auto* value = JsonMember(object, key);
  if (value == nullptr ||
      value->kind != vkpt::scene::JsonValue::Kind::Number ||
      !std::isfinite(value->number) ||
      value->number < 0.0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value->number);
}

inline std::optional<float> JsonFloatMember(const vkpt::scene::JsonValue& object,
                                            std::string_view key) {
  const auto* value = JsonMember(object, key);
  if (value == nullptr ||
      value->kind != vkpt::scene::JsonValue::Kind::Number ||
      !std::isfinite(value->number)) {
    return std::nullopt;
  }
  return static_cast<float>(value->number);
}

inline std::string JsonStringMember(const vkpt::scene::JsonValue& object,
                                    std::string_view key,
                                    std::string fallback = {}) {
  const auto* value = JsonMember(object, key);
  if (value == nullptr || value->kind != vkpt::scene::JsonValue::Kind::String) {
    return fallback;
  }
  return value->string;
}

inline bool JsonReadVec3Member(const vkpt::scene::JsonValue& object,
                               std::string_view key,
                               vkpt::scene::Vec3& out) {
  const auto* value = JsonMember(object, key);
  if (value == nullptr ||
      value->kind != vkpt::scene::JsonValue::Kind::Array ||
      value->array.size() < 3u) {
    return false;
  }
  for (std::size_t i = 0; i < 3u; ++i) {
    if (value->array[i].kind != vkpt::scene::JsonValue::Kind::Number ||
        !std::isfinite(value->array[i].number)) {
      return false;
    }
  }
  out.x = static_cast<float>(value->array[0].number);
  out.y = static_cast<float>(value->array[1].number);
  out.z = static_cast<float>(value->array[2].number);
  return true;
}

inline bool JsonReadQuatMember(const vkpt::scene::JsonValue& object,
                               std::string_view key,
                               vkpt::scene::Quat& out) {
  const auto* value = JsonMember(object, key);
  if (value == nullptr ||
      value->kind != vkpt::scene::JsonValue::Kind::Array ||
      value->array.size() < 4u) {
    return false;
  }
  for (std::size_t i = 0; i < 4u; ++i) {
    if (value->array[i].kind != vkpt::scene::JsonValue::Kind::Number ||
        !std::isfinite(value->array[i].number)) {
      return false;
    }
  }
  out.x = static_cast<float>(value->array[0].number);
  out.y = static_cast<float>(value->array[1].number);
  out.z = static_cast<float>(value->array[2].number);
  out.w = static_cast<float>(value->array[3].number);
  return true;
}

inline bool ReadFileBytes(const std::filesystem::path& path,
                          std::vector<std::uint8_t>& out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }
  const auto size = file.tellg();
  if (size < 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  file.seekg(0, std::ios::beg);
  if (!out.empty()) {
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  }
  return file.good() || file.eof();
}

struct GltfBufferView {
  std::size_t buffer = 0;
  std::size_t byte_offset = 0;
  std::size_t byte_length = 0;
  std::size_t byte_stride = 0;
};

struct GltfAccessor {
  std::size_t buffer_view = std::numeric_limits<std::size_t>::max();
  std::size_t byte_offset = 0;
  std::size_t count = 0;
  int component_type = 0;
  std::string type;
};

inline std::size_t GltfComponentCount(std::string_view type) {
  if (type == "SCALAR") {
    return 1u;
  }
  if (type == "VEC2") {
    return 2u;
  }
  if (type == "VEC3") {
    return 3u;
  }
  if (type == "VEC4") {
    return 4u;
  }
  return 0u;
}

inline std::size_t GltfComponentSize(int component_type) {
  switch (component_type) {
    case 5120:
    case 5121:
      return 1u;
    case 5122:
    case 5123:
      return 2u;
    case 5125:
    case 5126:
      return 4u;
    default:
      return 0u;
  }
}

template <typename T>
inline bool GltfReadScalar(const std::vector<std::uint8_t>& bytes,
                           std::size_t offset,
                           T& out) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

inline bool GltfReadAccessorFloats(const std::vector<GltfAccessor>& accessors,
                                   const std::vector<GltfBufferView>& views,
                                   const std::vector<std::vector<std::uint8_t>>& buffers,
                                   std::size_t accessor_index,
                                   std::vector<float>& out,
                                   std::size_t* out_components = nullptr) {
  out.clear();
  if (accessor_index >= accessors.size()) {
    return false;
  }
  const auto& accessor = accessors[accessor_index];
  if (accessor.buffer_view >= views.size() || accessor.component_type != 5126) {
    return false;
  }
  const auto& view = views[accessor.buffer_view];
  if (view.buffer >= buffers.size()) {
    return false;
  }
  const std::size_t components = GltfComponentCount(accessor.type);
  const std::size_t component_size = GltfComponentSize(accessor.component_type);
  if (components == 0u || component_size == 0u) {
    return false;
  }
  const std::size_t stride = view.byte_stride == 0u
      ? components * component_size
      : view.byte_stride;
  const auto& bytes = buffers[view.buffer];
  const std::size_t base = view.byte_offset + accessor.byte_offset;
  out.reserve(accessor.count * components);
  for (std::size_t item = 0; item < accessor.count; ++item) {
    const std::size_t item_offset = base + item * stride;
    for (std::size_t component = 0; component < components; ++component) {
      float value = 0.0f;
      if (!GltfReadScalar(bytes, item_offset + component * component_size, value) ||
          !std::isfinite(value)) {
        out.clear();
        return false;
      }
      out.push_back(value);
    }
  }
  if (out_components != nullptr) {
    *out_components = components;
  }
  return true;
}

inline bool GltfReadAccessorIndices(const std::vector<GltfAccessor>& accessors,
                                    const std::vector<GltfBufferView>& views,
                                    const std::vector<std::vector<std::uint8_t>>& buffers,
                                    std::size_t accessor_index,
                                    std::vector<std::uint32_t>& out) {
  out.clear();
  if (accessor_index >= accessors.size()) {
    return false;
  }
  const auto& accessor = accessors[accessor_index];
  if (accessor.buffer_view >= views.size() || GltfComponentCount(accessor.type) != 1u) {
    return false;
  }
  const auto& view = views[accessor.buffer_view];
  if (view.buffer >= buffers.size()) {
    return false;
  }
  const std::size_t component_size = GltfComponentSize(accessor.component_type);
  if (component_size == 0u) {
    return false;
  }
  const std::size_t stride = view.byte_stride == 0u ? component_size : view.byte_stride;
  const auto& bytes = buffers[view.buffer];
  const std::size_t base = view.byte_offset + accessor.byte_offset;
  out.reserve(accessor.count);
  for (std::size_t item = 0; item < accessor.count; ++item) {
    const std::size_t offset = base + item * stride;
    std::uint32_t value = 0u;
    if (accessor.component_type == 5121) {
      std::uint8_t raw = 0u;
      if (!GltfReadScalar(bytes, offset, raw)) {
        return false;
      }
      value = raw;
    } else if (accessor.component_type == 5123) {
      std::uint16_t raw = 0u;
      if (!GltfReadScalar(bytes, offset, raw)) {
        return false;
      }
      value = raw;
    } else if (accessor.component_type == 5125) {
      if (!GltfReadScalar(bytes, offset, value)) {
        return false;
      }
    } else {
      return false;
    }
    out.push_back(value);
  }
  return true;
}

inline ObjLoadResult LoadGltf(const std::filesystem::path& gltf_path,
                              std::vector<std::string>* diagnostics = nullptr) {
  ObjLoadResult result;
  result.source_format = "gltf";

  std::ifstream file(gltf_path);
  if (!file) {
    if (diagnostics) {
      diagnostics->push_back("glTF load failed: " + PathString(gltf_path));
    }
    return result;
  }
  std::ostringstream json_text;
  json_text << file.rdbuf();
  auto parsed = vkpt::scene::JsonParser::parse(json_text.str());
  if (!parsed || parsed->kind != vkpt::scene::JsonValue::Kind::Object) {
    if (diagnostics) {
      diagnostics->push_back("glTF JSON parse failed: " + PathString(gltf_path));
    }
    return result;
  }
  const auto& root = *parsed;
  const auto* buffers_node = JsonMember(root, "buffers");
  const auto* buffer_views_node = JsonMember(root, "bufferViews");
  const auto* accessors_node = JsonMember(root, "accessors");
  const auto* meshes_node = JsonMember(root, "meshes");
  if (buffers_node == nullptr || buffer_views_node == nullptr ||
      accessors_node == nullptr || meshes_node == nullptr ||
      buffers_node->kind != vkpt::scene::JsonValue::Kind::Array ||
      buffer_views_node->kind != vkpt::scene::JsonValue::Kind::Array ||
      accessors_node->kind != vkpt::scene::JsonValue::Kind::Array ||
      meshes_node->kind != vkpt::scene::JsonValue::Kind::Array) {
    if (diagnostics) {
      diagnostics->push_back("glTF missing buffers/accessors/meshes: " + PathString(gltf_path));
    }
    return result;
  }

  std::vector<std::vector<std::uint8_t>> buffers;
  buffers.reserve(buffers_node->array.size());
  for (const auto& buffer_node : buffers_node->array) {
    const auto uri = JsonStringMember(buffer_node, "uri");
    if (uri.empty() || uri.rfind("data:", 0u) == 0u) {
      if (diagnostics) {
        diagnostics->push_back("glTF scene import currently requires external .bin buffers");
      }
      return {};
    }
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(ResolvePath(gltf_path.parent_path(), uri), bytes)) {
      if (diagnostics) {
        diagnostics->push_back("glTF buffer load failed: " + uri);
      }
      return {};
    }
    buffers.push_back(std::move(bytes));
  }

  std::vector<GltfBufferView> buffer_views;
  buffer_views.reserve(buffer_views_node->array.size());
  for (const auto& view_node : buffer_views_node->array) {
    GltfBufferView view;
    view.buffer = JsonIndexMember(view_node, "buffer").value_or(0u);
    view.byte_offset = JsonIndexMember(view_node, "byteOffset").value_or(0u);
    view.byte_length = JsonIndexMember(view_node, "byteLength").value_or(0u);
    view.byte_stride = JsonIndexMember(view_node, "byteStride").value_or(0u);
    buffer_views.push_back(view);
  }

  std::vector<GltfAccessor> accessors;
  accessors.reserve(accessors_node->array.size());
  for (const auto& accessor_node : accessors_node->array) {
    GltfAccessor accessor;
    accessor.buffer_view = JsonIndexMember(accessor_node, "bufferView")
                               .value_or(std::numeric_limits<std::size_t>::max());
    accessor.byte_offset = JsonIndexMember(accessor_node, "byteOffset").value_or(0u);
    accessor.count = JsonIndexMember(accessor_node, "count").value_or(0u);
    accessor.component_type = static_cast<int>(JsonIndexMember(accessor_node, "componentType").value_or(0u));
    accessor.type = JsonStringMember(accessor_node, "type");
    accessors.push_back(std::move(accessor));
  }

  const auto* images_node = JsonMember(root, "images");
  const auto* textures_node = JsonMember(root, "textures");
  auto texture_uri = [&](std::size_t texture_index) -> std::string {
    if (textures_node == nullptr ||
        textures_node->kind != vkpt::scene::JsonValue::Kind::Array ||
        texture_index >= textures_node->array.size()) {
      return {};
    }
    const auto source = JsonIndexMember(textures_node->array[texture_index], "source");
    if (!source ||
        images_node == nullptr ||
        images_node->kind != vkpt::scene::JsonValue::Kind::Array ||
        *source >= images_node->array.size()) {
      return {};
    }
    return JsonStringMember(images_node->array[*source], "uri");
  };

  const auto* materials_node = JsonMember(root, "materials");
  if (materials_node != nullptr && materials_node->kind == vkpt::scene::JsonValue::Kind::Array) {
    for (std::size_t index = 0; index < materials_node->array.size(); ++index) {
      const auto& material_node = materials_node->array[index];
      ObjMaterial material;
      material.name = JsonStringMember(material_node,
                                       "name",
                                       "gltf_material_" + std::to_string(index));
      material.family = "gltf_pbr";
      material.double_sided = true;
      if (const auto* pbr = JsonMember(material_node, "pbrMetallicRoughness")) {
        if (const auto* factor = JsonMember(*pbr, "baseColorFactor");
            factor != nullptr &&
            factor->kind == vkpt::scene::JsonValue::Kind::Array &&
            factor->array.size() >= 3u) {
          for (std::size_t component = 0; component < 3u; ++component) {
            if (factor->array[component].kind != vkpt::scene::JsonValue::Kind::Number) {
              continue;
            }
            const float value = Clamp(static_cast<float>(factor->array[component].number), 0.0f, 1.0f);
            if (component == 0u) {
              material.albedo.x = value;
            } else if (component == 1u) {
              material.albedo.y = value;
            } else {
              material.albedo.z = value;
            }
          }
        }
        if (const auto roughness = JsonFloatMember(*pbr, "roughnessFactor")) {
          material.roughness = Clamp(*roughness, 0.0f, 1.0f);
        }
        if (const auto metallic = JsonFloatMember(*pbr, "metallicFactor")) {
          material.metallic = Clamp(*metallic, 0.0f, 1.0f);
        }
        if (const auto* base_texture = JsonMember(*pbr, "baseColorTexture")) {
          if (const auto texture_index = JsonIndexMember(*base_texture, "index")) {
            material.base_color_texture = texture_uri(*texture_index);
            if (!material.base_color_texture.empty()) {
              result.texture_uris.push_back(material.base_color_texture);
            }
          }
        }
      }
      result.materials.push_back(std::move(material));
    }
  }
  if (result.materials.empty()) {
    result.materials.push_back(ObjMaterial{});
  }

  for (std::size_t mesh_index = 0; mesh_index < meshes_node->array.size(); ++mesh_index) {
    const auto& mesh_node = meshes_node->array[mesh_index];
    const auto mesh_name = JsonStringMember(mesh_node, "name", "gltf_mesh_" + std::to_string(mesh_index));
    const auto* primitives_node = JsonMember(mesh_node, "primitives");
    if (primitives_node == nullptr ||
        primitives_node->kind != vkpt::scene::JsonValue::Kind::Array) {
      continue;
    }
    for (std::size_t primitive_index = 0; primitive_index < primitives_node->array.size(); ++primitive_index) {
      const auto& primitive_node = primitives_node->array[primitive_index];
      if (JsonIndexMember(primitive_node, "mode").value_or(4u) != 4u) {
        continue;
      }
      const auto* attributes = JsonMember(primitive_node, "attributes");
      if (attributes == nullptr) {
        continue;
      }
      const auto position_accessor = JsonIndexMember(*attributes, "POSITION");
      if (!position_accessor) {
        continue;
      }
      std::vector<float> positions;
      std::size_t position_components = 0u;
      if (!GltfReadAccessorFloats(accessors,
                                  buffer_views,
                                  buffers,
                                  *position_accessor,
                                  positions,
                                  &position_components) ||
          position_components != 3u) {
        continue;
      }
      std::vector<float> texcoord_values;
      std::size_t texcoord_components = 0u;
      const bool has_texcoords =
          JsonIndexMember(*attributes, "TEXCOORD_0").has_value() &&
          GltfReadAccessorFloats(accessors,
                                 buffer_views,
                                 buffers,
                                 *JsonIndexMember(*attributes, "TEXCOORD_0"),
                                 texcoord_values,
                                 &texcoord_components) &&
          texcoord_components == 2u &&
          texcoord_values.size() / 2u == positions.size() / 3u;

      std::vector<std::uint32_t> indices;
      if (const auto index_accessor = JsonIndexMember(primitive_node, "indices")) {
        if (!GltfReadAccessorIndices(accessors,
                                     buffer_views,
                                     buffers,
                                     *index_accessor,
                                     indices)) {
          continue;
        }
      } else {
        indices.resize(positions.size() / 3u);
        for (std::size_t i = 0; i < indices.size(); ++i) {
          indices[i] = static_cast<std::uint32_t>(i);
        }
      }
      if (indices.empty() || indices.size() % 3u != 0u) {
        continue;
      }

      ObjGeometryBucket bucket;
      const std::size_t material_index = JsonIndexMember(primitive_node, "material").value_or(0u);
      bucket.material_name = material_index < result.materials.size()
          ? result.materials[material_index].name
          : result.materials.front().name;
      if (primitive_index > 0u) {
        bucket.material_name += "_" + std::to_string(primitive_index);
      }
      bucket.vertices.reserve(positions.size() / 3u);
      bucket.texcoords.reserve(positions.size() / 3u);
      for (std::size_t i = 0; i + 2u < positions.size(); i += 3u) {
        bucket.vertices.push_back({positions[i], positions[i + 1u], positions[i + 2u]});
        if (has_texcoords) {
          const std::size_t uv_index = (i / 3u) * 2u;
          bucket.texcoords.push_back({texcoord_values[uv_index], texcoord_values[uv_index + 1u]});
        }
      }
      bucket.indices = std::move(indices);
      if (bucket.texcoords.size() != bucket.vertices.size()) {
        bucket.texcoords.clear();
      }
      (void)mesh_name;
      result.geometry.push_back(std::move(bucket));
    }
  }

  const auto* nodes_node = JsonMember(root, "nodes");
  std::size_t root_node_index = 0u;
  if (const auto* scenes_node = JsonMember(root, "scenes");
      scenes_node != nullptr &&
      scenes_node->kind == vkpt::scene::JsonValue::Kind::Array &&
      !scenes_node->array.empty()) {
    const std::size_t scene_index = JsonIndexMember(root, "scene").value_or(0u);
    if (scene_index < scenes_node->array.size()) {
      if (const auto* scene_nodes = JsonMember(scenes_node->array[scene_index], "nodes");
          scene_nodes != nullptr &&
          scene_nodes->kind == vkpt::scene::JsonValue::Kind::Array &&
          !scene_nodes->array.empty() &&
          scene_nodes->array[0].kind == vkpt::scene::JsonValue::Kind::Number) {
        root_node_index = static_cast<std::size_t>(scene_nodes->array[0].number);
      }
    }
  }
  if (nodes_node != nullptr &&
      nodes_node->kind == vkpt::scene::JsonValue::Kind::Array &&
      root_node_index < nodes_node->array.size()) {
    result.root_transform = IdentityTransform();
    const auto& node = nodes_node->array[root_node_index];
    JsonReadVec3Member(node, "translation", result.root_transform.translation);
    JsonReadQuatMember(node, "rotation", result.root_transform.rotation);
    JsonReadVec3Member(node, "scale", result.root_transform.scale);
    result.has_root_transform = true;
  }

  const auto* animations_node = JsonMember(root, "animations");
  if (animations_node != nullptr &&
      animations_node->kind == vkpt::scene::JsonValue::Kind::Array &&
      !animations_node->array.empty()) {
    const auto& animation_node = animations_node->array.front();
    result.animation.clip = JsonStringMember(animation_node, "name", "gltf_animation");
    result.animation.looping = true;
    result.animation.duration_seconds = 1.0f;
    result.animation.playback_speed = 1.0f;
    bool has_motion = false;
    if (const auto* samplers = JsonMember(animation_node, "samplers");
        samplers != nullptr && samplers->kind == vkpt::scene::JsonValue::Kind::Array) {
      for (const auto& sampler : samplers->array) {
        if (const auto input = JsonIndexMember(sampler, "input")) {
          std::vector<float> times;
          std::size_t components = 0u;
          if (GltfReadAccessorFloats(accessors, buffer_views, buffers, *input, times, &components) &&
              components == 1u && !times.empty()) {
            const auto max_time = *std::max_element(times.begin(), times.end());
            if (std::isfinite(max_time) && max_time > 0.0f) {
              result.animation.duration_seconds =
                  std::max(result.animation.duration_seconds, max_time);
            }
          }
        }
      }
    }
    if (const auto* channels = JsonMember(animation_node, "channels");
        channels != nullptr && channels->kind == vkpt::scene::JsonValue::Kind::Array) {
      for (const auto& channel : channels->array) {
        const auto* target = JsonMember(channel, "target");
        const auto path = target == nullptr ? std::string{} : JsonStringMember(*target, "path");
        if (path == "rotation") {
          result.animation.rotation_degrees.y = 360.0f;
          has_motion = true;
        } else if (path == "translation") {
          result.animation.translation_amplitude.y = 0.25f;
          has_motion = true;
        } else if (path == "scale") {
          result.animation.scale_amplitude = {0.1f, 0.1f, 0.1f};
          has_motion = true;
        }
      }
    }
    if (!has_motion) {
      result.animation.clip.clear();
    }
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
  if (result.geometry.empty() && diagnostics) {
    diagnostics->push_back("glTF has no renderable triangle primitives: " + PathString(gltf_path));
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
    std::uint64_t import_base = asset.id == 0 ? 900000u : asset.id * 1000u;
    std::uint32_t material_index = 0;
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
    root.transform = loaded.has_root_transform ? loaded.root_transform : IdentityTransform();
    root.has_hierarchy = true;
    root.hierarchy.parent = 0;
    root.hierarchy.sibling_order = static_cast<std::uint32_t>(document.entities.size());
    if (!loaded.animation.clip.empty()) {
      root.animation = loaded.animation;
    }
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
