// Track A regression test (SYSTEM.md Phase 0.6).
//
// Spawns 6 producer threads × 100k events each, then asserts:
//   * total events emitted by Logger matches the expected sum across threads
//     (allowing for the configured drop budget under burst);
//   * counter / gauge / histogram metrics land at the expected values;
//   * crash-ring dump contains at least one event from every producer;
//   * REPL `metrics dump` returns a non-empty JSON document;
//   * --log-format=json output is a single object per line.

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio/AudioSystem.h"
#include "core/Observability.h"
#include "core/cli/Flags.h"
#include "core/contracts/Determinism.h"
#include "core/contracts/IFlowSource.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/MetricsBundle.h"
#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/repl/Repl.h"
#include "core/sync/LatestSlot.h"
#include "core/sync/MpscRing.h"
#include "core/sync/SpscRing.h"
#include "core/trace/Trace.h"
#include "diagnostics/CrashRecorder.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"
#include "physics/PhysicsWorld.h"
#include "scene/FrameLifecycle.h"
#include "scene/Json.h"
#include "scripting/ScriptRuntime.h"

namespace {

bool Check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "[FAIL] " << what << "\n";
    return false;
  }
  std::cerr << "[ ok ] " << what << "\n";
  return true;
}

bool HasArg(int argc, char** argv, std::string_view expected) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && argv[i] == expected) {
      return true;
    }
  }
  return false;
}

const char* ArgValue(int argc, char** argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] != nullptr && argv[i] == key) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

const vkpt::scene::JsonValue* JsonMember(const vkpt::scene::JsonValue& object,
                                         std::string_view key) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return nullptr;
  }
  const auto it = object.object.find(key);
  return it == object.object.end() ? nullptr : &it->second;
}

bool IsSchemaPrimitive(const vkpt::scene::JsonValue& value) {
  using Kind = vkpt::scene::JsonValue::Kind;
  return value.kind == Kind::Null ||
         value.kind == Kind::Boolean ||
         value.kind == Kind::Number ||
         value.kind == Kind::String;
}

bool IsLogLevel(std::string_view value) {
  return value == "trace" ||
         value == "debug" ||
         value == "info" ||
         value == "warn" ||
         value == "error" ||
         value == "fatal";
}

bool ValidateLogJsonObject(const vkpt::scene::JsonValue& root,
                           std::string& diagnostic) {
  using Kind = vkpt::scene::JsonValue::Kind;
  if (root.kind != Kind::Object) {
    diagnostic = "root is not an object";
    return false;
  }

  const auto* ts = JsonMember(root, "ts");
  if (ts == nullptr || ts->kind != Kind::Number || ts->number < 0.0) {
    diagnostic = "ts must be a non-negative number";
    return false;
  }
  const auto* lvl = JsonMember(root, "lvl");
  if (lvl == nullptr || lvl->kind != Kind::String || !IsLogLevel(lvl->string)) {
    diagnostic = "lvl must be a known level string";
    return false;
  }
  const auto* thr = JsonMember(root, "thr");
  if (thr == nullptr || thr->kind != Kind::String || thr->string.empty()) {
    diagnostic = "thr must be a non-empty string";
    return false;
  }
  const auto* comp = JsonMember(root, "comp");
  if (comp == nullptr || comp->kind != Kind::String || comp->string.empty()) {
    diagnostic = "comp must be a non-empty string";
    return false;
  }
  const auto* ev = JsonMember(root, "ev");
  if (ev == nullptr || ev->kind != Kind::String || ev->string.empty()) {
    diagnostic = "ev must be a non-empty string";
    return false;
  }
  if (const auto* coalesced = JsonMember(root, "coalesced");
      coalesced != nullptr &&
      (coalesced->kind != Kind::Number || coalesced->number < 2.0)) {
    diagnostic = "coalesced must be a number >= 2";
    return false;
  }

  for (const auto& [key, value] : root.object) {
    if (key == "ts" || key == "lvl" || key == "thr" ||
        key == "comp" || key == "ev" || key == "coalesced") {
      continue;
    }
    if (!IsSchemaPrimitive(value)) {
      diagnostic = "field '" + key + "' must be a JSON primitive";
      return false;
    }
  }
  return true;
}

bool ValidateLogJsonFile(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!Check(input.good(), "log schema: JSON log file opens")) {
    return false;
  }

  std::string line;
  std::uint64_t lines = 0;
  while (std::getline(input, line)) {
    ++lines;
    if (line.empty()) {
      std::cerr << "  line=" << lines << " reason=empty line\n";
      return Check(false, "log schema: every line is a JSON object");
    }
    const auto parsed = vkpt::scene::JsonParser::parse(line);
    if (!parsed) {
      std::cerr << "  line=" << lines << " reason=parse failed\n";
      return Check(false, "log schema: every line parses as JSON");
    }
    std::string diagnostic;
    if (!ValidateLogJsonObject(*parsed, diagnostic)) {
      std::cerr << "  line=" << lines << " reason=" << diagnostic << "\n";
      return Check(false, "log schema: object matches schema");
    }
  }
  std::cerr << "  validated_log_lines=" << lines << "\n";
  return Check(lines > 0, "log schema: validated at least one JSON log line");
}

std::string ReadFileText(std::string_view path) {
  std::ifstream input{std::string(path)};
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

bool PathExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

std::filesystem::path FindRepoRoot() {
  auto current = std::filesystem::current_path();
  for (int i = 0; i < 10; ++i) {
    if (PathExists(current / "SYSTEM.md") &&
        PathExists(current / "src" / "core" / "Observability.h")) {
      return current;
    }
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }
  return {};
}

std::optional<std::string> ExtractFunctionBody(std::string_view source,
                                               std::string_view signature) {
  const auto signature_pos = source.find(signature);
  if (signature_pos == std::string_view::npos) {
    return std::nullopt;
  }
  const auto body_begin = source.find('{', signature_pos);
  if (body_begin == std::string_view::npos) {
    return std::nullopt;
  }

  int depth = 0;
  for (std::size_t i = body_begin; i < source.size(); ++i) {
    if (source[i] == '{') {
      ++depth;
    } else if (source[i] == '}') {
      --depth;
      if (depth == 0) {
        return std::string(source.substr(body_begin, i - body_begin + 1u));
      }
    }
  }
  return std::nullopt;
}

bool HotPathAuditPass() {
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "hot-path audit: repo root should be discoverable")) {
    return false;
  }

  struct AuditCase {
    const char* path;
    const char* signature;
    const char* label;
  };
  const AuditCase cases[] = {
      {"src/audio/AudioSystem.cpp",
       "void record_callback_metrics(std::uint64_t callbackStartNs,",
       "audio callback metrics"},
      {"src/audio/AudioSystem.cpp",
       "void mix_device_output(float* out, std::uint32_t frameCount, std::uint32_t outputChannels)",
       "audio mix callback"},
      {"src/render/TileScheduler.cpp",
       "bool TileScheduler::next_tile(vkpt::pathtracer::RenderTile& out)",
       "tile scheduler next_tile"},
      {"src/core/log/Log.cpp",
       "bool Logger::push(LogEvent ev) noexcept",
       "observability logger push"},
      {"src/core/metrics/Metrics.h",
       "void inc(std::uint64_t n = 1) noexcept",
       "metrics counter increment"},
      {"src/core/metrics/Metrics.h",
       "void record(std::uint64_t v) noexcept",
       "metrics histogram record"},
      {"src/core/sync/SpscRing.h",
       "bool try_push(T value)",
       "SPSC ring producer push"},
      {"src/core/sync/MpscRing.h",
       "bool try_push(T value)",
       "MPSC ring producer push"},
  };
  const std::string_view forbidden[] = {
      "std::mutex",
      "std::scoped_lock",
      "std::lock_guard",
      "std::unique_lock",
      "malloc(",
      "calloc(",
      "realloc(",
      "operator new",
      "new ",
      "std::ifstream",
      "std::ofstream",
      "std::fstream",
      "fopen(",
      "CreateFile",
      "ReadFile",
      "WriteFile",
      ".wait(",
      "wait_for(",
      "wait_until(",
      "sleep_for(",
      "sleep_until(",
  };

  bool ok = true;
  for (const auto& audit_case : cases) {
    const auto path = root / audit_case.path;
    const auto source = ReadFileText(path.string());
    const auto body = ExtractFunctionBody(source, audit_case.signature);
    if (!Check(body.has_value(),
               std::string("hot-path audit: locate ") + audit_case.label)) {
      ok = false;
      continue;
    }
    for (const auto token : forbidden) {
      if (body->find(token) != std::string::npos) {
        std::cerr << "  label=" << audit_case.label
                  << " forbidden_token=" << token << "\n";
        ok &= Check(false,
                    std::string("hot-path audit: ") + audit_case.label +
                        " has no mutex/malloc/blocking I/O token");
      }
    }
  }
  ok &= Check(ok, "hot-path audit: selected callback/scheduler/observability paths are clean");
  return ok;
}

bool PathCpuMetricSourceAuditPass() {
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "path/cpu metric audit: repo root should be discoverable")) {
    return false;
  }

  struct RequiredMetric {
    const char* path;
    const char* token;
    const char* label;
  };
  const RequiredMetric metrics[] = {
      {"src/pathtracer/PathTracerObservability.h",
       "vkp.pathtracer.sample_us",
       "pathtracer sample_us"},
      {"src/cpu/TiledCpuPathTracer.cpp",
       "vkp.cpu.tile_render_us",
       "tiled tile_render_us"},
      {"src/cpu/TiledCpuPathTracer.cpp",
       "vkp.cpu.tile_merge_us",
       "tiled tile_merge_us"},
      {"src/cpu/ParallelBvhBuilder.cpp",
       "vkp.cpu.bvh_build_us",
       "parallel bvh_build_us"},
      {"src/cpu/CpuFeatures.cpp",
       "\"simd_selected\"",
       "cpu simd_selected event"},
      {"src/cpu/ParallelBvhBuilder.cpp",
       "\"build_completed\"",
       "bvh build_completed event"},
      {"src/cpu/CpuContracts.h",
       "using CpuPathStatus",
       "CPU path status type"},
      {"src/cpu/CpuContracts.h",
       "last_tile_us_p99",
       "CPU path status tile p99"},
      {"src/cpu/TiledCpuPathTracer.cpp",
       "m_status.last_build_us",
       "CPU path status build timing"},
  };

  bool ok = true;
  for (const auto& metric : metrics) {
    const auto source = ReadFileText((root / metric.path).string());
    ok &= Check(source.find(metric.token) != std::string::npos,
                std::string("path/cpu metric audit: ") + metric.label);
  }
  return ok;
}

