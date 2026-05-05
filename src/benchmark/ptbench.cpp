#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "build_info.generated.h"
#include "diagnostics/CrashRecorder.h"
#include "cpu/CpuFeatures.h"
#include "cpu/PacketRay.h"
#include "cpu/ParallelBvhBuilder.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"
#include "cpu/SimdKernelNeon.h"
#include "cpu/SimdKernelSve.h"
#include "cpu/SimdKernelAvx2.h"
#include "cpu/SimdKernelAvx512.h"
#include "cpu/TiledCpuPathTracer.h"
#include "jobs/JobSystem.h"
#include "materials/MaterialDescriptors.h"
#include "pathtracer/PathTracer.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/VulkanBackend.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"

#if defined(PT_ENABLE_VULKAN)
#include "gpu/VulkanGpuPathTracer.h"
#endif

#if defined(PT_ENABLE_D3D12)
#include "gpu/D3D12GpuPathTracer.h"
#endif

#if !defined(PT_SHADER_SPV_PATH)
#define PT_SHADER_SPV_PATH ""
#endif

#if !defined(PT_SHADER_HLSL_PATH)
#define PT_SHADER_HLSL_PATH ""
#endif

namespace {

using Path = std::filesystem::path;

class TraceProfiler final : public vkpt::benchmark::IProfiler {
 public:
  vkpt::benchmark::ProfilerEventHandle begin_event(vkpt::benchmark::ProfilerEventKind kind,
                                                   std::string_view name,
                                                   std::string_view category,
                                                   uint32_t thread_id) override {
    ActiveEvent active;
    active.handle = m_nextHandle++;
    active.event.kind = kind;
    active.event.name = std::string(name);
    active.event.category = std::string(category);
    active.event.thread_id = thread_id;
    active.event.start_ms = elapsed_ms();
    m_active.push_back(std::move(active));
    return m_active.back().handle;
  }

  void end_event(vkpt::benchmark::ProfilerEventHandle handle) override {
    const auto now = elapsed_ms();
    const auto it = std::find_if(m_active.begin(), m_active.end(), [&](const ActiveEvent& active) {
      return active.handle == handle;
    });
    if (it == m_active.end()) {
      return;
    }
    auto event = it->event;
    event.duration_ms = std::max(0.0, now - event.start_ms);
    m_events.push_back(std::move(event));
    m_active.erase(it);
  }

  std::string emit_trace() const override {
    return vkpt::benchmark::SerializeProfilerTrace(m_events);
  }

  void reset_frame() override {
    m_active.clear();
    m_events.clear();
    m_origin = std::chrono::steady_clock::now();
  }

  vkpt::benchmark::ProfilerCapabilities describe_capabilities() const override {
    return vkpt::benchmark::DefaultProfilerCapabilities();
  }

  const std::vector<vkpt::benchmark::ProfilerEvent>& events() const { return m_events; }

 private:
  struct ActiveEvent {
    vkpt::benchmark::ProfilerEventHandle handle = 0;
    vkpt::benchmark::ProfilerEvent event;
  };

  double elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_origin).count();
  }

  vkpt::benchmark::ProfilerEventHandle m_nextHandle = 1;
  std::chrono::steady_clock::time_point m_origin = std::chrono::steady_clock::now();
  std::vector<ActiveEvent> m_active;
  std::vector<vkpt::benchmark::ProfilerEvent> m_events;
};

