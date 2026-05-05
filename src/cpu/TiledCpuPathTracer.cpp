#include "cpu/TiledCpuPathTracer.h"

#if defined(PT_ENABLE_AVX2)
#include "cpu/Avx2PathTracer.h"
#endif

#include "cpu/CpuFeatures.h"

#include <algorithm>
#include <limits>
#include <thread>

namespace vkpt::cpu {

TiledCpuPathTracer::TiledCpuPathTracer(TiledRenderConfig config)
    : m_config(config) {
  const std::size_t workers =
      (config.worker_count == 0)
          ? std::max<std::size_t>(1u, std::thread::hardware_concurrency())
          : static_cast<std::size_t>(config.worker_count);
  m_jobSystem = std::make_unique<vkpt::jobs::JobSystem>(workers);
  if (config.deterministic) {
    m_jobSystem->set_deterministic(true);
  }

  // Detect SIMD capability for tile tracer selection
  const auto features = vkpt::cpu::QueryCpuFeatures();
  m_simdDispatch = BuildSimdDispatchInfo(features);
}

TiledCpuPathTracer::~TiledCpuPathTracer() {
  shutdown();
}

bool TiledCpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& settings) {
  m_settings = vkpt::pathtracer::MakeRenderSettings(vkpt::pathtracer::MakePathTraceSettings(settings));
  m_initialized = false;
  m_tiles.clear();
  m_film.resize(m_settings.width, m_settings.height);
  m_film.set_resolve_settings(m_settings.film_resolve);
  m_film.clear();
  if (m_jobSystem && m_settings.deterministic) {
    m_jobSystem->set_deterministic(true);
  }
  return true;
}

bool TiledCpuPathTracer::load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) {
  m_scene = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  m_initialized = false;
  m_tiles.clear();
  return true;
}

bool TiledCpuPathTracer::build_or_update_acceleration() {
  // Build a parallel BVH from the scene's triangle AABBs
  const auto& verts = m_scene.vertices;
  const auto& inds = m_scene.indices;

  std::vector<BvhAabb> prim_aabbs;
  const std::size_t tri_count = inds.size() / 3;
  prim_aabbs.reserve(tri_count);
  for (std::size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = inds[t * 3 + 0];
    const uint32_t i1 = inds[t * 3 + 1];
    const uint32_t i2 = inds[t * 3 + 2];
    if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
      continue;
    }
    BvhAabb aabb;
    for (int k = 0; k < 3; ++k) {
      aabb.min[k] = std::numeric_limits<float>::max();
      aabb.max[k] = -std::numeric_limits<float>::max();
    }
    const float* v0 = &verts[i0].x;
    const float* v1 = &verts[i1].x;
    const float* v2 = &verts[i2].x;
    for (int k = 0; k < 3; ++k) {
      aabb.min[k] = std::min({v0[k], v1[k], v2[k]});
      aabb.max[k] = std::max({v0[k], v1[k], v2[k]});
    }
    prim_aabbs.push_back(aabb);
  }

  const bool deterministic = m_config.deterministic || m_settings.deterministic;
  (void)m_bvhBuilder.build(
      prim_aabbs,
      m_jobSystem.get(),
      deterministic);
  m_bvhStats = m_bvhBuilder.last_stats();

  // Initialize tile tracers
  init_tile_tracers();
  return m_initialized;
}

void TiledCpuPathTracer::init_tile_tracers() {
  m_tiles.clear();
  const uint32_t h = m_settings.height;
  const uint32_t tile_h = std::max(1u, m_config.tile_height);
  const uint32_t n_tiles = (h + tile_h - 1u) / tile_h;

  m_tiles.resize(n_tiles);
  for (uint32_t i = 0; i < n_tiles; ++i) {
    const uint32_t start_y = i * tile_h;
    const uint32_t end_y = std::min(start_y + tile_h, h);
    m_tiles[i].start_y = start_y;
    m_tiles[i].end_y = end_y;

#if defined(PT_ENABLE_AVX2)
    const bool use_avx2_tile =
        !m_settings.deterministic &&
        m_externalAccelerator == nullptr &&
        m_scene.sdf_primitives.empty() &&
        m_simdDispatch.preferred == vkpt::cpu::SimdBackend::X86Avx2;
    if (use_avx2_tile) {
      m_tiles[i].tracer = std::make_unique<vkpt::cpu::Avx2CpuPathTracer>();
    } else {
      m_tiles[i].tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
    }
#else
    m_tiles[i].tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
#endif
    m_tiles[i].tracer->configure(m_settings);
    if (m_externalAccelerator) {
      if (auto* kernel = dynamic_cast<vkpt::pathtracer::ICpuRayKernel*>(m_tiles[i].tracer.get())) {
        kernel->set_accelerator(m_externalAccelerator);
      }
    }
    m_tiles[i].tracer->load_scene_snapshot(m_scene);
    m_tiles[i].tracer->build_or_update_acceleration();
    m_tiles[i].tracer->reset_accumulation();
  }
  m_initialized = true;
}

