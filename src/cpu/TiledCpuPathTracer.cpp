#include "cpu/TiledCpuPathTracer.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace vkpt::cpu {

namespace {

BvhBuildStats to_bvh_build_stats(const vkpt::pathtracer::RayAcceleratorBuildInfo& info,
                                 std::size_t worker_count) {
  BvhBuildStats stats{};
  stats.node_count = info.node_count;
  stats.leaf_count = info.leaf_count;
  stats.prim_count = info.primitive_count;
  stats.build_ms = info.build_ms;
  stats.worker_count = worker_count;
  stats.deterministic = info.deterministic;
  return stats;
}

}  // namespace

TiledCpuPathTracer::TiledCpuPathTracer(TiledRenderConfig config)
    : m_config(config) {
  const std::size_t workers =
      (config.worker_count == 0)
          ? std::max<std::size_t>(1u, std::thread::hardware_concurrency())
          : static_cast<std::size_t>(config.worker_count);
  m_jobSystem = std::make_unique<vkpt::jobs::JobSystem>(
      vkpt::jobs::JobSystemConfig{
          workers,
          vkpt::jobs::WorkerThreadPriority::Background,
          false});
  if (config.deterministic) {
    m_jobSystem->set_deterministic(true);
  }
}

TiledCpuPathTracer::~TiledCpuPathTracer() {
  shutdown();
}

bool TiledCpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& settings) {
  m_settings = vkpt::pathtracer::MakeRenderSettings(vkpt::pathtracer::MakePathTraceSettings(settings));
  m_initialized = false;
  m_tiles.clear();
  m_sharedAccelerator.reset();
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
  m_sharedAccelerator.reset();
  return true;
}

bool TiledCpuPathTracer::build_or_update_acceleration() {
  const bool deterministic = m_config.deterministic || m_settings.deterministic;
  if (m_externalAccelerator) {
    // External accelerators are rebuilt in place so callers can own lifetime
    // while tiles still receive the same shared ICpuRayKernel pointer.
    m_sharedAccelerator.reset();
    if (!m_externalAccelerator->build(m_scene, deterministic)) {
      m_bvhStats = {};
      return false;
    }
    m_bvhStats = to_bvh_build_stats(m_externalAccelerator->build_info(), worker_count());
  } else {
    // Default path: create one BVH accelerator and share it across all tile
    // tracers instead of rebuilding a per-tile copy.
    m_sharedAccelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
    if (!m_sharedAccelerator || !m_sharedAccelerator->build(m_scene, deterministic)) {
      m_sharedAccelerator.reset();
      m_bvhStats = {};
      return false;
    }
    m_bvhStats = to_bvh_build_stats(m_sharedAccelerator->build_info(), worker_count());
  }

  // Recreate tile tracers after acceleration changes so each tile sees the
  // current scene snapshot, camera, and accelerator pointer.
  init_tile_tracers();
  return m_initialized;
}

void TiledCpuPathTracer::init_tile_tracers() {
  m_tiles.clear();
  const uint32_t h = m_settings.height;
  const uint32_t tile_h = std::max(1u, m_config.tile_height);
  const uint32_t n_tiles = (h + tile_h - 1u) / tile_h;
  vkpt::pathtracer::IRayAccelerator* activeAccelerator =
      m_externalAccelerator ? m_externalAccelerator : m_sharedAccelerator.get();

  m_tiles.resize(n_tiles);
  bool initialized = true;
  for (uint32_t i = 0; i < n_tiles; ++i) {
    const uint32_t start_y = i * tile_h;
    const uint32_t end_y = std::min(start_y + tile_h, h);
    m_tiles[i].start_y = start_y;
    m_tiles[i].end_y = end_y;

    auto tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
    m_tiles[i].kernel = tracer.get();
    m_tiles[i].tracer = std::move(tracer);
    auto tileSettings = m_settings;
    // Tile-local deterministic settings make repeated tiled runs stable even
    // when worker scheduling changes.
    tileSettings.deterministic = true;
    initialized = m_tiles[i].tracer->configure(tileSettings) && initialized;
    if (activeAccelerator) {
      if (m_tiles[i].kernel != nullptr) {
        initialized = m_tiles[i].kernel->set_accelerator(activeAccelerator) && initialized;
      } else {
        initialized = false;
      }
    }
    initialized = m_tiles[i].tracer->load_scene_snapshot(m_scene) && initialized;
    if (activeAccelerator) {
      initialized = m_tiles[i].tracer->update_camera(m_scene.camera_position,
                                                     m_scene.camera_target,
                                                     m_scene.camera_up,
                                                     m_scene.camera_fov_deg) &&
                    initialized;
    } else {
      initialized = m_tiles[i].tracer->build_or_update_acceleration() && initialized;
    }
    initialized = m_tiles[i].tracer->reset_accumulation() && initialized;
  }
  m_initialized = initialized;
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
  vkpt::pathtracer::IRayAccelerator* activeAccelerator =
      m_externalAccelerator ? m_externalAccelerator : m_sharedAccelerator.get();
  bool applied = true;
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    applied = tile.kernel != nullptr && tile.kernel->set_accelerator(activeAccelerator) && applied;
  }
  return applied;
}

