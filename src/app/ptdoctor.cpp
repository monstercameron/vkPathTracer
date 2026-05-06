#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::optional<std::string> ReadEnvVar(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out(value, value_size > 0 ? value_size - 1 : 0);
  std::free(value);
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
#else
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

std::filesystem::path ResolvePtappPath(const char* argv0) {
  if (auto overridePath = ReadEnvVar("VKPT_PTAPP_PATH")) {
    return std::filesystem::path(*overridePath);
  }

  std::filesystem::path self = argv0 != nullptr ? std::filesystem::path(argv0)
                                                : std::filesystem::path();
  if (self.empty()) {
    self = std::filesystem::current_path() / "ptdoctor";
  }

  std::error_code ec;
  const auto absoluteSelf = std::filesystem::absolute(self, ec);
  const auto base = ec ? self : absoluteSelf;
  const auto dir = base.has_parent_path() ? base.parent_path()
                                          : std::filesystem::current_path();

#ifdef _WIN32
  auto sibling = dir / "ptapp.exe";
#else
  auto sibling = dir / "ptapp";
#endif
  if (std::filesystem::exists(sibling, ec) && !ec) {
    return sibling;
  }

#ifdef _WIN32
  return "ptapp.exe";
#else
  return "ptapp";
#endif
}

bool IsCrashTestArg(std::string_view arg) {
  return arg == "--crash-test";
}

std::vector<std::string> MapDoctorArguments(int argc, char** argv) {
  std::vector<std::string> mapped;
  mapped.reserve(static_cast<std::size_t>(argc) + 2u);

  if (argc <= 1) {
    mapped.emplace_back("--doctor");
    return mapped;
  }

  bool crashTest = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i])
                                                    : std::string_view();
    if (IsCrashTestArg(arg)) {
      crashTest = true;
    }
    mapped.emplace_back(arg);
  }

  if (crashTest) {
    const bool alreadyHeadless = [&mapped]() {
      for (const auto& arg : mapped) {
        if (arg == "--headless") {
          return true;
        }
      }
      return false;
    }();
    if (!alreadyHeadless) {
      mapped.insert(mapped.begin(), "--headless");
    }
  }

  return mapped;
}

int RunPtapp(const std::filesystem::path& ptapp, const std::vector<std::string>& args) {
  std::vector<std::string> ownedArgs;
  ownedArgs.reserve(args.size() + 1u);
  ownedArgs.push_back(ptapp.string());
  for (const auto& arg : args) {
    ownedArgs.push_back(arg);
  }

  std::vector<char*> childArgv;
  childArgv.reserve(ownedArgs.size() + 1u);
  for (auto& arg : ownedArgs) {
    childArgv.push_back(arg.data());
  }
  childArgv.push_back(nullptr);

#ifdef _WIN32
  const int code = _spawnv(_P_WAIT, ownedArgs.front().c_str(), childArgv.data());
  if (code == -1) {
    std::cerr << "ptdoctor: failed to spawn " << ownedArgs.front()
              << " errno=" << errno << "\n";
    return 127;
  }
  return code;
#else
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "ptdoctor: failed to fork errno=" << errno << "\n";
    return 127;
  }

  if (pid == 0) {
    execv(ownedArgs.front().c_str(), childArgv.data());
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::cerr << "ptdoctor: waitpid failed errno=" << errno << "\n";
    return 127;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 127;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto ptapp = ResolvePtappPath(argc > 0 ? argv[0] : nullptr);
    const auto args = MapDoctorArguments(argc, argv);

    const int code = RunPtapp(ptapp, args);
    if (code != 0) {
      std::cerr << "ptdoctor: delegated command failed via " << ptapp.string()
                << " with exit code " << code << "\n";
    }
    return code;
  } catch (const std::bad_alloc& ex) {
    std::cerr << "ptdoctor: out of memory: " << ex.what() << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "ptdoctor: unhandled exception: " << ex.what() << "\n";
  } catch (...) {
    std::cerr << "ptdoctor: unhandled non-standard exception\n";
  }
  return 127;
}
