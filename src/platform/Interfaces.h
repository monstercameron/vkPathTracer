#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/health/Health.h"
#include "core/Types.h"

namespace vkpt::platform {

struct WindowMetrics {
  int width = 0;
  int height = 0;
  float dpiScale = 1.0f;
};

enum class InputEventType {
  None,
  KeyDown,
  KeyUp,
  MouseMove,
  MouseButtonDown,
  MouseButtonUp,
  MouseWheel,
  WindowResize,
  MenuCommand,
  FocusLost,
  FocusGained,
  CloseRequested,
};

struct InputEvent {
  InputEventType type = InputEventType::None;
  int code = 0;
  float x = 0.0f;
  float y = 0.0f;
  std::uint32_t device_id = 0u;
  std::int32_t raw_code = 0;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
  float delta_z = 0.0f;
};

struct UiStatus {
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  std::string backend = "unknown";
  double repaint_hz = 0.0;
  std::uint64_t last_tick_ns = 0;
  std::uint64_t ticks_total = 0;
  std::uint64_t last_event_ns = 0;
  std::size_t event_queue_depth = 0;
  std::uint64_t current_flow_id = 0;
  std::uint64_t errors_total = 0;
  std::string last_error;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  std::uint64_t determinism_frame_index = 0u;
  std::string determinism_scenario_id;

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                              determinism_base_seed,
                                              determinism_frame_index,
                                              determinism_scenario_id);
  }

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
  }
};

struct EventQueueStatus {
  std::size_t depth = 0;
  std::size_t high_water_mark = 0;
  std::uint64_t dropped_total = 0;
};

struct PlatformStatus {
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  std::uint64_t last_tick_ns = 0;
  std::uint64_t ticks_total = 0;
  std::uint64_t errors_total = 0;
  bool initialized = false;
  bool headless = false;
  bool window_open = false;
  bool input_focused = false;
  std::string vsync_mode = "unknown";
  std::string last_error;
  EventQueueStatus events{};
  std::uint64_t current_flow_id = 0;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  std::uint64_t determinism_frame_index = 0u;
  std::string determinism_scenario_id;

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                              determinism_base_seed,
                                              determinism_frame_index,
                                              determinism_scenario_id);
  }

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
  }
};

using IPlatformStatus = PlatformStatus;

struct InputEventNormalizer final {
  static InputEvent key(int raw_key, bool pressed) {
    return InputEvent{pressed ? InputEventType::KeyDown : InputEventType::KeyUp, raw_key, 0.0f, 0.0f, 0u, raw_key};
  }

  static InputEvent mouse_move(float x, float y, float dx = 0.0f, float dy = 0.0f) {
    InputEvent event{InputEventType::MouseMove, 0, x, y};
    event.delta_x = dx;
    event.delta_y = dy;
    return event;
  }

  static InputEvent mouse_button(int button, bool pressed, float x = 0.0f, float y = 0.0f) {
    return InputEvent{pressed ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp, button, x, y, 0u, button};
  }

  static InputEvent mouse_wheel(float delta, float x = 0.0f, float y = 0.0f) {
    InputEvent event{InputEventType::MouseWheel, 0, x, y};
    event.delta_z = delta;
    return event;
  }

  static InputEvent resize(std::uint32_t width, std::uint32_t height) {
    InputEvent event{InputEventType::WindowResize, 0,
                     static_cast<float>(width), static_cast<float>(height),
                     0u, 0};
    return event;
  }

  static InputEvent focus(bool focused) {
    return InputEvent{focused ? InputEventType::FocusGained : InputEventType::FocusLost, 0, 0.0f, 0.0f};
  }

  static InputEvent close() {
    return InputEvent{InputEventType::CloseRequested, 0, 0.0f, 0.0f};
  }

  static InputEvent menu_command(std::uint32_t command_id) {
    return InputEvent{InputEventType::MenuCommand, 0, 0.0f, 0.0f, 0u, static_cast<std::int32_t>(command_id)};
  }
};

class IEvents {
 public:
  virtual ~IEvents() = default;