std::string_view ToLower(std::string_view text) {
  static std::array<std::string, 16> store;
  static std::size_t index = 0;
  auto& out = store[index++ % store.size()];
  out.clear();
  out.reserve(text.size());
  for (const auto ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

bool ParseUnsigned(std::string_view text, std::uint32_t& out) {
  try {
    out = static_cast<std::uint32_t>(std::stoul(std::string(text)));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseUnsigned64(std::string_view text, std::uint64_t& out) {
  try {
    out = std::stoull(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseResolution(std::string_view text, std::uint32_t& width, std::uint32_t& height) {
  const auto pos = text.find('x');
  if (pos == std::string_view::npos) {
    return false;
  }
  const auto w = text.substr(0, pos);
  const auto h = text.substr(pos + 1);
  if (w.empty() || h.empty()) {
    return false;
  }
  return ParseUnsigned(w, width) && ParseUnsigned(h, height);
}

std::string NowUtcString() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000u;

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif

  std::ostringstream out;
  out << std::put_time(&tm, "%Y%m%dT%H%M%S");
  out << 'Z';
  out << '.' << std::setfill('0') << std::setw(3) << ms;
  return out.str();
}

std::string RunIdFromNow() {
  static std::uint64_t counter = 0u;
  auto ts = NowUtcString();
  auto value = (std::uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return ts + "-" + std::to_string(value + ++counter);
}

bool WriteFile(const Path& path, std::string_view text, std::string* error = nullptr) {
  std::ofstream output(path);
  if (!output.is_open()) {
    if (error) {
      *error = "failed to open output path: " + path.string();
    }
    return false;
  }
  output << text;
  output.flush();
  if (!output.good()) {
    if (error) {
      *error = "failed to write output path: " + path.string();
    }
    return false;
  }
  return true;
}

bool EnsureDirectory(const Path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

struct TolerancePolicy {
  double abs = 0.0;
  double rel = 0.0;
};

bool ParseTolerance(std::string_view text, TolerancePolicy& policy, std::string* error = nullptr) {
  if (text.empty()) {
    policy.abs = 0.001;
    return true;
  }

  policy = {};
  auto cursor = std::string{text};
  auto start = std::size_t{0};
  bool gotAny = false;

  const auto trim = [](std::string_view value) {
    auto left = value.find_first_not_of(" \t");
    auto right = value.find_last_not_of(" \t");
    if (left == std::string_view::npos || right == std::string_view::npos) {
      return std::string_view{};
    }
    return value.substr(left, right - left + 1);
  };

  while (start < cursor.size()) {
    auto comma = cursor.find(',', start);
    const auto item = trim(std::string_view{cursor.data() + start, comma == std::string::npos ? cursor.size() - start
                                                                                : comma - start});
    if (!item.empty()) {
      auto eq = item.find('=');
      if (eq == std::string_view::npos) {
        if (error) {
          *error = "invalid tolerance policy token";
        }
        return false;
      }
      const std::string_view key = item.substr(0, eq);
      const std::string_view value = item.substr(eq + 1);
      if (value.empty()) {
        if (error) {
          *error = "invalid tolerance policy value";
        }
        return false;
      }
      try {
        const double v = std::stod(std::string(value));
        if (key == "abs") {
          policy.abs = v;
          gotAny = true;
        } else if (key == "rel") {
          policy.rel = v;
          gotAny = true;
        } else if (error) {
          *error = "unsupported tolerance key: " + std::string(key);
          return false;
        }
      } catch (...) {
        if (error) {
          *error = "invalid tolerance number";
        }
        return false;
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }

  if (!gotAny && error) {
    *error = "invalid tolerance policy";
    return false;
  }
  if (!gotAny) {
    policy.abs = 0.001;
  }
  return true;
}

std::string NormalizeBackend(std::string_view backend) {
  const auto normalized = std::string(ToLower(backend));
  if (normalized == "vulkancompute") {
    return "vulkan";
  }
  if (normalized == "cuda" || normalized == "gpu") {
    return "vulkan";
  }
  if (normalized == "dxr") {
    return "d3d12-dxr";
  }
  return normalized.empty() ? "cpu" : normalized;
}

std::vector<std::string> AvailableRendererPaths(std::string_view backend) {
  const auto normalized = NormalizeBackend(backend);
  if (normalized == "vulkan" || normalized == "vulkan-compute") {
    return {"gpu-compute"};
  }
  if (normalized == "d3d12") {
    return {"d3d12-compute", "dxr"};
  }
  if (normalized == "d3d12-dxr") {
    return {"dxr", "d3d12-dxr"};
  }
  if (normalized == "auto" || normalized == "null" || normalized == "cpu") {
    return {"cpu-scalar", "cpu-tiled"};
  }
  return {};
}

bool ValidateBackendRenderer(std::string_view backend, std::string_view rendererPath, std::string* error = nullptr) {
  const auto normalizedBackend = NormalizeBackend(backend);
  const auto normalizedRenderer = std::string(ToLower(rendererPath));
  const auto rendererPaths = AvailableRendererPaths(normalizedBackend);
  if (rendererPaths.empty()) {
    if (error) {
      *error = "unsupported backend: " + std::string(backend);
    }
    return false;
  }
  for (const auto& allowed : rendererPaths) {
    if (allowed == normalizedRenderer) {
      return true;
    }
  }
  if (error) {
    *error = "backend '" + std::string(backend) + "' does not support renderer path '" + std::string(rendererPath) + "'";
  }
  return false;
}

struct SceneComparison {
  double mean_abs_error = 0.0;
  double max_error = 0.0;
  double rmse = 0.0;
  std::size_t nan_inf_count = 0;
  std::vector<float> diff;
};

struct ImageRgb {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> rgb;
};

uint16_t ReadU16LE(const std::vector<std::uint8_t>& data, std::size_t pos) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[pos + 1]) << 8 | static_cast<uint16_t>(data[pos]));
}

uint32_t ReadU32BE(const std::vector<std::uint8_t>& data, std::size_t pos) {
  return (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
         (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
}

bool InflateStoredDeflate(const std::vector<std::uint8_t>& compressed, std::vector<std::uint8_t>& raw, std::string* error = nullptr) {
  if (compressed.size() < 2 || compressed[0] != 0x78 || compressed[1] != 0x01) {
    if (error) {
      *error = "invalid zlib header";
    }
    return false;
  }

  std::size_t pos = 2;
  while (pos < compressed.size()) {
    const std::uint8_t flags = compressed[pos++];
    const bool finalBlock = (flags & 1u) != 0u;
    const std::uint8_t type = static_cast<std::uint8_t>((flags >> 1) & 3u);
    if (type != 0u) {
      if (error) {
        *error = "unsupported deflate block type";
      }
      return false;
    }
    if (pos + 4 > compressed.size()) {
      if (error) {
        *error = "truncated stored block";
      }
      return false;
    }
    const std::uint16_t len = ReadU16LE(compressed, pos);
    const std::uint16_t nlen = ReadU16LE(compressed, pos + 2);
    if (static_cast<std::uint16_t>(~len) != nlen) {
      if (error) {
        *error = "stored block checksum mismatch";
      }
      return false;
    }
    pos += 4;
    if (pos + len > compressed.size()) {
      if (error) {
        *error = "stored block data truncated";
      }
      return false;
    }
    raw.insert(raw.end(), compressed.begin() + static_cast<std::ptrdiff_t>(pos),
               compressed.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
    if (finalBlock) {
      break;
    }
  }
  return true;
}

bool LoadPng(const Path& path, ImageRgb& image, std::string* error = nullptr) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (error) {
      *error = "cannot open image: " + path.string();
    }
    return false;
  }
  std::vector<std::uint8_t> bytes(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.size() < 8 || bytes[0] != 0x89 || bytes[1] != 0x50 || bytes[2] != 0x4e || bytes[3] != 0x47 ||
      bytes[4] != 0x0d || bytes[5] != 0x0a || bytes[6] != 0x1a || bytes[7] != 0x0a) {
    if (error) {
      *error = "invalid png signature";
    }
    return false;
  }

  std::size_t pos = 8;
  std::vector<std::uint8_t> idat;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  while (pos + 8 <= bytes.size()) {
    const std::uint32_t length = ReadU32BE(bytes, pos);
    pos += 4;
    if (pos + 4 > bytes.size()) {
      if (error) *error = "invalid chunk header";
      return false;
    }
    const std::string type{
        static_cast<char>(bytes[pos]), static_cast<char>(bytes[pos + 1]), static_cast<char>(bytes[pos + 2]),
        static_cast<char>(bytes[pos + 3])};
    pos += 4;
    if (pos + length + 4 > bytes.size()) {
      if (error) {
        *error = "invalid chunk size";
      }
      return false;
    }
    if (type == "IHDR") {
      if (length < 8) {
        if (error) {
          *error = "invalid IHDR length";
        }
        return false;
      }
      width = ReadU32BE(bytes, pos);
      height = ReadU32BE(bytes, pos + 4);
    } else if (type == "IDAT") {
      idat.insert(idat.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                  bytes.begin() + static_cast<std::ptrdiff_t>(pos + length));
    } else if (type == "IEND") {
      break;
    }
    pos += length + 4;  // data + crc
  }

  if (width == 0 || height == 0 || idat.empty()) {
    if (error) {
      *error = "missing png image data";
    }
    return false;
  }

  std::vector<std::uint8_t> raw;
  if (!InflateStoredDeflate(idat, raw, error)) {
    return false;
  }

  const std::size_t rowBytes = static_cast<std::size_t>(4u) * width;
  const std::size_t expected = (static_cast<std::size_t>(1u) + rowBytes) * height;
  if (raw.size() < expected) {
    if (error) {
      *error = "invalid decompressed png size";
    }
    return false;
  }

  image.width = width;
  image.height = height;
  image.rgb.resize(static_cast<std::size_t>(width) * height * 3u);
  std::size_t source = 0;
  std::size_t target = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::size_t filter = raw[source++];
    if (filter != 0u) {
      if (error) {
        *error = "png filter unsupported";
      }
      return false;
    }
    if (source + rowBytes > raw.size()) {
      if (error) {
        *error = "invalid decompressed png row size";
      }
      return false;
    }
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t px = static_cast<std::size_t>(x) * 4u;
      image.rgb[target++] = static_cast<float>(raw[source + px + 0u]) / 255.0f;
      image.rgb[target++] = static_cast<float>(raw[source + px + 1u]) / 255.0f;
      image.rgb[target++] = static_cast<float>(raw[source + px + 2u]) / 255.0f;
    }
    source += rowBytes;
  }
  return true;
}

bool LoadExrPlaceholder(const Path& path, ImageRgb& image, std::string* error = nullptr) {
  std::ifstream input(path);
  if (!input.is_open()) {
    if (error) {
      *error = "cannot open image: " + path.string();
    }
    return false;
  }

  std::string first;
  if (!std::getline(input, first)) {
    if (error) {
      *error = "empty exr placeholder";
    }
    return false;
  }
  std::string second;
  if (!std::getline(input, second)) {
    if (error) {
      *error = "missing exr dimensions";
    }
    return false;
  }
  {
    std::istringstream header(second);
    if (!(header >> image.width >> image.height)) {
      if (error) {
        *error = "invalid exr dimension header";
      }
      return false;
    }
  }
  if (image.width == 0 || image.height == 0) {
    if (error) {
      *error = "invalid exr dimensions";
    }
    return false;
  }
  image.rgb.clear();
  image.rgb.reserve(static_cast<std::size_t>(image.width) * image.height * 3u);
  for (float value = 0.0f; input >> value;) {
    image.rgb.push_back(value);
    float value2 = 0.0f;
    float value3 = 0.0f;
    if (!(input >> value2 >> value3)) {
      if (error) {
        *error = "insufficient exr pixels";
      }
      return false;
    }
    image.rgb.push_back(value2);
    image.rgb.push_back(value3);
  }
  const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * 3u;
  if (image.rgb.size() != expected) {
    if (error) {
      *error = "invalid exr pixel count";
    }
    return false;
  }
  return true;
}

bool LoadImage(const Path& path, ImageRgb& image, std::string* error = nullptr) {
  const std::string ext = path.extension().string();
  if (ext == ".exr" || ext == ".txt") {
    return LoadExrPlaceholder(path, image, error);
  }
  if (ext == ".png") {
    return LoadPng(path, image, error);
  }
  if (error) {
    *error = "unsupported image format: " + ext;
  }
  return false;
}

std::uint64_t Fnv1a64(std::string_view text) {
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= kPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value);

std::string HashBytes(const void* data, std::size_t byteCount) {
  if (byteCount == 0) {
    return Hex64(Fnv1a64(""));
  }
  const auto bytes = static_cast<const unsigned char*>(data);
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (std::size_t i = 0; i < byteCount; ++i) {
    hash ^= bytes[i];
    hash *= kPrime;
  }
  return Hex64(hash);
}

std::string HashText(std::string_view text) {
  return HashBytes(text.data(), text.size());
}

std::string Hex64(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 0; i < 16; ++i) {
    out[15 - i] = kHex[(value >> (i * 4)) & 0x0f];
  }
  return out;
}

std::string SerializeManifest(const std::vector<std::string>& names) {
  std::ostringstream out;
  out << "{";
  out << "\"assets\":[";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out << ",";
    out << "{\"uri\":\"" << EscapeJson(names[i]) << "\"}";
  }
  out << "]";
  out << "}";
  return out.str();
}

std::string SerializeSceneSnapshot(const vkpt::scene::SceneSnapshot& snapshot) {
  std::ostringstream out;
  out << "{";
  out << "\"asset_refs\":[";
  for (std::size_t i = 0; i < snapshot.asset_refs.size(); ++i) {
    if (i) out << ",";
    out << "\"" << EscapeJson(snapshot.asset_refs[i]) << "\"";
  }
  out << "],\"entity_ids\":[";
  for (std::size_t i = 0; i < snapshot.entity_ids.size(); ++i) {
    if (i) out << ",";
    out << snapshot.entity_ids[i];
  }
  out << "],\"renderables\":[";
  for (std::size_t i = 0; i < snapshot.renderables.size(); ++i) {
    const auto& renderable = snapshot.renderables[i];
    if (i) out << ",";
    out << "{";
    out << "\"entity_id\":" << renderable.entity_id << ",";
    out << "\"mesh_id\":" << renderable.mesh_id << ",";
    out << "\"material_id\":" << renderable.material_id << ",";
    out << "\"transform\":[";
    out << renderable.transform.translation.x << ",";
    out << renderable.transform.translation.y << ",";
    out << renderable.transform.translation.z << ",";
    out << renderable.transform.rotation.x << ",";
    out << renderable.transform.rotation.y << ",";
    out << renderable.transform.rotation.z << ",";
    out << renderable.transform.rotation.w << ",";
    out << renderable.transform.scale.x << ",";
    out << renderable.transform.scale.y << ",";
    out << renderable.transform.scale.z << "]";
    out << "}";
  }
  out << "],\"lights\":[";
  for (std::size_t i = 0; i < snapshot.lights.size(); ++i) {
    const auto& light = snapshot.lights[i];
    if (i) out << ",";
    out << "{";
    out << "\"entity_id\":" << light.entity_id << ",";
    out << "\"type\":\"" << EscapeJson(light.light.type) << "\",";
    out << "\"color\":[" << light.light.color.x << "," << light.light.color.y << "," << light.light.color.z << "],";
    out << "\"intensity\":" << light.light.intensity << ",";
    out << "\"radius\":" << light.light.radius << "}";
  }
  out << "],\"materials\":[";
  for (std::size_t i = 0; i < snapshot.materials.size(); ++i) {
    const auto& mat = snapshot.materials[i];
    if (i) out << ",";
    out << "{";
    out << "\"id\":" << mat.id << ",";
    out << "\"name\":\"" << EscapeJson(mat.material.name) << "\",";
    out << "\"albedo\":[" << mat.material.albedo.x << "," << mat.material.albedo.y << "," << mat.material.albedo.z << "],";
    out << "\"roughness\":" << mat.material.roughness << ",";
    out << "\"emission\":[" << mat.material.emission.x << "," << mat.material.emission.y << "," << mat.material.emission.z << "],";
    out << "\"emission_intensity\":" << mat.material.emission_intensity << "}";
  }
  out << "],\"benchmark_enabled\":" << (snapshot.benchmark.enabled ? "true" : "false") << ",";
  out << "\"frame_target\":" << snapshot.benchmark.frame_target << ",";
  out << "\"warmup_frames\":" << snapshot.benchmark.warmup_frames << ",";
  out << "\"scene_hash\":\"" << Hex64(0) << "\"";
  out << "}";
  return out.str();
}

std::string SerializeMetadata(const vkpt::benchmark::BenchmarkResult& result,
                             const vkpt::scene::SceneDocument& scene,
                             const std::string& rendererPath) {
  std::ostringstream out;
  out << "{";
  out << "\"run_id\":\"" << EscapeJson(result.run_id) << "\",";
  out << "\"command\":\"ptbench run\",";
  out << "\"scene_path\":\"" << EscapeJson(result.scene) << "\",";
  out << "\"backend\":\"" << EscapeJson(result.backend) << "\",";
  out << "\"renderer_path\":\"" << EscapeJson(rendererPath) << "\",";
  out << "\"scene_name\":\"" << EscapeJson(scene.metadata.scene_name) << "\",";
  out << "\"schema\":\"" << EscapeJson(scene.metadata.schema) << "\",";
  out << "\"asset_count\":" << scene.assets.size() << ",";
  out << "\"material_count\":" << scene.materials.size() << ",";
  out << "\"geometry_count\":" << scene.geometry.size() << ",";
  out << "\"sdf_count\":" << scene.sdf_primitives.size() << ",";
  out << "\"entity_count\":" << scene.entities.size() << ",";
  out << "\"benchmark_enabled\":" << (scene.benchmark.enabled ? "true" : "false") << ",";
  out << "\"benchmark_capabilities\":" << vkpt::benchmark::SerializeBenchmarkCapabilities(vkpt::benchmark::DefaultBenchmarkCapabilities()) << ",";
  out << "\"profiler_capabilities\":" << vkpt::benchmark::SerializeProfilerCapabilities(vkpt::benchmark::DefaultProfilerCapabilities());
  out << "}";
  return out.str();
}

void PrintHelp() {
  std::cout << "ptbench <command> [options]\n\n";
  std::cout << "commands:\n";
  std::cout << "  run\n";
  std::cout << "  echo-desc\n";
  std::cout << "  list-scenes\n";
  std::cout << "  list-backends\n";
  std::cout << "  list-renderer-paths\n";
  std::cout << "  validate-scene\n";
  std::cout << "  validate-artifacts\n";
  std::cout << "  compare\n";
  std::cout << "  dump-capabilities\n";
  std::cout << "  run-experiments\n";
  std::cout << "  backend-experiments\n";
  std::cout << "  gpu-mem-pressure\n";
  std::cout << "  material-coverage\n";
  std::cout << "  shader-matrix\n";
  std::cout << "  release-check\n";
  std::cout << "  thread-sweep\n";
  std::cout << "  simd-sweep\n";
  std::cout << "  tile-sweep\n\n";
  std::cout << "run:\n";
  std::cout << "  --desc <benchmark-run.json>\n";
  std::cout << "  --echo-desc              (parse and echo resolved descriptor, no render)\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  --backend <cpu|vulkan|d3d12|d3d12-dxr|auto>\n";
  std::cout << "  --renderer-path <cpu-scalar|cpu-tiled|gpu-compute|d3d12-compute|dxr>\n";
  std::cout << "  --resolution <WxH>\n";
  std::cout << "  --spp <samples>\n";
  std::cout << "  --seed <value>\n";
  std::cout << "  --max-depth <value>\n";
  std::cout << "  --duration <seconds>\n";
  std::cout << "  --warmup-frames <count>\n";
  std::cout << "  --reference-image <path>\n";
  std::cout << "  --tolerance-policy <policy> (e.g. abs=0.001,rel=0.01)\n";
  std::cout << "  --output <artifact-dir>\n";
  std::cout << "  --workers <count>        (cpu-tiled: thread count, 0=auto)\n";
  std::cout << "  --tile-size <rows>       (cpu-tiled: rows per tile, default 16)\n";
  std::cout << "  --deterministic          (cpu-tiled: serialized execution)\n\n";
  std::cout << "echo-desc:\n";
  std::cout << "  --desc <benchmark-run.json>\n\n";
  std::cout << "validate-artifacts:\n";
  std::cout << "  --dir <artifact-dir>\n";
  std::cout << "  [--json]\n\n";
  std::cout << "thread-sweep:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--workers <list>]       (comma list, default available 1/2/4/8)\n";
  std::cout << "  [--spp <n>]              (samples per pixel, default 2)\n";
  std::cout << "  [--resolution <WxH>]     (default 128x128)\n";
  std::cout << "  [--output <path>]        (write thread_sweep.json to dir)\n\n";
  std::cout << "simd-sweep:\n";
  std::cout << "  [--rays <count>]         (rays per triangle test, default 1000000)\n";
  std::cout << "  [--triangles <count>]    (triangle count, default 1024)\n";
  std::cout << "  [--output <path>]        (write simd_sweep.json to dir)\n\n";
  std::cout << "tile-sweep:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--workers <count>]      (thread count, 0=auto)\n";
  std::cout << "  [--spp <n>]              (samples per pixel, default 4)\n";
  std::cout << "  [--resolution <WxH>]     (default 128x128)\n";
  std::cout << "  [--output <path>]        (write tile_sweep.json to dir)\n\n";
  std::cout << "validate-scene:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--backend <cpu|vulkan|auto>]\n";
  std::cout << "  [--renderer-path <cpu-scalar|gpu-compute>]\n";
  std::cout << "  [--json]\n\n";
  std::cout << "compare:\n";
  std::cout << "  --reference <path>\n";
  std::cout << "  --image <path>\n";
  std::cout << "  --output <path>\n";
  std::cout << "  [--tolerance-policy <policy>]\n";
  std::cout << "  [--disable-heatmap]\n";

  std::cout << "\ngpu-mem-pressure:\n";
  std::cout << "  [--max-mb <n>]          (default 512)\n";
  std::cout << "  [--step-mb <n>]         (default 64)\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "material-coverage:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments/material_coverage.json)\n";
  std::cout << "  [--json]\n\n";

  std::cout << "shader-matrix:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "backend-experiments:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "release-check:\n";
  std::cout << "  [--scene-pack <dir>]    (default assets/scenes)\n";
  std::cout << "  [--output <path>]       (default artifacts/release_check)\n";
}

bool WriteRunArtifacts(const vkpt::benchmark::BenchmarkResult& result,
                      const Path& artifactDir,
                      const vkpt::scene::SceneDocument& scene,
                      const vkpt::pathtracer::RTSceneLayoutManifest& manifest,
                      const Path& referencePath,
                      const Path& diffHeatmapPath,
                      const bool includeReference,
                      std::string* error) {
  (void)diffHeatmapPath;
  const Path resultsPath = artifactDir / "results.json";
  const Path resultsCsvPath = artifactDir / "results.csv";
  const Path metadataPath = artifactDir / "metadata.json";
  const Path snapshotPath = artifactDir / "scene_snapshot.json";
  const Path shaderManifestPath = artifactDir / "shader_manifest.json";
  const Path assetManifestPath = artifactDir / "asset_manifest.json";
  const Path logsPath = artifactDir / "logs.jsonl";
  const Path profilerTracePath = artifactDir / "profiler_trace.json";
  const Path required[] = {resultsPath, resultsCsvPath, metadataPath, snapshotPath, shaderManifestPath,
                           assetManifestPath, result.beauty_png, result.beauty_exr, logsPath, profilerTracePath};
  if (!WriteFile(resultsPath, vkpt::benchmark::SerializeBenchmarkResult(result), error)) {
    return false;
  }
  std::ofstream csv(resultsCsvPath);
  if (!csv.is_open()) {
    if (error) {
      *error = "failed to write results.csv";
    }
    return false;
  }
  csv << "run_id,scene,backend,renderer_path,resolution_width,resolution_height,spp,max_depth,total_ms,build_ms,render_ms,"
         "cpu_ms,paths_per_sec,samples_per_sec,samples_per_sec_per_thread,paths_per_sec_per_thread,"
         "samples_per_sec_per_megapixel,normalized_score,reference_error,image_hash\n";
  csv << EscapeJson(result.run_id) << "," << EscapeJson(result.scene) << ","
      << EscapeJson(result.backend) << "," << EscapeJson(result.renderer_path) << ","
      << result.resolution.width << "," << result.resolution.height << "," << result.spp << "," << result.max_depth << ","
      << std::fixed << std::setprecision(6) << result.timing.total_ms << "," << result.timing.build_ms << ","
      << result.timing.render_ms << "," << result.timing.cpu_ms << "," << result.throughput.paths_per_sec << ","
      << result.throughput.samples_per_sec << "," << result.score.samples_per_sec_per_thread << ","
      << result.score.paths_per_sec_per_thread << "," << result.score.samples_per_sec_per_megapixel << ","
      << result.score.normalized_score << "," << result.reference_error << ","
      << EscapeJson(result.image_hash) << "\n";
  csv.close();

  if (!WriteFile(metadataPath, SerializeMetadata(result, scene, result.renderer_path), error)) {
    return false;
  }
  if (!WriteFile(snapshotPath, SerializeSceneSnapshot(scene.snapshot()), error)) {
    return false;
  }
  if (!WriteFile(shaderManifestPath, vkpt::pathtracer::SerializeRTSceneDataLayoutManifest(manifest), error)) {
    return false;
  }
  std::vector<std::string> assetRefs;
  assetRefs.reserve(scene.assets.size());
  for (const auto& asset : scene.assets) {
    assetRefs.push_back(asset.uri);
  }
  if (!WriteFile(assetManifestPath, SerializeManifest(assetRefs), error)) {
    return false;
  }

  if (includeReference) {
    std::error_code ec;
    std::filesystem::copy_file(referencePath, artifactDir / "reference.exr", std::filesystem::copy_options::overwrite_existing, ec);
  }

  for (const auto& requiredPath : required) {
    std::error_code ec;
    if (!std::filesystem::exists(requiredPath, ec) || !std::filesystem::is_regular_file(requiredPath, ec)) {
      if (error) {
        *error = "required artifact not created: " + requiredPath.string();
      }
      return false;
    }
    if (std::filesystem::file_size(requiredPath, ec) == 0u && !ec) {
      if (error) {
        *error = "required artifact is empty: " + requiredPath.string();
      }
      return false;
    }
  }

  return true;
}

SceneComparison CompareImages(const ImageRgb& left, const ImageRgb& right, const TolerancePolicy& policy) {
  SceneComparison out{};
  if (left.width != right.width || left.height != right.height || left.width == 0 || left.height == 0) {
    return out;
  }
  const std::size_t count = static_cast<std::size_t>(left.width) * left.height * 3u;
  out.diff.resize(count);
  if (count == 0) {
    return out;
  }
  double sumAbs = 0.0;
  double sumSq = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double a = static_cast<double>(left.rgb[i]);
    const double b = static_cast<double>(right.rgb[i]);
    const double d = a - b;
    const double e = std::abs(d);
    if (!std::isfinite(a) || !std::isfinite(b)) {
      ++out.nan_inf_count;
      out.diff[i] = 0.0f;
      continue;
    }
    out.diff[i] = static_cast<float>(e);
    if (policy.rel > 0.0) {
      const double rel = std::max(std::abs(a), std::abs(b));
      const double adj = policy.rel > 0.0 ? std::max(1.0, rel) : 1.0;
      if (e > policy.rel * adj) {
      }
    }
    sumAbs += e;
    sumSq += e * e;
    if (e > out.max_error) {
      out.max_error = e;
    }
  }
  out.mean_abs_error = sumAbs / static_cast<double>(count);
  out.rmse = std::sqrt(sumSq / static_cast<double>(count));
  return out;
}

bool SaveDiffHeatmap(const Path& path,
                     std::uint32_t width,
                     std::uint32_t height,
                     const std::vector<float>& diffs,
                     std::string* error) {
  if (width == 0 || height == 0 || diffs.empty()) {
    if (error) {
      *error = "cannot generate empty heatmap";
    }
    return false;
  }
  std::vector<float> luma(diffs.size() / 3u);
  for (std::size_t i = 0; i < luma.size(); ++i) {
    luma[i] = (diffs[3u * i + 0u] + diffs[3u * i + 1u] + diffs[3u * i + 2u]) / 3.0f;
  }
  float maxDiff = 0.0f;
  for (float v : luma) {
    if (v > maxDiff) {
      maxDiff = v;
    }
  }
  const float scale = maxDiff > 0.0f ? 1.0f / maxDiff : 0.0f;
  vkpt::pathtracer::FilmLdr heatmap;
  heatmap.width = width;
  heatmap.height = height;
  heatmap.rgba8.assign(static_cast<std::size_t>(width) * height * 4u, 0u);
  for (std::size_t i = 0; i < luma.size(); ++i) {
    const float t = std::min(1.0f, luma[i] * scale);
    const auto r = static_cast<std::uint8_t>(std::clamp(t, 0.0f, 1.0f) * 255.0f);
    const auto g = static_cast<std::uint8_t>(std::clamp(1.0f - t, 0.0f, 1.0f) * 255.0f);
    heatmap.rgba8[i * 4u + 0u] = r;
    heatmap.rgba8[i * 4u + 1u] = g;
    heatmap.rgba8[i * 4u + 2u] = 0u;
    heatmap.rgba8[i * 4u + 3u] = 255u;
  }
  std::string saveError;
  return vkpt::pathtracer::SavePngCompat(path.string(), heatmap, &saveError);
}

struct RunOptions {
  std::string descPath;
  std::string scenePath;
  std::string backend = "cpu";
  std::string rendererPath = "cpu-scalar";
  std::string output = "artifacts/benchmarks/run";
  std::string referenceImage;
  std::string tolerance = "abs=0.001";
  std::uint32_t width = 256;
  std::uint32_t height = 256;
  std::uint32_t spp = 8;
  std::uint64_t seed = 0xBADC0FFEEull;
  std::uint32_t maxDepth = 6;
  double duration = 0.0;
  std::uint32_t warmupFrames = 0;
  std::uint32_t workers = 0;      // 0 = hardware concurrency
  std::uint32_t tileHeight = 16;  // rows per tile for cpu-tiled
  bool deterministic = false;
  bool json = false;
  bool echoDesc = false;
};

void ApplyRunDesc(const vkpt::benchmark::BenchmarkRunDesc& desc, RunOptions& out) {
  out.scenePath = desc.scene_path;
  out.backend = desc.backend;
  out.rendererPath = desc.renderer_path;
  out.width = desc.resolution.width;
  out.height = desc.resolution.height;
  out.spp = desc.samples_per_pixel;
  out.duration = desc.duration;
  out.warmupFrames = desc.warmup_frames;
  out.seed = desc.seed;
  out.output = desc.output_directory;
  out.referenceImage = desc.reference_image;
  out.tolerance = desc.tolerance_policy;
  out.maxDepth = desc.max_depth;
  out.workers = desc.worker_count;
  out.tileHeight = desc.tile_height;
  out.deterministic = desc.deterministic;
}

vkpt::benchmark::BenchmarkRunDesc ToRunDesc(const RunOptions& opts) {
  vkpt::benchmark::BenchmarkRunDesc desc;
  desc.scene_path = opts.scenePath;
  desc.backend = opts.backend;
  desc.renderer_path = opts.rendererPath;
  desc.resolution.width = opts.width;
  desc.resolution.height = opts.height;
  desc.samples_per_pixel = opts.spp;
  desc.duration = opts.duration;
  desc.warmup_frames = opts.warmupFrames;
  desc.seed = opts.seed;
  desc.output_directory = opts.output;
  desc.reference_image = opts.referenceImage;
  desc.tolerance_policy = opts.tolerance;
  desc.max_depth = opts.maxDepth;
  desc.worker_count = opts.workers;
  desc.tile_height = opts.tileHeight;
  desc.deterministic = opts.deterministic;
  return desc;
}

bool ParseRunArgs(const std::vector<std::string_view>& args, RunOptions& out, std::string* error = nullptr) {
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--desc") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --desc value";
        return false;
      }
      out.descPath = std::string(args[++i]);
      const auto desc = vkpt::benchmark::LoadBenchmarkRunDescFromFile(out.descPath);
      if (!desc) {
        if (error) *error = "failed to load benchmark descriptor: " + out.descPath;
        return false;
      }
      ApplyRunDesc(desc.value(), out);
    }
  }

  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--desc") {
      ++i;
    } else if (token == "--echo-desc") {
      out.echoDesc = true;
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --scene value";
        return false;
      }
      out.scenePath = std::string(args[++i]);
    } else if (token == "--backend") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --backend value";
        return false;
      }
      out.backend = std::string(args[++i]);
    } else if (token == "--renderer-path") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --renderer-path value";
        return false;
      }
      out.rendererPath = std::string(args[++i]);
    } else if (token == "--output") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --output value";
        return false;
      }
      out.output = std::string(args[++i]);
    } else if (token == "--reference-image") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --reference-image value";
        return false;
      }
      out.referenceImage = std::string(args[++i]);
    } else if (token == "--tolerance-policy") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --tolerance-policy value";
        return false;
      }
      out.tolerance = std::string(args[++i]);
    } else if (token == "--resolution") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --resolution value";
        return false;
      }
      std::uint32_t w = 0;
      std::uint32_t h = 0;
      if (!ParseResolution(args[++i], w, h) || w == 0 || h == 0) {
        if (error) *error = "invalid --resolution";
        return false;
      }
      out.width = w;
      out.height = h;
    } else if (token == "--spp") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --spp value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.spp) || out.spp == 0) {
        if (error) *error = "invalid --spp";
        return false;
      }
    } else if (token == "--seed") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --seed value";
        return false;
      }
      if (!ParseUnsigned64(args[++i], out.seed)) {
        if (error) *error = "invalid --seed";
        return false;
      }
    } else if (token == "--max-depth") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --max-depth value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.maxDepth) || out.maxDepth == 0) {
        if (error) *error = "invalid --max-depth";
        return false;
      }
    } else if (token == "--duration") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --duration value";
        return false;
      }
      try {
        out.duration = std::stod(std::string(args[++i]));
      } catch (...) {
        if (error) *error = "invalid --duration";
        return false;
      }
    } else if (token == "--warmup-frames") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --warmup-frames value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.warmupFrames)) {
        if (error) *error = "invalid --warmup-frames";
        return false;
      }
    } else if (token == "--json") {
      out.json = true;
    } else if (token == "--workers") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --workers value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.workers)) {
        if (error) *error = "invalid --workers";
        return false;
      }
    } else if (token == "--tile-size") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --tile-size value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.tileHeight) || out.tileHeight == 0) {
        if (error) *error = "invalid --tile-size (must be row count > 0)";
        return false;
      }
    } else if (token == "--deterministic") {
      out.deterministic = true;
    } else {
      if (error) {
        *error = "unknown option: " + std::string(token);
      }
      return false;
    }
  }
  return true;
}

