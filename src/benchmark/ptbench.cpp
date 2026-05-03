#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "build_info.generated.h"
#include "scene/Scene.h"

namespace {

bool ParseUnsigned(std::string_view text, std::uint32_t& out) {
  try {
    out = std::stoul(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseUint64(std::string_view text, std::uint64_t& out) {
  try {
    out = std::stoull(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDouble(std::string_view text, double& out) {
  try {
    out = std::stod(std::string(text));
    return true;
  } catch (...) {
    return false;
  }
}

void PrintUsage() {
  std::cout << "ptbench <command> [options]\n";
  std::cout << "\ncommands:\n";
  std::cout << "  run                     Resolve and display a benchmark run descriptor\n";
  std::cout << "  list-scenes             List available scene descriptors in assets/scenes\n";
  std::cout << "  list-backends           Print known benchmark backends\n";
  std::cout << "  list-renderer-paths     Print known renderer entry points\n";
  std::cout << "  validate-scene <path>   Validate a scene JSON descriptor\n";
  std::cout << "  compare <A> <B>         Compare two BenchmarkResult JSON files\n";
  std::cout << "  dump-capabilities       Print build/runtime capability snapshot\n";
  std::cout << "  run-experiments         Build resolved run descriptors for scene pack\n";
  std::cout << "\nrun options:\n";
  std::cout << "  --descriptor <path>     Load run descriptor json\n";
  std::cout << "  --scene <path>          Scene file path (fallback when descriptor missing)\n";
  std::cout << "  --backend <name>        Backend override (default scalar-cpu)\n";
  std::cout << "  --spp <n>               Samples per pixel override\n";
  std::cout << "  --width <px> --height <px> resolution override\n";
  std::cout << "\nrun-experiments options:\n";
  std::cout << "  --scene-root <path>     Scene descriptor directory (default assets/scenes)\n";
}

std::string MakeRunId(std::string_view scene_path) {
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  const auto stem = std::filesystem::path(scene_path).stem().string();
  return "run-" + stem + "-" + std::to_string(now);
}

vkpt::benchmark::BenchmarkRunDesc ResolveDefaultRun(std::string_view scenePath,
                                                    std::string_view backend,
                                                    std::uint32_t spp,
                                                    std::uint32_t width,
                                                    std::uint32_t height,
                                                    std::uint64_t seed) {
  vkpt::benchmark::BenchmarkRunDesc out;
  out.scene_path = std::filesystem::absolute(std::filesystem::path(scenePath)).string();
  out.backend = backend.empty() ? "scalar-cpu" : std::string(backend);
  out.renderer_path = std::filesystem::absolute(std::filesystem::path("./build/bin/ptapp")).string();
  out.resolution = {width, height};
  out.samples_per_pixel = spp;
  out.duration = 0.0;
  out.warmup_frames = 0;
  out.seed = seed;
  out.output_directory = "artifacts/benchmarks";
  out.reference_image = "";
  out.tolerance_policy = "abs=0.001";
  return out;
}

int CommandListScenes(std::string_view sceneRoot) {
  using namespace vkpt::scene;
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::exists(sceneRoot)) {
    std::cerr << "scene root missing: " << sceneRoot << "\n";
    return 2;
  }
  for (const auto& entry : std::filesystem::directory_iterator(sceneRoot)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  for (const auto& file : files) {
    auto loaded = SceneDocument::load_from_file(file.string());
    if (!loaded) {
      std::cout << "invalid: " << file.string() << "\n";
      continue;
    }
    std::vector<std::string> issues;
    bool valid = loaded.value().validate(&issues);
    std::cout << (valid ? "valid  " : "invalid") << " " << file.string() << "\n";
    if (!valid) {
      for (const auto& issue : issues) {
        std::cout << "  - " << issue << "\n";
      }
    }
  }
  return 0;
}

int CommandListBackends() {
  std::cout << "scalar-cpu\n";
  return 0;
}

int CommandListRendererPaths() {
  const std::vector<std::string> candidates = {
      "./build/bin/ptapp", "./ptapp", "./build/manual/ptapp", "./cmake-build/ptapp", "./build/debug/ptapp"};
  for (const auto& candidate : candidates) {
    std::cout << candidate << "\n";
  }
  return 0;
}

int CommandValidateScene(std::string_view path) {
  const auto loaded = vkpt::scene::SceneDocument::load_from_file(path);
  if (!loaded) {
    std::cerr << "invalid scene file: " << path << "\n";
    return 2;
  }
  std::vector<std::string> issues;
  if (!loaded.value().validate(&issues)) {
    std::cerr << "scene validation failed:\n";
    for (const auto& issue : issues) {
      std::cerr << "  - " << issue << "\n";
    }
    return 2;
  }
  std::cout << "scene valid: " << path << "\n";
  const auto& doc = loaded.value();
  std::cout << "scene name: " << (doc.metadata.scene_name.empty() ? doc.metadata.schema : doc.metadata.scene_name) << "\n";
  std::cout << "scene hash: " << doc.export_hash_hex() << "\n";
  return 0;
}

int CommandCompare(std::string_view lhsPath, std::string_view rhsPath) {
  const auto lhs = vkpt::benchmark::LoadBenchmarkResultFromFile(lhsPath);
  if (!lhs) {
    std::cerr << "unable to parse lhs result: " << lhsPath << "\n";
    return 2;
  }
  const auto rhs = vkpt::benchmark::LoadBenchmarkResultFromFile(rhsPath);
  if (!rhs) {
    std::cerr << "unable to parse rhs result: " << rhsPath << "\n";
    return 2;
  }
  const auto& a = lhs.value();
  const auto& b = rhs.value();
  std::cout << "scene: " << a.scene << " :: " << b.scene << "\n";
  std::cout << "rendering time delta ms: " << (a.timing.total_ms - b.timing.total_ms) << "\n";
  std::cout << "throughput delta paths/s: " << (a.throughput.paths_per_sec - b.throughput.paths_per_sec) << "\n";
  std::cout << "reference error delta: " << (a.reference_error - b.reference_error) << "\n";
  return 0;
}

int CommandDumpCapabilities() {
  std::cout << "{\n";
  std::cout << "  \"app\": \"ptbench\",\n";
  std::cout << "  \"version\": \"" << vkpt::build::kProjectVersion << "\",\n";
  std::cout << "  \"git\": \"" << vkpt::build::kGitHash << "\",\n";
  std::cout << "  \"build_date\": \"" << vkpt::build::kBuildDate << "\",\n";
  std::cout << "  \"compiler\": \"" << vkpt::build::kCompilerName << "\",\n";
  std::cout << "  \"build_type\": \"" << vkpt::build::kBuildType << "\",\n";
  std::cout << "  \"features\": \"" << vkpt::build::kEnabledFeatureFlags << "\",\n";
  std::cout << "  \"commands\": [\"run\",\"list-scenes\",\"list-backends\",\"list-renderer-paths\",\"validate-scene\",\"compare\",\"dump-capabilities\",\"run-experiments\"]\n";
  std::cout << "}\n";
  return 0;
}

int CommandRun(const std::vector<std::string_view>& args) {
  std::string descriptorPath;
  std::string scenePath;
  std::string backend = "scalar-cpu";
  std::uint32_t spp = 16;
  std::uint32_t width = 320;
  std::uint32_t height = 240;
  std::uint64_t seed = 0xBEEFCAFEuLL;
  bool overrideBackend = false;
  bool overrideSpp = false;
  bool overrideWidth = false;
  bool overrideHeight = false;
  bool overrideSeed = false;

  auto list = args;
  for (size_t i = 0; i < list.size(); ++i) {
    const auto token = list[i];
    if (token == "--descriptor" || token == "--run") {
      if (i + 1 >= list.size()) {
        std::cerr << "missing value for " << token << "\n";
        return 2;
      }
      descriptorPath = std::string(list[++i]);
    } else if (token == "--scene") {
      if (i + 1 >= list.size()) {
        std::cerr << "missing value for --scene\n";
        return 2;
      }
      scenePath = std::string(list[++i]);
    } else if (token == "--backend") {
      if (i + 1 >= list.size()) {
        std::cerr << "missing value for --backend\n";
        return 2;
      }
      backend = std::string(list[++i]);
      overrideBackend = true;
    } else if (token == "--spp") {
      if (i + 1 >= list.size() || !ParseUnsigned(list[++i], spp)) {
        std::cerr << "invalid --spp\n";
        return 2;
      }
      overrideSpp = true;
    } else if (token == "--width") {
      if (i + 1 >= list.size() || !ParseUnsigned(list[++i], width)) {
        std::cerr << "invalid --width\n";
        return 2;
      }
      overrideWidth = true;
    } else if (token == "--height") {
      if (i + 1 >= list.size() || !ParseUnsigned(list[++i], height)) {
        std::cerr << "invalid --height\n";
        return 2;
      }
      overrideHeight = true;
    } else if (token == "--seed") {
      if (i + 1 >= list.size() || !ParseUint64(list[++i], seed)) {
        std::cerr << "invalid --seed\n";
        return 2;
      }
      overrideSeed = true;
    }
  }

  vkpt::benchmark::BenchmarkRunDesc run;
  if (!descriptorPath.empty()) {
    const auto loaded = vkpt::benchmark::LoadBenchmarkRunDescFromFile(descriptorPath);
    if (!loaded) {
      std::cerr << "unable to parse descriptor: " << descriptorPath << "\n";
      return 2;
    }
    run = loaded.value();
  } else {
    if (scenePath.empty()) {
      std::cerr << "no --descriptor or --scene passed\n";
      return 2;
    }
    run = ResolveDefaultRun(scenePath, backend, spp, width, height, seed);
  }
  run.scene_path = std::filesystem::absolute(std::filesystem::path(run.scene_path)).string();
  run.resolution.width = std::max<std::uint32_t>(1, run.resolution.width);
  run.resolution.height = std::max<std::uint32_t>(1, run.resolution.height);
  if (overrideWidth) {
    run.resolution.width = std::max<std::uint32_t>(1, width);
  }
  if (overrideHeight) {
    run.resolution.height = std::max<std::uint32_t>(1, height);
  }
  if (overrideSpp) {
    run.samples_per_pixel = spp;
  } else if (run.samples_per_pixel == 0) {
    run.samples_per_pixel = spp;
  }
  if (overrideBackend) {
    run.backend = backend;
  }
  if (overrideSeed) {
    run.seed = seed;
  }
  run.output_directory = "artifacts/benchmarks";
  run.tolerance_policy = run.tolerance_policy.empty() ? "abs=0.001" : run.tolerance_policy;
  std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(run) << "\n";
  std::cout << "run_id: " << MakeRunId(run.scene_path) << "\n";
  std::cout << "resolved_scene_path: " << run.scene_path << "\n";
  std::cout << "resolved_output_dir: " << run.output_directory << "\n";
  return 0;
}

int CommandRunExperiments(const std::vector<std::string_view>& args) {
  std::string sceneRoot = "assets/scenes";
  std::uint32_t spp = 16;
  std::uint32_t width = 320;
  std::uint32_t height = 240;
  std::string backend = "scalar-cpu";
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--scene-root") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --scene-root\n";
        return 2;
      }
      sceneRoot = std::string(args[++i]);
    } else if (args[i] == "--spp") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], spp)) {
        std::cerr << "invalid --spp\n";
        return 2;
      }
    } else if (args[i] == "--width") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], width)) {
        std::cerr << "invalid --width\n";
        return 2;
      }
    } else if (args[i] == "--height") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], height)) {
        std::cerr << "invalid --height\n";
        return 2;
      }
    } else if (args[i] == "--backend") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --backend\n";
        return 2;
      }
      backend = std::string(args[++i]);
    }
  }

  if (!std::filesystem::exists(sceneRoot)) {
    std::cerr << "scene root missing: " << sceneRoot << "\n";
    return 2;
  }

  for (const auto& entry : std::filesystem::directory_iterator(sceneRoot)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    const auto filePath = entry.path().string();
    auto desc = ResolveDefaultRun(filePath, backend, spp, width, height, 0xBADF00DULL);
    std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(desc) << "\n";
  }
  return 0;
}