  // State contract:
  // state\method      publish  consume  drain   status
  // Ready             ok       ok       ok      ok
  // ShuttingDown      noop     ok       ok      ok
  // Failed            error    ok       ok      ok
  //
  // consume() is non-destructive so overlays, script input, and recorders can
  // co-exist. drain() is the explicit destructive consumer operation.
  virtual vkpt::core::Status publish_status(std::string_view source,
                                            const InputEvent& event) = 0;
  void publish(std::string_view source, const InputEvent& event) {
    (void)publish_status(source, event);
  }
  virtual std::size_t consume(std::vector<InputEvent>& out) const = 0;
  virtual std::size_t drain(std::vector<InputEvent>& out) = 0;
  virtual EventQueueStatus status() const = 0;
};

std::string_view InputEventTypeName(InputEventType type) noexcept;
void SetUiTelemetryBackend(std::string_view backend);
void RecordUiInputEvent(std::string_view source,
                        const InputEvent& event,
                        std::size_t queue_depth,
                        std::uint64_t processing_us);
void RecordUiEventQueueDepth(std::size_t queue_depth);
void RecordUiRepaint(std::uint64_t repaint_us, double repaint_hz, double frame_age_ms);
void RecordUiAnomaly(std::string_view operation,
                     std::string_view reason,
                     std::uint64_t flow_id = 0);
void SetUiDeterminismContext(const vkpt::core::DeterminismContext& context);
vkpt::core::DeterminismContext GetUiDeterminismContext();
UiStatus GetUiStatus();
void ResetUiTelemetryForTest();

class IInputSource {
 public:
  virtual ~IInputSource() = default;

  // State contract:
  // state\method      poll
  // Ready             ok
  // Degraded          ok (may return fewer events)
  // Failed            error-as-empty
  // ShuttingDown      noop
  virtual std::size_t poll(std::vector<InputEvent>& out) = 0;
};

class IInput {
 public:
  virtual ~IInput() = default;

  // State contract:
  // state\method      consume  set_source_status
  // Ready             ok       ok
  // Degraded          ok       ok
  // Failed            ok       error
  // ShuttingDown      noop     error
  virtual std::size_t consume(std::vector<InputEvent>& out) = 0;
  virtual vkpt::core::Status set_source_status(
      std::shared_ptr<IInputSource> source) = 0;
  void set_source(std::shared_ptr<IInputSource> source) {
    (void)set_source_status(std::move(source));
  }
};

class IWindow {
 public:
  virtual ~IWindow() = default;

  // State contract:
  // state\method      initialize_status  is_open  close  metrics  poll_events_status
  // Uninitialized     ->Ready            ok       noop   ok       noop
  // Ready             ok                 ok       ->Closed ok      ok
  // Degraded          ok                 ok       ->Closed ok      ok
  // Failed            error              ok       noop   ok       error
  // ShuttingDown      Busy               ok       noop   ok       noop
  virtual vkpt::core::Status initialize_status(std::size_t width,
                                               std::size_t height,
                                               std::string_view title) = 0;
  bool initialize(std::size_t width, std::size_t height, std::string_view title) {
    return initialize_status(width, height, title).is_ok();
  }
  virtual bool is_open() const = 0;
  virtual void close() = 0;
  virtual WindowMetrics metrics() const = 0;
  virtual vkpt::core::Status poll_events_status() = 0;
  bool poll_events() { return poll_events_status().is_ok(); }
};

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;

  // State contract:
  // state\method      read_text_file  file_exists
  // Ready             ok/error        ok
  // Degraded          ok/error        ok
  // Failed            error           false
  // ShuttingDown      error           false
  virtual vkpt::core::Result<std::string> read_text_file(std::string_view path) const = 0;
  virtual bool file_exists(std::string_view path) const = 0;
};

class ITimeSource {
 public:
  virtual ~ITimeSource() = default;
  virtual std::uint64_t now_ms() const = 0;
};

class IClipboard {
 public:
  virtual ~IClipboard() = default;

  // State contract:
  // state\method      set_text  get_text
  // Ready             ok/error  ok/error
  // Degraded          ok/error  ok/error
  // Failed            error     error
  // ShuttingDown      error     error
  virtual vkpt::core::Result<void> set_text(std::string_view text) = 0;
  virtual vkpt::core::Result<std::string> get_text() const = 0;
};

class INativeSurfaceProvider {
 public:
  virtual ~INativeSurfaceProvider() = default;

