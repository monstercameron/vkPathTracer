#include "platform/PlatformFactory.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "platform/HeadlessPlatform.h"

#if defined(PT_ENABLE_RAW_DESKTOP)
#include "platform/DesktopPlatform.h"
#endif

#if defined(PT_ENABLE_QT)
#include "platform/qt/QtPlatform.h"
#endif

namespace vkpt::platform {

namespace {

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

RuntimePlatformKind ParseRuntimePlatform(std::string_view name) {
  const std::string value = ToLower(name);
  if (value.empty() || value == "auto") {
    return RuntimePlatformKind::Auto;
  }
  if (value == "raw" || value == "desktop" || value == "native" || value == "win32") {
    return RuntimePlatformKind::Raw;
  }
  if (value == "qt") {
    return RuntimePlatformKind::Qt;
  }
  if (value == "headless") {
    return RuntimePlatformKind::Headless;
  }
  return RuntimePlatformKind::Invalid;
}

const char* RuntimePlatformKindName(RuntimePlatformKind kind) {
  switch (kind) {
    case RuntimePlatformKind::Invalid: return "invalid";
    case RuntimePlatformKind::Auto: return "auto";
    case RuntimePlatformKind::Raw: return "raw";
    case RuntimePlatformKind::Qt: return "qt";
    case RuntimePlatformKind::Headless: return "headless";
    default: return "auto";
  }
}

RuntimePlatformKind ResolveRuntimePlatform(RuntimePlatformKind requested,
                                          bool wants_window,
                                          bool headless_requested) {
  if (requested == RuntimePlatformKind::Invalid) {
    return RuntimePlatformKind::Invalid;
  }
  if (requested != RuntimePlatformKind::Auto) {
    return requested;
  }
  if (headless_requested) {
    return RuntimePlatformKind::Headless;
  }
  return wants_window ? RuntimePlatformKind::Raw : RuntimePlatformKind::Headless;
}

bool IsPlatformBuilt(RuntimePlatformKind kind) {
  switch (kind) {
    case RuntimePlatformKind::Invalid:
      return false;
    case RuntimePlatformKind::Headless:
      return true;
    case RuntimePlatformKind::Raw:
#if defined(PT_ENABLE_RAW_DESKTOP)
      return true;
#else
      return false;
#endif
    case RuntimePlatformKind::Qt:
#if defined(PT_ENABLE_QT)
      return true;
#else
      return false;
#endif
    case RuntimePlatformKind::Auto:
    default:
      return true;
  }
}

std::unique_ptr<IPlatform> CreatePlatform(RuntimePlatformKind kind, std::string_view name) {
  switch (kind) {
    case RuntimePlatformKind::Headless:
      return std::make_unique<HeadlessPlatform>(name);
    case RuntimePlatformKind::Raw:
#if defined(PT_ENABLE_RAW_DESKTOP)
      return std::make_unique<DesktopPlatform>(name);
#else
      return nullptr;
#endif
    case RuntimePlatformKind::Qt:
#if defined(PT_ENABLE_QT)
      return std::make_unique<QtPlatform>(name);
#else
      return nullptr;
#endif
    case RuntimePlatformKind::Auto:
    default:
      return std::make_unique<HeadlessPlatform>(name);
  }
}

}  // namespace vkpt::platform
