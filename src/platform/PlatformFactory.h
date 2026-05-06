#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "platform/Interfaces.h"

namespace vkpt::platform {

enum class HostPlatformKind {
  Unknown,
  Windows,
  Linux,
  MacOS,
  Web,
};

enum class RuntimePlatformKind {
  Invalid,
  Auto,
  Raw,
  Qt,
  Headless,
};

struct RuntimePlatformSupport {
  RuntimePlatformKind kind = RuntimePlatformKind::Invalid;
  const char* name = "invalid";
  bool built = false;
  bool available = false;
  bool stub = false;
  const char* implementation = "none";
  const char* unavailable_reason = "";
};

HostPlatformKind HostPlatform();
const char* HostPlatformName(HostPlatformKind kind);
RuntimePlatformKind ParseRuntimePlatform(std::string_view name);
const char* RuntimePlatformKindName(RuntimePlatformKind kind);
RuntimePlatformSupport DescribeRuntimePlatform(RuntimePlatformKind kind);
std::vector<RuntimePlatformSupport> DescribeRuntimePlatforms();

// Resolves platform intent from CLI/config state.
RuntimePlatformKind ResolveRuntimePlatform(RuntimePlatformKind requested,
                                          bool wants_window,
                                          bool headless_requested);

bool IsPlatformBuilt(RuntimePlatformKind kind);
bool IsPlatformAvailable(RuntimePlatformKind kind);
std::unique_ptr<IPlatform> CreatePlatform(RuntimePlatformKind kind, std::string_view name);

}  // namespace vkpt::platform
