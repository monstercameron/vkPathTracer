// Drag-throughput smoke for the D3D12 path tracer.
//
// Loads cornell_native.json, finds the first dynamic-transform mesh instance
// (the glossy cuboid in the default scene), and times the two phases that
// together drive editor "drag" interactivity:
//
//   * REST phase: render N full-quality samples back-to-back, no transform
//     updates. Measures the steady-state cost of one sample at the chosen
//     resolution / spp / depth.
//
//   * DRAG phase: every iteration, post a synthetic instance-transform update
//     for the picked instance through apply_instance_transform_update (the
//     exact call the editor's gizmo flush invokes), then render one sample.
//     Each iteration's apply / render / reset_accumulation timings are
//     recorded separately so we can see where the millisecond budget goes.
//
// The output is a per-phase median plus a tagged breakdown of the heaviest
// per-iteration step. Run with PTDRAG_WIDTH / PTDRAG_HEIGHT / PTDRAG_FRAMES
// env vars to match the editor's viewport size if it differs from default.
//
// This test is intentionally single-threaded against the raw IPathTracer —
// it bypasses RenderCoordinator's command queue, FrameHandoff, and the Qt
// main loop. That keeps the measurement focused on backend cost: any
// divergence between this test's drag-fps and the editor's drag-fps is
// orchestration overhead (post() coalescing, Qt event flush, frame handoff).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "gpu/D3D12GpuPathTracer.h"
#include "pathtracer/PathTracer.h"
#include "pathtracer/SceneConversion.h"
#include "render/RenderCoordinator.h"
#include "scene/SceneDocument.h"

#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

double Median(std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

double Percentile(std::vector<double>& samples, double pct) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const auto idx =
      std::min<std::size_t>(samples.size() - 1, static_cast<std::size_t>(samples.size() * pct));
  return samples[idx];
}

std::uint32_t EnvU32(const char* name, std::uint32_t fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return fallback;
  }
  try {
    return static_cast<std::uint32_t>(std::stoul(raw));
  } catch (...) {
    return fallback;
  }
}

