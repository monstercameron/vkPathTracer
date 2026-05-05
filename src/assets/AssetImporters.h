#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assets/AssetRegistry.h"
#include "assets/TextureAsset.h"

namespace vkpt::assets {

enum class ImportDiagnosticSeverity : std::uint8_t {
  Info,
  Warning,
  Error,
};

struct AssetImportDiagnostic {
  ImportDiagnosticSeverity severity = ImportDiagnosticSeverity::Info;
  std::string code;
  std::string message;
  bool lossy_conversion = false;
};

struct AssetValidationResult {
  bool valid = false;
  std::string reason;
  std::vector<AssetImportDiagnostic> diagnostics;
};

struct AssetImportSource {
  std::string uri;
  std::vector<std::byte> bytes;
  std::string root_directory;
  std::string binding_context;
};

struct AssetImportOptions {
  bool metadata_only = true;
  bool allow_lossy_conversions = true;
  bool infer_texture_semantic_from_uri = true;
  TextureChannelSemantic texture_semantic = TextureChannelSemantic::Unknown;
  TextureColorSpace texture_color_space = TextureColorSpace::Unknown;
  std::unordered_map<std::string, std::string> sidecar_text_by_uri;
};

struct AssetImportResult {
  bool success = false;
  std::vector<AssetRecord> assets;
  std::vector<AssetImportDiagnostic> diagnostics;
  std::string deterministic_import_hash;
};

class IAssetImporter {
 public:
  virtual ~IAssetImporter() = default;

  [[nodiscard]] virtual std::string_view importer_id() const = 0;
  [[nodiscard]] virtual std::vector<std::string_view> supported_extensions() const = 0;
  [[nodiscard]] virtual std::vector<std::string_view> supported_features() const = 0;
  [[nodiscard]] virtual AssetValidationResult validate_source(const AssetImportSource& source,
                                                              const AssetImportOptions& options) const = 0;
  [[nodiscard]] virtual AssetImportResult import_source(const AssetImportSource& source,
                                                        const AssetImportOptions& options) const = 0;
  [[nodiscard]] virtual std::vector<AssetImportDiagnostic> emit_diagnostics() const = 0;
};

namespace detail {

inline std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

inline std::string Trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

inline std::string ExtensionOf(std::string_view uri) {
  const auto path = std::filesystem::path(std::string(uri));
  return ToLower(path.extension().string());
}

inline bool HasExtension(std::string_view uri, const std::vector<std::string_view>& extensions) {
  const auto ext = ExtensionOf(uri);
  for (const auto candidate : extensions) {
    if (ext == candidate) {
      return true;
    }
  }
  return false;
}

inline std::uint8_t Byte(const std::vector<std::byte>& bytes, std::size_t index) {
  return std::to_integer<std::uint8_t>(bytes[index]);
}

inline std::uint32_t ReadU32Le(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(Byte(bytes, offset)) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 1)) << 8u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 2)) << 16u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 3)) << 24u);
}

inline std::uint32_t ReadU32Be(const std::vector<std::byte>& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(Byte(bytes, offset)) << 24u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 1)) << 16u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 2)) << 8u) |
         static_cast<std::uint32_t>(Byte(bytes, offset + 3));
}

inline std::uint16_t ReadU16Be(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Byte(bytes, offset)) << 8u) |
                                    static_cast<std::uint16_t>(Byte(bytes, offset + 1)));
}

inline std::vector<std::byte> ReadFileBytes(std::string_view uri) {
  std::ifstream file(std::filesystem::path(std::string(uri)), std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size <= 0) {
    return {};
  }
  file.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  file.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

inline std::vector<std::byte> ResolveSourceBytes(const AssetImportSource& source) {
  if (!source.bytes.empty()) {
    return source.bytes;
  }
  if (!source.uri.empty()) {
    return ReadFileBytes(source.uri);
  }
  return {};
}

inline std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text;
  text.resize(bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    text[i] = static_cast<char>(Byte(bytes, i));
  }
  return text;
}

inline std::string HashBytesHex(const std::vector<std::byte>& bytes) {
  if (bytes.empty()) {
    return HashTextHex("");
  }
  return Hex64(Fnv1a64Bytes(bytes.data(), bytes.size()));
}

inline AssetImportDiagnostic Diagnostic(ImportDiagnosticSeverity severity,
                                        std::string code,
                                        std::string message,
                                        bool lossy = false) {
  return {severity, std::move(code), std::move(message), lossy};
}

inline std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
    if (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n') {
      start = end + 2;
    }
  }
  return lines;
}

