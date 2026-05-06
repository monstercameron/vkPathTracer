#include "pathtracer/PathTracer.h"
#include "pathtracer/ScalarCpuPathTracerJobs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-4f;

void atomic_add_u64(uint64_t& value, uint64_t delta = 1u) {
  std::atomic_ref<uint64_t> ref(value);
  ref.fetch_add(delta, std::memory_order_relaxed);
}

uint64_t atomic_load_u64(const uint64_t& value) {
  std::atomic_ref<const uint64_t> ref(value);
  return ref.load(std::memory_order_relaxed);
}

vkpt::pathtracer::Vec3 operator+(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z}; }
vkpt::pathtracer::Vec3 operator-(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z}; }
vkpt::pathtracer::Vec3 operator*(const vkpt::pathtracer::Vec3& lhs, float rhs) { return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs}; }
vkpt::pathtracer::Vec3 operator/(const vkpt::pathtracer::Vec3& lhs, float rhs) { return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs}; }

float dot(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z; }
float length_sq(const vkpt::pathtracer::Vec3& value) { return dot(value, value); }
float length(const vkpt::pathtracer::Vec3& value) { return std::sqrt(length_sq(value)); }
vkpt::pathtracer::Vec3 normalize(const vkpt::pathtracer::Vec3& value) {
  const float l = length(value);
  return l <= kEpsilon ? vkpt::pathtracer::Vec3{0.0f, 1.0f, 0.0f} : value / l;
}

float luminance(const vkpt::pathtracer::Vec3& value) {
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

float clamp01(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return std::min(1.0f, std::max(0.0f, v));
}

float radians(float deg) { return deg * (kPi / 180.0f); }

}  // namespace