std::string EnvStr(const char* name, const char* fallback) {
  const char* raw = std::getenv(name);
  return raw ? std::string(raw) : std::string(fallback);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenePath = EnvStr(
      "PTDRAG_SCENE", argc > 1 ? argv[1] : "assets/scenes/cornell_native.json");
  const std::string hlslPath = EnvStr(
      "PTDRAG_HLSL", "src/shaders/gpu/pathtrace_cs.hlsl");
  const std::uint32_t width = EnvU32("PTDRAG_WIDTH", 1280u);
  const std::uint32_t height = EnvU32("PTDRAG_HEIGHT", 720u);
  const std::uint32_t restFrames = EnvU32("PTDRAG_REST_FRAMES", 30u);
  const std::uint32_t dragFrames = EnvU32("PTDRAG_DRAG_FRAMES", 60u);
  const std::uint32_t maxDepth = EnvU32("PTDRAG_MAX_DEPTH", 6u);

  std::cout << "scene=" << scenePath << "\n"
            << "hlsl=" << hlslPath << "\n"
            << "size=" << width << "x" << height << "\n"
            << "rest_frames=" << restFrames << " drag_frames=" << dragFrames << "\n"
            << "max_depth=" << maxDepth << "\n";

  if (!std::filesystem::exists(scenePath)) {
    std::cerr << "scene file not found: " << scenePath << "\n";
    return 2;
  }
  if (!std::filesystem::exists(hlslPath)) {
    std::cerr << "shader file not found: " << hlslPath << "\n";
    return 2;
  }

  const auto sceneResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!sceneResult) {
    std::cerr << "scene load failed\n";
    return 2;
  }
  auto sceneData = vkpt::pathtracer::BuildSceneDataFromDocument(sceneResult.value());
  if (!sceneData) {
    std::cerr << "scene conversion failed\n";
    return 2;
  }

  // Pick the first dynamic-transform instance — that is the path the editor
  // gizmo drives in the cornell scene (the "glossy cuboid" or any mesh entity
  // with a transform).
  std::uint32_t targetInstance = vkpt::pathtracer::kInvalidRTInstanceIndex;
  for (std::size_t i = 0; i < sceneData.value().instances.size(); ++i) {
    if (sceneData.value().instances[i].has_flag(
            vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      targetInstance = static_cast<std::uint32_t>(i);
      break;
    }
  }
  if (targetInstance == vkpt::pathtracer::kInvalidRTInstanceIndex) {
    std::cerr << "scene has no dynamic-transform instance — drag would route through "
                 "full-acceleration fallback. That alone could be the bug.\n";
    return 3;
  }
  const auto& target = sceneData.value().instances[targetInstance];
  std::cout << "target_instance=" << targetInstance
            << " entity_id=" << target.entity_id
            << " local_vtx=" << target.local_vertex_count
            << " local_idx=" << target.local_index_count
            << " flags=0x" << std::hex << target.flags << std::dec << "\n";

  auto tracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
  if (!tracer->is_valid()) {
    std::cerr << "tracer init failed: " << tracer->last_error() << "\n";
    return 2;
  }
  std::cout << "gpu=" << tracer->gpu_name() << " dxr=" << tracer->dxr_tier_string() << "\n";

  vkpt::pathtracer::RenderSettings settings;
  settings.width = width;
  settings.height = height;
  settings.spp = 1u << 30;  // effectively unbounded
  settings.max_depth = maxDepth;
  settings.seed = 0x1234abcdULL;
  settings.enable_nee = true;
  settings.enable_mis = true;
  settings.enable_denoiser = true;
  settings.enable_temporal_aa = true;

  if (!tracer->configure(settings) ||
      !tracer->load_scene_snapshot(sceneData.value()) ||
      !tracer->build_or_update_acceleration() ||
      !tracer->reset_accumulation()) {
    std::cerr << "tracer setup failed: " << tracer->last_error() << "\n";
    return 2;
  }

  // Exercise the editor toggle path before warm-up so timings stay comparable.
  auto toggledSettings = settings;
  toggledSettings.enable_denoiser = false;
  toggledSettings.enable_temporal_aa = false;
  if (!tracer->update_render_settings(toggledSettings) ||
      !tracer->reset_accumulation() ||
      !tracer->render_sample_batch(0u, height, 0u, 0u)) {
    std::cerr << "toggle-off render settings smoke failed: "
              << tracer->last_error() << "\n";
    return 2;
  }
  if (!tracer->update_render_settings(settings) ||
      !tracer->reset_accumulation() ||
      !tracer->render_sample_batch(0u, height, 0u, 0u)) {
    std::cerr << "toggle-on render settings smoke failed: "
              << tracer->last_error() << "\n";
    return 2;
  }

  // ---- Warm-up: render a couple of full samples so PSO/heap creation and any
  // first-call DXR setup don't skew the first measured frame.
  for (std::uint32_t i = 0; i < 2u; ++i) {
    if (!tracer->render_sample_batch(0u, height, i, 0u)) {
      std::cerr << "warmup render failed: " << tracer->last_error() << "\n";
      return 2;
    }
  }

  // ---- REST phase: full-quality samples, no transform updates.
  std::vector<double> restSampleMs;
  restSampleMs.reserve(restFrames);
  tracer->reset_accumulation();
  for (std::uint32_t i = 0; i < restFrames; ++i) {
    const auto t0 = Clock::now();
    if (!tracer->render_sample_batch(0u, height, i, 0u)) {
      std::cerr << "rest render failed at sample " << i << ": "
                << tracer->last_error() << "\n";
      return 2;
    }
    const auto t1 = Clock::now();
    restSampleMs.push_back(Ms(t1 - t0));
  }

  // ---- DRAG phase: every iteration emits a new transform update for the
  // picked instance (translating it in a small loop) and renders one sample.
  // This mirrors what the editor's gizmo flush triggers per frame.
  std::vector<double> dragApplyMs;
  std::vector<double> dragResetMs;
  std::vector<double> dragRenderMs;
  std::vector<double> dragTotalMs;
  dragApplyMs.reserve(dragFrames);
  dragResetMs.reserve(dragFrames);
  dragRenderMs.reserve(dragFrames);
  dragTotalMs.reserve(dragFrames);

  vkpt::pathtracer::Vec3 baseTranslation = target.translation;
  for (std::uint32_t i = 0; i < dragFrames; ++i) {
    const float phase = 0.005f * static_cast<float>(i + 1u);
    vkpt::pathtracer::RTInstanceTransformUpdate update{};
    update.entity_id = target.entity_id;
    update.instance_index = targetInstance;
    update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                   vkpt::pathtracer::kRTInstanceFlagTransformDirty;
    update.transform_revision = i + 1u;
    update.translation = {baseTranslation.x + phase, baseTranslation.y, baseTranslation.z};
    update.rotation = target.rotation;
    update.scale = target.scale;

    auto options = vkpt::pathtracer::MakeStandardTransformUpdateOptions(
        vkpt::pathtracer::RenderUpdateReason::EditorGizmoMotion,
        i + 1u,
        "drag-throughput-test");

    const std::array<vkpt::pathtracer::RTInstanceTransformUpdate, 1> updates{update};

    const auto t0 = Clock::now();
    const auto applyResult = tracer->apply_instance_transform_update(updates, options);
    const auto t1 = Clock::now();
    if (!applyResult.applied()) {
      std::cerr << "drag apply failed at frame " << i
                << " status=" << static_cast<int>(applyResult.status)
                << " msg=" << applyResult.message << "\n";
      return 2;
    }
    if (options.reset_accumulation && !tracer->reset_accumulation()) {
      std::cerr << "drag reset_accumulation failed at frame " << i << "\n";
      return 2;
    }
    const auto t2 = Clock::now();
    if (!tracer->render_sample_batch(0u, height, i, 0u)) {
      std::cerr << "drag render failed at frame " << i << ": "
                << tracer->last_error() << "\n";
      return 2;
    }
    const auto t3 = Clock::now();

    dragApplyMs.push_back(Ms(t1 - t0));
    dragResetMs.push_back(Ms(t2 - t1));
    dragRenderMs.push_back(Ms(t3 - t2));
    dragTotalMs.push_back(Ms(t3 - t0));
  }

  auto report = [](const char* label, std::vector<double>& xs) {
    auto cp = xs;
    const auto med = Median(cp);
    const auto p95 = Percentile(cp, 0.95);
    auto sum = 0.0;
    for (double v : xs) sum += v;
    const auto avg = xs.empty() ? 0.0 : sum / static_cast<double>(xs.size());
    std::cout << "  " << std::setw(16) << std::left << label
              << " median=" << std::setw(7) << std::fixed << std::setprecision(2) << med
              << "ms  p95=" << std::setw(7) << p95
              << "ms  avg=" << std::setw(7) << avg
              << "ms  fps@median=" << std::setw(6) << std::setprecision(1)
              << (med > 0.0 ? 1000.0 / med : 0.0) << "\n";
  };

  std::cout << "\n=== REST (full sample, no transform updates) ===\n";
  report("sample", restSampleMs);

  std::cout << "\n=== DRAG (apply + reset + render per frame) ===\n";
  report("apply_xform", dragApplyMs);
  report("reset_accum", dragResetMs);
  report("render", dragRenderMs);
  report("iteration", dragTotalMs);

  // Identify the dominant cost so the user can see the bottleneck at a glance.
  auto medCp1 = dragApplyMs;
  auto medCp2 = dragResetMs;
  auto medCp3 = dragRenderMs;
  const double dominantApply = Median(medCp1);
  const double dominantReset = Median(medCp2);
  const double dominantRender = Median(medCp3);
  const char* dominantLabel = "render";
  double dominantValue = dominantRender;
  if (dominantApply > dominantValue) {
    dominantValue = dominantApply;
    dominantLabel = "apply_xform";
  }
  if (dominantReset > dominantValue) {
    dominantValue = dominantReset;
    dominantLabel = "reset_accum";
  }
  std::cout << "\ndominant_per_frame=" << dominantLabel
            << " (" << std::fixed << std::setprecision(2) << dominantValue << "ms median)\n";

  tracer->shutdown();
  tracer.reset();

  // ---- ORCHESTRATED phase ------------------------------------------------
  // Drive a fresh tracer through RenderCoordinator the same way the editor
  // does: producer thread posts a transform every ~publishInterval, consumer
  // thread acquires the latest published frame at the same cadence. This
  // catches any cost the bare-tracer phases miss — mutex contention on the
  // command queue, FrameHandoff drops, frame-acquire blocking.
  const std::uint32_t orchFrames = EnvU32("PTDRAG_ORCH_FRAMES", 120u);
  const std::uint32_t orchHz = EnvU32("PTDRAG_ORCH_HZ", 60u);
  if (orchFrames > 0u) {
    auto coordinatorTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
    if (!coordinatorTracer->is_valid()) {
      std::cerr << "orchestrated tracer init failed: "
                << coordinatorTracer->last_error() << "\n";
      return 2;
    }

    vkpt::render::RenderCoordinatorConfig coordConfig{};
    coordConfig.publish_hz = orchHz;
    coordConfig.immediate_publish_count = 4u;

    vkpt::render::RenderCoordinator coordinator(std::move(coordinatorTracer),
                                                settings,
                                                sceneData.value(),
                                                coordConfig);
    if (!coordinator.start()) {
      std::cerr << "coordinator start failed: " << coordinator.stats().error << "\n";
      return 2;
    }

    // Let the worker stabilize and produce at least one frame before posting drags.
    for (int waitMs = 0; waitMs < 1000; waitMs += 10) {
      if (coordinator.acquire_latest_frame()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto interval = std::chrono::microseconds(1'000'000u / std::max(1u, orchHz));
    std::vector<double> orchPostMs;
    std::vector<double> orchAcquireMs;
    std::vector<double> orchTickMs;
    std::vector<bool> orchAcquired;
    orchPostMs.reserve(orchFrames);
    orchAcquireMs.reserve(orchFrames);
    orchTickMs.reserve(orchFrames);
    orchAcquired.reserve(orchFrames);
    std::uint32_t framesGotten = 0u;
    std::uint64_t lastFrameId = 0u;
    std::uint64_t framesAdvanced = 0u;

    auto orchStart = Clock::now();
    auto nextTick = orchStart;
    for (std::uint32_t i = 0; i < orchFrames; ++i) {
      nextTick += interval;
      const float phase = 0.005f * static_cast<float>(i + 1u);
      vkpt::pathtracer::RTInstanceTransformUpdate update{};
      update.entity_id = target.entity_id;
      update.instance_index = targetInstance;
      update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                     vkpt::pathtracer::kRTInstanceFlagTransformDirty;
      update.transform_revision = i + 1u;
      update.translation = {baseTranslation.x + phase, baseTranslation.y, baseTranslation.z};
      update.rotation = target.rotation;
      update.scale = target.scale;

      auto options = vkpt::pathtracer::MakeStandardTransformUpdateOptions(
          vkpt::pathtracer::RenderUpdateReason::EditorGizmoMotion,
          i + 1u,
          "drag-throughput-coordinator");

      const auto tickStart = Clock::now();
      coordinator.post_instance_transforms({update}, options);
      const auto postEnd = Clock::now();

      auto frame = coordinator.acquire_latest_frame();
      const auto acquireEnd = Clock::now();
      if (frame) {
        ++framesGotten;
        if (frame->frame_id != lastFrameId) {
          ++framesAdvanced;
          lastFrameId = frame->frame_id;
        }
      }
      orchPostMs.push_back(Ms(postEnd - tickStart));
      orchAcquireMs.push_back(Ms(acquireEnd - postEnd));
      orchTickMs.push_back(Ms(acquireEnd - tickStart));
      orchAcquired.push_back(frame.has_value());

      if (Clock::now() < nextTick) {
        std::this_thread::sleep_until(nextTick);
      }
    }
    const auto orchEnd = Clock::now();
    coordinator.stop();

    std::cout << "\n=== ORCHESTRATED (RenderCoordinator + handoff, " << orchHz << "Hz tick) ===\n";
    report("post", orchPostMs);
    report("acquire", orchAcquireMs);
    report("tick", orchTickMs);
    const double wallSec = std::chrono::duration<double>(orchEnd - orchStart).count();
    const double advancedFps = wallSec > 0.0 ? static_cast<double>(framesAdvanced) / wallSec : 0.0;
    const auto coordStats = coordinator.stats();
    std::cout << "  ticks=" << orchFrames
              << " frames_acquired=" << framesGotten
              << " unique_frames=" << framesAdvanced
              << "  unique_fps=" << std::fixed << std::setprecision(1) << advancedFps
              << "\n  coord generation=" << coordStats.generation
              << " published=" << coordStats.handoff.published
              << " dropped=" << coordStats.handoff.dropped
              << " applied_xforms=" << coordStats.instance_transform_updates_applied
              << "\n";
  }

  return 0;
}
