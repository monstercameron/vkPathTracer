#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Interfaces.h"

namespace vkpt::platform {

class DesktopWindow final : public IWindow {
 public:
  vkpt::core::Status initialize_status(std::size_t width,
                                       std::size_t height,
                                       std::string_view title) override;
  bool is_open() const override;
  void close() override;
  WindowMetrics metrics() const override;
  vkpt::core::Status poll_events_status() override;
  vkpt::core::Status resize_status(std::size_t width, std::size_t height);
  bool resize(std::size_t width, std::size_t height) {
    return resize_status(width, height).is_ok();
  }
  void* native_handle() const;

  void set_title(std::string_view title);
  void set_overlay_text(std::string_view text);
  void set_framebuffer_rgba(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t height);
  void clear_framebuffer();
  void on_native_resize(std::size_t width, std::size_t height);
  const std::string& overlay_text() const { return m_overlayText; }
  bool focused() const { return m_focused; }
  const std::vector<std::uint8_t>& framebuffer_bgra() const { return m_framebufferBgra; }
  std::size_t framebuffer_width() const { return m_framebufferWidth; }
  std::size_t framebuffer_height() const { return m_framebufferHeight; }
  void emit_focus_change(bool focused);
  void emit_close_requested();
  void emit_menu_command(std::uint32_t command_id);
  void emit_key(std::int32_t key, bool pressed);
  void emit_mouse_move(int x, int y);
  void emit_mouse_button(std::int32_t button, bool pressed, int x, int y);
  void emit_mouse_wheel(float delta, int x, int y);
  std::vector<InputEvent> drain_events();
  void mark_closed();

 private:
  bool m_open = false;
  bool m_focused = true;
  WindowMetrics m_metrics{1280, 720, 1.0f};
  std::string m_title;
  std::string m_overlayText;
  std::vector<std::uint8_t> m_framebufferBgra;
  std::size_t m_framebufferWidth = 0;
  std::size_t m_framebufferHeight = 0;
  void* m_hwnd = nullptr;
  std::deque<InputEvent> m_events;
  int m_lastMouseX = 0;
  int m_lastMouseY = 0;
};

class DesktopInput final : public IInput {
 public:
  std::size_t consume(std::vector<InputEvent>& out) override;
  vkpt::core::Status set_source_status(std::shared_ptr<IInputSource> source) override;
  void queue(InputEvent event);
  void queue_normalized(InputEvent event);
  void emit_key(std::int32_t key, bool pressed);
  void emit_mouse_move(float x, float y, float dx, float dy);
  void emit_mouse_button(std::int32_t button, bool pressed, float x, float y);
  void emit_mouse_wheel(float delta, float x, float y);
  void emit_touch(std::int32_t touch_id, std::int32_t phase, float x, float y);

 private:
  std::shared_ptr<IInputSource> m_source;
  std::deque<InputEvent> m_queue;
};

class DesktopEvents final : public IEvents {
 public:
  vkpt::core::Status publish_status(std::string_view source,
                                    const InputEvent& event) override;
  std::size_t consume(std::vector<InputEvent>& out) const override;
  std::size_t drain(std::vector<InputEvent>& out) override;
  EventQueueStatus status() const override;

 private:
  std::deque<InputEvent> m_events;
  std::size_t m_highWaterMark = 0u;
  std::uint64_t m_droppedTotal = 0u;
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
  void set_handles(void* window_handle, void* instance_handle);

 private:
  void* m_windowHandle = nullptr;
  void* m_instanceHandle = nullptr;
};

class DesktopPlatform final : public IPlatform {
 public:
  explicit DesktopPlatform(std::string_view name = "vkpt-desktop");

  vkpt::core::Status initialize_status() override;
  vkpt::core::Status shutdown_status() override;
  void set_determinism(const vkpt::core::DeterminismContext& context) override;
  vkpt::core::DeterminismContext determinism_context() const override;
  bool is_headless() const override;
  PlatformStatus status() const override;

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
  std::string m_lastError;
  vkpt::core::DeterminismContext m_determinism;
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
