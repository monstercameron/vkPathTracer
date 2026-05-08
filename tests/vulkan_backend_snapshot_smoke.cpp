#include "render/backends/VulkanBackend.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "render/interface/RenderContracts.h"
#include "scene/SnapshotRing.h"

#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename T>
vkpt::scene::CowArray<T> MakeCowArray(std::initializer_list<T> values) {
  if (values.size() == 0u) {
    return {};
  }
  T* raw = new T[values.size()];
  std::size_t index = 0u;
  for (const auto& value : values) {
    raw[index++] = value;
  }
  return vkpt::scene::CowArray<T>(
      std::shared_ptr<const T[]>(raw, std::default_delete<const T[]>()),
      values.size());
}

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "vulkan_backend_snapshot_smoke: " << message << "\n";
    return false;
  }
  return true;
}

std::string ReadFileText(const char* path) {
  std::ifstream input(path);
  std::string out;
  std::string line;
  while (std::getline(input, line)) {
    out += line;
    out.push_back('\n');
  }
  return out;
}

const vkpt::core::metrics::MetricSnapshot* FindMetric(
    const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
    std::string_view name,
    vkpt::core::metrics::Kind kind) {
  for (const auto& metric : metrics) {
    if (metric.name == name && metric.kind == kind) {
      return &metric;
    }
  }
  return nullptr;
}

vkpt::pathtracer::RTInstance MakeInstance(std::uint32_t entity_id,
                                          float x,
                                          std::uint32_t revision) {
  vkpt::pathtracer::RTInstance instance;
  instance.entity_id = entity_id;
  instance.geometry_id = 7u;
  instance.first_triangle = 0u;
  instance.triangle_count = 1u;
  instance.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
  instance.transform_revision = revision;
  instance.local_first_vertex = 0u;
  instance.local_vertex_count = 3u;
  instance.local_first_index = 0u;
  instance.local_index_count = 3u;
  instance.translation = {x, 0.0f, 0.0f};
  instance.scale = {1.0f, 1.0f, 1.0f};
  return instance;
}

vkpt::scene::RenderSceneSnapshot::Ptr MakeInitialSnapshot() {
  auto snapshot = std::make_shared<vkpt::scene::RenderSceneSnapshot>();
  snapshot->generation = 1u;
  snapshot->topology_revision = 1u;
  snapshot->transform_revision = 1u;
  snapshot->camera_revision = 1u;
  snapshot->material_revision = 1u;
  snapshot->vertices = MakeCowArray<vkpt::pathtracer::Vec3>({
      {-1.0f, 0.0f, -3.0f},
      {1.0f, 0.0f, -3.0f},
      {0.0f, 1.0f, -3.0f}});
  snapshot->indices = MakeCowArray<std::uint32_t>({0u, 1u, 2u});
  snapshot->local_vertices = snapshot->vertices;
  snapshot->local_indices = snapshot->indices;
  snapshot->instances = MakeCowArray<vkpt::pathtracer::RTInstance>({
      MakeInstance(42u, 0.0f, 1u)});
  return snapshot;
}

vkpt::scene::RenderSceneSnapshot::Ptr MakeStaticGeneration(
    const vkpt::scene::RenderSceneSnapshot& previous,
    std::uint64_t generation) {
  auto snapshot = std::make_shared<vkpt::scene::RenderSceneSnapshot>(previous);
  snapshot->generation = generation;
  snapshot->build_stats.cow_reused_arrays = 8u;
  snapshot->build_stats.cow_total_arrays = 8u;
  snapshot->acceleration.reused_from_previous = true;
  return snapshot;
}

vkpt::scene::RenderSceneSnapshot::Ptr MakeCameraGeneration(
    const vkpt::scene::RenderSceneSnapshot& previous) {
  auto snapshot = std::make_shared<vkpt::scene::RenderSceneSnapshot>(previous);
  snapshot->generation = previous.generation + 1u;
  snapshot->camera_revision = previous.camera_revision + 1u;
  snapshot->camera.position.x += 0.25f;
  snapshot->build_stats.cow_reused_arrays = 8u;
  snapshot->build_stats.cow_total_arrays = 8u;
  snapshot->acceleration.reused_from_previous = true;
  return snapshot;
}

