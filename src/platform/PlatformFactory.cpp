#include "platform/PlatformFactory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "platform/HeadlessPlatform.h"

#if defined(PT_ENABLE_RAW_DESKTOP)
#include "platform/DesktopPlatform.h"
#endif

#if defined(PT_ENABLE_QT)
#include "platform/qt/QtPlatform.h"
#endif

namespace vkpt::platform {

namespace {

struct UiTelemetryState {
  std::atomic<std::uint64_t> event_seq{0};
  std::atomic<std::uint64_t> repaint_seq{0};
  std::atomic<std::uint64_t> last_event_ns{0};
  std::atomic<std::uint64_t> queue_depth{0};
  std::atomic<std::uint64_t> repaint_hz_milli{0};
  std::atomic<std::uint64_t> errors_total{0};
  std::mutex mutex;
  std::string backend = "unknown";
  std::string last_error;
  vkpt::core::DeterminismContext determinism;
};

UiTelemetryState& UiTelemetry() {
  static UiTelemetryState state;
  return state;
}

std::uint64_t UiNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

void RecordUiAnomalyInternal(std::string_view operation,
                             std::string_view reason,
                             std::uint64_t flow_id) {
  auto& telemetry = UiTelemetry();
  telemetry.errors_total.fetch_add(1u, std::memory_order_relaxed);
  {
    std::scoped_lock lock(telemetry.mutex);
    telemetry.last_error = std::string(reason);
  }
  VKP_LOG(Warn,
          "ui",
          "operation_failed",
          "operation",
          operation,
          "reason",
          reason,
          "flow_id",
          flow_id);
}

void EmitPlatformSelected(RuntimePlatformKind requested,
                          RuntimePlatformKind selected,
                          bool wants_window,
                          bool headless_requested,
                          std::string_view reason) {
  const auto support = DescribeRuntimePlatform(selected);
  SetUiTelemetryBackend(RuntimePlatformKindName(selected));
  VKP_LOG(Info,
          "platform",
          "selected",
          "backend",
          RuntimePlatformKindName(selected),
          "requested",
          RuntimePlatformKindName(requested),
          "implementation",
          support.implementation,
          "host",
          HostPlatformName(HostPlatform()),
          "wants_window",
          wants_window,
          "headless",
          headless_requested,
          "reason",
          reason,
          "flow_id",
          std::uint64_t{0});
}

void EmitPlatformFallback(RuntimePlatformKind requested,
                          RuntimePlatformKind selected,
                          std::string_view reason) {
  RecordUiAnomalyInternal("select_backend", reason, 0u);
  VKP_LOG(Warn,
          "platform",
          "fallback",
          "requested",
          RuntimePlatformKindName(requested),
          "selected",
          RuntimePlatformKindName(selected),
          "reason",
          reason,
          "flow_id",
          std::uint64_t{0});
}

}  // namespace

std::string_view InputEventTypeName(InputEventType type) noexcept {
  switch (type) {
    case InputEventType::KeyDown: return "key_down";
    case InputEventType::KeyUp: return "key_up";
    case InputEventType::MouseMove: return "mouse_move";
    case InputEventType::MouseButtonDown: return "mouse_button_down";
    case InputEventType::MouseButtonUp: return "mouse_button_up";
    case InputEventType::MouseWheel: return "mouse_wheel";
    case InputEventType::WindowResize: return "window_resize";
    case InputEventType::MenuCommand: return "menu_command";
    case InputEventType::FocusLost: return "focus_lost";
    case InputEventType::FocusGained: return "focus_gained";
    case InputEventType::CloseRequested: return "close_requested";
    case InputEventType::None:
    default: return "none";
  }
}

void SetUiTelemetryBackend(std::string_view backend) {
  auto& telemetry = UiTelemetry();
  const std::string backend_name = backend.empty() ? "unknown" : std::string(backend);
  std::scoped_lock lock(telemetry.mutex);
  telemetry.backend = backend_name;
  telemetry.last_error.clear();
  VKP_LIFECYCLE_CONFIG("ui",
                       "backend",
                       telemetry.backend,
                       "flow_id",
                       std::uint64_t{0});
  VKP_LIFECYCLE_STARTED("ui",
                        "backend",
                        telemetry.backend,
                        "flow_id",
                        std::uint64_t{0});
}

void RecordUiInputEvent(std::string_view source,
                        const InputEvent& event,
                        std::size_t queue_depth,
                        std::uint64_t processing_us) {
  auto& telemetry = UiTelemetry();
  const std::string_view source_name = source.empty() ? std::string_view("unknown") : source;
  const auto seq = telemetry.event_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
  telemetry.last_event_ns.store(UiNowNs(), std::memory_order_relaxed);
  telemetry.queue_depth.store(static_cast<std::uint64_t>(queue_depth), std::memory_order_relaxed);
  VKP_METRIC_INC("vkp.ui.input_events_total");
  VKP_METRIC_SET("vkp.ui.event_queue_depth", static_cast<double>(queue_depth));
  VKP_METRIC_OBSERVE("vkp.ui.input_event_processing_us", processing_us);
  VKP_LOG_SAMPLED(1000000000ull,
                  Debug,
                  "ui",
                  "input_event",
                  "event_type",
                  InputEventTypeName(event.type),
                  "seq",
                  seq,
                  "flow_id",
                  seq,
                  "processing_us",
                  processing_us,
                  "source",
                  source_name);
}

void RecordUiEventQueueDepth(std::size_t queue_depth) {
  auto& telemetry = UiTelemetry();
  telemetry.queue_depth.store(static_cast<std::uint64_t>(queue_depth), std::memory_order_relaxed);
  VKP_METRIC_SET("vkp.ui.event_queue_depth", static_cast<double>(queue_depth));
}

void RecordUiRepaint(std::uint64_t repaint_us, double repaint_hz, double frame_age_ms) {
  auto& telemetry = UiTelemetry();
  telemetry.repaint_seq.fetch_add(1u, std::memory_order_relaxed);
  telemetry.repaint_hz_milli.store(static_cast<std::uint64_t>(std::max(0.0, repaint_hz) * 1000.0),
                                   std::memory_order_relaxed);
  VKP_METRIC_SET("vkp.ui.repaint_hz", repaint_hz);
  VKP_METRIC_OBSERVE("vkp.ui.repaint_us", repaint_us);
  VKP_METRIC_OBSERVE("vkp.ui.paint_us", repaint_us);
  VKP_METRIC_SET("vkp.ui.frame_age_ms", frame_age_ms);
}

void RecordUiAnomaly(std::string_view operation,
                     std::string_view reason,
                     std::uint64_t flow_id) {
  RecordUiAnomalyInternal(operation, reason, flow_id);
}

void SetUiDeterminismContext(const vkpt::core::DeterminismContext& context) {
  auto& telemetry = UiTelemetry();
  vkpt::core::DeterminismContext previous;
  {
    std::scoped_lock lock(telemetry.mutex);
    previous = telemetry.determinism;
    telemetry.determinism = context;
  }
  vkpt::core::EmitDeterminismChangedIfNeeded("ui", previous, context);
}

vkpt::core::DeterminismContext GetUiDeterminismContext() {
  auto& telemetry = UiTelemetry();
  std::scoped_lock lock(telemetry.mutex);
  return telemetry.determinism;
}

UiStatus GetUiStatus() {
  auto& telemetry = UiTelemetry();
  UiStatus status;
  {
    std::scoped_lock lock(telemetry.mutex);
    status.backend = telemetry.backend;
    status.last_error = telemetry.last_error;
    status.set_determinism(telemetry.determinism);
  }
  status.lifecycle = status.last_error.empty()
      ? (status.backend == "unknown"
             ? vkpt::core::contracts::ComponentLifecycle::Uninitialized
             : vkpt::core::contracts::ComponentLifecycle::Ready)
      : vkpt::core::contracts::ComponentLifecycle::Failed;
  status.repaint_hz =
      static_cast<double>(telemetry.repaint_hz_milli.load(std::memory_order_relaxed)) / 1000.0;
  status.last_event_ns = telemetry.last_event_ns.load(std::memory_order_relaxed);
  status.last_tick_ns = status.last_event_ns;
  status.event_queue_depth =
      static_cast<std::size_t>(telemetry.queue_depth.load(std::memory_order_relaxed));
  const auto event_seq = telemetry.event_seq.load(std::memory_order_relaxed);
  const auto repaint_seq = telemetry.repaint_seq.load(std::memory_order_relaxed);
  status.ticks_total = std::max(event_seq, repaint_seq);
  status.current_flow_id = std::max(event_seq, repaint_seq);
  status.errors_total = telemetry.errors_total.load(std::memory_order_relaxed);
  return status;
}

void ResetUiTelemetryForTest() {
  auto& telemetry = UiTelemetry();
  telemetry.event_seq.store(0u, std::memory_order_relaxed);
  telemetry.repaint_seq.store(0u, std::memory_order_relaxed);
  telemetry.last_event_ns.store(0u, std::memory_order_relaxed);
  telemetry.queue_depth.store(0u, std::memory_order_relaxed);
  telemetry.repaint_hz_milli.store(0u, std::memory_order_relaxed);
  telemetry.errors_total.store(0u, std::memory_order_relaxed);
  {
    std::scoped_lock lock(telemetry.mutex);
    telemetry.backend = "unknown";
    telemetry.last_error.clear();
    telemetry.determinism = {};
  }
  vkpt::core::metrics::MetricsRegistry::instance().reset("vkp.ui.");
}

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
    EmitPlatformSelected(requested,
                         requested,
                         wants_window,
                         headless_requested,
                         "explicit");
    return requested;
  }
  if (headless_requested) {
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Headless,
                         wants_window,
                         headless_requested,
                         "headless_requested");
    return RuntimePlatformKind::Headless;
  }
  if (!wants_window) {
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Headless,
                         wants_window,
                         headless_requested,
                         "no_window_requested");
    return RuntimePlatformKind::Headless;
  }
