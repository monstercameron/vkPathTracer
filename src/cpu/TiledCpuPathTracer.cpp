#include "cpu/TiledCpuPathTracer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "cpu/CpuFeatures.h"
#include "core/contracts/Lifecycle.h"
#include "core/metrics/Metrics.h"
#include "pathtracer/PathTracerObservability.h"

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

std::uint64_t build_us_from_stats(const BvhBuildStats& stats) {
  if (stats.build_ms <= 0.0) {
    return 0u;
  }
  return static_cast<std::uint64_t>(stats.build_ms * 1000.0);
}

std::uint64_t percentile99_us(const std::vector<std::uint64_t>& durations,
                              const std::vector<std::size_t>& active_indices) {
  if (active_indices.empty()) {
    return 0u;
  }
  std::vector<std::uint64_t> samples;
  samples.reserve(active_indices.size());
  for (const std::size_t index : active_indices) {
    if (index < durations.size()) {
      samples.push_back(durations[index]);
    }
  }
  if (samples.empty()) {
    return 0u;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t rank =
      ((samples.size() * 99u) + 99u) / 100u;
  return samples[std::min(rank, samples.size()) - 1u];
}

CpuPathTracerLifecycle lifecycle_from_state(bool configured,
                                            bool scene_loaded,
                                            bool accel_valid,
                                            bool failed) {
  if (failed) {
    return CpuPathTracerLifecycle::Failed;
  }
  if (!configured) {
    return CpuPathTracerLifecycle::Uninitialized;
  }
  if (!scene_loaded) {
    return CpuPathTracerLifecycle::Configured;
  }
  if (!accel_valid) {
    return CpuPathTracerLifecycle::SceneLoaded;
  }
  return CpuPathTracerLifecycle::Ready;
}

vkpt::pathtracer::PathTracerLifecycle path_lifecycle_from_cpu(
    CpuPathTracerLifecycle lifecycle) {
  switch (lifecycle) {
    case CpuPathTracerLifecycle::Configured:
      return vkpt::pathtracer::PathTracerLifecycle::Configured;
    case CpuPathTracerLifecycle::SceneLoaded:
      return vkpt::pathtracer::PathTracerLifecycle::SceneLoaded;
    case CpuPathTracerLifecycle::Ready:
      return vkpt::pathtracer::PathTracerLifecycle::Ready;
    case CpuPathTracerLifecycle::Failed:
      return vkpt::pathtracer::PathTracerLifecycle::Failed;
    case CpuPathTracerLifecycle::Uninitialized:
    case CpuPathTracerLifecycle::ShuttingDown:
      return vkpt::pathtracer::PathTracerLifecycle::Uninitialized;
  }
  return vkpt::pathtracer::PathTracerLifecycle::Uninitialized;
}

}  // namespace

TiledCpuPathTracer::TiledCpuPathTracer(TiledRenderConfig config)
    : m_config(config) {
  const auto features = vkpt::cpu::QueryCpuFeatures();
  (void)vkpt::cpu::BuildSimdDispatchInfo(features);
  m_status.kernel = vkpt::cpu::SelectedSimdKernelName(features);

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
    m_jobSystem->set_determinism(config.determinism_context());
  }
}

TiledCpuPathTracer::~TiledCpuPathTracer() {
  shutdown();
}

vkpt::core::Status TiledCpuPathTracer::configure(
    const vkpt::pathtracer::RenderSettings& settings) {
  m_settings = vkpt::pathtracer::MakeRenderSettings(vkpt::pathtracer::MakePathTraceSettings(settings));
  if (!m_config.deterministic && m_settings.deterministic) {
    m_config.deterministic = true;
    m_config.determinism_base_seed = m_settings.seed;
    m_config.determinism_frame_index = m_settings.determinism_frame_index;
    m_config.determinism_scenario_id = m_settings.determinism_scenario_id;
  }
  m_configured = true;
  m_hasScene = false;
  m_initialized = false;
  m_failed = false;
  m_tiles.clear();
  m_sharedAccelerator.reset();
  m_film.resize(m_settings.width, m_settings.height);
  m_film.set_resolve_settings(m_settings.film_resolve);
  m_film.clear();
  m_status.last_tile_us_p99 = 0u;
  m_status.last_build_us = 0u;
  m_currentSample = 0u;
  m_accumulationGen = 0u;
  m_lastError.clear();
  if (m_jobSystem && m_settings.deterministic) {
    m_jobSystem->set_determinism(m_config.determinism_context());
  }
  VKP_LIFECYCLE_CONFIG("cpu",
                       "backend",
                       "tiled-cpu",
                       "tile_height",
                       static_cast<std::uint64_t>(m_config.tile_height),
                       "worker_count",
                       static_cast<std::uint64_t>(worker_count()),
                       "deterministic",
                       m_config.deterministic,
                       "flow_id",
                       CurrentFlowId(m_flowSource));
  return vkpt::core::Status::ok("tiled CPU path tracer configured");
}

