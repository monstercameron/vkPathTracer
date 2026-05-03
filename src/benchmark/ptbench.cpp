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
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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
#include "pathtracer/PathTracer.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/VulkanBackend.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"

namespace {

using Path = std::filesystem::path;

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

std::string ReadFile(const Path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
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
  return normalized.empty() ? "cpu" : normalized;
}

std::vector<std::string> AvailableRendererPaths(std::string_view backend) {
  const auto normalized = NormalizeBackend(backend);
  if (normalized == "vulkan" || normalized == "vulkan-compute") {
    return {"gpu-compute"};
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

uint16_t ReadU16BE(const std::vector<std::uint8_t>& data, std::size_t pos) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[pos]) << 8 | static_cast<uint16_t>(data[pos + 1]));
}

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

std::string HashVectorF32(const std::vector<float>& data) {
  if (data.empty()) {
    return Hex64(Fnv1a64(""));
  }
  return HashBytes(reinterpret_cast<const void*>(data.data()), data.size() * sizeof(float));
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
  out << "\"benchmark_enabled\":" << (scene.benchmark.enabled ? "true" : "false");
  out << "}";
  return out.str();
}

void PrintHelp() {
  std::cout << "ptbench <command> [options]\n\n";
  std::cout << "commands:\n";
  std::cout << "  run\n";
  std::cout << "  list-scenes\n";
  std::cout << "  list-backends\n";
  std::cout << "  list-renderer-paths\n";
  std::cout << "  validate-scene\n";
  std::cout << "  compare\n";
  std::cout << "  dump-capabilities\n";
  std::cout << "  run-experiments\n";
  std::cout << "  gpu-mem-pressure\n";
  std::cout << "  shader-matrix\n";
  std::cout << "  release-check\n";
  std::cout << "  simd-sweep\n";
  std::cout << "  tile-sweep\n\n";
  std::cout << "run:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  --backend <cpu|vulkan|auto>\n";
  std::cout << "  --renderer-path <cpu-scalar|cpu-tiled|gpu-compute>\n";
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

  std::cout << "shader-matrix:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "release-check:\n";
  std::cout << "  [--scene-pack <dir>]    (default assets/scenes)\n";
  std::cout << "  [--output <path>]       (default artifacts/release_check)\n";
}

bool WriteCsvValue(std::ofstream& out, double value) {
  if (!out.good()) {
    return false;
  }
  out << std::fixed << std::setprecision(6) << value;
  return true;
}

