#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "editor/UiModels.h"

namespace vkpt::app {

void PrintBackendDiagnostics();
void PrintAcceleratorDiagnostics(std::uint32_t width, std::uint32_t height);

std::string_view BenchmarkRunBackendFromAction(std::string_view action_id);
std::string_view BenchmarkRendererFromBackend(std::string_view backend);
std::string MakeMenuActionArtifactPath(std::string_view action_id);
bool LaunchBenchmarkRun(const vkpt::editor::RunBenchmarkCommand& command,
                       const std::string& backend,
                       const std::string& renderer,
                       const std::string& scene_path,
                       const std::string& artifact_dir,
                       std::string* out_result_path = nullptr);
std::optional<std::filesystem::path> FindLatestBenchmarkResultJson();
bool OpenPathInExplorer(const std::filesystem::path& path);
std::string ResolveMenuFallbackScenePath(const vkpt::editor::RunBenchmarkCommand& command,
                                        const std::string& active_scene,
                                        const std::string& cli_scene,
                                        const std::string& default_scene);
vkpt::editor::RunBenchmarkCommand ResolveBenchmarkCommand(
    const vkpt::editor::RunBenchmarkCommand& command,
    std::string_view action_id,
    const std::string& active_scene,
    const std::string& cli_scene);

}  // namespace vkpt::app