vkpt::core::Status TiledCpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  if (!m_configured) {
    m_failed = true;
    m_lastError = "load_scene_snapshot called before configure";
    VKP_LOG(Warn,
            "cpu",
            "scene_load_failed",
            "reason",
            "not_configured",
            "flow_id",
            CurrentFlowId(m_flowSource));
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_lastError);
  }
  m_scene = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  m_hasScene = true;
  m_initialized = false;
  m_failed = false;
  m_tiles.clear();
  m_sharedAccelerator.reset();
  m_status.last_tile_us_p99 = 0u;
  m_status.last_build_us = 0u;
  m_lastError.clear();
  return vkpt::core::Status::ok("tiled CPU scene loaded");
}

vkpt::core::Status TiledCpuPathTracer::build_or_update_acceleration() {
  if (!m_configured || !m_hasScene) {
    m_failed = true;
    m_lastError = !m_configured
                      ? "build_or_update_acceleration called before configure"
                      : "build_or_update_acceleration called before load_scene_snapshot";
    VKP_LOG(Warn,
            "cpu",
            "acceleration_failed",
            "reason",
            !m_configured ? "not_configured" : "no_scene",
            "flow_id",
            CurrentFlowId(m_flowSource));
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_lastError);
  }
  const bool deterministic = m_config.deterministic || m_settings.deterministic;
  if (m_externalAccelerator) {
    // External accelerators are rebuilt in place so callers can own lifetime
    // while tiles still receive the same shared ICpuRayKernel pointer.
    m_sharedAccelerator.reset();
    if (!m_externalAccelerator->build(m_scene, deterministic)) {
      m_bvhStats = {};
      m_status.last_build_us = 0u;
      m_initialized = false;
      m_failed = true;
      m_lastError = "external CPU accelerator build failed";
      VKP_LOG(Warn,
              "cpu",
              "acceleration_failed",
              "reason",
              "external_build_failed",
              "flow_id",
              CurrentFlowId(m_flowSource));
      return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                       m_lastError);
    }
    m_bvhStats = to_bvh_build_stats(m_externalAccelerator->build_info(), worker_count());
  } else {
    // Default path: create one BVH accelerator and share it across all tile
    // tracers instead of rebuilding a per-tile copy.
    m_sharedAccelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
    if (!m_sharedAccelerator || !m_sharedAccelerator->build(m_scene, deterministic)) {
      m_sharedAccelerator.reset();
      m_bvhStats = {};
      m_status.last_build_us = 0u;
      m_initialized = false;
      m_failed = true;
      m_lastError = "shared CPU accelerator build failed";
      VKP_LOG(Warn,
              "cpu",
              "acceleration_failed",
              "reason",
              "shared_build_failed",
              "flow_id",
              CurrentFlowId(m_flowSource));
      return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                       m_lastError);
    }
    m_bvhStats = to_bvh_build_stats(m_sharedAccelerator->build_info(), worker_count());
  }
  m_status.last_build_us = build_us_from_stats(m_bvhStats);

  // Recreate tile tracers after acceleration changes so each tile sees the
  // current scene snapshot, camera, and accelerator pointer.
  init_tile_tracers();
  if (!m_initialized) {
    m_failed = true;
    m_lastError = "tiled CPU tile tracer initialization failed";
    VKP_LOG(Warn,
            "cpu",
            "tile_init_failed",
            "flow_id",
            CurrentFlowId(m_flowSource));
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                     m_lastError);
  }
  m_failed = false;
  m_lastError.clear();
  VKP_LIFECYCLE_STARTED("cpu",
                        "backend",
                        "tiled-cpu",
                        "kernel",
                        m_status.kernel,
                        "tile_count",
                        static_cast<std::uint64_t>(m_tiles.size()),
                        "flow_id",
                        CurrentFlowId(m_flowSource));
  return vkpt::core::Status::ok("tiled CPU acceleration ready");
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
  if (!m_configured) {
    m_lastError = "reset_accumulation called before configure";
    return false;
  }
  m_film.clear();
  m_counters = {};
  m_currentSample = 0u;
  ++m_accumulationGen;
  for (auto& tile : m_tiles) {
    if (tile.tracer) {
      tile.tracer->reset_accumulation();
    }
  }
  m_lastError.clear();
  return true;
}