namespace vkpt::pathtracer {

Ray ScalarCpuPathTracer::camera_rays(uint32_t x,
                                     uint32_t y,
                                     uint32_t sample_index,
                                     uint32_t frame_index,
                                     uint32_t path_id,
                                     uint64_t& sample_seed) {
  SampleKey key{};
  key.pixel_index = static_cast<vkpt::core::StableId>(y) * m_settings.width + x;
  key.sample_index = sample_index;
  key.frame_index = frame_index;
  key.path_id = path_id;
  key.seed = m_settings.seed;
  key.dimension = 0;
  key.path_depth = 0;
  Rng rng(key);

  const float fx = (static_cast<float>(x) + rng.next01()) / std::max(1u, m_settings.width);
  const float fy = (static_cast<float>(y) + rng.next01()) / std::max(1u, m_settings.height);
  sample_seed ^= static_cast<uint64_t>(rng.next01() * 0xffffu);
  const float aspect = static_cast<float>(m_settings.width) / std::max(1.0f, static_cast<float>(m_settings.height));
  const float tanHalfFov = std::tan(0.5f * radians(m_scene.camera_fov_deg));
  const float nx = (2.0f * fx - 1.0f) * aspect * tanHalfFov;
  const float ny = (1.0f - 2.0f * fy) * tanHalfFov;
  const Vec3 dir = normalize(m_camera_forward + m_camera_right * nx + m_camera_up * ny);
  const float aperture_radius = m_scene.camera_aperture_radius > 0.0f
      ? m_scene.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  const float focus_distance = m_scene.camera_focus_distance > 0.0f
      ? m_scene.camera_focus_distance
      : m_settings.camera_focus_distance;
  if (aperture_radius > 0.0f && focus_distance > kEpsilon) {
    const float lens_radius_sample = std::sqrt(rng.next01());
    const float lens_phi = 2.0f * kPi * rng.next01();
    float aperture_boundary = 1.0f;
    const uint32_t iris_blades = std::min(m_scene.camera_iris_blade_count, 64u);
    const float iris_roundness = clamp01(m_scene.camera_iris_roundness);
    if (iris_blades >= 3u && iris_roundness < 0.999f) {
      const float sector = 2.0f * kPi / static_cast<float>(iris_blades);
      const float local_phi = lens_phi - radians(m_scene.camera_iris_rotation_degrees);
      const float wrapped = local_phi - sector * std::floor(local_phi / sector);
      const float centered = wrapped > sector * 0.5f ? wrapped - sector : wrapped;
      const float polygon_boundary =
          std::cos(sector * 0.5f) / std::max(0.1f, std::cos(centered));
      aperture_boundary = polygon_boundary * (1.0f - iris_roundness) + iris_roundness;
    }
    const float lens_r = aperture_radius * lens_radius_sample * aperture_boundary;
    const float anamorphic_squeeze =
        std::isfinite(m_scene.camera_anamorphic_squeeze)
            ? std::max(0.01f, m_scene.camera_anamorphic_squeeze)
            : 1.0f;
    const Vec3 lens_offset =
        m_camera_right * (lens_r * std::cos(lens_phi) * anamorphic_squeeze) +
        m_camera_up * (lens_r * std::sin(lens_phi));
    const Vec3 focus_point = m_scene.camera_position + dir * focus_distance;
    const Vec3 origin = m_scene.camera_position + lens_offset;
    return Ray{origin, normalize(focus_point - origin)};
  }
  return Ray{m_scene.camera_position, dir};
}

bool ScalarCpuPathTracer::render_sample_batch(uint32_t start_y,
                                             uint32_t end_y,
                                             uint32_t sample_index,
                                             uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  const uint32_t maxY = std::min(end_y, m_settings.height);
  const uint32_t minY = std::min(start_y, maxY);
  const uint64_t pixelCount64 = static_cast<uint64_t>(m_settings.width) * (maxY - minY);
  if (pixelCount64 == 0u) {
    return true;
  }
  if (pixelCount64 > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint64_t firstPixel64 = static_cast<uint64_t>(minY) * m_settings.width;
  if (firstPixel64 > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return render_sample_contiguous_pixels(static_cast<uint32_t>(firstPixel64),
                                         static_cast<uint32_t>(pixelCount64),
                                         sample_index,
                                         frame_index);
}

void ScalarCpuPathTracer::shade_pixel(uint32_t pixel,
                                      uint32_t sample_index,
                                      uint32_t frame_index,
                                      RenderBatchAccum& accum) {
  if (m_settings.width == 0u || m_settings.height == 0u) {
    return;
  }
  const uint64_t totalPixels = static_cast<uint64_t>(m_settings.width) * m_settings.height;
  if (static_cast<uint64_t>(pixel) >= totalPixels) {
    return;
  }

  accum.min_pixel = std::min(accum.min_pixel, pixel);
  accum.max_pixel = std::max(accum.max_pixel, pixel);

  const uint32_t x = pixel % m_settings.width;
  const uint32_t y = pixel / m_settings.width;

  uint64_t seed = 0;
  const uint64_t sampleSeed = (static_cast<uint64_t>(y) << 32) | x;
  const uint64_t pathId = sampleSeed + static_cast<uint64_t>(sample_index) + static_cast<uint64_t>(frame_index);
  const Ray ray = camera_rays(x, y, sample_index, frame_index, static_cast<uint32_t>(pathId), seed);
  SampleKey key{};
  key.pixel_index = sampleSeed;
  key.sample_index = sample_index;
  key.frame_index = frame_index;
  key.path_id = pathId;
  key.seed = m_settings.seed + sampleSeed + seed;
  key.path_depth = 0;
  Rng rng(key);
  uint64_t rayCounter = 0;
  Vec3 sample = trace(ray,
                      sample_index,
                      frame_index,
                      static_cast<uint32_t>(pathId),
                      0,
                      rayCounter,
                      accum.counters,
                      rng);
  if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z)) {
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning,
                                      "pathtracer",
                                      "non-finite sample",
                                      {{"pixel", std::to_string(sampleSeed)}, {"sample", std::to_string(sample_index)}});
    return;
  }

  const float lum = luminance(sample);
  accum.lum_sum += lum;
  accum.sample_max = std::max(accum.sample_max, std::max(sample.x, std::max(sample.y, sample.z)));
  ++accum.sample_count;
  accum.ray_count += rayCounter;
  m_film.add_sample(x, y, sample);
}

