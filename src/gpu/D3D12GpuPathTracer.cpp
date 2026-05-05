#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.h"
#include "core/Logging.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <iomanip>
#include <iterator>
#include <limits>
#include <wincodec.h>

namespace vkpt::gpu {

namespace {
constexpr UINT kPathTraceRoot32BitValues = static_cast<UINT>(sizeof(PathTraceConstants) / sizeof(uint32_t));
constexpr uint32_t kGpuSdfStrideFloats = 16u;
constexpr uint32_t kGpuTriDataStrideFloats = 18u;
constexpr uint32_t kDxrStaticInstanceId = 0x00ffffffu;
constexpr uint32_t kInvalidTextureIndex = 0xffffffffu;
constexpr uint32_t kMaxTextureDimension = 512u;
uint64_t g_d3d12RenderBatchCalls = 0u;
uint64_t g_d3d12SceneUploadCalls = 0u;

void LogInfo(const std::string& msg) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "d3d12", msg);
}
void LogError(const std::string& msg) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "d3d12", msg);
}
void LogDebug(const std::string& msg) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug, "d3d12", msg);
}

std::string WStringToUtf8(const wchar_t* src) {
  if (!src) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, src, -1, &out[0], len, nullptr, nullptr);
  return out;
}

std::string FormatHr(HRESULT hr) {
  std::ostringstream ss;
  ss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
     << static_cast<uint32_t>(hr);
  return ss.str();
}

struct LoadedTextureRgba8 {
  uint32_t width = 1u;
  uint32_t height = 1u;
  std::vector<uint32_t> texels{0xffffffffu};
};

std::string LowercaseExtension(const std::filesystem::path& path) {
  std::string ext = path.extension().generic_string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

std::filesystem::path ResolveTexturePath(std::string_view uri) {
  std::filesystem::path path{std::string(uri)};
  if (std::filesystem::exists(path)) {
    return path.lexically_normal();
  }
  const auto cwdPath = (std::filesystem::current_path() / path).lexically_normal();
  if (std::filesystem::exists(cwdPath)) {
    return cwdPath;
  }
  const auto sceneRelative = (std::filesystem::current_path() / "assets" / "scenes" / path).lexically_normal();
  if (std::filesystem::exists(sceneRelative)) {
    return sceneRelative;
  }
  return path.lexically_normal();
}

bool DownsampleTextureNearest(LoadedTextureRgba8& texture, uint32_t maxDimension) {
  const uint32_t largest = std::max(texture.width, texture.height);
  if (largest <= maxDimension || maxDimension == 0u || texture.width == 0u || texture.height == 0u) {
    return true;
  }
  const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(
      (static_cast<uint64_t>(texture.width) * maxDimension) / largest));
  const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(
      (static_cast<uint64_t>(texture.height) * maxDimension) / largest));
  std::vector<uint32_t> downsampled(static_cast<std::size_t>(targetWidth) * targetHeight, 0xffffffffu);
  for (uint32_t y = 0; y < targetHeight; ++y) {
    const uint32_t srcY = std::min(texture.height - 1u,
                                   static_cast<uint32_t>((static_cast<uint64_t>(y) * texture.height) / targetHeight));
    for (uint32_t x = 0; x < targetWidth; ++x) {
      const uint32_t srcX = std::min(texture.width - 1u,
                                     static_cast<uint32_t>((static_cast<uint64_t>(x) * texture.width) / targetWidth));
      downsampled[static_cast<std::size_t>(y) * targetWidth + x] =
          texture.texels[static_cast<std::size_t>(srcY) * texture.width + srcX];
    }
  }
  texture.width = targetWidth;
  texture.height = targetHeight;
  texture.texels = std::move(downsampled);
  return true;
}

bool LoadTgaRgba8(const std::filesystem::path& path,
                  uint32_t maxDimension,
                  LoadedTextureRgba8& out,
                  std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error) *error = "TGA open failed";
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() < 18u) {
    if (error) *error = "TGA header too small";
    return false;
  }
  const uint8_t idLength = bytes[0];
  const uint8_t colorMapType = bytes[1];
  const uint8_t imageType = bytes[2];
  const uint16_t width = static_cast<uint16_t>(bytes[12] | (bytes[13] << 8u));
  const uint16_t height = static_cast<uint16_t>(bytes[14] | (bytes[15] << 8u));
  const uint8_t bitsPerPixel = bytes[16];
  const uint8_t descriptor = bytes[17];
  if (colorMapType != 0u || width == 0u || height == 0u ||
      (imageType != 2u && imageType != 3u && imageType != 10u) ||
      (bitsPerPixel != 8u && bitsPerPixel != 24u && bitsPerPixel != 32u)) {
    if (error) *error = "unsupported TGA format";
    return false;
  }
  std::size_t offset = 18u + idLength;
  if (offset >= bytes.size()) {
    if (error) *error = "TGA data offset invalid";
    return false;
  }

  out.width = width;
  out.height = height;
  out.texels.assign(static_cast<std::size_t>(width) * height, 0xffffffffu);
  const bool topOrigin = (descriptor & 0x20u) != 0u;
  const auto writePixel = [&](uint32_t linearIndex, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    const uint32_t x = linearIndex % width;
    const uint32_t srcY = linearIndex / width;
    const uint32_t y = topOrigin ? srcY : (height - 1u - srcY);
    out.texels[static_cast<std::size_t>(y) * width + x] =
        static_cast<uint32_t>(r) |
        (static_cast<uint32_t>(g) << 8u) |
        (static_cast<uint32_t>(b) << 16u) |
        (static_cast<uint32_t>(a) << 24u);
  };
  const uint32_t pixelCount = static_cast<uint32_t>(width) * height;
  const uint32_t bytesPerPixel = bitsPerPixel / 8u;
  if (imageType == 2u || imageType == 3u) {
    const std::size_t required = offset + static_cast<std::size_t>(pixelCount) * bytesPerPixel;
    if (required > bytes.size()) {
      if (error) *error = "TGA pixel data truncated";
      return false;
    }
    for (uint32_t i = 0; i < pixelCount; ++i) {
      if (bitsPerPixel == 8u) {
        const uint8_t v = bytes[offset++];
        writePixel(i, v, v, v, 255u);
      } else {
        const uint8_t b = bytes[offset++];
        const uint8_t g = bytes[offset++];
        const uint8_t r = bytes[offset++];
        const uint8_t a = bitsPerPixel == 32u ? bytes[offset++] : 255u;
        writePixel(i, b, g, r, a);
      }
    }
  } else {
    uint32_t written = 0u;
    while (written < pixelCount && offset < bytes.size()) {
      const uint8_t packet = bytes[offset++];
      const uint32_t count = static_cast<uint32_t>(packet & 0x7fu) + 1u;
      if ((packet & 0x80u) != 0u) {
        if (offset + bytesPerPixel > bytes.size()) {
          if (error) *error = "TGA RLE packet truncated";
          return false;
        }
        uint8_t b = 0u, g = 0u, r = 0u, a = 255u;
        if (bitsPerPixel == 8u) {
          b = g = r = bytes[offset++];
        } else {
          b = bytes[offset++];
          g = bytes[offset++];
          r = bytes[offset++];
          if (bitsPerPixel == 32u) a = bytes[offset++];
        }
        for (uint32_t i = 0; i < count && written < pixelCount; ++i) {
          writePixel(written++, b, g, r, a);
        }
      } else {
        for (uint32_t i = 0; i < count && written < pixelCount; ++i) {
          if (offset + bytesPerPixel > bytes.size()) {
            if (error) *error = "TGA raw packet truncated";
            return false;
          }
          uint8_t b = 0u, g = 0u, r = 0u, a = 255u;
          if (bitsPerPixel == 8u) {
            b = g = r = bytes[offset++];
          } else {
            b = bytes[offset++];
            g = bytes[offset++];
            r = bytes[offset++];
            if (bitsPerPixel == 32u) a = bytes[offset++];
          }
          writePixel(written++, b, g, r, a);
        }
      }
    }
    if (written != pixelCount) {
      if (error) *error = "TGA RLE ended early";
      return false;
    }
  }

  return DownsampleTextureNearest(out, maxDimension);
}

bool LoadTextureRgba8(std::string_view uri, uint32_t maxDimension, LoadedTextureRgba8& out, std::string* error) {
  const auto path = ResolveTexturePath(uri);
  if (LowercaseExtension(path) == ".tga") {
    return LoadTgaRgba8(path, maxDimension, out, error);
  }
  static bool s_wicComInitialized = false;
  if (!s_wicComInitialized) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
      if (error) *error = "COM init failed hr=" + FormatHr(coHr);
      return false;
    }
    s_wicComInitialized = true;
  }
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (error) *error = "WIC factory failed hr=" + FormatHr(hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromFilename(path.wstring().c_str(),
                                          nullptr,
                                          GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand,
                                          &decoder);
  if (FAILED(hr)) {
    if (error) *error = "WIC decoder failed for " + path.generic_string() + " hr=" + FormatHr(hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    if (error) *error = "WIC first frame failed hr=" + FormatHr(hr);
    return false;
  }

  UINT sourceWidth = 0u;
  UINT sourceHeight = 0u;
  hr = frame->GetSize(&sourceWidth, &sourceHeight);
  if (FAILED(hr) || sourceWidth == 0u || sourceHeight == 0u) {
    if (error) *error = "WIC texture has invalid dimensions";
    return false;
  }

  UINT targetWidth = sourceWidth;
  UINT targetHeight = sourceHeight;
  const UINT largest = std::max(sourceWidth, sourceHeight);
  if (largest > maxDimension && maxDimension > 0u) {
    targetWidth = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(sourceWidth) * maxDimension) / largest));
    targetHeight = std::max(1u, static_cast<UINT>((static_cast<uint64_t>(sourceHeight) * maxDimension) / largest));
  }

  Microsoft::WRL::ComPtr<IWICBitmapSource> source;
  hr = frame.As(&source);
  if (FAILED(hr)) {
    if (error) *error = "WIC source conversion failed hr=" + FormatHr(hr);
    return false;
  }
  if (targetWidth != sourceWidth || targetHeight != sourceHeight) {
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) {
      hr = scaler->Initialize(source.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
    }
    if (FAILED(hr)) {
      if (error) *error = "WIC scaler failed hr=" + FormatHr(hr);
      return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapSource> scaledSource;
    hr = scaler.As(&scaledSource);
    if (FAILED(hr)) {
      if (error) *error = "WIC scaler source conversion failed hr=" + FormatHr(hr);
      return false;
    }
    source = scaledSource;
  }

  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(source.Get(),
                               GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone,
                               nullptr,
                               0.0,
                               WICBitmapPaletteTypeCustom);
  }
  if (FAILED(hr)) {
    if (error) *error = "WIC RGBA conversion failed hr=" + FormatHr(hr);
    return false;
  }

  const UINT stride = targetWidth * 4u;
  std::vector<uint8_t> rgba(static_cast<std::size_t>(stride) * targetHeight);
  hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(rgba.size()), rgba.data());
  if (FAILED(hr)) {
    if (error) *error = "WIC CopyPixels failed hr=" + FormatHr(hr);
    return false;
  }

  out.width = targetWidth;
  out.height = targetHeight;
  out.texels.resize(static_cast<std::size_t>(targetWidth) * targetHeight);
  for (std::size_t i = 0; i < out.texels.size(); ++i) {
    const std::size_t b = i * 4u;
    out.texels[i] = static_cast<uint32_t>(rgba[b + 0u]) |
                    (static_cast<uint32_t>(rgba[b + 1u]) << 8u) |
                    (static_cast<uint32_t>(rgba[b + 2u]) << 16u) |
                    (static_cast<uint32_t>(rgba[b + 3u]) << 24u);
  }

  return true;
}

template <typename T>
std::string FormatFirstN(const T* values, size_t count, size_t max_values = 4u) {
  const size_t shown = std::min(count, max_values);
  std::ostringstream ss;
  for (size_t i = 0; i < shown; ++i) {
    if (i) ss << ", ";
    ss << values[i];
  }
  if (count > shown) {
    ss << "...";
  }
  return ss.str();
}

uint32_t ParseEnvU32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue) {
  std::string valueText;
#if defined(_WIN32)
  char* valueBuffer = nullptr;
  size_t valueLength = 0u;
  if (_dupenv_s(&valueBuffer, &valueLength, name) == 0 && valueBuffer != nullptr) {
    valueText.assign(valueBuffer, valueLength > 0u ? valueLength - 1u : 0u);
    std::free(valueBuffer);
  }
#else
  if (const char* value = std::getenv(name)) {
    valueText = value;
  }
#endif
  if (valueText.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(valueText.c_str(), &end, 10);
  if (end == valueText.c_str() || *end != '\0' || parsed < minValue || parsed > maxValue) {
    LogError(std::string("ignoring invalid ") + name + "=" + valueText +
             " (expected " + std::to_string(minValue) + ".." + std::to_string(maxValue) + ")");
    return fallback;
  }
  return static_cast<uint32_t>(parsed);
}

std::string ReadEnvString(const char* name) {
  std::string valueText;
#if defined(_WIN32)
  char* valueBuffer = nullptr;
  size_t valueLength = 0u;
  if (_dupenv_s(&valueBuffer, &valueLength, name) == 0 && valueBuffer != nullptr) {
    valueText.assign(valueBuffer, valueLength > 0u ? valueLength - 1u : 0u);
    std::free(valueBuffer);
  }
#else
  if (const char* value = std::getenv(name)) {
    valueText = value;
  }
#endif
  std::transform(valueText.begin(), valueText.end(), valueText.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return valueText;
}

bool ParseEnvBool(const char* name, bool fallback) {
  const std::string valueText = ReadEnvString(name);
  if (valueText.empty()) {
    return fallback;
  }
  if (valueText == "1" || valueText == "true" || valueText == "yes" || valueText == "on") {
    return true;
  }
  if (valueText == "0" || valueText == "false" || valueText == "no" || valueText == "off") {
    return false;
  }
  LogError(std::string("ignoring invalid ") + name + "=" + valueText +
           " (expected 0/1, true/false, yes/no, or on/off)");
  return fallback;
}

bool D3D12VerboseLoggingEnabled() {
  static const bool enabled = ParseEnvBool("PT_D3D12_VERBOSE_LOG", false);
  return enabled;
}

std::string SelectDxrBuildMode() {
  const std::string valueText = ReadEnvString("PT_D3D12_DXR_BUILD_MODE");
  if (valueText.empty()) {
    return "fast_trace";
  }
  if (valueText == "fast_trace" || valueText == "fast_build" || valueText == "none") {
    return valueText;
  }
  LogError("ignoring invalid PT_D3D12_DXR_BUILD_MODE=" + valueText +
           " (expected fast_trace, fast_build, or none)");
  return "fast_trace";
}

uint32_t SelectBvhLeafSize() {
  return ParseEnvU32("PT_D3D12_BVH_LEAF_SIZE", 16u, 1u, 16u);
}

uint32_t SelectBvhBucketCount() {
  const uint32_t value = ParseEnvU32("PT_D3D12_BVH_BUCKETS", 16u, 2u, 16u);
  if (value == 4u || value == 8u || value == 16u) {
    return value;
  }
  LogError("ignoring unsupported PT_D3D12_BVH_BUCKETS=" + std::to_string(value) +
           " (expected 4, 8, or 16)");
  return 16u;
}

std::string SelectBvhSplitMode() {
  const std::string valueText = ReadEnvString("PT_D3D12_BVH_SPLIT_MODE");
  if (valueText.empty()) {
    return "sah";
  }
  if (valueText == "sah" || valueText == "median") {
    return valueText;
  }
  LogError("ignoring invalid PT_D3D12_BVH_SPLIT_MODE=" + valueText +
           " (expected sah or median)");
  return "sah";
}

std::string SelectShaderTraversalMode() {
  const std::string valueText = ReadEnvString("PT_D3D12_SHADER_TRAVERSAL");
  if (valueText.empty()) {
    return "baseline";
  }
  if (valueText == "baseline" || valueText == "bounds_helper" || valueText == "near_order") {
    return valueText;
  }
  LogError("ignoring invalid PT_D3D12_SHADER_TRAVERSAL=" + valueText +
           " (expected baseline, bounds_helper, or near_order)");
  return "baseline";
}

const char* ShaderTraversalDefine(const std::string& mode) {
  if (mode == "bounds_helper") {
    return "1";
  }
  if (mode == "near_order") {
    return "2";
  }
  return "0";
}

bool SelectPackedTriangleBuffer() {
  return ParseEnvBool("PT_D3D12_PACKED_TRIANGLES", true);
}

D3D12_COMMAND_LIST_TYPE SelectComputeCommandListType() {
  const std::string valueText = ReadEnvString("PT_D3D12_COMMAND_QUEUE");
  if (valueText.empty() || valueText == "compute") {
    return D3D12_COMMAND_LIST_TYPE_COMPUTE;
  }
  if (valueText == "direct" || valueText == "graphics") {
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
  }
  LogError("ignoring invalid PT_D3D12_COMMAND_QUEUE=" + valueText +
           " (expected compute or direct)");
  return D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

const char* CommandListTypeName(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute";
}

const char* BoolDefine(bool value) {
  return value ? "1" : "0";
}

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS DxrBuildPreferenceFlags(
    const std::string& mode) {
  if (mode == "fast_build") {
    return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  }
  if (mode == "none") {
    return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  }
  return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
}

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS AddDxrBuildFlags(
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS lhs,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS rhs) {
  return static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
      static_cast<UINT>(lhs) | static_cast<UINT>(rhs));
}

uint32_t SelectRaysPerPixelPerDispatch(const vkpt::pathtracer::RenderSettings& settings) {
  // Finite/offline renders should preserve the public --spp contract. Interactive
  // preview uses an unlimited spp sentinel and can batch more rays per GPU submit.
  const bool interactivePreview = settings.spp == std::numeric_limits<uint32_t>::max();
  const uint32_t fallback = interactivePreview ? 8u : 1u;
  return ParseEnvU32("PT_D3D12_RAYS_PER_PIXEL", fallback, 1u, 64u);
}

uint32_t SelectReadbackInterval(const vkpt::pathtracer::RenderSettings& settings) {
  const bool interactivePreview = settings.spp == std::numeric_limits<uint32_t>::max();
  const uint32_t fallback = interactivePreview ? 4u : 1u;
  return ParseEnvU32("PT_D3D12_READBACK_INTERVAL", fallback, 1u, 64u);
}

uint64_t SaturatingMulU64(uint64_t lhs, uint64_t rhs) {
  if (lhs == 0u || rhs == 0u) {
    return 0u;
  }
  if (lhs > std::numeric_limits<uint64_t>::max() / rhs) {
    return std::numeric_limits<uint64_t>::max();
  }
  return lhs * rhs;
}

uint64_t EstimateLogicalRaysPerD3D12Sample(const vkpt::pathtracer::RenderSettings& settings,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           bool usingDxrDispatch) {
  const uint64_t maxDepth = static_cast<uint64_t>(std::max(1u, settings.max_depth));
  uint64_t raysPerSample = maxDepth;

  // The GPU shaders do not currently write exact per-ray counters. Count the
  // primary/continuation scene query at each path depth, plus the direct-light
  // shadow query that both the compute shader and default DXR shader issue.
  const bool hasDirectLight = !scene.lights.empty();
  const bool shadowQueriesEnabled = hasDirectLight &&
      (!usingDxrDispatch || ParseEnvBool("PT_D3D12_DXR_SHADOW_RAYS", true));
  if (shadowQueriesEnabled) {
    raysPerSample += maxDepth;
  }
  return std::max<uint64_t>(1u, raysPerSample);
}

D3D12_UNORDERED_ACCESS_VIEW_DESC MakeRawBufferUavDesc(UINT64 byteSize) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = static_cast<UINT>(byteSize / sizeof(uint32_t));
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  return uav;
}

D3D12TemporalCameraState MakeTemporalCameraState(const PathTraceConstants& pc) {
  D3D12TemporalCameraState state{};
  state.camera_pos_x = pc.camera_pos_x;
  state.camera_pos_y = pc.camera_pos_y;
  state.camera_pos_z = pc.camera_pos_z;
  state.fov_tan_half = pc.fov_tan_half;
  state.cam_fwd_x = pc.cam_fwd_x;
  state.cam_fwd_y = pc.cam_fwd_y;
  state.cam_fwd_z = pc.cam_fwd_z;
  state.aspect = pc.aspect;
  state.cam_right_x = pc.cam_right_x;
  state.cam_right_y = pc.cam_right_y;
  state.cam_right_z = pc.cam_right_z;
  state.cam_up_x = pc.cam_up_x;
  state.cam_up_y = pc.cam_up_y;
  state.cam_up_z = pc.cam_up_z;
  return state;
}

void FillPreviousCameraConstants(PathTraceConstants& pc, const D3D12TemporalCameraState& state) {
  pc.prev_camera_pos_x = state.camera_pos_x;
  pc.prev_camera_pos_y = state.camera_pos_y;
  pc.prev_camera_pos_z = state.camera_pos_z;
  pc.prev_fov_tan_half = state.fov_tan_half;
  pc.prev_cam_fwd_x = state.cam_fwd_x;
  pc.prev_cam_fwd_y = state.cam_fwd_y;
  pc.prev_cam_fwd_z = state.cam_fwd_z;
  pc.prev_aspect = state.aspect;
  pc.prev_cam_right_x = state.cam_right_x;
  pc.prev_cam_right_y = state.cam_right_y;
  pc.prev_cam_right_z = state.cam_right_z;
  pc.prev_cam_up_x = state.cam_up_x;
  pc.prev_cam_up_y = state.cam_up_y;
  pc.prev_cam_up_z = state.cam_up_z;
}

D3D12_RESOURCE_BARRIER MakeTransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return barrier;
}

constexpr uint32_t kGpuInstanceStrideU32 = 24u;

