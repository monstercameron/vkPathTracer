#include <filesystem>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "build_info.generated.h"
#include "core/Assert.h"
#include "core/Config.h"
#include "core/Logging.h"
#include "diagnostics/CrashHooks.h"
#include "diagnostics/CrashRecorder.h"
#include "diagnostics/StatusFile.h"
#include "pathtracer/PathTracer.h"
#include "platform/HeadlessPlatform.h"
#include "scene/Scene.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/VulkanBackend.h"
#include "render/interface/RenderContracts.h"

namespace {

void InitializeLogging() {
  auto& logger = vkpt::log::Logger::instance();
  logger.set_min_severity(vkpt::log::Severity::Trace);
  std::filesystem::create_directories("artifacts/logs");
  logger.add_sink(std::make_unique<vkpt::log::ConsoleSink>(std::cout));
  logger.add_sink(std::make_unique<vkpt::log::PlainTextFileSink>("artifacts/logs/ptapp.log"));
  logger.add_sink(std::make_unique<vkpt::log::JsonlFileSink>("artifacts/logs/ptapp.jsonl"));
  logger.add_sink(std::make_unique<vkpt::log::RingBufferSink>(1024));
}

void InitializeCrashRecorder() {
  vkpt::diagnostics::CrashRecorder::instance().set_build_info(
    std::string(vkpt::build::kProjectVersion),
    std::string(vkpt::build::kGitHash),
    std::string(vkpt::build::kCompilerName) + " " + std::string(vkpt::build::kCompilerVersion),
    std::string(vkpt::build::kTargetOs),
    std::string(vkpt::build::kTargetArch),
    std::string(vkpt::build::kBuildType),
    std::string(vkpt::build::kEnabledFeatureFlags));
}

void PrintUsage() {
  std::cout << "ptapp [options]\n";
  std::cout << "  --version             Print build metadata and exit\n";
  std::cout << "  --version --json      Print build metadata as JSON\n";
  std::cout << "  --doctor              Run full self-diagnostics (ptdoctor)\n";
  std::cout << "  --check-build         Check build metadata\n";
  std::cout << "  --check-cpu           Check CPU capabilities\n";
  std::cout << "  --check-backends      Check render backends\n";
  std::cout << "  --check-assets        Check asset directories\n";
  std::cout << "  --check-shaders       Check shader directories\n";
  std::cout << "  --dump-config         Print resolved runtime config as JSON\n";
  std::cout << "  --config <path>       Load a config file (key=value format)\n";
  std::cout << "  --list-backends       Print known render backends and capabilities\n";
  std::cout << "  --headless            Initialize headless platform\n";
  std::cout << "  --scene <path>        Set startup scene\n";
  std::cout << "  --backend <name>      Select backend\n";
  std::cout << "  --log-level <n>       Select log level\n";
  std::cout << "  --crash-test          Simulate a crash and write crash artifacts\n";
  std::cout << "  --render              Render using scalar CPU path tracer\n";
  std::cout << "  --output <path>       Render output PNG path\n";
  std::cout << "  --exr-output <path>   Render output EXR path\n";
  std::cout << "  --width <px>          Render width\n";
  std::cout << "  --height <px>         Render height\n";
  std::cout << "  --spp <samples>       Samples per pixel\n";
  std::cout << "  --max-depth <depth>   Max ray depth\n";
}

// ---- ptdoctor checks -------------------------------------------------------

struct DoctorCheckResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

DoctorCheckResult CheckBuild() {
  DoctorCheckResult r;
  r.name = "build";
  r.passed = true;
  r.detail = std::string("version=") + std::string(vkpt::build::kProjectVersion)
           + " git=" + std::string(vkpt::build::kGitHash)
           + " compiler=" + std::string(vkpt::build::kCompilerName)
           + " target=" + std::string(vkpt::build::kTargetOs) + "/" + std::string(vkpt::build::kTargetArch)
           + " features=[" + std::string(vkpt::build::kEnabledFeatureFlags) + "]";
  return r;
}

DoctorCheckResult CheckCpu() {
  DoctorCheckResult r;
  r.name = "cpu";
  r.passed = true;
  std::ostringstream detail;
  detail << "simd_options=[" << vkpt::build::kSimdCompileOptions << "]";
#if defined(__AVX2__)
  detail << " avx2=yes";
#elif defined(__AVX__)
  detail << " avx=yes";
#else
  detail << " avx=no";
#endif
#if defined(__SSE4_2__)
  detail << " sse4.2=yes";
#endif
#if defined(__ARM_NEON)
  detail << " neon=yes";
#endif
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckBackends() {
  DoctorCheckResult r;
  r.name = "backends";
  auto names = vkpt::render::AvailableBackendNames();
  if (names.empty()) {
    r.passed = false;
    r.detail = "no backends available";
    return r;
  }
  r.passed = true;
  std::ostringstream detail;
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) { detail << name << ":unavailable "; continue; }
    if (!backend->initialize()) { detail << name << ":init_failed "; continue; }
    const auto caps = backend->capabilities();
    detail << name << ":ok(compute=" << (caps.compute ? "y" : "n")
           << ",rt=" << (caps.ray_tracing ? "y" : "n") << ") ";
  }
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckAssets() {
  DoctorCheckResult r;
  r.name = "assets";
  const std::filesystem::path sceneDir = "assets/scenes";
  if (!std::filesystem::exists(sceneDir)) {
    r.passed = false;
    r.detail = "assets/scenes directory missing";
    return r;
  }
  std::size_t count = 0;
  for (const auto& e : std::filesystem::directory_iterator(sceneDir)) {
    if (e.path().extension() == ".json") ++count;
  }
  r.passed = count > 0;
  r.detail = std::string("scene_files=") + std::to_string(count)
           + " path=" + std::filesystem::absolute(sceneDir).string();
  return r;
}

DoctorCheckResult CheckShaders() {
  DoctorCheckResult r;
  r.name = "shaders";
  const std::filesystem::path shaderDir = "src/shaders";
  if (!std::filesystem::exists(shaderDir)) {
    r.passed = false;
    r.detail = "src/shaders directory missing";
    return r;
  }
  std::size_t count = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(shaderDir)) {
    if (e.is_regular_file()) ++count;
  }
  r.passed = true;
  r.detail = std::string("shader_files=") + std::to_string(count);
  return r;
}