int RunCommand(const std::vector<std::string_view>& args) {
  RunOptions opts;
  std::string parseError;
  if (!ParseRunArgs(args, opts, &parseError)) {
    std::cerr << parseError << "\n";
    PrintHelp();
    return 1;
  }
  if (opts.echoDesc) {
    auto desc = ToRunDesc(opts);
    std::string issue;
    if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
      std::cerr << "invalid benchmark descriptor: " << issue << "\n";
      return 1;
    }
    std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(desc) << "\n";
    return 0;
  }
  if (opts.scenePath.empty()) {
    std::cerr << "missing --scene\n";
    return 1;
  }

  std::string compatError;
  if (!ValidateBackendRenderer(opts.backend, opts.rendererPath, &compatError)) {
    std::cerr << compatError << "\n";
    return 1;
  }
  const std::string normalizedBackend = NormalizeBackend(opts.backend);
  const std::string normalizedRenderer = std::string(ToLower(opts.rendererPath));
  const bool isVulkanPath = (normalizedBackend == "vulkan" && normalizedRenderer == "gpu-compute");
  const bool isD3D12ComputePath =
      (normalizedBackend == "d3d12" && normalizedRenderer == "d3d12-compute");
  const bool isD3D12DxrPath =
      ((normalizedBackend == "d3d12" || normalizedBackend == "d3d12-dxr") &&
       (normalizedRenderer == "dxr" || normalizedRenderer == "d3d12-dxr"));
  const bool isTiledPath = (std::string(ToLower(opts.rendererPath)) == "cpu-tiled");
  std::unique_ptr<vkpt::render::IRenderBackend> backend;
  if (isVulkanPath) {
    backend = vkpt::render::CreateBackend(normalizedBackend);
    if (!backend) {
      std::cerr << "failed to create backend: " << normalizedBackend << "\n";
      return 2;
    }
    if (!backend->initialize()) {
      std::cerr << "failed to initialize backend: " << normalizedBackend << "\n";
      return 2;
    }
    if (!vkpt::render::RunVulkanComputeSmoke(*backend)) {
      std::cerr << "vulkan path smoke test failed\n";
      return 2;
    }

    // Gate 10 (C20): record renderer crash-state snapshot for crash artifacts.
    const auto crashState = vkpt::render::BuildRendererCrashState(*backend, 0u, "ptbench.run:start");
    vkpt::diagnostics::CrashRecorder::instance().update_renderer_state_json(
        vkpt::render::SerializeRenderCrashState(crashState));
  }

  TolerancePolicy tolerance;
  if (!ParseTolerance(opts.tolerance, tolerance, &parseError)) {
    std::cerr << "invalid tolerance policy: " << parseError << "\n";
    return 1;
  }

  const auto sceneResult = vkpt::scene::SceneDocument::load_from_file(opts.scenePath);
  if (!sceneResult) {
    std::cerr << "failed to load scene: " << opts.scenePath << "\n";
    return 2;
  }
  auto scene = sceneResult.value();
  std::vector<std::string> issues;
  if (!scene.validate(&issues)) {
    std::cerr << "scene validation failed:\n";
    for (const auto& issue : issues) {
      std::cerr << " - " << issue << "\n";
    }
    return 2;
  }

  const Path artifactDir(opts.output);
  if (!EnsureDirectory(artifactDir)) {
    std::cerr << "cannot create artifact directory: " << artifactDir.string() << "\n";
    return 2;
  }

  const auto runId = RunIdFromNow();
  std::string runError;
  vkpt::benchmark::BenchmarkResult result;
  result.run_id = runId;
  result.scene = opts.scenePath;
  result.backend = normalizedBackend;
  result.renderer_path = opts.rendererPath;
  result.cpu_simd_mode = isVulkanPath ? "simulated-vulkan" : "scalar";
  result.tolerance_policy = opts.tolerance;
  result.resolution.width = opts.width;
  result.resolution.height = opts.height;
  result.spp = opts.spp;
  result.seed = opts.seed;
  result.max_depth = opts.maxDepth;
  result.output_directory = artifactDir.string();
  result.artifact_directory = artifactDir.string();
  result.beauty_png = (artifactDir / "beauty.png").string();
  result.beauty_exr = (artifactDir / "beauty.exr").string();
  result.reference_exr = opts.referenceImage.empty() ? "" : (artifactDir / "reference.exr").string();
  result.diff_heatmap_png = opts.referenceImage.empty() ? "" : (artifactDir / "diff_heatmap.png").string();
  result.profiler_trace_json = (artifactDir / "profiler_trace.json").string();
  result.logs_jsonl = (artifactDir / "logs.jsonl").string();

  result.build_info.app_version = vkpt::build::kProjectVersion;
  result.build_info.git_hash = vkpt::build::kGitHash;
  result.build_info.build_date = vkpt::build::kBuildDate;
  result.build_info.compiler =
      std::string(vkpt::build::kCompilerName) + " " + std::string(vkpt::build::kCompilerVersion);
  result.build_info.build_type = vkpt::build::kBuildType;

  result.device_info.backend = result.backend;
  result.device_info.renderer_path = result.renderer_path;
  result.device_info.cpu_name = "unknown";
  result.device_info.gpu_name = isVulkanPath ? (backend ? backend->name() : "simulated-vulkan") : "";
  result.scene_hash = scene.export_hash_hex();
  std::string assetList;
  for (const auto& asset : scene.assets) {
    assetList += asset.uri;
  }
  result.asset_hash = Hex64(Fnv1a64(assetList));
  result.reference_error = 0.0;
  result.timing = {};
  result.timing_breakdown.clear();
  const auto manifestResult = vkpt::pathtracer::BuildRTSceneDataLayoutManifest();
  if (!manifestResult) {
    std::cerr << "failed to build shader manifest\n";
    return 2;
  }
  result.shader_hash = HashText(SerializeRTSceneDataLayoutManifest(manifestResult.value()));
  result.memory = {};
  result.timing.build_ms = 0.0;
  result.timing.render_ms = 0.0;
  result.timing.cpu_ms = 0.0;
  result.throughput.paths_per_sec = 0.0;
  result.throughput.samples_per_sec = 0.0;

  TraceProfiler profiler;
  const auto totalProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::FrameStage, "total", "frame", 0u);
  const auto buildStart = std::chrono::high_resolution_clock::now();
  const auto buildProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::BvhBuild, "scene_build", "scene", 0u);
  auto sceneData = vkpt::pathtracer::BuildSceneDataFromDocument(scene);
  if (!sceneData) {
    std::cerr << "failed to build render scene data\n";
    return 2;
  }

  vkpt::pathtracer::RenderSettings renderSettings;
  renderSettings.width = opts.width;
  renderSettings.height = opts.height;
  renderSettings.spp = opts.spp;
  renderSettings.max_depth = opts.maxDepth;
  renderSettings.seed = opts.seed;
  renderSettings.enable_nee = true;
  renderSettings.enable_mis = true;

  std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
  if (isVulkanPath) {
#if defined(PT_ENABLE_VULKAN)
    auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(PT_SHADER_SPV_PATH);
    if (!gpuTracer->is_valid()) {
      std::cerr << "vulkan path tracer init failed: " << gpuTracer->last_error() << "\n";
      return 2;
    }
    result.cpu_simd_mode = "vulkan-compute";
    result.device_info.gpu_name = gpuTracer->gpu_name();
    result.diagnostics.push_back("renderer=gpu-vulkan-compute");
    result.diagnostics.push_back("gpu_name=" + gpuTracer->gpu_name());
    result.diagnostics.push_back("gpu_type=" + gpuTracer->gpu_type());
    result.diagnostics.push_back("gpu_vram_mb=" + std::to_string(gpuTracer->vram_mb()));
    tracer = std::move(gpuTracer);
#else
    result.diagnostics.push_back("renderer=gpu-compute-simulated");
    tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
#endif
  } else if (isD3D12ComputePath || isD3D12DxrPath) {
#if defined(PT_ENABLE_D3D12)
    auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(PT_SHADER_HLSL_PATH);
    if (!gpuTracer->is_valid()) {
      std::cerr << "d3d12 path tracer init failed: " << gpuTracer->last_error() << "\n";
      return 2;
    }
    if (isD3D12DxrPath) {
      gpuTracer->set_prefer_dxr(true);
      result.cpu_simd_mode = "d3d12-dxr-requested";
      result.diagnostics.push_back("renderer=d3d12-dxr");
    } else {
      result.cpu_simd_mode = "d3d12-compute";
      result.diagnostics.push_back("renderer=d3d12-compute");
    }
    result.device_info.gpu_name = gpuTracer->gpu_name();
    result.diagnostics.push_back("gpu_name=" + gpuTracer->gpu_name());
    result.diagnostics.push_back("gpu_vram_mb=" + std::to_string(gpuTracer->vram_mb()));
    result.diagnostics.push_back("dxr_supported=" + std::string(gpuTracer->dxr_supported() ? "true" : "false"));
    result.diagnostics.push_back("dxr_tier=" + gpuTracer->dxr_tier_string());
    tracer = std::move(gpuTracer);
#else
    std::cerr << "D3D12 support is not enabled in this build\n";
    return 2;
#endif
  } else if (isTiledPath) {
    vkpt::cpu::TiledRenderConfig tiledConfig;
    tiledConfig.tile_height = opts.tileHeight;
    tiledConfig.worker_count = opts.workers;
    tiledConfig.deterministic = opts.deterministic;
    tracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
  } else {
    tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
  }
  if (!tracer->configure(renderSettings) || !tracer->load_scene_snapshot(sceneData.value()) ||
      !tracer->build_or_update_acceleration()) {
    std::cerr << "path tracer init failed\n";
    return 2;
  }
  const auto buildEnd = std::chrono::high_resolution_clock::now();
  profiler.end_event(buildProfile);
  tracer->reset_accumulation();
  const auto renderStart = std::chrono::high_resolution_clock::now();
  const auto renderProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::RenderPass, "render_samples", "render", 0u);
  for (std::uint32_t sample = 0; sample < opts.spp; ++sample) {
    if (!tracer->render_sample_batch(0, opts.height, sample, 0)) {
      std::cerr << "render failed\n";
      return 2;
    }
  }
  profiler.end_event(renderProfile);
  const auto renderEnd = std::chrono::high_resolution_clock::now();
  const auto resolveStart = std::chrono::high_resolution_clock::now();
  const auto resolveProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::CpuZone, "resolve_and_write", "io", 0u);

  const auto ldr = tracer->resolve_ldr();
  const auto hdr = tracer->resolve_hdr();
  std::string writeError;
  if (!vkpt::pathtracer::SavePngCompat(result.beauty_png, ldr, &writeError)) {
    std::cerr << "failed to save beauty png: " << writeError << "\n";
    return 2;
  }
  if (!vkpt::pathtracer::SaveExrCompat(result.beauty_exr, hdr, &writeError)) {
    std::cerr << "failed to save beauty exr: " << writeError << "\n";
    return 2;
  }
  profiler.end_event(resolveProfile);
  profiler.end_event(totalProfile);
  const auto resolveEnd = std::chrono::high_resolution_clock::now();

  std::vector<std::uint8_t> beautyBytes;
  {
    std::ifstream in(result.beauty_png, std::ios::binary);
    if (in.is_open()) {
      beautyBytes = std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
  }
  result.image_hash = Hex64(Fnv1a64(std::string_view(reinterpret_cast<const char*>(beautyBytes.data()), beautyBytes.size())));
  const auto counters = tracer->read_counters();
  const double totalMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(renderEnd - buildStart).count();
  const double buildMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(buildEnd - buildStart).count();
  const double renderMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(renderEnd - renderStart).count();
  const double resolveMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(resolveEnd - resolveStart).count();

  result.timing.total_ms = totalMs;
  result.timing.build_ms = buildMs;
  result.timing.render_ms = renderMs;
  result.timing.cpu_ms = renderMs;

  // Gate 10 (F18): profiler/timing breakdown schema.
  result.timing_breakdown.push_back({"scene_build", "scene", buildMs});
  result.timing_breakdown.push_back({"render_samples", "render", renderMs});
  result.timing_breakdown.push_back({"resolve_and_write", "io", resolveMs});
  const double pixels = static_cast<double>(opts.width) * static_cast<double>(opts.height) * std::max(1.0, 1.0 * opts.spp);
  if (renderMs > 0.0) {
    result.throughput.samples_per_sec = 1000.0 * pixels / renderMs;
    result.throughput.paths_per_sec = 1000.0 * static_cast<double>(counters.rays) / renderMs;
  }
  result.score = vkpt::benchmark::ComputeBenchmarkScore(
      result.throughput, result.resolution, std::max(1u, std::thread::hardware_concurrency()));
  result.memory.peak_mb = 0.0;
  result.memory.current_mb = 0.0;

  if (!opts.referenceImage.empty()) {
    ImageRgb reference;
    ImageRgb candidate;
    std::string refErr;
    std::string candErr;
    if (!LoadImage(opts.referenceImage, reference, &refErr)) {
      std::cerr << "failed to load reference: " << refErr << "\n";
      return 2;
    }
    if (!LoadImage(result.beauty_exr, candidate, &candErr)) {
      std::cerr << "failed to load candidate: " << candErr << "\n";
      return 2;
    }
    const auto stats = CompareImages(reference, candidate, tolerance);
    result.reference_error = stats.mean_abs_error;

    const auto referencePath = Path(opts.referenceImage);
    if (!std::filesystem::copy_file(referencePath, Path(result.reference_exr), std::filesystem::copy_options::overwrite_existing)) {
      // continue; optional reference output.
    }
    if (!referencePath.empty()) {
      const Path heatmapPath = Path(result.diff_heatmap_png);
      std::string heatmapErr;
      if (!SaveDiffHeatmap(heatmapPath, std::max(reference.width, candidate.width), std::max(reference.height, candidate.height),
                           stats.diff, &heatmapErr)) {
        std::cerr << "failed to save heatmap: " << heatmapErr << "\n";
      } else {
        (void)stats;
      }
    }
    (void)stats;
  }

  result.diagnostics.push_back("schema=" + std::string(opts.json ? "json" : "text"));

  // F08: record multithreaded benchmark metrics for cpu-tiled path
  if (isTiledPath) {
    const auto* tiled = dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(tracer.get());
    if (tiled) {
      const std::size_t wc = tiled->worker_count();
      const uint32_t th = tiled->tile_height();
      const auto& bvh = tiled->bvh_stats();
      result.diagnostics.push_back("renderer=cpu-tiled");
      result.diagnostics.push_back("worker_count=" + std::to_string(wc));
      result.diagnostics.push_back("tile_height_rows=" + std::to_string(th));
      result.diagnostics.push_back("bvh_nodes=" + std::to_string(bvh.node_count));
      result.diagnostics.push_back("bvh_build_ms=" + std::to_string(bvh.build_ms));
      result.diagnostics.push_back("deterministic=" + std::string(opts.deterministic ? "true" : "false"));
      // samples_per_sec and paths_per_sec already in result.throughput
      // speedup estimate vs scalar: ratio of worker_count (linear scaling assumption)
      const double speedup_estimate = static_cast<double>(wc);
      result.diagnostics.push_back("speedup_estimate_vs_scalar=" + std::to_string(speedup_estimate));
    }
  }

#if defined(PT_ENABLE_D3D12)
  if (isD3D12ComputePath || isD3D12DxrPath) {
    const auto* d3d12 = dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(tracer.get());
    if (d3d12) {
      if (isD3D12DxrPath) {
        result.cpu_simd_mode = d3d12->using_dxr_dispatch() ? "d3d12-dxr" : "d3d12-compute-fallback";
      } else {
        result.cpu_simd_mode = "d3d12-compute";
      }
      result.diagnostics.push_back("prefer_dxr=" + std::string(d3d12->prefer_dxr() ? "true" : "false"));
      result.diagnostics.push_back("using_dxr_dispatch=" + std::string(d3d12->using_dxr_dispatch() ? "true" : "false"));
      result.diagnostics.push_back("rays_per_pixel_per_dispatch=" +
                                   std::to_string(d3d12->rays_per_pixel_per_dispatch()));
      result.diagnostics.push_back("readback_interval=" + std::to_string(d3d12->readback_interval()));
      result.diagnostics.push_back("force_readback_every_sample=" +
                                   std::string(d3d12->force_readback_every_sample() ? "true" : "false"));
      result.diagnostics.push_back("dynamic_instance_transforms=" +
                                   std::string(d3d12->dynamic_instance_transforms_allowed() ? "true" : "false"));
      result.diagnostics.push_back("dxr_build_mode=" + d3d12->dxr_build_mode());
    }
  }
#endif

  {
    std::ofstream traceFile(result.profiler_trace_json);
    if (traceFile.is_open()) {
      traceFile << profiler.emit_trace() << "\n";
    }
  }

  {
    std::ofstream logFile(result.logs_jsonl);
    if (!logFile.is_open()) {
      std::cerr << "warning: failed to create logs.jsonl\n";
    } else {
      logFile << "{";
      logFile << "\"ts\":\"" << EscapeJson(NowUtcString()) << "\",";
      logFile << "\"severity\":\"info\",";
      logFile << "\"message\":\"run complete\",";
      logFile << "\"fields\":[{\"k\":\"scene\",\"v\":\"" << EscapeJson(result.scene) << "\"},"
              << "{\"k\":\"backend\",\"v\":\"" << EscapeJson(result.backend) << "\"},"
              << "{\"k\":\"renderer\",\"v\":\"" << EscapeJson(result.renderer_path) << "\"},"
              << "{\"k\":\"total_ms\",\"v\":\"" << std::fixed << std::setprecision(6) << result.timing.total_ms << "\"},"
              << "{\"k\":\"normalized_score\",\"v\":\"" << std::fixed << std::setprecision(6)
              << result.score.normalized_score << "\"}"
              << "]"
              << "}\n";
    }
  }

  if (!WriteRunArtifacts(result, artifactDir, scene, manifestResult.value(), Path(opts.referenceImage), Path(result.diff_heatmap_png),
                        !opts.referenceImage.empty(), &runError)) {
    std::cerr << runError << "\n";
    return 2;
  }

  const auto artifactValidation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(artifactDir.string());
  if (!artifactValidation.ok) {
    std::cerr << "artifact validation failed: "
              << vkpt::benchmark::SerializeBenchmarkArtifactValidation(artifactValidation) << "\n";
    return 2;
  }

  if (opts.json) {
    std::cout << vkpt::benchmark::SerializeBenchmarkResult(result) << "\n";
  } else {
    std::cout << "run complete\n";
    std::cout << "output: " << artifactDir.string() << "\n";
    std::cout << "beauty: " << result.beauty_png << "\n";
    std::cout << "exr: " << result.beauty_exr << "\n";
    std::cout << "results: " << (artifactDir / "results.json").string() << "\n";
  }
  return 0;
}