bool ScalarCpuPathTracer::render_sample_contiguous_pixels(uint32_t first_pixel,
                                                          uint32_t pixel_count,
                                                          uint32_t sample_index,
                                                          uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  if (pixel_count == 0u) {
    return true;
  }

  auto& jobs = ScalarRenderJobs();
  jobs.set_deterministic(m_settings.deterministic);
  const uint32_t threadCount = m_settings.deterministic
      ? 1u
      : std::max(1u, std::min(m_worker_count, pixel_count));
  std::vector<RenderBatchAccum> locals(static_cast<std::size_t>(threadCount));

  if (threadCount == 1u) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
      shade_pixel(first_pixel + i, sample_index, frame_index, locals[0]);
    }
  } else {
    auto run_worker = [&](uint32_t workerIndex) {
      RenderBatchAccum& local = locals[workerIndex];
      const uint64_t begin = (static_cast<uint64_t>(pixel_count) * workerIndex) / threadCount;
      const uint64_t end = (static_cast<uint64_t>(pixel_count) * (workerIndex + 1u)) / threadCount;
      for (uint64_t i = begin; i < end; ++i) {
        shade_pixel(first_pixel + static_cast<uint32_t>(i), sample_index, frame_index, local);
      }
    };

    std::vector<vkpt::core::JobHandle> handles;
    handles.reserve(threadCount);
    for (uint32_t t = 0u; t < threadCount; ++t) {
      handles.push_back(jobs.submit_job([&, t]() {
        run_worker(t);
      }));
    }
    if (!jobs.wait_group(handles)) {
      return false;
    }
  }

  return finish_render_batch(locals, sample_index, frame_index);
}

bool ScalarCpuPathTracer::render_sample_pixels(const uint32_t* pixel_indices,
                                               uint32_t pixel_count,
                                               uint32_t sample_index,
                                               uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  if (pixel_indices == nullptr || pixel_count == 0u) {
    return true;
  }

  auto& jobs = ScalarRenderJobs();
  jobs.set_deterministic(m_settings.deterministic);
  const uint32_t threadCount = m_settings.deterministic
      ? 1u
      : std::max(1u, std::min(m_worker_count, pixel_count));
  std::vector<RenderBatchAccum> locals(static_cast<std::size_t>(threadCount));

  if (threadCount == 1u) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
      shade_pixel(pixel_indices[i], sample_index, frame_index, locals[0]);
    }
  } else {
    auto run_worker = [&](uint32_t workerIndex) {
      RenderBatchAccum& local = locals[workerIndex];
      for (uint32_t i = workerIndex; i < pixel_count; i += threadCount) {
        shade_pixel(pixel_indices[i], sample_index, frame_index, local);
      }
    };

    std::vector<vkpt::core::JobHandle> handles;
    handles.reserve(threadCount);
    for (uint32_t t = 0u; t < threadCount; ++t) {
      handles.push_back(jobs.submit_job([&, t]() {
        run_worker(t);
      }));
    }
    if (!jobs.wait_group(handles)) {
      return false;
    }
  }

  return finish_render_batch(locals, sample_index, frame_index);
}