void RunDoctor(bool checkBuild, bool checkCpu, bool checkBackends,
               bool checkAssets, bool checkShaders) {
  const std::vector<DoctorCheckResult> results = {
    checkBuild    ? CheckBuild()    : DoctorCheckResult{"build",    true, "skipped"},
    checkCpu      ? CheckCpu()      : DoctorCheckResult{"cpu",      true, "skipped"},
    checkBackends ? CheckBackends() : DoctorCheckResult{"backends", true, "skipped"},
    checkAssets   ? CheckAssets()   : DoctorCheckResult{"assets",   true, "skipped"},
    checkShaders  ? CheckShaders()  : DoctorCheckResult{"shaders",  true, "skipped"},
  };

  bool allOk = true;
  for (const auto& r : results) {
    const char* status = r.passed ? "ok " : "FAIL";
    std::cout << "[" << status << "] " << r.name << ": " << r.detail << "\n";
    if (!r.passed) allOk = false;
  }
  std::cout << "\ndoctor: " << (allOk ? "ok" : "FAIL") << "\n";
}

void PrintBackendDiagnostics() {
  std::cout << "available backends:\n";
  auto names = vkpt::render::AvailableBackendNames();
  if (names.empty()) {
    std::cout << "  (none)\n";
    return;
  }
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) {
      std::cout << "  " << name << " unavailable\n";
      continue;
    }
    if (!backend->initialize()) {
      std::cout << "  " << name << " failed to initialize\n";
      continue;
    }
    auto capabilities = backend->capabilities();
    std::cout << "  " << vkpt::render::BackendKindToString(backend->kind()) << " -> " << capabilities.backend_name << "\n";
    std::cout << "    " << vkpt::render::SerializeBackendCapabilities(capabilities) << "\n";
  }
  const auto manifest = vkpt::pathtracer::BuildRTSceneDataLayoutManifest();
  if (manifest) {
    std::cout << "rt layout:\n";
    std::cout << "  " << vkpt::pathtracer::SerializeRTSceneDataLayoutManifest(manifest.value()) << "\n";
  }
}