vkpt::scene::RenderSceneSnapshot::Ptr MakeTransformGeneration(
    const vkpt::scene::RenderSceneSnapshot& previous) {
  auto snapshot = std::make_shared<vkpt::scene::RenderSceneSnapshot>(previous);
  snapshot->generation = previous.generation + 1u;
  snapshot->transform_revision = previous.transform_revision + 1u;
  const auto moved = MakeInstance(42u, 2.0f, 2u);
  snapshot->instances = MakeCowArray<vkpt::pathtracer::RTInstance>({moved});

  vkpt::scene::RenderInstanceMotion motion;
  motion.entity_id = 42u;
  motion.instance_index = 0u;
  motion.previous_valid = true;
  motion.previous = previous.instances[0];
  motion.current = moved;
  snapshot->instance_motion = MakeCowArray<vkpt::scene::RenderInstanceMotion>({motion});
  snapshot->acceleration.reused_from_previous = true;
  snapshot->acceleration.transform_refit_descriptor = true;
  return snapshot;
}

vkpt::scene::RenderSceneSnapshot::Ptr MakeTopologyGeneration(
    const vkpt::scene::RenderSceneSnapshot& previous) {
  auto snapshot = std::make_shared<vkpt::scene::RenderSceneSnapshot>(previous);
  snapshot->generation = previous.generation + 1u;
  snapshot->topology_revision = previous.topology_revision + 1u;
  snapshot->vertices = MakeCowArray<vkpt::pathtracer::Vec3>({
      {-1.0f, 0.0f, -3.0f},
      {1.0f, 0.0f, -3.0f},
      {0.0f, 1.0f, -3.0f},
      {0.0f, -1.0f, -3.0f}});
  snapshot->indices = MakeCowArray<std::uint32_t>({0u, 1u, 2u, 0u, 3u, 1u});
  snapshot->local_vertices = snapshot->vertices;
  snapshot->local_indices = snapshot->indices;
  snapshot->acceleration.reused_from_previous = false;
  snapshot->acceleration.transform_refit_descriptor = false;
  return snapshot;
}

bool CheckDeviceCommandSlots(vkpt::render::VulkanComputeBackend& backend) {
  auto device = backend.create_device();
  if (!Check(static_cast<bool>(device), "backend should create a simulated device") ||
      !Check(device->begin(), "device should begin")) {
    return false;
  }
  auto* vulkanDevice = dynamic_cast<vkpt::render::VulkanDevice*>(device.get());
  if (!Check(vulkanDevice != nullptr, "device should expose Vulkan diagnostics") ||
      !Check(vulkanDevice->command_buffer_count() == 2u,
             "device should default to double-buffer command contexts")) {
    device->end();
    return false;
  }

  auto c0 = device->create_command_context();
  auto c1 = device->create_command_context();
  auto c2 = device->create_command_context();
  auto* v0 = dynamic_cast<vkpt::render::VulkanCommandContext*>(c0.get());
  auto* v1 = dynamic_cast<vkpt::render::VulkanCommandContext*>(c1.get());
  auto* v2 = dynamic_cast<vkpt::render::VulkanCommandContext*>(c2.get());
  const bool ok = Check(v0 != nullptr && v1 != nullptr && v2 != nullptr,
                        "command contexts should expose Vulkan slot ids") &&
                  Check(v0->command_buffer_index() == 0u,
                        "first command context should use slot 0") &&
                  Check(v1->command_buffer_index() == 1u,
                        "second command context should use slot 1") &&
                  Check(v2->command_buffer_index() == 0u,
                        "third command context should reuse slot 0 after rotation");
  device->end();
  return ok;
}

