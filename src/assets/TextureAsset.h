#pragma once

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "assets/AssetRegistry.h"

namespace vkpt::assets {

enum class TextureFormat : std::uint8_t {
  Unknown,
  R8Unorm,
  R8G8Unorm,
  R8G8B8Unorm,
  R8G8B8A8Unorm,
  R8G8B8Srgb,
  R8G8B8A8Srgb,
  R16G16B16A16Float,
  R32G32B32A32Float,
};

enum class TextureColorSpace : std::uint8_t {
  Srgb,
  Linear,
  HdrLinear,
  NonColorData,
  Unknown,
};

enum class TextureChannelSemantic : std::uint8_t {
  BaseColor,
  Normal,
  Roughness,
  Metallic,
  Occlusion,
  Emissive,
  Alpha,
  Height,
  Data,
  Unknown,
};

enum class TextureWrapMode : std::uint8_t {
  Repeat,
  ClampToEdge,
  MirroredRepeat,
};

enum class TextureFilter : std::uint8_t {
  Nearest,
  Linear,
  LinearMipmapLinear,
};

struct TextureSamplerDesc {
  TextureWrapMode wrap_u = TextureWrapMode::Repeat;
  TextureWrapMode wrap_v = TextureWrapMode::Repeat;
  TextureWrapMode wrap_w = TextureWrapMode::Repeat;
  TextureFilter min_filter = TextureFilter::LinearMipmapLinear;
  TextureFilter mag_filter = TextureFilter::Linear;
  float anisotropy = 1.0f;
};

struct TextureDesc {
  TextureFormat format = TextureFormat::Unknown;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mip_count = 1;
  TextureColorSpace color_space = TextureColorSpace::Unknown;
  TextureChannelSemantic channel_semantic = TextureChannelSemantic::Unknown;
  std::string source_hash;
  TextureSamplerDesc sampler{};
};

struct TextureAsset {
  AssetRecord record;
  TextureDesc desc;
};

struct ExrSupportPolicy {
  bool input_declared = true;
  bool input_decode_available = false;
  bool output_declared = true;
  bool output_write_available = false;
  std::string policy_id = "exr_metadata_policy_v1";
  std::string notes = "OpenEXR assets are declared for HDR/reference workflows; full decode/write requires an EXR backend integration.";
};

inline const char* ToString(TextureFormat format) {
  switch (format) {
    case TextureFormat::R8Unorm:
      return "r8_unorm";
    case TextureFormat::R8G8Unorm:
      return "r8g8_unorm";
    case TextureFormat::R8G8B8Unorm:
      return "r8g8b8_unorm";
    case TextureFormat::R8G8B8A8Unorm:
      return "r8g8b8a8_unorm";
    case TextureFormat::R8G8B8Srgb:
      return "r8g8b8_srgb";
    case TextureFormat::R8G8B8A8Srgb:
      return "r8g8b8a8_srgb";
    case TextureFormat::R16G16B16A16Float:
      return "r16g16b16a16_float";
    case TextureFormat::R32G32B32A32Float:
      return "r32g32b32a32_float";
    case TextureFormat::Unknown:
    default:
      return "unknown";
  }
}

inline const char* ToString(TextureColorSpace color_space) {
  switch (color_space) {
    case TextureColorSpace::Srgb:
      return "srgb";
    case TextureColorSpace::Linear:
      return "linear";
    case TextureColorSpace::HdrLinear:
      return "hdr_linear";
    case TextureColorSpace::NonColorData:
      return "non_color_data";
    case TextureColorSpace::Unknown:
    default:
      return "unknown";
  }
}

inline const char* ToString(TextureChannelSemantic semantic) {
  switch (semantic) {
    case TextureChannelSemantic::BaseColor:
      return "base_color";
    case TextureChannelSemantic::Normal:
      return "normal";
    case TextureChannelSemantic::Roughness:
      return "roughness";
    case TextureChannelSemantic::Metallic:
      return "metallic";
    case TextureChannelSemantic::Occlusion:
      return "occlusion";
    case TextureChannelSemantic::Emissive:
      return "emissive";
    case TextureChannelSemantic::Alpha:
      return "alpha";
    case TextureChannelSemantic::Height:
      return "height";
    case TextureChannelSemantic::Data:
      return "data";
    case TextureChannelSemantic::Unknown:
    default:
      return "unknown";
  }
}

inline const char* ToString(TextureWrapMode wrap) {
  switch (wrap) {
    case TextureWrapMode::Repeat:
      return "repeat";
    case TextureWrapMode::ClampToEdge:
      return "clamp_to_edge";
    case TextureWrapMode::MirroredRepeat:
      return "mirrored_repeat";
    default:
      return "repeat";
  }
}

inline const char* ToString(TextureFilter filter) {
  switch (filter) {
    case TextureFilter::Nearest:
      return "nearest";
    case TextureFilter::Linear:
      return "linear";
    case TextureFilter::LinearMipmapLinear:
      return "linear_mipmap_linear";
    default:
      return "linear";
  }
}

inline TextureColorSpace DefaultColorSpaceForSemantic(TextureChannelSemantic semantic) {
  switch (semantic) {
    case TextureChannelSemantic::BaseColor:
    case TextureChannelSemantic::Emissive:
      return TextureColorSpace::Srgb;
    case TextureChannelSemantic::Normal:
    case TextureChannelSemantic::Roughness:
    case TextureChannelSemantic::Metallic:
    case TextureChannelSemantic::Occlusion:
    case TextureChannelSemantic::Alpha:
    case TextureChannelSemantic::Height:
    case TextureChannelSemantic::Data:
      return TextureColorSpace::NonColorData;
    case TextureChannelSemantic::Unknown:
    default:
      return TextureColorSpace::Unknown;
  }
}

inline TextureFormat TextureFormatForChannels(std::uint8_t channels, TextureColorSpace color_space) {
  const bool srgb = color_space == TextureColorSpace::Srgb;
  switch (channels) {
    case 1:
      return TextureFormat::R8Unorm;
    case 2:
      return TextureFormat::R8G8Unorm;
    case 3:
      return srgb ? TextureFormat::R8G8B8Srgb : TextureFormat::R8G8B8Unorm;
    case 4:
      return srgb ? TextureFormat::R8G8B8A8Srgb : TextureFormat::R8G8B8A8Unorm;
    default:
      return TextureFormat::Unknown;
  }
}

inline std::string SerializeTextureDesc(const TextureDesc& desc) {
  std::ostringstream out;
  out << "{";
  out << "\"format\":\"" << ToString(desc.format) << "\",";
  out << "\"width\":" << desc.width << ",";
  out << "\"height\":" << desc.height << ",";
  out << "\"mip_count\":" << desc.mip_count << ",";
  out << "\"color_space\":\"" << ToString(desc.color_space) << "\",";
  out << "\"channel_semantic\":\"" << ToString(desc.channel_semantic) << "\",";
  out << "\"source_hash\":\"" << EscapeJson(desc.source_hash) << "\",";
  out << "\"sampler\":{";
  out << "\"wrap_u\":\"" << ToString(desc.sampler.wrap_u) << "\",";
  out << "\"wrap_v\":\"" << ToString(desc.sampler.wrap_v) << "\",";
  out << "\"wrap_w\":\"" << ToString(desc.sampler.wrap_w) << "\",";
  out << "\"min_filter\":\"" << ToString(desc.sampler.min_filter) << "\",";
  out << "\"mag_filter\":\"" << ToString(desc.sampler.mag_filter) << "\",";
  out << "\"anisotropy\":" << desc.sampler.anisotropy;
  out << "}}";
  return out.str();
}

inline AssetRecord MakeTextureAssetRecord(std::string_view name,
                                          std::string_view source_uri,
                                          const TextureDesc& desc) {
  AssetRecord record;
  record.asset_class = AssetClass::Texture;
  record.name = std::string(name);
  record.source_uri = std::string(source_uri);
  record.source_hash = desc.source_hash;
  record.id = MakeAssetId(AssetClass::Texture, std::string(source_uri) + ":" + desc.source_hash);
  record.tags = {"texture", ToString(desc.channel_semantic), ToString(desc.color_space)};
  record.metadata = {
      {"format", ToString(desc.format)},
      {"width", std::to_string(desc.width)},
      {"height", std::to_string(desc.height)},
      {"mip_count", std::to_string(desc.mip_count)},
      {"color_space", ToString(desc.color_space)},
      {"channel_semantic", ToString(desc.channel_semantic)},
      {"sampler_min_filter", ToString(desc.sampler.min_filter)},
      {"sampler_mag_filter", ToString(desc.sampler.mag_filter)},
      {"texture_desc", SerializeTextureDesc(desc)},
  };
  return record;
}

inline ExrSupportPolicy GetDefaultExrSupportPolicy() {
  return {};
}

inline std::string SerializeExrSupportPolicy(const ExrSupportPolicy& policy) {
  std::ostringstream out;
  out << "{";
  out << "\"policy_id\":\"" << EscapeJson(policy.policy_id) << "\",";
  out << "\"input_declared\":" << (policy.input_declared ? "true" : "false") << ",";
  out << "\"input_decode_available\":" << (policy.input_decode_available ? "true" : "false") << ",";
  out << "\"output_declared\":" << (policy.output_declared ? "true" : "false") << ",";
  out << "\"output_write_available\":" << (policy.output_write_available ? "true" : "false") << ",";
  out << "\"notes\":\"" << EscapeJson(policy.notes) << "\"";
  out << "}";
  return out.str();
}

}  // namespace vkpt::assets