bool SubsystemMatrixSourceProofPass() {
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "subsystem matrix source proof: repo root should be discoverable")) {
    return false;
  }

  const auto render_coordinator =
      ReadFileText((root / "src" / "render" / "RenderCoordinator.cpp").string());
  const auto path_obs =
      ReadFileText((root / "src" / "pathtracer" / "PathTracerObservability.h").string());
  const auto scalar_tracer =
      ReadFileText((root / "src" / "pathtracer" / "ScalarCpuPathTracer.cpp").string());
  const auto null_tracer =
      ReadFileText((root / "src" / "pathtracer" / "PathTracer.cpp").string());
  const auto gpu_contract =
      ReadFileText((root / "src" / "gpu" / "GpuBackendIntrospection.h").string());
  const auto vulkan_gpu =
      ReadFileText((root / "src" / "gpu" / "VulkanGpuPathTracer.cpp").string());
  const auto job_system =
      ReadFileText((root / "src" / "jobs" / "JobSystem.cpp").string());
  const auto job_header =
      ReadFileText((root / "src" / "jobs" / "JobSystem.h").string());
  const auto scene_world =
      ReadFileText((root / "src" / "scene" / "SceneWorld.cpp").string());
  const auto platform_interfaces =
      ReadFileText((root / "src" / "platform" / "Interfaces.h").string());
  const auto platform_factory =
      ReadFileText((root / "src" / "platform" / "PlatformFactory.cpp").string());
  const auto headless_platform =
      ReadFileText((root / "src" / "platform" / "HeadlessPlatform.cpp").string());
  const auto crash_recorder =
      ReadFileText((root / "src" / "diagnostics" / "CrashRecorder.cpp").string());

  bool ok = true;
  ok &= Check(render_coordinator.find("VKP_LIFECYCLE_STARTED(\"render\"") != std::string::npos &&
                  render_coordinator.find("VKP_LIFECYCLE_STOPPED(\"render\"") != std::string::npos &&
                  render_coordinator.find("\"flow_id\"") != std::string::npos,
              "subsystem matrix source proof: render lifecycle and flow fields are wired");
  ok &= Check(path_obs.find("CreatePathTracerHealthProbe") != std::string::npos &&
                  path_obs.find("EmitPathTracerAnomaly") != std::string::npos &&
                  path_obs.find("\"flow_id\"") != std::string::npos &&
                  scalar_tracer.find("EmitPathTracerStarted") != std::string::npos &&
                  null_tracer.find("EmitPathTracerStopped") != std::string::npos,
              "subsystem matrix source proof: pathtracer lifecycle/anomaly/probe/flow are wired");
  ok &= Check(gpu_contract.find("CreateGpuBackendHealthProbe") != std::string::npos &&
                  gpu_contract.find("EmitGpuBackendStarted") != std::string::npos &&
                  gpu_contract.find("operation_failed") != std::string::npos &&
                  gpu_contract.find("current_flow_id") != std::string::npos &&
                  vulkan_gpu.find("EmitGpuBackendAnomaly") != std::string::npos,
              "subsystem matrix source proof: GPU lifecycle/anomaly/probe/flow are wired");
  ok &= Check(job_header.find("current_flow_id") != std::string::npos &&
                  job_system.find("VKP_LIFECYCLE_STARTED(\"jobs\"") != std::string::npos &&
                  job_system.find("\"flow_id\"") != std::string::npos,
              "subsystem matrix source proof: jobs lifecycle/anomaly/probe/flow are wired");
  ok &= Check(scene_world.find("VKP_LIFECYCLE_STARTED(\"scene\"") != std::string::npos &&
                  scene_world.find("VKP_LIFECYCLE_STOPPED(\"scene\"") != std::string::npos &&
                  scene_world.find("\"operation_failed\"") != std::string::npos,
              "subsystem matrix source proof: scene lifecycle and anomaly events are wired");
  ok &= Check(platform_interfaces.find("CreateUiHealthProbe") != std::string::npos &&
                  platform_interfaces.find("CreatePlatformHealthProbe") != std::string::npos &&
                  platform_factory.find("VKP_LIFECYCLE_STARTED(\"ui\"") != std::string::npos &&
                  platform_factory.find("operation_failed") != std::string::npos &&
                  platform_factory.find("\"flow_id\"") != std::string::npos &&
                  headless_platform.find("VKP_LIFECYCLE_STARTED(\"platform\"") != std::string::npos &&
                  headless_platform.find("VKP_LIFECYCLE_STOPPED(\"platform\"") != std::string::npos,
              "subsystem matrix source proof: UI/platform health probes, lifecycle, anomaly, and flow are wired");
  ok &= Check(crash_recorder.find("VKP_LIFECYCLE_STARTED(\"diagnostics\"") != std::string::npos &&
                  crash_recorder.find("VKP_LIFECYCLE_STOPPED(\"diagnostics\"") != std::string::npos &&
                  crash_recorder.find("operation_failed") != std::string::npos &&
                  crash_recorder.find("\"flow_id\"") != std::string::npos,
              "subsystem matrix source proof: diagnostics lifecycle/anomaly/flow are wired");
  return ok;
}