int EchoDescCommand(const std::vector<std::string_view>& args) {
  RunOptions opts;
  opts.echoDesc = true;
  std::string parseError;
  if (!ParseRunArgs(args, opts, &parseError)) {
    std::cerr << parseError << "\n";
    return 1;
  }
  auto desc = ToRunDesc(opts);
  std::string issue;
  if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
    std::cerr << "invalid benchmark descriptor: " << issue << "\n";
    return 1;
  }
  std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(desc) << "\n";
  return 0;
}

int ValidateArtifactsCommand(const std::vector<std::string_view>& args) {
  std::string dir;
  bool json = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--dir") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --dir\n";
        return 1;
      }
      dir = std::string(args[++i]);
    } else if (args[i] == "--json") {
      json = true;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (dir.empty()) {
    std::cerr << "validate-artifacts requires --dir\n";
    return 1;
  }
  const auto validation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(dir);
  if (json) {
    std::cout << vkpt::benchmark::SerializeBenchmarkArtifactValidation(validation) << "\n";
  } else {
    std::cout << "artifacts: " << dir << "\n";
    std::cout << "valid: " << (validation.ok ? "yes" : "no") << "\n";
    for (const auto& item : validation.missing_files) {
      std::cout << " - missing: " << item << "\n";
    }
    for (const auto& item : validation.empty_files) {
      std::cout << " - empty: " << item << "\n";
    }
    for (const auto& item : validation.invalid_files) {
      std::cout << " - invalid: " << item << "\n";
    }
  }
  return validation.ok ? 0 : 2;
}

