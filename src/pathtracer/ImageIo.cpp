#include "pathtracer/ImageIo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace vkpt::pathtracer {

namespace {

uint32_t crc32_for_byte(uint32_t r) {
  for (int j = 0; j < 8; ++j) {
    r = (r & 1u) ? (0xEDB88320u ^ (r >> 1)) : (r >> 1);
  }
  return r;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, std::size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256u> values{};
    for (uint32_t i = 0; i < 256; ++i) {
      values[static_cast<std::size_t>(i)] = crc32_for_byte(i);
    }
    return values;
  }();

  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc;
}

uint32_t crc32_chunk(std::string_view type, const std::vector<uint8_t>& data) {
  uint32_t crc = 0xffffffffu;
  crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(type.data()), type.size());
  crc = crc32_update(crc, data.data(), data.size());
  return crc ^ 0xffffffffu;
}

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  constexpr uint32_t kMod = 65521u;
  constexpr std::size_t kMaxBlock = 5552u;
  uint32_t a = 1u;
  uint32_t b = 0u;
  std::size_t offset = 0u;
  while (offset < bytes.size()) {
    const std::size_t block_end = std::min(offset + kMaxBlock, bytes.size());
    for (; offset < block_end; ++offset) {
      a += bytes[offset];
      b += a;
    }
    a %= kMod;
    b %= kMod;
  }
  return (b << 16) | a;
}

void write_u16_le(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void write_u32_be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void append_chunk(std::vector<uint8_t>& out, std::string_view type, const std::vector<uint8_t>& data) {
  write_u32_be(out, static_cast<uint32_t>(data.size()));
  out.insert(out.end(), type.begin(), type.end());
  out.insert(out.end(), data.begin(), data.end());
  write_u32_be(out, crc32_chunk(type, data));
}

bool checked_mul_size(std::size_t lhs, std::size_t rhs, std::size_t& out) {
  if (lhs != 0u && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

bool checked_image_sizes(uint32_t width,
                         uint32_t height,
                         std::size_t bytes_per_pixel,
                         std::size_t& pixels,
                         std::size_t& bytes) {
  pixels = 0u;
  bytes = 0u;
  return checked_mul_size(static_cast<std::size_t>(width), static_cast<std::size_t>(height), pixels) &&
         checked_mul_size(pixels, bytes_per_pixel, bytes);
}

std::vector<uint8_t> encode_deflate_stored(const std::vector<uint8_t>& raw) {
  // Compatibility PNG writer uses zlib stored blocks, avoiding a dependency on a compression library.
  std::vector<uint8_t> out;
  const std::size_t block_count = raw.empty() ? 0u : ((raw.size() + 0xfffeu) / 0xffffu);
  out.reserve(2u + block_count * 5u + raw.size() + 4u);
  out.push_back(0x78);
  out.push_back(0x01);

  std::size_t offset = 0;
  while (offset < raw.size()) {
    const uint16_t len = static_cast<uint16_t>(std::min(raw.size() - offset, static_cast<std::size_t>(0xffffu)));
    const uint16_t nlen = static_cast<uint16_t>(~len);
    out.push_back(static_cast<uint8_t>((offset + len >= raw.size()) ? 1 : 0));
    write_u16_le(out, len);
    write_u16_le(out, nlen);
    out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
               raw.begin() + static_cast<std::ptrdiff_t>(offset + len));
    offset += len;
  }

  write_u32_be(out, adler32(raw));
  return out;
}

}  // namespace

bool SavePngCompat(const std::string& path, const FilmLdr& image, std::string* error) {
  if (image.width == 0 || image.height == 0 || image.rgba8.empty()) {
    if (error) *error = "empty image";
    return false;
  }
  std::size_t pixels = 0u;
  std::size_t rgba_bytes = 0u;
  std::size_t raw_bytes = 0u;
  if (!checked_image_sizes(image.width, image.height, 4u, pixels, rgba_bytes) ||
      static_cast<std::size_t>(image.height) > std::numeric_limits<std::size_t>::max() - rgba_bytes ||
      image.rgba8.size() < rgba_bytes) {
    if (error) *error = "image dimensions or data size are invalid";
    return false;
  }
  raw_bytes = rgba_bytes + static_cast<std::size_t>(image.height);
  if (raw_bytes > std::numeric_limits<uint32_t>::max()) {
    if (error) *error = "image too large for PNG compatibility writer";
    return false;
  }

  // PNG scanlines are filter byte + RGBA payload; filter 0 keeps output deterministic.
  std::vector<uint8_t> raw(raw_bytes);
  std::size_t writeOffset = 0u;
  std::size_t readOffset = 0u;
  const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4u;
  for (uint32_t y = 0; y < image.height; ++y) {
    raw[writeOffset++] = 0;
    std::copy_n(image.rgba8.data() + readOffset, rowBytes, raw.data() + writeOffset);
    readOffset += rowBytes;
    writeOffset += rowBytes;
  }
  const std::vector<uint8_t> compressed = encode_deflate_stored(raw);
  if (compressed.size() > std::numeric_limits<uint32_t>::max()) {
    if (error) *error = "compressed image too large for PNG compatibility writer";
    return false;
  }

  std::vector<uint8_t> ihdr;
  write_u32_be(ihdr, image.width);
  write_u32_be(ihdr, image.height);
  ihdr.push_back(8);
  ihdr.push_back(6);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);

  std::vector<uint8_t> png;
  png.reserve(8 + 12 + ihdr.size() + 12 + compressed.size() + 12);
  png.insert(png.end(), {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a});
  append_chunk(png, "IHDR", ihdr);
  append_chunk(png, "IDAT", compressed);
  append_chunk(png, "IEND", {});

  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) {
      *error = "failed to open output path";
    }
    return false;
  }
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(out);
}

bool SaveExrCompat(const std::string& path, const FilmHdr& image, std::string* error) {
  if (image.width == 0 || image.height == 0 || image.rgbf.empty()) {
    if (error) *error = "empty image";
    return false;
  }
  std::size_t pixels = 0u;
  std::size_t rgb_values = 0u;
  if (!checked_image_sizes(image.width, image.height, 3u, pixels, rgb_values) ||
      image.rgbf.size() < rgb_values) {
    if (error) *error = "image dimensions or data size are invalid";
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) {
      *error = "failed to open output path";
    }
    return false;
  }
  // Placeholder text output preserves HDR samples for tests until real OpenEXR writing is available.
  out << "# EXR-compatible placeholder written by vkpt path tracer.\n";
  out << image.width << " " << image.height << "\n";
  for (std::size_t i = 0; i < rgb_values; i += 3) {
    out << image.rgbf[i] << " " << image.rgbf[i + 1] << " " << image.rgbf[i + 2] << "\n";
  }
  return true;
}

}  // namespace vkpt::pathtracer