  // State contract:
  // state\method      native_window_handle  native_instance_handle
  // Uninitialized     nullptr               nullptr
  // Ready             ok/nullptr            ok/nullptr
  // Failed            nullptr               nullptr
  // ShuttingDown      nullptr               nullptr
  virtual void* native_window_handle() const = 0;
  virtual void* native_instance_handle() const = 0;
};

class IPlatform {
 public:
  virtual ~IPlatform() = default;

  // State contract:
  // state\method      initialize_status  shutdown_status  status  set_determinism
  // Uninitialized     ->Ready            noop             ok      ok
  // Initializing       Busy              ->ShuttingDown   ok      ok
  // Ready              ok                ->ShuttingDown   ok      ok
  // Degraded           ok                ->ShuttingDown   ok      ok
  // Failed             error             ->ShuttingDown   ok      ok
  // ShuttingDown       Busy              noop             ok      ok
  //
  // initialize_status()/shutdown_status() are the contract methods. The
  // initialize()/shutdown() wrappers preserve older call sites while still
  // routing through Status.
  virtual vkpt::core::Status initialize_status() = 0;
  virtual vkpt::core::Status shutdown_status() = 0;
  virtual void set_determinism(const vkpt::core::DeterminismContext& context) = 0;
  virtual vkpt::core::DeterminismContext determinism_context() const = 0;

  vkpt::core::Result<void> initialize() {
    return vkpt::core::ResultFromStatus(initialize_status());
  }
  void shutdown() {
    (void)shutdown_status();
  }
  virtual bool is_headless() const = 0;
  virtual PlatformStatus status() const = 0;

  virtual IWindow* window() = 0;
  virtual const IWindow* window() const = 0;
  virtual IInput* input() = 0;
  virtual const IInput* input() const = 0;
  virtual IEvents* events() = 0;
  virtual const IEvents* events() const = 0;
  virtual IFileSystem* file_system() = 0;
  virtual const IFileSystem* file_system() const = 0;
  virtual ITimeSource* time_source() = 0;
  virtual const ITimeSource* time_source() const = 0;
  virtual IClipboard* clipboard() = 0;
  virtual const IClipboard* clipboard() const = 0;
  virtual INativeSurfaceProvider* native_surface() = 0;
  virtual const INativeSurfaceProvider* native_surface() const = 0;
};

inline vkpt::core::health::Report EvaluateUiHealth(const UiStatus& status) {
  if (status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Failed ||
      !status.last_error.empty()) {
    return {vkpt::core::health::Status::Failed,
            status.last_error.empty() ? "ui failed" : status.last_error};
  }
  if (status.backend == "unknown") {
    return {vkpt::core::health::Status::Degraded, "ui backend unknown"};
  }
  return {vkpt::core::health::Status::Ok, "ok"};
}

template <typename StatusFn>
std::shared_ptr<vkpt::core::health::IHealthProbe> CreateUiHealthProbe(
    StatusFn status_fn) {
  class UiHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit UiHealthProbe(StatusFn fn) : m_statusFn(std::move(fn)) {}

    std::string name() const override { return "app/ui"; }

    vkpt::core::health::Report check() override {
      return EvaluateUiHealth(m_statusFn());
    }

   private:
    StatusFn m_statusFn;
  };

  return std::make_shared<UiHealthProbe>(std::move(status_fn));
}

inline vkpt::core::health::Report EvaluatePlatformHealth(
    const PlatformStatus& status) {
  if (!status.last_error.empty()) {
    return {vkpt::core::health::Status::Failed, status.last_error};
  }
  if (!status.initialized || !status.window_open) {
    return {vkpt::core::health::Status::Degraded,
            "platform is not initialized or window is closed"};
  }
  return {vkpt::core::health::Status::Ok, "ok"};
}

template <typename StatusFn>
std::shared_ptr<vkpt::core::health::IHealthProbe> CreatePlatformHealthProbe(
    StatusFn status_fn) {
  class PlatformHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit PlatformHealthProbe(StatusFn fn) : m_statusFn(std::move(fn)) {}

    std::string name() const override { return "platform"; }

    vkpt::core::health::Report check() override {
      return EvaluatePlatformHealth(m_statusFn());
    }

   private:
    StatusFn m_statusFn;
  };

  return std::make_shared<PlatformHealthProbe>(std::move(status_fn));
}

}  // namespace vkpt::platform
