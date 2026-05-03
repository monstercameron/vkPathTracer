#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
  virtual void publish(std::string_view source, const InputEvent& event) = 0;
  virtual std::size_t consume(std::vector<InputEvent>& out) = 0;
};

class IInput {
 public:
  virtual ~IInput() = default;
  virtual std::size_t consume(std::vector<InputEvent>& out) = 0;
};

class IWindow {
 public:
  virtual ~IWindow() = default;
  virtual bool initialize(std::size_t width, std::size_t height, std::string_view title) = 0;
  virtual bool is_open() const = 0;
  virtual void close() = 0;
  virtual WindowMetrics metrics() const = 0;
  virtual bool poll_events() = 0;
};

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;
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
  virtual vkpt::core::Result<void> set_text(std::string_view text) = 0;
  virtual vkpt::core::Result<std::string> get_text() const = 0;
};

class INativeSurfaceProvider {
 public:
  virtual ~INativeSurfaceProvider() = default;
  virtual void* native_window_handle() const = 0;
  virtual void* native_instance_handle() const = 0;
};

class IPlatform {
 public:
  virtual ~IPlatform() = default;

  virtual vkpt::core::Result<void> initialize() = 0;
  virtual void shutdown() = 0;
  virtual bool is_headless() const = 0;

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

}  // namespace vkpt::platform