bool WriteRunArtifacts(const vkpt::benchmark::BenchmarkResult& result,
                      const Path& artifactDir,
                      const vkpt::scene::SceneDocument& scene,
                      const vkpt::pathtracer::RTSceneLayoutManifest& manifest,
                      const Path& referencePath,
                      const Path& diffHeatmapPath,
                      const bool includeReference,
                      std::string* error) {
  const Path resultsPath = artifactDir / "results.json";
  const Path resultsCsvPath = artifactDir / "results.csv";
  const Path metadataPath = artifactDir / "metadata.json";
  const Path snapshotPath = artifactDir / "scene_snapshot.json";
  const Path shaderManifestPath = artifactDir / "shader_manifest.json";
  const Path assetManifestPath = artifactDir / "asset_manifest.json";
  const Path logsPath = artifactDir / "logs.jsonl";
  const Path required[] = {resultsPath, resultsCsvPath, metadataPath, snapshotPath, shaderManifestPath, assetManifestPath, logsPath};
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
         "cpu_ms,paths_per_sec,samples_per_sec,reference_error,image_hash\n";
  csv << EscapeJson(result.run_id) << "," << EscapeJson(result.scene) << ","
      << EscapeJson(result.backend) << "," << EscapeJson(result.renderer_path) << ","
      << result.resolution.width << "," << result.resolution.height << "," << result.spp << "," << result.max_depth << ","
      << std::fixed << std::setprecision(6) << result.timing.total_ms << "," << result.timing.build_ms << ","
      << result.timing.render_ms << "," << result.timing.cpu_ms << "," << result.throughput.paths_per_sec << ","
      << result.throughput.samples_per_sec << "," << result.reference_error << ","
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

  if (!std::filesystem::exists(resultsPath) || !std::filesystem::exists(resultsCsvPath) ||
      !std::filesystem::exists(metadataPath) || !std::filesystem::exists(snapshotPath) ||
      !std::filesystem::exists(shaderManifestPath) || !std::filesystem::exists(assetManifestPath)) {
    if (error) {
      *error = "required artifacts not created";
    }
    return false;
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
};

bool ParseRunArgs(const std::vector<std::string_view>& args, RunOptions& out, std::string* error = nullptr) {
  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--scene") {
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
  const bool isVulkanPath = (std::string(ToLower(opts.rendererPath)) == "gpu-compute");
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
  result.diff_heatmap_png = (artifactDir / "diff_heatmap.png").string();

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

  const auto buildStart = std::chrono::high_resolution_clock::now();
  auto sceneData = vkpt::pathtracer::BuildSceneDataFromDocument(scene);
  if (!sceneData) {
    std::cerr << "failed to build render scene data\n";
    return 2;
  }
  auto buildEnd = std::chrono::high_resolution_clock::now();

  vkpt::pathtracer::RenderSettings renderSettings;
  renderSettings.width = opts.width;
  renderSettings.height = opts.height;
  renderSettings.spp = opts.spp;
  renderSettings.max_depth = opts.maxDepth;
  renderSettings.seed = opts.seed;

  std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
  if (isTiledPath) {
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
  tracer->reset_accumulation();
  const auto renderStart = std::chrono::high_resolution_clock::now();
  for (std::uint32_t sample = 0; sample < opts.spp; ++sample) {
    if (!tracer->render_sample_batch(0, opts.height, sample, 0)) {
      std::cerr << "render failed\n";
      return 2;
    }
  }
  const auto renderEnd = std::chrono::high_resolution_clock::now();
  const auto resolveStart = std::chrono::high_resolution_clock::now();

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
                           {
                               stats.diff.data(), stats.diff.data(),
                           },
                           &heatmapErr)) {
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

  if (!WriteRunArtifacts(result, artifactDir, scene, manifestResult.value(), Path(opts.referenceImage), Path(result.diff_heatmap_png),
                        !opts.referenceImage.empty(), &runError)) {
    std::cerr << runError << "\n";
    return 2;
  }

  std::ofstream logFile(artifactDir / "logs.jsonl");
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
            << "{\"k\":\"total_ms\",\"v\":\"" << std::fixed << std::setprecision(6) << result.timing.total_ms << "\"}"
            << "]"
            << "}\n";
  }
  logFile.close();

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
  const std::size_t entityCount = result.value().snapshot().entity_ids.size();
  const std::size_t cameraCount = result.value().snapshot().camera ? 1u : 0u;
  const bool lights = !result.value().snapshot().lights.empty();
  const bool materials = !result.value().materials.empty();
  const bool benchmarkSettings = true;
  if (!validBackend && !backendError.empty()) {
    issues.push_back(backendError);
  }
  if (result.value().export_hash_hex().empty()) {
    issues.push_back("missing scene hash");
  }
  if (entityCount == 0) {
    issues.push_back("no entities");
  }

  const bool ok = validScene && validBackend && benchmarkSettings;
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
  std::cout << "  ]\n";
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
    const std::size_t pixelCount = static_cast<std::size_t>(reference.width) * reference.height * 3u;
    const std::vector<float> diffs(candidate.width * candidate.height * 3u, 0.0f);
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
  };

  std::vector<SweepResult> results;

  auto bench_mode = [&](const char* name, bool available, auto fn) {
    SweepResult r;
    r.mode_name = name;
    r.available = available;
    if (!available) { results.push_back(r); return; }
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
  results.push_back({"neon", 0, 0.0, false});
#endif

#if defined(__ARM_FEATURE_SVE)
  bench_mode("sve", cpuFeatures.sve,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_sve(p, t, h);
    });
#else
  results.push_back({"sve", 0, 0.0, false});
#endif

#if defined(__AVX2__)
  bench_mode("avx2", cpuFeatures.avx2,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx2_full(p, t, h);
    });
#else
  results.push_back({"avx2", 0, 0.0, false});
#endif

#if defined(__AVX512F__)
  bench_mode("avx512", cpuFeatures.avx512f,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx512(p, t, h);
    });
#else
  results.push_back({"avx512", 0, 0.0, false});
