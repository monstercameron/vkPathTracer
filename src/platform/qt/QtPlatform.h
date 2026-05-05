#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Interfaces.h"

class QWidget;

namespace vkpt::platform {

class QtViewportWindow;

struct QtFramebufferStats {
  std::uint64_t received = 0;
  std::uint64_t presented = 0;
  std::uint64_t dropped = 0;
  std::uint64_t latestPublishedId = 0;
  std::uint64_t latestPresentedId = 0;
  std::size_t latestPublishedWidth = 0;
  std::size_t latestPublishedHeight = 0;
  std::size_t latestPresentedWidth = 0;
  std::size_t latestPresentedHeight = 0;
};

struct QtSelectionOverlayBox {
  struct Line {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    std::uint8_t r = 255u;
    std::uint8_t g = 255u;
    std::uint8_t b = 255u;
    std::uint8_t a = 255u;
    float width = 1.5f;
  };

  struct Point {
    float x = 0.0f;
    float y = 0.0f;
    float radius = 5.0f;
    std::uint8_t r = 255u;
    std::uint8_t g = 255u;
    std::uint8_t b = 255u;
    std::uint8_t a = 255u;
    std::string label;
  };

  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  std::string label;
  bool primary = false;
  std::vector<Line> lines;
  std::vector<Point> points;
};

struct QtMenuItem {
  std::uint32_t command_id = 0u;
  std::string label;
  bool enabled = true;
  std::vector<QtMenuItem> children;
};

enum class QtDockArea {
  Left,
  Right,
  Bottom,
  Top,
};

enum class QtDockPanelContent {
  Tree,
  Properties,
  Text,
};

struct QtDockRow {
  std::string label;
  std::string value;
  std::vector<QtDockRow> children;
};

struct QtDockProperty {
  std::string group;
  std::string name;
  std::string value;
  std::string unit;
  bool editable = false;
  bool enabled = true;
};

struct QtDockPanel {
  std::string id;
  std::string title;
  QtDockArea area = QtDockArea::Right;
  QtDockPanelContent content = QtDockPanelContent::Tree;
  bool visible = true;
  bool enabled = true;
  bool closable = true;
  bool movable = true;
  bool floatable = true;
  std::vector<std::string> rows;
  std::vector<QtDockRow> tree_rows;
  std::vector<QtDockProperty> properties;
  std::string text;
};

struct QtStatusBarField {
  std::string id;
  std::string text;
  int stretch = 0;
};

struct QtStatusBarText {
  std::string message;
  int timeout_ms = 0;
  std::vector<QtStatusBarField> fields;
};

enum class QtViewportCursor {
  Default,
  Translate,
  Rotate,
  Scale,
};

class QtWindow final : public IWindow {
 public:
  ~QtWindow() override;

  bool initialize(std::size_t width, std::size_t height, std::string_view title) override;
  bool is_open() const override;
  void close() override;
  WindowMetrics metrics() const override;
  bool poll_events() override;
  bool resize(std::size_t width, std::size_t height);
  void set_title(std::string_view title);
  void set_overlay_text(std::string_view text);
  void set_selection_overlay_boxes(const std::vector<QtSelectionOverlayBox>& boxes);
  void set_viewport_cursor(QtViewportCursor cursor);
  void set_menu_bar(const std::vector<QtMenuItem>& menus);
  void set_dock_panels(const std::vector<QtDockPanel>& panels);
  void set_status_bar_text(std::string_view text);
  void set_status_bar_text(const QtStatusBarText& status);

  void set_framebuffer_rgba(const std::vector<std::uint8_t>& rgba,
                            std::size_t width, std::size_t height);
  void clear_framebuffer();
  QtFramebufferStats framebuffer_stats() const;
  void* native_handle() const;
  void on_native_resize(std::size_t width, std::size_t height);
  void emit_focus_change(bool focused);
  void emit_close_requested();
  void emit_key(std::int32_t key, std::int32_t raw_key, bool pressed);
  void emit_mouse_move(int x, int y);
  void emit_mouse_button(std::int32_t button, bool pressed, int x, int y);
  void emit_mouse_wheel(float delta_x, float delta_y, int x, int y);
  void emit_menu_command(std::uint32_t command_id);
  std::vector<InputEvent> drain_events();
  void mark_closed();
  void destroy();

 private:
  friend class QtViewportWindow;
  friend class QtInput;
  friend class QtSurfaceProvider;

  enum class PendingFramebufferKind {
    None,
    Clear,
    Frame,
  };

  struct PendingFramebuffer {
    std::uint64_t id = 0;
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> rgba;

    void reset();
  };

  void queue_event(InputEvent event);
  void update_metrics_from_widget();
  bool request_framebuffer_clear();
  bool enqueue_frame_update_locked(QWidget* widget);
  void deliver_pending_frame_to_widget(QWidget* widget);
  void record_frame_presented(std::uint64_t frameId,
                              std::size_t width,
                              std::size_t height);
  void record_frame_dropped(std::uint64_t frameId);
  void clear_frame_handoff_locked(bool accepting);

  bool m_open = false;
  bool m_closeEventQueued = false;
  bool m_focused = false;
  WindowMetrics m_metrics{1280, 720, 1.0f};
  std::string m_title;
  std::string m_overlayText;
  std::string m_statusBarText;
  std::string m_menuBarSignature;
  QWidget* m_shell = nullptr;
  QWidget* m_widget = nullptr;
  std::deque<InputEvent> m_events;
  int m_lastMouseX = 0;
  int m_lastMouseY = 0;

  mutable std::mutex m_frameMutex;
  bool m_frameAccepting = false;
  bool m_frameUpdateQueued = false;
  PendingFramebufferKind m_pendingFramebufferKind = PendingFramebufferKind::None;
  PendingFramebuffer m_pendingFramebuffer;
  std::uint64_t m_nextFramebufferId = 1;
  QtFramebufferStats m_framebufferStats;
};

class QtInput final : public IInput {
 public:
  std::size_t consume(std::vector<InputEvent>& out) override;
  void set_window(QtWindow* window);

 private:
  QtWindow* m_window = nullptr;
};

class QtEvents final : public IEvents {
 public:
  void publish(std::string_view source, const InputEvent& event) override;
  std::size_t consume(std::vector<InputEvent>& out) override;

 private:
  std::deque<InputEvent> m_events;
};

class QtTimeSource final : public ITimeSource {
 public:
  explicit QtTimeSource(std::uint64_t startMs = 0) : m_startMs(startMs) {}
  std::uint64_t now_ms() const override;

 private:
  std::uint64_t m_startMs = 0;
};

class QtFileSystem final : public IFileSystem {
 public:
  vkpt::core::Result<std::string> read_text_file(std::string_view path) const override;
  bool file_exists(std::string_view path) const override;
};

class QtClipboard final : public IClipboard {
 public:
  vkpt::core::Result<void> set_text(std::string_view text) override;
  vkpt::core::Result<std::string> get_text() const override;

 private:
  std::string m_text;
};

class QtSurfaceProvider final : public INativeSurfaceProvider {
 public:
  void* native_window_handle() const override;
  void* native_instance_handle() const override;
  void set_window(QtWindow* window);

 private:
  QtWindow* m_window = nullptr;
};

class QtPlatform final : public IPlatform {
 public:
  explicit QtPlatform(std::string_view name = "vkpt-qt");

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
  QtWindow m_window;
  QtInput m_input;
  QtEvents m_events;
  QtTimeSource m_time_source;
  QtFileSystem m_file_system;
  QtClipboard m_clipboard;
  QtSurfaceProvider m_surface;
};

}  // namespace vkpt::platform
