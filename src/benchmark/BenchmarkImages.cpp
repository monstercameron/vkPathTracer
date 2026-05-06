#include "benchmark/BenchmarkRuntimeInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pathtracer/PathTracer.h"

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;
bool ParseTolerance(std::string_view text, TolerancePolicy& policy, std::string* error) {
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

bool LoadImage(const Path& path, ImageRgb& image, std::string* error) {
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
}  // namespace vkpt::benchmark::ptbench