#if defined(_WIN32)
  if (IsPlatformAvailable(RuntimePlatformKind::Raw)) {
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Raw,
                         wants_window,
                         headless_requested,
                         "auto_preferred");
    return RuntimePlatformKind::Raw;
  }
  if (IsPlatformAvailable(RuntimePlatformKind::Qt)) {
    EmitPlatformFallback(requested, RuntimePlatformKind::Qt, "raw_unavailable");
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Qt,
                         wants_window,
                         headless_requested,
                         "auto_fallback");
    return RuntimePlatformKind::Qt;
  }
#else
  if (IsPlatformAvailable(RuntimePlatformKind::Qt)) {
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Qt,
                         wants_window,
                         headless_requested,
                         "auto_preferred");
    return RuntimePlatformKind::Qt;
  }
  if (IsPlatformAvailable(RuntimePlatformKind::Raw)) {
    EmitPlatformFallback(requested, RuntimePlatformKind::Raw, "qt_unavailable");
    EmitPlatformSelected(requested,
                         RuntimePlatformKind::Raw,
                         wants_window,
                         headless_requested,
                         "auto_fallback");
    return RuntimePlatformKind::Raw;
  }
#endif
  EmitPlatformFallback(requested, RuntimePlatformKind::Headless, "window_platform_unavailable");
  EmitPlatformSelected(requested,
                       RuntimePlatformKind::Headless,
                       wants_window,
                       headless_requested,
                       "auto_fallback");
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