inline std::vector<std::string> SplitWords(std::string_view text) {
  std::istringstream input{std::string(text)};
  std::vector<std::string> words;
  std::string word;
  while (input >> word) {
    words.push_back(word);
  }
  return words;
}

inline std::optional<std::string_view> ExtractNamedArray(std::string_view json, std::string_view name) {
  const std::string key = "\"" + std::string(name) + "\"";
  const auto key_pos = json.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  const auto colon = json.find(':', key_pos + key.size());
  if (colon == std::string_view::npos) {
    return {};
  }
  const auto begin = json.find('[', colon + 1);
  if (begin == std::string_view::npos) {
    return {};
  }
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = begin; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
      if (depth == 0) {
        return json.substr(begin, i - begin + 1);
      }
    }
  }
  return {};
}

inline std::vector<std::string_view> TopLevelObjects(std::string_view array_text) {
  std::vector<std::string_view> out;
  std::size_t object_begin = std::string_view::npos;
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < array_text.size(); ++i) {
    const char c = array_text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '{') {
      if (depth == 0) {
        object_begin = i;
      }
      ++depth;
    } else if (c == '}') {
      if (depth > 0) {
        --depth;
      }
      if (depth == 0 && object_begin != std::string_view::npos) {
        out.push_back(array_text.substr(object_begin, i - object_begin + 1));
        object_begin = std::string_view::npos;
      }
    }
  }
  return out;
}

inline std::optional<std::string> ExtractStringValue(std::string_view object_text, std::string_view name) {
  const std::string key = "\"" + std::string(name) + "\"";
  const auto key_pos = object_text.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  const auto colon = object_text.find(':', key_pos + key.size());
  if (colon == std::string_view::npos) {
    return {};
  }
  const auto quote = object_text.find('"', colon + 1);
  if (quote == std::string_view::npos) {
    return {};
  }
  std::string out;
  bool escaped = false;
  for (std::size_t i = quote + 1; i < object_text.size(); ++i) {
    const char c = object_text[i];
    if (escaped) {
      out.push_back(c);
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      return out;
    }
    out.push_back(c);
  }
  return {};
}

inline std::vector<std::string> ExtractObjectNames(std::string_view json, std::string_view array_name) {
  std::vector<std::string> names;
  const auto array = ExtractNamedArray(json, array_name);
  if (!array) {
    return names;
  }
  for (const auto object : TopLevelObjects(*array)) {
    if (auto name = ExtractStringValue(object, "name")) {
      names.push_back(*name);
    } else {
      names.emplace_back();
    }
  }
  return names;
}

inline std::vector<std::string> ExtractImageUris(std::string_view json) {
  std::vector<std::string> uris;
  const auto array = ExtractNamedArray(json, "images");
  if (!array) {
    return uris;
  }
  for (const auto object : TopLevelObjects(*array)) {
    if (auto uri = ExtractStringValue(object, "uri")) {
      uris.push_back(*uri);
    } else {
      uris.emplace_back();
    }
  }
  return uris;
}

inline TextureChannelSemantic InferTextureSemantic(std::string_view uri, std::string_view context) {
  const auto key = ToLower(std::string(uri) + " " + std::string(context));
  if (key.find("normal") != std::string::npos || key.find("_nrm") != std::string::npos) {
    return TextureChannelSemantic::Normal;
  }
  if (key.find("rough") != std::string::npos) {
    return TextureChannelSemantic::Roughness;
  }
  if (key.find("metal") != std::string::npos) {
    return TextureChannelSemantic::Metallic;
  }
  if (key.find("occlusion") != std::string::npos || key.find("_ao") != std::string::npos) {
    return TextureChannelSemantic::Occlusion;
  }
  if (key.find("emissive") != std::string::npos || key.find("emission") != std::string::npos) {
    return TextureChannelSemantic::Emissive;
  }
  if (key.find("alpha") != std::string::npos || key.find("opacity") != std::string::npos) {
    return TextureChannelSemantic::Alpha;
  }
  if (key.find("height") != std::string::npos || key.find("displace") != std::string::npos) {
    return TextureChannelSemantic::Height;
  }
  return TextureChannelSemantic::BaseColor;
}

inline std::string FileNameOrUri(std::string_view uri) {
  const auto path = std::filesystem::path(std::string(uri));
  const auto name = path.filename().string();
  return name.empty() ? std::string(uri) : name;
}

}  // namespace detail

