#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::benchmark::ptbench {

bool ReadProcessEnvBool(const char* name);
std::vector<std::string> BuildPixAutorunArgs(const std::filesystem::path& configPath = {});
std::vector<std::string_view> MakeArgViews(const std::vector<std::string>& args);

void PrintHelp();

int RunCommand(const std::vector<std::string_view>& args);
int EchoDescCommand(const std::vector<std::string_view>& args);
int ListScenesCommand();
int ListBackendsCommand();
int ListRendererPathsCommand(const std::vector<std::string_view>& args);
int ValidateSceneCommand(const std::vector<std::string_view>& args);
int ValidateArtifactsCommand(const std::vector<std::string_view>& args);
int CompareCommand(const std::vector<std::string_view>& args);
int DumpCapabilitiesCommand();
int RunExperimentsCommand(const std::vector<std::string_view>& args);
int BackendExperimentsCommand(const std::vector<std::string_view>& args);
int GpuMemPressureCommand(const std::vector<std::string_view>& args);
int MaterialCoverageCommand(const std::vector<std::string_view>& args);
int ShaderMatrixCommand(const std::vector<std::string_view>& args);
int ReleaseCheckCommand(const std::vector<std::string_view>& args);
int ThreadSweepCommand(const std::vector<std::string_view>& args);
int SimdSweepCommand(const std::vector<std::string_view>& args);
int TileSweepCommand(const std::vector<std::string_view>& args);

}  // namespace vkpt::benchmark::ptbench