std::vector<std::string> BuildRunArgsFromDesc(const vkpt::benchmark::BenchmarkRunDesc& desc) {
  std::vector<std::string> args = {
      "ptbench",
      "run",
      "--scene",
      desc.scene_path,
      "--backend",
      desc.backend,
      "--renderer-path",
      desc.renderer_path,
      "--resolution",
      std::to_string(desc.resolution.width) + "x" + std::to_string(desc.resolution.height),
      "--spp",
      std::to_string(desc.samples_per_pixel),
      "--seed",
      std::to_string(desc.seed),
      "--max-depth",
      std::to_string(desc.max_depth),
      "--duration",
      std::to_string(desc.duration),
      "--warmup-frames",
      std::to_string(desc.warmup_frames),
      "--tolerance-policy",
      desc.tolerance_policy.empty() ? std::string("abs=0.001") : desc.tolerance_policy,
      "--output",
      desc.output_directory,
      "--workers",
      std::to_string(desc.worker_count),
      "--tile-size",
      std::to_string(desc.tile_height),
  };
  if (!desc.reference_image.empty()) {
    args.push_back("--reference-image");
    args.push_back(desc.reference_image);
  }
  if (desc.deterministic) {
    args.push_back("--deterministic");
  }
  return args;
}

std::vector<std::string_view> ToArgViews(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.emplace_back(arg);
  }
  return views;
}

class CliBenchmarkRunner final : public vkpt::benchmark::IBenchmarkRunner {
 public:
  vkpt::core::Result<vkpt::benchmark::BenchmarkResult> run_once(
      const vkpt::benchmark::BenchmarkRunDesc& desc) override {
    std::string issue;
    if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
      return vkpt::core::Result<vkpt::benchmark::BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    const auto args = BuildRunArgsFromDesc(desc);
    const auto views = ToArgViews(args);
    const int rc = RunCommand(views);
    if (rc != 0) {
      return vkpt::core::Result<vkpt::benchmark::BenchmarkResult>::error(vkpt::core::ErrorCode::Internal);
    }
    return vkpt::benchmark::LoadBenchmarkResultFromFile((Path(desc.output_directory) / "results.json").string());
  }

  vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>> run_suite(
      const std::vector<vkpt::benchmark::BenchmarkRunDesc>& descs) override {
    std::vector<vkpt::benchmark::BenchmarkResult> results;
    results.reserve(descs.size());
    for (const auto& desc : descs) {
      auto result = run_once(desc);
      if (!result) {
        return vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>>::error(result.error());
      }
      results.push_back(std::move(result.value()));
    }
    return vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>>::ok(std::move(results));
  }

  vkpt::benchmark::BenchmarkArtifactValidation validate_artifacts(std::string_view artifact_directory) const override {
    return vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(artifact_directory);
  }

  std::string summarize_results(const std::vector<vkpt::benchmark::BenchmarkResult>& results) const override {
    return vkpt::benchmark::SummarizeBenchmarkResults(results);
  }
};

int ListScenesCommand() {
  const Path sceneDir("assets/scenes");
  if (!std::filesystem::exists(sceneDir)) {
    std::cerr << "scene directory missing: " << sceneDir.string() << "\n";
    return 1;
  }
  std::cout << "scenes:\n";
  for (const auto& entry : std::filesystem::directory_iterator(sceneDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::cout << "  " << entry.path().filename().string() << "\n";
  }
  return 0;
}

int ListBackendsCommand() {
  const auto names = vkpt::render::AvailableBackendNames();
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) {
      std::cout << name << " unavailable\n";
      continue;
    }
    if (!backend->initialize()) {
      std::cout << name << " failed initialize\n";
      continue;
    }
    const auto caps = backend->capabilities();
    std::cout << name << "\n";
    std::cout << "  " << vkpt::render::SerializeBackendCapabilities(caps) << "\n";
  }
  return 0;
}

int ListRendererPathsCommand(const std::vector<std::string_view>& args) {
  std::string backend = "auto";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--backend") {
      if (i + 1 < args.size()) {
        backend = std::string(args[i + 1]);
      }
      ++i;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  const auto rendererPaths = AvailableRendererPaths(backend);
  std::cout << "renderer-paths:\n";
  for (const auto& path : rendererPaths) {
    std::cout << "  " << path << "\n";
  }
  return 0;
}

int ValidateSceneCommand(const std::vector<std::string_view>& args) {
  std::string scenePath;
  std::string backend = "cpu";
  std::string renderer = "cpu-scalar";
  bool json = false;

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --scene\n";
        return 1;
      }
      scenePath = std::string(args[++i]);
    } else if (args[i] == "--backend") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --backend\n";
        return 1;
      }
      backend = std::string(args[++i]);
    } else if (args[i] == "--renderer-path") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --renderer-path\n";
        return 1;
      }
      renderer = std::string(args[++i]);
    } else if (args[i] == "--json") {
      json = true;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scenePath.empty()) {
    std::cerr << "missing --scene\n";
    return 1;
  }

  const auto result = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!result) {
    if (json) {
      std::cout << "{\"ok\":false,\"scene\":\"" << EscapeJson(scenePath) << "\",\"issues\":[\"load failed\"]}\n";
    } else {
      std::cerr << "validate failed: cannot load " << scenePath << "\n";
    }
    return 2;
  }

  std::vector<std::string> issues;
  const bool validScene = result.value().validate(&issues);
  std::string backendError;
  const bool validBackend = ValidateBackendRenderer(backend, renderer, &backendError);
  const auto snapshot = result.value().snapshot();
  const std::size_t entityCount = snapshot.entity_ids.size();
  const std::size_t cameraCount = snapshot.camera ? 1u : 0u;
  const bool lights = !snapshot.lights.empty();
  const bool materials = !result.value().materials.empty();
  const bool benchmarkSettings = result.value().benchmark.enabled && result.value().benchmark.frame_target > 0u;
  if (!validBackend && !backendError.empty()) {
    issues.push_back(backendError);
  }
  if (result.value().metadata.schema.empty()) {
    issues.push_back("metadata.schema is empty");
  }
  if (result.value().export_hash_hex().empty()) {
    issues.push_back("missing scene hash");
  }
  if (entityCount == 0) {
    issues.push_back("no entities");
  }
  if (cameraCount == 0) {
    issues.push_back("no camera");
  }
  if (!lights) {
    issues.push_back("no lights");
  }
  if (!materials) {
    issues.push_back("no materials");
  }
  if (!benchmarkSettings) {
    issues.push_back("benchmark settings are disabled or missing frame_target");
  }
  if (result.value().benchmark.warmup_frames > result.value().benchmark.frame_target &&
      result.value().benchmark.frame_target > 0u) {
    issues.push_back("benchmark warmup_frames exceeds frame_target");
  }
  std::unordered_set<std::uint64_t> materialIds;
  for (const auto& material : result.value().materials) {
    materialIds.insert(material.id);
  }
  for (const auto& geometry : result.value().geometry) {
    if (geometry.material_id != 0u && materialIds.find(geometry.material_id) == materialIds.end()) {
      issues.push_back("geometry references missing material " + std::to_string(geometry.material_id));
    }
  }
  for (const auto& renderable : snapshot.renderables) {
    if (renderable.material_id != 0u && materialIds.find(renderable.material_id) == materialIds.end()) {
      issues.push_back("renderable references missing material " + std::to_string(renderable.material_id));
    }
  }
  std::unordered_set<std::string> assetUris;
  for (const auto& asset : result.value().assets) {
    if (asset.uri.empty()) {
      issues.push_back("asset has empty uri");
    }
    if (!asset.uri.empty() && !assetUris.insert(asset.uri).second) {
      issues.push_back("duplicate asset uri " + asset.uri);
    }
  }

  const bool ok = validScene && validBackend && benchmarkSettings && issues.empty();
  if (json) {
    std::ostringstream out;
    out << "{";
    out << "\"ok\":" << (ok ? "true" : "false") << ",";
    out << "\"scene\":\"" << EscapeJson(scenePath) << "\",";
    out << "\"schema_version\":\"" << EscapeJson(result.value().metadata.schema) << "\",";
    out << "\"asset_count\":" << result.value().assets.size() << ",";
    out << "\"material_count\":" << result.value().materials.size() << ",";
    out << "\"entity_count\":" << entityCount << ",";
    out << "\"camera_count\":" << cameraCount << ",";
    out << "\"has_lights\":" << (lights ? "true" : "false") << ",";
    out << "\"has_materials\":" << (materials ? "true" : "false") << ",";
    out << "\"benchmark_settings\":" << (benchmarkSettings ? "true" : "false") << ",";
    out << "\"backend_compatible\":" << (validBackend ? "true" : "false") << ",";
    out << "\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) out << ",";
      out << "\"" << EscapeJson(issues[i]) << "\"";
    }
    out << "]}";
    std::cout << out.str() << "\n";
  } else {
    std::cout << "scene: " << scenePath << "\n";
    std::cout << "valid: " << (ok ? "yes" : "no") << "\n";
    std::cout << "schema: " << result.value().metadata.schema << "\n";
    std::cout << "hash: " << result.value().export_hash_hex() << "\n";
    for (const auto& issue : issues) {
      std::cout << " - " << issue << "\n";
    }
    std::cout << "backend compatibility: " << (validBackend ? "ok" : "failed") << "\n";
  }
  return ok ? 0 : 2;
}

