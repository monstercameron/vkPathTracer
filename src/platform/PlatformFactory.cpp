#include "platform/PlatformFactory.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

HostPlatformKind HostPlatform() {
#if defined(__EMSCRIPTEN__)
  return HostPlatformKind::Web;
#elif defined(_WIN32)
  return HostPlatformKind::Windows;
#elif defined(__APPLE__)
  return HostPlatformKind::MacOS;
#elif defined(__linux__)
  return HostPlatformKind::Linux;
#else
  return HostPlatformKind::Unknown;
#endif
}

const char* HostPlatformName(HostPlatformKind kind) {
  switch (kind) {
    case HostPlatformKind::Windows: return "windows";
    case HostPlatformKind::Linux: return "linux";
    case HostPlatformKind::MacOS: return "macos";
    case HostPlatformKind::Web: return "web";
    case HostPlatformKind::Unknown:
    default: return "unknown";
  }
}

RuntimePlatformKind ParseRuntimePlatform(std::string_view name) {
  const std::string value = ToLower(name);
  if (value.empty() || value == "auto") {
    return RuntimePlatformKind::Auto;
  }
  if (value == "raw" || value == "desktop" || value == "native" || value == "win32" ||
      value == "x11" || value == "wayland" || value == "linux" ||
      value == "macos" || value == "osx" || value == "cocoa") {
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

RuntimePlatformSupport DescribeRuntimePlatform(RuntimePlatformKind kind) {
  RuntimePlatformSupport support;
  support.kind = kind;
  support.name = RuntimePlatformKindName(kind);
  switch (kind) {
    case RuntimePlatformKind::Invalid:
      support.unavailable_reason = "invalid platform request";
      return support;
    case RuntimePlatformKind::Auto:
      support.built = true;
      support.available = true;
      support.implementation = "resolver";
      return support;
    case RuntimePlatformKind::Headless:
      support.built = true;
      support.available = true;
      support.implementation = "headless";
      return support;
    case RuntimePlatformKind::Raw:
#if defined(PT_ENABLE_RAW_DESKTOP)
      support.built = true;
#if defined(_WIN32)
      support.available = true;
      support.implementation = "win32";
#elif defined(__APPLE__)
      support.stub = true;
      support.implementation = "macos-cocoa-stub";
      support.unavailable_reason =
          "raw macOS/Cocoa windowing is stubbed; use Qt or headless until the native implementation lands";
#elif defined(__linux__)
      support.stub = true;
      support.implementation = "linux-x11-wayland-stub";
      support.unavailable_reason =
          "raw Linux X11/Wayland windowing is stubbed; use Qt or headless until the native implementation lands";
#else
      support.stub = true;
      support.implementation = "native-desktop-stub";
      support.unavailable_reason =
          "raw native desktop windowing is stubbed for this host; use Qt or headless";
#endif
#else
      support.unavailable_reason = "PT_ENABLE_RAW_DESKTOP is disabled";
#endif
      return support;
    case RuntimePlatformKind::Qt:
#if defined(PT_ENABLE_QT)
      support.built = true;
      support.available = true;
      support.implementation = "qt-widgets";
#else
      support.unavailable_reason = "PT_ENABLE_QT is disabled";
#endif
      return support;
    default:
      support.unavailable_reason = "unknown platform";
      return support;
  }
}

std::vector<RuntimePlatformSupport> DescribeRuntimePlatforms() {
  return {
      DescribeRuntimePlatform(RuntimePlatformKind::Headless),
      DescribeRuntimePlatform(RuntimePlatformKind::Raw),
      DescribeRuntimePlatform(RuntimePlatformKind::Qt),
  };
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
  if (!wants_window) {
    return RuntimePlatformKind::Headless;
  }
#if defined(_WIN32)
  if (IsPlatformAvailable(RuntimePlatformKind::Raw)) {
    return RuntimePlatformKind::Raw;
  }
  if (IsPlatformAvailable(RuntimePlatformKind::Qt)) {
    return RuntimePlatformKind::Qt;
  }
#else
  if (IsPlatformAvailable(RuntimePlatformKind::Qt)) {
    return RuntimePlatformKind::Qt;
  }
  if (IsPlatformAvailable(RuntimePlatformKind::Raw)) {
    return RuntimePlatformKind::Raw;
  }
#endif
  return RuntimePlatformKind::Headless;
}

bool IsPlatformBuilt(RuntimePlatformKind kind) {
  return DescribeRuntimePlatform(kind).built;
}

bool IsPlatformAvailable(RuntimePlatformKind kind) {
  return DescribeRuntimePlatform(kind).available;
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