bool ScalarCpuPathTracer::finish_render_batch(const std::vector<RenderBatchAccum>& locals,
                                              uint32_t sample_index,
                                              uint32_t frame_index) {
  float sampleLumSum = 0.0f;
  float sampleMax = 0.0f;
  std::uint64_t sampleCount = 0u;
  std::uint64_t rayCount = 0u;
  uint32_t minPixel = std::numeric_limits<uint32_t>::max();
  uint32_t maxPixel = 0u;
  SampleCounters counters{};
  for (const auto& local : locals) {
    sampleLumSum += local.lum_sum;
    sampleMax = std::max(sampleMax, local.sample_max);
    sampleCount += local.sample_count;
    rayCount += local.ray_count;
    counters.triangle_tests += local.counters.triangle_tests;
    counters.sdf_tests += local.counters.sdf_tests;
    counters.sdf_steps += local.counters.sdf_steps;
    counters.triangle_hits += local.counters.triangle_hits;
    counters.sdf_hits += local.counters.sdf_hits;
    counters.sdf_misses += local.counters.sdf_misses;
    counters.bvh_node_visits += local.counters.bvh_node_visits;
    counters.bvh_leaf_visits += local.counters.bvh_leaf_visits;
    counters.shadow_tests += local.counters.shadow_tests;
    if (local.sample_count > 0u) {
      minPixel = std::min(minPixel, local.min_pixel);
      maxPixel = std::max(maxPixel, local.max_pixel);
    }
  }
  atomic_add_u64(m_counters.samples, sampleCount);
  atomic_add_u64(m_counters.rays, rayCount);
  atomic_add_u64(m_counters.triangle_tests, counters.triangle_tests);
  atomic_add_u64(m_counters.sdf_tests, counters.sdf_tests);
  atomic_add_u64(m_counters.sdf_steps, counters.sdf_steps);
  atomic_add_u64(m_counters.triangle_hits, counters.triangle_hits);
  atomic_add_u64(m_counters.sdf_hits, counters.sdf_hits);
  atomic_add_u64(m_counters.sdf_misses, counters.sdf_misses);
  atomic_add_u64(m_counters.bvh_node_visits, counters.bvh_node_visits);
  atomic_add_u64(m_counters.bvh_leaf_visits, counters.bvh_leaf_visits);
  atomic_add_u64(m_counters.shadow_tests, counters.shadow_tests);

  if (sampleCount > 0u && (sample_index == 0u || ((sample_index + 1u) % 4u) == 0u || (sample_index + 1u) == m_settings.spp)) {
    const float avgLum = sampleCount == 0u ? 0.0f : (sampleLumSum / static_cast<float>(sampleCount));
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                      "traceprobe",
                                      "render_sample_pixels",
                                      {
                                        {"frame", std::to_string(frame_index)},
                                        {"sample_index", std::to_string(sample_index)},
                                        {"pixels", std::to_string(sampleCount)},
                                        {"pixel_span", std::to_string(minPixel) + "-" + std::to_string(maxPixel)},
                                        {"avg_lum", std::to_string(avgLum)},
                                        {"sample_max", std::to_string(sampleMax)},
                                        {"samples", std::to_string(sampleCount)},
                                        {"sdf_steps", std::to_string(atomic_load_u64(m_counters.sdf_steps))},
                                        {"bvh_node_visits", std::to_string(atomic_load_u64(m_counters.bvh_node_visits))}
                                      });
  }
  return true;
}

SampleCounters ScalarCpuPathTracer::read_counters() const {
  SampleCounters out;
  out.samples = atomic_load_u64(m_counters.samples);
  out.rays = atomic_load_u64(m_counters.rays);
  out.triangle_tests = atomic_load_u64(m_counters.triangle_tests);
  out.sdf_tests = atomic_load_u64(m_counters.sdf_tests);
  out.sdf_steps = atomic_load_u64(m_counters.sdf_steps);
  out.triangle_hits = atomic_load_u64(m_counters.triangle_hits);
  out.sdf_hits = atomic_load_u64(m_counters.sdf_hits);
  out.sdf_misses = atomic_load_u64(m_counters.sdf_misses);
  out.bvh_node_visits = atomic_load_u64(m_counters.bvh_node_visits);
  out.bvh_leaf_visits = atomic_load_u64(m_counters.bvh_leaf_visits);
  out.shadow_tests = atomic_load_u64(m_counters.shadow_tests);
  return out;
}

void ScalarCpuPathTracer::shutdown() {
  m_configured = false;
  m_has_scene = false;
  m_film = FilmBuffer{};
  m_scene = RTSceneData{};
  m_accelerator.reset();
  m_external_accelerator = nullptr;
  m_accel_info = {};
}


}  // namespace vkpt::pathtracer