bool TiledCpuPathTracer::replace_film_history(
    const vkpt::pathtracer::FilmBuffer& film) {
  if (m_settings.width != film.width() || m_settings.height != film.height()) {
    return false;
  }
  bool ok = m_film.copy_from(film);
  for (auto& tile : m_tiles) {
    if (!tile.tracer) {
      continue;
    }
    ok = tile.tracer->replace_film_history(film) && ok;
  }
  return ok;
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
      m_status.last_build_us = 0u;
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
      m_status.last_build_us = 0u;
      return false;
    }
    activeAccelerator = m_sharedAccelerator.get();
    m_bvhStats = to_bvh_build_stats(m_sharedAccelerator->build_info(), worker_count());
  }
  m_status.last_build_us = build_us_from_stats(m_bvhStats);

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

vkpt::pathtracer::InstanceTransformPlan
TiledCpuPathTracer::plan_instance_transform_update(
    std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
    const vkpt::pathtracer::InstanceTransformUpdateOptions& options) const {
  if (!m_initialized) {
    return {vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
            static_cast<std::uint32_t>(updates.size()),
            0u,
            "tiled CPU tracer is not initialized"};
  }

  std::uint32_t matched = 0u;
  for (const auto& update : updates) {
    std::uint32_t instanceIndex = update.instance_index;
    if (instanceIndex >= m_scene.instances.size() && update.entity_id != 0u) {
      for (std::size_t index = 0; index < m_scene.instances.size(); ++index) {
        if (m_scene.instances[index].entity_id == update.entity_id) {
          instanceIndex = static_cast<std::uint32_t>(index);
          break;
        }
      }
    }
    if (instanceIndex >= m_scene.instances.size()) {
      return {vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
              static_cast<std::uint32_t>(updates.size()),
              matched,
              "tiled CPU transform update references an unknown instance"};
    }
    ++matched;
  }

  const vkpt::pathtracer::IRayAccelerator* activeAccelerator =
      m_externalAccelerator ? m_externalAccelerator : m_sharedAccelerator.get();
  if (activeAccelerator != nullptr) {
    const auto acceleratorPlan =
        activeAccelerator->plan_instance_transform_update(m_scene, updates, options);
    if (acceleratorPlan.can_apply_without_full_fallback() ||
        acceleratorPlan.status != vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported) {
      return acceleratorPlan;
    }
  }

  return {vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild,
          static_cast<std::uint32_t>(updates.size()),
          matched,
          "tiled CPU tracer rebuilds its shared BVH for instance transform updates"};
}

vkpt::pathtracer::InstanceTransformUpdateResult
TiledCpuPathTracer::apply_instance_transform_update(
    std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
    const vkpt::pathtracer::InstanceTransformUpdateOptions& options) {
  const auto plan = plan_instance_transform_update(updates, options);
  if (plan.can_apply_without_full_fallback()) {
    vkpt::pathtracer::IRayAccelerator* activeAccelerator =
        m_externalAccelerator ? m_externalAccelerator : m_sharedAccelerator.get();
    if (activeAccelerator == nullptr) {
      return {vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported,
              static_cast<std::uint32_t>(updates.size()),
              0u,
              0.0,
              0.0,
              0.0,
              0.0,
              "tiled CPU tracer has no active accelerator"};
    }
    const auto result =
        activeAccelerator->apply_instance_transform_update(m_scene, updates, options);
    if (!result.applied()) {
      return result;
    }
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updateVec(
        updates.begin(),
        updates.end());
    if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(
            m_scene,
            updateVec,
            vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly)) {
      return {vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
              static_cast<std::uint32_t>(updates.size()),
              0u,
              0.0,
              0.0,
              0.0,
              0.0,
              "tiled CPU tracer failed to commit transform metadata after accelerator refit"};
    }
    for (auto& tile : m_tiles) {
      if (tile.tracer) {
        (void)tile.tracer->load_scene_snapshot(m_scene);
      }
    }
    const auto info = activeAccelerator->build_info();
    m_bvhStats = to_bvh_build_stats(info, worker_count());
    return result;
  }

  if (!vkpt::pathtracer::TransformUpdateStatusAllowedByPolicy(
          plan.status,
          options.fallback_policy)) {
    return {plan.status,
            plan.requested_count,
            0u,
            0.0,
            0.0,
            0.0,
            0.0,
            plan.message};
  }

  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updateVec(
      updates.begin(),
      updates.end());
  if (!update_instance_transforms(updateVec)) {
    return {vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
            static_cast<std::uint32_t>(updates.size()),
            0u,
            0.0,
            0.0,
            0.0,
            0.0,
            "tiled CPU transform update rebuild failed"};
  }
  return {vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild,
          static_cast<std::uint32_t>(updates.size()),
          static_cast<std::uint32_t>(updates.size()),
          0.0,
          0.0,
          0.0,
          static_cast<double>(m_bvhStats.build_ms),
          "tiled CPU transform update rebuilt shared BVH"};
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