bool TiledCpuPathTracer::reset_accumulation() {
  m_film.clear();
  m_counters = {};
  for (auto& tile : m_tiles) {
    if (tile.tracer) {
      tile.tracer->reset_accumulation();
    }
  }
  return true;
}

bool TiledCpuPathTracer::set_accelerator(vkpt::pathtracer::IRayAccelerator* accelerator) {
  m_externalAccelerator = accelerator;
  bool applied = true;
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    auto* kernel = dynamic_cast<vkpt::pathtracer::ICpuRayKernel*>(tile.tracer.get());
    applied = kernel != nullptr && kernel->set_accelerator(accelerator) && applied;
  }
  return applied;
}

bool TiledCpuPathTracer::update_camera(const vkpt::pathtracer::Vec3& pos,
                                       const vkpt::pathtracer::Vec3& target,
                                       const vkpt::pathtracer::Vec3& up,
                                       float fov_deg) {
  m_scene.camera_position = pos;
  m_scene.camera_target = target;
  m_scene.camera_up = up;
  m_scene.camera_fov_deg = fov_deg;
  bool ok = true;
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    if (!tile.tracer->update_camera(pos, target, up, fov_deg)) {
      ok = tile.tracer->load_scene_snapshot(m_scene) &&
           tile.tracer->build_or_update_acceleration() &&
           ok;
    }
  }
  return ok;
}

bool TiledCpuPathTracer::render_sample_batch(
    uint32_t start_y,
    uint32_t end_y,
    uint32_t sample_index,
    uint32_t frame_index) {
  return render_sample_batch_cancellable(start_y, end_y, sample_index, frame_index, {});
}

bool TiledCpuPathTracer::render_sample_batch_cancellable(
    uint32_t start_y,
    uint32_t end_y,
    uint32_t sample_index,
    uint32_t frame_index,
    std::stop_token stop) {
  if (!m_initialized) {
    return false;
  }
  if (stop.stop_requested()) {
    return false;
  }

  std::vector<vkpt::core::RuntimeHandle> handles;
  handles.reserve(m_tiles.size());

  for (auto& tile : m_tiles) {
    // Only render tile rows that overlap [start_y, end_y)
    const uint32_t tile_start = std::max(tile.start_y, start_y);
    const uint32_t tile_end = std::min(tile.end_y, end_y);
    if (tile_start >= tile_end) {
      continue;
    }
    auto handle = m_jobSystem->submit_job([&tile, tile_start, tile_end, sample_index, frame_index, stop]() {
      if (stop.stop_requested()) {
        return;
      }
      tile.tracer->render_sample_batch(tile_start, tile_end, sample_index, frame_index);
    });
    handles.push_back(handle);
  }

  if (!m_jobSystem->wait_group(handles, stop) || stop.stop_requested()) {
    return false;
  }
  merge_tiles();
  return true;
}

void TiledCpuPathTracer::merge_tiles() {
  m_film.clear();
  m_counters = {};
  for (const auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    m_film.import_tile(tile.tracer->film(), tile.start_y, tile.end_y);
    const auto tc = tile.tracer->read_counters();
    m_counters.samples += tc.samples;
    m_counters.rays += tc.rays;
    m_counters.triangle_tests += tc.triangle_tests;
    m_counters.triangle_hits += tc.triangle_hits;
    m_counters.sdf_tests += tc.sdf_tests;
    m_counters.sdf_steps += tc.sdf_steps;
    m_counters.sdf_hits += tc.sdf_hits;
    m_counters.sdf_misses += tc.sdf_misses;
    m_counters.bvh_node_visits += tc.bvh_node_visits;
    m_counters.bvh_leaf_visits += tc.bvh_leaf_visits;
    m_counters.shadow_tests += tc.shadow_tests;
  }
}

vkpt::pathtracer::FilmLdr TiledCpuPathTracer::resolve_ldr() const {
  return m_film.resolve_ldr();
}

vkpt::pathtracer::FilmHdr TiledCpuPathTracer::resolve_hdr() const {
  return m_film.resolve_hdr();
}

vkpt::pathtracer::SampleCounters TiledCpuPathTracer::read_counters() const {
  return m_counters;
}

void TiledCpuPathTracer::shutdown() {
  m_tiles.clear();
  m_initialized = false;
  m_externalAccelerator = nullptr;
  if (m_jobSystem) {
    m_jobSystem->shutdown();
  }
}

std::size_t TiledCpuPathTracer::worker_count() const {
  return m_jobSystem ? m_jobSystem->worker_count() : 0u;
}

}  // namespace vkpt::cpu