bool TiledCpuPathTracer::update_camera(const vkpt::pathtracer::Vec3& pos,
                                       const vkpt::pathtracer::Vec3& target,
                                       const vkpt::pathtracer::Vec3& up,
                                       float fov_deg) {
  auto camera = vkpt::pathtracer::ExtractCameraState(m_scene);
  camera.position = pos;
  camera.target = target;
  camera.up = up;
  camera.fov_deg = fov_deg;
  return update_camera_state(camera);
}

bool TiledCpuPathTracer::update_camera_state(const vkpt::pathtracer::RTCameraState& camera) {
  vkpt::pathtracer::ApplyCameraState(m_scene, camera);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  bool ok = true;
  vkpt::pathtracer::IRayAccelerator* activeAccelerator =
      m_externalAccelerator ? m_externalAccelerator : m_sharedAccelerator.get();
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    if (!tile.tracer->update_camera_state(camera)) {
      bool tileOk = false;
      const bool loaded = tile.tracer->load_scene_snapshot(m_scene);
      if (loaded && activeAccelerator) {
        if (tile.kernel != nullptr) {
          tileOk = tile.kernel->set_accelerator(activeAccelerator) &&
                   tile.tracer->update_camera_state(camera);
        }
      } else if (loaded) {
        tileOk = tile.tracer->build_or_update_acceleration();
      }
      ok = tileOk && ok;
    }
  }
  return ok;
}

bool TiledCpuPathTracer::update_instance_transforms(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  if (!m_initialized || updates.empty()) {
    return false;
  }
  if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(m_scene, updates)) {
    return false;
  }

  const bool deterministic = m_config.deterministic || m_settings.deterministic;
  vkpt::pathtracer::IRayAccelerator* activeAccelerator = nullptr;
  if (m_externalAccelerator) {
    if (!m_externalAccelerator->build(m_scene, deterministic)) {
      m_bvhStats = {};
      return false;
    }
    activeAccelerator = m_externalAccelerator;
    m_bvhStats = to_bvh_build_stats(m_externalAccelerator->build_info(), worker_count());
  } else {
    if (!m_sharedAccelerator) {
      m_sharedAccelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
    }
    if (!m_sharedAccelerator || !m_sharedAccelerator->build(m_scene, deterministic)) {
      m_bvhStats = {};
      return false;
    }
    activeAccelerator = m_sharedAccelerator.get();
    m_bvhStats = to_bvh_build_stats(m_sharedAccelerator->build_info(), worker_count());
  }

  bool ok = activeAccelerator != nullptr;
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    bool tileOk = tile.tracer->load_scene_snapshot(m_scene);
    if (tileOk && activeAccelerator) {
      if (tile.kernel != nullptr) {
        tileOk = tile.kernel->set_accelerator(activeAccelerator);
      } else {
        tileOk = false;
      }
    } else if (tileOk) {
      tileOk = tile.tracer->build_or_update_acceleration();
    }
    ok = tileOk && ok;
  }
  return ok;
}

bool TiledCpuPathTracer::update_scene_delta(
    const vkpt::pathtracer::RTSceneDeltaUpdate& update) {
  if (!m_initialized) {
    return false;
  }
  auto nextScene = m_scene;
  if (!vkpt::pathtracer::ApplySceneDeltaUpdate(nextScene, update)) {
    return false;
  }
  bool ok = true;
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    ok = tile.tracer->update_scene_delta(update) && ok;
  }
  if (ok) {
    m_scene = std::move(nextScene);
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
    // Only render tile rows that overlap [start_y, end_y).
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
  m_counters = {};
  for (const auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    m_film.import_tile(tile.tracer->film(), tile.start_y, tile.end_y);
    // Counters are reduced after all jobs finish; tile tracers never mutate
    // the aggregate counters directly.
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