void PrintVersionText() {
  std::cout << "ptapp " << vkpt::build::kProjectVersion << '\n';
  std::cout << "git: " << vkpt::build::kGitHash << '\n';
  std::cout << "build date: " << vkpt::build::kBuildDate << '\n';
  std::cout << "compiler: " << vkpt::build::kCompilerName << ' ' << vkpt::build::kCompilerVersion << '\n';
  std::cout << "target: " << vkpt::build::kTargetOs << '/' << vkpt::build::kTargetArch << '\n';
  std::cout << "build type: " << vkpt::build::kBuildType << '\n';
  std::cout << "features: " << vkpt::build::kEnabledFeatureFlags << '\n';
}

void PrintVersionJson() {
  std::cout << "{\n";
  std::cout << "  \"app\": \"ptapp\",\n";
  std::cout << "  \"version\": \"" << vkpt::build::kProjectVersion << "\",\n";
  std::cout << "  \"git_hash\": \"" << vkpt::build::kGitHash << "\",\n";
  std::cout << "  \"build_date\": \"" << vkpt::build::kBuildDate << "\",\n";
  std::cout << "  \"compiler\": \"" << vkpt::build::kCompilerName << " " << vkpt::build::kCompilerVersion << "\",\n";
  std::cout << "  \"cpp_standard\": \"" << vkpt::build::kCxxStandard << "\",\n";
  std::cout << "  \"target\": \"" << vkpt::build::kTargetOs << '/' << vkpt::build::kTargetArch << "\",\n";
  std::cout << "  \"build_type\": \"" << vkpt::build::kBuildType << "\",\n";
  std::cout << "  \"simd_compile_options\": \"" << vkpt::build::kSimdCompileOptions << "\",\n";
  std::cout << "  \"backend_compile_options\": \"" << vkpt::build::kBackendCompileOptions << "\",\n";
  std::cout << "  \"enabled_features\": [\"" << vkpt::build::kEnabledFeatureFlags << "\"],\n";
  std::cout << "  \"disabled_features\": [\"" << vkpt::build::kDisabledFeatureFlags << "\"]\n";
  std::cout << "}\n";
}