std::vector<std::string_view> ArgsToVector(int argc, char** argv) {
  std::vector<std::string_view> args;
  args.reserve(static_cast<size_t>(argc - 2));
  for (int i = 2; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h") {
    PrintUsage();
    return 0;
  }
  if (command == "list-scenes") {
    std::string_view root = "assets/scenes";
    if (argc >= 3) {
      root = argv[2];
    }
    return CommandListScenes(root);
  }
  if (command == "list-backends") {
    return CommandListBackends();
  }
  if (command == "list-renderer-paths") {
    return CommandListRendererPaths();
  }
  if (command == "validate-scene") {
    if (argc < 3) {
      std::cerr << "validate-scene requires path\n";
      return 2;
    }
    return CommandValidateScene(argv[2]);
  }
  if (command == "compare") {
    if (argc < 4) {
      std::cerr << "compare requires two paths\n";
      return 2;
    }
    return CommandCompare(argv[2], argv[3]);
  }
  if (command == "dump-capabilities") {
    return CommandDumpCapabilities();
  }
  if (command == "run-experiments") {
    const auto args = ArgsToVector(argc, argv);
    return CommandRunExperiments(args);
  }
  if (command == "run") {
    const auto args = ArgsToVector(argc, argv);
    return CommandRun(args);
  }

  std::cerr << "unknown command: " << command << "\n";
  PrintUsage();
  return 2;
}
