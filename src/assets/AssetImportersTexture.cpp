#include "assets/AssetImporters.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace vkpt::assets {

std::string_view TextureMetadataImporter::importer_id() const {
  return "texture.metadata";
}

std::vector<std::string_view> TextureMetadataImporter::supported_extensions() const {
  return {".png", ".jpg", ".jpeg"};
}

std::vector<std::string_view> TextureMetadataImporter::supported_features() const {
  return {"metadata", "png_dimensions", "jpeg_dimensions", "semantic_inference"};
}

AssetValidationResult TextureMetadataImporter::validate_source(const AssetImportSource& source,
                                                               const AssetImportOptions&) const {
  if (!detail::HasExtension(source.uri, supported_extensions())) {
    return {false, "unsupported texture extension", {}};
  }
  const auto bytes = detail::ResolveSourceBytes(source);
  if (bytes.empty()) {
    return {false, "texture source is empty or unavailable", {}};
  }
  TextureDesc desc;
  const auto ext = detail::ExtensionOf(source.uri);
  const bool parsed = ext == ".png" ? parse_png(bytes, &desc) : parse_jpeg(bytes, &desc);
  if (!parsed) {
    return {false, "texture metadata could not be parsed", {detail::Diagnostic(
        ImportDiagnosticSeverity::Error,
        "texture.parse_failed",
        "Texture metadata parser could not identify dimensions/format")}};
  }
  return {true, {}, {}};
}

AssetImportResult TextureMetadataImporter::import_source(const AssetImportSource& source,
                                                         const AssetImportOptions& options) const {
  AssetImportResult result;
  const auto bytes = detail::ResolveSourceBytes(source);
  const auto ext = detail::ExtensionOf(source.uri);
  TextureDesc desc;
  const bool parsed = ext == ".png" ? parse_png(bytes, &desc) : parse_jpeg(bytes, &desc);
  if (!parsed) {
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                    "texture.parse_failed",
                                                    "Texture metadata could not be parsed"));
    return result;
  }

  desc.source_hash = detail::HashBytesHex(bytes);
  desc.channel_semantic = options.texture_semantic != TextureChannelSemantic::Unknown
      ? options.texture_semantic
      : (options.infer_texture_semantic_from_uri
             ? detail::InferTextureSemantic(source.uri, source.binding_context)
             : TextureChannelSemantic::Unknown);
  desc.color_space = options.texture_color_space != TextureColorSpace::Unknown
      ? options.texture_color_space
      : DefaultColorSpaceForSemantic(desc.channel_semantic);
  if (desc.color_space == TextureColorSpace::Srgb) {
    desc.format = TextureFormatForChannels(channel_count(desc.format), TextureColorSpace::Srgb);
  }

  result.assets.push_back(MakeTextureAssetRecord(detail::FileNameOrUri(source.uri), source.uri, desc));
  result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                  "texture.metadata_only",
                                                  "Texture importer records metadata; pixel upload is deferred",
                                                  true));
  result.deterministic_import_hash =
      HashTextHex(std::string("texture:") + source.uri + ":" + desc.source_hash);
  result.success = true;
  return result;
}

std::vector<AssetImportDiagnostic> TextureMetadataImporter::emit_diagnostics() const {
  return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                             "texture.metadata_ready",
                             "Texture metadata importer supports PNG/JPEG dimensions and semantic inference")};
}

bool TextureMetadataImporter::parse_png(const std::vector<std::byte>& bytes, TextureDesc* out) {
  static constexpr std::uint8_t kPngSig[8] = {0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au};
  if (out == nullptr || bytes.size() < 33u) {
    return false;
  }
  for (std::size_t i = 0; i < 8u; ++i) {
    if (detail::Byte(bytes, i) != kPngSig[i]) {
      return false;
    }
  }
  if (detail::ReadU32Be(bytes, 8) != 13u ||
      detail::Byte(bytes, 12) != 'I' ||
      detail::Byte(bytes, 13) != 'H' ||
      detail::Byte(bytes, 14) != 'D' ||
      detail::Byte(bytes, 15) != 'R') {
    return false;
  }

  const auto width = detail::ReadU32Be(bytes, 16);
  const auto height = detail::ReadU32Be(bytes, 20);
  const auto bit_depth = detail::Byte(bytes, 24);
  const auto color_type = detail::Byte(bytes, 25);
  if (width == 0u || height == 0u || bit_depth != 8u) {
    return false;
  }
  std::uint8_t channels = 0u;
  switch (color_type) {
    case 0u: channels = 1u; break;
    case 2u: channels = 3u; break;
    case 4u: channels = 2u; break;
    case 6u: channels = 4u; break;
    default: return false;
  }
  out->width = width;
  out->height = height;
  out->format = TextureFormatForChannels(channels, TextureColorSpace::Linear);
  out->mip_count = 1u;
  return true;
}

