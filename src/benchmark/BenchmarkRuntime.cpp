#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include "benchmark/BenchmarkRuntime.h"
#include "benchmark/BenchmarkRuntimeInternal.h"
#include "benchmark/BenchmarkTraceProfiler.h"
#include "benchmark/BenchmarkSchema.h"
#include "build_info.generated.h"
#include "core/ExecutionTrace.h"
#include "diagnostics/CrashRecorder.h"
#include "cpu/CpuFeatures.h"
#include "cpu/PacketRay.h"
#include "cpu/ParallelBvhBuilder.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"
#include "cpu/SimdKernelNeon.h"
#include "cpu/SimdKernelSve.h"
#include "cpu/SimdKernelAvx2.h"
#include "cpu/SimdKernelAvx512.h"
#include "cpu/TiledCpuPathTracer.h"
#include "jobs/JobSystem.h"
#include "materials/MaterialDescriptors.h"
#include "pathtracer/PathTracer.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/VulkanBackend.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"
#if defined(PT_ENABLE_VULKAN)
#include "gpu/VulkanGpuPathTracer.h"
#endif
#if defined(PT_ENABLE_D3D12)
#include "gpu/D3D12GpuPathTracer.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <dxgidebug.h>
#include <DXProgrammableCapture.h>
#if defined(PT_HAS_WINPIX_EVENT_RUNTIME)
#include <pix3.h>
#endif
#endif
#endif
#if defined(LoadImage)
#undef LoadImage
#endif
#if !defined(PT_SHADER_SPV_PATH)
#define PT_SHADER_SPV_PATH ""
#endif
#if !defined(PT_SHADER_HLSL_PATH)
#define PT_SHADER_HLSL_PATH ""
#endif

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;

class ScopedPixProgrammaticCapture {
 public:
  explicit ScopedPixProgrammaticCapture(bool enabled) {
#if defined(_WIN32) && defined(PT_ENABLE_D3D12) && defined(PT_HAS_WINPIX_EVENT_RUNTIME)
    if (!enabled) {
      return;
    }
    const std::string capturePath = ReadProcessEnvRaw("PTBENCH_PIX_CAPTURE_PATH");
    PIXCaptureParameters params = {};
    PIXCaptureParameters* paramsPtr = nullptr;
    if (!capturePath.empty()) {
      m_capturePath = Path(capturePath).wstring();
      params.GpuCaptureParameters.FileName = m_capturePath.c_str();
      paramsPtr = &params;
    }
    const HRESULT beginHr = PIXBeginCapture(PIX_CAPTURE_GPU, paramsPtr);
    if (ReadProcessEnvBool("PTBENCH_PIX_VERBOSE")) {
      std::cerr << "PIXBeginCapture hr=0x" << std::hex
                << static_cast<unsigned long>(beginHr) << std::dec
                << " path=" << capturePath << "\n";
    }
    if (SUCCEEDED(beginHr)) {
      m_active = true;
    }
#elif defined(_WIN32) && defined(PT_ENABLE_D3D12)
    if (!enabled) {
      return;
    }
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&m_analysis))) && m_analysis) {
      m_analysis->BeginCapture();
      m_active = true;
    }
#else
    (void)enabled;
#endif
  }

  ~ScopedPixProgrammaticCapture() {
#if defined(_WIN32) && defined(PT_ENABLE_D3D12) && defined(PT_HAS_WINPIX_EVENT_RUNTIME)
    if (m_active) {
      const HRESULT endHr = PIXEndCapture(false);
      if (ReadProcessEnvBool("PTBENCH_PIX_VERBOSE")) {
        std::cerr << "PIXEndCapture hr=0x" << std::hex
                  << static_cast<unsigned long>(endHr) << std::dec << "\n";
      }
    }
#elif defined(_WIN32) && defined(PT_ENABLE_D3D12)
    if (m_active && m_analysis) {
      m_analysis->EndCapture();
    }
#endif
  }

  ScopedPixProgrammaticCapture(const ScopedPixProgrammaticCapture&) = delete;
  ScopedPixProgrammaticCapture& operator=(const ScopedPixProgrammaticCapture&) = delete;

 private:
#if defined(_WIN32) && defined(PT_ENABLE_D3D12)
  bool m_active = false;
#endif
#if defined(_WIN32) && defined(PT_ENABLE_D3D12) && defined(PT_HAS_WINPIX_EVENT_RUNTIME)
  std::wstring m_capturePath;
