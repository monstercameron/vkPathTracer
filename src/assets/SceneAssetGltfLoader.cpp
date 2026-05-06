#include "assets/SceneAssetLoaderInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene/Json.h"

namespace vkpt::assets::scene_asset_detail {

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

ObjLoadResult LoadGltf(const std::filesystem::path& gltf_path,
                         std::vector<std::string>* diagnostics) {
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

}  // namespace vkpt::assets::scene_asset_detail