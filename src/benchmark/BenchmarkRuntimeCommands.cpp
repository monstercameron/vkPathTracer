#include "benchmark/BenchmarkRuntime.h"
#include "benchmark/BenchmarkRuntimeInternal.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::benchmark::ptbench {

int EchoDescCommand(const std::vector<std::string_view>& args) {
  RunOptions opts;
  opts.echoDesc = true;
  std::string parseError;
  if (!ParseRunArgs(args, opts, &parseError)) {
    std::cerr << parseError << "\n";
    return 1;
  }
  auto desc = ToRunDesc(opts);
  std::string issue;
  if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
    std::cerr << "invalid benchmark descriptor: " << issue << "\n";
    return 1;
  }
  std::cout << vkpt::benchmark::SerializeBenchmarkRunDesc(desc) << "\n";
  return 0;
}

int ValidateArtifactsCommand(const std::vector<std::string_view>& args) {
  std::string dir;
  bool json = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--dir") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --dir\n";
        return 1;
      }
      dir = std::string(args[++i]);
    } else if (args[i] == "--json") {
      json = true;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (dir.empty()) {
    std::cerr << "validate-artifacts requires --dir\n";
    return 1;
  }
  const auto validation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(dir);
  if (json) {
    std::cout << vkpt::benchmark::SerializeBenchmarkArtifactValidation(validation) << "\n";
  } else {
    std::cout << "artifacts: " << dir << "\n";
    std::cout << "valid: " << (validation.ok ? "yes" : "no") << "\n";
    for (const auto& item : validation.missing_files) {
      std::cout << " - missing: " << item << "\n";
    }
    for (const auto& item : validation.empty_files) {
      std::cout << " - empty: " << item << "\n";
    }
    for (const auto& item : validation.invalid_files) {
      std::cout << " - invalid: " << item << "\n";
    }
  }
  return validation.ok ? 0 : 2;
}

std::vector<std::string> BuildRunArgsFromDesc(const vkpt::benchmark::BenchmarkRunDesc& desc) {
  std::vector<std::string> args = {
      "ptbench",
      "run",
      "--scene",
      desc.scene_path,
      "--backend",
      desc.backend,
      "--renderer-path",
      desc.renderer_path,
      "--resolution",
      std::to_string(desc.resolution.width) + "x" + std::to_string(desc.resolution.height),
      "--spp",
      std::to_string(desc.samples_per_pixel),
      "--seed",
      std::to_string(desc.seed),
      "--max-depth",
      std::to_string(desc.max_depth),
      "--duration",
      std::to_string(desc.duration),
      "--warmup-frames",
      std::to_string(desc.warmup_frames),
      "--tolerance-policy",
      desc.tolerance_policy.empty() ? std::string("abs=0.001") : desc.tolerance_policy,
      "--output",
      desc.output_directory,
      "--workers",
      std::to_string(desc.worker_count),
      "--tile-size",
      std::to_string(desc.tile_height),
  };
  if (!desc.reference_image.empty()) {
    args.push_back("--reference-image");
    args.push_back(desc.reference_image);
  }
  if (desc.deterministic) {
    args.push_back("--deterministic");
  }
  return args;
}

std::vector<std::string_view> ToArgViews(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.emplace_back(arg);
  }
  return views;
}

class CliBenchmarkRunner final : public vkpt::benchmark::IBenchmarkRunner {
 public:
  vkpt::core::Result<vkpt::benchmark::BenchmarkResult> run_once(
      const vkpt::benchmark::BenchmarkRunDesc& desc) override {
    std::string issue;
    if (!vkpt::benchmark::ValidateBenchmarkRunDesc(desc, &issue)) {
      return vkpt::core::Result<vkpt::benchmark::BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    const auto args = BuildRunArgsFromDesc(desc);
    const auto views = ToArgViews(args);
    const int rc = RunCommand(views);
    if (rc != 0) {
      return vkpt::core::Result<vkpt::benchmark::BenchmarkResult>::error(vkpt::core::ErrorCode::Internal);
    }
    return vkpt::benchmark::LoadBenchmarkResultFromFile(
        (std::filesystem::path(desc.output_directory) / "results.json").string());
  }

  vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>> run_suite(
      const std::vector<vkpt::benchmark::BenchmarkRunDesc>& descs) override {
    std::vector<vkpt::benchmark::BenchmarkResult> results;
    results.reserve(descs.size());
    for (const auto& desc : descs) {
      auto result = run_once(desc);
      if (!result) {
        return vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>>::error(result.error());
      }
      results.push_back(std::move(result.value()));
    }
    return vkpt::core::Result<std::vector<vkpt::benchmark::BenchmarkResult>>::ok(std::move(results));
  }

  vkpt::benchmark::BenchmarkArtifactValidation validate_artifacts(std::string_view artifact_directory) const override {
    return vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(artifact_directory);
  }

  std::string summarize_results(const std::vector<vkpt::benchmark::BenchmarkResult>& results) const override {
    return vkpt::benchmark::SummarizeBenchmarkResults(results);
  }
};

vkpt::core::Result<vkpt::benchmark::BenchmarkResult> RunCliBenchmarkOnce(
    const vkpt::benchmark::BenchmarkRunDesc& desc) {
  CliBenchmarkRunner runner;
  return runner.run_once(desc);
}


}  // namespace vkpt::benchmark::ptbench