bool CheckSnapshotBinding(vkpt::render::VulkanComputeBackend& backend) {
  vkpt::scene::SnapshotRing ring;
  const auto reader = ring.register_reader("vulkan-backend-smoke");
  if (!Check(reader != vkpt::scene::SnapshotRing::kInvalidReader,
             "snapshot reader should register")) {
    return false;
  }

  auto initial = MakeInitialSnapshot();
  ring.publish(initial);
  if (!Check(backend.bind_latest_snapshot(ring, reader),
             "backend should bind initial snapshot from ring")) {
    return false;
  }
  auto state = backend.snapshot_state();
  if (!Check(state.last_transition == vkpt::render::VulkanSnapshotTransitionKind::InitialUpload,
             "initial snapshot should classify as initial upload") ||
      !Check(state.reset_accumulation, "initial upload should reset accumulation")) {
    return false;
  }

  auto staticNext = MakeStaticGeneration(*initial, 2u);
  ring.publish(staticNext);
  if (!Check(backend.bind_latest_snapshot(ring, reader),
             "backend should bind static snapshot generation")) {
    return false;
  }
  state = backend.snapshot_state();
  if (!Check(state.last_transition == vkpt::render::VulkanSnapshotTransitionKind::Continue,
             "unchanged revisions should continue accumulation") ||
      !Check(state.geometry_storage_reused_from_previous,
             "static generation should report reused geometry storage") ||
      !Check(!state.reset_accumulation,
             "static generation should not reset accumulation")) {
    return false;
  }

  auto cameraNext = MakeCameraGeneration(*staticNext);
  ring.publish(cameraNext);
  if (!Check(backend.bind_latest_snapshot(ring, reader),
             "backend should bind camera-only snapshot")) {
    return false;
  }
  state = backend.snapshot_state();
  if (!Check(state.last_transition == vkpt::render::VulkanSnapshotTransitionKind::ReprojectCamera,
             "camera-only generation should classify as reprojection") ||
      !Check(state.geometry_storage_reused_from_previous,
             "camera-only generation should reuse geometry storage") ||
      !Check(!state.reset_accumulation,
             "camera-only generation should avoid full reset")) {
    return false;
  }

  auto transformNext = MakeTransformGeneration(*cameraNext);
  ring.publish(transformNext);
  if (!Check(backend.bind_latest_snapshot(ring, reader),
             "backend should bind transform-only snapshot")) {
    return false;
  }
  state = backend.snapshot_state();
  if (!Check(state.last_transition == vkpt::render::VulkanSnapshotTransitionKind::RefitTransforms,
             "transform-only generation should classify as refit") ||
      !Check(state.transform_refit_descriptor,
             "transform generation should expose refit descriptor") ||
      !Check(!state.reset_accumulation,
             "transform refit should avoid full reset in simulated backend")) {
    return false;
  }

  auto topologyNext = MakeTopologyGeneration(*transformNext);
  ring.publish(topologyNext);
  if (!Check(backend.bind_latest_snapshot(ring, reader),
             "backend should bind topology snapshot")) {
    return false;
  }
  state = backend.snapshot_state();
  return Check(state.last_transition == vkpt::render::VulkanSnapshotTransitionKind::RebuildScene,
               "topology generation should classify as rebuild") &&
         Check(state.reset_accumulation,
               "topology rebuild should reset accumulation") &&
         Check(!state.geometry_storage_reused_from_previous,
               "topology generation should not report reused geometry storage");
}

