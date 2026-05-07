#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracerInternal.h"
#include "core/Logging.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <vector>
#include <wincodec.h>

namespace vkpt::gpu {

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
  std::string out(static_cast<size_t>(len), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), len, nullptr, nullptr);
  if (written <= 0) return {};
  out.resize(static_cast<size_t>(written));
  if (!out.empty() && out.back() == '\0') {
    out.pop_back();
  }
  return out;
}

std::string FormatHr(HRESULT hr) {
  std::ostringstream ss;
  ss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
     << static_cast<uint32_t>(hr);
  return ss.str();
}

std::string LowercaseExtension(const std::filesystem::path& path) {
  std::string ext = path.extension().generic_string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

std::filesystem::path ResolveTexturePath(std::string_view uri) {
  std::filesystem::path path{std::string(uri)};
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && !ec) {
    return path.lexically_normal();
  }
  const auto cwdPath = (std::filesystem::current_path() / path).lexically_normal();
  ec.clear();
  if (std::filesystem::exists(cwdPath, ec) && !ec) {
    return cwdPath;
  }
  const auto sceneRelative = (std::filesystem::current_path() / "assets" / "scenes" / path).lexically_normal();
  ec.clear();
  if (std::filesystem::exists(sceneRelative, ec) && !ec) {
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
    const std::size_t required = static_cast<std::size_t>(pixelCount) * bytesPerPixel;
    if (required > bytes.size() - offset) {
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
        if (offset > bytes.size() || bytesPerPixel > bytes.size() - offset) {
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
          if (offset > bytes.size() || bytesPerPixel > bytes.size() - offset) {
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
  struct ComApartmentScope {
    explicit ComApartmentScope(HRESULT result_in)
        : result(result_in),
          needs_uninitialize(result_in == S_OK || result_in == S_FALSE) {}
    ~ComApartmentScope() {
      if (needs_uninitialize) {
        CoUninitialize();
      }
    }
    HRESULT result = E_FAIL;
    bool needs_uninitialize = false;
  };
  ComApartmentScope comScope(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  if (FAILED(comScope.result) && comScope.result != RPC_E_CHANGED_MODE) {
    if (error) *error = "COM init failed hr=" + FormatHr(comScope.result);
    return false;
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
    return "fast_build";
  }
  if (valueText == "fast_trace" || valueText == "fast_build" || valueText == "none") {
    return valueText;
  }
  LogError("ignoring invalid PT_D3D12_DXR_BUILD_MODE=" + valueText +
           " (expected fast_trace, fast_build, or none)");
  return "fast_build";
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
  // preview uses an unlimited spp sentinel and prioritizes frame cadence over
  // convergence. Extra rays can still be requested with PT_D3D12_RAYS_PER_PIXEL.
  const bool interactivePreview = settings.spp == std::numeric_limits<uint32_t>::max();
  const uint32_t fallback = interactivePreview ? 1u : 1u;
  return ParseEnvU32("PT_D3D12_RAYS_PER_PIXEL", fallback, 1u, 64u);
}

uint32_t SelectReadbackInterval(const vkpt::pathtracer::RenderSettings& settings) {
  const bool interactivePreview = settings.spp == std::numeric_limits<uint32_t>::max();
  const uint32_t fallback = interactivePreview ? 1u : 1u;
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
  (void)usingDxrDispatch;
  const uint64_t maxDepth = static_cast<uint64_t>(std::max(1u, settings.max_depth));
  uint64_t raysPerSample = maxDepth;

  // The GPU shaders do not currently write exact per-ray counters. Count the
  // primary/continuation scene query at each path depth, plus the direct-light
  // shadow query issued in the compute shader and in DXR shader code.
  const bool hasDirectLight = !scene.lights.empty();
  const bool shadowQueriesEnabled = hasDirectLight;
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

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