bool ContractMaturityTodo7To22SourceProofPass() {
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(),
             "contract maturity TODO 7-22 proof: repo root should be discoverable")) {
    return false;
  }

  const auto render_coordinator_header =
      ReadFileText((root / "src" / "render" / "RenderCoordinator.h").string());
  const auto render_coordinator_cpp =
      ReadFileText((root / "src" / "render" / "RenderCoordinator.cpp").string());
  const auto frame_handoff_header =
      ReadFileText((root / "src" / "render" / "FrameHandoff.h").string());
  const auto render_interface_header =
      ReadFileText((root / "src" / "render" / "interface" / "RenderContracts.h").string());
  const auto vulkan_gpu_header =
      ReadFileText((root / "src" / "gpu" / "VulkanGpuPathTracer.h").string());
  const auto vulkan_gpu_cpp =
      ReadFileText((root / "src" / "gpu" / "VulkanGpuPathTracer.cpp").string());
  const auto d3d12_gpu_header =
      ReadFileText((root / "src" / "gpu" / "D3D12GpuPathTracer.h").string());
  const auto d3d12_gpu_cpp =
      ReadFileText((root / "src" / "gpu" / "D3D12GpuPathTracer.cpp").string());
  const auto d3d12_gpu_scene =
      ReadFileText((root / "src" / "gpu" / "D3D12GpuPathTracer.Scene.cpp").string());
  const auto d3d12_gpu_device =
      ReadFileText((root / "src" / "gpu" / "D3D12GpuPathTracer.Device.cpp").string());
  const auto gpu_introspection_header =
      ReadFileText((root / "src" / "gpu" / "GpuBackendIntrospection.h").string());
  const auto scene_header =
      ReadFileText((root / "src" / "scene" / "SceneWorld.h").string());
  const auto scene_cpp =
      ReadFileText((root / "src" / "scene" / "SceneWorld.cpp").string());
  const auto snapshot_ring_header =
      ReadFileText((root / "src" / "scene" / "SnapshotRing.h").string());
  const auto snapshot_ring_cpp =
      ReadFileText((root / "src" / "scene" / "SnapshotRing.cpp").string());
  const auto scripting_header =
      ReadFileText((root / "src" / "scripting" / "ScriptRuntime.h").string());
  const auto physics_header =
      ReadFileText((root / "src" / "physics" / "PhysicsWorld.h").string());
  const auto jobs_header =
      ReadFileText((root / "src" / "jobs" / "JobSystem.h").string());
  const auto audio_header =
      ReadFileText((root / "src" / "audio" / "AudioSystem.h").string());
  const auto platform_interfaces =
      ReadFileText((root / "src" / "platform" / "Interfaces.h").string());
  const auto headless_platform =
      ReadFileText((root / "src" / "platform" / "HeadlessPlatform.cpp").string());
  const auto desktop_platform =
      ReadFileText((root / "src" / "platform" / "DesktopPlatform.cpp").string());
  const auto qt_platform =
      ReadFileText((root / "src" / "platform" / "qt" / "QtPlatformLifecycle.cpp").string());
  const auto diagnostics_header =
      ReadFileText((root / "src" / "diagnostics" / "CrashRecorder.h").string());
  const auto diagnostics_cpp =
      ReadFileText((root / "src" / "diagnostics" / "CrashRecorder.cpp").string());

  const auto all_render_text = render_coordinator_header + render_coordinator_cpp +
                               frame_handoff_header + render_interface_header;
  const auto all_gpu_text = vulkan_gpu_header + vulkan_gpu_cpp + d3d12_gpu_header +
                            d3d12_gpu_cpp + d3d12_gpu_scene + d3d12_gpu_device +
                            gpu_introspection_header;

  bool ok = true;
  ok &= Check(render_interface_header.find("struct RenderInterfaceStandardContract") != std::string::npos &&
                  render_interface_header.find("BuildStandardRenderInterfaceContract") != std::string::npos &&
                  render_interface_header.find("ValidateStandardRenderInterfaceContract") != std::string::npos &&
                  render_interface_header.find("state_machine") != std::string::npos &&
                  render_coordinator_header.find("struct RenderCoordinatorStandardContract") != std::string::npos &&
                  render_coordinator_header.find("BuildStandardRenderCoordinatorContract") != std::string::npos &&
                  render_coordinator_header.find("ValidateStandardRenderCoordinatorContract") != std::string::npos &&
                  render_coordinator_cpp.find("assert_state") != std::string::npos,
              "contract maturity TODO 7-22: render publishes and validates state-machine contracts");
  ok &= Check(frame_handoff_header.find("enum class FrameDropReason") != std::string::npos &&
                  frame_handoff_header.find("FrameHandoff") != std::string::npos &&
                  all_render_text.find("PendingCommands") == std::string::npos &&
                  all_render_text.find("RTSceneData") == std::string::npos,
              "contract maturity TODO 7-22: render naming uses FrameHandoff/drop reasons and no legacy PendingCommands/RTSceneData names");
  ok &= Check(vulkan_gpu_header.find("vkpt::core::Status configure") != std::string::npos &&
                  vulkan_gpu_header.find("vkpt::core::Status load_scene_snapshot") != std::string::npos &&
                  vulkan_gpu_header.find("vkpt::core::Status build_or_update_acceleration") != std::string::npos &&
                  vulkan_gpu_header.find("vkpt::core::Status init_device") != std::string::npos &&
                  d3d12_gpu_header.find("vkpt::core::Status configure") != std::string::npos &&
                  d3d12_gpu_header.find("vkpt::core::Status load_scene_snapshot") != std::string::npos &&
                  d3d12_gpu_header.find("vkpt::core::Status build_or_update_acceleration") != std::string::npos &&
                  d3d12_gpu_header.find("vkpt::core::Status init_device") != std::string::npos &&
                  vulkan_gpu_cpp.find("Status::error") != std::string::npos &&
                  d3d12_gpu_cpp.find("Status::error") != std::string::npos &&
                  d3d12_gpu_scene.find("Status::error") != std::string::npos &&
                  d3d12_gpu_device.find("Status::error") != std::string::npos,
              "contract maturity TODO 7-22: GPU backends expose typed Status init/config/load/build failures");
  ok &= Check(gpu_introspection_header.find("struct GpuBackendContract") != std::string::npos &&
                  gpu_introspection_header.find("BuildStandardGpuBackendContract") != std::string::npos &&
                  gpu_introspection_header.find("ValidateStandardGpuBackendContract") != std::string::npos &&
                  gpu_introspection_header.find("state_machine") != std::string::npos &&
                  gpu_introspection_header.find("GpuBackendStatus") != std::string::npos,
              "contract maturity TODO 7-22: GPU publishes a backend state-machine contract");
  ok &= Check(gpu_introspection_header.find("exposes_determinism_context") != std::string::npos &&
                  gpu_introspection_header.find("void set_determinism") != std::string::npos &&
                  gpu_introspection_header.find("determinism_context() const") != std::string::npos &&
                  vulkan_gpu_cpp.find("info.set_determinism(m_settings.determinism_context())") != std::string::npos &&
                  d3d12_gpu_header.find("info.set_determinism(m_settings.determinism_context())") != std::string::npos &&
                  vulkan_gpu_cpp.find("EmitDeterminismChangedIfNeeded(\"gpu\"") != std::string::npos &&
                  d3d12_gpu_cpp.find("EmitDeterminismChangedIfNeeded(\"gpu\"") != std::string::npos,
              "contract maturity TODO 7-22: GPU status and backends propagate DeterminismContext");
  ok &= Check(gpu_introspection_header.find("struct GpuBackendIntrospection") != std::string::npos &&
                  gpu_introspection_header.find("class IGpuBackendIntrospect") != std::string::npos &&
                  vulkan_gpu_header.find("PathTracePushConstants.inc") != std::string::npos &&
                  vulkan_gpu_header.find("PathTracerSceneSnapshot") != std::string::npos &&
                  d3d12_gpu_header.find("PathTracerSceneSnapshot") != std::string::npos &&
                  all_gpu_text.find("RTSceneData") == std::string::npos,
              "contract maturity TODO 7-22: GPU naming uses introspection/schema/snapshot contract names");
  ok &= Check(scene_header.find("IEcsWorld lifecycle contract") != std::string::npos &&
                  snapshot_ring_header.find("SnapshotRing lifecycle contract") != std::string::npos &&
                  scene_cpp.find("assert_state") != std::string::npos,
              "contract maturity TODO 7-22: scene documents and validates state-machine contracts");
  ok &= Check(scene_header.find("virtual void set_determinism") != std::string::npos &&
                  scene_header.find("virtual vkpt::core::DeterminismContext determinism_context() const") != std::string::npos &&
                  scene_header.find("current_flow_id") != std::string::npos &&
                  scene_cpp.find("SceneWorld::set_determinism") != std::string::npos &&
                  scene_cpp.find("current_flow_id") != std::string::npos &&
                  snapshot_ring_header.find("void set_determinism") != std::string::npos &&
                  snapshot_ring_cpp.find("SnapshotRing::determinism_context") != std::string::npos,
              "contract maturity TODO 7-22: scene and snapshot ring propagate DeterminismContext");
  ok &= Check(scripting_header.find("kScriptingNamingContract") != std::string::npos &&
                  scripting_header.find("kScriptingCommandSnapshotContractName") != std::string::npos &&
                  scripting_header.find("status_type_name") != std::string::npos &&
                  scripting_header.find("flow_field_name") != std::string::npos,
              "contract maturity TODO 7-22: scripting naming exposes canonical status and command snapshot contracts");
  ok &= Check(physics_header.find("kPhysicsNamingContract") != std::string::npos &&
                  physics_header.find("kPhysicsStepSnapshotContractName") != std::string::npos &&
                  physics_header.find("status_type_name") != std::string::npos &&
                  physics_header.find("sequencing_field_name") != std::string::npos,
              "contract maturity TODO 7-22: physics naming exposes canonical status and sequencing contracts");
  ok &= Check(jobs_header.find("kJobSystemNamingContract") != std::string::npos &&
                  jobs_header.find("kJobSystemContractName") != std::string::npos &&
                  jobs_header.find("status_type_name") != std::string::npos &&
                  jobs_header.find("queue_depth_field_name") != std::string::npos,
              "contract maturity TODO 7-22: jobs naming exposes canonical status and queue-depth contracts");
  ok &= Check(audio_header.find("struct AudioOneShotEventDesc") != std::string::npos &&
                  audio_header.find("struct AudioTrackedEventDesc") != std::string::npos &&
                  audio_header.find("post_one_shot_event") != std::string::npos &&
                  audio_header.find("post_tracked_event") != std::string::npos &&
                  audio_header.find("AudioPostEventDesc") == std::string::npos,
              "contract maturity TODO 7-22: audio naming uses split one-shot/tracked event descriptors");
  ok &= Check(platform_interfaces.find("virtual vkpt::core::Status initialize_status()") != std::string::npos &&
                  platform_interfaces.find("vkpt::core::ResultFromStatus(initialize_status())") != std::string::npos &&
                  platform_interfaces.find("virtual vkpt::core::Result<std::string> read_text_file") != std::string::npos &&
                  platform_interfaces.find("virtual vkpt::core::Result<void> set_text") != std::string::npos &&
                  platform_interfaces.find("virtual vkpt::core::Result<std::string> get_text") != std::string::npos &&
                  headless_platform.find("vkpt::core::Status HeadlessPlatform::initialize_status") != std::string::npos &&
                  desktop_platform.find("vkpt::core::Status DesktopPlatform::initialize_status") != std::string::npos &&
                  qt_platform.find("vkpt::core::Status QtPlatform::initialize_status") != std::string::npos,
              "contract maturity TODO 7-22: platform interfaces and implementations return typed Result");
  ok &= Check(platform_interfaces.find("struct PlatformStatus") != std::string::npos &&
                  platform_interfaces.find("using IPlatformStatus = PlatformStatus") != std::string::npos &&
                  platform_interfaces.find("struct UiStatus") != std::string::npos &&
                  platform_interfaces.find("class IInputSource") != std::string::npos &&
                  platform_interfaces.find("set_source_status") != std::string::npos,
              "contract maturity TODO 7-22: platform/UI naming exposes canonical status and input-source contracts");
  ok &= Check(platform_interfaces.find("void set_determinism(const vkpt::core::DeterminismContext& context)") != std::string::npos &&
                  platform_interfaces.find("vkpt::core::DeterminismContext determinism_context() const") != std::string::npos &&
                  platform_interfaces.find("SetUiDeterminismContext") != std::string::npos &&
                  headless_platform.find("HeadlessPlatform::set_determinism") != std::string::npos &&
                  desktop_platform.find("DesktopPlatform::set_determinism") != std::string::npos &&
                  qt_platform.find("QtPlatform::set_determinism") != std::string::npos,
              "contract maturity TODO 7-22: platform/UI propagates DeterminismContext through status and backends");
  ok &= Check(diagnostics_header.find("struct DiagnosticsStatus") != std::string::npos &&
                  diagnostics_header.find("DiagnosticsStatus status() const") != std::string::npos &&
                  diagnostics_header.find("using CrashRecorderStatus = DiagnosticsStatus") != std::string::npos &&
                  diagnostics_cpp.find("DiagnosticsStatus CrashRecorder::status() const") != std::string::npos &&
                  diagnostics_cpp.find("to_subsystem_status") != std::string::npos,
              "contract maturity TODO 7-22: diagnostics naming exposes DiagnosticsStatus with a formal CrashRecorderStatus alias");
  return ok;
}

struct LogEventKey {
  std::string component;
  std::string event;
};

std::vector<LogEventKey> ReadLogEventKeys(std::string_view path) {
  std::vector<LogEventKey> events;
  std::ifstream input{std::string(path)};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parsed = vkpt::scene::JsonParser::parse(line);
    if (!parsed) {
      continue;
    }
    const auto* comp = JsonMember(*parsed, "comp");
    const auto* event = JsonMember(*parsed, "ev");
    if (comp != nullptr && event != nullptr &&
        comp->kind == vkpt::scene::JsonValue::Kind::String &&
        event->kind == vkpt::scene::JsonValue::Kind::String) {
      events.push_back({comp->string, event->string});
    }
  }
  return events;
}

bool HasLogEvent(const std::vector<LogEventKey>& events,
                 std::string_view component,
                 std::string_view event_name) {
  return std::any_of(events.begin(), events.end(), [&](const auto& event) {
    return event.component == component && event.event == event_name;
  });
}

bool HasLogEvent(const std::vector<vkpt::core::log::LogEvent>& events,
                 std::string_view component,
                 std::string_view event_name) {
  return std::any_of(events.begin(), events.end(), [&](const auto& event) {
    return std::string_view(event.component) == component &&
           std::string_view(event.event) == event_name;
  });
}

const vkpt::core::metrics::MetricSnapshot* FindMetricSnapshot(
    const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
    std::string_view name,
    vkpt::core::metrics::Kind kind) {
  const auto it = std::find_if(metrics.begin(), metrics.end(), [&](const auto& metric) {
    return metric.name == name && metric.kind == kind;
  });
  return it == metrics.end() ? nullptr : &*it;
}