bool TiledCpuPathTracer::render_tile(
    const vkpt::pathtracer::RenderTile& tile,
    uint32_t frame_index) {
  return render_tile_cancellable(tile, frame_index, {});
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

  const uint32_t maxY = std::min(end_y, m_settings.height);
  const uint32_t minY = std::min(start_y, maxY);
  if (minY >= maxY || m_settings.width == 0u) {
    return true;
  }

  vkpt::pathtracer::RenderTile tile;
  tile.x = 0u;
  tile.y = minY;
  tile.width = m_settings.width;
  tile.height = maxY - minY;
  tile.sample_index = sample_index;
  tile.tile_id = minY / std::max(1u, m_config.tile_height);
  return render_tile_cancellable(tile, frame_index, stop);
}

bool TiledCpuPathTracer::render_tile_cancellable(
    const vkpt::pathtracer::RenderTile& tile,
    uint32_t frame_index,
    std::stop_token stop) {
  const auto sampleStartUs = vkpt::pathtracer::observability::NowUs();
  auto finishSample = [&](bool result) {
    VKP_METRIC_OBSERVE("vkp.pathtracer.sample_us",
                       vkpt::pathtracer::observability::ElapsedUsSince(sampleStartUs));
    return result;
  };

  if (tile.width == 0u || tile.height == 0u) {
    return finishSample(true);
  }
  if (!m_initialized) {
    m_lastError = "render_tile called before CPU acceleration is ready";
    VKP_LOG(Warn,
            "cpu",
            "tile_failed",
            "reason",
            "not_ready",
            "tile_id",
            static_cast<std::uint64_t>(tile.tile_id),
            "flow_id",
            CurrentFlowId(m_flowSource));
    return finishSample(false);
  }
  if (stop.stop_requested()) {
    m_lastError = "render_tile cancelled";
    return finishSample(false);
  }

  const uint32_t x0 = std::min(tile.x, m_settings.width);
  const uint32_t y0 = std::min(tile.y, m_settings.height);
  const uint32_t x1 = std::min<std::uint32_t>(m_settings.width, x0 + tile.width);
  const uint32_t y1 = std::min<std::uint32_t>(m_settings.height, y0 + tile.height);
  if (x0 >= x1 || y0 >= y1) {
    return finishSample(true);
  }

  std::vector<vkpt::core::RuntimeHandle> handles;
  handles.reserve(m_tiles.size());
  std::vector<std::uint64_t> tileDurationsUs(m_tiles.size(), 0u);
  std::vector<std::size_t> activeTileIndices;
  activeTileIndices.reserve(m_tiles.size());
  std::atomic_bool childOk{true};

  for (std::size_t tileIndex = 0u; tileIndex < m_tiles.size(); ++tileIndex) {
    auto& tileState = m_tiles[tileIndex];
    const uint32_t tile_start = std::max(tileState.start_y, y0);
    const uint32_t tile_end = std::min(tileState.end_y, y1);
    if (tile_start >= tile_end) {
      continue;
    }
    activeTileIndices.push_back(tileIndex);
    vkpt::pathtracer::RenderTile clipped = tile;
    clipped.x = x0;
    clipped.y = tile_start;
    clipped.width = x1 - x0;
    clipped.height = tile_end - tile_start;
    auto handle = m_jobSystem->submit_job([&tileState,
                                           clipped,
                                           frame_index,
                                           stop,
                                           &childOk,
                                           &tileDurationsUs,
                                           tileIndex]() {
      const auto tileStartUs = vkpt::pathtracer::observability::NowUs();
      if (stop.stop_requested()) {
        tileDurationsUs[tileIndex] =
            vkpt::pathtracer::observability::ElapsedUsSince(tileStartUs);
        VKP_METRIC_OBSERVE("vkp.cpu.tile_render_us", tileDurationsUs[tileIndex]);
        return;
      }
      if (!tileState.tracer ||
          !tileState.tracer->render_tile_cancellable(clipped, frame_index, stop)) {
        childOk.store(false, std::memory_order_release);
      }
      tileDurationsUs[tileIndex] =
          vkpt::pathtracer::observability::ElapsedUsSince(tileStartUs);
      VKP_METRIC_OBSERVE("vkp.cpu.tile_render_us", tileDurationsUs[tileIndex]);
    });
    handles.push_back(handle);
  }

  if (!m_jobSystem->wait_group(handles, stop) ||
      stop.stop_requested() ||
      !childOk.load(std::memory_order_acquire)) {
    m_lastError = stop.stop_requested() ? "render_tile cancelled"
                                        : "child CPU tile render failed";
    VKP_LOG(Warn,
            "cpu",
            "tile_failed",
            "reason",
            stop.stop_requested() ? "cancelled" : "child_failed",
            "tile_id",
            static_cast<std::uint64_t>(tile.tile_id),
            "flow_id",
            CurrentFlowId(m_flowSource));
    return finishSample(false);
  }
  m_status.last_tile_us_p99 =
      percentile99_us(tileDurationsUs, activeTileIndices);
  const auto mergeStartUs = vkpt::pathtracer::observability::NowUs();
  merge_tiles();
  VKP_METRIC_OBSERVE("vkp.cpu.tile_merge_us",
                     vkpt::pathtracer::observability::ElapsedUsSince(mergeStartUs));
  m_currentSample = std::max(m_currentSample, tile.sample_index + 1u);
  m_lastError.clear();
  return finishSample(true);
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

vkpt::pathtracer::PathTracerStatus TiledCpuPathTracer::status() const {
  const auto cpu = cpu_status();
  vkpt::pathtracer::PathTracerStatus out;
  out.backend = cpu.backend;
  out.lifecycle = path_lifecycle_from_cpu(cpu.lifecycle);
  out.scene_loaded = cpu.scene_loaded;
  out.accel_valid = cpu.accel_valid;
  out.ready_to_render = cpu.ready_to_render;
  out.current_sample = cpu.current_sample;
  out.total_samples = cpu.total_samples;
  out.accumulation_gen = m_accumulationGen;
  out.last_error = cpu.last_error;
  return out;
}

CpuPathTracerStatus TiledCpuPathTracer::cpu_status() const {
  auto out = m_status;
  const auto counters = read_counters();
  const auto context = m_config.determinism_context();
  out.backend = "tiled-cpu";
  out.lifecycle =
      lifecycle_from_state(m_configured, m_hasScene, m_initialized, m_failed);
  out.configured = m_configured;
  out.scene_loaded = m_hasScene;
  out.accel_valid = m_initialized;
  out.ready_to_render = m_configured && m_hasScene && m_initialized && !m_failed;
  out.deterministic = context.enabled;
  out.determinism_base_seed = context.base_seed;
  out.determinism_frame_index = context.frame_index;
  out.determinism_scenario_id = context.scenario_id;
  out.current_flow_id = CurrentFlowId(m_flowSource);
  out.current_sample = m_currentSample;
  out.total_samples = counters.samples;
  out.total_rays = counters.rays;
  out.worker_count = worker_count();
  out.tile_height = m_config.tile_height;
  out.tile_count = m_tiles.size();
  out.last_error = m_lastError;
  const auto report = EvaluateCpuPathTracerHealth(out);
  switch (report.status) {
    case vkpt::core::health::Status::Ok:
      out.health = vkpt::core::contracts::SubsystemHealth::Ok;
      break;
    case vkpt::core::health::Status::Degraded:
      out.health = vkpt::core::contracts::SubsystemHealth::Degraded;
      break;
    case vkpt::core::health::Status::Failed:
      out.health = vkpt::core::contracts::SubsystemHealth::Failed;
      break;
  }
  out.health_reason = report.reason;
  return out;
}

void TiledCpuPathTracer::shutdown() {
  const bool wasConfigured = m_configured || m_hasScene || m_initialized;
  m_tiles.clear();
  m_initialized = false;
  m_configured = false;
  m_hasScene = false;
  m_failed = false;
  m_externalAccelerator = nullptr;
  m_sharedAccelerator.reset();
  m_scene = {};
  m_film = {};
  m_counters = {};
  m_currentSample = 0u;
  m_lastError.clear();
  if (m_jobSystem) {
    m_jobSystem->shutdown();
  }
  if (wasConfigured) {
    VKP_LIFECYCLE_STOPPED("cpu",
                          "backend",
                          "tiled-cpu",
                          "flow_id",
                          CurrentFlowId(m_flowSource));
  }
}

std::size_t TiledCpuPathTracer::worker_count() const {
  return m_jobSystem ? m_jobSystem->worker_count() : 0u;
}

}  // namespace vkpt::cpu