#endif
#if defined(_WIN32) && defined(PT_ENABLE_D3D12)
  Microsoft::WRL::ComPtr<IDXGraphicsAnalysis> m_analysis;
#endif
};

void LoadPixGpuCapturerForProgrammaticCapture(bool enabled) {
#if defined(_WIN32) && defined(PT_ENABLE_D3D12) && defined(PT_HAS_WINPIX_EVENT_RUNTIME)
  static HMODULE pixGpuCapturer = nullptr;
  if (enabled && pixGpuCapturer == nullptr) {
    pixGpuCapturer = PIXLoadLatestWinPixGpuCapturerLibrary();
    if (ReadProcessEnvBool("PTBENCH_PIX_VERBOSE")) {
      std::cerr << "PIXLoadLatestWinPixGpuCapturerLibrary module="
                << reinterpret_cast<void*>(pixGpuCapturer)
                << " last_error=" << GetLastError() << "\n";
    }
  }
#else
  (void)enabled;
#endif
}

std::string_view ToLower(std::string_view text) {
  static std::array<std::string, 16> store;
  static std::size_t index = 0;
  auto& out = store[index++ % store.size()];
  out.clear();
  out.reserve(text.size());
  for (const auto ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

bool ParseUnsigned(std::string_view text, std::uint32_t& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() ||
      parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool ParseUnsigned64(std::string_view text, std::uint64_t& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool ParseResolution(std::string_view text, std::uint32_t& width, std::uint32_t& height) {
  const auto pos = text.find('x');
  if (pos == std::string_view::npos) {
    return false;
  }
  const auto w = text.substr(0, pos);
  const auto h = text.substr(pos + 1);
  if (w.empty() || h.empty()) {
    return false;
  }
  return ParseUnsigned(w, width) && ParseUnsigned(h, height);
}

std::string NowUtcString() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000u;

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif

  std::ostringstream out;
  out << std::put_time(&tm, "%Y%m%dT%H%M%S");
  out << 'Z';
  out << '.' << std::setfill('0') << std::setw(3) << ms;
  return out.str();
}

std::string RunIdFromNow() {
  static std::uint64_t counter = 0u;
  auto ts = NowUtcString();
  auto value = (std::uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return ts + "-" + std::to_string(value + ++counter);
}

bool WriteFile(const Path& path, std::string_view text, std::string* error = nullptr) {
  std::ofstream output(path);
  if (!output.is_open()) {
    if (error) {
      *error = "failed to open output path: " + path.string();
    }
    return false;
  }
  output << text;
  output.flush();
  if (!output.good()) {
    if (error) {
      *error = "failed to write output path: " + path.string();
    }
    return false;
  }
  return true;
}

bool EnsureDirectory(const Path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

std::string NormalizeBackend(std::string_view backend) {
  const auto normalized = std::string(ToLower(backend));
  if (normalized == "vulkancompute") {
    return "vulkan";
  }
  if (normalized == "cuda" || normalized == "gpu") {
    return "vulkan";
  }
  if (normalized == "dxr") {
    return "d3d12-dxr";
  }
  return normalized.empty() ? "cpu" : normalized;
}

std::vector<std::string> AvailableRendererPaths(std::string_view backend) {
  const auto normalized = NormalizeBackend(backend);
  if (normalized == "vulkan" || normalized == "vulkan-compute") {
    return {"gpu-compute"};
  }
  if (normalized == "d3d12") {
    return {"d3d12-compute", "dxr"};
  }
  if (normalized == "d3d12-dxr") {
    return {"dxr", "d3d12-dxr"};
  }
  if (normalized == "auto" || normalized == "null" || normalized == "cpu") {
    return {"cpu-scalar", "cpu-tiled"};
  }
  return {};
}

bool ValidateBackendRenderer(std::string_view backend, std::string_view rendererPath, std::string* error = nullptr) {
  const auto normalizedBackend = NormalizeBackend(backend);
  const auto normalizedRenderer = std::string(ToLower(rendererPath));
  const auto rendererPaths = AvailableRendererPaths(normalizedBackend);
  if (rendererPaths.empty()) {
    if (error) {
      *error = "unsupported backend: " + std::string(backend);
    }
    return false;
  }
  for (const auto& allowed : rendererPaths) {
    if (allowed == normalizedRenderer) {
      return true;
    }
  }
  if (error) {
    *error = "backend '" + std::string(backend) + "' does not support renderer path '" + std::string(rendererPath) + "'";
  }
  return false;
}

std::uint64_t Fnv1a64(std::string_view text) {
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= kPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value);

std::string HashBytes(const void* data, std::size_t byteCount) {
  if (byteCount == 0) {
    return Hex64(Fnv1a64(""));
  }
  const auto bytes = static_cast<const unsigned char*>(data);
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (std::size_t i = 0; i < byteCount; ++i) {
    hash ^= bytes[i];
    hash *= kPrime;
  }
  return Hex64(hash);
}

std::string HashText(std::string_view text) {
  return HashBytes(text.data(), text.size());
}

std::string Hex64(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 0; i < 16; ++i) {
    out[15 - i] = kHex[(value >> (i * 4)) & 0x0f];
  }
  return out;
}

std::string SerializeManifest(const std::vector<std::string>& names) {
  std::ostringstream out;
  out << "{";
  out << "\"assets\":[";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out << ",";
    out << "{\"uri\":\"" << EscapeJson(names[i]) << "\"}";
  }
  out << "]";
  out << "}";
  return out.str();
}

int RunCommand(const std::vector<std::string_view>& args) {
  RunOptions opts;
  std::string parseError;
  if (!ParseRunArgs(args, opts, &parseError)) {
    std::cerr << parseError << "\n";
    PrintHelp();
    return 1;
  }
  if (opts.echoDesc) {
    auto desc = ToRunDesc(opts);
    std::string issue;
    if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
      std::cerr << "invalid benchmark descriptor: " << issue << "\n";
      return 1;
    }
    std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(desc) << "\n";
    return 0;
  }
  if (opts.scenePath.empty()) {
    std::cerr << "missing --scene\n";
    return 1;
  }

  std::string compatError;
  if (!ValidateBackendRenderer(opts.backend, opts.rendererPath, &compatError)) {
    std::cerr << compatError << "\n";
    return 1;
  }
  const std::string normalizedBackend = NormalizeBackend(opts.backend);
  const std::string normalizedRenderer = std::string(ToLower(opts.rendererPath));
  const bool isVulkanPath = (normalizedBackend == "vulkan" && normalizedRenderer == "gpu-compute");
  const bool isD3D12ComputePath =
      (normalizedBackend == "d3d12" && normalizedRenderer == "d3d12-compute");
  const bool isD3D12DxrPath =
      ((normalizedBackend == "d3d12" || normalizedBackend == "d3d12-dxr") &&
       (normalizedRenderer == "dxr" || normalizedRenderer == "d3d12-dxr"));
  const bool isTiledPath = (std::string(ToLower(opts.rendererPath)) == "cpu-tiled");
  const bool wantsPixProgrammaticCapture =
      ReadProcessEnvBool("PTBENCH_PIX_PROGRAMMATIC_CAPTURE") &&
      (isD3D12ComputePath || isD3D12DxrPath);
  vkpt::core::TraceExecution("ptbench_run_resolved", {
    {"scene", opts.scenePath},
    {"backend", normalizedBackend},
    {"renderer_path", normalizedRenderer},
    {"resolution", std::to_string(opts.width) + "x" + std::to_string(opts.height)},
    {"spp", std::to_string(opts.spp)},
    {"max_depth", std::to_string(opts.maxDepth)},
    {"workers", std::to_string(opts.workers)},
    {"tile_height", std::to_string(opts.tileHeight)},
    {"deterministic", opts.deterministic ? "true" : "false"},
    {"pix_capture", wantsPixProgrammaticCapture ? "true" : "false"}
  });
  LoadPixGpuCapturerForProgrammaticCapture(wantsPixProgrammaticCapture);
  std::optional<ScopedPixProgrammaticCapture> pixCapture;
  if (wantsPixProgrammaticCapture) {
    pixCapture.emplace(true);
  }
  std::unique_ptr<vkpt::render::IRenderBackend> backend;
  if (isVulkanPath) {
    backend = vkpt::render::CreateBackend(normalizedBackend);
    if (!backend) {
      std::cerr << "failed to create backend: " << normalizedBackend << "\n";
      return 2;
    }
    if (!backend->initialize().is_ok()) {
      std::cerr << "failed to initialize backend: " << normalizedBackend << "\n";
      return 2;
    }
    if (!vkpt::render::RunVulkanComputeSmoke(*backend)) {
      std::cerr << "vulkan path smoke test failed\n";
      return 2;
    }

    // Gate 10 (C20): record renderer crash-state snapshot for crash artifacts.
    const auto crashState = vkpt::render::BuildRendererCrashState(*backend, 0u, "ptbench.run:start");
    vkpt::diagnostics::CrashRecorder::instance().update_renderer_state_json(
        vkpt::render::SerializeRenderCrashState(crashState));
  }

  TolerancePolicy tolerance;
  if (!ParseTolerance(opts.tolerance, tolerance, &parseError)) {
    std::cerr << "invalid tolerance policy: " << parseError << "\n";
    return 1;
  }

  const auto sceneResult = vkpt::scene::SceneDocument::load_from_file(opts.scenePath);
  if (!sceneResult) {
    std::cerr << "failed to load scene: " << opts.scenePath << "\n";
    return 2;
  }
  auto scene = sceneResult.value();
  std::vector<std::string> issues;
  if (!scene.validate(&issues)) {
    std::cerr << "scene validation failed:\n";
    for (const auto& issue : issues) {
      std::cerr << " - " << issue << "\n";
    }
    return 2;
  }
  vkpt::core::TraceExecution("ptbench_scene_document_loaded", {
    {"scene", opts.scenePath},
    {"scene_name", scene.metadata.scene_name},
    {"entities", std::to_string(scene.entities.size())},
    {"geometry", std::to_string(scene.geometry.size())},
    {"materials", std::to_string(scene.materials.size())},
    {"assets", std::to_string(scene.assets.size())},
    {"lights", std::to_string(scene.lights.size())}
  });

  const Path artifactDir(opts.output);
  if (!EnsureDirectory(artifactDir)) {
    std::cerr << "cannot create artifact directory: " << artifactDir.string() << "\n";
    return 2;
  }

  const auto runId = RunIdFromNow();
  std::string runError;
  vkpt::benchmark::BenchmarkResult result;
  result.run_id = runId;
  result.scene = opts.scenePath;
  result.backend = normalizedBackend;
  result.renderer_path = opts.rendererPath;
  result.cpu_simd_mode = isVulkanPath ? "simulated-vulkan" : "scalar";
  result.tolerance_policy = opts.tolerance;
  result.resolution.width = opts.width;
  result.resolution.height = opts.height;
  result.spp = opts.spp;
  result.seed = opts.seed;
  result.max_depth = opts.maxDepth;
  result.output_directory = artifactDir.string();
  result.artifact_directory = artifactDir.string();
  result.beauty_png = (artifactDir / "beauty.png").string();
  result.beauty_exr = (artifactDir / "beauty.exr").string();
  result.reference_exr = opts.referenceImage.empty() ? "" : (artifactDir / "reference.exr").string();
  result.diff_heatmap_png = opts.referenceImage.empty() ? "" : (artifactDir / "diff_heatmap.png").string();
  result.profiler_trace_json = (artifactDir / "profiler_trace.json").string();
  result.logs_jsonl = (artifactDir / "logs.jsonl").string();

  result.build_info.app_version = vkpt::build::kProjectVersion;
  result.build_info.git_hash = vkpt::build::kGitHash;
  result.build_info.build_date = vkpt::build::kBuildDate;
  result.build_info.compiler =
      std::string(vkpt::build::kCompilerName) + " " + std::string(vkpt::build::kCompilerVersion);
  result.build_info.build_type = vkpt::build::kBuildType;

  result.device_info.backend = result.backend;
  result.device_info.renderer_path = result.renderer_path;
  result.device_info.cpu_name = "unknown";
  result.device_info.gpu_name = isVulkanPath ? (backend ? backend->name() : "simulated-vulkan") : "";
  result.scene_hash = scene.export_hash_hex();
  std::string assetList;
  for (const auto& asset : scene.assets) {
    assetList += asset.uri;
  }
  result.asset_hash = Hex64(Fnv1a64(assetList));
  result.reference_error = 0.0;
  result.timing = {};
  result.timing_breakdown.clear();
  const auto manifestResult = vkpt::pathtracer::BuildPathTracerSceneSnapshotLayoutManifest();
  if (!manifestResult) {
    std::cerr << "failed to build shader manifest\n";
    return 2;
  }
  result.shader_hash = HashText(SerializePathTracerSceneSnapshotLayoutManifest(manifestResult.value()));
  result.memory = {};
  result.timing.build_ms = 0.0;
  result.timing.render_ms = 0.0;
  result.timing.cpu_ms = 0.0;
  result.throughput.paths_per_sec = 0.0;
  result.throughput.samples_per_sec = 0.0;

  // Benchmark timing starts at scene conversion so build/render/resolve rows
  // stay comparable across CPU, Vulkan, and D3D12 paths.
  TraceProfiler profiler;
  const auto totalProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::FrameStage, "total", "frame", 0u);
  const auto buildStart = std::chrono::high_resolution_clock::now();
  const auto buildProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::BvhBuild, "scene_build", "scene", 0u);
  auto sceneData = vkpt::pathtracer::BuildSceneDataFromDocument(scene);
  if (!sceneData) {
    std::cerr << "failed to build render scene data\n";
    return 2;
  }
  vkpt::core::TraceExecution("ptbench_rt_scene_ready", {
    {"vertices", std::to_string(sceneData.value().vertices.size())},
    {"indices", std::to_string(sceneData.value().indices.size())},
    {"instances", std::to_string(sceneData.value().instances.size())},
    {"materials", std::to_string(sceneData.value().materials.size())},
    {"textures", std::to_string(sceneData.value().textures.size())},
    {"lights", std::to_string(sceneData.value().lights.size())},
    {"sdf_primitives", std::to_string(sceneData.value().sdf_primitives.size())}
  });

  vkpt::pathtracer::RenderSettings renderSettings;
  renderSettings.width = opts.width;
  renderSettings.height = opts.height;
  renderSettings.spp = opts.spp;
  renderSettings.max_depth = opts.maxDepth;
  renderSettings.seed = opts.seed;
  renderSettings.enable_nee = true;
  renderSettings.enable_mis = true;

  std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
  if (isVulkanPath) {
#if defined(PT_ENABLE_VULKAN)
    auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(PT_SHADER_SPV_PATH);
    if (!gpuTracer->is_valid()) {
      std::cerr << "vulkan path tracer init failed: " << gpuTracer->last_error() << "\n";
      return 2;
    }
    result.cpu_simd_mode = "vulkan-compute";
    result.device_info.gpu_name = gpuTracer->gpu_name();
    result.diagnostics.push_back("renderer=gpu-vulkan-compute");
    result.diagnostics.push_back("gpu_name=" + gpuTracer->gpu_name());
    result.diagnostics.push_back("gpu_type=" + gpuTracer->gpu_type());
    result.diagnostics.push_back("gpu_vram_mb=" + std::to_string(gpuTracer->vram_mb()));
    tracer = std::move(gpuTracer);
#else
    result.diagnostics.push_back("renderer=gpu-compute-simulated");
    tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
#endif
  } else if (isD3D12ComputePath || isD3D12DxrPath) {
#if defined(PT_ENABLE_D3D12)
    const std::string d3d12HlslPath = [] {
      const std::string overridePath = ReadProcessEnvRaw("PT_D3D12_HLSL_PATH");
      return overridePath.empty() ? std::string(PT_SHADER_HLSL_PATH) : overridePath;
    }();
    auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(d3d12HlslPath);
    if (!gpuTracer->is_valid()) {
      std::cerr << "d3d12 path tracer init failed: " << gpuTracer->last_error() << "\n";
      return 2;
    }
    if (isD3D12DxrPath) {
      gpuTracer->set_prefer_dxr(true);
      result.cpu_simd_mode = "d3d12-dxr-requested";
      result.diagnostics.push_back("renderer=d3d12-dxr");
    } else {
      result.cpu_simd_mode = "d3d12-compute";
      result.diagnostics.push_back("renderer=d3d12-compute");
    }
    result.device_info.gpu_name = gpuTracer->gpu_name();
    result.diagnostics.push_back("gpu_name=" + gpuTracer->gpu_name());
    result.diagnostics.push_back("gpu_vram_mb=" + std::to_string(gpuTracer->vram_mb()));
    result.diagnostics.push_back("dxr_supported=" + std::string(gpuTracer->dxr_supported() ? "true" : "false"));
    result.diagnostics.push_back("dxr_tier=" + gpuTracer->dxr_tier_string());
    result.diagnostics.push_back("d3d12_hlsl_path=" + d3d12HlslPath);
    tracer = std::move(gpuTracer);
#else
    std::cerr << "D3D12 support is not enabled in this build\n";
    return 2;
#endif
  } else if (isTiledPath) {
    vkpt::cpu::TiledRenderConfig tiledConfig;
    tiledConfig.tile_height = opts.tileHeight;
    tiledConfig.worker_count = opts.workers;
    tiledConfig.deterministic = opts.deterministic;
    tracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
  } else {
    tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
  }
  vkpt::core::TraceExecution("ptbench_tracer_selected", {
    {"backend", normalizedBackend},
    {"renderer_path", normalizedRenderer},
    {"cpu_simd_mode", result.cpu_simd_mode},
    {"gpu_name", result.device_info.gpu_name},
    {"artifact_dir", result.artifact_directory}
  });
  if (!tracer->configure(renderSettings) || !tracer->load_scene_snapshot(sceneData.value()) ||
      !tracer->build_or_update_acceleration()) {
    std::cerr << "path tracer init failed\n";
    return 2;
  }
  const auto buildEnd = std::chrono::high_resolution_clock::now();
  profiler.end_event(buildProfile);
  tracer->reset_accumulation();
  const auto renderStart = std::chrono::high_resolution_clock::now();
  const auto renderProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::RenderPass, "render_samples", "render", 0u);
  for (std::uint32_t sample = 0; sample < opts.spp; ++sample) {
    vkpt::pathtracer::RenderTile tile;
    tile.x = 0u;
    tile.y = 0u;
    tile.width = opts.width;
    tile.height = opts.height;
    tile.sample_index = sample;
    if (!tracer->render_tile(tile, 0u)) {
      std::cerr << "render failed\n";
      return 2;
    }
  }
  profiler.end_event(renderProfile);
  const auto renderEnd = std::chrono::high_resolution_clock::now();
  pixCapture.reset();
  const auto resolveStart = std::chrono::high_resolution_clock::now();
  const auto resolveProfile = profiler.begin_event(vkpt::benchmark::ProfilerEventKind::CpuZone, "resolve_and_write", "io", 0u);

  const auto ldr = tracer->resolve_ldr();
  auto hdr = tracer->resolve_hdr();
  if ((isD3D12ComputePath || isD3D12DxrPath) &&
      (hdr.width == 0u || hdr.height == 0u || hdr.rgbf.empty()) &&
      ldr.width > 0u && ldr.height > 0u && !ldr.rgba8.empty()) {
    hdr.width = ldr.width;
    hdr.height = ldr.height;
    hdr.rgbf.assign(static_cast<std::size_t>(ldr.width) * ldr.height * 3u, 0.0f);
    const std::size_t pixelCount = static_cast<std::size_t>(ldr.width) * ldr.height;
    for (std::size_t i = 0; i < pixelCount; ++i) {
      const std::size_t ldrBase = i * 4u;
      const std::size_t hdrBase = i * 3u;
      hdr.rgbf[hdrBase + 0u] = static_cast<float>(ldr.rgba8[ldrBase + 0u]) / 255.0f;
      hdr.rgbf[hdrBase + 1u] = static_cast<float>(ldr.rgba8[ldrBase + 1u]) / 255.0f;
      hdr.rgbf[hdrBase + 2u] = static_cast<float>(ldr.rgba8[ldrBase + 2u]) / 255.0f;
    }
    result.diagnostics.push_back("hdr_artifact_source=ldr_fallback");
  }
  std::string writeError;
  if (!vkpt::pathtracer::SavePngCompat(result.beauty_png, ldr, &writeError)) {
    std::cerr << "failed to save beauty png: " << writeError << "\n";
    return 2;
  }
  if (!vkpt::pathtracer::SaveExrCompat(result.beauty_exr, hdr, &writeError)) {
    std::cerr << "failed to save beauty exr: " << writeError << "\n";
    return 2;
  }
  profiler.end_event(resolveProfile);
  profiler.end_event(totalProfile);
  const auto resolveEnd = std::chrono::high_resolution_clock::now();

  std::vector<std::uint8_t> beautyBytes;
  {
    std::ifstream in(result.beauty_png, std::ios::binary);
    if (in.is_open()) {
      beautyBytes = std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
  }
  result.image_hash = Hex64(Fnv1a64(std::string_view(reinterpret_cast<const char*>(beautyBytes.data()), beautyBytes.size())));
  const auto counters = tracer->read_counters();
  const double totalMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(renderEnd - buildStart).count();
  const double buildMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(buildEnd - buildStart).count();
  const double renderMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(renderEnd - renderStart).count();
  const double resolveMs =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(resolveEnd - resolveStart).count();

  result.timing.total_ms = totalMs;
  result.timing.build_ms = buildMs;
  result.timing.render_ms = renderMs;
  result.timing.cpu_ms = renderMs;

  // Keep these event names stable; artifact readers use them as the timing
  // breakdown schema rather than as display-only labels.
  result.timing_breakdown.push_back({"scene_build", "scene", buildMs});
  result.timing_breakdown.push_back({"render_samples", "render", renderMs});
  result.timing_breakdown.push_back({"resolve_and_write", "io", resolveMs});
  const double pixels = static_cast<double>(opts.width) * static_cast<double>(opts.height) * std::max(1.0, 1.0 * opts.spp);
  if (renderMs > 0.0) {
    result.throughput.samples_per_sec = 1000.0 * pixels / renderMs;
    result.throughput.paths_per_sec = 1000.0 * static_cast<double>(counters.rays) / renderMs;
  }
  result.score = vkpt::benchmark::ComputeBenchmarkScore(
      result.throughput, result.resolution, std::max(1u, std::thread::hardware_concurrency()));
  result.memory.peak_mb = 0.0;
  result.memory.current_mb = 0.0;

  if (!opts.referenceImage.empty()) {
    ImageRgb reference;
    ImageRgb candidate;
    std::string refErr;
    std::string candErr;
    if (!LoadImage(opts.referenceImage, reference, &refErr)) {
      std::cerr << "failed to load reference: " << refErr << "\n";
      return 2;
    }
    if (!LoadImage(result.beauty_exr, candidate, &candErr)) {
      std::cerr << "failed to load candidate: " << candErr << "\n";
      return 2;
    }
    const auto stats = CompareImages(reference, candidate, tolerance);
    result.reference_error = stats.mean_abs_error;

    const auto referencePath = Path(opts.referenceImage);
    {
      std::error_code copyEc;
      std::filesystem::copy_file(referencePath,
                                 Path(result.reference_exr),
                                 std::filesystem::copy_options::overwrite_existing,
                                 copyEc);
      if (copyEc) {
        std::cerr << "warning: failed to copy reference image: "
                  << copyEc.message() << "\n";
      }
    }
    if (!referencePath.empty()) {
      const Path heatmapPath = Path(result.diff_heatmap_png);
      std::string heatmapErr;
      if (!SaveDiffHeatmap(heatmapPath, std::max(reference.width, candidate.width), std::max(reference.height, candidate.height),
                           stats.diff, &heatmapErr)) {
        std::cerr << "failed to save heatmap: " << heatmapErr << "\n";
      } else {
        (void)stats;
      }
    }
    (void)stats;
  }

  result.diagnostics.push_back("schema=" + std::string(opts.json ? "json" : "text"));

  // F08: record multithreaded benchmark metrics for cpu-tiled path
  if (isTiledPath) {
    const auto* tiled = dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(tracer.get());
    if (tiled) {
      const std::size_t wc = tiled->worker_count();
      const uint32_t th = tiled->tile_height();
      const auto& bvh = tiled->bvh_stats();
      result.diagnostics.push_back("renderer=cpu-tiled");
      result.diagnostics.push_back("worker_count=" + std::to_string(wc));
      result.diagnostics.push_back("tile_height_rows=" + std::to_string(th));
      result.diagnostics.push_back("bvh_nodes=" + std::to_string(bvh.node_count));
      result.diagnostics.push_back("bvh_build_ms=" + std::to_string(bvh.build_ms));
      result.diagnostics.push_back("deterministic=" + std::string(opts.deterministic ? "true" : "false"));
      // samples_per_sec and paths_per_sec already in result.throughput
      // speedup estimate vs scalar: ratio of worker_count (linear scaling assumption)
      const double speedup_estimate = static_cast<double>(wc);
      result.diagnostics.push_back("speedup_estimate_vs_scalar=" + std::to_string(speedup_estimate));
    }
  }

#if defined(PT_ENABLE_D3D12)
  if (isD3D12ComputePath || isD3D12DxrPath) {
    const auto* d3d12 = dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(tracer.get());
    if (d3d12) {
      if (isD3D12DxrPath) {
        result.cpu_simd_mode = d3d12->using_dxr_dispatch() ? "d3d12-dxr" : "d3d12-compute-fallback";
      } else {
        result.cpu_simd_mode = "d3d12-compute";
      }
      result.diagnostics.push_back("prefer_dxr=" + std::string(d3d12->prefer_dxr() ? "true" : "false"));
      result.diagnostics.push_back("using_dxr_dispatch=" + std::string(d3d12->using_dxr_dispatch() ? "true" : "false"));
      result.diagnostics.push_back("rays_per_pixel_per_dispatch=" +
                                   std::to_string(d3d12->rays_per_pixel_per_dispatch()));
      result.diagnostics.push_back("readback_interval=" + std::to_string(d3d12->readback_interval()));
      result.diagnostics.push_back("force_readback_every_sample=" +
                                   std::string(d3d12->force_readback_every_sample() ? "true" : "false"));
      result.diagnostics.push_back("dynamic_instance_transforms=" +
                                   std::string(d3d12->dynamic_instance_transforms_allowed() ? "true" : "false"));
      result.diagnostics.push_back("dxr_build_mode=" + d3d12->dxr_build_mode());
      result.diagnostics.push_back("bvh_leaf_size=" + std::to_string(d3d12->bvh_leaf_size()));
      result.diagnostics.push_back("bvh_bucket_count=" + std::to_string(d3d12->bvh_bucket_count()));
      result.diagnostics.push_back("bvh_split_mode=" + d3d12->bvh_split_mode());
      result.diagnostics.push_back("shader_traversal_mode=" + d3d12->shader_traversal_mode());
      result.diagnostics.push_back("packed_triangle_buffer=" +
                                   std::string(d3d12->packed_triangle_buffer_enabled() ? "true" : "false"));
      result.diagnostics.push_back("compute_packed_triangle_intersections=" +
                                   std::string(d3d12->packed_triangle_buffer_enabled() ? "true" : "false"));
      if (d3d12->using_dxr_dispatch()) {
        result.diagnostics.push_back("dxr_packed_triangle_closest_hit=true");
      }
    }
  }
#endif

  {
    std::ofstream traceFile(result.profiler_trace_json);
    if (traceFile.is_open()) {
      traceFile << profiler.emit_trace() << "\n";
    }
  }

  {
    std::ofstream logFile(result.logs_jsonl);
    if (!logFile.is_open()) {
      std::cerr << "warning: failed to create logs.jsonl\n";
    } else {
      logFile << "{";
      logFile << "\"ts\":\"" << EscapeJson(NowUtcString()) << "\",";
      logFile << "\"severity\":\"info\",";
      logFile << "\"message\":\"run complete\",";
      logFile << "\"fields\":[{\"k\":\"scene\",\"v\":\"" << EscapeJson(result.scene) << "\"},"
              << "{\"k\":\"backend\",\"v\":\"" << EscapeJson(result.backend) << "\"},"
              << "{\"k\":\"renderer\",\"v\":\"" << EscapeJson(result.renderer_path) << "\"},"
              << "{\"k\":\"total_ms\",\"v\":\"" << std::fixed << std::setprecision(6) << result.timing.total_ms << "\"},"
              << "{\"k\":\"normalized_score\",\"v\":\"" << std::fixed << std::setprecision(6)
              << result.score.normalized_score << "\"}"
              << "]"
              << "}\n";
    }
  }

  if (!WriteRunArtifacts(result, artifactDir, scene, manifestResult.value(), Path(opts.referenceImage), Path(result.diff_heatmap_png),
                        !opts.referenceImage.empty(), &runError)) {
    std::cerr << runError << "\n";
    return 2;
  }

  vkpt::core::TraceExecution("ptbench_run_complete", {
    {"scene", result.scene},
    {"backend", result.backend},
    {"renderer_path", result.renderer_path},
    {"artifact_dir", result.artifact_directory},
    {"beauty_png", result.beauty_png},
    {"total_ms", std::to_string(result.timing.total_ms)},
    {"render_ms", std::to_string(result.timing.render_ms)},
    {"normalized_score", std::to_string(result.score.normalized_score)},
    {"samples", std::to_string(counters.samples)},
    {"rays", std::to_string(counters.rays)},
    {"triangle_hits", std::to_string(counters.triangle_hits)},
    {"sdf_hits", std::to_string(counters.sdf_hits)},
    {"reference_image", opts.referenceImage.empty() ? "none" : opts.referenceImage},
    {"reference_error", std::to_string(result.reference_error)}
  });

  const auto artifactValidation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(artifactDir.string());
  if (!artifactValidation.ok) {
    std::cerr << "artifact validation failed: "
              << vkpt::benchmark::SerializeBenchmarkArtifactValidation(artifactValidation) << "\n";
    return 2;
  }

  if (opts.json) {
    std::cout << vkpt::benchmark::SerializeBenchmarkResult(result) << "\n";
  } else {
    std::cout << "run complete\n";
    std::cout << "output: " << artifactDir.string() << "\n";
    std::cout << "beauty: " << result.beauty_png << "\n";
    std::cout << "exr: " << result.beauty_exr << "\n";
    std::cout << "results: " << (artifactDir / "results.json").string() << "\n";
  }
  return 0;
}

}  // namespace vkpt::benchmark::ptbench