uint32_t FloatBits(float value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float UintBitsToFloat(uint32_t value) {
  float bits = 0.0f;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

D3D12_RAYTRACING_INSTANCE_DESC MakeDxrInstanceDesc(
    uint32_t instanceId,
    D3D12_GPU_VIRTUAL_ADDRESS blasAddress,
    const vkpt::pathtracer::Vec3& translation,
    vkpt::pathtracer::Quat4 rotation,
    const vkpt::pathtracer::Vec3& scale) {
  const float len2 = rotation.x * rotation.x + rotation.y * rotation.y +
                     rotation.z * rotation.z + rotation.w * rotation.w;
  if (len2 > 1.0e-12f) {
    const float invLen = 1.0f / std::sqrt(len2);
    rotation.x *= invLen;
    rotation.y *= invLen;
    rotation.z *= invLen;
    rotation.w *= invLen;
  } else {
    rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  }

  const float x = rotation.x;
  const float y = rotation.y;
  const float z = rotation.z;
  const float w = rotation.w;
  const float xx = x * x;
  const float yy = y * y;
  const float zz = z * z;
  const float xy = x * y;
  const float xz = x * z;
  const float yz = y * z;
  const float wx = w * x;
  const float wy = w * y;
  const float wz = w * z;

  D3D12_RAYTRACING_INSTANCE_DESC desc{};
  desc.Transform[0][0] = (1.0f - 2.0f * (yy + zz)) * scale.x;
  desc.Transform[0][1] = (2.0f * (xy - wz)) * scale.y;
  desc.Transform[0][2] = (2.0f * (xz + wy)) * scale.z;
  desc.Transform[0][3] = translation.x;
  desc.Transform[1][0] = (2.0f * (xy + wz)) * scale.x;
  desc.Transform[1][1] = (1.0f - 2.0f * (xx + zz)) * scale.y;
  desc.Transform[1][2] = (2.0f * (yz - wx)) * scale.z;
  desc.Transform[1][3] = translation.y;
  desc.Transform[2][0] = (2.0f * (xz - wy)) * scale.x;
  desc.Transform[2][1] = (2.0f * (yz + wx)) * scale.y;
  desc.Transform[2][2] = (1.0f - 2.0f * (xx + yy)) * scale.z;
  desc.Transform[2][3] = translation.z;
  desc.InstanceID = instanceId & 0x00ffffffu;
  desc.InstanceMask = 0xffu;
  desc.InstanceContributionToHitGroupIndex = 0u;
  desc.Flags = 0u;
  desc.AccelerationStructure = blasAddress;
  return desc;
}

void StoreEmptyBvh(std::vector<float>& gpu_bvh) {
  gpu_bvh.assign(8u, 0.0f);
  gpu_bvh[6u] = UintBitsToFloat(0x80000000u);
  gpu_bvh[7u] = UintBitsToFloat(0u);
}

// ============================================================================
// BVH builder types and recursive construction
// ============================================================================
struct BvhTriRef {
  uint32_t orig_idx;     // original triangle index into flat index buffer
  float    centroid[3];
  float    bmin[3], bmax[3];
};

struct DynamicInstanceRef {
  uint32_t instance_index = 0u;
  float centroid[3]{};
  float bmin[3]{};
  float bmax[3]{};
};

struct BvhBuildConfig {
  uint32_t leaf_size = 4u;
  uint32_t bucket_count = 8u;
  bool use_median_split = false;
};

static float bvh_sa(const float bmin[3], const float bmax[3]) {
  float dx = bmax[0]-bmin[0], dy = bmax[1]-bmin[1], dz = bmax[2]-bmin[2];
  if (dx < 0) dx = 0; if (dy < 0) dy = 0; if (dz < 0) dz = 0;
  return 2.0f * (dx*dy + dy*dz + dz*dx);
}

// Writes 8 floats per node into gpu_bvh (pre-sized to max_nodes*8).
// Reorders triangles into reord_idx / reord_tri_mat for cache-friendly leaf access.
// Returns node index of the root.
static uint32_t bvh_build(
  std::vector<BvhTriRef>&        refs,
  uint32_t start, uint32_t count,
  const std::vector<uint32_t>&   orig_idx,
  const std::vector<uint32_t>&   orig_tri_mat,
  const BvhBuildConfig&          config,
  std::vector<float>&            gpu_bvh,
  std::vector<uint32_t>&         reord_idx,
  std::vector<uint32_t>&         reord_tri_mat,
  uint32_t&                      node_count)
{
  auto as_fb = [](uint32_t u) -> float {
    float f; std::memcpy(&f, &u, sizeof(f)); return f;
  };

  const uint32_t ni   = node_count++;
  const uint32_t base = ni * 8u;

  // Compute node AABB
  float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t i = start; i < start + count; ++i) {
    for (int k = 0; k < 3; ++k) {
      bmin[k] = std::min(bmin[k], refs[i].bmin[k]);
      bmax[k] = std::max(bmax[k], refs[i].bmax[k]);
    }
  }
  gpu_bvh[base+0u]=bmin[0]; gpu_bvh[base+1u]=bmin[1]; gpu_bvh[base+2u]=bmin[2];
  gpu_bvh[base+3u]=bmax[0]; gpu_bvh[base+4u]=bmax[1]; gpu_bvh[base+5u]=bmax[2];

  if (count <= config.leaf_size) {
    const uint32_t first_tri = static_cast<uint32_t>(reord_idx.size() / 3u);
    for (uint32_t i = start; i < start + count; ++i) {
      const uint32_t t = refs[i].orig_idx;
      reord_idx.push_back(orig_idx[t*3u+0u]);
      reord_idx.push_back(orig_idx[t*3u+1u]);
      reord_idx.push_back(orig_idx[t*3u+2u]);
      reord_tri_mat.push_back(orig_tri_mat[t]);
    }
    gpu_bvh[base+6u] = as_fb(0x80000000u | first_tri); // leaf flag in bit 31
    gpu_bvh[base+7u] = as_fb(count);
    return ni;
  }

  if (config.use_median_split) {
    float cmin[3] = {1e30f, 1e30f, 1e30f};
    float cmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = start; i < start + count; ++i) {
      for (int k = 0; k < 3; ++k) {
        cmin[k] = std::min(cmin[k], refs[i].centroid[k]);
        cmax[k] = std::max(cmax[k], refs[i].centroid[k]);
      }
    }
    float extent[3] = {cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2]};
    int axis = 0;
    if (extent[1] > extent[axis]) axis = 1;
    if (extent[2] > extent[axis]) axis = 2;
    const uint32_t mid = start + count / 2u;
    std::nth_element(refs.begin() + start, refs.begin() + mid, refs.begin() + start + count,
      [axis](const BvhTriRef& a, const BvhTriRef& b) {
        return a.centroid[axis] < b.centroid[axis];
      });

    const uint32_t left_child  = bvh_build(refs, start, mid - start,
                        orig_idx, orig_tri_mat, config,
                        gpu_bvh, reord_idx, reord_tri_mat, node_count);
    const uint32_t right_child = bvh_build(refs, mid, (start + count) - mid,
                        orig_idx, orig_tri_mat, config,
                        gpu_bvh, reord_idx, reord_tri_mat, node_count);
    gpu_bvh[base+6u] = as_fb(left_child);
    gpu_bvh[base+7u] = as_fb(right_child);
    return ni;
  }

  // SAH split using a configurable bucket count along each axis.
  constexpr int kMaxBvhBuckets = 16;
  const int bucket_count = static_cast<int>(std::clamp(config.bucket_count, 2u, 16u));
  const float node_sa_v = std::max(1e-9f, bvh_sa(bmin, bmax));
  int   best_axis      = 0;
  float best_split_val = 0.0f;
  float best_cost      = 1e30f;
  bool  found_split    = false;

  for (int axis = 0; axis < 3; ++axis) {
    float cmin = 1e30f, cmax = -1e30f;
    for (uint32_t i = start; i < start + count; ++i) {
      cmin = std::min(cmin, refs[i].centroid[axis]);
      cmax = std::max(cmax, refs[i].centroid[axis]);
    }
    const float extent = cmax - cmin;
    if (extent < 1e-6f) continue;

    struct Bkt { float mn[3], mx[3]; uint32_t n; };
    Bkt bkts[kMaxBvhBuckets];
    for (int b = 0; b < bucket_count; ++b) {
      bkts[b].n = 0;
      for (int k=0;k<3;++k) { bkts[b].mn[k]=1e30f; bkts[b].mx[k]=-1e30f; }
    }
    for (uint32_t i = start; i < start + count; ++i) {
      const int b = std::min(bucket_count - 1,
                             static_cast<int>((refs[i].centroid[axis] - cmin) / extent *
                                              static_cast<float>(bucket_count)));
      bkts[b].n++;
      for (int k=0;k<3;++k) {
        bkts[b].mn[k] = std::min(bkts[b].mn[k], refs[i].bmin[k]);
        bkts[b].mx[k] = std::max(bkts[b].mx[k], refs[i].bmax[k]);
      }
    }
    for (int s = 1; s < bucket_count; ++s) {
      float lmn[3]={1e30f,1e30f,1e30f}, lmx[3]={-1e30f,-1e30f,-1e30f};
      float rmn[3]={1e30f,1e30f,1e30f}, rmx[3]={-1e30f,-1e30f,-1e30f};
      uint32_t lc = 0, rc = 0;
      for (int b=0;b<s;++b) {
        if (!bkts[b].n) continue;
        lc += bkts[b].n;
        for (int k=0;k<3;++k) { lmn[k]=std::min(lmn[k],bkts[b].mn[k]); lmx[k]=std::max(lmx[k],bkts[b].mx[k]); }
      }
      for (int b=s;b<bucket_count;++b) {
        if (!bkts[b].n) continue;
        rc += bkts[b].n;
        for (int k=0;k<3;++k) { rmn[k]=std::min(rmn[k],bkts[b].mn[k]); rmx[k]=std::max(rmx[k],bkts[b].mx[k]); }
      }
      if (!lc || !rc) continue;
      const float cost = (bvh_sa(lmn,lmx)*(float)lc + bvh_sa(rmn,rmx)*(float)rc) / node_sa_v;
      if (cost < best_cost) {
        best_cost      = cost;
        best_axis      = axis;
        best_split_val = cmin + static_cast<float>(s) / static_cast<float>(bucket_count) * extent;
        found_split    = true;
      }
    }
  }

  // Partition refs around the best split
  uint32_t mid;
  if (found_split) {
    const int   ax  = best_axis;
    const float spv = best_split_val;
    auto it = std::stable_partition(
      refs.begin() + start, refs.begin() + start + count,
      [ax, spv](const BvhTriRef& r) { return r.centroid[ax] < spv; });
    mid = static_cast<uint32_t>(it - refs.begin());
    if (mid <= start || mid >= start + count) mid = start + count / 2u;
  } else {
    mid = start + count / 2u;
  }

  const uint32_t left_child  = bvh_build(refs, start, mid - start,
                      orig_idx, orig_tri_mat, config,
                      gpu_bvh, reord_idx, reord_tri_mat, node_count);
  const uint32_t right_child = bvh_build(refs, mid, (start + count) - mid,
                      orig_idx, orig_tri_mat, config,
                      gpu_bvh, reord_idx, reord_tri_mat, node_count);

  // Write internal node children (bit 31 = 0 means internal)
  gpu_bvh[base+6u] = as_fb(left_child);
  gpu_bvh[base+7u] = as_fb(right_child);
  return ni;
}

static uint32_t dynamic_bvh_build(
  std::vector<DynamicInstanceRef>& refs,
  uint32_t start,
  uint32_t count,
  std::vector<float>& gpu_bvh,
  uint32_t& node_count)
{
  auto as_fb = [](uint32_t u) -> float {
    float f; std::memcpy(&f, &u, sizeof(f)); return f;
  };

  const uint32_t ni = node_count++;
  const uint32_t base = ni * 8u;
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t i = start; i < start + count; ++i) {
    for (int k = 0; k < 3; ++k) {
      bmin[k] = std::min(bmin[k], refs[i].bmin[k]);
      bmax[k] = std::max(bmax[k], refs[i].bmax[k]);
    }
  }
  gpu_bvh[base+0u]=bmin[0]; gpu_bvh[base+1u]=bmin[1]; gpu_bvh[base+2u]=bmin[2];
  gpu_bvh[base+3u]=bmax[0]; gpu_bvh[base+4u]=bmax[1]; gpu_bvh[base+5u]=bmax[2];

  if (count <= 1u) {
    gpu_bvh[base+6u] = as_fb(0x80000000u | refs[start].instance_index);
    gpu_bvh[base+7u] = as_fb(1u);
    return ni;
  }

  float extent[3] = {bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2]};
  int axis = 0;
  if (extent[1] > extent[axis]) axis = 1;
  if (extent[2] > extent[axis]) axis = 2;
  const uint32_t mid = start + count / 2u;
  std::nth_element(refs.begin() + start, refs.begin() + mid, refs.begin() + start + count,
                   [axis](const DynamicInstanceRef& lhs, const DynamicInstanceRef& rhs) {
                     return lhs.centroid[axis] < rhs.centroid[axis];
                   });

  const uint32_t left = dynamic_bvh_build(refs, start, mid - start, gpu_bvh, node_count);
  const uint32_t right = dynamic_bvh_build(refs, mid, start + count - mid, gpu_bvh, node_count);
  gpu_bvh[base+6u] = as_fb(left);
  gpu_bvh[base+7u] = as_fb(right);
  return ni;
}

vkpt::pathtracer::Vec3 Cross(const vkpt::pathtracer::Vec3& a, const vkpt::pathtracer::Vec3& b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

vkpt::pathtracer::Vec3 RotateQuat(const vkpt::pathtracer::Vec3& value,
                                  vkpt::pathtracer::Quat4 q) {
  const float len2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
  if (len2 > 1.0e-12f) {
    const float invLen = 1.0f / std::sqrt(len2);
    q.x *= invLen; q.y *= invLen; q.z *= invLen; q.w *= invLen;
  } else {
    q = {};
  }
  const vkpt::pathtracer::Vec3 qv{q.x, q.y, q.z};
  const auto t = Cross(qv, value);
  const vkpt::pathtracer::Vec3 doubled{t.x * 2.0f, t.y * 2.0f, t.z * 2.0f};
  const auto c = Cross(qv, doubled);
  return {value.x + doubled.x * q.w + c.x,
          value.y + doubled.y * q.w + c.y,
          value.z + doubled.z * q.w + c.z};
}

uint32_t BuildDynamicInstanceBvhFromPackedInstances(const std::vector<uint32_t>& insts,
                                                    uint32_t instanceCount,
                                                    std::vector<float>& outBvh) {
  std::vector<DynamicInstanceRef> refs;
  refs.reserve(instanceCount);
  for (uint32_t instanceIndex = 0u; instanceIndex < instanceCount; ++instanceIndex) {
    const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    if (ib + 22u >= insts.size()) {
      continue;
    }
    const uint32_t flags = insts[ib + 3u];
    const uint32_t triCount = insts[ib + 1u];
    if ((flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u || triCount == 0u) {
      continue;
    }
    const vkpt::pathtracer::Vec3 translation{
        UintBitsToFloat(insts[ib + 4u]),
        UintBitsToFloat(insts[ib + 5u]),
        UintBitsToFloat(insts[ib + 6u])};
    const vkpt::pathtracer::Quat4 rotation{
        UintBitsToFloat(insts[ib + 8u]),
        UintBitsToFloat(insts[ib + 9u]),
        UintBitsToFloat(insts[ib + 10u]),
        UintBitsToFloat(insts[ib + 11u])};
    const vkpt::pathtracer::Vec3 scale{
        UintBitsToFloat(insts[ib + 12u]),
        UintBitsToFloat(insts[ib + 13u]),
        UintBitsToFloat(insts[ib + 14u])};
    const vkpt::pathtracer::Vec3 localMin{
        UintBitsToFloat(insts[ib + 16u]),
        UintBitsToFloat(insts[ib + 17u]),
        UintBitsToFloat(insts[ib + 18u])};
    const vkpt::pathtracer::Vec3 localMax{
        UintBitsToFloat(insts[ib + 20u]),
        UintBitsToFloat(insts[ib + 21u]),
        UintBitsToFloat(insts[ib + 22u])};

    DynamicInstanceRef ref{};
    ref.instance_index = instanceIndex;
    for (int k = 0; k < 3; ++k) {
      ref.bmin[k] = 1e30f;
      ref.bmax[k] = -1e30f;
    }
    for (uint32_t corner = 0u; corner < 8u; ++corner) {
      const vkpt::pathtracer::Vec3 local{
          (corner & 1u) ? localMax.x : localMin.x,
          (corner & 2u) ? localMax.y : localMin.y,
          (corner & 4u) ? localMax.z : localMin.z};
      const vkpt::pathtracer::Vec3 scaled{local.x * scale.x, local.y * scale.y, local.z * scale.z};
      const auto rotated = RotateQuat(scaled, rotation);
      const vkpt::pathtracer::Vec3 world{
          rotated.x + translation.x,
          rotated.y + translation.y,
          rotated.z + translation.z};
      ref.bmin[0] = std::min(ref.bmin[0], world.x);
      ref.bmin[1] = std::min(ref.bmin[1], world.y);
      ref.bmin[2] = std::min(ref.bmin[2], world.z);
      ref.bmax[0] = std::max(ref.bmax[0], world.x);
      ref.bmax[1] = std::max(ref.bmax[1], world.y);
      ref.bmax[2] = std::max(ref.bmax[2], world.z);
    }
    ref.centroid[0] = (ref.bmin[0] + ref.bmax[0]) * 0.5f;
    ref.centroid[1] = (ref.bmin[1] + ref.bmax[1]) * 0.5f;
    ref.centroid[2] = (ref.bmin[2] + ref.bmax[2]) * 0.5f;
    refs.push_back(ref);
  }

  if (refs.empty()) {
    StoreEmptyBvh(outBvh);
    return 0u;
  }
  outBvh.assign(std::max<std::size_t>(1u, refs.size() * 2u) * 8u, 0.0f);
  uint32_t nodeCount = 0u;
  dynamic_bvh_build(refs, 0u, static_cast<uint32_t>(refs.size()), outBvh, nodeCount);
  outBvh.resize(static_cast<std::size_t>(nodeCount) * 8u);
  return static_cast<uint32_t>(refs.size());
}

} // namespace

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point)
    : m_hlslPath(std::move(hlsl_path)), m_entryPoint(std::move(entry_point)) {
  m_shaderTraversalMode = SelectShaderTraversalMode();
  m_packedTriangleBufferEnabled = SelectPackedTriangleBuffer();
  LogDebug("D3D12 tracer ctor hlsl=" + m_hlslPath + " entry=" + m_entryPoint +
           " shader_traversal=" + m_shaderTraversalMode +
           " packed_triangles=" + (m_packedTriangleBufferEnabled ? "true" : "false"));
  m_valid = init_device() && create_root_sig_and_pso();
  if (!m_valid) LogError("D3D12GpuPathTracer init failed: " + m_error);
}

D3D12GpuPathTracer::~D3D12GpuPathTracer() { shutdown(); }

std::string D3D12GpuPathTracer::dxr_tier_string() const {
  if (m_dxrTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
    return "not_supported";
  }
  if (m_dxrTier >= D3D12_RAYTRACING_TIER_1_0) {
    // Some SDK/header combos may expose newer tiers without named enum labels.
    // Treat any tier >= 1.1 as "1.1+" to avoid contradictory reporting.
    if (m_dxrTier > D3D12_RAYTRACING_TIER_1_0) {
      return "1.1+";
    }
    return "1.0";
  }
  return "unknown";
}

void D3D12GpuPathTracer::set_prefer_dxr(bool enabled) {
  const bool wasEnabled = m_preferDxr;
  m_preferDxr = enabled;
  m_usingDxrDispatch = false;
  m_loggedDxrFallback = false;
  if (enabled && !wasEnabled && m_sceneUploaded) {
    const std::size_t expectedTriDataFloats =
        static_cast<std::size_t>(m_gpuIdx.size() / 3u) * kGpuTriDataStrideFloats;
    if (m_gpuTriData.size() < expectedTriDataFloats) {
      m_sceneUploaded = false;
      m_dxrAccelReady = false;
    }
  }
  if (enabled && m_dxrSupported && !m_device5) {
    if (FAILED(m_device.As(&m_device5))) {
      m_dxrSupported = false;
      m_dxrTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      LogError("set_prefer_dxr: ID3D12Device5 query failed");
    }
  }
  if (enabled && m_dxrSupported && !m_dxrRuntimeObjectsReady) {
    if (!init_dxr_runtime_objects()) {
      LogError("set_prefer_dxr: runtime object init failed: " + m_error);
    }
  }
  if (enabled && m_dxrRuntimeObjectsReady && !m_dxrPipelineReady) {
    // Derive RT shader path: replace _cs.hlsl -> _rt.hlsl
    m_rtHlslPath = m_hlslPath;
    auto pos = m_rtHlslPath.rfind("_cs.hlsl");
    if (pos != std::string::npos) {
      m_rtHlslPath.replace(pos, 8, "_rt.hlsl");
    } else {
      auto extpos = m_rtHlslPath.rfind(".hlsl");
      if (extpos != std::string::npos)
        m_rtHlslPath.replace(extpos, 5, "_rt.hlsl");
      else
        m_rtHlslPath += "_rt.hlsl";
    }
    LogInfo("set_prefer_dxr: rt_shader=" + m_rtHlslPath);
    if (!create_dxr_pipeline()) {
      LogError("set_prefer_dxr: pipeline creation failed: " + m_error);
    }
  }
}

// ============================================================================
// IPathTracer
// ============================================================================

