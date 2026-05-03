#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "build_info.generated.h"

namespace {

void PrintUsage() {
  std::cout << "ptapp [--version] [--headless] [--scene <path>] [--backend <name>] [--log-level <level>] [--json]\n";
  std::cout << "  --version         Print build metadata and exit\n";
  std::cout << "  --headless        Initialize headless mode\n";
  std::cout << "  --scene <path>    Set startup scene\n";
  std::cout << "  --backend <name>  Select backend\n";
  std::cout << "  --log-level <n>   Select log level\n";
  std::cout << "  --crash-test      Simulate a recoverable crash path\n";
  std::cout << "  --json            Emit JSON for --version\n";
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
  std::cout << "  \"app\": \"ptapp\"," << '\n';
  std::cout << "  \"version\": \"" << vkpt::build::kProjectVersion << "\"," << '\n';
  std::cout << "  \"git_hash\": \"" << vkpt::build::kGitHash << "\"," << '\n';
  std::cout << "  \"build_date\": \"" << vkpt::build::kBuildDate << "\"," << '\n';
  std::cout << "  \"compiler\": \"" << vkpt::build::kCompilerName << ' ' << vkpt::build::kCompilerVersion << "\"," << '\n';
  std::cout << "  \"cpp_standard\": \"" << vkpt::build::kCxxStandard << "\"," << '\n';
  std::cout << "  \"target\": \"" << vkpt::build::kTargetOs << '/' << vkpt::build::kTargetArch << "\"," << '\n';
  std::cout << "  \"build_type\": \"" << vkpt::build::kBuildType << "\"," << '\n';
  std::cout << "  \"sanitizers_enabled\": " << (vkpt::build::kSanitizersEnabled ? "true" : "false") << "," << '\n';
  std::cout << "  \"sanitizer_flavor\": \"" << vkpt::build::kSanitizerFlavor << "\"," << '\n';
  std::cout << "  \"strict_determinism\": " << (vkpt::build::kStrictDeterminism ? "true" : "false") << "," << '\n';
  std::cout << "  \"simd_compile_options\": \"" << vkpt::build::kSimdCompileOptions << "\"," << '\n';
  std::cout << "  \"backend_compile_options\": \"" << vkpt::build::kBackendCompileOptions << "\"," << '\n';
  std::cout << "  \"enabled_features\": [\"" << vkpt::build::kEnabledFeatureFlags << "\"]," << '\n';
  std::cout << "  \"disabled_features\": [\"" << vkpt::build::kDisabledFeatureFlags << "\"]" << '\n';
  std::cout << "}\n";
}

}

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);

  bool showVersion = false;
  bool versionJson = false;
  bool headless = false;
  std::string_view scene;
  std::string_view backend;
  std::string_view logLevel = "info";

  for (size_t i = 1; i < args.size(); ++i) {
    auto token = args[i];
    if (token == "--version") {
      showVersion = true;
    } else if (token == "--json") {
      versionJson = true;
    } else if (token == "--headless") {
      headless = true;
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing value for --scene\n";
        return 1;
      }
      scene = args[++i];
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
    } else if (token == "--crash-test") {
      return 42;
    } else if (token == "--help" || token == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      PrintUsage();
      return 1;
    }
  }

  if (showVersion) {
    if (versionJson) {
      PrintVersionJson();
    } else {
      PrintVersionText();
    }
    return 0;
  }

  std::cout << "ptapp started\n";
  std::cout << "mode: " << (headless ? "headless" : "demo") << "\n";
  std::cout << "backend: " << (backend.empty() ? "auto" : backend) << "\n";
  if (!scene.empty()) {
    std::cout << "scene: " << scene << "\n";
  }
  std::cout << "log level: " << logLevel << "\n";
  return 0;
}