int MaterialCoverageCommand(const std::vector<std::string_view>& args) {
  bool json = false;
  std::string output = "artifacts/experiments/material_coverage.json";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output value\n";
        return 1;
      }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }

  const auto& descriptors = vkpt::materials::GetMaterialRegistry();
  vkpt::scene::SceneDocument doc;
  doc.metadata.schema = "1.0";
  doc.metadata.scene_name = "material_coverage";
  doc.benchmark.enabled = true;
  doc.benchmark.frame_target = 1;
  doc.benchmark.warmup_frames = 0;

  auto is_one_of = [](std::string_view id, std::initializer_list<std::string_view> values) {
    for (const auto value : values) {
      if (id == value) {
        return true;
      }
    }
    return false;
  };

  for (std::size_t i = 0; i < descriptors.size(); ++i) {
    const auto& descriptor = descriptors[i];
    const auto materialId = static_cast<vkpt::core::StableId>(1000u + i);

    vkpt::scene::SceneMaterialDefinition material;
    material.id = materialId;
    material.name = descriptor.display_name;
    material.family = descriptor.id;
    doc.materials.push_back(std::move(material));

    vkpt::scene::SceneGeometryDefinition geometry;
    geometry.id = static_cast<vkpt::core::StableId>(2000u + i);
    geometry.primitive = "triangle";
    geometry.material_id = materialId;
    const float x = static_cast<float>(i % 12u);
    const float y = static_cast<float>(i / 12u);
    geometry.vertices = {{x, y, 0.0f}, {x + 0.8f, y, 0.0f}, {x + 0.4f, y + 0.75f, 0.0f}};
    geometry.indices = {0u, 1u, 2u};
    doc.geometry.push_back(std::move(geometry));

    vkpt::scene::SceneEntityDefinition entity;
    entity.id = static_cast<vkpt::core::StableId>(3000u + i);
    entity.name = descriptor.id;
    entity.has_mesh = true;
    entity.mesh.mesh_id = static_cast<vkpt::core::StableId>(2000u + i);
    entity.mesh.material_id = materialId;
    doc.entities.push_back(std::move(entity));
  }

  vkpt::scene::SceneEntityDefinition camera;
  camera.id = 4000u;
  camera.name = "coverage_camera";
  camera.has_camera = true;
  camera.camera.fov = 45.0f;
  doc.entities.push_back(std::move(camera));

  vkpt::scene::SceneEntityDefinition light;
  light.id = 4001u;
  light.name = "coverage_light";
  light.has_light = true;
  light.light.type = "point";
  light.light.color = {1.0f, 1.0f, 1.0f};
  light.light.intensity = 4.0f;
  doc.entities.push_back(std::move(light));

  std::vector<std::string> issues;
  auto rtResult = vkpt::pathtracer::BuildSceneDataFromDocument(doc);
  if (!rtResult) {
    issues.push_back("rt_scene_build_failed");
  }

  struct Row {
    std::string id;
    std::string status;
    uint32_t model = 0;
    uint32_t effect = 0;
    float roughness = 0.0f;
    float metallic = 0.0f;
    float transmission = 0.0f;
    float clearcoat = 0.0f;
    float alpha = 1.0f;
    bool emissive = false;
    std::string issue;
  };
  std::vector<Row> rows;
  std::array<std::uint32_t, 16> modelCounts{};
  std::array<std::uint32_t, 32> effectCounts{};

  if (rtResult) {
    const auto& rtScene = rtResult.value();
    if (rtScene.materials.size() != descriptors.size()) {
      issues.push_back("material_count_mismatch:" + std::to_string(rtScene.materials.size()) +
                       "!=" + std::to_string(descriptors.size()));
    }
    const std::size_t count = std::min(rtScene.materials.size(), descriptors.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto& descriptor = descriptors[i];
      const auto& material = rtScene.materials[i];
      const std::string id = descriptor.id;
      Row row;
      row.id = id;
      row.status = vkpt::materials::ToString(descriptor.status);
      row.model = material.material_model;
      row.effect = material.material_effect;
      row.roughness = material.roughness;
      row.metallic = material.metallic;
      row.transmission = material.transmission;
      row.clearcoat = material.clearcoat;
      row.alpha = material.alpha;
      row.emissive = material.is_emissive();

      if (row.model < modelCounts.size()) {
        ++modelCounts[row.model];
      }
      if (row.effect < effectCounts.size()) {
        ++effectCounts[row.effect];
      }

      auto fail = [&](std::string message) {
        if (!row.issue.empty()) {
          row.issue += ";";
        }
        row.issue += std::move(message);
      };

      if (descriptor.status != vkpt::materials::ImplementationStatus::Implemented) {
        fail("descriptor_not_implemented");
      }
      if (!std::isfinite(row.roughness) || !std::isfinite(row.metallic) ||
          !std::isfinite(row.transmission) || !std::isfinite(row.clearcoat) ||
          !std::isfinite(row.alpha)) {
        fail("non_finite_material_value");
      }
      if (row.roughness < 0.0f || row.roughness > 1.0f ||
          row.metallic < 0.0f || row.metallic > 1.0f ||
          row.transmission < 0.0f || row.transmission > 1.0f ||
          row.clearcoat < 0.0f || row.clearcoat > 1.0f ||
          row.alpha < 0.0f || row.alpha > 1.0f) {
        fail("material_value_out_of_range");
      }

      const bool emissive = is_one_of(id, {"emissive", "environment_emissive", "blackbody_emission",
                                           "fire_plasma", "fire_sparkle_emission",
                                           "light_emitting_textile", "bokeh_motion_blur_stress"});
      const bool metal = is_one_of(id, {"ggx_rough_conductor", "metallic_pbr", "anisotropic_ggx",
                                        "brushed_metal", "ground_metal"});
      const bool glass = is_one_of(id, {"ggx_rough_dielectric", "dielectric_glass",
                                        "spectral_glass_approx", "frosted_glass", "dirty_glass",
                                        "water_fluid_surface", "ice_crystal", "resin", "epoxy",
                                        "gemstone", "frosted_acrylic", "translucent_polymer"});
      const bool coated = is_one_of(id, {"clearcoat", "paint", "car_paint", "porcelain_ceramic",
                                         "wet_surface", "energy_conserving_layered",
                                         "thin_film_iridescent", "diffraction_grating",
                                         "holographic_coating", "retroreflector",
                                         "caustics_inspired_response"});
      const bool sheen = is_one_of(id, {"velvet", "fabric_cloth", "hair_fur_lobes", "pearl_lustre"});
      const bool toon = is_one_of(id, {"toon_surface", "stylized_diffuse", "xray"});
      const bool volume = is_one_of(id, {"volumetric_medium", "volumetric_shafts", "smoke", "chromatic_dust"});

      if (id == "diffuse" && row.model != 0u) {
        fail("diffuse_model");
      }
      if (emissive && (row.model != 1u || !row.emissive)) {
        fail("emissive_runtime");
      }
      if (id == "mirror" && (row.model != 2u || row.roughness > 0.01f || row.metallic < 0.99f)) {
        fail("mirror_runtime");
      }
      if (is_one_of(id, {"specular", "glossy", "normal_mapped_pbr", "plastic", "rubber"}) &&
          row.model != 3u) {
        fail("specular_runtime");
      }
      if (metal && (row.model != 4u || row.metallic < 0.9f)) {
        fail("metal_runtime");
      }
      if (glass && (row.model != 5u || row.transmission <= 0.05f)) {
        fail("glass_runtime");
      }
      if (coated && (row.model != 7u || row.clearcoat <= 0.05f)) {
        fail("clearcoat_runtime");
      }
      if (sheen && (row.model != 6u || (row.effect == 0u && row.clearcoat <= 0.05f))) {
        fail("sheen_runtime");
      }
      if (toon && row.model != 8u) {
        fail("toon_runtime");
      }
      const bool validVolumeEffect = row.effect == 15u || (id == "chromatic_dust" && row.effect == 6u);
      if (volume && (row.model != 9u || !validVolumeEffect || row.alpha >= 0.99f)) {
        fail("volume_runtime");
      }
      if (id == "rubber" && (row.transmission > 0.05f || row.model == 5u)) {
        fail("rubber_not_glass");
      }
      if (id != "diffuse" && !emissive && row.model == 0u && row.effect == 0u) {
        fail("unclassified_diffuse_fallback");
      }

      if (!row.issue.empty()) {
        issues.push_back(id + ":" + row.issue);
      }
      rows.push_back(std::move(row));
    }
  }

  const bool ok = issues.empty();
  EnsureDirectory(Path(output).parent_path());
  std::ofstream out{Path(output)};
  if (out.is_open()) {
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"material_count\": " << descriptors.size() << ",\n";
    out << "  \"model_counts\": [";
    for (std::size_t i = 0; i < modelCounts.size(); ++i) {
      if (i) out << ",";
      out << modelCounts[i];
    }
    out << "],\n";
    out << "  \"effect_counts\": [";
    for (std::size_t i = 0; i < effectCounts.size(); ++i) {
      if (i) out << ",";
      out << effectCounts[i];
    }
    out << "],\n";
    out << "  \"issues\": [";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) out << ",";
      out << "\"" << EscapeJson(issues[i]) << "\"";
    }
    out << "],\n";
    out << "  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"id\":\"" << EscapeJson(row.id) << "\","
          << "\"status\":\"" << EscapeJson(row.status) << "\","
          << "\"model\":" << row.model << ","
          << "\"effect\":" << row.effect << ","
          << "\"roughness\":" << row.roughness << ","
          << "\"metallic\":" << row.metallic << ","
          << "\"transmission\":" << row.transmission << ","
          << "\"clearcoat\":" << row.clearcoat << ","
          << "\"alpha\":" << row.alpha << ","
          << "\"emissive\":" << (row.emissive ? "true" : "false") << ","
          << "\"issue\":\"" << EscapeJson(row.issue) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
  }

  if (json) {
    std::cout << "{\"ok\":" << (ok ? "true" : "false")
              << ",\"material_count\":" << descriptors.size()
              << ",\"issue_count\":" << issues.size()
              << ",\"artifact\":\"" << EscapeJson(output) << "\"";
    if (!issues.empty()) {
      std::cout << ",\"issues\":[";
      for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "\"" << EscapeJson(issues[i]) << "\"";
      }
      std::cout << "]";
    }
    std::cout << "}\n";
  } else {
    std::cout << "material coverage: " << (ok ? "ok" : "failed") << "\n";
    std::cout << "materials: " << descriptors.size() << "\n";
    std::cout << "artifact: " << output << "\n";
    for (const auto& issue : issues) {
      std::cout << " - " << issue << "\n";
    }
  }
  return ok ? 0 : 2;
}

int DumpCapabilitiesCommand() {
  const auto cpuFeatures = vkpt::cpu::QueryCpuFeatures();
  const auto bestSimd = vkpt::cpu::SelectBestSimdMode(cpuFeatures);

  std::cout << "{\n";
  std::cout << "  \"cpu\":" << vkpt::cpu::SerializeCpuFeatures(cpuFeatures) << ",\n";
  std::cout << "  \"simd_mode\":\"" << vkpt::cpu::SimdModeName(bestSimd) << "\",\n";
  std::cout << "  \"backends\": [\n";
  const auto names = vkpt::render::AvailableBackendNames();
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto name = names[i];
    const auto backend = vkpt::render::CreateBackend(name);
    std::cout << "    {\n";
    std::cout << "      \"name\":\"" << EscapeJson(name) << "\",\n";
    if (backend && backend->initialize()) {
      std::cout << "      \"available\":true,\n";
      std::cout << "      \"capabilities\":" << vkpt::render::SerializeBackendCapabilities(backend->capabilities()) << "\n";
    } else {
      std::cout << "      \"available\":false,\n";
      std::cout << "      \"capabilities\":null\n";
    }
    std::cout << "    }";
    if (i + 1 < names.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }
  std::cout << "  ],\n";
  std::cout << "  \"benchmark_capabilities\":"
            << vkpt::benchmark::SerializeBenchmarkCapabilities(vkpt::benchmark::DefaultBenchmarkCapabilities()) << ",\n";
  std::cout << "  \"profiler_capabilities\":"
            << vkpt::benchmark::SerializeProfilerCapabilities(vkpt::benchmark::DefaultProfilerCapabilities()) << "\n";
  std::cout << "}\n";
  return 0;
}

int CompareCommand(const std::vector<std::string_view>& args) {
  std::string referencePath;
  std::string imagePath;
  std::string outputPath;
  std::string tolerance = "abs=0.001";
  bool noHeatmap = false;

  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--reference") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --reference\n";
        return 1;
      }
      referencePath = std::string(args[++i]);
    } else if (token == "--image") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --image\n";
        return 1;
      }
      imagePath = std::string(args[++i]);
    } else if (token == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output\n";
        return 1;
      }
      outputPath = std::string(args[++i]);
    } else if (token == "--tolerance-policy") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --tolerance-policy\n";
        return 1;
      }
      tolerance = std::string(args[++i]);
    } else if (token == "--disable-heatmap") {
      noHeatmap = true;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      return 1;
    }
  }
  if (referencePath.empty() || imagePath.empty() || outputPath.empty()) {
    std::cerr << "compare requires --reference --image --output\n";
    return 1;
  }
  TolerancePolicy policy;
  std::string parseError;
  if (!ParseTolerance(tolerance, policy, &parseError)) {
    std::cerr << "invalid tolerance: " << parseError << "\n";
    return 1;
  }
  ImageRgb reference;
  ImageRgb candidate;
  std::string err;
  if (!LoadImage(referencePath, reference, &err) || !LoadImage(imagePath, candidate, &err)) {
    std::cerr << "compare load failed: " << err << "\n";
    return 2;
  }
  const auto result = CompareImages(reference, candidate, policy);
  std::cout << std::fixed << std::setprecision(8);
  std::cout << "mean_abs_error: " << result.mean_abs_error << "\n";
  std::cout << "max_error: " << result.max_error << "\n";
  std::cout << "rmse: " << result.rmse << "\n";
  std::cout << "nan_inf_count: " << result.nan_inf_count << "\n";

  if (!noHeatmap) {
    Path out(outputPath);
    EnsureDirectory(out);
    Path heatmap = out / "diff_heatmap.png";
    // CompareImages stores per-channel difference in candidate-sized vector.
    // Rebuild heatmap by reusing stored diff vector from another compare.
    const auto diffResult = CompareImages(reference, candidate, policy);
    if (SaveDiffHeatmap(heatmap, reference.width, reference.height, std::vector<float>(diffResult.diff), nullptr)) {
      std::cout << "heatmap: " << heatmap.string() << "\n";
    }
  }
  return 0;
}

// F09: SIMD sweep — measures raw ray/triangle intersection throughput for each available kernel.
std::vector<uint32_t> ParseWorkerList(const std::string& text) {
  std::vector<uint32_t> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto comma = text.find(',', start);
    const auto token = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!token.empty()) {
      try {
        const auto value = static_cast<uint32_t>(std::stoul(token));
        if (value > 0u) {
          values.push_back(value);
        }
      } catch (...) {
        return {};
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

int ThreadSweepCommand(const std::vector<std::string_view>& args) {
  std::string scene;
  uint32_t spp = 2;
  std::string resolution = "128x128";
  std::string output = "artifacts/thread_sweep";
  std::vector<uint32_t> workers;

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene" && i + 1 < args.size()) {
      scene = std::string(args[++i]);
    } else if (args[i] == "--workers" && i + 1 < args.size()) {
      workers = ParseWorkerList(std::string(args[++i]));
      if (workers.empty()) {
        std::cerr << "invalid --workers list\n";
        return 1;
      }
    } else if (args[i] == "--spp" && i + 1 < args.size()) {
      if (!ParseUnsigned(args[++i], spp) || spp == 0u) {
        std::cerr << "invalid --spp\n";
        return 1;
      }
    } else if (args[i] == "--resolution" && i + 1 < args.size()) {
      resolution = std::string(args[++i]);
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scene.empty()) {
    std::cerr << "thread-sweep requires --scene\n";
    return 1;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (!ParseResolution(resolution, width, height) || width == 0u || height == 0u) {
    std::cerr << "invalid --resolution\n";
    return 1;
  }

  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  if (workers.empty()) {
    workers = {1u, 2u, 4u, 8u};
  }

  struct Row {
    uint32_t workers = 0;
    std::string status;
    std::string reason;
    double samples_per_sec = 0.0;
    double paths_per_sec = 0.0;
    double normalized_score = 0.0;
    double speedup_vs_one_worker = 0.0;
    std::string artifact_dir;
  };
  std::vector<Row> rows;
  CliBenchmarkRunner runner;
  double oneWorkerSamples = 0.0;

  for (const auto workerCount : workers) {
    Row row;
    row.workers = workerCount;
    row.artifact_dir = (Path(output) / ("workers_" + std::to_string(workerCount))).string();
    if (workerCount > hardware) {
      row.status = "skipped";
      row.reason = "worker count exceeds hardware concurrency";
      rows.push_back(std::move(row));
      continue;
    }

    vkpt::benchmark::BenchmarkRunDesc desc;
    desc.scene_path = scene;
    desc.backend = "cpu";
    desc.renderer_path = "cpu-tiled";
    desc.resolution.width = width;
    desc.resolution.height = height;
    desc.samples_per_pixel = spp;
    desc.duration = 0.0;
    desc.warmup_frames = 0;
    desc.seed = 0xBADC0FFEEull;
    desc.output_directory = row.artifact_dir;
    desc.tolerance_policy = "abs=0.001";
    desc.max_depth = 6;
    desc.worker_count = workerCount;
    desc.tile_height = 16;

    const auto result = runner.run_once(desc);
    if (!result) {
      row.status = "failed";
      row.reason = "benchmark run failed";
    } else {
      row.status = "ok";
      row.samples_per_sec = result.value().throughput.samples_per_sec;
      row.paths_per_sec = result.value().throughput.paths_per_sec;
      row.normalized_score = result.value().score.normalized_score;
      if (workerCount == 1u) {
        oneWorkerSamples = std::max(1.0, row.samples_per_sec);
      }
      if (oneWorkerSamples > 0.0) {
        row.speedup_vs_one_worker = row.samples_per_sec / oneWorkerSamples;
      }
    }
    rows.push_back(std::move(row));
  }

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "thread_sweep.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"scene\":\"" << EscapeJson(scene) << "\",\n";
    out << "  \"hardware_threads\":" << hardware << ",\n";
    out << "  \"rows\":[\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"workers\":" << row.workers
          << ",\"status\":\"" << EscapeJson(row.status) << "\""
          << ",\"reason\":\"" << EscapeJson(row.reason) << "\""
          << ",\"samples_per_sec\":" << std::fixed << std::setprecision(4) << row.samples_per_sec
          << ",\"paths_per_sec\":" << std::fixed << std::setprecision(4) << row.paths_per_sec
          << ",\"normalized_score\":" << std::fixed << std::setprecision(4) << row.normalized_score
          << ",\"speedup_vs_one_worker\":" << std::fixed << std::setprecision(4) << row.speedup_vs_one_worker
          << ",\"artifact_dir\":\"" << EscapeJson(row.artifact_dir) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  const bool failed = std::any_of(rows.begin(), rows.end(), [](const Row& row) { return row.status == "failed"; });
  return failed ? 2 : 0;
}

int SimdSweepCommand(const std::vector<std::string_view>& args) {
  uint64_t rayCount   = 1'000'000ULL;
  uint64_t triCount   = 1024ULL;
  std::string output  = "artifacts/simd_sweep";

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--rays" && i + 1 < args.size()) {
      rayCount = static_cast<uint64_t>(std::stoul(std::string(args[++i])));
    } else if (args[i] == "--triangles" && i + 1 < args.size()) {
      triCount = static_cast<uint64_t>(std::stoul(std::string(args[++i])));
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }

  // Generate pseudo-random triangles and rays
  auto lcg = [](uint64_t& s) -> float {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<float>((s >> 33) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
  };

  std::vector<vkpt::cpu::TriangleSOA> triangles(triCount);
  uint64_t seed = 0xDEADBEEF12345678ULL;
  for (auto& tri : triangles) {
    const float v0x = lcg(seed) * 10.0f - 5.0f;
    const float v0y = lcg(seed) * 10.0f - 5.0f;
    const float v0z = lcg(seed) * 10.0f - 5.0f;
    tri.v0x = v0x; tri.v0y = v0y; tri.v0z = v0z;
    tri.e1x = lcg(seed) * 2.0f - 1.0f;
    tri.e1y = lcg(seed) * 2.0f - 1.0f;
    tri.e1z = lcg(seed) * 2.0f - 1.0f;
    tri.e2x = lcg(seed) * 2.0f - 1.0f;
    tri.e2y = lcg(seed) * 2.0f - 1.0f;
    tri.e2z = lcg(seed) * 2.0f - 1.0f;
    tri.material_index = 0;
  }

  // Build a ray packet (reuse same packet for the benchmark, like a hot cache)
  constexpr uint32_t kPacketW = 4u;
  vkpt::cpu::RayPacket packet{};
  packet.count = kPacketW;
  seed = 0xCAFEBABECAFEBABEULL;
  for (uint32_t i = 0; i < kPacketW; ++i) {
    packet.origin_x[i] = lcg(seed) * 2.0f - 1.0f;
    packet.origin_y[i] = lcg(seed) * 2.0f - 1.0f;
    packet.origin_z[i] = (lcg(seed) + 1.0f) * 5.0f;
    const float dx = lcg(seed) * 0.1f;
    const float dy = lcg(seed) * 0.1f;
    const float dz = -1.0f;
    const float inv_len = 1.0f / std::sqrt(dx*dx + dy*dy + dz*dz);
    packet.dir_x[i] = dx * inv_len;
    packet.dir_y[i] = dy * inv_len;
    packet.dir_z[i] = dz * inv_len;
  }

  struct SweepResult {
    std::string mode_name;
    uint64_t total_rays = 0;
    double mrays_per_sec = 0.0;
    bool available = false;
    std::string status = "skipped";
    std::string skip_reason;
    double speedup_vs_scalar = 0.0;
  };

  std::vector<SweepResult> results;

  auto bench_mode = [&](const char* name, bool available, auto fn) {
    SweepResult r;
    r.mode_name = name;
    r.available = available;
    if (!available) {
      r.status = "skipped";
      r.skip_reason = "not supported by this CPU or build";
      results.push_back(r);
      return;
    }
    r.status = "ok";
    vkpt::cpu::HitPacket hits{};
    vkpt::cpu::reset_hit_packet(hits, kPacketW);
    // Warm-up
    for (uint64_t t = 0; t < 1000ULL; ++t) fn(packet, triangles[t % triCount], hits);
    vkpt::cpu::reset_hit_packet(hits, kPacketW);
    const auto t0 = std::chrono::high_resolution_clock::now();
    const uint64_t total_pairs = rayCount / kPacketW;
    for (uint64_t t = 0; t < total_pairs; ++t) fn(packet, triangles[t % triCount], hits);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.total_rays = total_pairs * kPacketW;
    r.mrays_per_sec = (ms > 0.0) ? (static_cast<double>(r.total_rays) / ms / 1000.0) : 0.0;
    results.push_back(r);
    std::cout << "  " << name << ": " << std::fixed << std::setprecision(2) << r.mrays_per_sec << " Mrays/s\n";
  };

  const auto cpuFeatures = vkpt::cpu::QueryCpuFeatures();
  const auto bestMode    = vkpt::cpu::SelectBestSimdMode(cpuFeatures);
  std::cout << "simd-sweep: " << rayCount << " rays x " << triCount << " triangles\n";
  std::cout << "cpu: " << cpuFeatures.architecture << ", best_mode=" << vkpt::cpu::SimdModeName(bestMode) << "\n";

  bench_mode("scalar", true,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_scalar(p, t, h);
    });

#if defined(__ARM_NEON)
  bench_mode("neon", cpuFeatures.neon,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_neon(p, t, h);
    });
#else
  results.push_back({"neon", 0, 0.0, false, "skipped", "not compiled for ARM NEON", 0.0});
#endif

#if defined(__ARM_FEATURE_SVE)
  bench_mode("sve", cpuFeatures.sve,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_sve(p, t, h);
    });