bool D3D12GpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) {
    m_error = "configure before init";
    LogError("configure rejected: " + m_error);
    return false;
  }
  if (s.width == 0u || s.height == 0u) {
    m_error = "configure invalid dimensions";
    LogError("configure rejected: " + m_error);
    return false;
  }
  m_settings   = s;
  m_configured = true;
  m_filmPixels = s.width * s.height;
  m_film.resize(s.width, s.height);
  m_film.set_resolve_settings(s.film_resolve);
  m_film.clear();
  m_counters          = {};
  m_hasScene          = false;
  m_sceneUploaded     = false;
  m_raysPerPixelPerDispatch = SelectRaysPerPixelPerDispatch(s);
  m_readbackInterval = SelectReadbackInterval(s);
  m_forceReadbackEverySample = ParseEnvBool("PT_D3D12_FORCE_READBACK_EVERY_SAMPLE", false);
  m_dynamicInstanceTransformsAllowed = ParseEnvBool("PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS", true);
  m_temporalHistoryValid = false;
  m_dxrBuildMode = SelectDxrBuildMode();
  m_bvhLeafSize = SelectBvhLeafSize();
  m_bvhBucketCount = SelectBvhBucketCount();
  m_bvhSplitMode = SelectBvhSplitMode();
  std::ostringstream cfg;
  cfg << "configure width=" << s.width << " height=" << s.height
      << " spp=" << s.spp
      << " max_depth=" << s.max_depth << " seed=" << s.seed
      << " gpu_denoiser=" << (s.enable_denoiser ? "true" : "false")
      << " temporal_aa=" << (s.enable_temporal_aa ? "true" : "false")
      << " rays_per_pixel_per_dispatch=" << m_raysPerPixelPerDispatch
      << " readback_interval=" << m_readbackInterval
      << " force_readback_every_sample=" << (m_forceReadbackEverySample ? "true" : "false")
      << " dynamic_instance_transforms=" << (m_dynamicInstanceTransformsAllowed ? "true" : "false")
      << " dxr_build_mode=" << m_dxrBuildMode
      << " bvh_leaf_size=" << m_bvhLeafSize
      << " bvh_bucket_count=" << m_bvhBucketCount
      << " bvh_split_mode=" << m_bvhSplitMode
      << " shader_traversal=" << m_shaderTraversalMode
      << " packed_triangles=" << (m_packedTriangleBufferEnabled ? "true" : "false");
  LogDebug(cfg.str());
  destroy_film_buffer();
  return create_film_buffer();
}

bool D3D12GpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::RTSceneData& scene) {
  if (!m_configured) {
    m_error = "load_scene_snapshot before configure";
    LogError("load_scene_snapshot rejected: " + m_error);
    return false;
  }
  m_sceneData      = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_hasScene       = true;
  m_sceneUploaded  = false;
  m_temporalHistoryValid = false;
  std::ostringstream ss;
  ss << "scene snapshot loaded verts=" << scene.vertices.size()
     << " idx=" << scene.indices.size()
     << " mats=" << scene.materials.size()
     << " inst=" << scene.instances.size()
     << " tess=" << scene.tessellation_requests.size()
     << " lights=" << scene.lights.size();
  LogDebug(ss.str());
  return true;
}

bool D3D12GpuPathTracer::update_camera(
    const vkpt::pathtracer::Vec3& pos,
    const vkpt::pathtracer::Vec3& target,
    const vkpt::pathtracer::Vec3& up,
    float fov_deg) {
  if (!m_sceneUploaded) {
    // Geometry hasn't been uploaded yet — can't update camera independently.
    return false;
  }
  m_sceneData.camera_position = pos;
  m_sceneData.camera_target   = target;
  m_sceneData.camera_up       = up;
  m_sceneData.camera_fov_deg  = fov_deg;
  std::ostringstream ss;
  ss << "update_camera pos=(" << pos.x << "," << pos.y << "," << pos.z
     << ") target=(" << target.x << "," << target.y << "," << target.z << ")";
  LogDebug(ss.str());
  return true;
}

bool D3D12GpuPathTracer::update_instance_transforms(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  if (!m_sceneUploaded || !m_instBuf || updates.empty()) {
    return false;
  }
  if (!m_dynamicInstanceTransformsEnabled) {
    return false;
  }
  if (m_gpuInsts.size() < m_sceneData.instances.size() * kGpuInstanceStrideU32) {
    return false;
  }
  const bool updateDxrTlas = (m_preferDxr || m_usingDxrDispatch);
  if (updateDxrTlas && (!m_dxrAccelReady || m_dxrInstanceDescs.empty())) {
    return false;
  }

  auto findInstanceIndex = [&](const vkpt::pathtracer::RTInstanceTransformUpdate& update) {
    if (update.instance_index < m_sceneData.instances.size()) {
      return update.instance_index;
    }
    if (update.entity_id != 0u) {
      for (std::size_t index = 0; index < m_sceneData.instances.size(); ++index) {
        if (m_sceneData.instances[index].entity_id == update.entity_id) {
          return static_cast<uint32_t>(index);
        }
      }
    }
    return vkpt::pathtracer::kInvalidRTInstanceIndex;
  };

  bool changed = false;
  for (const auto& update : updates) {
    const uint32_t instanceIndex = findInstanceIndex(update);
    if (instanceIndex >= m_sceneData.instances.size()) {
      continue;
    }
    auto& instance = m_sceneData.instances[instanceIndex];
    if (!instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      continue;
    }
    instance.translation = update.translation;
    instance.rotation = update.rotation;
    instance.scale = update.scale;
    instance.flags |= update.flags;
    instance.transform_revision = update.transform_revision;

    const std::size_t base = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    m_gpuInsts[base + 3u] = instance.flags;
    m_gpuInsts[base + 4u] = FloatBits(instance.translation.x);
    m_gpuInsts[base + 5u] = FloatBits(instance.translation.y);
    m_gpuInsts[base + 6u] = FloatBits(instance.translation.z);
    m_gpuInsts[base + 8u] = FloatBits(instance.rotation.x);
    m_gpuInsts[base + 9u] = FloatBits(instance.rotation.y);
    m_gpuInsts[base + 10u] = FloatBits(instance.rotation.z);
    m_gpuInsts[base + 11u] = FloatBits(instance.rotation.w);
    m_gpuInsts[base + 12u] = FloatBits(instance.scale.x);
    m_gpuInsts[base + 13u] = FloatBits(instance.scale.y);
    m_gpuInsts[base + 14u] = FloatBits(instance.scale.z);
    changed = true;
  }

  if (!changed) {
    return false;
  }
  m_dynamicInstanceCount = BuildDynamicInstanceBvhFromPackedInstances(
      m_gpuInsts,
      static_cast<uint32_t>(m_sceneData.instances.size()),
      m_gpuDynamicBvh);
  const bool uploaded = updateDxrTlas ? update_dxr_instance_buffer_and_tlas() : upload_instance_buffer();
  if (uploaded) {
    m_temporalHistoryValid = false;
  }
  return uploaded;
}

bool D3D12GpuPathTracer::build_or_update_acceleration() {
  if (!m_hasScene) {
    m_error = "build_or_update_acceleration before snapshot";
    LogError("build_or_update_acceleration rejected: " + m_error);
    return false;
  }
  m_gpuMats.clear();
  for (const auto& m : m_sceneData.materials) {
    m_gpuMats.push_back(m.albedo.x); m_gpuMats.push_back(m.albedo.y);
    m_gpuMats.push_back(m.albedo.z);
    m_gpuMats.push_back(m.emissive.x); m_gpuMats.push_back(m.emissive.y);
    m_gpuMats.push_back(m.emissive.z);
    m_gpuMats.push_back(m.roughness); m_gpuMats.push_back(static_cast<float>(m.material_model));
    m_gpuMats.push_back(m.metallic); m_gpuMats.push_back(m.ior);
    m_gpuMats.push_back(m.transmission); m_gpuMats.push_back(m.clearcoat);
    m_gpuMats.push_back(m.sheen);
    m_gpuMats.push_back(m.normal_texture_index != kInvalidTextureIndex
                            ? static_cast<float>(m.normal_texture_index + 1u)
                            : 0.0f);
    uint32_t packed_effect = (m.material_effect & 1023u) | ((m.material_flags & 1u) ? 1024u : 0u);
    if (m.base_color_texture_index != kInvalidTextureIndex && m.base_color_texture_index < 8191u) {
      packed_effect |= ((m.base_color_texture_index + 1u) << 11u);
    }
    m_gpuMats.push_back(m.alpha); m_gpuMats.push_back(static_cast<float>(packed_effect));
  }
  if (m_gpuMats.empty()) m_gpuMats.assign(16, 0.0f);

  struct PackedInstance {
    uint32_t first_triangle = 0u;
    uint32_t triangle_count = 0u;
    uint32_t material_index = 0u;
    uint32_t flags = 0u;
    uint32_t local_bvh_first_node = 0u;
    uint32_t local_bvh_node_count = 0u;
    vkpt::pathtracer::Vec3 translation{};
    vkpt::pathtracer::Quat4 rotation{};
    vkpt::pathtracer::Vec3 scale{1.0f, 1.0f, 1.0f};
    vkpt::pathtracer::Vec3 local_bounds_min{};
    vkpt::pathtracer::Vec3 local_bounds_max{};
  };

  m_gpuVerts.clear();
  m_gpuTexcoords.clear();
  m_gpuIdx.clear();
  m_gpuTriData.clear();
  m_gpuInsts.clear();
  m_gpuLocalBvh.clear();
  m_dynamicInstanceTransformsEnabled = false;
  m_staticTriangleCount = 0u;
  std::vector<PackedInstance> packedInstances(m_sceneData.instances.size());
  std::vector<uint32_t> staticTriMat;
  const BvhBuildConfig bvhConfig{
      m_bvhLeafSize,
      m_bvhBucketCount,
      m_bvhSplitMode == "median"};

  auto append_vertex = [&](const vkpt::pathtracer::Vec3& vertex, uint32_t sourceIndex = kInvalidTextureIndex) {
    const uint32_t out = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    m_gpuVerts.push_back(vertex.x);
    m_gpuVerts.push_back(vertex.y);
    m_gpuVerts.push_back(vertex.z);
    if (sourceIndex < m_sceneData.texcoords.size()) {
      const auto& uv = m_sceneData.texcoords[sourceIndex];
      m_gpuTexcoords.push_back(uv.u);
      m_gpuTexcoords.push_back(uv.v);
    } else {
      m_gpuTexcoords.push_back(0.0f);
      m_gpuTexcoords.push_back(0.0f);
    }
    return out;
  };
  auto expand_bounds = [](vkpt::pathtracer::Vec3& bmin,
                          vkpt::pathtracer::Vec3& bmax,
                          const vkpt::pathtracer::Vec3& point,
                          bool& valid) {
    if (!valid) {
      bmin = point;
      bmax = point;
      valid = true;
      return;
    }
    bmin.x = std::min(bmin.x, point.x);
    bmin.y = std::min(bmin.y, point.y);
    bmin.z = std::min(bmin.z, point.z);
    bmax.x = std::max(bmax.x, point.x);
    bmax.y = std::max(bmax.y, point.y);
    bmax.z = std::max(bmax.z, point.z);
  };

  auto instance_has_compatible_triangles = [&](const vkpt::pathtracer::RTInstance& inst) {
    if (inst.triangle_count == 0u || inst.first_triangle > m_sceneData.indices.size() / 3u) {
      return false;
    }
    const uint64_t firstIndex = static_cast<uint64_t>(inst.first_triangle) * 3ull;
    const uint64_t indexCount = static_cast<uint64_t>(inst.triangle_count) * 3ull;
    return firstIndex + indexCount <= m_sceneData.indices.size();
  };

  auto instance_has_local_triangles = [&](const vkpt::pathtracer::RTInstance& inst) {
    const uint64_t firstIndex = inst.local_first_index;
    const uint64_t indexCount = inst.local_index_count;
    const uint64_t firstVertex = inst.local_first_vertex;
    const uint64_t vertexCount = inst.local_vertex_count;
    return indexCount >= 3u &&
           indexCount % 3u == 0u &&
           firstIndex + indexCount <= m_sceneData.local_indices.size() &&
           vertexCount > 0u &&
           firstVertex + vertexCount <= m_sceneData.local_vertices.size();
  };

  const bool allowDynamicInstanceTransforms = m_dynamicInstanceTransformsAllowed;
  auto should_use_dynamic_instance = [&](const vkpt::pathtracer::RTInstance& inst) {
    return allowDynamicInstanceTransforms &&
           inst.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform) &&
           instance_has_local_triangles(inst);
  };

  auto make_base_packed_instance = [](const vkpt::pathtracer::RTInstance& inst) {
    PackedInstance packed{};
    packed.material_index = inst.material_index;
    packed.flags = inst.flags;
    packed.translation = inst.translation;
    packed.rotation = inst.rotation;
    packed.scale = inst.scale;
    return packed;
  };

  auto append_static_instance = [&](std::size_t instanceIndex) {
    const auto& inst = m_sceneData.instances[instanceIndex];
    PackedInstance packed = make_base_packed_instance(inst);
    packed.flags &= ~vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
    packed.first_triangle = static_cast<uint32_t>(m_gpuIdx.size() / 3u);
    if (!instance_has_compatible_triangles(inst)) {
      packedInstances[instanceIndex] = packed;
      return;
    }

    const uint32_t firstIndex = inst.first_triangle * 3u;
    const uint32_t indexCount = inst.triangle_count * 3u;
    uint32_t minIndex = std::numeric_limits<uint32_t>::max();
    uint32_t maxIndex = 0u;
    for (uint32_t offset = 0u; offset < indexCount; ++offset) {
      const uint32_t index = m_sceneData.indices[firstIndex + offset];
      if (index >= m_sceneData.vertices.size()) {
        continue;
      }
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    if (minIndex == std::numeric_limits<uint32_t>::max() || maxIndex >= m_sceneData.vertices.size()) {
      packedInstances[instanceIndex] = packed;
      return;
    }

    const uint32_t baseVertex = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    for (uint32_t index = minIndex; index <= maxIndex; ++index) {
      append_vertex(m_sceneData.vertices[index], index);
    }
    for (uint32_t offset = 0u; offset < indexCount; offset += 3u) {
      const uint32_t i0 = m_sceneData.indices[firstIndex + offset + 0u];
      const uint32_t i1 = m_sceneData.indices[firstIndex + offset + 1u];
      const uint32_t i2 = m_sceneData.indices[firstIndex + offset + 2u];
      if (i0 < minIndex || i1 < minIndex || i2 < minIndex ||
          i0 > maxIndex || i1 > maxIndex || i2 > maxIndex) {
        continue;
      }
      m_gpuIdx.push_back(baseVertex + (i0 - minIndex));
      m_gpuIdx.push_back(baseVertex + (i1 - minIndex));
      m_gpuIdx.push_back(baseVertex + (i2 - minIndex));
      staticTriMat.push_back(inst.material_index);
    }
    packed.triangle_count = static_cast<uint32_t>(m_gpuIdx.size() / 3u) - packed.first_triangle;
    packedInstances[instanceIndex] = packed;
  };

  auto append_dynamic_instance = [&](std::size_t instanceIndex) {
    const auto& inst = m_sceneData.instances[instanceIndex];
    PackedInstance packed = make_base_packed_instance(inst);
    packed.flags |= vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
    packed.first_triangle = static_cast<uint32_t>(m_gpuIdx.size() / 3u);

    const uint32_t baseVertex = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    bool boundsValid = false;
    for (uint32_t offset = 0u; offset < inst.local_vertex_count; ++offset) {
      const auto& vertex = m_sceneData.local_vertices[inst.local_first_vertex + offset];
      expand_bounds(packed.local_bounds_min, packed.local_bounds_max, vertex, boundsValid);
      append_vertex(vertex);
    }
    for (uint32_t offset = 0u; offset < inst.local_index_count; offset += 3u) {
      const uint32_t i0 = m_sceneData.local_indices[inst.local_first_index + offset + 0u];
      const uint32_t i1 = m_sceneData.local_indices[inst.local_first_index + offset + 1u];
      const uint32_t i2 = m_sceneData.local_indices[inst.local_first_index + offset + 2u];
      if (i0 >= inst.local_vertex_count || i1 >= inst.local_vertex_count || i2 >= inst.local_vertex_count) {
        continue;
      }
      m_gpuIdx.push_back(baseVertex + i0);
      m_gpuIdx.push_back(baseVertex + i1);
      m_gpuIdx.push_back(baseVertex + i2);
    }
    packed.triangle_count = static_cast<uint32_t>(m_gpuIdx.size() / 3u) - packed.first_triangle;
    if (!boundsValid) {
      packed.local_bounds_min = {-1.0f, -1.0f, -1.0f};
      packed.local_bounds_max = {1.0f, 1.0f, 1.0f};
    }
    if (packed.triangle_count > 0u) {
      const std::size_t indexOffset = static_cast<std::size_t>(packed.first_triangle) * 3u;
      const std::size_t indexCount = static_cast<std::size_t>(packed.triangle_count) * 3u;
      std::vector<BvhTriRef> refs(packed.triangle_count);
      for (uint32_t t = 0u; t < packed.triangle_count; ++t) {
        const uint32_t i0 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 0u];
        const uint32_t i1 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 1u];
        const uint32_t i2 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 2u];
        const float v0[3] = {m_gpuVerts[i0*3u], m_gpuVerts[i0*3u+1u], m_gpuVerts[i0*3u+2u]};
        const float v1[3] = {m_gpuVerts[i1*3u], m_gpuVerts[i1*3u+1u], m_gpuVerts[i1*3u+2u]};
        const float v2[3] = {m_gpuVerts[i2*3u], m_gpuVerts[i2*3u+1u], m_gpuVerts[i2*3u+2u]};
        refs[t].orig_idx = t;
        for (int k = 0; k < 3; ++k) {
          refs[t].bmin[k] = std::min({v0[k], v1[k], v2[k]});
          refs[t].bmax[k] = std::max({v0[k], v1[k], v2[k]});
          refs[t].centroid[k] = (refs[t].bmin[k] + refs[t].bmax[k]) * 0.5f;
        }
      }

      std::vector<uint32_t> localOrigIdx(
          m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset),
          m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset + indexCount));
      std::vector<uint32_t> localTriMat(packed.triangle_count, packed.material_index);
      std::vector<uint32_t> reorderedIdx;
      std::vector<uint32_t> reorderedTriMat;
      reorderedIdx.reserve(localOrigIdx.size());
      reorderedTriMat.reserve(packed.triangle_count);
      std::vector<float> localBvh(std::max(1u, 2u * packed.triangle_count) * 8u, 0.0f);
      uint32_t nodeCount = 0u;
      bvh_build(refs, 0u, packed.triangle_count, localOrigIdx, localTriMat,
                bvhConfig, localBvh, reorderedIdx, reorderedTriMat, nodeCount);
      localBvh.resize(static_cast<std::size_t>(nodeCount) * 8u);
      if (reorderedIdx.size() == indexCount && nodeCount > 0u) {
        std::copy(reorderedIdx.begin(), reorderedIdx.end(),
                  m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset));
        const uint32_t firstNode = static_cast<uint32_t>(m_gpuLocalBvh.size() / 8u);
        for (uint32_t node = 0u; node < nodeCount; ++node) {
          const std::size_t base = static_cast<std::size_t>(node) * 8u;
          const uint32_t lf = FloatBits(localBvh[base + 6u]);
          const uint32_t rc = FloatBits(localBvh[base + 7u]);
          if ((lf & 0x80000000u) != 0u) {
            const uint32_t firstTri = (lf & 0x7fffffffu) + packed.first_triangle;
            localBvh[base + 6u] = UintBitsToFloat(0x80000000u | firstTri);
          } else {
            localBvh[base + 6u] = UintBitsToFloat(lf + firstNode);
            localBvh[base + 7u] = UintBitsToFloat(rc + firstNode);
          }
        }
        packed.local_bvh_first_node = firstNode;
        packed.local_bvh_node_count = nodeCount;
        m_gpuLocalBvh.insert(m_gpuLocalBvh.end(), localBvh.begin(), localBvh.end());
      }
    }
    packedInstances[instanceIndex] = packed;
    m_dynamicInstanceTransformsEnabled = m_dynamicInstanceTransformsEnabled || packed.triangle_count > 0u;
  };

  for (std::size_t instanceIndex = 0; instanceIndex < m_sceneData.instances.size(); ++instanceIndex) {
    if (!should_use_dynamic_instance(m_sceneData.instances[instanceIndex])) {
      append_static_instance(instanceIndex);
    }
  }
  m_staticTriangleCount = static_cast<uint32_t>(staticTriMat.size());
  for (std::size_t instanceIndex = 0; instanceIndex < m_sceneData.instances.size(); ++instanceIndex) {
    if (should_use_dynamic_instance(m_sceneData.instances[instanceIndex])) {
      append_dynamic_instance(instanceIndex);
    }
  }

  if (m_gpuVerts.empty()) {
    m_gpuVerts.assign(3, 0.0f);
    m_gpuTexcoords.assign(2, 0.0f);
  }
  if (m_gpuIdx.empty()) {
    m_gpuIdx.push_back(0u);
  }

  m_gpuInsts.clear();
  m_gpuInsts.reserve(packedInstances.size() * kGpuInstanceStrideU32);
  for (const auto& inst : packedInstances) {
    m_gpuInsts.push_back(inst.first_triangle);
    m_gpuInsts.push_back(inst.triangle_count);
    m_gpuInsts.push_back(inst.material_index);
    m_gpuInsts.push_back(inst.flags);
    m_gpuInsts.push_back(FloatBits(inst.translation.x));
    m_gpuInsts.push_back(FloatBits(inst.translation.y));
    m_gpuInsts.push_back(FloatBits(inst.translation.z));
    m_gpuInsts.push_back(inst.local_bvh_first_node);
    m_gpuInsts.push_back(FloatBits(inst.rotation.x));
    m_gpuInsts.push_back(FloatBits(inst.rotation.y));
    m_gpuInsts.push_back(FloatBits(inst.rotation.z));
    m_gpuInsts.push_back(FloatBits(inst.rotation.w));
    m_gpuInsts.push_back(FloatBits(inst.scale.x));
    m_gpuInsts.push_back(FloatBits(inst.scale.y));
    m_gpuInsts.push_back(FloatBits(inst.scale.z));
    m_gpuInsts.push_back(inst.local_bvh_node_count);
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_min.x));
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_min.y));
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_min.z));
    m_gpuInsts.push_back(0u);
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_max.x));
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_max.y));
    m_gpuInsts.push_back(FloatBits(inst.local_bounds_max.z));
    m_gpuInsts.push_back(0u);
  }
  if (m_gpuInsts.empty()) m_gpuInsts.assign(kGpuInstanceStrideU32, 0u);
  if (m_gpuLocalBvh.empty()) StoreEmptyBvh(m_gpuLocalBvh);
  m_dynamicInstanceCount = BuildDynamicInstanceBvhFromPackedInstances(
      m_gpuInsts,
      static_cast<uint32_t>(packedInstances.size()),
      m_gpuDynamicBvh);

  m_gpuLights.clear();
  for (const auto& lt : m_sceneData.lights) {
    m_gpuLights.push_back(lt.position.x); m_gpuLights.push_back(lt.position.y);
    m_gpuLights.push_back(lt.position.z);
    m_gpuLights.push_back(lt.color.x); m_gpuLights.push_back(lt.color.y);
    m_gpuLights.push_back(lt.color.z);
    m_gpuLights.push_back(lt.intensity); m_gpuLights.push_back(std::max(0.0f, lt.radius));
    m_gpuLights.push_back(lt.direction.x); m_gpuLights.push_back(lt.direction.y);
    m_gpuLights.push_back(lt.direction.z);
    m_gpuLights.push_back(lt.spot_inner_cos); m_gpuLights.push_back(lt.spot_outer_cos);
    m_gpuLights.push_back(0.0f); m_gpuLights.push_back(0.0f); m_gpuLights.push_back(0.0f);
  }
  if (m_gpuLights.empty()) m_gpuLights.assign(16, 0.0f);

  m_gpuSdfs.clear();
  m_gpuSdfs.reserve(m_sceneData.sdf_primitives.size() * kGpuSdfStrideFloats);
  for (const auto& sdf : m_sceneData.sdf_primitives) {
    const uint32_t shape = static_cast<uint32_t>(sdf.shape);
    m_gpuSdfs.push_back(static_cast<float>(shape));
    m_gpuSdfs.push_back(static_cast<float>(sdf.material_index));
    m_gpuSdfs.push_back(std::max(0.001f, sdf.radius));
    m_gpuSdfs.push_back(sdf.param_a);
    m_gpuSdfs.push_back(sdf.position.x);
    m_gpuSdfs.push_back(sdf.position.y);
    m_gpuSdfs.push_back(sdf.position.z);
    m_gpuSdfs.push_back(sdf.param_b);
    m_gpuSdfs.push_back(sdf.scale.x);
    m_gpuSdfs.push_back(sdf.scale.y);
    m_gpuSdfs.push_back(sdf.scale.z);
    m_gpuSdfs.push_back(0.0f);
    m_gpuSdfs.push_back(sdf.rotation.x);
    m_gpuSdfs.push_back(sdf.rotation.y);
    m_gpuSdfs.push_back(sdf.rotation.z);
    m_gpuSdfs.push_back(0.0f);
  }
  if (m_gpuSdfs.empty()) m_gpuSdfs.assign(kGpuSdfStrideFloats, 0.0f);
  if (!build_texture_buffers()) {
    return false;
  }

  std::ostringstream us;
  us << "packed scene verts=" << (m_gpuVerts.size() / 3u)
     << " idx=" << m_gpuIdx.size()
     << " mats=" << (m_gpuMats.size() / 16u)
     << " inst=" << (m_gpuInsts.size() / kGpuInstanceStrideU32)
     << " static_tris=" << m_staticTriangleCount
     << " dynamic_xform=" << (m_dynamicInstanceTransformsEnabled ? "on" : "off")
     << " dynamic_tlas_instances=" << m_dynamicInstanceCount
     << " dynamic_tlas_nodes=" << (m_gpuDynamicBvh.size() / 8u)
     << " local_bvh_nodes=" << (m_gpuLocalBvh.size() / 8u)
     << " sdfs=" << m_sceneData.sdf_primitives.size()
     << " tess=" << m_sceneData.tessellation_requests.size()
     << " lights=" << (m_gpuLights.size() / 16u)
     << " textures=" << (m_gpuTextureMeta.size() / 4u)
     << " bytes="
     << (m_gpuVerts.size() * sizeof(float)) << ","
     << (m_gpuIdx.size() * sizeof(uint32_t)) << ","
     << (m_gpuMats.size() * sizeof(float)) << ","
     << (m_gpuInsts.size() * sizeof(uint32_t)) << ","
     << (m_gpuLocalBvh.size() * sizeof(float)) << ","
     << (m_gpuLights.size() * sizeof(float)) << ","
     << (m_gpuSdfs.size() * sizeof(float)) << ","
     << (m_gpuTexels.size() * sizeof(uint32_t)) << ","
     << (m_gpuTextureMeta.size() * sizeof(uint32_t));
  LogDebug(us.str());

  if (!m_sceneData.tessellation_requests.empty()) {
    const auto& request = m_sceneData.tessellation_requests.front();
    std::ostringstream tessLog;
    tessLog << "cached GPU tessellation requested count=" << m_sceneData.tessellation_requests.size()
            << " first_geometry=" << request.geometry_id
            << " factor=" << request.factor
            << " generated_vertices=" << request.generated_vertex_count
            << " generated_indices=" << request.generated_index_count
            << " projection_mode=" << request.projection_mode
            << " cache_key=" << request.cache_key
            << " cache=" << (request.cache_generated_geometry ? "on" : "off");
    LogInfo(tessLog.str());
  }

  // ===== Build BVH =====
  m_gpuBvh.clear();
  m_gpuTriMat.clear();
  const uint32_t total_tris = m_staticTriangleCount;
  if (total_tris > 0u) {
    // Build per-triangle reference list with AABB and centroid
    std::vector<BvhTriRef> refs(total_tris);
    for (uint32_t t = 0u; t < total_tris; ++t) {
      const uint32_t i0 = m_gpuIdx[t*3u+0u], i1 = m_gpuIdx[t*3u+1u], i2 = m_gpuIdx[t*3u+2u];
      const float v0[3] = {m_gpuVerts[i0*3u], m_gpuVerts[i0*3u+1u], m_gpuVerts[i0*3u+2u]};
      const float v1[3] = {m_gpuVerts[i1*3u], m_gpuVerts[i1*3u+1u], m_gpuVerts[i1*3u+2u]};
      const float v2[3] = {m_gpuVerts[i2*3u], m_gpuVerts[i2*3u+1u], m_gpuVerts[i2*3u+2u]};
      refs[t].orig_idx = t;
      for (int k = 0; k < 3; ++k) {
        refs[t].bmin[k] = std::min({v0[k], v1[k], v2[k]});
        refs[t].bmax[k] = std::max({v0[k], v1[k], v2[k]});
        refs[t].centroid[k] = (refs[t].bmin[k] + refs[t].bmax[k]) * 0.5f;
      }
    }

    // Allocate GPU node buffer (max 2*total_tris nodes for binary BVH)
    const uint32_t max_nodes = std::max(1u, 2u * total_tris);
    m_gpuBvh.assign(static_cast<size_t>(max_nodes) * 8u, 0.0f);

    std::vector<uint32_t> orig_idx_copy = m_gpuIdx; // save before reorder
    std::vector<uint32_t> reord_idx;
    reord_idx.reserve(m_gpuIdx.size());
    uint32_t node_count = 0u;
    bvh_build(refs, 0u, total_tris, orig_idx_copy, staticTriMat,
              bvhConfig, m_gpuBvh, reord_idx, m_gpuTriMat, node_count);

    // Trim to actual node count, replace the static triangle segment with the
    // BVH order, then append dynamic triangles exactly as packed for instances.
    m_gpuBvh.resize(static_cast<size_t>(node_count) * 8u);
    const std::size_t dynamicIndexStart = static_cast<std::size_t>(total_tris) * 3u;
    if (dynamicIndexStart < m_gpuIdx.size()) {
      reord_idx.insert(reord_idx.end(), m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(dynamicIndexStart), m_gpuIdx.end());
    }
    m_gpuIdx = std::move(reord_idx);

    std::ostringstream bvhLog;
    bvhLog << "BVH built nodes=" << node_count << " tris=" << total_tris
           << " reord_idx=" << m_gpuIdx.size()
           << " leaf_size=" << m_bvhLeafSize
           << " buckets=" << m_bvhBucketCount
           << " split=" << m_bvhSplitMode;
    LogDebug(bvhLog.str());
  }
  if (m_gpuBvh.empty())   StoreEmptyBvh(m_gpuBvh);   // dummy empty root leaf
  if (m_gpuTriMat.empty()) m_gpuTriMat.push_back(0u);   // dummy entry

  m_gpuTriData.clear();
  const uint32_t packedTriCount = static_cast<uint32_t>(m_gpuIdx.size() / 3u);
  const bool fullPackedTriDataRequired = m_preferDxr || m_packedTriangleBufferEnabled;
  if (fullPackedTriDataRequired) {
    std::vector<uint32_t> packedTriMat(packedTriCount, 0u);
    for (uint32_t tri = 0u; tri < m_staticTriangleCount && tri < m_gpuTriMat.size() && tri < packedTriCount; ++tri) {
      packedTriMat[tri] = m_gpuTriMat[tri];
    }
    for (const auto& inst : packedInstances) {
      if ((inst.flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u) {
        continue;
      }
      const uint64_t begin = inst.first_triangle;
      const uint64_t end = begin + inst.triangle_count;
      if (begin >= packedTriMat.size()) {
        continue;
      }
      for (uint64_t tri = begin; tri < end && tri < packedTriMat.size(); ++tri) {
        packedTriMat[static_cast<std::size_t>(tri)] = inst.material_index;
      }
    }
    auto material_is_double_sided = [&](uint32_t matIndex) {
      const std::size_t effectOffset = static_cast<std::size_t>(matIndex) * 16u + 15u;
      if (effectOffset >= m_gpuMats.size()) {
        return false;
      }
      const uint32_t packedEffect = static_cast<uint32_t>(m_gpuMats[effectOffset] + 0.5f);
      return (packedEffect & 1024u) != 0u;
    };
    auto push_packed_tri = [&](uint32_t triIndex) {
      const std::size_t ib = static_cast<std::size_t>(triIndex) * 3u;
      const uint32_t i0 = ib + 0u < m_gpuIdx.size() ? m_gpuIdx[ib + 0u] : 0u;
      const uint32_t i1 = ib + 1u < m_gpuIdx.size() ? m_gpuIdx[ib + 1u] : i0;
      const uint32_t i2 = ib + 2u < m_gpuIdx.size() ? m_gpuIdx[ib + 2u] : i0;
      auto vertex = [&](uint32_t index, int axis) {
        const std::size_t offset = static_cast<std::size_t>(index) * 3u + static_cast<std::size_t>(axis);
        return offset < m_gpuVerts.size() ? m_gpuVerts[offset] : 0.0f;
      };
      auto texcoord = [&](uint32_t index, int axis) {
        const std::size_t offset = static_cast<std::size_t>(index) * 2u + static_cast<std::size_t>(axis);
        return offset < m_gpuTexcoords.size() ? m_gpuTexcoords[offset] : 0.0f;
      };
      const float v0[3] = {vertex(i0, 0), vertex(i0, 1), vertex(i0, 2)};
      const float v1[3] = {vertex(i1, 0), vertex(i1, 1), vertex(i1, 2)};
      const float v2[3] = {vertex(i2, 0), vertex(i2, 1), vertex(i2, 2)};
      const float uv0[2] = {texcoord(i0, 0), texcoord(i0, 1)};
      const float uv1[2] = {texcoord(i1, 0), texcoord(i1, 1)};
      const float uv2[2] = {texcoord(i2, 0), texcoord(i2, 1)};
      const float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
      const float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
      const uint32_t matIndex = triIndex < packedTriMat.size() ? packedTriMat[triIndex] : 0u;
      const float doubleSided = material_is_double_sided(matIndex) ? 1.0f : 0.0f;
      m_gpuTriData.push_back(v0[0]); m_gpuTriData.push_back(v0[1]); m_gpuTriData.push_back(v0[2]); m_gpuTriData.push_back(static_cast<float>(matIndex));
      m_gpuTriData.push_back(e1[0]); m_gpuTriData.push_back(e1[1]); m_gpuTriData.push_back(e1[2]); m_gpuTriData.push_back(doubleSided);
      m_gpuTriData.push_back(e2[0]); m_gpuTriData.push_back(e2[1]); m_gpuTriData.push_back(e2[2]); m_gpuTriData.push_back(0.0f);
      m_gpuTriData.push_back(uv0[0]); m_gpuTriData.push_back(uv0[1]);
      m_gpuTriData.push_back(uv1[0]); m_gpuTriData.push_back(uv1[1]);
      m_gpuTriData.push_back(uv2[0]); m_gpuTriData.push_back(uv2[1]);
    };
    m_gpuTriData.reserve(static_cast<std::size_t>(packedTriCount) * kGpuTriDataStrideFloats);
    for (uint32_t tri = 0u; tri < packedTriCount; ++tri) {
      push_packed_tri(tri);
    }
  }
  if (m_gpuTriData.empty()) {
    m_gpuTriData.assign(kGpuTriDataStrideFloats, 0.0f);
  }
  {
    std::ostringstream triLog;
    triLog << "packed triangle data tris=" << packedTriCount
           << " stride_floats=" << kGpuTriDataStrideFloats
           << " bytes=" << (m_gpuTriData.size() * sizeof(float))
           << " full_data=" << (fullPackedTriDataRequired ? "true" : "false")
           << " compute_enabled=" << (m_packedTriangleBufferEnabled ? "true" : "false")
           << " dxr_requested=" << (m_preferDxr ? "true" : "false");
    LogDebug(triLog.str());
  }

  destroy_scene_buffers();
  if (!upload_scene_buffers()) return false;
  m_sceneUploaded = true;

  // Build hardware acceleration structures for DXR if pipeline is ready
  if (m_preferDxr && m_dxrPipelineReady && m_dxrRuntimeObjectsReady) {
    m_dxrAccelReady = false;
    if (!build_dxr_acceleration_structures()) {
      LogError("build_or_update_acceleration: BLAS/TLAS build failed: " + m_error);
      // Non-fatal: fall back to compute path
    }
  }

  LogDebug("scene upload complete");
  return true;
}