bool TextureMetadataImporter::parse_jpeg(const std::vector<std::byte>& bytes, TextureDesc* out) {
  if (out == nullptr || bytes.size() < 4u || detail::Byte(bytes, 0) != 0xff || detail::Byte(bytes, 1) != 0xd8) {
    return false;
  }
  std::size_t i = 2u;
  while (i + 8u < bytes.size()) {
    while (i < bytes.size() && detail::Byte(bytes, i) != 0xff) {
      ++i;
    }
    while (i < bytes.size() && detail::Byte(bytes, i) == 0xff) {
      ++i;
    }
    if (i >= bytes.size()) {
      break;
    }
    const auto marker = detail::Byte(bytes, i++);
    if (marker == 0xd9u || marker == 0xdau) {
      break;
    }
    if (i + 2u > bytes.size()) {
      return false;
    }
    const auto segment_length = detail::ReadU16Be(bytes, i);
    if (segment_length < 2u || i + segment_length > bytes.size()) {
      return false;
    }
    const bool sof = (marker >= 0xc0u && marker <= 0xc3u) ||
                     (marker >= 0xc5u && marker <= 0xc7u) ||
                     (marker >= 0xc9u && marker <= 0xcbu) ||
                     (marker >= 0xcdu && marker <= 0xcfu);
    if (sof) {
      const auto height = detail::ReadU16Be(bytes, i + 3u);
      const auto width = detail::ReadU16Be(bytes, i + 5u);
      const auto channels = detail::Byte(bytes, i + 7u);
      if (width == 0u || height == 0u || channels == 0u) {
        return false;
      }
      out->width = width;
      out->height = height;
      out->format = TextureFormatForChannels(std::min<std::uint8_t>(channels, 4u), TextureColorSpace::Linear);
      out->mip_count = 1u;
      return true;
    }
    i += segment_length;
  }
  return false;
}

std::uint8_t TextureMetadataImporter::channel_count(TextureFormat format) {
  switch (format) {
    case TextureFormat::R8Unorm:
      return 1u;
    case TextureFormat::R8G8Unorm:
      return 2u;
    case TextureFormat::R8G8B8Unorm:
    case TextureFormat::R8G8B8Srgb:
      return 3u;
    case TextureFormat::R8G8B8A8Unorm:
    case TextureFormat::R8G8B8A8Srgb:
    case TextureFormat::R16G16B16A16Float:
    case TextureFormat::R32G32B32A32Float:
      return 4u;
    default:
      return 0u;
  }
}

std::string_view ExrPolicyImporter::importer_id() const {
  return "exr.policy";
}

std::vector<std::string_view> ExrPolicyImporter::supported_extensions() const {
  return {".exr"};
}

std::vector<std::string_view> ExrPolicyImporter::supported_features() const {
  return {"metadata", "hdr_policy", "decode_deferred"};
}

AssetValidationResult ExrPolicyImporter::validate_source(const AssetImportSource& source,
                                                         const AssetImportOptions&) const {
  if (!detail::HasExtension(source.uri, supported_extensions())) {
    return {false, "unsupported EXR extension", {}};
  }
  const auto bytes = detail::ResolveSourceBytes(source);
  if (bytes.empty()) {
    return {false, "EXR source is empty or unavailable", {detail::Diagnostic(
        ImportDiagnosticSeverity::Warning,
        "exr.unavailable",
        "EXR metadata policy registered, but source bytes are unavailable")}};
  }
  if (bytes.size() < 4u || detail::ReadU32Le(bytes, 0) != 0x01312f76u) {
    return {false, "invalid EXR magic", {detail::Diagnostic(
        ImportDiagnosticSeverity::Error,
        "exr.invalid_magic",
        "EXR file does not start with the OpenEXR magic number")}};
  }
  return {true, {}, {}};
}

AssetImportResult ExrPolicyImporter::import_source(const AssetImportSource& source,
                                                   const AssetImportOptions&) const {
  AssetImportResult result;
  const auto bytes = detail::ResolveSourceBytes(source);
  const auto source_hash = bytes.empty() ? HashTextHex(source.uri) : detail::HashBytesHex(bytes);
  TextureDesc desc;
  desc.width = 0u;
  desc.height = 0u;
  desc.format = TextureFormat::R16G16B16A16Float;
  desc.color_space = TextureColorSpace::HdrLinear;
  desc.channel_semantic = TextureChannelSemantic::Data;
  desc.source_hash = source_hash;
  auto record = MakeTextureAssetRecord(detail::FileNameOrUri(source.uri), source.uri, desc);
  record.tags.push_back("exr");
  record.metadata["exr_policy"] = SerializeExrSupportPolicy(GetDefaultExrSupportPolicy());
  result.assets.push_back(std::move(record));
  result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                  "exr.decode_deferred",
                                                  "EXR asset declared for HDR/reference workflows; pixel decode is deferred",
                                                  true));
  result.deterministic_import_hash = HashTextHex(std::string("exr:") + source.uri + ":" + source_hash);
  result.success = true;
  return result;
}

std::vector<AssetImportDiagnostic> ExrPolicyImporter::emit_diagnostics() const {
  return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                             "exr.policy_ready",
                             "EXR policy importer declares HDR assets while decode/write support remains backend-gated")};
}

}  // namespace vkpt::assets