#else
  results.push_back({"sve", 0, 0.0, false, "skipped", "not compiled for ARM SVE", 0.0});
#endif

#if defined(__AVX2__)
  bench_mode("avx2", cpuFeatures.avx2,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx2_full(p, t, h);
    });
#else
  results.push_back({"avx2", 0, 0.0, false, "skipped", "not compiled for AVX2", 0.0});
#endif

#if defined(__AVX512F__)
  bench_mode("avx512", cpuFeatures.avx512f,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx512(p, t, h);
    });
#else
  results.push_back({"avx512", 0, 0.0, false, "skipped", "not compiled for AVX-512", 0.0});
#endif

  // Find best available
  std::string best_name = "scalar";
  double best_mrays = 0.0;
  double scalar_mrays = 0.0;
  for (const auto& r : results) {
    if (r.available && r.mode_name == "scalar") {
      scalar_mrays = std::max(1.0e-9, r.mrays_per_sec);
    }
  }
  for (auto& r : results) {
    if (r.available && scalar_mrays > 0.0) {
      r.speedup_vs_scalar = r.mrays_per_sec / scalar_mrays;
    }
  }
  for (const auto& r : results) {
    if (r.available && r.mrays_per_sec > best_mrays) {
      best_mrays = r.mrays_per_sec;
      best_name = r.mode_name;
    }
  }
  std::cout << "best: " << best_name << " (" << std::fixed << std::setprecision(2) << best_mrays << " Mrays/s)\n";

  // Write JSON output
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "simd_sweep.json";
  std::ofstream jf(outPath);
  if (jf.is_open()) {
    jf << "{\n";
    jf << "  \"architecture\":\"" << cpuFeatures.architecture << "\",\n";
    jf << "  \"best_mode\":\"" << best_name << "\",\n";
    jf << "  \"results\":[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      jf << "    {\"mode\":\"" << r.mode_name << "\",\"available\":" << (r.available ? "true" : "false")
         << ",\"status\":\"" << EscapeJson(r.status) << "\""
         << ",\"skip_reason\":\"" << EscapeJson(r.skip_reason) << "\""
         << ",\"mrays_per_sec\":" << std::fixed << std::setprecision(4) << r.mrays_per_sec
         << ",\"speedup_vs_scalar\":" << std::fixed << std::setprecision(4) << r.speedup_vs_scalar << "}";
      if (i + 1 < results.size()) jf << ",";
      jf << "\n";
    }
    jf << "  ]\n}\n";
    std::cout << "results: " << outPath.string() << "\n";
  }
  return 0;
}

// F10: Tile-size sweep — runs cpu-tiled with different tile heights and measures throughput.
int TileSweepCommand(const std::vector<std::string_view>& args) {
  std::string scene;
  uint32_t workers = 0;
  uint32_t spp = 4;
  std::string resolution = "128x128";
  std::string output = "artifacts/tile_sweep";

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene" && i + 1 < args.size()) {
      scene = std::string(args[++i]);
    } else if (args[i] == "--workers" && i + 1 < args.size()) {
      workers = static_cast<uint32_t>(std::stoul(std::string(args[++i])));
    } else if (args[i] == "--spp" && i + 1 < args.size()) {
      spp = static_cast<uint32_t>(std::stoul(std::string(args[++i])));
    } else if (args[i] == "--resolution" && i + 1 < args.size()) {
      resolution = std::string(args[++i]);
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scene.empty()) {
    std::cerr << "tile-sweep requires --scene\n";
    return 1;
  }

  const uint32_t kTileSizes[] = {8, 16, 32, 64};
  struct TileResult {
    uint32_t tile_height;
    double samples_per_sec;
    double normalized_score;
    double render_ms;
    bool ok;
    std::string status;
    std::string reason;
  };
  std::vector<TileResult> results;

  std::cout << "tile-sweep: " << scene << " workers=" << workers << " spp=" << spp << "\n";

  for (const uint32_t tile_h : kTileSizes) {
    const Path tileOut = Path(output) / (std::string("tile") + std::to_string(tile_h));
    const std::vector<std::string> call_args = {
      "ptbench", "run",
      "--scene", scene,
      "--backend", "cpu",
      "--renderer-path", "cpu-tiled",
      "--resolution", resolution,
      "--spp", std::to_string(spp),
      "--workers", std::to_string(workers),
      "--tile-size", std::to_string(tile_h),
      "--output", tileOut.string()
    };
    std::vector<std::string_view> cargs;
    for (const auto& a : call_args) cargs.emplace_back(a);

    const auto t0 = std::chrono::high_resolution_clock::now();
    const int rc = RunCommand(cargs);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    TileResult tr{};
    tr.tile_height = tile_h;
    tr.ok = (rc == 0);
    tr.status = tr.ok ? "ok" : "failed";
    tr.reason = tr.ok ? "" : "benchmark run failed";
    tr.render_ms = elapsed_ms;

    if (tr.ok) {
      const Path res_path = tileOut / "results.json";
      const auto parsed = vkpt::benchmark::LoadBenchmarkResultFromFile(res_path.string());
      if (parsed) {
        tr.samples_per_sec = parsed.value().throughput.samples_per_sec;
        tr.normalized_score = parsed.value().score.normalized_score;
      } else {
        tr.ok = false;
        tr.status = "failed";
        tr.reason = "results.json failed schema validation";
      }
    }
    results.push_back(tr);
    std::cout << "  tile_height=" << tile_h << ": ";
    if (tr.ok) {
      std::cout << std::fixed << std::setprecision(2) << tr.samples_per_sec / 1e6 << " Msamples/s";
    } else {
      std::cout << "FAILED";
    }
    std::cout << "\n";
  }

  // Find best
  uint32_t best_tile = 16;
  double best_sps = 0.0;
  for (const auto& r : results) {
    if (r.ok && r.samples_per_sec > best_sps) {
      best_sps = r.samples_per_sec;
      best_tile = r.tile_height;
    }
  }
  std::cout << "best tile_height: " << best_tile << " (" << std::fixed << std::setprecision(2) << best_sps / 1e6 << " Msamples/s)\n";

  // Write JSON
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "tile_sweep.json";
  std::ofstream jf(outPath);
  if (jf.is_open()) {
    jf << "{\n";
    jf << "  \"scene\":\"" << EscapeJson(scene) << "\",\n";
    jf << "  \"best_tile_height\":" << best_tile << ",\n";
    jf << "  \"results\":[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      jf << "    {\"tile_height\":" << r.tile_height
         << ",\"ok\":" << (r.ok ? "true" : "false")
         << ",\"status\":\"" << EscapeJson(r.status) << "\""
         << ",\"reason\":\"" << EscapeJson(r.reason) << "\""
         << ",\"tile_shape\":\"full-width-row-band\""
         << ",\"samples_per_sec\":" << std::fixed << std::setprecision(2) << r.samples_per_sec
         << ",\"normalized_score\":" << std::fixed << std::setprecision(2) << r.normalized_score
         << ",\"render_ms\":" << std::fixed << std::setprecision(2) << r.render_ms
         << "}";
      if (i + 1 < results.size()) jf << ",";
      jf << "\n";
    }
    jf << "  ]\n}\n";
    std::cout << "results: " << outPath.string() << "\n";
  }
  return 0;
}

bool BackendRegistered(std::string_view name) {
  const auto normalized = vkpt::render::NormalizeBackendName(name);
  const auto names = vkpt::render::AvailableBackendNames();
  return std::find(names.begin(), names.end(), normalized) != names.end();
}

int BackendExperimentsCommand(const std::vector<std::string_view>& args) {
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output value\n";
        return 1;
      }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }

  struct Target {
    std::string experiment;
    std::string backend;
    std::string renderer_path;
    bool requires_rt = false;
    bool requires_render_path = true;
  };
  const std::vector<Target> targets = {
      {"vulkan_compute_vs_rt", "vulkan", "gpu-compute", false, true},
      {"vulkan_compute_vs_rt", "vulkan", "gpu-rt", true, true},
      {"d3d12_compute_vs_dxr", "d3d12", "d3d12-compute", false, true},
      {"d3d12_compute_vs_dxr", "d3d12-dxr", "dxr", true, true},
      {"metal_compute_vs_rt", "metal", "metal-compute", false, true},
      {"metal_compute_vs_rt", "metal-rt", "metal-rt", true, true},
      {"webgpu_workgroup_sweep", "webgpu", "webgpu-compute", false, true},
  };

  struct Row {
    std::string experiment;
    std::string backend;
    std::string renderer_path;
    std::string status;
    std::string reason;
    std::string capability_summary;
  };
  std::vector<Row> rows;
  rows.reserve(targets.size());

  for (const auto& target : targets) {
    Row row;
    row.experiment = target.experiment;
    row.backend = target.backend;
    row.renderer_path = target.renderer_path;
    if (!BackendRegistered(target.backend)) {
      row.status = "skipped";
      row.reason = "backend is not registered in this build";
      rows.push_back(std::move(row));
      continue;
    }
    auto backend = vkpt::render::CreateBackend(target.backend);
    if (!backend || !backend->initialize()) {
      row.status = "skipped";
      row.reason = backend ? backend->last_error() : "backend creation failed";
      rows.push_back(std::move(row));
      continue;
    }
    const auto caps = backend->capabilities();
    row.capability_summary = vkpt::render::SerializeBackendCapabilities(caps);
    if (target.requires_rt && !caps.ray_tracing) {
      row.status = "skipped";
      row.reason = "ray tracing capability is not exposed by this backend";
    } else if (target.renderer_path != "gpu-compute") {
      row.status = "skipped";
      row.reason = "backend adapter exists, but ptbench render path is not wired yet";
    } else {
      row.status = "available";
      row.reason = caps.is_simulated ? "simulated compute backend available" : "compute backend available";
    }
    rows.push_back(std::move(row));
  }

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "backend_experiments.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"experiment\":\"" << EscapeJson(row.experiment) << "\","
          << "\"backend\":\"" << EscapeJson(row.backend) << "\","
          << "\"renderer_path\":\"" << EscapeJson(row.renderer_path) << "\","
          << "\"status\":\"" << EscapeJson(row.status) << "\","
          << "\"reason\":\"" << EscapeJson(row.reason) << "\","
          << "\"capabilities\":" << (row.capability_summary.empty() ? "null" : row.capability_summary) << "}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return 0;
}