class ImporterRegistry {
 public:
  bool register_importer(std::shared_ptr<IAssetImporter> importer) {
    if (!importer || importer->importer_id().empty()) {
      return false;
    }
    for (const auto& existing : m_importers) {
      if (existing->importer_id() == importer->importer_id()) {
        return false;
      }
    }
    m_importers.push_back(std::move(importer));
    return true;
  }

  [[nodiscard]] const IAssetImporter* importer_for_extension(std::string_view extension) const {
    const auto normalized = detail::ToLower(extension);
    for (const auto& importer : m_importers) {
      for (const auto ext : importer->supported_extensions()) {
        if (normalized == ext) {
          return importer.get();
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const IAssetImporter* importer_for_source(const AssetImportSource& source) const {
    return importer_for_extension(detail::ExtensionOf(source.uri));
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options = {}) const {
    const auto* importer = importer_for_source(source);
    if (!importer) {
      return {false, "no importer registered for extension " + detail::ExtensionOf(source.uri), {}};
    }
    return importer->validate_source(source, options);
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options = {}) const {
    const auto* importer = importer_for_source(source);
    if (!importer) {
      AssetImportResult result;
      result.success = false;
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "asset.no_importer",
                                                      "no importer registered for extension " + detail::ExtensionOf(source.uri)));
      return result;
    }
    return importer->import_source(source, options);
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const {
    std::vector<std::string_view> out;
    for (const auto& importer : m_importers) {
      const auto extensions = importer->supported_extensions();
      out.insert(out.end(), extensions.begin(), extensions.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  [[nodiscard]] const std::vector<std::shared_ptr<IAssetImporter>>& importers() const {
    return m_importers;
  }

 private:
  std::vector<std::shared_ptr<IAssetImporter>> m_importers;
};

class FakeAssetImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override {
    return "fake";
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override {
    return {".fake"};
  }

  [[nodiscard]] std::vector<std::string_view> supported_features() const override {
    return {"unit_test", "metadata_only"};
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const override {
    if (!detail::HasExtension(source.uri, supported_extensions())) {
      return {false, "unsupported extension", {}};
    }
    if (source.uri.empty() && source.bytes.empty()) {
      return {false, "fake importer needs a uri or bytes", {}};
    }
    return {true, "", {}};
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override {
    AssetImportResult result;
    auto validation = validate_source(source, options);
    if (!validation.valid) {
      result.diagnostics = std::move(validation.diagnostics);
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "fake.invalid",
                                                      validation.reason));
      return result;
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    const auto source_hash = bytes.empty() ? HashTextHex(source.uri) : detail::HashBytesHex(bytes);
    AssetRecord record;
    record.asset_class = AssetClass::Scene;
    record.name = detail::FileNameOrUri(source.uri);
    record.source_uri = source.uri;
    record.source_hash = source_hash;
    record.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash);
    record.tags = {"fake", "test"};
    record.metadata = {{"importer", "fake"}};
    result.assets.push_back(std::move(record));
    result.deterministic_import_hash = HashTextHex(std::string("fake:") + source.uri + ":" + source_hash);
    result.success = true;
    return result;
  }

  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override {
    return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                               "fake.ready",
                               "fake metadata importer is available for importer interface tests")};
  }
};

class GltfGlbImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override {
    return "gltf_glb_mvp";
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override {
    return {".gltf", ".glb"};
  }

  [[nodiscard]] std::vector<std::string_view> supported_features() const override {
    return {"gltf2", "glb", "static_mesh_metadata", "material_metadata", "texture_references"};
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const override {
    if (!detail::HasExtension(source.uri, supported_extensions())) {
      return {false, "unsupported extension", {}};
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    if (bytes.empty()) {
      return {false, "source bytes unavailable", {}};
    }
    const auto ext = detail::ExtensionOf(source.uri);
    if (ext == ".glb") {
      if (bytes.size() < 20) {
        return {false, "GLB header is too small", {}};
      }
      if (detail::ReadU32Le(bytes, 0) != 0x46546c67u) {
        return {false, "missing GLB magic", {}};
      }
      if (detail::ReadU32Le(bytes, 4) != 2u) {
        return {false, "only GLB version 2 is supported", {}};
      }
      if (detail::ReadU32Le(bytes, 8) > bytes.size()) {
        return {false, "GLB declared length exceeds source size", {}};
      }
      if (detail::ReadU32Le(bytes, 16) != 0x4e4f534au) {
        return {false, "first GLB chunk is not JSON", {}};
      }
      return {true, "", {}};
    }
    const auto text = detail::BytesToString(bytes);
    if (text.find("\"asset\"") == std::string::npos || text.find("\"version\"") == std::string::npos) {
      return {false, "gltf JSON must contain asset.version", {}};
    }
    return {true, "", {}};
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override {
    AssetImportResult result;
    auto validation = validate_source(source, options);
    if (!validation.valid) {
      result.diagnostics = std::move(validation.diagnostics);
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "gltf.invalid",
                                                      validation.reason));
      return result;
    }

    const auto bytes = detail::ResolveSourceBytes(source);
    const auto source_hash = detail::HashBytesHex(bytes);
    const auto json = extract_json(bytes, detail::ExtensionOf(source.uri) == ".glb");
    const auto mesh_names = detail::ExtractObjectNames(json, "meshes");
    const auto material_names = detail::ExtractObjectNames(json, "materials");
    const auto image_uris = detail::ExtractImageUris(json);
    const auto texture_array = detail::ExtractNamedArray(json, "textures");
    const auto node_array = detail::ExtractNamedArray(json, "nodes");
    const auto texture_count = texture_array ? detail::TopLevelObjects(*texture_array).size() : image_uris.size();
    const auto node_count = node_array ? detail::TopLevelObjects(*node_array).size() : 0u;

    AssetRecord scene;
    scene.asset_class = AssetClass::Scene;
    scene.name = detail::FileNameOrUri(source.uri);
    scene.source_uri = source.uri;
    scene.source_hash = source_hash;
    scene.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash + "#scene");
    scene.tags = {"gltf", "scene"};
    scene.metadata = {
        {"importer", "gltf_glb_mvp"},
        {"mesh_count", std::to_string(mesh_names.size())},
        {"material_count", std::to_string(material_names.size())},
        {"texture_count", std::to_string(texture_count)},
        {"node_count", std::to_string(node_count)},
        {"metadata_only", options.metadata_only ? "true" : "false"},
    };
    result.assets.push_back(std::move(scene));

    for (std::size_t i = 0; i < mesh_names.size(); ++i) {
      AssetRecord mesh;
      mesh.asset_class = AssetClass::Mesh;
      mesh.name = mesh_names[i].empty() ? "mesh_" + std::to_string(i) : mesh_names[i];
      mesh.source_uri = source.uri + "#mesh/" + std::to_string(i);
      mesh.source_hash = HashTextHex(std::string(json.substr(0, std::min<std::size_t>(json.size(), 4096))) +
                                      mesh.source_uri);
      mesh.id = MakeAssetId(AssetClass::Mesh, mesh.source_uri + ":" + mesh.source_hash);
      mesh.tags = {"gltf", "static_mesh"};
      mesh.metadata = {{"mesh_index", std::to_string(i)}, {"importer", "gltf_glb_mvp"}};
      result.assets.push_back(std::move(mesh));
    }

    for (std::size_t i = 0; i < material_names.size(); ++i) {
      AssetRecord material;
      material.asset_class = AssetClass::Material;
      material.name = material_names[i].empty() ? "material_" + std::to_string(i) : material_names[i];
      material.source_uri = source.uri + "#material/" + std::to_string(i);
      material.source_hash = HashTextHex(source_hash + material.source_uri);
      material.id = MakeAssetId(AssetClass::Material, material.source_uri + ":" + material.source_hash);
      material.tags = {"gltf", "pbr", "material"};
      material.metadata = {
          {"material_index", std::to_string(i)},
          {"workflow", "metallic_roughness"},
          {"importer", "gltf_glb_mvp"},
      };
      result.assets.push_back(std::move(material));
    }

    const std::size_t texture_assets = std::max<std::size_t>(texture_count, image_uris.size());
    for (std::size_t i = 0; i < texture_assets; ++i) {
      const std::string image_uri = i < image_uris.size() && !image_uris[i].empty()
                                        ? image_uris[i]
                                        : source.uri + "#image/" + std::to_string(i);
      TextureDesc desc;
      desc.channel_semantic = TextureChannelSemantic::BaseColor;
      desc.color_space = TextureColorSpace::Srgb;
      desc.source_hash = HashTextHex(source_hash + image_uri);
      auto texture = MakeTextureAssetRecord(detail::FileNameOrUri(image_uri), image_uri, desc);
      texture.tags.push_back("gltf");
      texture.metadata["texture_index"] = std::to_string(i);
      texture.metadata["importer"] = "gltf_glb_mvp";
      result.assets.push_back(std::move(texture));
    }

    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "gltf.metadata_only",
                                                    "glTF/GLB MVP currently imports deterministic mesh/material/texture metadata; buffer geometry decode is deferred",
                                                    true));
    if (material_names.empty()) {
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                      "gltf.no_materials",
                                                      "glTF has no material entries; renderer should resolve benchmark-safe fallback material"));
    }
    result.deterministic_import_hash = HashTextHex(std::string("gltf:") + source.uri + ":" + source_hash + ":" +
                                                   std::to_string(result.assets.size()));
    result.success = true;
    return result;
  }

  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override {
    return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                               "gltf.ready",
                               "glTF/GLB metadata importer is registered for static mesh/material/texture MVP imports")};
  }

 private:
  static std::string extract_json(const std::vector<std::byte>& bytes, bool glb) {
    if (!glb) {
      return detail::BytesToString(bytes);
    }
    const auto json_length = detail::ReadU32Le(bytes, 12);
    const auto json_begin = 20u;
    if (json_begin + json_length > bytes.size()) {
      return {};
    }
    std::string text;
    text.resize(json_length);
    for (std::size_t i = 0; i < json_length; ++i) {
      text[i] = static_cast<char>(detail::Byte(bytes, json_begin + i));
    }
    return text;
  }
};

class ObjMtlImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override {
    return "obj_mtl_mvp";
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override {
    return {".obj"};
  }

  [[nodiscard]] std::vector<std::string_view> supported_features() const override {
    return {"obj", "mtl", "static_mesh_metadata", "legacy_material_to_pbr_fallback"};
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const override {
    if (!detail::HasExtension(source.uri, supported_extensions())) {
      return {false, "unsupported extension", {}};
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    if (bytes.empty()) {
      return {false, "source bytes unavailable", {}};
    }
    const auto text = detail::BytesToString(bytes);
    if (text.find("\nv ") == std::string::npos && !text.starts_with("v ")) {
      return {false, "OBJ has no vertex positions", {}};
    }
    return {true, "", {}};
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override {
    AssetImportResult result;
    auto validation = validate_source(source, options);
    if (!validation.valid) {
      result.diagnostics = std::move(validation.diagnostics);
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "obj.invalid",
                                                      validation.reason));
      return result;
    }

    const auto bytes = detail::ResolveSourceBytes(source);
    const auto text = detail::BytesToString(bytes);
    const auto source_hash = detail::HashBytesHex(bytes);
    std::size_t positions = 0;
    std::size_t normals = 0;
    std::size_t texcoords = 0;
    std::size_t faces = 0;
    std::size_t non_tri_faces = 0;
    std::set<std::string> mtllibs;
    std::set<std::string> used_materials;

    for (const auto& raw_line : detail::SplitLines(text)) {
      const auto line = detail::Trim(raw_line);
      if (line.empty() || line[0] == '#') {
        continue;
      }
      const auto words = detail::SplitWords(line);
      if (words.empty()) {
        continue;
      }
      if (words[0] == "v") {
        ++positions;
      } else if (words[0] == "vn") {
        ++normals;
      } else if (words[0] == "vt") {
        ++texcoords;
      } else if (words[0] == "f") {
        ++faces;
        if (words.size() != 4) {
          ++non_tri_faces;
        }
      } else if (words[0] == "mtllib" && words.size() >= 2) {
        mtllibs.insert(words[1]);
      } else if (words[0] == "usemtl" && words.size() >= 2) {
        used_materials.insert(words[1]);
      }
    }

    AssetRecord scene;
    scene.asset_class = AssetClass::Scene;
    scene.name = detail::FileNameOrUri(source.uri);
    scene.source_uri = source.uri;
    scene.source_hash = source_hash;
    scene.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash + "#obj_scene");
    scene.tags = {"obj", "scene"};
    scene.metadata = {
        {"importer", "obj_mtl_mvp"},
        {"mtllib_count", std::to_string(mtllibs.size())},
        {"used_material_count", std::to_string(used_materials.size())},
    };
    result.assets.push_back(std::move(scene));

    AssetRecord mesh;
    mesh.asset_class = AssetClass::Mesh;
    mesh.name = detail::FileNameOrUri(source.uri);
    mesh.source_uri = source.uri + "#mesh/0";
    mesh.source_hash = HashTextHex(source_hash + mesh.source_uri);
    mesh.id = MakeAssetId(AssetClass::Mesh, mesh.source_uri + ":" + mesh.source_hash);
    mesh.tags = {"obj", "static_mesh"};
    mesh.metadata = {
        {"importer", "obj_mtl_mvp"},
        {"position_count", std::to_string(positions)},
        {"normal_count", std::to_string(normals)},
        {"texcoord_count", std::to_string(texcoords)},
        {"face_count", std::to_string(faces)},
        {"non_tri_face_count", std::to_string(non_tri_faces)},
    };
    result.assets.push_back(std::move(mesh));

    const auto parsed_materials = parse_mtl_material_names(source, options, mtllibs);
    std::set<std::string> material_names = parsed_materials;
    material_names.insert(used_materials.begin(), used_materials.end());
    if (material_names.empty()) {
      material_names.insert("obj_fallback_material");
    }
    std::size_t material_index = 0;
    for (const auto& material_name : material_names) {
      AssetRecord material;
      material.asset_class = AssetClass::Material;
      material.name = material_name;
      material.source_uri = source.uri + "#material/" + material_name;
      material.source_hash = HashTextHex(source_hash + material.source_uri);
      material.id = MakeAssetId(AssetClass::Material, material.source_uri + ":" + material.source_hash);
      material.tags = {"obj", "mtl", "pbr_fallback"};
      material.metadata = {
          {"importer", "obj_mtl_mvp"},
          {"material_index", std::to_string(material_index++)},
          {"compatibility", "legacy_mtl_to_pbr_fallback"},
      };
      result.assets.push_back(std::move(material));
    }

    if (non_tri_faces > 0) {
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                      "obj.non_tri_faces",
                                                      "OBJ contains non-triangle faces; triangulation must be applied before renderer upload",
                                                      true));
    }
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "obj.legacy_materials",
                                                    "OBJ/MTL material fields are mapped to PBR fallback descriptors with compatibility notes",
                                                    true));
    result.deterministic_import_hash = HashTextHex(std::string("obj:") + source.uri + ":" + source_hash + ":" +
                                                   std::to_string(result.assets.size()));
    result.success = true;
    return result;
  }

  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override {
    return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                               "obj.ready",
                               "OBJ/MTL metadata importer is registered with legacy material compatibility diagnostics")};
  }

 private:
  static std::set<std::string> parse_mtl_material_names(const AssetImportSource& source,
                                                        const AssetImportOptions& options,
                                                        const std::set<std::string>& mtllibs) {
    std::set<std::string> out;
    for (const auto& mtl : mtllibs) {
      std::string text;
      if (const auto it = options.sidecar_text_by_uri.find(mtl); it != options.sidecar_text_by_uri.end()) {
        text = it->second;
      } else {
        const auto obj_path = std::filesystem::path(source.uri);
        const auto mtl_path = obj_path.parent_path() / mtl;
        std::ifstream file(mtl_path);
        if (file) {
          std::ostringstream buffer;
          buffer << file.rdbuf();
          text = buffer.str();
        }
      }
      for (const auto& raw_line : detail::SplitLines(text)) {
        const auto line = detail::Trim(raw_line);
        const auto words = detail::SplitWords(line);
        if (words.size() >= 2 && words[0] == "newmtl") {
          out.insert(words[1]);
        }
      }
    }
    return out;
  }
};

class TextureMetadataImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override {
    return "texture_metadata";
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override {
    return {".png", ".jpg", ".jpeg"};
  }

  [[nodiscard]] std::vector<std::string_view> supported_features() const override {
    return {"png_header", "jpeg_header", "texture_metadata", "sampler_defaults"};
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const override {
    if (!detail::HasExtension(source.uri, supported_extensions())) {
      return {false, "unsupported extension", {}};
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    if (bytes.empty()) {
      return {false, "source bytes unavailable", {}};
    }
    TextureDesc desc;
    const auto ext = detail::ExtensionOf(source.uri);
    if (ext == ".png" && !parse_png(bytes, &desc)) {
      return {false, "invalid PNG header", {}};
    }
    if ((ext == ".jpg" || ext == ".jpeg") && !parse_jpeg(bytes, &desc)) {
      return {false, "invalid JPEG header", {}};
    }
    return {true, "", {}};
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override {
    AssetImportResult result;
    auto validation = validate_source(source, options);
    if (!validation.valid) {
      result.diagnostics = std::move(validation.diagnostics);
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "texture.invalid",
                                                      validation.reason));
      return result;
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    const auto ext = detail::ExtensionOf(source.uri);
    TextureDesc desc;
    const bool parsed = ext == ".png" ? parse_png(bytes, &desc) : parse_jpeg(bytes, &desc);
    if (!parsed) {
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "texture.header_parse_failed",
                                                      "texture header parser failed after validation"));
      return result;
    }
    desc.source_hash = detail::HashBytesHex(bytes);
    desc.channel_semantic = options.texture_semantic == TextureChannelSemantic::Unknown &&
                                    options.infer_texture_semantic_from_uri
                                ? detail::InferTextureSemantic(source.uri, source.binding_context)
                                : options.texture_semantic;
    if (desc.channel_semantic == TextureChannelSemantic::Unknown) {
      desc.channel_semantic = TextureChannelSemantic::BaseColor;
    }
    desc.color_space = options.texture_color_space == TextureColorSpace::Unknown
                           ? DefaultColorSpaceForSemantic(desc.channel_semantic)
                           : options.texture_color_space;
    const auto channels = channel_count(desc.format);
    desc.format = TextureFormatForChannels(channels, desc.color_space);
    result.assets.push_back(MakeTextureAssetRecord(detail::FileNameOrUri(source.uri), source.uri, desc));
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "texture.metadata_only",
                                                    "PNG/JPEG path validates image headers and creates texture metadata; full pixel decode is deferred",
                                                    true));
    result.deterministic_import_hash = HashTextHex(std::string("texture:") + source.uri + ":" + desc.source_hash);
    result.success = true;
    return result;
  }

  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override {
    return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                               "texture.ready",
                               "PNG/JPEG metadata importer is registered with robust header validation")};
  }

 private:
  static bool parse_png(const std::vector<std::byte>& bytes, TextureDesc* out) {
    static constexpr std::uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33) {
      return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
      if (detail::Byte(bytes, i) != kPngSig[i]) {
        return false;
      }
    }
    if (detail::ReadU32Be(bytes, 8) != 13u) {
      return false;
    }
    if (detail::Byte(bytes, 12) != 'I' || detail::Byte(bytes, 13) != 'H' ||
        detail::Byte(bytes, 14) != 'D' || detail::Byte(bytes, 15) != 'R') {
      return false;
    }
    const auto width = detail::ReadU32Be(bytes, 16);
    const auto height = detail::ReadU32Be(bytes, 20);
    const auto bit_depth = detail::Byte(bytes, 24);
    const auto color_type = detail::Byte(bytes, 25);
    if (width == 0 || height == 0 || bit_depth != 8) {
      return false;
    }
    std::uint8_t channels = 0;
    switch (color_type) {
      case 0:
        channels = 1;
        break;
      case 2:
        channels = 3;
        break;
      case 4:
        channels = 2;
        break;
      case 6:
        channels = 4;
        break;
      default:
        return false;
    }
    out->width = width;
    out->height = height;
    out->format = TextureFormatForChannels(channels, TextureColorSpace::Linear);
    out->mip_count = 1;
    return true;
  }

  static bool parse_jpeg(const std::vector<std::byte>& bytes, TextureDesc* out) {
    if (bytes.size() < 4 || detail::Byte(bytes, 0) != 0xff || detail::Byte(bytes, 1) != 0xd8) {
      return false;
    }
    std::size_t i = 2;
    while (i + 4 < bytes.size()) {
      while (i < bytes.size() && detail::Byte(bytes, i) != 0xff) {
        ++i;
      }
      while (i < bytes.size() && detail::Byte(bytes, i) == 0xff) {
        ++i;
      }
      if (i >= bytes.size()) {
        return false;
      }
      const auto marker = detail::Byte(bytes, i++);
      if (marker == 0xd9 || marker == 0xda) {
        break;
      }
      if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
        continue;
      }
      if (i + 2 > bytes.size()) {
        return false;
      }
      const auto segment_length = detail::ReadU16Be(bytes, i);
      if (segment_length < 2 || i + segment_length > bytes.size()) {
        return false;
      }
      const bool sof = (marker >= 0xc0 && marker <= 0xc3) ||
                       (marker >= 0xc5 && marker <= 0xc7) ||
                       (marker >= 0xc9 && marker <= 0xcb) ||
                       (marker >= 0xcd && marker <= 0xcf);
      if (sof) {
        if (segment_length < 7) {
          return false;
        }
        const auto height = detail::ReadU16Be(bytes, i + 3);
        const auto width = detail::ReadU16Be(bytes, i + 5);
        const auto channels = detail::Byte(bytes, i + 7);
        if (width == 0 || height == 0 || channels == 0) {
          return false;
        }
        out->width = width;
        out->height = height;
        out->format = TextureFormatForChannels(channels, TextureColorSpace::Linear);
        out->mip_count = 1;
        return true;
      }
      i += segment_length;
    }
    return false;
  }

  static std::uint8_t channel_count(TextureFormat format) {
    switch (format) {
      case TextureFormat::R8Unorm:
        return 1;
      case TextureFormat::R8G8Unorm:
        return 2;
      case TextureFormat::R8G8B8Unorm:
      case TextureFormat::R8G8B8Srgb:
        return 3;
      case TextureFormat::R8G8B8A8Unorm:
      case TextureFormat::R8G8B8A8Srgb:
        return 4;
      default:
        return 4;
    }
  }
};

class ExrPolicyImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override {
    return "exr_policy";
  }

  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override {
    return {".exr"};
  }

  [[nodiscard]] std::vector<std::string_view> supported_features() const override {
    return {"exr_header_magic", "hdr_texture_policy", "reference_output_policy"};
  }

  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const override {
    if (!detail::HasExtension(source.uri, supported_extensions())) {
      return {false, "unsupported extension", {}};
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    if (bytes.empty()) {
      return {true, "EXR path declared without bytes; policy-only validation", {
          detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                             "exr.policy_only",
                             "EXR source bytes unavailable; treating as policy declaration only")}};
    }
    if (bytes.size() < 4 || detail::ReadU32Le(bytes, 0) != 0x01312f76u) {
      return {false, "invalid OpenEXR magic", {}};
    }
    return {true, "", {}};
  }

  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override {
    AssetImportResult result;
    auto validation = validate_source(source, options);
    result.diagnostics = std::move(validation.diagnostics);
    if (!validation.valid) {
      result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                      "exr.invalid",
                                                      validation.reason));
      return result;
    }
    const auto bytes = detail::ResolveSourceBytes(source);
    const auto source_hash = bytes.empty() ? HashTextHex(source.uri) : detail::HashBytesHex(bytes);
    TextureDesc desc;
    desc.format = TextureFormat::R16G16B16A16Float;
    desc.color_space = TextureColorSpace::HdrLinear;
    desc.channel_semantic = options.texture_semantic == TextureChannelSemantic::Unknown
                                ? TextureChannelSemantic::Data
                                : options.texture_semantic;
    desc.source_hash = source_hash;
    auto record = MakeTextureAssetRecord(detail::FileNameOrUri(source.uri), source.uri, desc);
    record.metadata["exr_policy"] = SerializeExrSupportPolicy(GetDefaultExrSupportPolicy());
    record.metadata["decode_available"] = "false";
    result.assets.push_back(std::move(record));
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "exr.decode_deferred",
                                                    "EXR support is represented as an input/output policy hook; full HDR decode/write is deferred",
                                                    true));
    result.deterministic_import_hash = HashTextHex(std::string("exr:") + source.uri + ":" + source_hash);
    result.success = true;
    return result;
  }

  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override {
    return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                               "exr.policy_registered",
                               "EXR policy hook is registered for HDR texture/reference output capability discovery")};
  }
};