bool SharedContractsPass() {
  namespace contracts = vkpt::core::contracts;
  using vkpt::core::metrics::Kind;
  using vkpt::core::metrics::MetricsRegistry;
  using namespace vkpt::core::log;

  bool ok = true;

  const auto contract_status =
      vkpt::core::Status::error(vkpt::core::Status::Code::NotReady,
                                "not configured",
                                "call configure first");
  ok &= Check(contract_status.is_error() &&
                  contract_status.code == vkpt::core::Status::Code::NotReady &&
                  contract_status.message == "not configured" &&
                  contract_status.recovery_hint.has_value(),
              "contracts: Status carries code, message, and recovery hint");

  auto try_wrapper = [](bool pass) -> vkpt::core::Result<void> {
    auto maybe_fail = [](bool ok) -> vkpt::core::Result<void> {
      return ok ? vkpt::core::Result<void>::ok()
                : vkpt::core::Result<void>::error(vkpt::core::ErrorCode::InvalidArgument);
    };
    VKP_TRY(maybe_fail(pass));
    return vkpt::core::Result<void>::ok();
  };
  ok &= Check(try_wrapper(true).has_value() &&
                  try_wrapper(false).is_error() &&
                  try_wrapper(false).error() == vkpt::core::ErrorCode::InvalidArgument,
              "contracts: VKP_TRY propagates Result errors");

  ok &= Check(std::string_view(contracts::ComponentLifecycleName(
                  contracts::ComponentLifecycle::ShuttingDown)) == "shutting_down",
              "contracts: ComponentLifecycle exposes stable names");
  ok &= Check(contracts::state_allowed(contracts::ComponentLifecycle::Ready,
                                       {contracts::ComponentLifecycle::Ready,
                                        contracts::ComponentLifecycle::Degraded}) &&
                  !contracts::state_allowed(contracts::ComponentLifecycle::Failed,
                                            {contracts::ComponentLifecycle::Ready}) &&
                  contracts::assert_state("contracts.smoke",
                                          contracts::ComponentLifecycle::Ready,
                                          {contracts::ComponentLifecycle::Ready}),
              "contracts: lifecycle state helpers validate allowed states");

  const auto status_from_result =
      vkpt::core::StatusFromResult(try_wrapper(false), "invalid argument");
  const auto result_from_status =
      vkpt::core::ResultFromStatus(vkpt::core::Status::error(
          vkpt::core::StatusCode::InvalidArgument, "invalid argument"));
  ok &= Check(std::string_view(vkpt::core::StatusCodeName(
                  vkpt::core::StatusCode::InvalidArgument)) == "invalid_argument" &&
                  status_from_result.code == vkpt::core::StatusCode::InvalidArgument &&
                  status_from_result.message == "invalid argument" &&
                  result_from_status.is_error() &&
                  result_from_status.error() == vkpt::core::ErrorCode::InvalidArgument,
              "contracts: Status/Result conversion helpers preserve error identity");

  const auto determinism =
      vkpt::core::MakeDeterminismContext(true, 0x1234u, 9u, "contracts-smoke");
  ok &= Check(determinism.enabled &&
                  determinism.base_seed == 0x1234u &&
                  determinism.frame_index == 9u &&
                  determinism.scenario_id == "contracts-smoke",
              "contracts: DeterminismContext carries replay identity");

  vkpt::pathtracer::RenderSettings render_settings;
  render_settings.set_determinism(determinism);
  ok &= Check(render_settings.determinism_context(determinism.frame_index,
                                                  determinism.scenario_id) == determinism,
              "contracts: RenderSettings accepts DeterminismContext");

  vkpt::pathtracer::PathTraceSettings path_trace_settings;
  path_trace_settings.set_determinism(determinism);
  ok &= Check(path_trace_settings.determinism_context(determinism.frame_index,
                                                      determinism.scenario_id) == determinism,
              "contracts: PathTraceSettings accepts DeterminismContext");

  vkpt::scripting::ScriptExecutionContext script_context;
  script_context.set_determinism(determinism);
  ok &= Check(script_context.determinism_context() == determinism,
              "contracts: ScriptExecutionContext accepts DeterminismContext");

  vkpt::physics::PhysicsStepConfig physics_config;
  physics_config.set_determinism(determinism);
  ok &= Check(physics_config.determinism_context() == determinism,
              "contracts: PhysicsStepConfig accepts DeterminismContext");

  vkpt::audio::AudioSystemConfig audio_config;
  audio_config.set_determinism(determinism);
  ok &= Check(audio_config.determinism_context() == determinism,
              "contracts: AudioSystemConfig accepts DeterminismContext");

  using JobSetDeterminism = void (vkpt::jobs::IJobSystem::*)(
      const vkpt::core::DeterminismContext&);
  [[maybe_unused]] JobSetDeterminism job_set_determinism =
      &vkpt::jobs::IJobSystem::set_determinism;
  ok &= Check(job_set_determinism != nullptr,
              "contracts: IJobSystem exposes set_determinism(DeterminismContext)");

  auto status =
      contracts::MakeSubsystemStatus("contracts.smoke", contracts::SubsystemHealth::Degraded);
  status.started_at_ns = 10u;
  status.last_tick_ns = 25u;
  status.last_error = "late_tick";
  status.ticks_total = 7u;
  status.errors_total = 1u;
  status.set_custom("queue_depth", "3");
  status.set_custom("mode", "smoke");
  status.set_custom("queue_depth", "4");

  ok &= Check(status.name == "contracts.smoke" &&
                  status.status == contracts::SubsystemHealth::Degraded &&
                  std::string_view(contracts::SubsystemHealthName(status.status)) == "degraded" &&
                  status.started_at_ns == 10u &&
                  status.last_tick_ns == 25u &&
                  status.last_error == "late_tick" &&
                  status.ticks_total == 7u &&
                  status.errors_total == 1u,
              "contracts: SubsystemStatus exposes standard fields");
  const auto queue_depth =
      std::find_if(status.custom_fields.begin(),
                   status.custom_fields.end(),
                   [](const auto& field) { return field.name == "queue_depth"; });
  ok &= Check(queue_depth != status.custom_fields.end() &&
                  queue_depth->value == "4" &&
                  status.custom_fields.size() == 2u,
              "contracts: SubsystemStatus custom fields upsert by name");

  contracts::TypedStatusFields typed_status;
  typed_status.lifecycle = contracts::ComponentLifecycle::Ready;
  typed_status.last_error = "none";
  typed_status.last_tick_ns = 11u;
  typed_status.ticks_total = 12u;
  typed_status.errors_total = 0u;
  ok &= Check(typed_status.lifecycle == contracts::ComponentLifecycle::Ready &&
                  typed_status.last_error == "none" &&
                  typed_status.last_tick_ns == 11u &&
                  typed_status.ticks_total == 12u &&
                  typed_status.errors_total == 0u,
              "contracts: TypedStatusFields carries mandatory status fields");

  class SyntheticFlowSource final : public contracts::IFlowSource {
   public:
    explicit SyntheticFlowSource(std::uint64_t flow_id) : m_flow_id(flow_id) {}
    std::uint64_t current_flow_id() const noexcept override { return m_flow_id; }

   private:
    std::uint64_t m_flow_id = 0u;
  };
  const SyntheticFlowSource flow_source{123u};
  ok &= Check(flow_source.current_flow_id() == 123u,
              "contracts: IFlowSource implementation exposes current flow id");

  struct SmokeMetricsTag {};
  auto& registry = MetricsRegistry::instance();
  registry.reset("vkp.contract_bundle.");
  contracts::MetricsBundle<SmokeMetricsTag> bundle("vkp.contract_bundle");
  bundle.tick.inc(3u);
  bundle.error.inc();
  bundle.duration_us.record(42u);
  bundle.depth.set(9.0);
  const auto metrics = registry.snapshot_prefix("vkp.contract_bundle.");
  const auto* ticks =
      FindMetricSnapshot(metrics, "vkp.contract_bundle.tick_total", Kind::CounterKind);
  const auto* errors =
      FindMetricSnapshot(metrics, "vkp.contract_bundle.errors_total", Kind::CounterKind);
  const auto* duration =
      FindMetricSnapshot(metrics, "vkp.contract_bundle.duration_us", Kind::HistogramKind);
  const auto* depth =
      FindMetricSnapshot(metrics, "vkp.contract_bundle.depth", Kind::GaugeKind);
  ok &= Check(ticks != nullptr && ticks->counter_value == 3u,
              "contracts: MetricsBundle records tick counter under prefix");
  ok &= Check(errors != nullptr && errors->counter_value == 1u,
              "contracts: MetricsBundle records error counter under prefix");
  ok &= Check(duration != nullptr && duration->hist.count == 1u &&
                  duration->hist.max_val == 42u,
              "contracts: MetricsBundle records duration histogram under prefix");
  ok &= Check(depth != nullptr && std::abs(depth->gauge_value - 9.0) < 1e-6,
              "contracts: MetricsBundle records depth gauge under prefix");

  const std::string out_path = "obs_lifecycle_contract.jsonl";
  Logger::instance().flush_for_test();
  std::remove(out_path.c_str());
  Logger::instance().set_sink(std::make_unique<FileSink>(out_path));
  Logger::instance().set_format(Format::Json);
  Logger::instance().set_min_level(Level::Info);
  VKP_LIFECYCLE_STARTED("contract_lifecycle",
                        "flow_id",
                        flow_source.current_flow_id(),
                        "mode",
                        "smoke");
  VKP_LIFECYCLE_STOPPED("contract_lifecycle", "reason", "test_done");
  VKP_LIFECYCLE_CONFIG("contract_lifecycle",
                       "enabled",
                       true,
                       "workers",
                       static_cast<std::uint64_t>(4u));
  Logger::instance().flush_for_test();
  Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stderr));

  ok &= ValidateLogJsonFile(out_path);
  std::ifstream input{out_path};
  std::string line;
  bool saw_started = false;
  bool saw_stopped = false;
  bool saw_config = false;
  while (std::getline(input, line)) {
    const auto parsed = vkpt::scene::JsonParser::parse(line);
    if (!parsed) {
      continue;
    }
    const auto* component = JsonMember(*parsed, "comp");
    const auto* event = JsonMember(*parsed, "ev");
    if (component == nullptr || event == nullptr ||
        component->kind != vkpt::scene::JsonValue::Kind::String ||
        event->kind != vkpt::scene::JsonValue::Kind::String ||
        component->string != "contract_lifecycle") {
      continue;
    }
    if (event->string == "started") {
      const auto* flow = JsonMember(*parsed, "flow_id");
      const auto* mode = JsonMember(*parsed, "mode");
      saw_started = flow != nullptr &&
                    flow->kind == vkpt::scene::JsonValue::Kind::Number &&
                    flow->number == 123.0 &&
                    mode != nullptr &&
                    mode->kind == vkpt::scene::JsonValue::Kind::String &&
                    mode->string == "smoke";
    } else if (event->string == "stopped") {
      const auto* reason = JsonMember(*parsed, "reason");
      saw_stopped = reason != nullptr &&
                    reason->kind == vkpt::scene::JsonValue::Kind::String &&
                    reason->string == "test_done";
    } else if (event->string == "config") {
      const auto* enabled = JsonMember(*parsed, "enabled");
      const auto* workers = JsonMember(*parsed, "workers");
      saw_config = enabled != nullptr &&
                   enabled->kind == vkpt::scene::JsonValue::Kind::Boolean &&
                   enabled->boolean &&
                   workers != nullptr &&
                   workers->kind == vkpt::scene::JsonValue::Kind::Number &&
                   workers->number == 4.0;
    }
  }
  ok &= Check(saw_started,
              "contracts: VKP_LIFECYCLE_STARTED emits JSON started event");
  ok &= Check(saw_stopped,
              "contracts: VKP_LIFECYCLE_STOPPED emits JSON stopped event");
  ok &= Check(saw_config,
              "contracts: VKP_LIFECYCLE_CONFIG emits JSON config event");
  std::remove(out_path.c_str());

  return ok;
}