bool D3D12GpuPathTracer::reset_accumulation() {
  if (!m_configured || !m_filmBuf || !m_clearHeap || !m_clearCpuHeap) return false;
  // Zero the film on GPU using the persistent clear heap (avoids per-frame allocation).
  if (!wait_for_gpu()) {
    m_error = "reset wait_for_gpu";
    LogError("reset_accumulation: " + m_error);
    return false;
  }
  const auto res = m_cmdAllocator->Reset();
  if (FAILED(res)) {
    m_error = "cmd allocator reset failed hr=" + FormatHr(res);
    LogError("reset_accumulation: " + m_error);
    return false;
  }
  const auto res2 = m_cmdList->Reset(m_cmdAllocator.Get(), nullptr);
  if (FAILED(res2)) {
    m_error = "cmd list reset failed hr=" + FormatHr(res2);
    LogError("reset_accumulation: " + m_error);
    return false;
  }

  if (m_filmPixels == 0u) {
    m_error = "film pixel count is zero";
    return false;
  }

  // Reuse the persistent clear heap created in create_film_buffer().
  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_clearHeap->GetGPUDescriptorHandleForHeapStart();

  // Transition film to unordered-access
  D3D12_RESOURCE_BARRIER rb{};
  rb.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb.Transition.pResource   = m_filmBuf.Get();
  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_cmdList->ResourceBarrier(1, &rb);
  m_cmdList->SetDescriptorHeaps(1, m_clearHeap.GetAddressOf());
  const UINT clearValues[4] = {0u, 0u, 0u, 0u};
  m_cmdList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, m_filmBuf.Get(),
                                         clearValues, 0, nullptr);

  rb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  m_cmdList->ResourceBarrier(1, &rb);
  const auto closeRes = m_cmdList->Close();
  if (FAILED(closeRes)) {
    m_error = "cmd list close failed";
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    m_error = "reset wait_for_gpu";
    LogError("reset_accumulation: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during reset_accumulation hr=" + FormatHr(removeHr);
    LogError("reset_accumulation: " + m_error);
    return false;
  }

  m_film.clear();
  m_counters = {};
  LogDebug("reset_accumulation complete");
  return true;
}

