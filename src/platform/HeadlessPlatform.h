#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/Interfaces.h"

namespace vkpt::platform {

class HeadlessWindow final : public IWindow {
 public:
  vkpt::core::Status initialize_status(std::size_t width,
                                       std::size_t height,
                                       std::string_view title) override;
  bool is_open() const override;
  void close() override;
  WindowMetrics metrics() const override;
  vkpt::core::Status poll_events_status() override;

  void set_title(std::string_view title);

 private:
  bool m_open = false;
  WindowMetrics m_metrics{1280, 720, 1.0f};
  std::string m_title;
};

class HeadlessInput final : public IInput {
 public:
  std::size_t consume(std::vector<InputEvent>& out) override;
  vkpt::core::Status set_source_status(std::shared_ptr<IInputSource> source) override;
  void queue(InputEvent event);

 private:
  std::shared_ptr<IInputSource> m_source;
  std::deque<InputEvent> m_queue;
};

class HeadlessEvents final : public IEvents {
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

class HeadlessTimeSource final : public ITimeSource {
 public:
  explicit HeadlessTimeSource(std::uint64_t startMs = 0) : m_startMs(startMs) {}
  std::uint64_t now_ms() const override;

 private:
  std::uint64_t m_startMs = 0;
};

class HeadlessFileSystem final : public IFileSystem {
 public:
  vkpt::core::Result<std::string> read_text_file(std::string_view path) const override;
  bool file_exists(std::string_view path) const override;
};

class HeadlessClipboard final : public IClipboard {
 public:
  vkpt::core::Result<void> set_text(std::string_view text) override;
  vkpt::core::Result<std::string> get_text() const override;

 private:
  std::string m_text;
};

class HeadlessSurfaceProvider final : public INativeSurfaceProvider {
 public:
  void* native_window_handle() const override;
  void* native_instance_handle() const override;
};

class HeadlessPlatform final : public IPlatform {
 public:
  explicit HeadlessPlatform(std::string_view name = "vkpt-headless");

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
  HeadlessWindow m_window;
  HeadlessInput m_input;
  HeadlessEvents m_events;
  HeadlessTimeSource m_time_source;
  HeadlessFileSystem m_file_system;
  HeadlessClipboard m_clipboard;
  HeadlessSurfaceProvider m_surface;
};

}  // namespace vkpt::platform