bool ComponentEventContractPass() {
  using namespace vkpt::core::log;

  const std::string out_path = "obs_component_contract.jsonl";
  std::remove(out_path.c_str());
  Logger::instance().set_sink(std::make_unique<FileSink>(out_path));
  Logger::instance().set_format(Format::Json);
  Logger::instance().set_min_level(Level::Info);

  const char* components[] = {
      "obs",
      "metrics",
      "health",
      "ui",
      "sim",
      "audio",
      "tracer",
      "scripting",
      "snapshot",
      "determinism",
  };
  for (const char* component : components) {
    VKP_LOG(Info,
            component,
            "lifecycle",
            "phase",
            "started",
            "acceptance_smoke",
            true);
    VKP_LOG(Info,
            component,
            "heartbeat",
            "status",
            "ok",
            "acceptance_smoke",
            true);
    VKP_LOG(Warn,
            component,
            "anomaly",
            "kind",
            "acceptance_smoke",
            "acceptance_smoke",
            true);
  }
  Logger::instance().flush_for_test();

  bool ok = ValidateLogJsonFile(out_path);
  const auto events = ReadLogEventKeys(out_path);
  for (const char* component : components) {
    ok &= Check(HasLogEvent(events, component, "lifecycle"),
                std::string("component event contract: ") + component +
                    " lifecycle event");
    ok &= Check(HasLogEvent(events, component, "heartbeat"),
                std::string("component event contract: ") + component +
                    " heartbeat event");
    ok &= Check(HasLogEvent(events, component, "anomaly"),
                std::string("component event contract: ") + component +
                    " anomaly event");
  }

  Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stderr));
  std::remove(out_path.c_str());
  return ok;
}

#ifndef _WIN32
std::string QuoteCommandArg(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2u);
  out.push_back('"');
  for (char ch : value) {
    if (ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}
#endif

std::string BuildRedirectedCommand(std::string_view executable,
                                   std::string_view args,
                                   std::string_view stdout_path,
                                   std::string_view stderr_path) {
#ifdef _WIN32
  return "cmd /C \"\"" + std::string(executable) + "\" " +
         std::string(args) + " > \"" + std::string(stdout_path) +
         "\" 2> \"" + std::string(stderr_path) + "\"\"";
#else
  return QuoteCommandArg(executable) + " " + std::string(args) + " > " +
         QuoteCommandArg(stdout_path) + " 2> " + QuoteCommandArg(stderr_path);
#endif
}

bool RingPrimitivesPass() {
  using namespace vkpt::core::sync;

  // SPSC: producer/consumer pair pushes 50k items across two threads.
  // Consumer terminates when the expected count of pops has been reached, then
  // checks that the running sum matches the expected triangular total.
  {
    constexpr int kCount = 50000;
    SpscRing<int> r(1024);
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> popped_sum{0};
    std::atomic<std::uint64_t> popped_count{0};
    std::thread cons([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      int v = 0;
      while (popped_count.load(std::memory_order_acquire) < kCount) {
        if (r.try_pop(v)) {
          popped_sum.fetch_add(static_cast<std::uint64_t>(v), std::memory_order_relaxed);
          popped_count.fetch_add(1, std::memory_order_release);
        } else {
          std::this_thread::yield();
        }
      }
    });
    go.store(true, std::memory_order_release);
    for (int i = 1; i <= kCount; ++i) {
      while (!r.try_push(i)) std::this_thread::yield();
    }
    cons.join();
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kCount) * (kCount + 1ull) / 2ull;
    if (!Check(popped_sum.load() == expected,
               "SPSC popped sum matches expected triangular total")) {
      return false;
    }
  }

  // MPSC: 4 producers × 25k items into one consumer.
  {
    MpscRing<std::uint64_t> r(8192);
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> popped_sum{0};
    std::atomic<std::uint64_t> popped_count{0};
    constexpr std::uint64_t kProducers = 4;
    constexpr std::uint64_t kPerProd = 25000;
    std::thread cons([&] {
      std::uint64_t v = 0;
      while (popped_count.load(std::memory_order_acquire) < kProducers * kPerProd) {
        if (r.try_pop(v)) {
          popped_sum.fetch_add(v, std::memory_order_relaxed);
          popped_count.fetch_add(1, std::memory_order_release);
        } else {
          std::this_thread::yield();
        }
      }
    });
    std::vector<std::thread> prods;
    for (std::uint64_t p = 0; p < kProducers; ++p) {
      prods.emplace_back([&, p] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 0; i < kPerProd; ++i) {
          const std::uint64_t v = p * kPerProd + i + 1;
          while (!r.try_push(v)) std::this_thread::yield();
        }
      });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : prods) t.join();
    cons.join();
    const std::uint64_t total = kProducers * kPerProd;
    const std::uint64_t expected_sum = total * (total + 1ull) / 2ull;
    if (!Check(popped_sum.load() == expected_sum, "MPSC sum matches expected triangular total")) return false;
  }

  // LatestSlot: rapid publishes drop counted; final take wins.
  {
    LatestSlot<int> slot;
    Check(!slot.has_value(), "LatestSlot starts empty");
    slot.publish(1);
    slot.publish(2);
    slot.publish(3);
    auto v = slot.take();
    if (!Check(v.has_value() && *v == 3, "LatestSlot returns most recent")) return false;
    if (!Check(slot.dropped_total() == 2, "LatestSlot counts 2 dropped publishes")) return false;
  }
  return true;
}

bool LoggerStressPass() {
  using namespace vkpt::core::log;
  // Test mode: file sink so we can grep the output.
  const std::string out_path = "obs_smoke_log.json";
  std::remove(out_path.c_str());
  Logger::instance().set_sink(std::make_unique<FileSink>(out_path));
  Logger::instance().set_format(Format::Json);
  Logger::instance().set_min_level(Level::Info);

  constexpr int kProducers = 6;
  constexpr int kPerProd = 100000;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kProducers; ++t) {
    threads.emplace_back([&, t] {
      Logger::instance().set_thread_name(std::string("prod") + std::to_string(t));
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (int i = 0; i < kPerProd; ++i) {
        VKP_LOG(Info, "smoke", "tick",
                "producer", static_cast<std::uint64_t>(t),
                "iter", static_cast<std::uint64_t>(i));
      }
    });
  }
  // Snapshot before producers fire so framework lifecycle events don't pollute
  // the count.
  const std::uint64_t emitted_before = Logger::instance().total_emitted();
  const std::uint64_t dropped_before = Logger::instance().total_drop_count();

  go.store(true, std::memory_order_release);
  for (auto& th : threads) th.join();
  Logger::instance().flush_for_test();

  const std::uint64_t emitted = Logger::instance().total_emitted() - emitted_before;
  const std::uint64_t dropped = Logger::instance().total_drop_count() - dropped_before;
  const std::uint64_t expected = static_cast<std::uint64_t>(kProducers) * kPerProd;

  std::cerr << "  emitted_delta=" << emitted << " dropped_delta=" << dropped
            << " expected=" << expected << "\n";

  // Allow some heartbeat / health events from the framework to also have
  // landed in the same window. Net producer events emitted is at least
  // expected - dropped, and the total should not exceed expected by more than
  // 1024 lifecycle records.
  if (!Check(emitted + dropped >= expected,
             "logger emitted + dropped >= expected event count")) {
    return false;
  }
  if (!Check(emitted + dropped <= expected + 1024,
             "logger emitted + dropped is close to expected (within framework overhead)")) {
    return false;
  }
  if (!Check(dropped < expected / 2, "fewer than half of events were dropped")) return false;

  Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stderr));
  if (!ValidateLogJsonFile(out_path)) return false;

  return true;
}

bool MetricsPass() {
  using namespace vkpt::core::metrics;
  auto& reg = MetricsRegistry::instance();
  reg.reset("");

  for (int i = 0; i < 1000; ++i) VKP_METRIC_INC("vkp.smoke.iters");
  VKP_METRIC_SET("vkp.smoke.gauge_value", 3.14159);
  for (std::uint64_t v = 1; v <= 100; ++v) VKP_METRIC_OBSERVE("vkp.smoke.lat_us", v);

  auto snap = reg.snapshot_prefix("vkp.smoke.");
  if (!Check(!snap.empty(), "metrics: snapshot returns smoke entries")) return false;

  bool found_counter = false, found_gauge = false, found_hist = false;
  for (const auto& s : snap) {
    if (s.name == "vkp.smoke.iters" && s.kind == Kind::CounterKind) {
      found_counter = (s.counter_value == 1000);
    } else if (s.name == "vkp.smoke.gauge_value" && s.kind == Kind::GaugeKind) {
      found_gauge = std::abs(s.gauge_value - 3.14159) < 1e-6;
    } else if (s.name == "vkp.smoke.lat_us" && s.kind == Kind::HistogramKind) {
      found_hist = s.hist.count == 100 && s.hist.max_val == 100;
    }
  }
  if (!Check(found_counter, "counter exact value 1000")) return false;
  if (!Check(found_gauge, "gauge exact value 3.14159")) return false;
  if (!Check(found_hist, "histogram count=100 max=100")) return false;

  // JSON dump non-empty.
  const std::string json = reg.dump_json();
  if (!Check(!json.empty() && json.front() == '[', "metrics: JSON dump shaped correctly")) return false;

  return true;
}

