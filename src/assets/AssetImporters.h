#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
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
  // Importers prefer explicit bytes, then fall back to resolving uri on disk.
  std::string uri;
  std::vector<std::byte> bytes;
  std::string root_directory;
  std::string binding_context;
};

struct AssetImportOptions {
  // Most importers emit stable records first; geometry/pixel decode is opt-in.
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

std::string ToLower(std::string_view text);
std::string Trim(std::string_view text);
std::string ExtensionOf(std::string_view uri);
bool HasExtension(std::string_view uri, const std::vector<std::string_view>& extensions);
std::uint8_t Byte(const std::vector<std::byte>& bytes, std::size_t index);
std::uint32_t ReadU32Le(const std::vector<std::byte>& bytes, std::size_t offset);
std::uint32_t ReadU32Be(const std::vector<std::byte>& bytes, std::size_t offset);
std::uint16_t ReadU16Be(const std::vector<std::byte>& bytes, std::size_t offset);
std::vector<std::byte> ReadFileBytes(std::string_view uri);
std::vector<std::byte> ResolveSourceBytes(const AssetImportSource& source);
std::string BytesToString(const std::vector<std::byte>& bytes);
std::string HashBytesHex(const std::vector<std::byte>& bytes);
AssetImportDiagnostic Diagnostic(ImportDiagnosticSeverity severity,
                                 std::string code,
                                 std::string message,
                                 bool lossy = false);
std::vector<std::string> SplitLines(std::string_view text);
std::vector<std::string> SplitWords(std::string_view text);
std::optional<std::string_view> ExtractNamedArray(std::string_view json, std::string_view name);
std::vector<std::string_view> TopLevelObjects(std::string_view array_text);
std::optional<std::string> ExtractStringValue(std::string_view object_text, std::string_view name);
std::vector<std::string> ExtractObjectNames(std::string_view json, std::string_view array_name);
std::vector<std::string> ExtractImageUris(std::string_view json);
TextureChannelSemantic InferTextureSemantic(std::string_view uri, std::string_view context);
std::string FileNameOrUri(std::string_view uri);

}  // namespace detail

class ImporterRegistry {
 public:
  // Dispatches by normalized extension so scene, mesh, and texture pipelines share diagnostics.
  bool register_importer(std::shared_ptr<IAssetImporter> importer);

  [[nodiscard]] const IAssetImporter* importer_for_extension(std::string_view extension) const;
  [[nodiscard]] const IAssetImporter* importer_for_source(const AssetImportSource& source) const;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options = {}) const;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options = {}) const;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const;
  [[nodiscard]] const std::vector<std::shared_ptr<IAssetImporter>>& importers() const;

 private:
  std::vector<std::shared_ptr<IAssetImporter>> m_importers;
};

class FakeAssetImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override;
  [[nodiscard]] std::vector<std::string_view> supported_features() const override;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options) const override;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override;
  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override;
};

class GltfGlbImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override;
  [[nodiscard]] std::vector<std::string_view> supported_features() const override;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options) const override;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override;
  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override;

 private:
  static std::string extract_json(const std::vector<std::byte>& bytes, bool glb);
};

class ObjMtlImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override;
  [[nodiscard]] std::vector<std::string_view> supported_features() const override;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options) const override;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override;
  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override;

 private:
  static std::set<std::string> parse_mtl_material_names(const AssetImportSource& source,
                                                        const AssetImportOptions& options,
                                                        const std::set<std::string>& mtllibs);
};

class TextureMetadataImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override;
  [[nodiscard]] std::vector<std::string_view> supported_features() const override;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options) const override;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override;
  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override;

 private:
  static bool parse_png(const std::vector<std::byte>& bytes, TextureDesc* out);
  static bool parse_jpeg(const std::vector<std::byte>& bytes, TextureDesc* out);
  static std::uint8_t channel_count(TextureFormat format);
};

class ExrPolicyImporter final : public IAssetImporter {
 public:
  [[nodiscard]] std::string_view importer_id() const override;
  [[nodiscard]] std::vector<std::string_view> supported_extensions() const override;
  [[nodiscard]] std::vector<std::string_view> supported_features() const override;
  [[nodiscard]] AssetValidationResult validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions& options) const override;
  [[nodiscard]] AssetImportResult import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const override;
  [[nodiscard]] std::vector<AssetImportDiagnostic> emit_diagnostics() const override;
};

ImporterRegistry CreateDefaultImporterRegistry(bool include_fake_importer = false);

std::string SerializeAssetCapabilityMatrix(const ImporterRegistry& registry,
                                           const ExrSupportPolicy& exr_policy = GetDefaultExrSupportPolicy());

}  // namespace vkpt::assets
