#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Interfaces.h"

namespace vkpt::platform {

class DesktopWindow final : public IWindow {
 public:
  bool initialize(std::size_t width, std::size_t height, std::string_view title) override;
  bool is_open() const override;
  void close() override;
  WindowMetrics metrics() const override;
  bool poll_events() override;
  bool resize(std::size_t width, std::size_t height);

  void set_title(std::string_view title);
  void emit_focus_change(bool focused);
  void emit_close_requested();
  std::vector<InputEvent> drain_events();

 private:
  bool m_open = false;
  bool m_focused = true;
  WindowMetrics m_metrics{1280, 720, 1.0f};
  std::string m_title;
  std::deque<InputEvent> m_events;
};

class DesktopInput final : public IInput {
 public:
  std::size_t consume(std::vector<InputEvent>& out) override;
  void queue(InputEvent event);
  void queue_normalized(InputEvent event);
  void emit_key(std::int32_t key, bool pressed);
  void emit_mouse_move(float x, float y, float dx, float dy);
  void emit_mouse_button(std::int32_t button, bool pressed, float x, float y);
  void emit_mouse_wheel(float delta, float x, float y);
  void emit_touch(std::int32_t touch_id, std::int32_t phase, float x, float y);

 private:
  std::deque<InputEvent> m_queue;
};

class DesktopEvents final : public IEvents {
 public:
  void publish(std::string_view source, const InputEvent& event) override;
  std::size_t consume(std::vector<InputEvent>& out) override;

 private:
  std::deque<InputEvent> m_events;
};

class DesktopTimeSource final : public ITimeSource {
 public:
  explicit DesktopTimeSource(std::uint64_t startMs = 0) : m_startMs(startMs) {}
  std::uint64_t now_ms() const override;

 private:
  std::uint64_t m_startMs = 0;
};

class DesktopFileSystem final : public IFileSystem {
 public:
  vkpt::core::Result<std::string> read_text_file(std::string_view path) const override;
  bool file_exists(std::string_view path) const override;
};

class DesktopClipboard final : public IClipboard {
 public:
  vkpt::core::Result<void> set_text(std::string_view text) override;
  vkpt::core::Result<std::string> get_text() const override;

 private:
  std::string m_text;
};

class DesktopSurfaceProvider final : public INativeSurfaceProvider {
 public:
  void* native_window_handle() const override;
  void* native_instance_handle() const override;
};

class DesktopPlatform final : public IPlatform {
 public:
  explicit DesktopPlatform(std::string_view name = "vkpt-desktop");

  vkpt::core::Result<void> initialize() override;
  void shutdown() override;
  bool is_headless() const override;

  IWindow* window() override;
  const IWindow* window() const override;
  IInput* input() override;
  const IInput* input() const override;
  IEvents* events() override;
  const IEvents* events() const override;
  IFileSystem* file_system() override;
  const IFileSystem* file_system() const override;
  ITimeSource* time_source() override;
  const ITimeSource* time_source() const override;
  IClipboard* clipboard() override;
  const IClipboard* clipboard() const override;
  INativeSurfaceProvider* native_surface() override;
  const INativeSurfaceProvider* native_surface() const override;

 private:
  std::string m_name;
  bool m_initialized = false;
  DesktopWindow m_window;
  DesktopInput m_input;
  DesktopEvents m_events;
  DesktopTimeSource m_time_source;
  DesktopFileSystem m_file_system;
  DesktopClipboard m_clipboard;
  DesktopSurfaceProvider m_surface;
};

}  // namespace vkpt::platform