bool CheckTileTimeline(vkpt::render::VulkanComputeBackend& backend) {
  std::vector<vkpt::pathtracer::RenderTile> tiles;
  for (std::uint32_t id = 0u; id < 6u; ++id) {
    vkpt::pathtracer::RenderTile tile;
    tile.x = 0u;
    tile.y = id * 8u;
    tile.width = 64u;
    tile.height = 8u;
    tile.sample_index = 5u;
    tile.tile_id = id;
    tile.gpu_id = id % 2u;
    tiles.push_back(tile);
  }

  vkpt::render::VulkanTileBatchConfig config;
  config.max_tiles_in_flight = 3u;
  config.command_buffer_count = 3u;
  config.frame_index = 77u;
  const auto result = backend.submit_tile_batch(tiles, config);
  if (!Check(result.success, "tile batch should submit successfully") ||
      !Check(result.submitted_tiles == tiles.size(),
             "tile batch should submit every tile") ||
      !Check(result.first_timeline_value == 1u,
             "first tile should signal timeline value 1") ||
      !Check(result.last_submitted_timeline_value == 6u,
             "six tiles should advance timeline to 6") ||
      !Check(result.completed_timeline_value == 6u,
             "batch completion should catch up to submitted timeline") ||
      !Check(result.present_timeline_value == 6u,
             "present should wait on completed timeline value") ||
      !Check(result.max_observed_in_flight == 3u,
             "scheduler should keep three tiles in flight")) {
    return false;
  }

  for (std::size_t i = 0u; i < result.submissions.size(); ++i) {
    const auto& record = result.submissions[i];
    if (!Check(record.signal_timeline_value == i + 1u,
               "timeline signal values should be monotonic") ||
        !Check(record.command_buffer_index == static_cast<std::uint32_t>(i % 3u),
               "command buffer slots should rotate across the in-flight ring") ||
        !Check(record.snapshot_generation == backend.snapshot_state().generation,
               "tile records should stamp the bound snapshot generation") ||
        !Check(record.frame_index == 77u,
               "tile records should stamp frame index")) {
      return false;
    }
  }

  const auto timeline = backend.timeline_diagnostics();
  return Check(timeline.timeline_semaphore,
               "timeline diagnostics should advertise timeline semaphore simulation") &&
         Check(timeline.double_buffered_command_buffers,
               "timeline diagnostics should retain double-buffer command slot capability") &&
         Check(timeline.cpu_gpu_overlap_observed,
               "timeline diagnostics should observe CPU/GPU overlap") &&
         Check(timeline.total_tile_submissions == 6u,
               "timeline diagnostics should count tile submissions") &&
         Check(timeline.total_presents == 1u,
               "timeline diagnostics should count present waits");
}

