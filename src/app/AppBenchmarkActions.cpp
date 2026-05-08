#include "app/AppBenchmarkActions.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pathtracer/PathTracer.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/D3D12Backend.h"
#include "render/interface/RenderContracts.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vkpt::app {

namespace {


std::string QuoteShellArg(std::string_view arg) {
  if (arg.find_first_of(" \"\t\r\n") == std::string_view::npos) {
    return std::string(arg);
  }
  std::string out;
  out.reserve(arg.size() + 2);
  out.push_back('\"');
  for (const char ch : arg) {
    if (ch == '\"') {
      out.append("\\\"");
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\"');
  return out;
}
std::uint64_t BenchmarkActionNowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string ReadProcessEnv(std::string_view name) {
  const std::string key(name);
#if defined(_WIN32)
  char* valueBuffer = nullptr;
  size_t valueLength = 0u;
  if (_dupenv_s(&valueBuffer, &valueLength, key.c_str()) == 0 && valueBuffer != nullptr) {
    std::string value(valueBuffer, valueLength > 0u ? valueLength - 1u : 0u);
    std::free(valueBuffer);
    return value;
  }
  return {};
#else
  const char* value = std::getenv(key.c_str());
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

bool ParseEnvBool(std::string_view name, bool fallback) {
  std::string value = ReadProcessEnv(name);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return fallback;
}

vkpt::render::AcceleratorSelectionPreset ParseAcceleratorPresetFromEnv() {
  std::string value = ReadProcessEnv("PT_D3D12_ACCELERATOR_PRESET");
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "high-performance" || value == "high_performance" || value == "all") {
    return vkpt::render::AcceleratorSelectionPreset::HighPerformance;
  }
  return vkpt::render::AcceleratorSelectionPreset::Auto;
}

}  // namespace


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
  const auto manifest = vkpt::pathtracer::BuildPathTracerSceneSnapshotLayoutManifest();
  if (manifest) {
    std::cout << "rt layout:\n";
    std::cout << "  " << vkpt::pathtracer::SerializePathTracerSceneSnapshotLayoutManifest(manifest.value()) << "\n";
  }
}

void PrintAcceleratorDiagnostics(uint32_t width, uint32_t height) {
  std::cout << "accelerators:\n";
  const bool includeCpu = ParseEnvBool("PT_D3D12_INCLUDE_CPU", true);
  const bool includeIntegratedGpu = ParseEnvBool("PT_D3D12_INCLUDE_INTEGRATED_GPU", true);
  const bool includeWarp = ParseEnvBool("PT_D3D12_INCLUDE_WARP", true);
  const auto accelerators = vkpt::render::EnumerateD3D12Accelerators(includeCpu, includeWarp);
  if (accelerators.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& accelerator : accelerators) {
      std::cout << "  " << vkpt::render::AcceleratorKindToString(accelerator.accelerator_kind)
                << " -> " << accelerator.name << "\n";
      std::cout << "    " << vkpt::render::SerializeAcceleratorCapabilities(accelerator) << "\n";
    }
  }

  vkpt::render::RayBudgetRequest request;
  request.width = width;
  request.height = height;
  request.polygon_frame_budget_ms = 16.6667;
  request.reserved_polygon_ms = 5.0;
  request.merge_budget_ms = 1.0;
  request.include_cpu = includeCpu;
  request.include_integrated_gpu = includeIntegratedGpu;
  request.include_warp = includeWarp;
  request.require_ray_tracing = false;
  const auto printPlan = [](std::string_view label, const vkpt::render::RayBudgetRequest& planRequest) {
    const auto plan = vkpt::render::BuildD3D12RayBudgetPlan(planRequest);
    std::cout << label << " ray budget plan:\n";
    std::cout << "  request=" << vkpt::render::SerializeRayBudgetRequest(planRequest) << "\n";
    std::cout << "  " << vkpt::render::SerializeRayBudgetPlan(plan) << "\n";
  };
  request.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::Auto;
  printPlan("auto", request);
  request.accelerator_preset = ParseAcceleratorPresetFromEnv();
  printPlan(std::string(vkpt::render::AcceleratorSelectionPresetToString(request.accelerator_preset)), request);
}