bool D3D12GpuPathTracer::ensure_compute_srv_uav_heap() {
  if (m_srvUavHeap) return true;

  // Slots: t0-t10 (11 SRVs), u0 = FilmBuf, u1 = LdrBuf,
  // u2 = denoised HDR, u3 = current guide, u4 = temporal HDR,
  // u5 = temporal history, u6 = previous guide.
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 18;
  heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap)))) {
    m_error = "srv heap";
    LogError("ensure_compute_srv_uav_heap: " + m_error);
    return false;
  }

  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

  auto makeSrv = [&](int slot, ID3D12Resource* buf, UINT64 size, DXGI_FORMAT fmt, UINT64 elementBytes = 4u) {
    if (!buf || size == 0u) {
      std::ostringstream st;
      st << "makeSrv slot=" << slot << " missing resource or empty size";
      LogError(st.str());
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = static_cast<UINT>(size / std::max<UINT64>(1u, elementBytes));
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(slot) * inc;
    m_device->CreateShaderResourceView(buf, &srv, h);
  };

  makeSrv(0, m_vertBuf.Get(),   m_gpuVerts.size()   * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(1, m_idxBuf.Get(),    m_gpuIdx.size()     * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(2, m_matBuf.Get(),    m_gpuMats.size()    * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(3, m_instBuf.Get(),   m_gpuInsts.size()   * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(4, m_ltBuf.Get(),     m_gpuLights.size()  * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(5, m_bvhBuf.Get(),    m_gpuBvh.size()     * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(6, m_triMatBuf.Get(), m_gpuTriMat.size()  * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(7, m_dynamicBvhBuf.Get(), m_gpuDynamicBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(8, m_localBvhBuf.Get(), m_gpuLocalBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(9, m_sdfBuf.Get(), m_gpuSdfs.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(10, m_triDataBuf.Get(), m_gpuTriData.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);

  {
    const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(filmSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(11) * inc;
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &uav, h);
  }
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(m_filmPixels);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(12) * inc;
    m_device->CreateUnorderedAccessView(m_ldrBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 denoiseSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(denoiseSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(13) * inc;
    m_device->CreateUnorderedAccessView(m_denoiseBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(guideSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(14) * inc;
    m_device->CreateUnorderedAccessView(m_guideBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 temporalSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(temporalSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(15) * inc;
    m_device->CreateUnorderedAccessView(m_temporalBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 temporalSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(temporalSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(16) * inc;
    m_device->CreateUnorderedAccessView(m_temporalHistoryBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(guideSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(17) * inc;
    m_device->CreateUnorderedAccessView(m_prevGuideBuf.Get(), nullptr, &uav, h);
  }

  return true;
}

bool D3D12GpuPathTracer::should_readback_sample(uint32_t sample_idx) const {
  if (m_forceReadbackEverySample) {
    return true;
  }
  const bool finiteSpp = m_settings.spp != std::numeric_limits<uint32_t>::max();
  if (finiteSpp) {
    return sample_idx + 1u >= m_settings.spp;
  }
  const uint32_t interval = std::max(1u, m_readbackInterval);
  return sample_idx < 4u || (sample_idx % interval) == 0u;
}

bool D3D12GpuPathTracer::render_sample_batch(uint32_t /*sy*/, uint32_t /*ey*/,
    uint32_t sample_idx, uint32_t frame_idx) {
  const uint64_t callIndex = ++g_d3d12RenderBatchCalls;
  const bool verbose = D3D12VerboseLoggingEnabled() &&
      ((sample_idx % 16u == 0u) || (frame_idx % 16u == 0u));
  if (!m_valid || !m_configured || !m_sceneUploaded) {
    m_error = "render not ready";
    std::ostringstream st;
    st << "render_sample_batch rejected: " << m_error
       << " valid=" << (m_valid ? "true" : "false")
       << " configured=" << (m_configured ? "true" : "false")
       << " sceneUploaded=" << (m_sceneUploaded ? "true" : "false")
       << " filmPixels=" << m_filmPixels;
    LogError(st.str());
    return false;
  }
  if (!m_filmBuf || !m_filmReadbackBuf || !m_filmReadbackPtr || !m_filmPixels) {
    m_error = "film resources unavailable";
    std::ostringstream st;
    st << "render_sample_batch rejected: " << m_error
       << " filmBuf=" << (m_filmBuf ? "ok" : "null")
       << " readbackBuf=" << (m_filmReadbackBuf ? "ok" : "null")
       << " readbackPtr=" << (m_filmReadbackPtr ? "ok" : "null")
       << " filmPixels=" << m_filmPixels;
    LogError(st.str());
    return false;
  }
  if (m_settings.width == 0u || m_settings.height == 0u) {
    m_error = "invalid film dimensions";
    LogError("render_sample_batch rejected: " + m_error);
    return false;
  }
  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch cfg sample=" << sample_idx
       << " frame=" << frame_idx
       << " call=" << callIndex;
    LogDebug(st.str());
  }

  const bool probe = verbose;
  if (probe) {
    std::ostringstream st;
    st << "render_sample_batch probe sample=" << sample_idx
       << " frame=" << frame_idx
       << " dims=" << m_settings.width << "x" << m_settings.height
       << " film_pixels=" << m_filmPixels
       << " states(vf/rs)= "
       << (m_filmBuf ? "film=ok" : "film=null") << "/"
       << (m_filmReadbackBuf ? "rb=ok" : "rb=null");
    LogDebug(st.str());
  }

  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch start frame=" << frame_idx << " sample=" << sample_idx
       << " dims=" << m_settings.width << "x" << m_settings.height;
    LogDebug(st.str());
  }

  if (m_preferDxr) {
    if (!m_dxrSupported || !m_dxrRuntimeObjectsReady) {
      m_error = "DXR path requested but runtime objects are unavailable";
      LogError("render_sample_batch: " + m_error);
      return false;
    }
    if (m_dxrPipelineReady && m_dxrAccelReady) {
      // Route to hardware DXR dispatch
      m_usingDxrDispatch = true;
      return dispatch_dxr_rays(sample_idx, frame_idx, should_readback_sample(sample_idx));
    }
    if (!m_loggedDxrFallback) {
      LogInfo("DXR mode requested; DispatchRays path not wired yet, using compute fallback");
      m_loggedDxrFallback = true;
    }
    m_usingDxrDispatch = false;
  } else {
    m_usingDxrDispatch = false;
  }

  const auto& sc = m_sceneData;

  // Camera basis
  auto norm3 = [](float x, float y, float z, float* out) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0]=x/l; out[1]=y/l; out[2]=z/l;
  };
  float fwd[3]; norm3(sc.camera_target.x - sc.camera_position.x,
    sc.camera_target.y - sc.camera_position.y,
    sc.camera_target.z - sc.camera_position.z, fwd);
  float rt[3] = {fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
    fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
    fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x};
  float rn[3]; norm3(rt[0],rt[1],rt[2],rn);
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
    rn[2]*fwd[0]-rn[0]*fwd[2], rn[0]*fwd[1]-rn[1]*fwd[0],un);

  PathTraceConstants pc{};
  pc.camera_pos_x = sc.camera_position.x;
  pc.camera_pos_y = sc.camera_position.y;
  pc.camera_pos_z = sc.camera_position.z;
  pc.fov_tan_half = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_fwd_x=fwd[0]; pc.cam_fwd_y=fwd[1]; pc.cam_fwd_z=fwd[2];
  pc.aspect = static_cast<float>(m_settings.width) /
              std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right_x=rn[0]; pc.cam_right_y=rn[1]; pc.cam_right_z=rn[2];
  pc.num_sdfs = static_cast<uint32_t>(sc.sdf_primitives.size());
  pc.cam_up_x=un[0]; pc.cam_up_y=un[1]; pc.cam_up_z=un[2];
  pc.sample_index = sample_idx;
  pc.num_insts    = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats     = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights   = static_cast<uint32_t>(sc.lights.size());
  pc.width        = m_settings.width;
  pc.height       = m_settings.height;
  pc.base_seed    = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                    ^ (frame_idx * 2654435761u);
  pc.env_r = sc.environment_color.x;
  pc.env_g = sc.environment_color.y;
  pc.env_b = sc.environment_color.z;
  pc.max_depth_f = static_cast<float>(std::max(1u, m_settings.max_depth));
  pc.aperture_radius = sc.camera_aperture_radius > 0.0f
      ? sc.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  pc.focus_distance = sc.camera_focus_distance > 0.0f
      ? sc.camera_focus_distance
      : m_settings.camera_focus_distance;
  pc.iris_blade_count = std::min(sc.camera_iris_blade_count, 64u);
  pc.iris_rotation_radians = sc.camera_iris_rotation_degrees * (3.14159265f / 180.0f);
  pc.iris_roundness = std::clamp(sc.camera_iris_roundness, 0.0f, 1.0f);
  pc.anamorphic_squeeze = std::isfinite(sc.camera_anamorphic_squeeze)
      ? std::max(0.01f, sc.camera_anamorphic_squeeze)
      : 1.0f;
  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch constants sample_index=" << pc.sample_index
       << " seed=" << pc.base_seed
       << " scene(inst/mat/light)=" << pc.num_insts << "/" << pc.num_mats << "/" << pc.num_lights
       << " max_depth=" << pc.max_depth_f
       << " env=(" << pc.env_r << "," << pc.env_g << "," << pc.env_b << ")";
    LogDebug(st.str());
  }
  if (probe) {
    const float fwdLen = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    const float rightLen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
    const float upLen = std::sqrt(un[0]*un[0] + un[1]*un[1] + un[2]*un[2]);
    std::ostringstream st;
    st << "render_sample_batch basis lens fwd=" << fwdLen
       << " right=" << rightLen << " up=" << upLen
       << " fwd=(" << fwd[0] << "," << fwd[1] << "," << fwd[2] << ")"
       << " right=(" << rn[0] << "," << rn[1] << "," << rn[2] << ")"
       << " up=(" << un[0] << "," << un[1] << "," << un[2] << ")";
    LogDebug(st.str());
    auto* pcWords = reinterpret_cast<const uint32_t*>(&pc);
    std::ostringstream dump;
    dump << "render_sample_batch constant_u32=";
    for (UINT i = 0u; i < kPathTraceRoot32BitValues; ++i) {
      if (i) dump << ",";
      dump << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << pcWords[i];
    }
    dump << std::dec;
    LogDebug(dump.str());
    if (g_d3d12RenderBatchCalls <= 4u) {
      std::ostringstream sampleVals;
      const float camPos[3] = {sc.camera_position.x, sc.camera_position.y, sc.camera_position.z};
      const float camTarget[3] = {sc.camera_target.x, sc.camera_target.y, sc.camera_target.z};
      sampleVals << "camera_pos=(" << camPos[0] << "," << camPos[1] << "," << camPos[2] << ")"
                 << " camera_target=(" << camTarget[0] << "," << camTarget[1] << "," << camTarget[2] << ")"
                 << " material0=" << FormatFirstN(m_gpuMats.data(), 16u, 16u)
                 << " first_inst=" << FormatFirstN(m_gpuInsts.data(), 4u, 4u)
                 << " first_light=" << FormatFirstN(m_gpuLights.data(), 8u, 8u);
      LogDebug(sampleVals.str());
    }
  }

  // Reset command list
  const HRESULT resetAllocatorHr = m_cmdAllocator->Reset();
  if (FAILED(resetAllocatorHr)) {
    m_error = "allocator reset hr=" + FormatHr(resetAllocatorHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  const HRESULT resetCmdListHr = m_cmdList->Reset(m_cmdAllocator.Get(), m_pso.Get());
  if (FAILED(resetCmdListHr)) {
    m_error = "cmd list reset hr=" + FormatHr(resetCmdListHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }

  // Upload constants
  m_cmdList->SetComputeRootSignature(m_rootSig.Get());

  // Create descriptor heap for SRVs/UAVs lazily and reuse across samples.
  // Slots: t0-t10 SRVs, u0 film, u1 LDR, u2 denoised HDR, u3/u6 guides, u4/u5 temporal.
  if (!ensure_compute_srv_uav_heap()) return false;
  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
  const auto resolveSettings =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData);
  const auto whiteBalance = vkpt::pathtracer::WhiteBalanceScale(resolveSettings.white_balance_kelvin);
  const bool doReadback = should_readback_sample(sample_idx);
  const bool doDenoise = doReadback && m_settings.enable_denoiser;
  const bool doTemporal = doReadback && m_settings.enable_temporal_aa;
  const bool doGuide = doDenoise || doTemporal;
  if (!m_settings.enable_temporal_aa) {
    m_temporalHistoryValid = false;
  }
  if (doDenoise && (!m_guidePso || !m_denoisePso || !m_guideBuf || !m_denoiseBuf)) {
    m_error = "GPU denoiser resources unavailable";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  if (doTemporal && (!m_guidePso || !m_temporalPso || !m_guideBuf ||
                     !m_temporalBuf || !m_temporalHistoryBuf || !m_prevGuideBuf)) {
    m_error = "GPU temporal AA resources unavailable";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  pc.rays_per_pixel = std::max(1u, m_raysPerPixelPerDispatch);
  pc.exposure = resolveSettings.exposure;
  pc.tone_map = static_cast<uint32_t>(resolveSettings.tone_map);
  pc.output_transform = static_cast<uint32_t>(resolveSettings.output_transform);
  pc.gamma = resolveSettings.gamma;
  pc.clamp_output = resolveSettings.clamp_output ? 1u : 0u;
  pc.white_balance_r = whiteBalance.x;
  pc.white_balance_g = whiteBalance.y;
  pc.white_balance_b = whiteBalance.z;
  pc.denoiser_enabled = doDenoise ? 1u : 0u;
  pc.denoiser_strength = doDenoise ? 1.0f : 0.0f;
  pc.denoiser_color_sigma = 0.22f;
  pc.temporal_enabled = doTemporal ? 1u : 0u;
  pc.temporal_history_valid = (doTemporal && m_temporalHistoryValid) ? 1u : 0u;
  pc.temporal_feedback = 0.92f;
  pc.temporal_depth_sigma = 0.05f;
  pc.temporal_normal_power = 28.0f;
  pc.temporal_color_margin = 0.12f;
  FillPreviousCameraConstants(pc, m_temporalHistoryValid ? m_temporalPrevCamera : MakeTemporalCameraState(pc));

  ID3D12DescriptorHeap* heaps[] = {m_srvUavHeap.Get()};
  m_cmdList->SetDescriptorHeaps(1, heaps);
  m_cmdList->SetComputeRootDescriptorTable(1, gpuHandle);

  // Root constants (b0) mirror PathTraceConstants exactly.
  std::memcpy(m_uploadPtr, &pc, sizeof(pc));
  m_cmdList->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());

  // --- Transition FilmBuf and LdrBuf to UAV for GPU work --------------------
  D3D12_RESOURCE_BARRIER filmBarrier{};
  filmBarrier.Type                       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  filmBarrier.Transition.pResource       = m_filmBuf.Get();
  filmBarrier.Transition.StateBefore     = D3D12_RESOURCE_STATE_COMMON;
  filmBarrier.Transition.StateAfter      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.Subresource     = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_cmdList->ResourceBarrier(1u, &filmBarrier);

  D3D12_RESOURCE_BARRIER ldrBarrier{};
  ldrBarrier.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  ldrBarrier.Transition.pResource        = m_ldrBuf.Get();
  ldrBarrier.Transition.StateBefore      = D3D12_RESOURCE_STATE_COMMON;
  ldrBarrier.Transition.StateAfter       = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ldrBarrier.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (doReadback) {
    m_cmdList->ResourceBarrier(1u, &ldrBarrier);
  }

  D3D12_RESOURCE_BARRIER denoiseBarrier = MakeTransitionBarrier(
      m_denoiseBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER guideBarrier = MakeTransitionBarrier(
      m_guideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER temporalBarrier = MakeTransitionBarrier(
      m_temporalBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER temporalHistoryBarrier = MakeTransitionBarrier(
      m_temporalHistoryBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER prevGuideBarrier = MakeTransitionBarrier(
      m_prevGuideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (doDenoise || doGuide || doTemporal) {
    std::array<D3D12_RESOURCE_BARRIER, 5> startBarriers{};
    UINT barrierCount = 0u;
    if (doDenoise) startBarriers[barrierCount++] = denoiseBarrier;
    if (doGuide) startBarriers[barrierCount++] = guideBarrier;
    if (doTemporal) {
      startBarriers[barrierCount++] = temporalBarrier;
      startBarriers[barrierCount++] = temporalHistoryBarrier;
      startBarriers[barrierCount++] = prevGuideBarrier;
    }
    if (barrierCount > 0u) {
      m_cmdList->ResourceBarrier(barrierCount, startBarriers.data());
    }
  }

  // --- Path trace dispatch --------------------------------------------------
  const UINT groupsX = (m_settings.width + 7u) / 8u;
  const UINT groupsY = (m_settings.height + 7u) / 8u;
  if (verbose) {
    std::ostringstream d;
    d << "dispatch groups " << groupsX << "x" << groupsY;
    LogDebug(d.str());
  }
  m_cmdList->SetPipelineState(m_pso.Get());
  constexpr wchar_t kPathTraceEvent[] = L"D3D12 Compute PathTrace Dispatch";
  m_cmdList->BeginEvent(0, kPathTraceEvent, sizeof(kPathTraceEvent));
  m_cmdList->Dispatch(groupsX, groupsY, 1u);
  m_cmdList->EndEvent();

  if (doReadback) {
  // UAV barrier on FilmBuf: ensure path trace writes are visible before tonemap reads.
  D3D12_RESOURCE_BARRIER uavBarrier{};
  uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = m_filmBuf.Get();
  m_cmdList->ResourceBarrier(1, &uavBarrier);

  if (doGuide) {
    m_cmdList->SetPipelineState(m_guidePso.Get());
    constexpr wchar_t kGuideEvent[] = L"D3D12 Compute Guide Dispatch";
    m_cmdList->BeginEvent(0, kGuideEvent, sizeof(kGuideEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_guideBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  if (doTemporal) {
    m_cmdList->SetPipelineState(m_temporalPso.Get());
    constexpr wchar_t kTemporalEvent[] = L"D3D12 Compute Temporal AA Dispatch";
    m_cmdList->BeginEvent(0, kTemporalEvent, sizeof(kTemporalEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_temporalBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  if (doDenoise) {
    m_cmdList->SetPipelineState(m_denoisePso.Get());
    constexpr wchar_t kDenoiseEvent[] = L"D3D12 Compute Denoise Dispatch";
    m_cmdList->BeginEvent(0, kDenoiseEvent, sizeof(kDenoiseEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_denoiseBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  // --- GPU tonemap dispatch: RGBA32F film → RGBA8 LdrBuf --------------------
  m_cmdList->SetPipelineState(m_tonemapPso.Get());
  constexpr wchar_t kTonemapEvent[] = L"D3D12 Compute Tonemap Dispatch";
  m_cmdList->BeginEvent(0, kTonemapEvent, sizeof(kTonemapEvent));
  m_cmdList->Dispatch(groupsX, groupsY, 1u);
  m_cmdList->EndEvent();

  // UAV barrier on LdrBuf: ensure tonemap writes complete before copy-out.
  uavBarrier.UAV.pResource = m_ldrBuf.Get();
  m_cmdList->ResourceBarrier(1, &uavBarrier);

  if (doTemporal) {
    const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    std::array<D3D12_RESOURCE_BARRIER, 4> copyStartBarriers = {
        MakeTransitionBarrier(m_temporalBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
        MakeTransitionBarrier(m_guideBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        MakeTransitionBarrier(m_prevGuideBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
    m_cmdList->ResourceBarrier(static_cast<UINT>(copyStartBarriers.size()), copyStartBarriers.data());
    m_cmdList->CopyBufferRegion(m_temporalHistoryBuf.Get(), 0, m_temporalBuf.Get(), 0, filmSize);
    m_cmdList->CopyBufferRegion(m_prevGuideBuf.Get(), 0, m_guideBuf.Get(), 0, guideSize);

    std::array<D3D12_RESOURCE_BARRIER, 4> copyEndBarriers = {
        MakeTransitionBarrier(m_temporalBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_guideBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_prevGuideBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
    m_cmdList->ResourceBarrier(static_cast<UINT>(copyEndBarriers.size()), copyEndBarriers.data());
  }

  // --- Transition for readback ----------------------------------------------
  // FilmBuf: UAV → COMMON (stays accumulating; no CPU readback needed for display)
  filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  // LdrBuf: UAV → COPY_SOURCE
  ldrBarrier.Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ldrBarrier.Transition.StateAfter   = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (doDenoise || (doGuide && !doTemporal)) {
    std::array<D3D12_RESOURCE_BARRIER, 4> readbackBarriers{};
    UINT barrierCount = 0u;
    readbackBarriers[barrierCount++] = filmBarrier;
    readbackBarriers[barrierCount++] = ldrBarrier;
    denoiseBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    denoiseBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    guideBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    guideBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    if (doDenoise) readbackBarriers[barrierCount++] = denoiseBarrier;
    if (doGuide && !doTemporal) readbackBarriers[barrierCount++] = guideBarrier;
    m_cmdList->ResourceBarrier(barrierCount, readbackBarriers.data());
  } else {
    D3D12_RESOURCE_BARRIER readbackBarriers[2] = {filmBarrier, ldrBarrier};
    m_cmdList->ResourceBarrier(2, readbackBarriers);
  }

  // Copy LdrBuf (RGBA8, 4 B/pixel) to CPU-visible readback buffer.
  const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
  m_cmdList->CopyBufferRegion(m_ldrReadbackBuf.Get(), 0, m_ldrBuf.Get(), 0, ldrSize);

  // LdrBuf: COPY_SOURCE → COMMON
  ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  m_cmdList->ResourceBarrier(1, &ldrBarrier);
  } else {
    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    m_cmdList->ResourceBarrier(1, &filmBarrier);
  }

  const auto closeRes = m_cmdList->Close();
  if (FAILED(closeRes)) {
    m_error = "cmd list close hr=" + FormatHr(closeRes);
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  if (verbose) {
    LogDebug("render_sample_batch execute cmdlist frame=" + std::to_string(frame_idx) +
             " sample=" + std::to_string(sample_idx));
  }
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu failed";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during render hr=" + FormatHr(removeHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }

  // Store GPU-tonemapped LDR result for resolve_ldr() — zero CPU work needed.
  if (doTemporal) {
    m_temporalHistoryValid = true;
    m_temporalPrevCamera = MakeTemporalCameraState(pc);
  }
  if (doReadback && m_ldrReadbackPtr && m_filmPixels > 0u) {
    m_ldrResolve.width  = m_settings.width;
    m_ldrResolve.height = m_settings.height;
    m_ldrResolve.rgba8.resize(static_cast<size_t>(m_filmPixels) * 4u);
    // LdrBuf stores uint packed as R|G<<8|B<<16|0xFF<<24.
    // In little-endian memory that maps directly to RGBA8 byte layout.
    std::memcpy(m_ldrResolve.rgba8.data(), m_ldrReadbackPtr,
                static_cast<size_t>(m_filmPixels) * 4u);
    if (verbose) {
      // Quick non-black probe on byte data (very cheap)
      const auto* b = m_ldrResolve.rgba8.data();
      bool nonBlack = false;
      for (size_t i = 0; i < std::min<size_t>(m_filmPixels, 64u) * 4u; i += 4u)
        if (b[i] || b[i+1u] || b[i+2u]) { nonBlack = true; break; }
      LogDebug("render_sample_batch ldr readback frame=" + std::to_string(frame_idx)
               + " non_black=" + (nonBlack ? "yes" : "no"));
    }
  }
  const uint64_t rpp = static_cast<uint64_t>(std::max(1u, m_raysPerPixelPerDispatch));
  const uint64_t sampleInc = static_cast<uint64_t>(m_settings.width) * m_settings.height * rpp;
  const uint64_t raysPerSample =
      EstimateLogicalRaysPerD3D12Sample(m_settings, m_sceneData, m_usingDxrDispatch);
  m_counters.samples += sampleInc;
  m_counters.rays    += SaturatingMulU64(sampleInc, raysPerSample);
  m_lastSampleIdx     = sample_idx * m_raysPerPixelPerDispatch + (m_raysPerPixelPerDispatch - 1u);
  if (verbose) {
    std::ostringstream en;
    en << "render_sample_batch complete frame=" << frame_idx << " sample=" << sample_idx
       << " samples=" << m_counters.samples << " rays=" << m_counters.rays
       << " estimated_rays_per_sample=" << raysPerSample;
    LogDebug(en.str());
  }
  return true;
}

vkpt::pathtracer::FilmLdr D3D12GpuPathTracer::resolve_ldr() const {
  // The GPU tonemap pass already produced an RGBA8 result during render_sample_batch().
  // Return it directly — no CPU tonemapping, no box filter, no per-pixel work.
  if (!m_ldrResolve.rgba8.empty()) {
    return m_ldrResolve;
  }
  // Fallback: if no GPU frame has been produced yet, return blank.
  vkpt::pathtracer::FilmLdr blank;
  blank.width  = m_settings.width;
  blank.height = m_settings.height;
  blank.rgba8.assign(static_cast<size_t>(m_filmPixels) * 4u, 0u);
  return blank;
}
vkpt::pathtracer::FilmHdr D3D12GpuPathTracer::resolve_hdr() const {
  return m_film.resolve_hdr();
}
vkpt::pathtracer::SampleCounters D3D12GpuPathTracer::read_counters() const {
  return m_counters;
}

void D3D12GpuPathTracer::shutdown() {
  if (!m_device) return;
  (void)wait_for_gpu();
  destroy_film_buffer();
  destroy_scene_buffers();
  m_uploadBuf.Reset();
  destroy_dxr_resources();
  m_dxrCmdList.Reset();
  m_dxrCmdAllocator.Reset();
  m_dxrCmdQueue.Reset();
  m_device5.Reset();
  m_cmdList.Reset();
  m_cmdAllocator.Reset();
  m_pso.Reset();
  m_tonemapPso.Reset();
  m_denoisePso.Reset();
  m_guidePso.Reset();
  m_temporalPso.Reset();
  m_rootSig.Reset();
  if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
  m_cmdQueue.Reset();
  m_device.Reset();
  m_factory.Reset();
  m_valid = false;
  m_uploadPtr = nullptr;
  m_dxrRuntimeObjectsReady = false;
  m_usingDxrDispatch = false;
  m_dxrAccelReady    = false;
  m_dxrPipelineReady = false;
}

// ============================================================================
// Init helpers
// ============================================================================

bool D3D12GpuPathTracer::init_device() {
#if defined(_DEBUG)
  Microsoft::WRL::ComPtr<ID3D12Debug> dbg;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
    dbg->EnableDebugLayer();
#endif

  UINT flags = 0;
#if defined(_DEBUG)
  flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
  const HRESULT createFactoryHr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory));
  if (FAILED(createFactoryHr)) {
    m_error = "CreateDXGIFactory2 hr=" + FormatHr(createFactoryHr);
    LogError("init_device: " + m_error);
    return false;
  }

  // Enumerate adapters, pick the one with the most dedicated VRAM
  Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
  SIZE_T bestVram = 0;
  std::string bestName;
  int adapterCount = 0;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (m_factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    const std::string name = WStringToUtf8(desc.Description);
    const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    const bool creates = SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), nullptr));
    std::ostringstream a;
    a << "adapter[" << i << "] " << name
      << " vram=" << (static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull))
      << "MB software=" << (software ? "true" : "false")
      << " create_ok=" << (creates ? "true" : "false");
    LogDebug(a.str());
    if (software || !creates) continue;
    ++adapterCount;
    if (desc.DedicatedVideoMemory > bestVram) {
      bestVram = desc.DedicatedVideoMemory;
      chosen = adapter;
      bestName = name;
    }
  }
  if (!chosen) {
    m_factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen));
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(chosen->GetDesc1(&desc))) {
      m_error = "EnumWarpAdapter returned invalid adapter";
      return false;
    }
    bestVram = 0;
    bestName = WStringToUtf8(desc.Description);
    LogDebug("falling back to WARP adapter");
  }
  if (chosen && adapterCount == 0) {
    LogDebug("adapter enum had no non-software D3D12 devices");
  }
  m_gpuName = bestName;
  m_vramMb  = static_cast<uint32_t>(bestVram / (1024u * 1024u));
  std::ostringstream sel;
  sel << "Selected GPU: " << m_gpuName << "  VRAM=" << m_vramMb << " MB";
  LogInfo(sel.str());

  const HRESULT createDeviceHr = D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
      IID_PPV_ARGS(&m_device));
  if (FAILED(createDeviceHr)) {
    m_error = "D3D12CreateDevice hr=" + FormatHr(createDeviceHr);
    LogError("init_device: " + m_error);
    return false;
  }

  // Probe DXR capability for migration planning and benchmark telemetry.
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
  const HRESULT opts5Hr = m_device->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5));
  if (SUCCEEDED(opts5Hr)) {
    m_dxrTier = opts5.RaytracingTier;
    m_dxrSupported = (m_dxrTier >= D3D12_RAYTRACING_TIER_1_0);
  } else {
    m_dxrTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    m_dxrSupported = false;
  }
  {
    std::ostringstream dxr;
    dxr << "DXR support=" << (m_dxrSupported ? "yes" : "no")
        << " tier=" << dxr_tier_string();
    LogInfo(dxr.str());
  }

  // Command queue
  const D3D12_COMMAND_LIST_TYPE commandListType = SelectComputeCommandListType();
  LogInfo(std::string("compute command queue type=") + CommandListTypeName(commandListType));
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type     = commandListType;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  const HRESULT createQueueHr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue));
  if (FAILED(createQueueHr)) {
    m_error = "CreateCommandQueue hr=" + FormatHr(createQueueHr);
    LogError("init_device: " + m_error);
    return false;
  }

  // Command allocator + list
  const HRESULT createAllocHr = m_device->CreateCommandAllocator(commandListType,
      IID_PPV_ARGS(&m_cmdAllocator));
  if (FAILED(createAllocHr)) {
    m_error = "CreateCommandAllocator hr=" + FormatHr(createAllocHr);
    LogError("init_device: " + m_error);
    return false;
  }
  const HRESULT createListHr = m_device->CreateCommandList(0, commandListType,
      m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
  if (FAILED(createListHr)) {
    m_error = "CreateCommandList hr=" + FormatHr(createListHr);
    LogError("init_device: " + m_error);
    return false;
  }
  m_cmdList->Close();

  // Fence
  const HRESULT createFenceHr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(&m_fence));
  if (FAILED(createFenceHr)) {
    m_error = "CreateFence hr=" + FormatHr(createFenceHr);
    LogError("init_device: " + m_error);
    return false;
  }
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent) {
    m_error = "CreateEventW";
    LogError("init_device: " + m_error);
    return false;
  }

  // Upload buffer — 64 MB
  m_uploadSize = 256ull * 1024 * 1024;
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = m_uploadSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  const HRESULT createUploadHr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadBuf));
  if (FAILED(createUploadHr)) {
    m_error = "upload buffer hr=" + FormatHr(createUploadHr);
    LogError("init_device: " + m_error);
    return false;
  }
  const HRESULT mapUploadHr = m_uploadBuf->Map(0, nullptr, &m_uploadPtr);
  if (FAILED(mapUploadHr)) {
    m_error = "upload buffer map hr=" + FormatHr(mapUploadHr);
    LogError("init_device: " + m_error);
    return false;
  }
  LogDebug("D3D12 device init success upload_heap=" + std::to_string(m_uploadSize));

  LogInfo("D3D12 device init success");
  return true;
}

bool D3D12GpuPathTracer::init_dxr_runtime_objects() {
  if (!m_dxrSupported || !m_device5) {
    return false;
  }

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  const HRESULT createQueueHr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_dxrCmdQueue));
  if (FAILED(createQueueHr)) {
    m_error = "CreateCommandQueue(DIRECT) hr=" + FormatHr(createQueueHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return false;
  }

  const HRESULT createAllocHr = m_device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_dxrCmdAllocator));
  if (FAILED(createAllocHr)) {
    m_error = "CreateCommandAllocator(DIRECT) hr=" + FormatHr(createAllocHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return false;
  }

  const HRESULT createListHr = m_device5->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_dxrCmdAllocator.Get(), nullptr,
      IID_PPV_ARGS(&m_dxrCmdList));
  if (FAILED(createListHr)) {
    m_error = "CreateCommandList4(DIRECT) hr=" + FormatHr(createListHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return false;
  }
  m_dxrCmdList->Close();
  m_dxrRuntimeObjectsReady = true;

  // Create DXR fence (separate from compute fence so queues can sync independently)
  const HRESULT createDxrFenceHr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
      IID_PPV_ARGS(&m_dxrFence));
  if (FAILED(createDxrFenceHr)) {
    m_error = "CreateFence(DXR) hr=" + FormatHr(createDxrFenceHr);
    LogError("init_dxr_runtime_objects: " + m_error);
    return false;
  }
  m_dxrFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_dxrFenceEvent) {
    m_error = "CreateEventW(DXR)";
    LogError("init_dxr_runtime_objects: " + m_error);
    return false;
  }

  std::ostringstream st;
  st << "DXR runtime objects ready tier=" << dxr_tier_string();
  LogInfo(st.str());
  return true;
}

bool D3D12GpuPathTracer::create_root_sig_and_pso() {
  // Root sig: param0 = constants CBV (b0), param1 = descriptor table
  D3D12_ROOT_PARAMETER params[2]{};
  // Constants (b0)
  params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace  = 0;
  params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
  // Descriptor table (t0-t10 SRV + u0-u6 UAV at slots 11-17)
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors     = 11;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace      = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors     = 7; // u0 film, u1 LDR, u2 denoise, u3/u6 guides, u4/u5 temporal
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace      = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 11;
  params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges   = ranges;
  params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 2;
  rsd.pParameters   = params;
  rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(hr)) {
    m_error = "SerializeRootSignature";
    if (err) LogError(static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  const HRESULT createRootHr = m_device->CreateRootSignature(0, sig->GetBufferPointer(),
      sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
  if (FAILED(createRootHr)) {
    m_error = "CreateRootSignature hr=" + FormatHr(createRootHr);
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }

  // Load HLSL source and compile at runtime
  std::ifstream f(m_hlslPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    m_error = "Cannot open HLSL: " + m_hlslPath;
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  const auto sz = static_cast<size_t>(f.tellg());
  LogDebug("compile shader " + m_hlslPath + " size=" + std::to_string(sz) + " bytes");
  f.seekg(0);
  std::string src(sz, '\0');
  f.read(&src[0], static_cast<std::streamsize>(sz));
  f.close();

  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  // Keep shader optimization enabled in debug builds. The Qt/D3D12 preview is
  // commonly launched from a debug preset, and unoptimized HLSL dominates ray
  // throughput far more than CPU-side debug symbols help here.
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  const char* traversalDefine = ShaderTraversalDefine(m_shaderTraversalMode);
  const char* packedTrisDefine = BoolDefine(m_packedTriangleBufferEnabled);
  D3D_SHADER_MACRO shaderMacros[] = {
      {"PT_D3D12_STATIC_TRAVERSAL_MODE", traversalDefine},
      {"PT_D3D12_PACKED_TRIANGLES", packedTrisDefine},
      {nullptr, nullptr}};
  HRESULT compileHr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      shaderMacros, nullptr, m_entryPoint.c_str(), "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(compileHr)) {
    m_error = "D3DCompile failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature     = m_rootSig.Get();
  pso.CS.pShaderBytecode  = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength   = csBlob->GetBufferSize();
  const HRESULT createPsoHr = m_device->CreateComputePipelineState(&pso,
      IID_PPV_ARGS(&m_pso));
  if (FAILED(createPsoHr)) {
    m_error = "CreateComputePipelineState hr=" + FormatHr(createPsoHr);
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  LogDebug("compiled shader bytes=" + std::to_string(csBlob->GetBufferSize()));
  LogInfo("D3D12 compute PSO created from " + m_hlslPath +
          " shader_traversal=" + m_shaderTraversalMode +
          " packed_triangles=" + (m_packedTriangleBufferEnabled ? "true" : "false"));

  // Compile tonemap entry point from same source
  if (!create_tonemap_pso(src)) return false;
  if (!create_guide_pso(src)) return false;
  if (!create_temporal_pso(src)) return false;
  if (!create_denoise_pso(src)) return false;

  return true;
}

bool D3D12GpuPathTracer::create_tonemap_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "tonemap_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile tonemap_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature    = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength  = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_tonemapPso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState tonemap hr=" + FormatHr(createHr);
    LogError("create_tonemap_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 tonemap PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_guide_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  const char* traversalDefine = ShaderTraversalDefine(m_shaderTraversalMode);
  const char* packedTrisDefine = BoolDefine(m_packedTriangleBufferEnabled);
  D3D_SHADER_MACRO shaderMacros[] = {
      {"PT_D3D12_STATIC_TRAVERSAL_MODE", traversalDefine},
      {"PT_D3D12_PACKED_TRIANGLES", packedTrisDefine},
      {nullptr, nullptr}};
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      shaderMacros, nullptr, "guide_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile guide_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_guidePso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState guide hr=" + FormatHr(createHr);
    LogError("create_guide_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 denoiser guide PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_denoise_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "denoise_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile denoise_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_denoisePso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState denoise hr=" + FormatHr(createHr);
    LogError("create_denoise_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 denoise PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_temporal_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "temporal_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile temporal_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_temporalPso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState temporal hr=" + FormatHr(createHr);
    LogError("create_temporal_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 temporal AA PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_film_buffer() {
  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  if (filmSize == 0u) {
    m_error = "film size is zero";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  LogDebug("create_film_buffer pixels=" + std::to_string(m_filmPixels) + " bytes=" + std::to_string(filmSize));

  // Default heap (GPU-visible UAV)
  D3D12_HEAP_PROPERTIES defhp{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = filmSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const HRESULT createFilmHr = m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_filmBuf));
  if (FAILED(createFilmHr)) {
    m_error = "film buf hr=" + FormatHr(createFilmHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // Readback buffer
  D3D12_HEAP_PROPERTIES rdhp{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC   rd2{};
  rd2.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd2.Width            = filmSize;
  rd2.Height           = 1;
  rd2.DepthOrArraySize = 1;
  rd2.MipLevels        = 1;
  rd2.Format           = DXGI_FORMAT_UNKNOWN;
  rd2.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd2.SampleDesc.Count = 1;
  const HRESULT createReadbackHr = m_device->CreateCommittedResource(&rdhp, D3D12_HEAP_FLAG_NONE, &rd2,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_filmReadbackBuf));
  if (FAILED(createReadbackHr)) {
    m_error = "film readback buf hr=" + FormatHr(createReadbackHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  const HRESULT mapReadbackHr = m_filmReadbackBuf->Map(0, nullptr, &m_filmReadbackPtr);
  if (FAILED(mapReadbackHr)) {
    m_error = "film readback map hr=" + FormatHr(mapReadbackHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // ---- LDR output buffer: GPU-side RGBA8 (one uint per pixel) ----------------
  const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
  D3D12_RESOURCE_DESC ldrRd{};
  ldrRd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  ldrRd.Width            = ldrSize;
  ldrRd.Height           = 1;
  ldrRd.DepthOrArraySize = 1;
  ldrRd.MipLevels        = 1;
  ldrRd.Format           = DXGI_FORMAT_UNKNOWN;
  ldrRd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ldrRd.SampleDesc.Count = 1;
  ldrRd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &ldrRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_ldrBuf)))) {
    m_error = "ldr buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // LDR readback buffer (CPU-visible, 4 bytes/pixel — 4× smaller than RGBA32F)
  D3D12_RESOURCE_DESC ldrRb{};
  ldrRb.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  ldrRb.Width            = ldrSize;
  ldrRb.Height           = 1;
  ldrRb.DepthOrArraySize = 1;
  ldrRb.MipLevels        = 1;
  ldrRb.Format           = DXGI_FORMAT_UNKNOWN;
  ldrRb.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ldrRb.SampleDesc.Count = 1;
  if (FAILED(m_device->CreateCommittedResource(&rdhp, D3D12_HEAP_FLAG_NONE, &ldrRb,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ldrReadbackBuf)))) {
    m_error = "ldr readback buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_ldrReadbackBuf->Map(0, nullptr, &m_ldrReadbackPtr))) {
    m_error = "ldr readback map failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // ---- HDR denoise buffers stay entirely on the GPU -------------------------
  D3D12_RESOURCE_DESC denoiseRd = rd;
  denoiseRd.Width = filmSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &denoiseRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_denoiseBuf)))) {
    m_error = "denoise buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  D3D12_RESOURCE_DESC guideRd = rd;
  const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
  guideRd.Width = guideSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &guideRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_guideBuf)))) {
    m_error = "denoise guide buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  D3D12_RESOURCE_DESC temporalRd = rd;
  temporalRd.Width = filmSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &temporalRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_temporalBuf)))) {
    m_error = "temporal buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &temporalRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_temporalHistoryBuf)))) {
    m_error = "temporal history buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &guideRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_prevGuideBuf)))) {
    m_error = "previous guide buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  m_temporalHistoryValid = false;

  // ---- Persistent clear heap for reset_accumulation (avoids per-frame alloc) -
  D3D12_DESCRIPTOR_HEAP_DESC chd{};
  chd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  chd.NumDescriptors = 1;
  chd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&m_clearHeap)))) {
    m_error = "clear heap create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC cpuChd = chd;
  cpuChd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(m_device->CreateDescriptorHeap(&cpuChd, IID_PPV_ARGS(&m_clearCpuHeap)))) {
    m_error = "clear CPU heap create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC clearUav = MakeRawBufferUavDesc(filmSize);
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &clearUav,
        m_clearHeap->GetCPUDescriptorHandleForHeapStart());
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &clearUav,
        m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart());
  }

  return true;
}

bool D3D12GpuPathTracer::build_texture_buffers() {
  m_gpuTexels.clear();
  m_gpuTextureMeta.clear();

  auto appendTexture = [&](const LoadedTextureRgba8& texture) -> bool {
    if (m_gpuTexels.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
      m_error = "texture buffer offset overflow";
      LogError("build_texture_buffers: " + m_error);
      return false;
    }
    const uint32_t offset = static_cast<uint32_t>(m_gpuTexels.size());
    m_gpuTexels.insert(m_gpuTexels.end(), texture.texels.begin(), texture.texels.end());
    m_gpuTextureMeta.push_back(offset);
    m_gpuTextureMeta.push_back(std::max(1u, texture.width));
    m_gpuTextureMeta.push_back(std::max(1u, texture.height));
    m_gpuTextureMeta.push_back(1u);
    return true;
  };

  if (m_sceneData.textures.empty()) {
    return appendTexture(LoadedTextureRgba8{});
  }

  for (const auto& uri : m_sceneData.textures) {
    LoadedTextureRgba8 texture;
    std::string loadError;
    if (!LoadTextureRgba8(uri, kMaxTextureDimension, texture, &loadError)) {
      LogError("texture load failed uri=" + uri + " error=" + loadError);
      texture = LoadedTextureRgba8{};
    }
    if (!appendTexture(texture)) {
      return false;
    }
  }

  std::ostringstream log;
  log << "texture buffers built count=" << (m_gpuTextureMeta.size() / 4u)
      << " texels=" << m_gpuTexels.size()
      << " bytes=" << (m_gpuTexels.size() * sizeof(uint32_t));
  LogInfo(log.str());
  return true;
}

bool D3D12GpuPathTracer::upload_scene_buffers() {
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu before scene upload";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset())) {
    m_error = "upload cmd allocator reset";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  if (FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "upload cmd list reset";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }

  UINT64 offset = 0;
  auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };

  auto stage = [&](const char* name, const void* data, UINT64 size, ID3D12Resource** dst) {
    const bool shouldLogUpload = g_d3d12SceneUploadCalls < 8u;
    if (shouldLogUpload) {
      std::ostringstream stageInfo;
      stageInfo << "upload_scene stage begin name=" << name << " bytes=" << size;
      LogDebug(stageInfo.str());
    }
    if (offset + size > m_uploadSize) {
      m_error = std::string("upload_scene_buffers overflow for ") + name;
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    if (!data || size == 0u) {
      m_error = std::string("upload_scene_buffers invalid data for ") + name;
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    const UINT64 stagedOffset = offset;
    std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + offset, data, static_cast<size_t>(size));

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC  rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=size; rd.Height=1;
    rd.DepthOrArraySize=1; rd.MipLevels=1; rd.Format=DXGI_FORMAT_UNKNOWN;
    rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.SampleDesc.Count=1;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(dst)))) {
      m_error = "upload_stage create resource failed";
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    ++g_d3d12SceneUploadCalls;
    if (shouldLogUpload) {
      std::ostringstream st;
      st << "upload_scene stage " << name << " offset=" << stagedOffset
         << " bytes=" << size;
      if (size > 0u && std::strcmp(name, "verts") == 0) {
        st << " first=" << FormatFirstN(reinterpret_cast<const float*>(data), size / sizeof(float), 8u);
      } else if (size > 0u && (std::strcmp(name, "mats") == 0 || std::strcmp(name, "lights") == 0)) {
        st << " first=" << FormatFirstN(reinterpret_cast<const float*>(data), size / sizeof(float), 8u);
      } else if (size > 0u && (std::strcmp(name, "idx") == 0 || std::strcmp(name, "inst") == 0)) {
        st << " first=" << FormatFirstN(reinterpret_cast<const uint32_t*>(data), size / sizeof(uint32_t), 8u);
      }
      LogDebug(st.str());
    }
    m_cmdList->CopyBufferRegion(*dst, 0, m_uploadBuf.Get(), offset, size);
    offset = align(offset + size);
    return true;
  };

  if (!stage("verts", m_gpuVerts.data(),  m_gpuVerts.size()  * sizeof(float), &m_vertBuf)) return false;
  if (!stage("idx", m_gpuIdx.data(),      m_gpuIdx.size()    * sizeof(uint32_t), &m_idxBuf)) return false;
  if (!stage("mats", m_gpuMats.data(),   m_gpuMats.size()   * sizeof(float), &m_matBuf)) return false;
  if (!stage("inst", m_gpuInsts.data(),  m_gpuInsts.size()  * sizeof(uint32_t), &m_instBuf)) return false;
  if (!stage("lights", m_gpuLights.data(), m_gpuLights.size() * sizeof(float), &m_ltBuf)) return false;

  if (!stage("bvh",    m_gpuBvh.data(),    m_gpuBvh.size()    * sizeof(float),    &m_bvhBuf))    return false;
  if (!stage("trimat", m_gpuTriMat.data(), m_gpuTriMat.size() * sizeof(uint32_t), &m_triMatBuf)) return false;
  if (!stage("tridata", m_gpuTriData.data(), m_gpuTriData.size() * sizeof(float), &m_triDataBuf)) return false;
  if (!stage("dynamic_bvh", m_gpuDynamicBvh.data(), m_gpuDynamicBvh.size() * sizeof(float), &m_dynamicBvhBuf)) return false;
  if (!stage("local_bvh", m_gpuLocalBvh.data(), m_gpuLocalBvh.size() * sizeof(float), &m_localBvhBuf)) return false;
  if (!stage("sdf", m_gpuSdfs.data(), m_gpuSdfs.size() * sizeof(float), &m_sdfBuf)) return false;
  if (!stage("texels", m_gpuTexels.data(), m_gpuTexels.size() * sizeof(uint32_t), &m_texelBuf)) return false;
  if (!stage("texmeta", m_gpuTextureMeta.data(), m_gpuTextureMeta.size() * sizeof(uint32_t), &m_texMetaBuf)) return false;

  // Transition all to non-pixel shader resource
  D3D12_RESOURCE_BARRIER barriers[13]{};
  ID3D12Resource* bufs[] = {m_vertBuf.Get(), m_idxBuf.Get(), m_matBuf.Get(),
                            m_instBuf.Get(), m_ltBuf.Get(), m_bvhBuf.Get(), m_triMatBuf.Get(),
                            m_triDataBuf.Get(), m_dynamicBvhBuf.Get(), m_localBvhBuf.Get(), m_sdfBuf.Get(),
                            m_texelBuf.Get(), m_texMetaBuf.Get()};
  for (int i = 0; i < 13; ++i) {
    barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[i].Transition.pResource=bufs[i];
    barriers[i].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[i].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  m_cmdList->ResourceBarrier(13, barriers);
  const auto closeRes = m_cmdList->Close();
  if (FAILED(closeRes)) {
    m_error = "cmd list close";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu failed";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during upload_scene_buffers hr=" + FormatHr(removeHr);
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  LogDebug("upload_scene_buffers complete");
  return true;
}

bool D3D12GpuPathTracer::upload_instance_buffer() {
  if (!m_instBuf || !m_dynamicBvhBuf || m_gpuInsts.empty() || m_gpuDynamicBvh.empty()) {
    return false;
  }
  const UINT64 instSize = static_cast<UINT64>(m_gpuInsts.size()) * sizeof(uint32_t);
  const UINT64 tlasSize = static_cast<UINT64>(m_gpuDynamicBvh.size()) * sizeof(float);
  const auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };
  const UINT64 tlasOffset = align(instSize);
  if (tlasOffset + tlasSize > m_uploadSize) {
    m_error = "upload_instance_buffer overflow";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  if (!wait_for_gpu()) {
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "instance upload command reset failed";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }

  std::memcpy(m_uploadPtr, m_gpuInsts.data(), static_cast<std::size_t>(instSize));
  std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + tlasOffset,
              m_gpuDynamicBvh.data(),
              static_cast<std::size_t>(tlasSize));
  D3D12_RESOURCE_BARRIER toCopy[2]{};
  ID3D12Resource* resources[2] = {m_instBuf.Get(), m_dynamicBvhBuf.Get()};
  for (int i = 0; i < 2; ++i) {
    toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy[i].Transition.pResource = resources[i];
    toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  m_cmdList->ResourceBarrier(2, toCopy);
  m_cmdList->CopyBufferRegion(m_instBuf.Get(), 0, m_uploadBuf.Get(), 0, instSize);
  m_cmdList->CopyBufferRegion(m_dynamicBvhBuf.Get(), 0, m_uploadBuf.Get(), tlasOffset, tlasSize);

  D3D12_RESOURCE_BARRIER toSrv[2] = {toCopy[0], toCopy[1]};
  for (auto& barrier : toSrv) {
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  m_cmdList->ResourceBarrier(2, toSrv);

  if (FAILED(m_cmdList->Close())) {
    m_error = "instance upload command close failed";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during upload_instance_buffer hr=" + FormatHr(removeHr);
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  return true;
}

void D3D12GpuPathTracer::destroy_scene_buffers() {
  m_vertBuf.Reset(); m_idxBuf.Reset(); m_matBuf.Reset();
  m_instBuf.Reset(); m_ltBuf.Reset();
  m_bvhBuf.Reset();  m_triMatBuf.Reset();
  m_triDataBuf.Reset();
  m_dynamicBvhBuf.Reset();
  m_localBvhBuf.Reset();
  m_sdfBuf.Reset();
  m_texelBuf.Reset();
  m_texMetaBuf.Reset();
  m_srvUavHeap.Reset();
  m_sceneUploaded = false;
}

void D3D12GpuPathTracer::destroy_film_buffer() {
  if (m_filmReadbackPtr && m_filmReadbackBuf) m_filmReadbackBuf->Unmap(0, nullptr);
  m_filmReadbackPtr = nullptr;
  m_filmReadbackBuf.Reset();
  m_filmBuf.Reset();
  if (m_ldrReadbackPtr && m_ldrReadbackBuf) m_ldrReadbackBuf->Unmap(0, nullptr);
  m_ldrReadbackPtr = nullptr;
  m_ldrReadbackBuf.Reset();
  m_ldrBuf.Reset();
  m_denoiseBuf.Reset();
  m_guideBuf.Reset();
  m_temporalBuf.Reset();
  m_temporalHistoryBuf.Reset();
  m_prevGuideBuf.Reset();
  m_temporalHistoryValid = false;
  m_clearHeap.Reset();
  m_clearCpuHeap.Reset();
  m_srvUavHeap.Reset();
}

bool D3D12GpuPathTracer::wait_for_gpu() {
  ++m_fenceValue;
  const HRESULT signalHr = m_cmdQueue->Signal(m_fence.Get(), m_fenceValue);
  if (FAILED(signalHr)) {
    m_error = "Signal failed hr=" + FormatHr(signalHr);
    LogError("wait_for_gpu: " + m_error);
    return false;
  }
  if (m_fence->GetCompletedValue() < m_fenceValue) {
    const HRESULT setEvHr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    if (FAILED(setEvHr)) {
      m_error = "SetEventOnCompletion failed hr=" + FormatHr(setEvHr);
      LogError("wait_for_gpu: " + m_error);
      return false;
    }
    const DWORD waitRes = WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    if (waitRes != WAIT_OBJECT_0) {
      m_error = "WaitForSingleObjectEx failed code=" + std::to_string(waitRes);
      LogError("wait_for_gpu: " + m_error);
      return false;
    }
    return true;
  }
  return true;
}

// ============================================================================
// DXR: compile DXIL, create global root sig, build pipeline, build AS, dispatch
// ============================================================================

bool D3D12GpuPathTracer::wait_for_dxr_gpu() {
  ++m_dxrFenceValue;
  const HRESULT signalHr = m_dxrCmdQueue->Signal(m_dxrFence.Get(), m_dxrFenceValue);
  if (FAILED(signalHr)) {
    m_error = "dxr Signal hr=" + FormatHr(signalHr);
    LogError("wait_for_dxr_gpu: " + m_error);
    return false;
  }
  if (m_dxrFence->GetCompletedValue() < m_dxrFenceValue) {
    m_dxrFence->SetEventOnCompletion(m_dxrFenceValue, m_dxrFenceEvent);
    if (WaitForSingleObjectEx(m_dxrFenceEvent, INFINITE, FALSE) != WAIT_OBJECT_0) {
      m_error = "dxr WaitForSingleObjectEx failed";
      LogError("wait_for_dxr_gpu: " + m_error);
      return false;
    }
  }
  return true;
}

void D3D12GpuPathTracer::destroy_dxr_resources() {
  if (m_sbtMappedPtr && m_sbtBuffer) { m_sbtBuffer->Unmap(0, nullptr); m_sbtMappedPtr = nullptr; }
  m_rtPsoProps.Reset();    m_rtPso.Reset();
  m_sbtBuffer.Reset();     m_dxrDescHeap.Reset();
  m_dxrGlobalRootSig.Reset();
  m_blasBuffer.Reset();    m_blasScratch.Reset();
  m_dxrBlasBuffers.clear();
  m_dxrBlasScratch.clear();
  m_dxrInstanceDescs.clear();
  m_blasVertUpload.Reset();
  m_blasIdxUpload.Reset();
  m_tlasBuffer.Reset();    m_tlasScratch.Reset();
  m_tlasInstanceBuf.Reset();
  if (m_dxrFenceEvent) { CloseHandle(m_dxrFenceEvent); m_dxrFenceEvent = nullptr; }
  m_dxrFence.Reset();
  m_dxrAccelReady    = false;
  m_dxrPipelineReady = false;
}

bool D3D12GpuPathTracer::compile_dxil(const std::string& path, std::vector<uint8_t>& outDxil) {
  // Load dxil.dll FIRST (for signing), then dxcompiler.dll
  static HMODULE s_dxcLib = nullptr;
  if (!s_dxcLib) {
    const wchar_t* kSdkDir =
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\";
    // Pre-load dxil.dll so DXC can find it for DXIL signing
    wchar_t dxilPath[MAX_PATH]{};
    wcscpy_s(dxilPath, kSdkDir); wcscat_s(dxilPath, L"dxil.dll");
    HMODULE dxilMod = LoadLibraryW(dxilPath);
    if (!dxilMod) {
      // Try bare name (might be in PATH)
      dxilMod = LoadLibraryW(L"dxil.dll");
    }
    if (dxilMod) LogDebug("compile_dxil: dxil.dll loaded for signing");
    else          LogInfo("compile_dxil: dxil.dll not found — DXIL will be unsigned");

    // Load dxcompiler.dll
    wchar_t dxcPath[MAX_PATH]{};
    wcscpy_s(dxcPath, kSdkDir); wcscat_s(dxcPath, L"dxcompiler.dll");
    s_dxcLib = LoadLibraryW(dxcPath);
    if (!s_dxcLib) s_dxcLib = LoadLibraryW(L"dxcompiler.dll");
    if (!s_dxcLib) {
      m_error = "dxcompiler.dll not found";
      LogError("compile_dxil: " + m_error);
      return false;
    }
  }

  typedef HRESULT (__stdcall *PFN_DxcCreate)(REFCLSID, REFIID, LPVOID*);
  auto dxcCreate = (PFN_DxcCreate)GetProcAddress(s_dxcLib, "DxcCreateInstance");
  if (!dxcCreate) {
    m_error = "DxcCreateInstance not in dxcompiler.dll";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcUtils>     utils;
  Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
  if (FAILED(dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
      FAILED(dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
    m_error = "DXC utils/compiler create failed";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
  std::wstring wpath(path.begin(), path.end());
  if (FAILED(utils->LoadFile(wpath.c_str(), nullptr, &sourceBlob))) {
    m_error = "DXC LoadFile failed: " + path;
    LogError("compile_dxil: " + m_error);
    return false;
  }

  DxcBuffer src{};
  src.Ptr      = sourceBlob->GetBufferPointer();
  src.Size     = sourceBlob->GetBufferSize();
  src.Encoding = DXC_CP_ACP;

  const bool dxrShadowRays = ParseEnvBool("PT_D3D12_DXR_SHADOW_RAYS", true);
  const wchar_t* shadowRayDefine =
      dxrShadowRays ? L"-DPT_D3D12_DXR_SHADOW_RAYS=1" : L"-DPT_D3D12_DXR_SHADOW_RAYS=0";
  LPCWSTR args[] = { L"-T", L"lib_6_3", L"-HV", L"2021", L"-O3", shadowRayDefine };
  Microsoft::WRL::ComPtr<IDxcResult> result;
  HRESULT hr = compiler->Compile(&src, args, ARRAYSIZE(args), nullptr, IID_PPV_ARGS(&result));

  Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr))
      && errors && errors->GetStringLength() > 0) {
    LogError("DXC errors:\n" + std::string(errors->GetStringPointer(), errors->GetStringLength()));
  }

  HRESULT status = E_FAIL;
  if (result) result->GetStatus(&status);
  if (FAILED(hr) || FAILED(status)) {
    m_error = "DXC compile failed for " + path;
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcBlob> dxilBlob;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxilBlob), nullptr))
      || !dxilBlob || dxilBlob->GetBufferSize() == 0) {
    m_error = "DXC produced empty DXIL blob";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  outDxil.resize(dxilBlob->GetBufferSize());
  std::memcpy(outDxil.data(), dxilBlob->GetBufferPointer(), outDxil.size());
  LogInfo("DXR shader compiled " + std::to_string(outDxil.size()) + " bytes from " + path +
          " shadow_rays=" + (dxrShadowRays ? "true" : "false"));
  return true;
}

bool D3D12GpuPathTracer::create_dxr_global_root_sig() {
  // Param 0: constants CBV (PCBuf) at b0
  // Param 1: root SRV at t0 (TLAS)
  // Param 2: descriptor table [t1-t9 SRV, u0 UAV]
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace  = 0;
  params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType                = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister   = 0;   // t0
  params[1].Descriptor.RegisterSpace    = 0;
  params[1].ShaderVisibility            = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 9;          // t1-t9
  ranges[0].BaseShaderRegister = 1;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;          // u0
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 9;
  params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 2;
  params[2].DescriptorTable.pDescriptorRanges   = ranges;
  params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 3;
  rsd.pParameters   = params;
  rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(hr)) {
    m_error = "dxr SerializeRootSignature";
    if (err) LogError(static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  const HRESULT createHr = m_device->CreateRootSignature(0, sig->GetBufferPointer(),
      sig->GetBufferSize(), IID_PPV_ARGS(&m_dxrGlobalRootSig));
  if (FAILED(createHr)) {
    m_error = "dxr CreateRootSignature hr=" + FormatHr(createHr);
    LogError("create_dxr_global_root_sig: " + m_error);
    return false;
  }
  LogDebug("DXR global root sig created");
  return true;
}

bool D3D12GpuPathTracer::create_dxr_pipeline() {
  if (!create_dxr_global_root_sig()) return false;

  // Compile DXIL library
  std::vector<uint8_t> dxil;
  if (!compile_dxil(m_rtHlslPath, dxil)) return false;

  // Subobject array: library, hitgroup, shaderconfig, pipelineconfig, globalrootsig
  constexpr UINT kNumSubobjects = 5;
  D3D12_STATE_SUBOBJECT subobjects[kNumSubobjects]{};
  UINT si = 0;

  // 1. DXIL library
  D3D12_EXPORT_DESC exports[3]{};
  exports[0] = {L"RayGen",    nullptr, D3D12_EXPORT_FLAG_NONE};
  exports[1] = {L"Miss",      nullptr, D3D12_EXPORT_FLAG_NONE};
  exports[2] = {L"ClosestHit",nullptr, D3D12_EXPORT_FLAG_NONE};
  D3D12_DXIL_LIBRARY_DESC libDesc{};
  libDesc.DXILLibrary = {dxil.data(), dxil.size()};
  libDesc.NumExports  = 3;
  libDesc.pExports    = exports;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc};

  // 2. Hit group
  D3D12_HIT_GROUP_DESC hitGroup{};
  hitGroup.HitGroupExport          = L"HitGroup0";
  hitGroup.Type                    = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hitGroup.ClosestHitShaderImport  = L"ClosestHit";
  hitGroup.AnyHitShaderImport      = nullptr;
  hitGroup.IntersectionShaderImport= nullptr;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup};

  // 3. Shader config (payload + attribute sizes)
  D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
  shaderCfg.MaxPayloadSizeInBytes   = 56; // sizeof(PathPayload) in pathtrace_rt.hlsl
  shaderCfg.MaxAttributeSizeInBytes = 8;  // float2 barycentrics
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderCfg};

  // 4. Pipeline config (max recursion = 1 since we loop in raygen)
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg{};
  pipelineCfg.MaxTraceRecursionDepth = 1;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineCfg};

  // 5. Global root signature
  D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig{};
  globalRootSig.pGlobalRootSignature = m_dxrGlobalRootSig.Get();
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig};

  D3D12_STATE_OBJECT_DESC soDesc{};
  soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  soDesc.NumSubobjects = si;
  soDesc.pSubobjects   = subobjects;

  const HRESULT soHr = m_device5->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_rtPso));
  if (FAILED(soHr)) {
    m_error = "CreateStateObject (DXR pipeline) hr=" + FormatHr(soHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }
  const HRESULT propsHr = m_rtPso->QueryInterface(IID_PPV_ARGS(&m_rtPsoProps));
  if (FAILED(propsHr)) {
    m_error = "QueryInterface ID3D12StateObjectProperties hr=" + FormatHr(propsHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }

  // Build shader binding table (3 x 64-byte records: raygen, miss, hitgroup)
  constexpr UINT kSbtRecordSize  = 32; // D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
  constexpr UINT kSbtRecordAlign = 64; // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
  constexpr UINT kSbtTotalBytes  = kSbtRecordAlign * 3; // raygen + miss + hitgroup

  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = kSbtTotalBytes;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;

  const HRESULT sbtHr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_sbtBuffer));
  if (FAILED(sbtHr)) {
    m_error = "SBT CreateCommittedResource hr=" + FormatHr(sbtHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }
  m_sbtBuffer->Map(0, nullptr, &m_sbtMappedPtr);
  std::memset(m_sbtMappedPtr, 0, kSbtTotalBytes);

  auto* sbtBytes = static_cast<uint8_t*>(m_sbtMappedPtr);
  void* rayGenId  = m_rtPsoProps->GetShaderIdentifier(L"RayGen");
  void* missId    = m_rtPsoProps->GetShaderIdentifier(L"Miss");
  void* hitId     = m_rtPsoProps->GetShaderIdentifier(L"HitGroup0");
  if (!rayGenId || !missId || !hitId) {
    m_error = "GetShaderIdentifier returned null — names may not match PSO exports";
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }
  std::memcpy(sbtBytes + 0,              rayGenId, kSbtRecordSize);
  std::memcpy(sbtBytes + kSbtRecordAlign, missId,  kSbtRecordSize);
  std::memcpy(sbtBytes + kSbtRecordAlign * 2, hitId, kSbtRecordSize);

  m_dxrPipelineReady = true;
  LogInfo("DXR pipeline ready; SBT written (" + std::to_string(kSbtTotalBytes) + " bytes)");
  return true;
}

bool D3D12GpuPathTracer::create_dxr_desc_heap() {
  // 7 descriptors: slots 0-5 → scene SRVs (t1-t6), slot 6 → film UAV (u0)
  D3D12_DESCRIPTOR_HEAP_DESC dh{};
  dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  dh.NumDescriptors = 10;
  dh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  const HRESULT hr = m_device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dxrDescHeap));
  if (FAILED(hr)) {
    m_error = "dxr CreateDescriptorHeap hr=" + FormatHr(hr);
    LogError("create_dxr_desc_heap: " + m_error);
    return false;
  }

  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_dxrDescHeap->GetCPUDescriptorHandleForHeapStart();

  auto makeSrv = [&](UINT slot, ID3D12Resource* buf, UINT64 bytes, DXGI_FORMAT fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = static_cast<UINT>(bytes / 4u);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += slot * inc;
    m_device->CreateShaderResourceView(buf, &srv, h);
  };
  makeSrv(0, m_vertBuf.Get(),   m_gpuVerts.size()  * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(1, m_idxBuf.Get(),    m_gpuIdx.size()    * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(2, m_matBuf.Get(),    m_gpuMats.size()   * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(3, m_instBuf.Get(),   m_gpuInsts.size()  * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(4, m_ltBuf.Get(),     m_gpuLights.size() * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(5, m_triMatBuf.Get(), m_gpuTriMat.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(6, m_triDataBuf.Get(), m_gpuTriData.size() * sizeof(float),   DXGI_FORMAT_R32_FLOAT);
  makeSrv(7, m_texelBuf.Get(), m_gpuTexels.size() * sizeof(uint32_t),   DXGI_FORMAT_R32_UINT);
  makeSrv(8, m_texMetaBuf.Get(), m_gpuTextureMeta.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);

  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(filmSize);
  D3D12_CPU_DESCRIPTOR_HANDLE h9 = cpu;
  h9.ptr += 9 * inc;
  m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &uav, h9);
  LogDebug("DXR descriptor heap created");
  return true;
}

bool D3D12GpuPathTracer::build_dxr_acceleration_structures() {
  if (!m_device5 || !m_cmdList || !m_vertBuf || !m_idxBuf || m_gpuVerts.empty() || m_gpuIdx.empty()) {
    m_error = "build_dxr_acceleration_structures: missing device5 or scene buffers";
    return false;
  }

  m_blasBuffer.Reset();
  m_blasScratch.Reset();
  m_tlasBuffer.Reset();
  m_tlasScratch.Reset();
  m_tlasInstanceBuf.Reset();
  m_dxrBlasBuffers.clear();
  m_dxrBlasScratch.clear();
  m_dxrInstanceDescs.clear();

  auto createBuffer = [&](UINT64 bytes,
                          D3D12_HEAP_TYPE heapType,
                          D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES initialState,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool {
    bytes = std::max<UINT64>(bytes, 4ull);
    D3D12_HEAP_PROPERTIES hp{heapType};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    const HRESULT hr = m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
      m_error = "DXR buffer create hr=" + FormatHr(hr);
      return false;
    }
    return true;
  };

  const UINT64 vertBytes = static_cast<UINT64>(m_gpuVerts.size()) * sizeof(float);
  const UINT64 idxBytes = static_cast<UINT64>(m_gpuIdx.size()) * sizeof(uint32_t);
  if (!createBuffer(vertBytes,
                    D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    m_blasVertUpload) ||
      !createBuffer(idxBytes,
                    D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    m_blasIdxUpload)) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  void* uploadPtr = nullptr;
  if (FAILED(m_blasVertUpload->Map(0, nullptr, &uploadPtr))) {
    m_error = "DXR vertex upload map failed";
    return false;
  }
  std::memcpy(uploadPtr, m_gpuVerts.data(), static_cast<std::size_t>(vertBytes));
  m_blasVertUpload->Unmap(0, nullptr);
  if (FAILED(m_blasIdxUpload->Map(0, nullptr, &uploadPtr))) {
    m_error = "DXR index upload map failed";
    return false;
  }
  std::memcpy(uploadPtr, m_gpuIdx.data(), static_cast<std::size_t>(idxBytes));
  m_blasIdxUpload->Unmap(0, nullptr);

  if (!wait_for_gpu()) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> buildList;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&buildList)))) {
    m_error = "build_dxr_acceleration_structures: compute command list is not ID3D12GraphicsCommandList4";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(buildList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "build_dxr_acceleration_structures: command reset failed";
    LogError(m_error);
    return false;
  }

  auto buildBlasRange = [&](uint32_t firstTriangle,
                            uint32_t triangleCount,
                            D3D12_GPU_VIRTUAL_ADDRESS& blasAddress) -> bool {
    const uint64_t firstIndex = static_cast<uint64_t>(firstTriangle) * 3ull;
    const uint64_t indexCount = static_cast<uint64_t>(triangleCount) * 3ull;
    if (triangleCount == 0u || firstIndex + indexCount > m_gpuIdx.size()) {
      m_error = "build_dxr_acceleration_structures: invalid BLAS triangle range";
      return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geomDesc.Triangles.VertexBuffer.StartAddress = m_blasVertUpload->GetGPUVirtualAddress();
    geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3u;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.VertexCount = static_cast<UINT>(m_gpuVerts.size() / 3u);
    geomDesc.Triangles.IndexBuffer =
        m_blasIdxUpload->GetGPUVirtualAddress() + firstIndex * sizeof(uint32_t);
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc.Triangles.IndexCount = static_cast<UINT>(indexCount);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = DxrBuildPreferenceFlags(m_dxrBuildMode);
    inputs.NumDescs = 1u;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geomDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0u) {
      m_error = "build_dxr_acceleration_structures: empty BLAS prebuild result";
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> blas;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    if (!createBuffer(info.ResultDataMaxSizeInBytes,
                      D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                      blas) ||
        !createBuffer(info.ScratchDataSizeInBytes,
                      D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      scratch)) {
      return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = inputs;
    buildDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    buildList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = blas.Get();
    buildList->ResourceBarrier(1, &barrier);

    blasAddress = blas->GetGPUVirtualAddress();
    m_dxrBlasBuffers.push_back(blas);
    m_dxrBlasScratch.push_back(scratch);
    return true;
  };

  D3D12_GPU_VIRTUAL_ADDRESS staticBlas = 0u;
  if (m_staticTriangleCount > 0u) {
    if (!buildBlasRange(0u, m_staticTriangleCount, staticBlas)) {
      LogError("build_dxr_acceleration_structures: static BLAS failed: " + m_error);
      return false;
    }
    m_dxrInstanceDescs.push_back(MakeDxrInstanceDesc(
        kDxrStaticInstanceId,
        staticBlas,
        {0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f}));
  }

  const uint32_t instanceCount = static_cast<uint32_t>(m_gpuInsts.size() / kGpuInstanceStrideU32);
  for (uint32_t instanceIndex = 0u; instanceIndex < instanceCount; ++instanceIndex) {
    const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    const uint32_t firstTriangle = m_gpuInsts[ib + 0u];
    const uint32_t triangleCount = m_gpuInsts[ib + 1u];
    const uint32_t flags = m_gpuInsts[ib + 3u];
    if ((flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u || triangleCount == 0u) {
      continue;
    }
    D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0u;
    if (!buildBlasRange(firstTriangle, triangleCount, blasAddress)) {
      LogError("build_dxr_acceleration_structures: dynamic BLAS failed: " + m_error);
      return false;
    }
    const vkpt::pathtracer::Vec3 translation{
        UintBitsToFloat(m_gpuInsts[ib + 4u]),
        UintBitsToFloat(m_gpuInsts[ib + 5u]),
        UintBitsToFloat(m_gpuInsts[ib + 6u])};
    const vkpt::pathtracer::Quat4 rotation{
        UintBitsToFloat(m_gpuInsts[ib + 8u]),
        UintBitsToFloat(m_gpuInsts[ib + 9u]),
        UintBitsToFloat(m_gpuInsts[ib + 10u]),
        UintBitsToFloat(m_gpuInsts[ib + 11u])};
    const vkpt::pathtracer::Vec3 scale{
        UintBitsToFloat(m_gpuInsts[ib + 12u]),
        UintBitsToFloat(m_gpuInsts[ib + 13u]),
        UintBitsToFloat(m_gpuInsts[ib + 14u])};
    m_dxrInstanceDescs.push_back(
        MakeDxrInstanceDesc(instanceIndex, blasAddress, translation, rotation, scale));
  }

  if (m_dxrInstanceDescs.empty()) {
    m_error = "build_dxr_acceleration_structures: no TLAS instances";
    LogError(m_error);
    return false;
  }
  m_blasBuffer = m_dxrBlasBuffers.front();
  m_blasScratch = m_dxrBlasScratch.front();

  const UINT64 instanceBytes =
      static_cast<UINT64>(m_dxrInstanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  if (!createBuffer(instanceBytes,
                    D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    m_tlasInstanceBuf)) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  void* instancePtr = nullptr;
  if (FAILED(m_tlasInstanceBuf->Map(0, nullptr, &instancePtr))) {
    m_error = "build_dxr_acceleration_structures: TLAS instance map failed";
    return false;
  }
  std::memcpy(instancePtr, m_dxrInstanceDescs.data(), static_cast<std::size_t>(instanceBytes));
  m_tlasInstanceBuf->Unmap(0, nullptr);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags = AddDxrBuildFlags(
      DxrBuildPreferenceFlags(m_dxrBuildMode),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
  tlasInputs.NumDescs = static_cast<UINT>(m_dxrInstanceDescs.size());
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlasInputs.InstanceDescs = m_tlasInstanceBuf->GetGPUVirtualAddress();

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
  m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
  const UINT64 tlasScratchBytes =
      std::max(tlasInfo.ScratchDataSizeInBytes, tlasInfo.UpdateScratchDataSizeInBytes);
  if (tlasInfo.ResultDataMaxSizeInBytes == 0u ||
      !createBuffer(tlasInfo.ResultDataMaxSizeInBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                    m_tlasBuffer) ||
      !createBuffer(tlasScratchBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    m_tlasScratch)) {
    LogError("build_dxr_acceleration_structures: TLAS alloc failed: " + m_error);
    return false;
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc{};
  tlasDesc.Inputs = tlasInputs;
  tlasDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  tlasDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
  buildList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

  D3D12_RESOURCE_BARRIER tlasBarrier{};
  tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  tlasBarrier.UAV.pResource = m_tlasBuffer.Get();
  buildList->ResourceBarrier(1, &tlasBarrier);

  if (FAILED(buildList->Close())) {
    m_error = "build_dxr_acceleration_structures: command close failed";
    LogError(m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {buildList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  const HRESULT removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR AS build hr=" + FormatHr(removeHr);
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }

  m_dxrDescHeap.Reset();
  if (!create_dxr_desc_heap()) {
    return false;
  }
  m_dxrAccelReady = true;
  LogInfo("DXR BLAS/TLAS built: blas=" + std::to_string(m_dxrBlasBuffers.size()) +
          " tlas_instances=" + std::to_string(m_dxrInstanceDescs.size()) +
          " dynamic=" + std::to_string(m_dxrInstanceDescs.size() - (m_staticTriangleCount > 0u ? 1u : 0u)));
  return true;
}

bool D3D12GpuPathTracer::update_dxr_instance_buffer_and_tlas() {
  if (!m_device5 || !m_cmdList || !m_tlasBuffer || !m_tlasScratch ||
      !m_tlasInstanceBuf || m_dxrInstanceDescs.empty()) {
    m_error = "update_dxr_instance_buffer_and_tlas: DXR TLAS resources are not ready";
    LogError(m_error);
    return false;
  }
  if (m_gpuInsts.size() < m_sceneData.instances.size() * kGpuInstanceStrideU32) {
    m_error = "update_dxr_instance_buffer_and_tlas: instance buffer is incomplete";
    LogError(m_error);
    return false;
  }

  for (auto& desc : m_dxrInstanceDescs) {
    if (desc.InstanceID == kDxrStaticInstanceId) {
      continue;
    }
    const uint32_t instanceIndex = desc.InstanceID;
    if (instanceIndex >= m_sceneData.instances.size()) {
      continue;
    }
    const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    const vkpt::pathtracer::Vec3 translation{
        UintBitsToFloat(m_gpuInsts[ib + 4u]),
        UintBitsToFloat(m_gpuInsts[ib + 5u]),
        UintBitsToFloat(m_gpuInsts[ib + 6u])};
    const vkpt::pathtracer::Quat4 rotation{
        UintBitsToFloat(m_gpuInsts[ib + 8u]),
        UintBitsToFloat(m_gpuInsts[ib + 9u]),
        UintBitsToFloat(m_gpuInsts[ib + 10u]),
        UintBitsToFloat(m_gpuInsts[ib + 11u])};
    const vkpt::pathtracer::Vec3 scale{
        UintBitsToFloat(m_gpuInsts[ib + 12u]),
        UintBitsToFloat(m_gpuInsts[ib + 13u]),
        UintBitsToFloat(m_gpuInsts[ib + 14u])};
    const auto blasAddress = desc.AccelerationStructure;
    desc = MakeDxrInstanceDesc(instanceIndex, blasAddress, translation, rotation, scale);
  }

  const UINT64 instanceBytes =
      static_cast<UINT64>(m_dxrInstanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  void* instancePtr = nullptr;
  if (FAILED(m_tlasInstanceBuf->Map(0, nullptr, &instancePtr))) {
    m_error = "update_dxr_instance_buffer_and_tlas: TLAS instance map failed";
    LogError(m_error);
    return false;
  }
  std::memcpy(instancePtr, m_dxrInstanceDescs.data(), static_cast<std::size_t>(instanceBytes));
  m_tlasInstanceBuf->Unmap(0, nullptr);

  if (!wait_for_gpu()) {
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&cl4)))) {
    m_error = "update_dxr_instance_buffer_and_tlas: compute command list is not ID3D12GraphicsCommandList4";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(cl4->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "update_dxr_instance_buffer_and_tlas: command reset failed";
    LogError(m_error);
    return false;
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags = AddDxrBuildFlags(
      AddDxrBuildFlags(DxrBuildPreferenceFlags(m_dxrBuildMode),
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
  tlasInputs.NumDescs = static_cast<UINT>(m_dxrInstanceDescs.size());
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlasInputs.InstanceDescs = m_tlasInstanceBuf->GetGPUVirtualAddress();

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC updateDesc{};
  updateDesc.Inputs = tlasInputs;
  updateDesc.SourceAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  updateDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  updateDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
  cl4->BuildRaytracingAccelerationStructure(&updateDesc, 0, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = m_tlasBuffer.Get();
  cl4->ResourceBarrier(1, &barrier);

  if (FAILED(cl4->Close())) {
    m_error = "update_dxr_instance_buffer_and_tlas: command close failed";
    LogError(m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {cl4.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  const HRESULT removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR TLAS update hr=" + FormatHr(removeHr);
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  return true;
}

bool D3D12GpuPathTracer::dispatch_dxr_rays(uint32_t sample_idx, uint32_t frame_idx, bool doReadback) {
  // Use the compute queue's command list (same queue as BLAS build) to avoid
  // any cross-queue memory visibility issues with the acceleration structures.
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&cl4)))) {
    m_error = "dispatch_dxr_rays: CL4 QI failed — DXR not supported on compute list";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(cl4->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "dispatch_dxr_rays: cmd list reset failed";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Build PathTraceConstants (identical layout to compute path)
  const auto& sc = m_sceneData;
  auto norm3 = [](float x, float y, float z, float* out) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0]=x/l; out[1]=y/l; out[2]=z/l;
  };
  float fwd[3]; norm3(sc.camera_target.x - sc.camera_position.x,
      sc.camera_target.y - sc.camera_position.y,
      sc.camera_target.z - sc.camera_position.z, fwd);
  float rt[3]  = {fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
                  fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
                  fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x};
  float rn[3]; norm3(rt[0], rt[1], rt[2], rn);
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
      rn[2]*fwd[0]-rn[0]*fwd[2], rn[0]*fwd[1]-rn[1]*fwd[0], un);

  PathTraceConstants pc{};
  pc.camera_pos_x = sc.camera_position.x;
  pc.camera_pos_y = sc.camera_position.y;
  pc.camera_pos_z = sc.camera_position.z;
  pc.fov_tan_half = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_fwd_x=fwd[0]; pc.cam_fwd_y=fwd[1]; pc.cam_fwd_z=fwd[2];
  pc.aspect    = static_cast<float>(m_settings.width) /
                 std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right_x=rn[0]; pc.cam_right_y=rn[1]; pc.cam_right_z=rn[2];
  pc.num_sdfs = static_cast<uint32_t>(sc.sdf_primitives.size());
  pc.cam_up_x=un[0]; pc.cam_up_y=un[1]; pc.cam_up_z=un[2];
  pc.sample_index = sample_idx;
  pc.num_insts    = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats     = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights   = static_cast<uint32_t>(sc.lights.size());
  pc.width        = m_settings.width;
  pc.height       = m_settings.height;
  pc.base_seed    = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                    ^ (frame_idx * 2654435761u);
  pc.env_r = sc.environment_color.x;
  pc.env_g = sc.environment_color.y;
  pc.env_b = sc.environment_color.z;
  pc.max_depth_f  = static_cast<float>(std::max(1u, m_settings.max_depth));
  pc.aperture_radius = sc.camera_aperture_radius > 0.0f
      ? sc.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  pc.focus_distance = sc.camera_focus_distance > 0.0f
      ? sc.camera_focus_distance
      : m_settings.camera_focus_distance;
  pc.iris_blade_count = std::min(sc.camera_iris_blade_count, 64u);
  pc.iris_rotation_radians = sc.camera_iris_rotation_degrees * (3.14159265f / 180.0f);
  pc.iris_roundness = std::clamp(sc.camera_iris_roundness, 0.0f, 1.0f);
  pc.anamorphic_squeeze = std::isfinite(sc.camera_anamorphic_squeeze)
      ? std::max(0.01f, sc.camera_anamorphic_squeeze)
      : 1.0f;
  const auto resolveSettings =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData);
  const auto whiteBalance = vkpt::pathtracer::WhiteBalanceScale(resolveSettings.white_balance_kelvin);
  pc.rays_per_pixel = std::max(1u, m_raysPerPixelPerDispatch);
  pc.exposure = resolveSettings.exposure;
  pc.tone_map = static_cast<uint32_t>(resolveSettings.tone_map);
  pc.output_transform = static_cast<uint32_t>(resolveSettings.output_transform);
  pc.gamma = resolveSettings.gamma;
  pc.clamp_output = resolveSettings.clamp_output ? 1u : 0u;
  pc.white_balance_r = whiteBalance.x;
  pc.white_balance_g = whiteBalance.y;
  pc.white_balance_b = whiteBalance.z;
  const bool doDenoise = doReadback && m_settings.enable_denoiser;
  const bool doTemporal = doReadback && m_settings.enable_temporal_aa;
  const bool doGuide = doDenoise || doTemporal;
  if (!m_settings.enable_temporal_aa) {
    m_temporalHistoryValid = false;
  }
  pc.denoiser_enabled = doDenoise ? 1u : 0u;
  pc.denoiser_strength = doDenoise ? 1.0f : 0.0f;
  pc.denoiser_color_sigma = 0.22f;
  pc.temporal_enabled = doTemporal ? 1u : 0u;
  pc.temporal_history_valid = (doTemporal && m_temporalHistoryValid) ? 1u : 0u;
  pc.temporal_feedback = 0.92f;
  pc.temporal_depth_sigma = 0.05f;
  pc.temporal_normal_power = 28.0f;
  pc.temporal_color_margin = 0.12f;
  FillPreviousCameraConstants(pc, m_temporalHistoryValid ? m_temporalPrevCamera : MakeTemporalCameraState(pc));
  if (doReadback && (!m_ldrBuf || !m_ldrReadbackBuf || !m_ldrReadbackPtr || !ensure_compute_srv_uav_heap())) {
    m_error = "DXR LDR readback resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doDenoise && (!m_guidePso || !m_denoisePso || !m_guideBuf || !m_denoiseBuf)) {
    m_error = "DXR GPU denoiser resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doTemporal && (!m_guidePso || !m_temporalPso || !m_guideBuf ||
                     !m_temporalBuf || !m_temporalHistoryBuf || !m_prevGuideBuf)) {
    m_error = "DXR GPU temporal AA resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Set global root signature and bind resources
  cl4->SetComputeRootSignature(m_dxrGlobalRootSig.Get());
  std::memcpy(m_uploadPtr, &pc, sizeof(pc));
  cl4->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());
  cl4->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());
  ID3D12DescriptorHeap* heaps[] = {m_dxrDescHeap.Get()};
  cl4->SetDescriptorHeaps(1, heaps);
  cl4->SetComputeRootDescriptorTable(2, m_dxrDescHeap->GetGPUDescriptorHandleForHeapStart());

  // Transition film to UAV for DXR accumulation.
  D3D12_RESOURCE_BARRIER filmBarrier{};
  filmBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  filmBarrier.Transition.pResource   = m_filmBuf.Get();
  filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl4->ResourceBarrier(1, &filmBarrier);

  // Set pipeline state and dispatch
  cl4->SetPipelineState1(m_rtPso.Get());

  // Log dispatch constants once when verbose D3D12 diagnostics are enabled.
  if (D3D12VerboseLoggingEnabled()) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      std::ostringstream dc;
      dc << "dispatch_dxr_rays constants: env=(" << pc.env_r << "," << pc.env_g << "," << pc.env_b
         << ") maxDepth=" << pc.max_depth_f << " rpp=" << pc.rays_per_pixel
         << " camPos=(" << pc.camera_pos_x << "," << pc.camera_pos_y << "," << pc.camera_pos_z << ")"
         << " camFwd=(" << pc.cam_fwd_x << "," << pc.cam_fwd_y << "," << pc.cam_fwd_z << ")"
         << " w=" << pc.width << " h=" << pc.height;
      LogDebug(dc.str());
    }
  }

  constexpr UINT kSbtAlign = 64; // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
  const D3D12_GPU_VIRTUAL_ADDRESS sbtGpu = m_sbtBuffer->GetGPUVirtualAddress();
  D3D12_DISPATCH_RAYS_DESC drd{};
  drd.RayGenerationShaderRecord = {sbtGpu + 0,          kSbtAlign};
  drd.MissShaderTable           = {sbtGpu + kSbtAlign,  kSbtAlign, kSbtAlign};
  drd.HitGroupTable             = {sbtGpu + kSbtAlign*2, kSbtAlign, kSbtAlign};
  drd.Width  = m_settings.width;
  drd.Height = m_settings.height;
  drd.Depth  = 1;
  constexpr wchar_t kDxrEvent[] = L"D3D12 DXR DispatchRays";
  cl4->BeginEvent(0, kDxrEvent, sizeof(kDxrEvent));
  cl4->DispatchRays(&drd);
  cl4->EndEvent();

  if (doReadback) {
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_filmBuf.Get();
    cl4->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER ldrBarrier{};
    ldrBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ldrBarrier.Transition.pResource   = m_ldrBuf.Get();
    ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ldrBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl4->ResourceBarrier(1, &ldrBarrier);

    D3D12_RESOURCE_BARRIER denoiseBarrier = MakeTransitionBarrier(
        m_denoiseBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER guideBarrier = MakeTransitionBarrier(
        m_guideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER temporalBarrier = MakeTransitionBarrier(
        m_temporalBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER temporalHistoryBarrier = MakeTransitionBarrier(
        m_temporalHistoryBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER prevGuideBarrier = MakeTransitionBarrier(
        m_prevGuideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (doDenoise || doGuide || doTemporal) {
      std::array<D3D12_RESOURCE_BARRIER, 5> startBarriers{};
      UINT barrierCount = 0u;
      if (doDenoise) startBarriers[barrierCount++] = denoiseBarrier;
      if (doGuide) startBarriers[barrierCount++] = guideBarrier;
      if (doTemporal) {
        startBarriers[barrierCount++] = temporalBarrier;
        startBarriers[barrierCount++] = temporalHistoryBarrier;
        startBarriers[barrierCount++] = prevGuideBarrier;
      }
      if (barrierCount > 0u) {
        cl4->ResourceBarrier(barrierCount, startBarriers.data());
      }
    }

    ID3D12DescriptorHeap* tonemapHeaps[] = {m_srvUavHeap.Get()};
    cl4->SetDescriptorHeaps(1, tonemapHeaps);
    cl4->SetComputeRootSignature(m_rootSig.Get());
    cl4->SetComputeRootDescriptorTable(1, m_srvUavHeap->GetGPUDescriptorHandleForHeapStart());
    cl4->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());
    const UINT groupsX = (m_settings.width + 7u) / 8u;
    const UINT groupsY = (m_settings.height + 7u) / 8u;
    if (doGuide) {
      cl4->SetPipelineState(m_guidePso.Get());
      constexpr wchar_t kDxrGuideEvent[] = L"D3D12 DXR Guide Dispatch";
      cl4->BeginEvent(0, kDxrGuideEvent, sizeof(kDxrGuideEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_guideBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }

    if (doTemporal) {
      cl4->SetPipelineState(m_temporalPso.Get());
      constexpr wchar_t kDxrTemporalEvent[] = L"D3D12 DXR Temporal AA Dispatch";
      cl4->BeginEvent(0, kDxrTemporalEvent, sizeof(kDxrTemporalEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_temporalBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }

    if (doDenoise) {
      cl4->SetPipelineState(m_denoisePso.Get());
      constexpr wchar_t kDxrDenoiseEvent[] = L"D3D12 DXR Denoise Dispatch";
      cl4->BeginEvent(0, kDxrDenoiseEvent, sizeof(kDxrDenoiseEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_denoiseBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }
    cl4->SetPipelineState(m_tonemapPso.Get());
    constexpr wchar_t kDxrTonemapEvent[] = L"D3D12 DXR Tonemap Dispatch";
    cl4->BeginEvent(0, kDxrTonemapEvent, sizeof(kDxrTonemapEvent));
    cl4->Dispatch(groupsX, groupsY, 1u);
    cl4->EndEvent();

    uavBarrier.UAV.pResource = m_ldrBuf.Get();
    cl4->ResourceBarrier(1, &uavBarrier);

    if (doTemporal) {
      const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
      const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
      std::array<D3D12_RESOURCE_BARRIER, 4> copyStartBarriers = {
          MakeTransitionBarrier(m_temporalBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
          MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
          MakeTransitionBarrier(m_guideBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
          MakeTransitionBarrier(m_prevGuideBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
      cl4->ResourceBarrier(static_cast<UINT>(copyStartBarriers.size()), copyStartBarriers.data());
      cl4->CopyBufferRegion(m_temporalHistoryBuf.Get(), 0, m_temporalBuf.Get(), 0, filmSize);
      cl4->CopyBufferRegion(m_prevGuideBuf.Get(), 0, m_guideBuf.Get(), 0, guideSize);

      std::array<D3D12_RESOURCE_BARRIER, 4> copyEndBarriers = {
          MakeTransitionBarrier(m_temporalBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_guideBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_prevGuideBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
      cl4->ResourceBarrier(static_cast<UINT>(copyEndBarriers.size()), copyEndBarriers.data());
    }

    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ldrBarrier.Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ldrBarrier.Transition.StateAfter   = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (doDenoise || (doGuide && !doTemporal)) {
      std::array<D3D12_RESOURCE_BARRIER, 4> readbackBarriers{};
      UINT barrierCount = 0u;
      readbackBarriers[barrierCount++] = filmBarrier;
      readbackBarriers[barrierCount++] = ldrBarrier;
      denoiseBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      denoiseBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      guideBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      guideBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      if (doDenoise) readbackBarriers[barrierCount++] = denoiseBarrier;
      if (doGuide && !doTemporal) readbackBarriers[barrierCount++] = guideBarrier;
      cl4->ResourceBarrier(barrierCount, readbackBarriers.data());
    } else {
      D3D12_RESOURCE_BARRIER readbackBarriers[2] = {filmBarrier, ldrBarrier};
      cl4->ResourceBarrier(2, readbackBarriers);
    }

    const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
    cl4->CopyBufferRegion(m_ldrReadbackBuf.Get(), 0, m_ldrBuf.Get(), 0, ldrSize);

    ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl4->ResourceBarrier(1, &ldrBarrier);
  } else {
    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl4->ResourceBarrier(1, &filmBarrier);
  }

  cl4->Close();
  ID3D12CommandList* lists[] = {cl4.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Drain any D3D12 InfoQueue messages only during verbose diagnostics.
  if (D3D12VerboseLoggingEnabled()) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> iq;
    if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&iq)))) {
      static bool s_iqLogged = false;
      if (!s_iqLogged) {
        s_iqLogged = true;
        const UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
          SIZE_T len = 0;
          iq->GetMessage(i, nullptr, &len);
          std::vector<char> buf(len);
          auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
          if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
            std::string sev = (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)   ? "ERROR"   :
                              (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ? "WARNING" : "INFO";
            LogInfo("[D3D12] " + sev + ": " + std::string(msg->pDescription, msg->DescriptionByteLength));
          }
        }
        if (n == 0) LogInfo("[D3D12 InfoQueue] No messages stored");
      }
    }
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR dispatch hr=" + FormatHr(removeHr);
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doTemporal) {
    m_temporalHistoryValid = true;
    m_temporalPrevCamera = MakeTemporalCameraState(pc);
  }

  if (doReadback) {
    m_ldrResolve.width  = m_settings.width;
    m_ldrResolve.height = m_settings.height;
    m_ldrResolve.rgba8.resize(static_cast<size_t>(m_filmPixels) * 4u);
    std::memcpy(m_ldrResolve.rgba8.data(), m_ldrReadbackPtr,
                static_cast<size_t>(m_filmPixels) * 4u);
  }
  const uint64_t rpp = static_cast<uint64_t>(std::max(1u, m_raysPerPixelPerDispatch));
  const uint64_t inc = static_cast<uint64_t>(m_settings.width) * m_settings.height * rpp;
  const uint64_t raysPerSample =
      EstimateLogicalRaysPerD3D12Sample(m_settings, m_sceneData, m_usingDxrDispatch);
  m_counters.samples += inc;
  m_counters.rays    += SaturatingMulU64(inc, raysPerSample);
  m_lastSampleIdx     = sample_idx * m_raysPerPixelPerDispatch + (m_raysPerPixelPerDispatch - 1u);
  return true;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