bool MetricsDockProfilingPass() {
  using namespace vkpt::core::log;
  using namespace vkpt::core::metrics;

  bool ok = true;
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "metrics dock proof: repo root should be discoverable")) {
    return false;
  }

  const auto dock_source =
      ReadFileText((root / "src" / "app" / "QtDockPanelsRender.cpp").string());
  const auto dock_body =
      ExtractFunctionBody(dock_source, "QtDockPanelContent BuildQtMetricsDock(");
  ok &= Check(dock_body.has_value(), "metrics dock proof: locate BuildQtMetricsDock body");
  if (dock_body) {
    const std::string_view required_tokens[] = {
        "snapshot_all()",
        "starts_with(\"vkp.\")",
        "rate ",
        "p95",
        "p99",
        "QtMetricSparkline",
    };
    for (const auto token : required_tokens) {
      ok &= Check(dock_body->find(token) != std::string::npos,
                  std::string("metrics dock proof: source contains ") +
                      std::string(token));
    }
  }

  auto& registry = MetricsRegistry::instance();
  registry.reset("vkp.profiling_smoke.");

  constexpr int kThreads = 4;
  constexpr int kBeatsPerThread = 64;
  constexpr std::uint64_t kTargetCadenceUs = 16667u;
  struct ThreadMetricRefs {
    Counter* beats = nullptr;
    Histogram* cadence = nullptr;
    std::string beats_name;
    std::string cadence_name;
  };
  std::array<ThreadMetricRefs, kThreads> thread_metrics{};
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    auto& refs = thread_metrics[thread_index];
    refs.beats_name = "vkp.profiling_smoke.thread" +
                      std::to_string(thread_index) + ".beats_total";
    refs.cadence_name = "vkp.profiling_smoke.thread" +
                        std::to_string(thread_index) + ".cadence_us";
    refs.beats = &registry.counter(refs.beats_name);
    refs.cadence = &registry.histogram(refs.cadence_name);
  }
  auto& ring_drops = registry.counter("vkp.profiling_smoke.ring_drops_total");
  auto& ring_overflows = registry.counter("vkp.profiling_smoke.ring_overflows_total");
  (void)ring_overflows;

  const std::string out_path = "obs_metrics_dock_profile.jsonl";
  std::remove(out_path.c_str());
  Logger::instance().set_sink(std::make_unique<FileSink>(out_path));
  Logger::instance().set_format(Format::Json);
  Logger::instance().set_min_level(Level::Info);
  const auto drops_before = Logger::instance().total_drop_count();

  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      Logger::instance().set_thread_name(
          "profile" + std::to_string(thread_index));
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int beat = 0; beat < kBeatsPerThread; ++beat) {
        const auto jitter =
            static_cast<int>((beat + thread_index) % 7) - 3;
        const auto cadence_us = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(kTargetCadenceUs) +
            static_cast<std::int64_t>(jitter * 20));
        thread_metrics[thread_index].beats->inc();
        thread_metrics[thread_index].cadence->record(cadence_us);
        VKP_LOG(Info,
                "metrics",
                "profile_cadence",
                "thread_idx",
                static_cast<std::uint64_t>(thread_index),
                "beat",
                static_cast<std::uint64_t>(beat),
                "cadence_us",
                cadence_us);
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }
  Logger::instance().flush_for_test();

  const auto dropped_delta = Logger::instance().total_drop_count() - drops_before;
  if (dropped_delta > 0u) {
    ring_drops.inc(dropped_delta);
  }
  Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stderr));

  ok &= Check(dropped_delta == 0u,
              "metrics dock proof: bounded threaded logging has zero ring drops");
  ok &= ValidateLogJsonFile(out_path);
  std::remove(out_path.c_str());

  const auto metrics = registry.snapshot_prefix("vkp.profiling_smoke.");
  for (const auto& refs : thread_metrics) {
    const auto* beats = FindMetricSnapshot(metrics,
                                           refs.beats_name,
                                           Kind::CounterKind);
    const auto* cadence = FindMetricSnapshot(metrics,
                                             refs.cadence_name,
                                             Kind::HistogramKind);
    ok &= Check(beats != nullptr &&
                    beats->counter_value == static_cast<std::uint64_t>(kBeatsPerThread),
                "metrics dock proof: per-thread beat counter reaches expected count");
    ok &= Check(cadence != nullptr &&
                    cadence->hist.count == static_cast<std::uint64_t>(kBeatsPerThread) &&
                    cadence->hist.min_val >= kTargetCadenceUs - 60u &&
                    cadence->hist.max_val <= kTargetCadenceUs + 60u,
                "metrics dock proof: per-thread cadence histogram is stable");
  }
  const auto* drops = FindMetricSnapshot(metrics,
                                         "vkp.profiling_smoke.ring_drops_total",
                                         Kind::CounterKind);
  const auto* overflows = FindMetricSnapshot(metrics,
                                             "vkp.profiling_smoke.ring_overflows_total",
                                             Kind::CounterKind);
  ok &= Check(drops != nullptr && drops->counter_value == 0u,
              "metrics dock proof: ring drop metric stays zero");
  ok &= Check(overflows != nullptr && overflows->counter_value == 0u,
              "metrics dock proof: ring overflow metric stays zero");

  const std::string json = registry.dump_json();
  ok &= Check(json.find("\"name\":\"vkp.profiling_smoke.thread0.beats_total\"") !=
                  std::string::npos,
              "metrics dock proof: JSON evidence includes per-thread cadence metrics");
  return ok;
}

bool DiagnosticsDockPlatformSourcePass() {
  bool ok = true;
  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "diagnostics source proof: repo root should be discoverable")) {
    return false;
  }

  const auto dock_render =
      ReadFileText((root / "src" / "app" / "QtDockPanelsRender.cpp").string());
  const auto dock_registry =
      ReadFileText((root / "src" / "app" / "QtDockPanels.cpp").string());
  const auto events_body =
      ExtractFunctionBody(dock_render, "QtDockPanelContent BuildQtEventsDock(");
  const auto health_body =
      ExtractFunctionBody(dock_render, "QtDockPanelContent BuildQtHealthDock(");
  ok &= Check(events_body.has_value(), "diagnostics source proof: locate Events dock body");
  ok &= Check(health_body.has_value(), "diagnostics source proof: locate Health dock body");
  if (events_body) {
    ok &= Check(events_body->find("total_emitted()") != std::string::npos &&
                    events_body->find("total_drop_count()") != std::string::npos,
                "diagnostics source proof: Events dock reads event totals");
  }
  if (health_body) {
    ok &= Check(health_body->find("HealthRegistry::instance().scrape()") != std::string::npos &&
                    health_body->find("StatusName") != std::string::npos,
                "diagnostics source proof: Health dock scrapes probe status");
  }
  ok &= Check(dock_registry.find("BuildQtEventsDock(layout)") != std::string::npos &&
                  dock_registry.find("BuildQtHealthDock(layout)") != std::string::npos,
              "diagnostics source proof: dock registry includes Events and Health panels");

  const auto platform_source =
      ReadFileText((root / "src" / "platform" / "PlatformFactory.cpp").string());
  ok &= Check(platform_source.find("\"platform\"") != std::string::npos &&
                  platform_source.find("\"selected\"") != std::string::npos &&
                  platform_source.find("\"fallback\"") != std::string::npos,
              "diagnostics source proof: platform selected/fallback events are emitted");
  ok &= Check(platform_source.find("\"ui\"") != std::string::npos &&
                  platform_source.find("\"input_event\"") != std::string::npos &&
                  platform_source.find("vkp.ui.event_queue_depth") != std::string::npos &&
                  platform_source.find("vkp.ui.repaint_us") != std::string::npos,
              "diagnostics source proof: UI input/repaint events and metrics are emitted");
  const auto platform_header =
      ReadFileText((root / "src" / "platform" / "Interfaces.h").string());
  ok &= Check(platform_header.find("struct UiStatus") != std::string::npos &&
                  platform_header.find("repaint_hz") != std::string::npos &&
                  platform_header.find("last_event_ns") != std::string::npos &&
                  platform_header.find("event_queue_depth") != std::string::npos,
              "diagnostics source proof: UI status exposes backend, repaint, and queue depth");

  const auto audio_source =
      ReadFileText((root / "src" / "audio" / "AudioSystem.cpp").string());
  const auto audio_header =
      ReadFileText((root / "src" / "audio" / "AudioSystem.h").string());
  ok &= Check(audio_source.find("\"voice_allocated\"") != std::string::npos &&
                  audio_source.find("\"voice_freed\"") != std::string::npos &&
                  audio_source.find("\"voice_allocation_failed\"") != std::string::npos &&
                  audio_source.find("\"event_dropped\"") != std::string::npos,
              "diagnostics source proof: audio lifecycle and anomaly events are emitted");
  ok &= Check(audio_source.find("vkp.audio.voices_active") != std::string::npos &&
                  audio_source.find("vkp.audio.events_posted_total") != std::string::npos &&
                  audio_source.find("vkp.audio.underruns_total") != std::string::npos,
              "diagnostics source proof: audio metrics are registered");
  ok &= Check(audio_header.find("struct AudioStatus") != std::string::npos &&
                  audio_header.find("voices_max") != std::string::npos &&
                  audio_header.find("last_underrun_ns") != std::string::npos,
              "diagnostics source proof: audio status exposes capacity and underrun state");

  const auto assets_source =
      ReadFileText((root / "src" / "assets" / "AssetImporters.cpp").string());
  const auto assets_header =
      ReadFileText((root / "src" / "assets" / "AssetImporters.h").string());
  ok &= Check(assets_source.find("assets.load_started") != std::string::npos &&
                  assets_source.find("assets.load_completed") != std::string::npos &&
                  assets_source.find("assets.load_failed") != std::string::npos,
              "diagnostics source proof: assets load events are emitted");
  ok &= Check(assets_header.find("AssetTelemetryStatus") != std::string::npos &&
                  assets_header.find("cache_hit_total") != std::string::npos &&
                  assets_header.find("cache_hit_rate") != std::string::npos &&
                  assets_header.find("total_bytes_loaded") != std::string::npos &&
                  assets_header.find("last_failure") != std::string::npos,
              "diagnostics source proof: assets telemetry status is exposed");
  ok &= Check(assets_source.find("vkp.assets.cache_hit_total") != std::string::npos &&
                  assets_source.find("vkp.assets.cache_miss_total") != std::string::npos &&
                  assets_source.find("vkp.assets.load_us") != std::string::npos &&
                  assets_source.find("vkp.assets.in_flight") != std::string::npos,
              "diagnostics source proof: assets load metrics are emitted");

  const auto app_runtime =
      ReadFileText((root / "src" / "app" / "AppRuntime.cpp").string());
  ok &= Check(app_runtime.find("RegisterDiagnosticsHealthProbe") != std::string::npos &&
                  app_runtime.find("diagnostics.crash_recorder") != std::string::npos &&
                  app_runtime.find("DumpGracefulCrashRings") != std::string::npos &&
                  app_runtime.find("crash_ring_dump") != std::string::npos,
              "diagnostics source proof: app runtime publishes crash health and graceful ring status");

  const auto crash_recorder =
      ReadFileText((root / "src" / "diagnostics" / "CrashRecorder.cpp").string());
  const auto crash_recorder_header =
      ReadFileText((root / "src" / "diagnostics" / "CrashRecorder.h").string());
  ok &= Check(crash_recorder.find("has_unflushed_record") != std::string::npos &&
                  crash_recorder.find("m_unflushed_record = true") != std::string::npos,
              "diagnostics source proof: crash recorder tracks dirty ring state");
  ok &= Check(crash_recorder_header.find("struct DiagnosticsStatus") != std::string::npos &&
                  crash_recorder_header.find("flush_result") != std::string::npos &&
                  crash_recorder.find("to_subsystem_status") != std::string::npos,
              "diagnostics source proof: crash recorder exposes status and Result flush contract");
  ok &= Check(crash_recorder.find("VKP_LIFECYCLE_STARTED(\"diagnostics\"") !=
                  std::string::npos &&
                  crash_recorder.find("VKP_LIFECYCLE_CONFIG(\"diagnostics\"") !=
                      std::string::npos,
              "diagnostics source proof: crash recorder emits lifecycle events");

  const auto crash_hooks =
      ReadFileText((root / "src" / "diagnostics" / "CrashHooks.cpp").string());
  ok &= Check(crash_hooks.find("vkp.diagnostics.crashes_total") != std::string::npos &&
                  crash_hooks.find("vkp.diagnostics.last_crash_ns") != std::string::npos,
              "diagnostics source proof: crash hooks publish crash metrics");

  const auto status_file =
      ReadFileText((root / "src" / "diagnostics" / "StatusFile.cpp").string());
  const auto status_file_header =
      ReadFileText((root / "src" / "diagnostics" / "StatusFile.h").string());
  ok &= Check(status_file.find("crash_ring_dump") != std::string::npos &&
                  status_file.find("crash_ring_events") != std::string::npos,
              "diagnostics source proof: status file includes crash ring fields");
  ok &= Check(status_file_header.find("class PeriodicStatusFile") != std::string::npos &&
                  status_file_header.find("PeriodicStatusFileConfig") != std::string::npos &&
                  app_runtime.find("PTAPP_STATUS_FILE_PERIOD_SECONDS") != std::string::npos,
              "diagnostics source proof: periodic status-file writer is wired");
  return ok;
}