std::string_view BenchmarkRunBackendFromAction(std::string_view action_id) {
  if (action_id.find("cpu") != std::string_view::npos) {
    return "cpu";
  }
  if (action_id.find("gpu") != std::string_view::npos ||
      action_id.find("backend") != std::string_view::npos) {
    return "vulkan";
  }
  return "cpu";
}

std::string_view BenchmarkRendererFromBackend(std::string_view backend) {
  if (backend == "vulkan") {
    return "gpu-compute";
  }
  return "cpu-scalar";
}

std::string MakeMenuActionArtifactPath(std::string_view action_id) {
  const std::filesystem::path root = "artifacts/benchmarks/ui";
  const std::string actionToken =
      std::string(action_id.empty() ? "menu_action" : action_id);
  std::string safeToken = actionToken;
  for (auto& ch : safeToken) {
    if ((ch < 'a' || ch > 'z') &&
        (ch < 'A' || ch > 'Z') &&
        (ch < '0' || ch > '9') &&
        ch != '-' && ch != '_') {
      ch = '_';
    }
  }
  std::string dir = root.string() + "/" + safeToken + "_" + std::to_string(BenchmarkActionNowMs());
  return dir;
}

std::string ResolveExecutable(std::string_view executable_name) {
#ifdef _WIN32
  wchar_t exePath[MAX_PATH] = {};
  const auto n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (n > 0) {
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path sibling = exeDir / std::string(executable_name);
    if (sibling.extension().empty()) {
      sibling += ".exe";
    }
    std::error_code ec;
    if (std::filesystem::exists(sibling, ec) && !ec) {
      return sibling.string();
    }
    sibling = exeDir / "Release" / (std::string(executable_name) + ".exe");
    ec.clear();
    if (std::filesystem::exists(sibling, ec) && !ec) {
      return sibling.string();
    }
  }
#endif
  std::filesystem::path fallback = std::string(executable_name);
  if (fallback.extension().empty()) {
    fallback += ".exe";
  }
  return fallback.string();
}

bool LaunchBenchmarkRun(const vkpt::editor::RunBenchmarkCommand& command,
                       const std::string& backend,
                       const std::string& renderer,
                       const std::string& scene_path,
                       const std::string& artifact_dir,
                       std::string* out_result_path) {
  const std::string exe = ResolveExecutable("ptbench");
  std::string cmd = QuoteShellArg(exe) + " run";
  cmd += " --scene " + QuoteShellArg(scene_path);
  cmd += " --backend " + QuoteShellArg(backend);
  cmd += " --renderer-path " + QuoteShellArg(renderer);
  cmd += " --resolution " + std::to_string(command.desc.resolution.width) + "x" +
         std::to_string(command.desc.resolution.height);
  cmd += " --spp " + std::to_string(command.desc.samples_per_pixel);
  cmd += " --seed " + std::to_string(command.desc.seed);
  cmd += " --max-depth " + std::to_string(command.desc.max_depth);
  cmd += " --output " + QuoteShellArg(artifact_dir);
  auto exitCode = std::system(cmd.c_str());
  if (out_result_path) {
    *out_result_path = (std::filesystem::path(artifact_dir) / "results.json").string();
  }
  return exitCode == 0;
}