bool CheckGpuTelemetry(vkpt::render::VulkanComputeBackend& backend) {
  using namespace vkpt::core::log;
  using namespace vkpt::core::metrics;
  (void)backend;

  vkpt::render::VulkanComputeBackend telemetryBackend;
  if (!Check(telemetryBackend.initialize(),
             "telemetry smoke backend should initialize")) {
    return false;
  }

  const char* logPath = "vulkan_gpu_telemetry_smoke.jsonl";
  std::remove(logPath);
  auto cleanup = [&]() {
    Logger::instance().shutdown();
    Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stdout));
    telemetryBackend.shutdown();
    std::remove(logPath);
  };
  Config config;
  config.min_level = Level::Debug;
  config.format = Format::Json;
  Logger::instance().start(config);
  Logger::instance().set_sink(std::make_unique<FileSink>(logPath));
  Logger::instance().set_min_level(Level::Debug);
  Logger::instance().set_format(Format::Json);
  MetricsRegistry::instance().reset("vkp.gpu.");

  vkpt::render::ComputePipelineDesc desc;
  desc.debug_label = "pathtrace.comp";
  desc.source_path = "src/shaders/gpu/pathtrace.comp";
  desc.source_format = vkpt::render::ShaderSourceFormat::Glsl;
  desc.entry_point = "main";
  std::string artifact;
  std::string diagnostics;
  if (!Check(telemetryBackend.compiler() != nullptr &&
                 telemetryBackend.compiler()->compile_compute_shader(desc, artifact, &diagnostics),
             "compiler should emit shader_compiled telemetry")) {
    cleanup();
    return false;
  }
  const std::string cacheKey = "vulkan-telemetry-cache-key";
  std::string cached;
  if (!Check(telemetryBackend.shader_cache() != nullptr &&
                 telemetryBackend.shader_cache()->store(cacheKey, artifact) &&
                 telemetryBackend.shader_cache()->query(cacheKey, cached),
             "shader cache should emit shader_cached telemetry")) {
    cleanup();
    return false;
  }

  if (!Check(telemetryBackend.bind_scene_snapshot(MakeInitialSnapshot()),
             "telemetry smoke should bind a snapshot")) {
    cleanup();
    return false;
  }
  vkpt::pathtracer::RenderTile tile;
  tile.width = 64u;
  tile.height = 8u;
  tile.sample_index = 1u;
  tile.tile_id = 99u;
  vkpt::render::VulkanTileBatchConfig configBatch;
  configBatch.frame_index = 12u;
  const auto batch = telemetryBackend.submit_tile_batch(
      std::span<const vkpt::pathtracer::RenderTile>(&tile, 1u),
      configBatch);
  if (!Check(batch.success, "telemetry smoke should submit one tile")) {
    cleanup();
    return false;
  }

  Logger::instance().flush_for_test();
  const auto gpuMetrics = MetricsRegistry::instance().snapshot_prefix("vkp.gpu.");
  const auto* fenceWait = FindMetric(gpuMetrics,
                                     "vkp.gpu.fence_wait_us",
                                     Kind::HistogramKind);
  const std::string logText = ReadFileText(logPath);
  const bool ok =
      Check(logText.find("\"comp\":\"gpu\"") != std::string::npos,
            "GPU telemetry log should contain gpu component") &&
      Check(logText.find("\"ev\":\"shader_compiled\"") != std::string::npos,
            "GPU telemetry log should contain shader_compiled") &&
      Check(logText.find("\"ev\":\"shader_cached\"") != std::string::npos,
            "GPU telemetry log should contain shader_cached") &&
      Check(logText.find("\"ev\":\"dispatch_submitted\"") != std::string::npos,
            "GPU telemetry log should contain dispatch_submitted") &&
      Check(logText.find("\"ev\":\"dispatch_completed\"") != std::string::npos,
            "GPU telemetry log should contain dispatch_completed") &&
      Check(fenceWait != nullptr && fenceWait->hist.count >= 1u,
            "GPU telemetry metrics should record fence wait histogram");
  cleanup();
  return ok;
}

bool CheckCapabilities(vkpt::render::VulkanComputeBackend& backend) {
  const auto caps = backend.capabilities();
  const std::string serialized = vkpt::render::SerializeBackendCapabilities(caps);
  return Check(caps.is_simulated, "backend should remain explicitly simulated") &&
         Check(caps.supports_multiqueue,
               "capabilities should expose simulated queue overlap") &&
         Check(caps.memory_model == "simulated-glsl-timeline",
               "capabilities should name timeline memory model") &&
         Check(serialized.find("multi-tile in-flight") != std::string::npos,
               "serialized caps should include multi-tile diagnostic note");
}

}  // namespace

int main() {
  vkpt::render::VulkanComputeBackend backend;
  if (!Check(backend.initialize(), "backend should initialize")) {
    return 1;
  }
  if (!CheckCapabilities(backend)) {
    return 1;
  }
  if (!CheckDeviceCommandSlots(backend)) {
    return 1;
  }
  if (!CheckSnapshotBinding(backend)) {
    return 1;
  }
  if (!CheckGpuTelemetry(backend)) {
    return 1;
  }
  if (!CheckTileTimeline(backend)) {
    return 1;
  }
  if (!Check(vkpt::render::RunVulkanComputeSmoke(backend),
             "legacy Vulkan compute smoke should still pass")) {
    return 1;
  }
  backend.shutdown();
  std::cout << "vulkan_backend_snapshot_smoke: ok\n";
  return 0;
}