#endif

  // Find best available
  std::string best_name = "scalar";
  double best_mrays = 0.0;
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
         << ",\"mrays_per_sec\":" << std::fixed << std::setprecision(4) << r.mrays_per_sec << "}";
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
    double render_ms;
    bool ok;
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
    tr.render_ms = elapsed_ms;

    if (tr.ok) {
      // Read back samples_per_sec from results.json
      const Path res_path = tileOut / "results.json";
      std::ifstream rf(res_path);
      std::string content((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
      // Quick parse: find "samples_per_sec":<number>
      const auto pos = content.find("\"samples_per_sec\":");
      if (pos != std::string::npos) {
        tr.samples_per_sec = std::stod(content.substr(pos + 18));
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
         << ",\"samples_per_sec\":" << std::fixed << std::setprecision(2) << r.samples_per_sec
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

int GpuMemPressureCommand(const std::vector<std::string_view>& args) {
  std::size_t max_mb = 512;
  std::size_t step_mb = 64;
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--max-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --max-mb value\n"; return 1; }
      max_mb = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
    } else if (args[i] == "--step-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --step-mb value\n"; return 1; }
      step_mb = static_cast<std::size_t>(std::stoull(std::string(args[++i])));
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
      allocations.emplace_back(target_mb * 1024ull * 1024ull, 0u);
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

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"allocated_mb\": " << allocated_mb << ",\n";
    out << "  \"max_mb\": " << max_mb << ",\n";
    out << "  \"step_mb\": " << step_mb << ",\n";
    out << "  \"fail_reason\": \"" << EscapeJson(fail_reason) << "\"\n";
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

  struct Row { std::string backend; std::string variant; bool ok; std::string diagnostics; };
  std::vector<Row> rows;

  for (const auto& name : vkpt::render::AvailableBackendNames()) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend || !backend->initialize()) {
      rows.push_back({name, "init", false, backend ? backend->last_error() : "create_backend_failed"});
      continue;
    }
    auto* compiler = backend->compiler();
    if (!compiler) {
      rows.push_back({name, "compiler", false, "no compiler"});
      continue;
    }

    const std::vector<std::vector<std::string>> defineSets = {
      {},
      {"DEBUG_VIEW=1"},
      {"MATERIAL_PACK=1"},
      {"MATERIAL_PACK=2", "EXPERIMENTAL=1"},
      {"RT=0", "SUBGROUPS=1"},
    };

    for (std::size_t i = 0; i < defineSets.size(); ++i) {
      vkpt::render::ComputePipelineDesc desc;
      desc.source_path = "shaders/ptbench_matrix.comp";
      desc.entry_point = "main";
      desc.debug_label = "ptbench_matrix";
      for (const auto& d : defineSets[i]) desc.defines.push_back(d);

      std::string artifact;
      std::string diag;
      const bool ok = compiler->compile_compute_shader(desc, artifact, &diag);
      rows.push_back({name, "variant_" + std::to_string(i), ok, ok ? artifact : diag});
    }
  }

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      out << "    {\"backend\":\"" << EscapeJson(r.backend) << "\","
          << "\"variant\":\"" << EscapeJson(r.variant) << "\","
          << "\"ok\":" << (r.ok ? "true" : "false") << ","
          << "\"diagnostics\":\"" << EscapeJson(r.diagnostics) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return 0;
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

  const Path outPath = Path(output) / "release_check.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"scene_validate_ok\": " << scene_ok << ",\n";
    out << "  \"scene_validate_fail\": " << scene_fail << ",\n";
    out << "  \"cpu_scalar_run_rc\": " << runRc << "\n";
    out << "}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return (scene_fail == 0 && runRc == 0) ? 0 : 2;
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

  Path inputDir("assets/scenes");
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
      const auto argv = std::vector<std::string_view>(sceneArgs.begin(), sceneArgs.end());
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
  if (command == "compare") {
    return CompareCommand(args);
  }
  if (command == "dump-capabilities") {
    return DumpCapabilitiesCommand();
  }
  if (command == "run-experiments") {
    return RunExperimentsCommand(args);
  }
  if (command == "gpu-mem-pressure") {
    return GpuMemPressureCommand(args);
  }
  if (command == "shader-matrix") {
    return ShaderMatrixCommand(args);
  }
  if (command == "release-check") {
    return ReleaseCheckCommand(args);
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