std::optional<std::filesystem::path> FindLatestBenchmarkResultJson() {
  const std::filesystem::path root = "artifacts/benchmarks";
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return std::nullopt;
  }
  std::optional<std::pair<std::filesystem::file_time_type, std::filesystem::path>> latest;
  for (std::filesystem::recursive_directory_iterator it(
           root, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto& entry = *it;
    std::error_code entryEc;
    if (!entry.is_regular_file(entryEc) || entryEc) {
      continue;
    }
    if (entry.path().filename() != "results.json") {
      continue;
    }
    const auto mtime = std::filesystem::last_write_time(entry.path(), entryEc);
    if (!entryEc && (!latest || mtime > latest->first)) {
      latest = std::make_pair(mtime, entry.path());
    }
  }
  if (latest) {
    return latest->second;
  }
  return std::nullopt;
}

[[maybe_unused]] std::vector<std::filesystem::path> ListRecentBenchmarkResultDirs(std::size_t max_items = 16) {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
  const std::filesystem::path root = "artifacts/benchmarks";
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return {};
  }
  for (std::filesystem::recursive_directory_iterator it(
           root, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto& entry = *it;
    std::error_code entryEc;
    if (!entry.is_regular_file(entryEc) || entryEc) {
      continue;
    }
    if (entry.path().filename() != "results.json") {
      continue;
    }
    const auto mtime = std::filesystem::last_write_time(entry.path(), entryEc);
    if (!entryEc) {
      found.emplace_back(mtime, entry.path().parent_path());
    }
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::filesystem::path> out;
  for (std::size_t i = 0; i < found.size() && i < max_items; ++i) {
    if (std::find(out.begin(), out.end(), found[i].second) == out.end()) {
      out.push_back(found[i].second);
    }
  }
  return out;
}

bool OpenPathInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::string command = "start \"\" " + QuoteShellArg(path.string());
  return std::system(command.c_str()) == 0;
#else
  std::string command = "xdg-open " + QuoteShellArg(path.string()) + " > /dev/null 2>&1";
  return std::system(command.c_str()) == 0;
#endif
}

std::string ResolveMenuFallbackScenePath(const vkpt::editor::RunBenchmarkCommand& command,
                                        const std::string& activeScene,
                                        const std::string& cliScene,
                                        const std::string& defaultScene) {
  if (!command.desc.scene_path.empty()) {
    return command.desc.scene_path;
  }
  if (!activeScene.empty()) {
    return activeScene;
  }
  if (!cliScene.empty()) {
    return cliScene;
  }
  if (!defaultScene.empty()) {
    return defaultScene;
  }
  return "scenes/test.json";
}

vkpt::editor::RunBenchmarkCommand ResolveBenchmarkCommand(
    const vkpt::editor::RunBenchmarkCommand& command,
    std::string_view action_id,
    const std::string& active_scene,
    const std::string& cli_scene) {
  auto resolved = command;

  if (resolved.desc.scene_path.empty()) {
    resolved.desc.scene_path = ResolveMenuFallbackScenePath(command, active_scene, cli_scene,
                                                           "scenes/test.json");
  }

  if (resolved.desc.resolution.width == 0u) {
    resolved.desc.resolution.width = 1024u;
  }
  if (resolved.desc.resolution.height == 0u) {
    resolved.desc.resolution.height = 576u;
  }
  if (resolved.desc.samples_per_pixel == 0u) {
    resolved.desc.samples_per_pixel = 128u;
  }
  if (resolved.desc.max_depth == 0u) {
    resolved.desc.max_depth = 10u;
  }
  if (resolved.desc.seed == 0u) {
    resolved.desc.seed = 42u;
  }

  if (resolved.desc.tolerance_policy.empty()) {
    resolved.desc.tolerance_policy = "default";
  }

  if (resolved.desc.backend.empty()) {
    resolved.desc.backend = std::string(BenchmarkRunBackendFromAction(action_id));
  }
  if (resolved.desc.renderer_path.empty() || resolved.desc.renderer_path == "hybrid") {
    resolved.desc.renderer_path = std::string(BenchmarkRendererFromBackend(resolved.desc.backend));
  }
  return resolved;
}
}  // namespace vkpt::app