int GpuMemPressureCommand(const std::vector<std::string_view>& args) {
  std::size_t max_mb = 512;
  std::size_t step_mb = 64;
  std::string backendName = "auto";
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--max-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --max-mb value\n"; return 1; }
      max_mb = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
    } else if (args[i] == "--step-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --step-mb value\n"; return 1; }
      step_mb = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
    } else if (args[i] == "--backend") {
      if (i + 1 >= args.size()) { std::cerr << "missing --backend value\n"; return 1; }
      backendName = std::string(args[++i]);
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (step_mb == 0) step_mb = 1;
  if (max_mb < step_mb) max_mb = step_mb;

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "gpu_mem_pressure.json";

  std::vector<std::vector<std::uint8_t>> allocations;
  allocations.reserve(max_mb / step_mb + 1);
  std::size_t allocated_mb = 0;
  bool ok = true;
  std::string fail_reason;

  for (std::size_t target_mb = step_mb; target_mb <= max_mb; target_mb += step_mb) {
    try {
      allocations.emplace_back(step_mb * 1024ull * 1024ull, 0u);
      allocated_mb = target_mb;
      std::cout << "allocated_mb=" << allocated_mb << "\n";
    } catch (const std::bad_alloc&) {
      ok = false;
      fail_reason = "std::bad_alloc";
      break;
    } catch (const std::exception& ex) {
      ok = false;
      fail_reason = ex.what();
      break;
    }
  }

  std::string backendCaps = "null";
  std::string backendStatus = "skipped";
  std::string backendReason = "backend unavailable";
  if (auto backend = vkpt::render::CreateBackend(backendName)) {
    if (backend->initialize()) {
      backendCaps = vkpt::render::SerializeBackendCapabilities(backend->capabilities());
      backendStatus = "available";
      backendReason = backend->capabilities().is_simulated ? "simulated backend; host allocation pressure used" :
                                                            "backend available; API memory budget not exposed";
    } else {
      backendReason = backend->last_error();
    }
  }

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"backend\": \"" << EscapeJson(backendName) << "\",\n";
    out << "  \"backend_status\": \"" << EscapeJson(backendStatus) << "\",\n";
    out << "  \"backend_reason\": \"" << EscapeJson(backendReason) << "\",\n";
    out << "  \"backend_capabilities\": " << backendCaps << ",\n";
    out << "  \"allocated_mb\": " << allocated_mb << ",\n";
    out << "  \"max_mb\": " << max_mb << ",\n";
    out << "  \"step_mb\": " << step_mb << ",\n";
    out << "  \"fail_reason\": \"" << EscapeJson(fail_reason) << "\",\n";
    out << "  \"stress_rows\": [\n";
    const std::vector<std::string> stress = {"texture_size_stress", "bvh_size_stress", "readback_pressure", "upload_pressure"};
    for (std::size_t i = 0; i < stress.size(); ++i) {
      out << "    {\"name\":\"" << stress[i] << "\",\"status\":\""
          << (ok ? "ok" : "failed") << "\",\"mode\":\"simulated-host-allocation\","
          << "\"allocated_mb\":" << allocated_mb << ",\"fallback_decision\":\""
          << (ok ? "continue" : "fail with diagnostics") << "\"}";
      if (i + 1 < stress.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return ok ? 0 : 2;
}

int ShaderMatrixCommand(const std::vector<std::string_view>& args) {
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "shader_matrix.json";

  struct MatrixTarget {
    std::string backend;
    std::string shader_family;
    std::string variant;
    std::vector<std::string> defines;
    std::string required_feature;
    bool cpu_validation = false;
  };
  const std::vector<MatrixTarget> targets = {
      {"vulkan", "pathtrace", "compute_minimum", {}, "compute", false},
      {"vulkan", "pathtrace", "vulkan_rt_optional", {"RT=1"}, "ray-tracing", false},
      {"d3d12", "pathtrace", "d3d12_compute", {}, "compute", false},
      {"d3d12-dxr", "pathtrace", "d3d12_dxr_optional", {"DXR=1"}, "ray-tracing", false},
      {"metal", "pathtrace", "metal_compute", {}, "compute", false},
      {"metal-rt", "pathtrace", "metal_rt_optional", {"RT=1"}, "ray-tracing", false},
      {"webgpu", "pathtrace", "webgpu_wgsl", {"WGSL=1"}, "compute", false},
      {"cpu", "materials", "cpu_material_validation", {}, "", true},
  };

  struct Row {
    std::string backend;
    std::string shader_family;
    std::string variant;
    std::string status;
    std::string diagnostics;
    std::string artifact;
  };
  std::vector<Row> rows;
  bool failed = false;

  for (const auto& target : targets) {
    if (target.cpu_validation) {
      const std::string coverageArtifact = (Path(output) / "material_coverage.json").string();
      std::vector<std::string> coverageArgs = {"ptbench", "material-coverage", "--output", coverageArtifact};
      std::vector<std::string_view> coverageViews;
      for (const auto& item : coverageArgs) {
        coverageViews.emplace_back(item);
      }
      const int coverageRc = MaterialCoverageCommand(coverageViews);
      if (coverageRc != 0) {
        failed = true;
      }
      rows.push_back({target.backend,
                      target.shader_family,
                      target.variant,
                      coverageRc == 0 ? "ok" : "failed",
                      coverageRc == 0 ? "all material families have runtime model/effect coverage"
                                      : "material coverage command failed",
                      coverageArtifact});
      continue;
    }
    if (!BackendRegistered(target.backend)) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped", "backend is not registered in this build", ""});
      continue;
    }
    auto backend = vkpt::render::CreateBackend(target.backend);
    if (!backend || !backend->initialize()) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped",
                      backend ? backend->last_error() : "create_backend_failed", ""});
      continue;
    }
    auto* compiler = backend->compiler();
    if (!compiler) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped", "no compiler", ""});
      continue;
    }
    if (!target.required_feature.empty() && !compiler->supports_feature(target.required_feature)) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped",
                      "compiler does not support feature: " + target.required_feature, ""});
      continue;
    }

    vkpt::render::ComputePipelineDesc desc;
    desc.source_path = "shaders/ptbench_matrix.comp";
    desc.entry_point = "main";
    desc.debug_label = "ptbench_matrix";
    desc.defines = target.defines;

    std::string artifact;
    std::string diag;
    const bool ok = compiler->compile_compute_shader(desc, artifact, &diag);
    if (!ok) {
      failed = true;
    }
    rows.push_back({target.backend, target.shader_family, target.variant, ok ? "ok" : "failed", diag, artifact});
  }

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      out << "    {\"backend\":\"" << EscapeJson(r.backend) << "\","
          << "\"shader_family\":\"" << EscapeJson(r.shader_family) << "\","
          << "\"variant\":\"" << EscapeJson(r.variant) << "\","
          << "\"status\":\"" << EscapeJson(r.status) << "\","
          << "\"artifact\":\"" << EscapeJson(r.artifact) << "\","
          << "\"diagnostics\":\"" << EscapeJson(r.diagnostics) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return failed ? 2 : 0;
}

int ReleaseCheckCommand(const std::vector<std::string_view>& args) {
  std::string scenePack = "assets/scenes";
  std::string output = "artifacts/release_check";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene-pack") {
      if (i + 1 >= args.size()) { std::cerr << "missing --scene-pack value\n"; return 1; }
      scenePack = std::string(args[++i]);
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  EnsureDirectory(Path(output));

  std::size_t scene_ok = 0;
  std::size_t scene_fail = 0;
  for (const auto& entry : std::filesystem::directory_iterator(Path(scenePack))) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    const auto scenePath = entry.path().string();
    std::vector<std::string_view> v = {"ptbench", "validate-scene", "--scene", scenePath};
    const int rc = ValidateSceneCommand(v);
    if (rc == 0) ++scene_ok; else ++scene_fail;
  }

  const Path runOut = Path(output) / "cpu_scalar_cornell";
  const std::string runOutStr = runOut.string();
  std::vector<std::string_view> runArgs = {
    "ptbench", "run",
    "--scene", "assets/scenes/cornell_native.json",
    "--backend", "cpu",
    "--renderer-path", "cpu-scalar",
    "--resolution", "128x128",
    "--spp", "2",
    "--output", runOutStr
  };
  const int runRc = RunCommand(runArgs);
  const auto artifactValidation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(runOut.string());

  const Path threadedOut = Path(output) / "thread_sweep";
  const std::string threadedOutStr = threadedOut.string();
  std::vector<std::string_view> threadArgs = {
    "ptbench", "thread-sweep",
    "--scene", "assets/scenes/cornell_native.json",
    "--workers", "1,2",
    "--resolution", "64x64",
    "--spp", "1",
    "--output", threadedOutStr
  };
  const int threadRc = ThreadSweepCommand(threadArgs);

  const Path backendOut = Path(output) / "backend_experiments";
  const std::string backendOutStr = backendOut.string();
  std::vector<std::string_view> backendArgs = {"ptbench", "backend-experiments", "--output", backendOutStr};
  const int backendRc = BackendExperimentsCommand(backendArgs);

  const Path outPath = Path(output) / "release_check.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"scene_validate_ok\": " << scene_ok << ",\n";
    out << "  \"scene_validate_fail\": " << scene_fail << ",\n";
    out << "  \"cpu_scalar_run_rc\": " << runRc << ",\n";
    out << "  \"artifact_contract_ok\": " << (artifactValidation.ok ? "true" : "false") << ",\n";
    out << "  \"thread_sweep_rc\": " << threadRc << ",\n";
    out << "  \"backend_experiments_rc\": " << backendRc << ",\n";
    out << "  \"checklist\": [\n";
    out << "    {\"name\":\"scene pack validates\",\"status\":\"" << (scene_fail == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"CPU scalar render passes\",\"status\":\"" << (runRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"benchmark artifacts validate\",\"status\":\"" << (artifactValidation.ok ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"CPU threaded render passes\",\"status\":\"" << (threadRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"backend experiments skip unavailable paths cleanly\",\"status\":\"" << (backendRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"UI G70 release checks\",\"status\":\"external\",\"reason\":\"covered by release_gate_check script UI smoke step when Qt smoke tool is present\"}\n";
    out << "  ]\n";
    out << "}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return (scene_fail == 0 && runRc == 0 && artifactValidation.ok && threadRc == 0 && backendRc == 0) ? 0 : 2;
}

int RunExperimentsCommand(const std::vector<std::string_view>& args) {
  std::string scenePack = "core";
  std::vector<std::string> includes;
  std::string output = "artifacts/benchmarks/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene-pack") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --scene-pack value\n";
        return 1;
      }
      scenePack = std::string(args[++i]);
    } else if (args[i] == "--include") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --include value\n";
        return 1;
      }
      includes.push_back(std::string(args[++i]));
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output value\n";
        return 1;
      }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  auto lower = [](const std::string& value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered;
  };
  bool hasCpu = includes.empty();
  bool hasGpu = false;
  for (auto& include : includes) {
    const auto lowered = lower(include);
    if (lowered == "cpu" || lowered == "cpu-scalar" || lowered == "scalar") {
      hasCpu = true;
    }
    if (lowered == "gpu" || lowered == "gpu-compute" || lowered == "gpu-paths" || lowered == "vulkan") {
      hasGpu = true;
    }
  }
  if (!hasCpu && !hasGpu) {
    hasCpu = true;
  }

  Path inputDir = (scenePack == "core") ? Path("assets/scenes") : Path(scenePack);
  if (!std::filesystem::exists(inputDir)) {
    std::cerr << "missing scene pack path: " << inputDir.string() << "\n";
    return 2;
  }
  if (scenePack.empty()) {
    std::cerr << "missing --scene-pack\n";
    return 1;
  }

  std::vector<Path> scenes;
  for (const auto& entry : std::filesystem::directory_iterator(inputDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    scenes.push_back(entry.path());
  }
  if (scenes.empty()) {
    std::cerr << "no scenes in pack: " << scenePack << "\n";
    return 2;
  }

  std::vector<std::pair<std::string, std::string>> runs;
  if (hasCpu) {
    runs.push_back({"cpu", "cpu-scalar"});
  }
  if (hasGpu) {
    runs.push_back({"vulkan", "gpu-compute"});
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const auto& scene : scenes) {
    for (const auto& run : runs) {
      std::vector<std::string> sceneArgs = {"ptbench", "run", "--scene", scene.string(), "--backend", run.first,
                                           "--renderer-path", run.second, "--resolution", "128x128", "--spp", "2", "--output",
                                           (Path(output) / scene.stem() / run.second).string()};
      std::vector<std::string_view> cargs;
      for (const auto& item : sceneArgs) {
        cargs.emplace_back(item);
      }
      const int rc = RunCommand(cargs);
      if (rc == 0) {
        ++passed;
      } else {
        ++failed;
      }
    }
  }
  std::cout << "run-experiments summary: passed=" << passed << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintHelp();
    return 1;
  }

  const std::vector<std::string_view> args(argv, argv + argc);
  const auto command = args[1];

  if (command == "--help" || command == "-h") {
    PrintHelp();
    return 0;
  }
  if (command == "run") {
    return RunCommand(args);
  }
  if (command == "echo-desc") {
    return EchoDescCommand(args);
  }
  if (command == "list-scenes") {
    return ListScenesCommand();
  }
  if (command == "list-backends") {
    return ListBackendsCommand();
  }
  if (command == "list-renderer-paths") {
    return ListRendererPathsCommand(args);
  }
  if (command == "validate-scene") {
    return ValidateSceneCommand(args);
  }
  if (command == "validate-artifacts") {
    return ValidateArtifactsCommand(args);
  }
  if (command == "compare") {
    return CompareCommand(args);
  }
  if (command == "dump-capabilities") {
    return DumpCapabilitiesCommand();
  }
  if (command == "run-experiments") {
    return RunExperimentsCommand(args);
  }
  if (command == "backend-experiments") {
    return BackendExperimentsCommand(args);
  }
  if (command == "gpu-mem-pressure") {
    return GpuMemPressureCommand(args);
  }
  if (command == "material-coverage") {
    return MaterialCoverageCommand(args);
  }
  if (command == "shader-matrix") {
    return ShaderMatrixCommand(args);
  }
  if (command == "release-check") {
    return ReleaseCheckCommand(args);
  }
  if (command == "thread-sweep") {
    return ThreadSweepCommand(args);
  }
  if (command == "simd-sweep") {
    return SimdSweepCommand(args);
  }
  if (command == "tile-sweep") {
    return TileSweepCommand(args);
  }

  std::cerr << "unknown command: " << command << "\n";
  PrintHelp();
  return 1;
}