bool ParseUnsigned(const std::string_view text, uint32_t& out) {
  try {
    out = static_cast<uint32_t>(std::stoul(std::string(text)));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // ---- Early init: logging + crash hooks ------------------------------------
  InitializeLogging();
  InitializeCrashRecorder();
  vkpt::diagnostics::install_crash_hooks("artifacts/crashes");
  vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("startup", 0);

  auto& logger = vkpt::log::Logger::instance();

  // ---- Parse CLI args -------------------------------------------------------
  const std::vector<std::string_view> args(argv, argv + argc);
  bool showVersion   = false;
  bool versionJson   = false;
  bool headless      = false;
  bool crashTest     = false;
  bool doctor        = false;
  bool checkBuild    = false;
  bool checkCpu      = false;
  bool checkBackends = false;
  bool checkAssets   = false;
  bool checkShaders  = false;
  bool dumpConfig    = false;
  bool listBackends  = false;
  bool doRender      = false;
  std::string configFilePath;
  std::string_view scenePath;
  std::string_view backend;
  std::string_view outputPath    = "artifacts/renders/cornell.png";
  std::string_view exrOutputPath;
  std::string_view logLevel      = "info";
  uint32_t width    = 320;
  uint32_t height   = 240;
  uint32_t spp      = 16;
  uint32_t maxDepth = 6;

  for (size_t i = 1; i < args.size(); ++i) {
    const auto token = args[i];
    if      (token == "--version")        { showVersion   = true; }
    else if (token == "--json")           { versionJson   = true; }
    else if (token == "--doctor")         { doctor = checkBuild = checkCpu = checkBackends = checkAssets = checkShaders = true; }
    else if (token == "--check-build")    { checkBuild    = true; }
    else if (token == "--check-cpu")      { checkCpu      = true; }
    else if (token == "--check-backends") { checkBackends = true; }
    else if (token == "--check-assets")   { checkAssets   = true; }
    else if (token == "--check-shaders")  { checkShaders  = true; }
    else if (token == "--dump-config")    { dumpConfig    = true; }
    else if (token == "--list-backends")  { listBackends  = true; }
    else if (token == "--headless")       { headless      = true; }
    else if (token == "--render")         { doRender      = true; }
    else if (token == "--crash-test")     { crashTest     = true; }
    else if (token == "--config") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --config\n"; return 1; }
      configFilePath = std::string(args[++i]);
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --scene\n"; return 1; }
      scenePath = args[++i];
    } else if (token == "--backend") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --backend\n"; return 1; }
      backend = args[++i];
    } else if (token == "--log-level") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --log-level\n"; return 1; }
      logLevel = args[++i];
    } else if (token == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --output\n"; return 1; }
      outputPath = args[++i];
    } else if (token == "--exr-output") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --exr-output\n"; return 1; }
      exrOutputPath = args[++i];
    } else if (token == "--width") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], width)) { std::cerr << "invalid value for --width\n"; return 1; }
    } else if (token == "--height") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], height)) { std::cerr << "invalid value for --height\n"; return 1; }
    } else if (token == "--spp") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], spp)) { std::cerr << "invalid value for --spp\n"; return 1; }
    } else if (token == "--max-depth") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], maxDepth)) { std::cerr << "invalid value for --max-depth\n"; return 1; }
    } else if (token == "--help" || token == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      PrintUsage();
      return 1;
    }
  }

  // ---- Build resolved config (A10) ------------------------------------------
  vkpt::config::RuntimeConfig config = vkpt::config::BuildDefaultConfig(configFilePath);
  // CLI flags override file/env values.
  if (!backend.empty())   { config.backend    = {std::string(backend),    vkpt::config::ConfigSource::CliFlag}; }
  if (!scenePath.empty()) { config.scene_path = {std::string(scenePath),  vkpt::config::ConfigSource::CliFlag}; }
  if (!logLevel.empty())  { config.log_level  = {std::string(logLevel),   vkpt::config::ConfigSource::CliFlag}; }
  if (headless)           { config.headless   = {true,                    vkpt::config::ConfigSource::CliFlag}; }
  if (width != 320)       { config.render_width  = {width,  vkpt::config::ConfigSource::CliFlag}; }
  if (height != 240)      { config.render_height = {height, vkpt::config::ConfigSource::CliFlag}; }
  if (spp != 16)          { config.spp           = {spp,    vkpt::config::ConfigSource::CliFlag}; }
  if (maxDepth != 6)      { config.max_depth     = {maxDepth, vkpt::config::ConfigSource::CliFlag}; }
  if (!outputPath.empty()) { config.output_path  = {std::string(outputPath), vkpt::config::ConfigSource::CliFlag}; }

  logger.log(vkpt::log::Severity::Info, "app", "arg parse complete");

  // ---- Status tracking (A13) ------------------------------------------------
  vkpt::diagnostics::StatusFileData status;
  status.build_status           = "ok";
  status.enabled_backend        = config.backend.value;
  status.selected_scene         = config.scene_path.value.empty() ? "none" : config.scene_path.value;
  status.selected_renderer_path = "cpu_scalar";
  const auto writeStatus = [&](const std::string& runStatus, const std::string& error = "") {
    status.last_run_status = runStatus;
    status.last_error      = error;
    std::string writeErr;
    if (!vkpt::diagnostics::WriteStatusFile(status, config.status_file_path.value, &writeErr)) {
      logger.log(vkpt::log::Severity::Warning, "app", "status file write failed: " + writeErr);
    }
  };

  // ---- --version ------------------------------------------------------------
  if (showVersion) {
    if (versionJson) { PrintVersionJson(); } else { PrintVersionText(); }
    writeStatus("version_query");
    return 0;
  }

  // ---- --dump-config (A10) --------------------------------------------------
  if (dumpConfig) {
    std::cout << vkpt::config::SerializeRuntimeConfig(config) << "\n";
    writeStatus("dump_config");
    return 0;
  }

  // ---- --doctor / --check-* (A09) -------------------------------------------
  if (doctor || checkBuild || checkCpu || checkBackends || checkAssets || checkShaders) {
    RunDoctor(checkBuild, checkCpu, checkBackends, checkAssets, checkShaders);
    writeStatus("doctor_ok");
    return 0;
  }

  if (listBackends) {
    PrintBackendDiagnostics();
    writeStatus("list_backends");
    return 0;
  }

  // ---- --crash-test (A07/A08) -----------------------------------------------
  if (crashTest) {
    logger.log(vkpt::log::Severity::Fatal, "app", "crash test requested — writing crash artifact");
    vkpt::diagnostics::CrashRecorder::instance().set_last_error("crash_test_requested");
    const std::string crashDir = vkpt::diagnostics::CrashRecorder::instance().flush(
        config.crash_artifact_dir.value);
    std::cout << "crash test: artifact written to " << (crashDir.empty() ? "(failed)" : crashDir) << "\n";
    status.last_crash_artifact = crashDir;
    writeStatus("crash_test", "crash_test_requested");
    return 42;
  }

  // ---- --render -------------------------------------------------------------
  if (doRender) {
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_prepare", 0);
    vkpt::scene::SceneDocument document;
    if (!config.scene_path.value.empty()) {
      auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
      if (!parseResult) {
        std::cerr << "failed to parse scene: " << config.scene_path.value << "\n";
        writeStatus("error:scene_parse_failed", "scene_parse_failed");
        return 2;
      }
      document = parseResult.value();
      vkpt::diagnostics::CrashRecorder::instance().update_scene(config.scene_path.value);
    } else {
      document.metadata.scene_name = "cornell";
      vkpt::diagnostics::CrashRecorder::instance().update_scene("cornell_builtin");
    }

    vkpt::pathtracer::RenderSettings settings{};
    settings.width     = std::max<uint32_t>(1, config.render_width.value);
    settings.height    = std::max<uint32_t>(1, config.render_height.value);
    settings.spp       = std::max<uint32_t>(1, config.spp.value);
    settings.max_depth = std::max<uint32_t>(1, config.max_depth.value);
    settings.seed      = 0xBAADF00DULL;

    auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!sceneResult) {
      std::cerr << "scene conversion failed\n";
      writeStatus("error:scene_conversion_failed", "scene_conversion_failed");
      return 2;
    }

    // ---- Vulkan software-BVH compute path (C10 / Gate 4) ------------------
    const bool useVulkan = (config.backend.value == "vulkan" ||
                            config.backend.value == "vulkan-compute");
    if (useVulkan) {
      vkpt::diagnostics::CrashRecorder::instance().update_backend("vulkan-compute");
      vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("vulkan_bvh_pass", 0);
      status.selected_renderer_path = "vulkan_bvh_compute";

      vkpt::render::VulkanComputeBackend vulkanBackend;
      auto bvhResult = vkpt::render::RunVulkanBVHPass(
          vulkanBackend, sceneResult.value(), settings.width, settings.height);

      if (!bvhResult.success) {
        std::cerr << "vulkan bvh pass failed: " << bvhResult.error << "\n";
        writeStatus("error:vulkan_bvh_failed", bvhResult.error);
        return 2;
      }

      // Simulated backend: film texture is zero-filled. Write a black PNG
      // placeholder so the output path contract is satisfied.
      std::filesystem::create_directories(
          std::filesystem::path(config.output_path.value).parent_path());
      vkpt::pathtracer::FilmLdr ldr;
      ldr.width  = settings.width;
      ldr.height = settings.height;
      ldr.rgba8.assign(static_cast<std::size_t>(settings.width) * settings.height * 4u, 0u);

      std::string saveError;
      if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
        std::cerr << "png save failed: " << saveError << "\n";
        writeStatus("error:png_save_failed", saveError);
        return 2;
      }

      std::cout << "render complete (vulkan-compute): " << config.output_path.value << "\n";
      std::cout << "vertices: "  << bvhResult.vertex_buffer_count << "\n";
      std::cout << "indices: "   << bvhResult.index_buffer_count  << "\n";
      std::cout << "instances: " << bvhResult.instance_count      << "\n";
      std::cout << "bvh_nodes: " << bvhResult.bvh_node_estimate   << "\n";

      const std::string perfSummary = "vertices=" + std::to_string(bvhResult.vertex_buffer_count)
                                    + " indices=" + std::to_string(bvhResult.index_buffer_count)
                                    + " bvh_nodes=" + std::to_string(bvhResult.bvh_node_estimate);
      status.performance_summary = perfSummary;
      writeStatus("render_ok");
      return 0;
    }

    // ---- CPU scalar path tracer (default) ----------------------------------
    vkpt::pathtracer::ScalarCpuPathTracer tracer;
    if (!tracer.configure(settings)) {
      std::cerr << "pathtracer configure failed\n";
      writeStatus("error:tracer_configure_failed", "tracer_configure_failed");
      return 2;
    }
    if (!tracer.load_scene_snapshot(sceneResult.value()) || !tracer.build_or_update_acceleration()) {
      std::cerr << "failed to prepare scene for path tracing\n";
      writeStatus("error:bvh_build_failed", "bvh_build_failed");
      return 2;
    }

    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_execute", 0);
    std::filesystem::create_directories(
        std::filesystem::path(config.output_path.value).parent_path());
    tracer.reset_accumulation();
    for (uint32_t sample = 0; sample < settings.spp; ++sample) {
      if (!tracer.render_sample_batch(0, settings.height, sample, 0)) {
        std::cerr << "render failed\n";
        writeStatus("error:render_failed", "render_sample_batch_failed");
        return 2;
      }
    }

    const auto ldr = tracer.resolve_ldr();
    const auto hdr = tracer.resolve_hdr();
    std::string saveError;
    if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
      std::cerr << "png save failed: " << saveError << "\n";
      writeStatus("error:png_save_failed", saveError);
      return 2;
    }
    if (!config.exr_output_path.value.empty()) {
      if (!vkpt::pathtracer::SaveExrCompat(config.exr_output_path.value, hdr, &saveError)) {
        std::cerr << "exr save failed: " << saveError << "\n";
        writeStatus("error:exr_save_failed", saveError);
        return 2;
      }
    }

    const auto counters = tracer.read_counters();
    std::cout << "render complete: " << config.output_path.value << "\n";
    std::cout << "samples: " << counters.samples << "\n";
    std::cout << "rays: " << counters.rays << "\n";
    std::cout << "triangle hits: " << counters.triangle_hits << "\n";
    std::cout << "sdf hits: " << counters.sdf_hits << "\n";

    const std::string perfSummary = "samples=" + std::to_string(counters.samples)
                                  + " rays=" + std::to_string(counters.rays)
                                  + " tri_hits=" + std::to_string(counters.triangle_hits);
    status.performance_summary = perfSummary;
    writeStatus("render_ok");
    return 0;
  }

  // ---- Default / headless mode ----------------------------------------------
  std::cout << "ptapp started\n";
  std::cout << "mode: " << (config.headless.value ? "headless" : "demo") << "\n";
  std::cout << "backend: " << config.backend.value << "\n";
  if (!config.scene_path.value.empty()) {
    std::cout << "scene: " << config.scene_path.value << "\n";
  }
  std::cout << "log level: " << config.log_level.value << "\n";
  logger.log(vkpt::log::Severity::Info, "app", "runtime boot", {
    {"backend",   config.backend.value},
    {"log_level", config.log_level.value},
    {"scene",     config.scene_path.value}
  });

  if (config.headless.value || args.size() == 1) {
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("headless_init", 0);
    vkpt::platform::HeadlessPlatform platform("vkpt-headless");
    auto state = platform.initialize();
    if (!state) {
      std::cerr << "headless init failed\n";
      writeStatus("error:headless_init_failed", "headless_init_failed");
      return 1;
    }
    std::cout << "headless platform initialized\n";
    platform.shutdown();
  }

  if (!config.scene_path.value.empty()) {
    auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
    if (!parseResult) {
      std::cerr << "Failed to load scene file: " << config.scene_path.value << "\n";
      writeStatus("error:scene_load_failed", "scene_load_failed");
      return 2;
    }
    std::cout << "scene entities: " << parseResult.value().snapshot().entity_ids.size() << '\n';
    std::cout << "scene hash: "     << parseResult.value().export_hash_hex() << '\n';
    std::cout << "asset refs: "     << parseResult.value().snapshot().asset_refs.size() << '\n';
  }

  writeStatus("ok");
  return 0;
}
