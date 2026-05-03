#pragma once

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
  FocusLost,
  FocusGained,
  CloseRequested,
};

struct InputEvent {
  InputEventType type = InputEventType::None;
  int code = 0;
  float x = 0.0f;
  float y = 0.0f;
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

