#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

namespace vkpt::benchmark::ptbench {

std::string EscapeJson(std::string_view text);
bool ParseUnsigned(std::string_view text, std::uint32_t& out);
bool ParseUnsigned64(std::string_view text, std::uint64_t& out);
bool ParseResolution(std::string_view text, std::uint32_t& width, std::uint32_t& height);
bool EnsureDirectory(const std::filesystem::path& path);
std::vector<std::string> AvailableRendererPaths(std::string_view backend);
bool ValidateBackendRenderer(std::string_view backend, std::string_view rendererPath, std::string* error);
std::string NowUtcString();
std::string RunIdFromNow();
bool WriteFile(const std::filesystem::path& path, std::string_view text, std::string* error);
std::string NormalizeBackend(std::string_view backend);
std::uint64_t Fnv1a64(std::string_view text);
std::string HashText(std::string_view text);
std::string Hex64(std::uint64_t value);
std::string SerializeManifest(const std::vector<std::string>& names);
std::string ReadProcessEnvRaw(const char* name);
std::string ReadProcessEnvOr(const char* name, const char* fallback);
void SetProcessEnvVar(const char* name, const std::string& value);

struct RunOptions {
  std::string descPath;
  std::string scenePath;
  std::string backend = "cpu";
  std::string rendererPath = "cpu-scalar";
  std::string output = "artifacts/benchmarks/run";
  std::string referenceImage;
  std::string tolerance = "abs=0.001";
  std::uint32_t width = 256;
  std::uint32_t height = 256;
  std::uint32_t spp = 8;
  std::uint64_t seed = 0xBADC0FFEEull;
  std::uint32_t maxDepth = 6;
  double duration = 0.0;
  std::uint32_t warmupFrames = 0;
  std::uint32_t workers = 0;
  std::uint32_t tileHeight = 16;
  bool deterministic = false;
  bool json = false;
  bool echoDesc = false;
};

void ApplyRunDesc(const vkpt::benchmark::BenchmarkRunDesc& desc, RunOptions& out);
vkpt::benchmark::BenchmarkRunDesc ToRunDesc(const RunOptions& opts);
bool ParseRunArgs(const std::vector<std::string_view>& args, RunOptions& out, std::string* error);
std::vector<std::string> BuildRunArgsFromDesc(const vkpt::benchmark::BenchmarkRunDesc& desc);
std::vector<std::string_view> ToArgViews(const std::vector<std::string>& args);

std::string SerializeSceneSnapshot(const vkpt::scene::SceneSnapshot& snapshot);
bool WriteRunArtifacts(const vkpt::benchmark::BenchmarkResult& result,
                       const std::filesystem::path& artifactDir,
                       const vkpt::scene::SceneDocument& scene,
                       const vkpt::pathtracer::RTSceneLayoutManifest& manifest,
                       const std::filesystem::path& referencePath,
                       const std::filesystem::path& diffHeatmapPath,
                       bool includeReference,
                       std::string* error);

struct TolerancePolicy {
  double abs = 0.0;
  double rel = 0.0;
};

struct SceneComparison {
  double mean_abs_error = 0.0;
  double max_error = 0.0;
  double rmse = 0.0;
  std::size_t nan_inf_count = 0;
  std::vector<float> diff;
};

struct ImageRgb {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> rgb;
};

bool ParseTolerance(std::string_view text, TolerancePolicy& policy, std::string* error);
bool LoadImage(const std::filesystem::path& path, ImageRgb& image, std::string* error);
SceneComparison CompareImages(const ImageRgb& left, const ImageRgb& right, const TolerancePolicy& policy);
bool SaveDiffHeatmap(const std::filesystem::path& path,
                     std::uint32_t width,
                     std::uint32_t height,
                     const std::vector<float>& diffs,
                     std::string* error);

vkpt::core::Result<vkpt::benchmark::BenchmarkResult> RunCliBenchmarkOnce(
    const vkpt::benchmark::BenchmarkRunDesc& desc);

}  // namespace vkpt::benchmark::ptbench
