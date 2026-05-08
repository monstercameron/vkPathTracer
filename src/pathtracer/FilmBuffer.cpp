#include "pathtracer/FilmBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>

namespace vkpt::pathtracer {

namespace {

Vec3 add_vec3(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

bool checked_mul_size(std::size_t lhs, std::size_t rhs, std::size_t& out) {
  if (lhs != 0u && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

bool checked_pixel_count(uint32_t width, uint32_t height, std::size_t& out) {
  return checked_mul_size(static_cast<std::size_t>(width), static_cast<std::size_t>(height), out);
}

Vec3 ColorTemperatureToRgb(float kelvin) {
  const float temperature = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;

  if (temperature <= 66.0f) {
    r = 1.0f;
    g = std::clamp(0.39008158f * std::log(std::max(1.0f, temperature)) - 0.63184144f, 0.0f, 1.0f);
    b = temperature <= 19.0f
        ? 0.0f
        : std::clamp(0.5432068f * std::log(std::max(1.0f, temperature - 10.0f)) - 1.1962541f, 0.0f, 1.0f);
  } else {
    r = std::clamp(1.2929362f * std::pow(temperature - 60.0f, -0.13320476f), 0.0f, 1.0f);
    g = std::clamp(1.1298909f * std::pow(temperature - 60.0f, -0.075514846f), 0.0f, 1.0f);
    b = 1.0f;
  }

  return {r, g, b};
}

}  // namespace

FilmBuffer::FilmBuffer(uint32_t width, uint32_t height) {
  resize(width, height);
}

void FilmBuffer::resize(uint32_t width, uint32_t height) {
  std::size_t pixel_count = 0u;
  if (!checked_pixel_count(width, height, pixel_count)) {
    m_width = 0u;
    m_height = 0u;
    m_accumulation.clear();
    m_sampleCounts.clear();
    m_invalidSamples.clear();
    return;
  }
  m_width = width;
  m_height = height;
  m_accumulation.assign(pixel_count, Vec3{});
  m_sampleCounts.assign(pixel_count, 0);
  m_invalidSamples.assign(pixel_count, 0.0f);
}

void FilmBuffer::clear() {
  std::fill(m_accumulation.begin(), m_accumulation.end(), Vec3{});
  std::fill(m_sampleCounts.begin(), m_sampleCounts.end(), 0);
  std::fill(m_invalidSamples.begin(), m_invalidSamples.end(), 0.0f);
}

void FilmBuffer::add_sample(uint32_t x, uint32_t y, const Vec3& color) {
  const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
  if (idx >= m_accumulation.size()) {
    return;
  }
  if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z)) {
    m_invalidSamples[idx] += 1.0f;
    return;
  }
  m_accumulation[idx] = add_vec3(m_accumulation[idx], color);
  m_sampleCounts[idx] += 1;
}

void FilmBuffer::set_pixel_raw(uint32_t x,
                               uint32_t y,
                               const Vec3& accumulation,
                               uint32_t sample_count,
                               float invalid_samples) {
  const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
  if (idx >= m_accumulation.size()) {
    return;
  }
  m_accumulation[idx] = accumulation;
  m_sampleCounts[idx] = sample_count;
  m_invalidSamples[idx] = std::max(0.0f, invalid_samples);
}

void FilmBuffer::reset_pixel(uint32_t x, uint32_t y) {
  const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
  if (idx >= m_accumulation.size()) {
    return;
  }
  m_accumulation[idx] = {};
  m_sampleCounts[idx] = 0u;
  m_invalidSamples[idx] = 0.0f;
}

bool FilmBuffer::copy_from(const FilmBuffer& src) {
  if (m_width != src.m_width || m_height != src.m_height) {
    return false;
  }
  m_accumulation = src.m_accumulation;
  m_sampleCounts = src.m_sampleCounts;
  m_invalidSamples = src.m_invalidSamples;
  m_resolveSettings = src.m_resolveSettings;
  return true;
}

void FilmBuffer::import_tile(const FilmBuffer& src, uint32_t start_y, uint32_t end_y) {
  if (m_width != src.m_width || m_height == 0 || src.m_height == 0) {
    return;
  }
  const uint32_t clamped_start = std::min(start_y, std::min(m_height, src.m_height));
  const uint32_t clamped_end = std::min(end_y, std::min(m_height, src.m_height));
  if (clamped_start >= clamped_end) {
    return;
  }
  const std::size_t row_width = m_width;
  for (uint32_t y = clamped_start; y < clamped_end; ++y) {
    const std::size_t offset = static_cast<std::size_t>(y) * row_width;
    std::copy_n(src.m_accumulation.begin() + static_cast<std::ptrdiff_t>(offset),
                row_width,
                m_accumulation.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy_n(src.m_sampleCounts.begin() + static_cast<std::ptrdiff_t>(offset),
                row_width,
                m_sampleCounts.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy_n(src.m_invalidSamples.begin() + static_cast<std::ptrdiff_t>(offset),
                row_width,
                m_invalidSamples.begin() + static_cast<std::ptrdiff_t>(offset));
  }
}

FilmLdr FilmBuffer::resolve_ldr() const {
  return resolve_ldr(m_resolveSettings);
}

FilmLdr FilmBuffer::resolve_ldr(const FilmResolveSettings& settings) const {
  return ApplyFilmResolve(resolve_hdr(), settings);
}

FilmHdr FilmBuffer::resolve_hdr() const {
  FilmHdr out;
  out.width = m_width;
  out.height = m_height;
  std::size_t num_pixels = 0u;
  std::size_t rgb_count = 0u;
  if (!checked_pixel_count(m_width, m_height, num_pixels) ||
      !checked_mul_size(num_pixels, 3u, rgb_count)) {
    out.width = 0u;
    out.height = 0u;
    return out;
  }
  out.rgbf.resize(rgb_count, 0.0f);
  if (m_accumulation.size() < num_pixels || m_sampleCounts.size() < num_pixels) {
    out.width = 0u;
    out.height = 0u;
    out.rgbf.clear();
    return out;
  }
  // HDR resolve averages valid accumulated samples and leaves exposure/tone mapping to ApplyFilmResolve.
  const uint32_t first_sample_count = num_pixels > 0u ? m_sampleCounts.front() : 0u;
  const bool uniform_sample_count = std::all_of(
      m_sampleCounts.begin(),
      m_sampleCounts.begin() + static_cast<std::ptrdiff_t>(num_pixels),
      [first_sample_count](uint32_t count) { return count == first_sample_count; });
  if (uniform_sample_count) {
    const float invSamples = 1.0f / std::max(1u, first_sample_count);
    const Vec3* src = m_accumulation.data();
    float* dst = out.rgbf.data();
    if (invSamples == 1.0f) {
      for (std::size_t idx = 0u; idx < num_pixels; ++idx) {
        *dst++ = src[idx].x;
        *dst++ = src[idx].y;
        *dst++ = src[idx].z;
      }
    } else {
      for (std::size_t idx = 0u; idx < num_pixels; ++idx) {
        *dst++ = src[idx].x * invSamples;
        *dst++ = src[idx].y * invSamples;
        *dst++ = src[idx].z * invSamples;
      }
    }
    return out;
  }

  std::size_t rgb = 0u;
  for (std::size_t idx = 0u; idx < num_pixels; ++idx) {
    const float invSamples = 1.0f / std::max(1u, m_sampleCounts[idx]);
    const auto& sample = m_accumulation[idx];
    out.rgbf[rgb++] = sample.x * invSamples;
    out.rgbf[rgb++] = sample.y * invSamples;
    out.rgbf[rgb++] = sample.z * invSamples;
  }
  return out;
}

Vec3 WhiteBalanceScale(float kelvin) {
  const Vec3 d65 = ColorTemperatureToRgb(6500.0f);
  const Vec3 target = ColorTemperatureToRgb(kelvin);
  constexpr float kMinWhite = 1.0e-3f;
  return {
      d65.x / std::max(kMinWhite, target.x),
      d65.y / std::max(kMinWhite, target.y),
      d65.z / std::max(kMinWhite, target.z)};
}

FilmResolveSettings CameraAdjustedFilmResolveSettings(const FilmResolveSettings& base,
                                                       const PathTracerSceneSnapshot& scene) {
  FilmResolveSettings out = base;
  // Convert camera exposure controls into a scalar multiplier before tone mapping.
  if (std::isfinite(scene.camera_f_stop) &&
      std::isfinite(scene.camera_shutter_seconds) &&
      std::isfinite(scene.camera_iso) &&
      scene.camera_f_stop > 0.0f &&
      scene.camera_shutter_seconds > 0.0f &&
      scene.camera_iso > 0.0f) {
    constexpr float kReferenceFStop = 2.8f;
    constexpr float kReferenceShutter = 1.0f / 60.0f;
    constexpr float kReferenceIso = 100.0f;
    const float reference = (kReferenceShutter * kReferenceIso) / (kReferenceFStop * kReferenceFStop);
    const float physical = (scene.camera_shutter_seconds * scene.camera_iso) /
        (scene.camera_f_stop * scene.camera_f_stop);
    out.exposure *= std::clamp(physical / std::max(1.0e-6f, reference), 1.0e-6f, 1.0e6f);
  }
  if (std::isfinite(scene.camera_exposure_compensation)) {
    out.exposure *= std::pow(2.0f, std::clamp(scene.camera_exposure_compensation, -32.0f, 32.0f));
  }
  if (std::isfinite(scene.camera_white_balance_kelvin) &&
      scene.camera_white_balance_kelvin >= 1000.0f &&
      scene.camera_white_balance_kelvin <= 40000.0f) {
    out.white_balance_kelvin = scene.camera_white_balance_kelvin;
  }
  return out;
}

FilmLdr ApplyFilmResolve(const FilmHdr& hdr, const FilmResolveSettings& settings) {
  FilmLdr ldr;
  ldr.width = hdr.width;
  ldr.height = hdr.height;
  std::size_t num_pixels = 0u;
  std::size_t rgba_count = 0u;
  std::size_t rgb_count = 0u;
  if (!checked_pixel_count(hdr.width, hdr.height, num_pixels) ||
      !checked_mul_size(num_pixels, 4u, rgba_count) ||
      !checked_mul_size(num_pixels, 3u, rgb_count)) {
    ldr.width = 0u;
    ldr.height = 0u;
    return ldr;
  }
  ldr.rgba8.resize(rgba_count, 255u);
  if (hdr.rgbf.size() < rgb_count) {
    std::fill(ldr.rgba8.begin(), ldr.rgba8.end(), 0u);
    for (std::size_t i = 0; i < num_pixels; ++i) {
      ldr.rgba8[i * 4u + 3u] = 255u;
    }
    return ldr;
  }

  const float inv_gamma = 1.0f / std::max(0.01f, settings.gamma);
  const Vec3 white_balance = WhiteBalanceScale(settings.white_balance_kelvin);
  // The resolve path applies exposure, white balance, tone map, then output transform in that order.
  auto tonemap = [&](float x) -> float {
    switch (settings.tone_map) {
      case ToneMapMode::Reinhard:
        return x / (1.0f + x);
      case ToneMapMode::FilmicApprox: {
        auto F = [](float v) -> float {
          const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F_ = 0.30f;
          return ((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F_)) - E / F_;
        };
        const float W = 11.2f;
        return F(x) / F(W);
      }
      case ToneMapMode::AcesApprox:
        return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
      default:
        return x;
    }
  };

  std::size_t rgb = 0u;
  std::size_t rgba = 0u;
  for (std::size_t i = 0; i < num_pixels; ++i) {
    float r = hdr.rgbf[rgb++] * settings.exposure * white_balance.x;
    float g = hdr.rgbf[rgb++] * settings.exposure * white_balance.y;
    float b = hdr.rgbf[rgb++] * settings.exposure * white_balance.z;
    if (!std::isfinite(r)) r = 0.0f;
    if (!std::isfinite(g)) g = 0.0f;
    if (!std::isfinite(b)) b = 0.0f;

    r = tonemap(r);
    g = tonemap(g);
    b = tonemap(b);

    if (settings.output_transform == OutputTransformMode::Gamma) {
      r = std::pow(std::max(0.0f, r), inv_gamma);
      g = std::pow(std::max(0.0f, g), inv_gamma);
      b = std::pow(std::max(0.0f, b), inv_gamma);
    }

    if (settings.clamp_output) {
      r = std::min(1.0f, std::max(0.0f, r));
      g = std::min(1.0f, std::max(0.0f, g));
      b = std::min(1.0f, std::max(0.0f, b));
    }

    ldr.rgba8[rgba++] = static_cast<uint8_t>(r * 255.0f + 0.5f);
    ldr.rgba8[rgba++] = static_cast<uint8_t>(g * 255.0f + 0.5f);
    ldr.rgba8[rgba++] = static_cast<uint8_t>(b * 255.0f + 0.5f);
    ldr.rgba8[rgba++] = 255u;
  }
  return ldr;
}

std::string SerializeFilmResolveSettings(const FilmResolveSettings& settings) {
  const char* tone_map_str = "linear";
  switch (settings.tone_map) {
    case ToneMapMode::Reinhard:     tone_map_str = "reinhard";      break;
    case ToneMapMode::FilmicApprox: tone_map_str = "filmic_approx"; break;
    case ToneMapMode::AcesApprox:   tone_map_str = "aces_approx";   break;
    default: break;
  }
  const char* output_transform_str = "gamma";
  switch (settings.output_transform) {
    case OutputTransformMode::Linear: output_transform_str = "linear"; break;
    case OutputTransformMode::Gamma:
    default: break;
  }
  std::ostringstream out;
  out << "{";
  out << "\"exposure\":" << settings.exposure << ",";
  out << "\"white_balance_kelvin\":" << settings.white_balance_kelvin << ",";
  out << "\"tone_map\":\"" << tone_map_str << "\",";
  out << "\"output_transform\":\"" << output_transform_str << "\",";
  out << "\"gamma\":" << settings.gamma << ",";
  out << "\"clamp_output\":" << (settings.clamp_output ? "true" : "false");
  out << "}";
  return out.str();
}

}  // namespace vkpt::pathtracer
