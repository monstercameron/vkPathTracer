#include "benchmark/BenchmarkCli.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/BenchmarkRuntime.h"
#include "core/ExecutionTrace.h"

namespace vkpt::benchmark::ptbench {

int RunBenchmarkCli(int argc, char** argv) {
  using Path = std::filesystem::path;

  if (argc < 2) {
    const Path pixAutorunConfig =
        (argc > 0 && argv[0] != nullptr) ? Path(argv[0]).parent_path() / "ptbench_pix_autorun.cfg"
                                         : Path("ptbench_pix_autorun.cfg");
    std::error_code ec;
    if (ReadProcessEnvBool("PTBENCH_PIX_AUTORUN") || (std::filesystem::exists(pixAutorunConfig, ec) && !ec)) {
      const auto ownedArgs = BuildPixAutorunArgs(pixAutorunConfig);
      const auto args = MakeArgViews(ownedArgs);
      return RunCommand(args);
    }
    PrintHelp();
    return 1;
  }

  const std::vector<std::string_view> args(argv, argv + argc);
  const auto command = args[1];
  vkpt::core::TraceExecution("ptbench_command", {
    {"command", std::string(command)},
    {"argc", std::to_string(argc)}
  });

  if (command == "--help" || command == "-h") {
    PrintHelp();
    return 0;
  }
  if (command == "run") {
    return RunCommand(args);
  }
  if (command == "echo-desc") {
    return EchoDescCommand(args);
  }
  if (command == "list-scenes") {
    return ListScenesCommand();
  }
  if (command == "list-backends") {
    return ListBackendsCommand();
  }
  if (command == "list-renderer-paths") {
    return ListRendererPathsCommand(args);
  }
  if (command == "validate-scene") {
    return ValidateSceneCommand(args);
  }
  if (command == "validate-artifacts") {
    return ValidateArtifactsCommand(args);
  }
  if (command == "compare") {
    return CompareCommand(args);
  }
  if (command == "dump-capabilities") {
    return DumpCapabilitiesCommand();
  }
  if (command == "run-experiments") {
    return RunExperimentsCommand(args);
  }
  if (command == "backend-experiments") {
    return BackendExperimentsCommand(args);
  }
  if (command == "gpu-mem-pressure") {
    return GpuMemPressureCommand(args);
  }
  if (command == "material-coverage") {
    return MaterialCoverageCommand(args);
  }
  if (command == "shader-matrix") {
    return ShaderMatrixCommand(args);
  }
  if (command == "release-check") {
    return ReleaseCheckCommand(args);
  }
  if (command == "thread-sweep") {
    return ThreadSweepCommand(args);
  }
  if (command == "simd-sweep") {
    return SimdSweepCommand(args);
  }
  if (command == "tile-sweep") {
    return TileSweepCommand(args);
  }

  std::cerr << "unknown command: " << command << "\n";
  PrintHelp();
  return 1;
}

}  // namespace vkpt::benchmark::ptbench
