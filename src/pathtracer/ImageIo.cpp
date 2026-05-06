#include "pathtracer/ImageIo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
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

uint32_t crc32_update(const uint8_t* data, std::size_t size) {
  static uint32_t table[256];
  static bool inited = false;
  if (!inited) {
    for (uint32_t i = 0; i < 256; ++i) {
      table[i] = crc32_for_byte(i);
    }
    inited = true;
  }

  uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  constexpr uint32_t kMod = 65521u;
  uint32_t a = 1u;
  uint32_t b = 0u;
  for (auto byte : bytes) {
    a = (a + byte) % kMod;
    b = (b + a) % kMod;
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
  std::vector<uint8_t> crcSeed;
  crcSeed.reserve(4 + data.size());
  crcSeed.insert(crcSeed.end(), type.begin(), type.end());
  crcSeed.insert(crcSeed.end(), data.begin(), data.end());
  write_u32_be(out, crc32_update(crcSeed.data(), crcSeed.size()));
}

std::vector<uint8_t> encode_deflate_stored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() * 2);
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

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(image.width) * image.height * 5u);
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const auto row = static_cast<std::size_t>(y) * image.width * 4u;
    for (uint32_t x = 0; x < image.width; ++x) {
      const auto idx = row + static_cast<std::size_t>(x) * 4u;
      raw.push_back(image.rgba8[idx + 0]);
      raw.push_back(image.rgba8[idx + 1]);
      raw.push_back(image.rgba8[idx + 2]);
      raw.push_back(image.rgba8[idx + 3]);
    }
  }
  const std::vector<uint8_t> compressed = encode_deflate_stored(raw);

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
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) {
      *error = "failed to open output path";
    }
    return false;
  }
  out << "# EXR-compatible placeholder written by vkpt path tracer.\n";
  out << image.width << " " << image.height << "\n";
  for (std::size_t i = 0; i < image.rgbf.size(); i += 3) {
    out << image.rgbf[i] << " " << image.rgbf[i + 1] << " " << image.rgbf[i + 2] << "\n";
  }
  return true;
}

}  // namespace vkpt::pathtracer
