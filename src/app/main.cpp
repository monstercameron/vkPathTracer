#include <filesystem>
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "build_info.generated.h"
#include "core/Logging.h"
#include "pathtracer/PathTracer.h"
#include "platform/HeadlessPlatform.h"
#include "scene/Scene.h"
#include "render/backends/BackendFactory.h"
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

void PrintUsage() {
  std::cout << "ptapp [--version] [--doctor] [--headless] [--scene <path>] [--backend <name>]\n";
  std::cout << "  --version             Print build metadata and exit\n";
  std::cout << "  --doctor              Run basic self-diagnostics\n";
  std::cout << "  --list-backends       Print known render backends and capabilities\n";
  std::cout << "  --headless            Initialize headless platform\n";
  std::cout << "  --scene <path>        Set startup scene\n";
  std::cout << "  --backend <name>      Select backend\n";
  std::cout << "  --log-level <n>       Select log level\n";
  std::cout << "  --crash-test          Simulate a recoverable crash path\n";
  std::cout << "  --json                Emit JSON for --version\n";
  std::cout << "  --render              Render using scalar CPU path tracer\n";
  std::cout << "  --output <path>       Render output PNG path (default artifacts/renders/cornell.png)\n";
  std::cout << "  --exr-output <path>   Render output EXR-like path\n";
  std::cout << "  --width <px>          Render width\n";
  std::cout << "  --height <px>         Render height\n";
  std::cout << "  --spp <samples>       Samples per pixel\n";
  std::cout << "  --max-depth <depth>    Max ray depth\n";
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
  InitializeLogging();
  auto& logger = vkpt::log::Logger::instance();

  const std::vector<std::string_view> args(argv, argv + argc);
  bool showVersion = false;
  bool versionJson = false;
  bool headless = false;
  bool crashTest = false;
  bool doctor = false;
  bool listBackends = false;
  bool doRender = false;
  std::string_view scenePath;
  std::string_view backend;
  std::string_view outputPath = "artifacts/renders/cornell.png";
  std::string_view exrOutputPath;
  std::string_view logLevel = "info";
  uint32_t width = 320;
  uint32_t height = 240;
  uint32_t spp = 16;
  uint32_t maxDepth = 6;

  for (size_t i = 1; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--version") {
      showVersion = true;
    } else if (token == "--doctor") {
      doctor = true;
    } else if (token == "--list-backends") {
      listBackends = true;
    } else if (token == "--json") {
      versionJson = true;
    } else if (token == "--headless") {
      headless = true;
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --scene\n";
        return 1;
      }
      scenePath = args[++i];
    } else if (token == "--backend") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --backend\n";
        return 1;
      }
      backend = args[++i];
    } else if (token == "--log-level") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --log-level\n";
        return 1;
      }
      logLevel = args[++i];
    } else if (token == "--render") {
      doRender = true;
    } else if (token == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --output\n";
        return 1;
      }
      outputPath = args[++i];
    } else if (token == "--exr-output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --exr-output\n";
        return 1;
      }
      exrOutputPath = args[++i];
    } else if (token == "--width") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], width)) {
        std::cerr << "invalid value for --width\n";
        return 1;
      }
    } else if (token == "--height") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], height)) {
        std::cerr << "invalid value for --height\n";
        return 1;
      }
    } else if (token == "--spp") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], spp)) {
        std::cerr << "invalid value for --spp\n";
        return 1;
      }
    } else if (token == "--max-depth") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], maxDepth)) {
        std::cerr << "invalid value for --max-depth\n";
        return 1;
      }
    } else if (token == "--crash-test") {
      crashTest = true;
    } else if (token == "--help" || token == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      PrintUsage();
      return 1;
    }
  }

  logger.log(vkpt::log::Severity::Info, "app", "arg parse complete");

  if (showVersion) {
    if (versionJson) {
      PrintVersionJson();
    } else {
      PrintVersionText();
    }
    return 0;
  }

  if (doctor) {
    PrintBackendDiagnostics();
    std::cout << "doctor: ok\n";
    std::cout << "backend: " << (backend.empty() ? "auto" : backend) << "\n";
    std::cout << "scene: " << (scenePath.empty() ? "none" : scenePath) << "\n";
    std::cout << "build: " << vkpt::build::kProjectVersion << "\n";
    return 0;
  }
  if (listBackends) {
    PrintBackendDiagnostics();
    return 0;
  }

  if (doRender) {
    vkpt::scene::SceneDocument document;
    if (!scenePath.empty()) {
      auto parseResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
      if (!parseResult) {
        std::cerr << "failed to parse scene: " << scenePath << "\n";
        return 2;
      }
      document = parseResult.value();
    } else {
      document.metadata.scene_name = "cornell";
    }

    vkpt::pathtracer::RenderSettings settings{};
    settings.width = std::max<uint32_t>(1, width);
    settings.height = std::max<uint32_t>(1, height);
    settings.spp = std::max<uint32_t>(1, spp);
    settings.max_depth = std::max<uint32_t>(1, maxDepth);
    settings.seed = 0xBAADF00DULL;

    vkpt::pathtracer::ScalarCpuPathTracer tracer;
    if (!tracer.configure(settings)) {
      std::cerr << "pathtracer configure failed\n";
      return 2;
    }

    auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!sceneResult) {
      std::cerr << "scene conversion failed\n";
      return 2;
    }
    if (!tracer.load_scene_snapshot(sceneResult.value()) || !tracer.build_or_update_acceleration()) {
      std::cerr << "failed to prepare scene for path tracing\n";
      return 2;
    }

    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path());
    tracer.reset_accumulation();
    for (uint32_t sample = 0; sample < settings.spp; ++sample) {
      if (!tracer.render_sample_batch(0, settings.height, sample, 0)) {
        std::cerr << "render failed\n";
        return 2;
      }
    }

    const auto ldr = tracer.resolve_ldr();
    const auto hdr = tracer.resolve_hdr();
    std::string saveError;
    if (!vkpt::pathtracer::SavePngCompat(std::string(outputPath), ldr, &saveError)) {
      std::cerr << "png save failed: " << saveError << "\n";
      return 2;
    }
    if (!exrOutputPath.empty()) {
      if (!vkpt::pathtracer::SaveExrCompat(std::string(exrOutputPath), hdr, &saveError)) {
        std::cerr << "exr save failed: " << saveError << "\n";
        return 2;
      }
    }

    const auto counters = tracer.read_counters();
    std::cout << "render complete: " << outputPath << "\n";
    std::cout << "samples: " << counters.samples << "\n";
    std::cout << "rays: " << counters.rays << "\n";
    std::cout << "triangle hits: " << counters.triangle_hits << "\n";
    std::cout << "sdf hits: " << counters.sdf_hits << "\n";
    return 0;
  }

  std::cout << "ptapp started\n";
  std::cout << "mode: " << (headless ? "headless" : "demo") << "\n";
  std::cout << "backend: " << (backend.empty() ? "auto" : backend) << "\n";
  if (!scenePath.empty()) {
    std::cout << "scene: " << scenePath << "\n";
  }
  std::cout << "log level: " << logLevel << "\n";
  logger.log(vkpt::log::Severity::Info, "app", "runtime boot", {
    {"backend", std::string(backend.empty() ? "auto" : backend)},
    {"log_level", std::string(logLevel)},
    {"scene", std::string(scenePath)}
  });

  if (headless || args.size() == 1) {
    vkpt::platform::HeadlessPlatform platform("vkpt-headless");
    auto state = platform.initialize();
    if (!state) {
      std::cerr << "headless init failed\n";
      return 1;
    }
    std::cout << "headless platform initialized\n";
    platform.shutdown();
  }

  if (crashTest) {
    logger.log(vkpt::log::Severity::Fatal, "app", "crash test requested");
    return 42;
  }

  if (!scenePath.empty()) {
    auto parseResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
    if (!parseResult) {
      std::cerr << "Failed to load scene file: " << scenePath << "\n";
      return 2;
    }
    std::cout << "scene entities: " << parseResult.value().snapshot().entity_ids.size() << '\n';
    std::cout << "scene hash: " << parseResult.value().export_hash_hex() << '\n';
    std::cout << "asset refs: " << parseResult.value().snapshot().asset_refs.size() << '\n';
  }

  return 0;
}