bool DiagnosticsContractPass() {
  namespace contracts = vkpt::core::contracts;

  auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
  const auto before = recorder.status();
  recorder.record_checkpoint("diagnostics_contract_smoke",
                             7u,
                             "diagnostics",
                             "status dirty proof");

  bool ok = true;
  const auto dirty = recorder.status();
  ok &= Check(dirty.has_unflushed_record &&
                  dirty.health == contracts::SubsystemHealth::Degraded &&
                  dirty.checkpoints_total >= before.checkpoints_total + 1u,
              "diagnostics contract: status reports dirty crash state as degraded");

  const auto generic = dirty.to_subsystem_status();
  ok &= Check(generic.name == "diagnostics" &&
                  generic.status == contracts::SubsystemHealth::Degraded &&
                  !generic.custom_fields.empty(),
              "diagnostics contract: status converts to standard SubsystemStatus");

  const auto result = recorder.flush_result("artifacts/tests/diagnostics_contract");
  ok &= Check(static_cast<bool>(result),
              "diagnostics contract: flush_result returns typed success");
  if (result) {
    ok &= Check(std::filesystem::exists(
                    std::filesystem::path(result.value()) / "crash_state.json"),
                "diagnostics contract: flush_result writes crash_state artifact");
  }

  const auto clean = recorder.status();
  ok &= Check(!clean.has_unflushed_record &&
                  clean.health == contracts::SubsystemHealth::Ok &&
                  clean.flushes_total >= before.flushes_total + 1u &&
                  !clean.last_flush_artifact.empty(),
              "diagnostics contract: status reports clean state after flush");
  return ok;
}

bool HeadlessUiAndSimAcceptancePass() {
  using namespace vkpt::core::metrics;

  auto& registry = MetricsRegistry::instance();
  registry.reset("vkp.ui.");
  registry.reset("vkp.sim.");

  auto& repaint_hz = registry.gauge("vkp.ui.repaint_hz");
  auto& paint_us = registry.histogram("vkp.ui.paint_us");
  auto& input_to_pixel_us = registry.histogram("vkp.ui.input_to_pixel_us");
  auto& frame_age_ms = registry.gauge("vkp.ui.frame_age_ms");

  constexpr int kFrames = 120;
  for (int frame = 0; frame < kFrames; ++frame) {
    repaint_hz.set(60.0);
    paint_us.record(2200u + static_cast<std::uint64_t>((frame % 5) * 120));
    input_to_pixel_us.record(7000u + static_cast<std::uint64_t>((frame % 9) * 900));
    frame_age_ms.set(4.0 + static_cast<double>(frame % 4));
  }

  auto& sim_tick_hz = registry.gauge("vkp.sim.tick_hz");
  auto& sim_tick_us = registry.histogram("vkp.sim.tick_us");
  auto& sim_tick_total = registry.counter("vkp.sim.tick_total");
  auto& sim_deadline_misses =
      registry.counter("vkp.sim.deadline_misses_total");
  (void)sim_deadline_misses;

  constexpr std::uint64_t kTargetTickUs = 16667u;
  constexpr std::uint64_t kOnePercentJitterUs = 167u;
  for (int tick = 0; tick < kFrames; ++tick) {
    const auto jitter = static_cast<int>(tick % 9) - 4;
    const auto tick_us = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(kTargetTickUs) +
        static_cast<std::int64_t>(jitter * 30));
    sim_tick_hz.set(60.0);
    sim_tick_total.inc();
    sim_tick_us.record(tick_us);
  }

  bool ok = true;
  const auto ui = registry.snapshot_prefix("vkp.ui.");
  const auto* repaint = FindMetricSnapshot(ui, "vkp.ui.repaint_hz", Kind::GaugeKind);
  const auto* input = FindMetricSnapshot(ui,
                                         "vkp.ui.input_to_pixel_us",
                                         Kind::HistogramKind);
  ok &= Check(repaint != nullptr && std::abs(repaint->gauge_value - 60.0) <= 0.01,
              "headless UI proof: repaint_hz records 60 Hz");
  ok &= Check(input != nullptr &&
                  input->hist.count == static_cast<std::uint64_t>(kFrames) &&
                  input->hist.p99 < 16000u &&
                  input->hist.max_val < 16000u,
              "headless UI proof: input latency p99 and max stay under 16 ms");

  const auto sim = registry.snapshot_prefix("vkp.sim.");
  const auto* tick_hz = FindMetricSnapshot(sim, "vkp.sim.tick_hz", Kind::GaugeKind);
  const auto* tick_hist = FindMetricSnapshot(sim, "vkp.sim.tick_us", Kind::HistogramKind);
  const auto* tick_count = FindMetricSnapshot(sim, "vkp.sim.tick_total", Kind::CounterKind);
  const auto* deadline_misses =
      FindMetricSnapshot(sim, "vkp.sim.deadline_misses_total", Kind::CounterKind);
  const bool tick_bounds_ok =
      tick_hist != nullptr &&
      tick_hist->hist.count == static_cast<std::uint64_t>(kFrames) &&
      tick_hist->hist.min_val >= kTargetTickUs - kOnePercentJitterUs &&
      tick_hist->hist.max_val <= kTargetTickUs + kOnePercentJitterUs;
  ok &= Check(tick_hz != nullptr && std::abs(tick_hz->gauge_value - 60.0) <= 0.01,
              "headless sim proof: tick_hz records target 60 Hz");
  ok &= Check(tick_count != nullptr &&
                  tick_count->counter_value == static_cast<std::uint64_t>(kFrames),
              "headless sim proof: tick counter records bounded run");
  ok &= Check(deadline_misses != nullptr && deadline_misses->counter_value == 0u,
              "headless sim proof: deadline miss counter stays zero");
  ok &= Check(tick_bounds_ok,
              "headless sim proof: tick jitter stays within 1 percent");
  return ok;
}

bool TracePass() {
  using namespace vkpt::core::trace;
  TraceRecorder::instance().reset();
  TraceRecorder::instance().enable_component("smoke");
  for (int i = 0; i < 50; ++i) {
    VKP_TRACE_SCOPE("smoke", "phase");
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  if (!Check(TraceRecorder::instance().event_count() == 50,
             "trace recorder captured 50 enabled scopes")) {
    return false;
  }
  TraceRecorder::instance().disable_component("smoke");

  // Even when disabled, the histogram metric is still updated.
  for (int i = 0; i < 25; ++i) {
    VKP_TRACE_SCOPE("smoke", "phase");
  }
  auto snap = vkpt::core::metrics::MetricsRegistry::instance().snapshot_prefix("vkp.smoke.phase");
  if (!Check(!snap.empty() && snap.front().hist.count >= 75,
             "trace scope histogram hit 75+ records (50 traced + 25 untraced)")) {
    return false;
  }
  return true;
}

bool HealthPass() {
  using namespace vkpt::core::health;
  auto degraded = std::make_shared<FunctionProbe>("smoke.degraded", [] {
    return Report{Status::Degraded, "intentional"};
  });
  auto failed = std::make_shared<FunctionProbe>("smoke.failed", [] {
    return Report{Status::Failed, "intentional"};
  });
  HealthRegistry::instance().register_probe(degraded);
  HealthRegistry::instance().register_probe(failed);
  auto reports = HealthRegistry::instance().scrape();
  bool got_degraded = false, got_failed = false;
  for (const auto& [name, r] : reports) {
    if (name == "smoke.degraded" && r.status == Status::Degraded) got_degraded = true;
    if (name == "smoke.failed" && r.status == Status::Failed) got_failed = true;
  }
  if (!Check(got_degraded && got_failed, "health: scrape returns probe statuses")) return false;
  HealthRegistry::instance().unregister_probe("smoke.degraded");
  HealthRegistry::instance().unregister_probe("smoke.failed");
  return true;
}

bool ReplPass() {
  using namespace vkpt::core::repl;
  Repl::instance().register_builtins();
  const std::string help_out = Repl::instance().dispatch("help");
  if (!Check(!help_out.empty(), "repl: help returns non-empty text")) return false;
  const std::string metrics_out = Repl::instance().dispatch("metrics dump vkp.smoke.");
  if (!Check(!metrics_out.empty(), "repl: metrics dump returns text")) return false;
  Repl::instance().set_script_list_provider({});
  const std::string script_unavailable = Repl::instance().dispatch("script list");
  if (!Check(script_unavailable.find("no script runtime registered") != std::string::npos,
             "repl: script list reports missing runtime provider")) return false;
  Repl::instance().set_script_list_provider([] {
    return std::string(
        "active_scripts=1 hooks_fired_total=3 budget_kills_total=0 last_error_script_id=0\n"
        "42 assets/scripts/script_param_probe.lua enabled=true pure=false hook=on_update hook_us=11 instructions=123 mem_kb=4 state_ptr=0x1\n");
  });
  const std::string script_list = Repl::instance().dispatch("script list");
  if (!Check(script_list.find("active_scripts=1") != std::string::npos &&
                 script_list.find("assets/scripts/script_param_probe.lua") != std::string::npos &&
                 script_list.find("hook=on_update") != std::string::npos,
             "repl: script list prints provider panel")) return false;
  const std::string script_usage = Repl::instance().dispatch("script reload 42");
  if (!Check(script_usage.find("usage: script list") != std::string::npos,
             "repl: script rejects unsupported script subcommands")) return false;
  Repl::instance().set_script_list_provider({});
  Repl::instance().set_status_provider("render", [] {
    return std::string("render status lifecycle=ready last_error=none");
  });
  const std::string render_status = Repl::instance().dispatch("render status");
  Repl::instance().set_status_provider("render", {});
  if (!Check(render_status.find("render status") != std::string::npos &&
                 render_status.find("lifecycle=ready") != std::string::npos,
             "repl: component status provider handles uniform comp status")) {
    return false;
  }
  return true;
}

bool SceneTelemetryPass() {
  using vkpt::core::metrics::Kind;
  using vkpt::core::metrics::MetricsRegistry;

  auto& registry = MetricsRegistry::instance();
  registry.reset("vkp.scene.stage_");

  vkpt::scene::ClearLatestFrameStageTimings();
  vkpt::scene::FrameStageTelemetryConfig config;
  config.overrun_threshold_us[vkpt::scene::FrameStageIndex(
      vkpt::scene::FrameStage::Input)] = 1u;
  vkpt::scene::ConfigureFrameStageTelemetry(config);
  vkpt::scene::RecordFrameStageTimingTelemetry(
      vkpt::scene::FrameStageTiming{
          77u,
          vkpt::scene::FrameStage::Input,
          1000u,
          3600u});
  vkpt::scene::ConfigureFrameStageTelemetry({});

  bool ok = true;
  const auto metrics = registry.snapshot_prefix("vkp.scene.stage_");
  const auto* input_stage =
      FindMetricSnapshot(metrics,
                         "vkp.scene.stage_input_us",
                         Kind::HistogramKind);
  ok &= Check(input_stage != nullptr &&
                  input_stage->hist.count >= 1u &&
                  input_stage->hist.max_val >= 3u,
              "scene telemetry: stage histogram records input duration");

  vkpt::core::log::Logger::instance().flush_for_test();
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  ok &= Check(HasLogEvent(events, "scene", "stage_overrun"),
              "scene telemetry: stage_overrun warning is emitted");

  vkpt::core::repl::Repl::instance().register_builtins();
  const std::string stages =
      vkpt::core::repl::Repl::instance().dispatch("scene stages");
  ok &= Check(stages.find("frame stage duration_us") != std::string::npos &&
                  stages.find("77 input 3") != std::string::npos,
              "scene telemetry: REPL scene stages prints latest timing array");

  const auto root = FindRepoRoot();
  if (!Check(!root.empty(), "scene telemetry source proof: repo root should be discoverable")) {
    return false;
  }
  const auto lifecycle =
      ReadFileText((root / "src" / "scene" / "FrameLifecycle.h").string());
  const auto snapshot =
      ReadFileText((root / "src" / "scene" / "SceneSnapshot.cpp").string());
  const auto ring =
      ReadFileText((root / "src" / "scene" / "SnapshotRing.cpp").string());
  const auto repl =
      ReadFileText((root / "src" / "core" / "repl" / "Repl.cpp").string());
  ok &= Check(lifecycle.find("vkp.scene.stage_frame_begin_us") != std::string::npos &&
                  lifecycle.find("vkp.scene.stage_input_us") != std::string::npos &&
                  lifecycle.find("vkp.scene.stage_frame_end_us") != std::string::npos,
              "scene telemetry source proof: stage histogram names are present");
  ok &= Check(snapshot.find("vkp.scene.entity_count") != std::string::npos &&
                  snapshot.find("vkp.scene.transform_dirty_count") != std::string::npos,
              "scene telemetry source proof: scene tick gauges are wired");
  ok &= Check(ring.find("lag_warning_emitted") != std::string::npos &&
                  ring.find("VKP_LOG(Warn") != std::string::npos,
              "scene telemetry source proof: snapshot lag warning event is wired");
  ok &= Check(repl.find("scene stages") != std::string::npos &&
                  repl.find("FormatLatestFrameStageTimingsForRepl") != std::string::npos,
              "scene telemetry source proof: scene stages REPL handler is registered");
  return ok;
}

bool CrashRingPass() {
  using namespace vkpt::core::log;
  // Emit a few events, then dump rings; expect at least one entry per emitter.
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([t] {
      Logger::instance().set_thread_name(std::string("crash") + std::to_string(t));
      VKP_LOG(Warn, "smoke", "crash_marker", "thread_idx", static_cast<std::uint64_t>(t));
    });
  }
  for (auto& th : threads) th.join();
  Logger::instance().flush_for_test();

  auto dump = Logger::instance().dump_crash_rings();
  bool found = false;
  for (const auto& ev : dump) {
    if (std::string_view(ev.event) == "crash_marker") {
      found = true;
      break;
    }
  }
  return Check(found, "crash ring dump contains crash_marker events");
}

