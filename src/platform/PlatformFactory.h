#pragma once

#include <memory>
#include <string_view>

#include "platform/Interfaces.h"

namespace vkpt::platform {

enum class RuntimePlatformKind {
  Invalid,
  Auto,
  Raw,
  Qt,
  Headless,
};

RuntimePlatformKind ParseRuntimePlatform(std::string_view name);
const char* RuntimePlatformKindName(RuntimePlatformKind kind);

// Resolves platform intent from CLI/config state.
RuntimePlatformKind ResolveRuntimePlatform(RuntimePlatformKind requested,
                                          bool wants_window,
                                          bool headless_requested);

bool IsPlatformBuilt(RuntimePlatformKind kind);
std::unique_ptr<IPlatform> CreatePlatform(RuntimePlatformKind kind, std::string_view name);

}  // namespace vkpt::platform