inline ImporterRegistry CreateDefaultImporterRegistry(bool include_fake_importer = false) {
  ImporterRegistry registry;
  registry.register_importer(std::make_shared<GltfGlbImporter>());
  registry.register_importer(std::make_shared<ObjMtlImporter>());
  registry.register_importer(std::make_shared<TextureMetadataImporter>());
  registry.register_importer(std::make_shared<ExrPolicyImporter>());
  if (include_fake_importer) {
    registry.register_importer(std::make_shared<FakeAssetImporter>());
  }
  return registry;
}

inline std::string SerializeAssetCapabilityMatrix(const ImporterRegistry& registry,
                                                  const ExrSupportPolicy& exr_policy = GetDefaultExrSupportPolicy()) {
  std::ostringstream out;
  out << "{\"schema\":\"vkpt.asset_capability_matrix.v1\",";
  out << "\"importers\":[";
  const auto& importers = registry.importers();
  for (std::size_t i = 0; i < importers.size(); ++i) {
    if (i > 0) out << ",";
    out << "{";
    out << "\"id\":\"" << EscapeJson(importers[i]->importer_id()) << "\",";
    out << "\"extensions\":[";
    const auto extensions = importers[i]->supported_extensions();
    for (std::size_t j = 0; j < extensions.size(); ++j) {
      if (j > 0) out << ",";
      out << "\"" << EscapeJson(extensions[j]) << "\"";
    }
    out << "],\"features\":[";
    const auto features = importers[i]->supported_features();
    for (std::size_t j = 0; j < features.size(); ++j) {
      if (j > 0) out << ",";
      out << "\"" << EscapeJson(features[j]) << "\"";
    }
    out << "]}";
  }
  out << "],\"exr\":" << SerializeExrSupportPolicy(exr_policy) << "}";
  return out.str();
}

}  // namespace vkpt::assets