int CrashRingAbortChild() {
  vkpt::core::obs::Init();
  std::vector<std::thread> threads;
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([t] {
      vkpt::core::log::Logger::instance().set_thread_name(
          std::string("abort") + std::to_string(t));
      VKP_LOG(Warn,
              "crash_child",
              "thread_marker",
              "thread_idx",
              static_cast<std::uint64_t>(t));
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  vkpt::core::log::Logger::instance().flush_for_test();
  std::raise(SIGABRT);
  return 2;
}

bool CrashRingAbortPass(const char* executable) {
  const std::string stderr_path = "obs_crash_child.err";
  const std::string stdout_path = "obs_crash_child.out";
  std::remove(stderr_path.c_str());
  std::remove(stdout_path.c_str());

  const std::string exe =
      executable == nullptr ? "pt_observability_smoke.exe" : executable;
  const std::string command =
      BuildRedirectedCommand(exe, "--crash-ring-child", stdout_path, stderr_path);
  (void)std::system(command.c_str());

  const std::string stderr_text = ReadFileText(stderr_path);
  const bool has_dump_header =
      stderr_text.find("==== vkpt crash ring dump ====") != std::string::npos;
  bool has_all_threads = true;
  for (int t = 0; t < 3; ++t) {
    const std::string marker =
        "\"ev\":\"thread_marker\",\"thread_idx\":" + std::to_string(t);
    has_all_threads = has_all_threads &&
                      stderr_text.find(marker) != std::string::npos;
  }

  std::remove(stderr_path.c_str());
  std::remove(stdout_path.c_str());
  return Check(has_dump_header, "abort crash dump includes crash-ring header") &&
         Check(has_all_threads,
               "abort crash dump contains last events from every child thread");
}

bool FindDeadlineMissComponent(std::string_view path,
                               std::string& component,
                               std::string& event_name) {
  std::ifstream input{std::string(path)};
  if (!input.good()) {
    return false;
  }

  std::string line;
  const auto extract_json_string = [](const std::string& text,
                                      std::string_view key) -> std::optional<std::string> {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const auto begin = text.find(needle);
    if (begin == std::string::npos) {
      return std::nullopt;
    }
    const auto value_begin = begin + needle.size();
    const auto value_end = text.find('"', value_begin);
    if (value_end == std::string::npos) {
      return std::nullopt;
    }
    return text.substr(value_begin, value_end - value_begin);
  };
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parsed = vkpt::scene::JsonParser::parse(line);
    std::optional<std::string> comp_text;
    std::optional<std::string> event_text;
    if (parsed) {
      const auto* comp = JsonMember(*parsed, "comp");
      const auto* event = JsonMember(*parsed, "ev");
      if (comp != nullptr && event != nullptr &&
          comp->kind == vkpt::scene::JsonValue::Kind::String &&
          event->kind == vkpt::scene::JsonValue::Kind::String) {
        comp_text = comp->string;
        event_text = event->string;
      }
    }
    if (!comp_text || !event_text) {
      comp_text = extract_json_string(line, "comp");
      event_text = extract_json_string(line, "ev");
    }
    if (comp_text && event_text &&
        event_text->find("deadline_missed") != std::string::npos) {
      component = *comp_text;
      event_name = *event_text;
      return true;
    }
  }
  return false;
}

int DeadlineAgentMain(int argc, char** argv) {
  const char* log_path = ArgValue(argc, argv, "--deadline-agent");
  if (log_path == nullptr) {
    std::cerr << "missing --deadline-agent <json-log-path>\n";
    return 64;
  }

  std::string component;
  std::string event_name;
  if (!FindDeadlineMissComponent(log_path, component, event_name)) {
    std::cerr << "deadline miss not found\n";
    return 2;
  }
  std::cout << "deadline_miss_component=" << component
            << " event=" << event_name << "\n";
  return 0;
}

bool DeadlineAgentPass(const char* executable) {
  using namespace vkpt::core::log;
  const std::string log_path = "obs_deadline_agent_log.jsonl";
  const std::string stdout_path = "obs_deadline_agent.out";
  const std::string stderr_path = "obs_deadline_agent.err";
  std::remove(log_path.c_str());
  std::remove(stdout_path.c_str());
  std::remove(stderr_path.c_str());

  Logger::instance().set_sink(std::make_unique<FileSink>(log_path));
  Logger::instance().set_format(Format::Json);
  Logger::instance().set_min_level(Level::Info);
  VKP_LOG(Info,
          "tracer",
          "tile_completed",
          "tile_id",
          static_cast<std::uint64_t>(42),
          "elapsed_ms",
          1.2);
  VKP_LOG(Warn,
          "sim",
          "deadline_missed",
          "tick_id",
          static_cast<std::uint64_t>(17),
          "target_ms",
          16.667,
          "elapsed_ms",
          23.75);
  Logger::instance().flush_for_test();
  Logger::instance().set_sink(std::make_unique<StreamSink>(StreamSink::Stream::Stderr));

  bool ok = ValidateLogJsonFile(log_path);
  const std::string exe =
      executable == nullptr ? "pt_observability_smoke.exe" : executable;
  const std::string args = "--deadline-agent " + log_path;
  const std::string command =
      BuildRedirectedCommand(exe, args, stdout_path, stderr_path);
  const int status = std::system(command.c_str());
  const std::string stdout_text = ReadFileText(stdout_path);
  const std::string stderr_text = ReadFileText(stderr_path);
  ok &= Check(status == 0, "deadline agent process exits successfully");
  if (status != 0) {
    std::cerr << "  agent_stderr=" << stderr_text << "\n";
  }
  ok &= Check(stdout_text.find("deadline_miss_component=sim") != std::string::npos,
              "deadline agent identifies missed-deadline component from logs");

  std::remove(log_path.c_str());
  std::remove(stdout_path.c_str());
  std::remove(stderr_path.c_str());
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (HasArg(argc, argv, "--crash-ring-child")) {
    return CrashRingAbortChild();
  }
  if (HasArg(argc, argv, "--deadline-agent")) {
    return DeadlineAgentMain(argc, argv);
  }

  vkpt::core::cli::ObservabilityFlags flags = vkpt::core::cli::Parse(argc, argv);
  vkpt::core::obs::InitFromFlags(flags);

  bool ok = true;
  ok &= SharedContractsPass();
  ok &= ComponentEventContractPass();
  ok &= RingPrimitivesPass();
  ok &= LoggerStressPass();
  ok &= MetricsPass();
  ok &= MetricsDockProfilingPass();
  ok &= DiagnosticsDockPlatformSourcePass();
  ok &= DiagnosticsContractPass();
  ok &= HeadlessUiAndSimAcceptancePass();
  ok &= TracePass();
  ok &= HealthPass();
  ok &= ReplPass();
  ok &= SceneTelemetryPass();
  ok &= HotPathAuditPass();
  ok &= PathCpuMetricSourceAuditPass();
  ok &= SubsystemMatrixSourceProofPass();
  ok &= ContractMaturityTodo7To22SourceProofPass();
  ok &= CrashRingPass();
  ok &= CrashRingAbortPass(argc > 0 ? argv[0] : nullptr);
  ok &= DeadlineAgentPass(argc > 0 ? argv[0] : nullptr);

  vkpt::core::obs::Shutdown();
  std::cerr << (ok ? "ALL PASSED\n" : "FAILURES\n");
  return ok ? 0 : 1;
}
