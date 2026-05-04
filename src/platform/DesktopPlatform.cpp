#include "platform/DesktopPlatform.h"

#include <cstdint>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstring>
#include "core/Logging.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#endif

namespace vkpt::platform {

#ifdef _WIN32
namespace {

std::wstring ToWide(std::string_view text) {
  const int utf16Len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (utf16Len <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(utf16Len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), utf16Len);
  return out;
}

int ExtractWindowX(LPARAM lparam) {
  return static_cast<int>(static_cast<short>(LOWORD(lparam)));
}

int ExtractWindowY(LPARAM lparam) {
  return static_cast<int>(static_cast<short>(HIWORD(lparam)));
}

LRESULT CALLBACK DesktopWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<DesktopWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self) {
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        auto* brush = CreateSolidBrush(RGB(0, 0, 0));
        if (brush) {
          FillRect(hdc, &clientRect, brush);
          DeleteObject(brush);
        } else {
          FillRect(hdc, &clientRect, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }

        const auto& framebuffer = self->framebuffer_bgra();
        const auto framebufferWidth = self->framebuffer_width();
        const auto framebufferHeight = self->framebuffer_height();
        static std::uint64_t paintCount = 0u;
        ++paintCount;
        if (!framebuffer.empty() && framebufferWidth > 0u && framebufferHeight > 0u) {
          BITMAPINFO bmi{};
          bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
          bmi.bmiHeader.biWidth = static_cast<LONG>(framebufferWidth);
          bmi.bmiHeader.biHeight = -static_cast<LONG>(framebufferHeight);
          bmi.bmiHeader.biPlanes = 1;
          bmi.bmiHeader.biBitCount = 32;
          bmi.bmiHeader.biCompression = BI_RGB;

          void* dibBits = nullptr;
          HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
          if (dib && dibBits) {
            std::memcpy(dibBits, framebuffer.data(), framebuffer.size());
            HDC memDc = CreateCompatibleDC(hdc);
            HGDIOBJ oldBmp = SelectObject(memDc, dib);
            StretchBlt(hdc,
                      clientRect.left,
                      clientRect.top,
                      clientRect.right - clientRect.left,
                      clientRect.bottom - clientRect.top,
                      memDc,
                      0,
                      0,
                      static_cast<int>(framebufferWidth),
                      static_cast<int>(framebufferHeight),
                      SRCCOPY);
            SelectObject(memDc, oldBmp);
            DeleteDC(memDc);
          } else {
            // Fallback for platforms/drivers that fail DIBSection creation.
            StretchDIBits(hdc,
                          clientRect.left,
                          clientRect.top,
                          clientRect.right - clientRect.left,
                          clientRect.bottom - clientRect.top,
                          0,
                          0,
                          static_cast<int>(framebufferWidth),
                          static_cast<int>(framebufferHeight),
                          framebuffer.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);
          }
          if (dib) {
            DeleteObject(dib);
          }

          if (paintCount <= 8u || (paintCount % 60u) == 0u) {
            vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                              "traceprobe",
                                              "wm_paint blit",
                                              {
                                                {"paint_count", std::to_string(paintCount)},
                                                {"fb_width", std::to_string(framebufferWidth)},
                                                {"fb_height", std::to_string(framebufferHeight)},
                                                {"fb_bytes", std::to_string(framebuffer.size())},
                                                {"client_width", std::to_string(clientRect.right - clientRect.left)},
                                                {"client_height", std::to_string(clientRect.bottom - clientRect.top)}
                                              });
          }
        }

        const auto overlay = self->overlay_text();
        const auto text = ToWide(overlay.empty() ? "vkpt desktop ui shell" : overlay);
        SetBkMode(hdc, TRANSPARENT);
        const int margin = 12;
        RECT textRect{
          clientRect.left + margin,
          clientRect.top + 40,
          clientRect.right - margin,
          clientRect.bottom - margin
        };
        // Draw a dark shadow pass first, then white text for readability.
        RECT shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 1);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, text.c_str(), -1, &shadowRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, text.c_str(), -1, &textRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOCLIP);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_CLOSE: {
        self->emit_close_requested();
        self->mark_closed();
        DestroyWindow(hwnd);
        return 0;
      }
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: {
        const auto vk = static_cast<int>(wparam);
        if ((lparam & (1 << 30)) == 0u) {
          self->emit_key(vk, true);
        }
        return 0;
      }
      case WM_KEYUP:
      case WM_SYSKEYUP: {
        const auto vk = static_cast<int>(wparam);
        self->emit_key(vk, false);
        return 0;
      }
      case WM_MOUSEMOVE:
        self->emit_mouse_move(ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      case WM_LBUTTONDOWN:
        self->emit_mouse_button(0, true, ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      case WM_LBUTTONUP:
        self->emit_mouse_button(0, false, ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      case WM_RBUTTONDOWN:
        self->emit_mouse_button(1, true, ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      case WM_RBUTTONUP:
        self->emit_mouse_button(1, false, ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      case WM_MOUSEWHEEL: {
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / 120.0f;
        self->emit_mouse_wheel(delta, ExtractWindowX(lparam), ExtractWindowY(lparam));
        return 0;
      }
      case WM_DESTROY:
        return 0;
      case WM_SIZE: {
        const auto width = static_cast<std::size_t>(LOWORD(lparam));
        const auto height = static_cast<std::size_t>(HIWORD(lparam));
        self->on_native_resize(width, height);
        return 0;
      }
      case WM_SETFOCUS:
        self->emit_focus_change(true);
        return 0;
      case WM_KILLFOCUS:
        self->emit_focus_change(false);
        return 0;
      case WM_COMMAND:
        if (HIWORD(wparam) == 0u) {
          const auto commandId = static_cast<std::uint32_t>(LOWORD(wparam));
          self->emit_menu_command(commandId);
        }
        return 0;
      default:
        break;
    }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace
#endif

bool DesktopWindow::initialize(std::size_t width, std::size_t height, std::string_view title) {
#ifdef _WIN32
  const auto className = L"vkpt-desktop-window";
  static bool classRegistered = false;
  if (!classRegistered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DesktopWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
    classRegistered = true;
  }

  const auto wideTitle = ToWide(title);
  auto* hwnd = CreateWindowExW(0u, className, wideTitle.c_str(), WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(width), static_cast<int>(height),
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    return false;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  m_hwnd = hwnd;
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
#endif
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  m_title.assign(title);
  m_open = true;
  m_lastMouseX = 0;
  m_lastMouseY = 0;
  m_events.emplace_back(InputEventNormalizer::resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
  m_events.emplace_back(InputEventNormalizer::focus(true));
  return true;
}

void* DesktopWindow::native_handle() const {
  return m_hwnd;
}

bool DesktopWindow::is_open() const {
  return m_open;
}

void DesktopWindow::close() {
  if (!m_open) {
    return;
  }
#ifdef _WIN32
  m_open = false;
  if (m_hwnd) {
    DestroyWindow(static_cast<HWND>(m_hwnd));
  }
#else
  m_open = false;
#endif
  m_events.emplace_back(InputEventNormalizer::close());
}

WindowMetrics DesktopWindow::metrics() const {
  return m_metrics;
}

bool DesktopWindow::poll_events() {
#ifdef _WIN32
  if (!m_hwnd) {
    return false;
  }
  MSG msg;
  while (PeekMessageW(&msg, static_cast<HWND>(m_hwnd), 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
#endif
  return m_open;
}

bool DesktopWindow::resize(std::size_t width, std::size_t height) {
  if (!m_open) {
    return false;
  }
  if (width == 0u || height == 0u) {
    return false;
  }
  const bool changed = (m_metrics.width != static_cast<int>(width))
                    || (m_metrics.height != static_cast<int>(height));
#ifdef _WIN32
  if (m_hwnd && changed) {
    const auto hwnd = static_cast<HWND>(m_hwnd);
    RECT requested{0, 0, static_cast<int>(width), static_cast<int>(height)};
    const DWORD style = static_cast<DWORD>(static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE)));
    const DWORD exStyle = static_cast<DWORD>(static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
    const bool hasMenu = GetMenu(hwnd) != nullptr;
    AdjustWindowRectEx(&requested, style, hasMenu, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0,
                 requested.right - requested.left,
                 requested.bottom - requested.top,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
#endif
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  m_events.emplace_back(InputEventNormalizer::resize(static_cast<std::uint32_t>(width),
                                                    static_cast<std::uint32_t>(height)));
  return true;
}

void DesktopWindow::on_native_resize(std::size_t width, std::size_t height) {
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  m_events.emplace_back(InputEventNormalizer::resize(static_cast<std::uint32_t>(width),
                                                    static_cast<std::uint32_t>(height)));
}

void DesktopWindow::set_title(std::string_view title) {
  m_title.assign(title);
#ifdef _WIN32
  if (m_hwnd) {
    const auto wideTitle = ToWide(title);
    SetWindowTextW(static_cast<HWND>(m_hwnd), wideTitle.c_str());
  }
#endif
}

void DesktopWindow::set_overlay_text(std::string_view text) {
  m_overlayText.assign(text);
#ifdef _WIN32
  if (m_hwnd) {
    InvalidateRect(static_cast<HWND>(m_hwnd), nullptr, FALSE);
  }
#endif
}

void DesktopWindow::set_framebuffer_rgba(const std::vector<std::uint8_t>& rgba,
                                         std::size_t width,
                                         std::size_t height) {
  if (width == 0u || height == 0u) {
    clear_framebuffer();
    return;
  }
  const std::size_t pixelCount = width * height;
  const std::size_t byteCount = pixelCount * 4u;
  if (rgba.size() < byteCount) {
    clear_framebuffer();
    return;
  }

  m_framebufferBgra.resize(byteCount);
  m_framebufferWidth = width;
  m_framebufferHeight = height;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t src = i * 4u;
    const std::size_t dst = src;
    m_framebufferBgra[dst + 0u] = rgba[src + 2u];
    m_framebufferBgra[dst + 1u] = rgba[src + 1u];
    m_framebufferBgra[dst + 2u] = rgba[src + 0u];
    m_framebufferBgra[dst + 3u] = 255u;
  }

  std::uint8_t rgbMax = 0u;
  std::uint64_t rgbSum = 0u;
  std::uint64_t rgbCount = 0u;
  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t base = i * 4u;
    const std::uint8_t r = rgba[base + 0u];
    const std::uint8_t g = rgba[base + 1u];
    const std::uint8_t b = rgba[base + 2u];
    rgbMax = std::max(rgbMax, std::max(r, std::max(g, b)));
    rgbSum += static_cast<std::uint64_t>(r) + static_cast<std::uint64_t>(g) + static_cast<std::uint64_t>(b);
    rgbCount += 3u;
  }
  const float rgbAvg = (rgbCount == 0u)
      ? 0.0f
      : static_cast<float>(rgbSum) / static_cast<float>(rgbCount);
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                    "traceprobe",
                                    "framebuffer upload",
                                    {
                                      {"width", std::to_string(width)},
                                      {"height", std::to_string(height)},
                                      {"bytes", std::to_string(byteCount)},
                                      {"rgb_max", std::to_string(static_cast<unsigned>(rgbMax))},
                                      {"rgb_avg", std::to_string(rgbAvg)}
                                    });

#ifdef _WIN32
  if (m_hwnd) {
    InvalidateRect(static_cast<HWND>(m_hwnd), nullptr, FALSE);
  }
#endif
}

void DesktopWindow::clear_framebuffer() {
  m_framebufferBgra.clear();
  m_framebufferWidth = 0u;
  m_framebufferHeight = 0u;
#ifdef _WIN32
  if (m_hwnd) {
    InvalidateRect(static_cast<HWND>(m_hwnd), nullptr, FALSE);
  }
#endif
}

void DesktopWindow::emit_focus_change(bool focused) {
  m_focused = focused;
  m_events.emplace_back(InputEventNormalizer::focus(focused));
}

void DesktopWindow::emit_close_requested() {
  m_events.emplace_back(InputEventNormalizer::close());
}

void DesktopWindow::emit_menu_command(std::uint32_t command_id) {
  m_events.push_back(InputEventNormalizer::menu_command(command_id));
}

void DesktopWindow::emit_key(std::int32_t key, bool pressed) {
  m_events.push_back(InputEventNormalizer::key(key, pressed));
}

void DesktopWindow::emit_mouse_move(int x, int y) {
  m_events.push_back(InputEventNormalizer::mouse_move(
      static_cast<float>(x), static_cast<float>(y),
      static_cast<float>(x - m_lastMouseX),
      static_cast<float>(y - m_lastMouseY)));
  m_lastMouseX = x;
  m_lastMouseY = y;
}

void DesktopWindow::emit_mouse_button(std::int32_t button, bool pressed, int x, int y) {
  m_events.push_back(InputEventNormalizer::mouse_button(button, pressed, static_cast<float>(x), static_cast<float>(y)));
}

void DesktopWindow::emit_mouse_wheel(float delta, int x, int y) {
  m_events.push_back(InputEventNormalizer::mouse_wheel(delta, static_cast<float>(x), static_cast<float>(y)));
}

std::vector<InputEvent> DesktopWindow::drain_events() {
  std::vector<InputEvent> out;
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out;
}

void DesktopWindow::mark_closed() {
  m_hwnd = nullptr;
  m_open = false;
}

std::size_t DesktopInput::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_queue.size());
  while (!m_queue.empty()) {
    out.push_back(m_queue.front());
    m_queue.pop_front();
  }
  return out.size();
}

void DesktopInput::queue(InputEvent event) {
  m_queue.push_back(event);
}

void DesktopInput::queue_normalized(InputEvent event) {
  m_queue.push_back(event);
}

void DesktopInput::emit_key(std::int32_t key, bool pressed) {
  queue(InputEventNormalizer::key(key, pressed));
}

void DesktopInput::emit_mouse_move(float x, float y, float dx, float dy) {
  queue(InputEventNormalizer::mouse_move(x, y, dx, dy));
}

void DesktopInput::emit_mouse_button(std::int32_t button, bool pressed, float x, float y) {
  queue(InputEventNormalizer::mouse_button(button, pressed, x, y));
}

void DesktopInput::emit_mouse_wheel(float delta, float x, float y) {
  queue(InputEventNormalizer::mouse_wheel(delta, x, y));
}

void DesktopInput::emit_touch(std::int32_t touch_id, std::int32_t phase, float x, float y) {
  InputEvent event{InputEventType::None, 0, x, y, 0u, touch_id};
  event.delta_z = static_cast<float>(phase);
  m_queue.push_back(event);
}

void DesktopEvents::publish(std::string_view source, const InputEvent& event) {
  (void)source;
  m_events.push_back(event);
}

std::size_t DesktopEvents::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out.size();
}

std::uint64_t DesktopTimeSource::now_ms() const {
  using namespace std::chrono;
  const auto now = duration_cast<std::chrono::milliseconds>(system_clock::now().time_since_epoch()).count();
  return now - m_startMs;
}

vkpt::core::Result<std::string> DesktopFileSystem::read_text_file(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  if (!stream) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  std::ostringstream out;
  out << stream.rdbuf();
  return vkpt::core::Result<std::string>::ok(out.str());
}

bool DesktopFileSystem::file_exists(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  return static_cast<bool>(stream);
}

vkpt::core::Result<void> DesktopClipboard::set_text(std::string_view text) {
  m_text = std::string(text);
  return vkpt::core::Result<void>::ok();
}

vkpt::core::Result<std::string> DesktopClipboard::get_text() const {
  if (m_text.empty()) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  return vkpt::core::Result<std::string>::ok(m_text);
}

void* DesktopSurfaceProvider::native_window_handle() const {
  return m_windowHandle;
}

void* DesktopSurfaceProvider::native_instance_handle() const {
  return m_instanceHandle;
}

void DesktopSurfaceProvider::set_handles(void* window_handle, void* instance_handle) {
  m_windowHandle = window_handle;
  m_instanceHandle = instance_handle;
}

DesktopPlatform::DesktopPlatform(std::string_view name) : m_name(name) {}

vkpt::core::Result<void> DesktopPlatform::initialize() {
  if (m_initialized) {
    return vkpt::core::Result<void>::ok();
  }
  if (!m_window.initialize(1280, 720, m_name)) {
    return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
  }
#ifdef _WIN32
  m_surface.set_handles(m_window.native_handle(), GetModuleHandleW(nullptr));
#endif
  m_initialized = true;
  return vkpt::core::Result<void>::ok();
}

void DesktopPlatform::shutdown() {
  if (!m_initialized) {
    return;
  }
  if (m_window.is_open()) {
    m_window.close();
  }
  m_initialized = false;
}

bool DesktopPlatform::is_headless() const {
  return false;
}

IWindow* DesktopPlatform::window() { return &m_window; }
const IWindow* DesktopPlatform::window() const { return &m_window; }
IInput* DesktopPlatform::input() { return &m_input; }
const IInput* DesktopPlatform::input() const { return &m_input; }
IEvents* DesktopPlatform::events() { return &m_events; }
const IEvents* DesktopPlatform::events() const { return &m_events; }
IFileSystem* DesktopPlatform::file_system() { return &m_file_system; }
const IFileSystem* DesktopPlatform::file_system() const { return &m_file_system; }
ITimeSource* DesktopPlatform::time_source() { return &m_time_source; }
const ITimeSource* DesktopPlatform::time_source() const { return &m_time_source; }
IClipboard* DesktopPlatform::clipboard() { return &m_clipboard; }
const IClipboard* DesktopPlatform::clipboard() const { return &m_clipboard; }
INativeSurfaceProvider* DesktopPlatform::native_surface() { return &m_surface; }
const INativeSurfaceProvider* DesktopPlatform::native_surface() const { return &m_surface; }

}  // namespace vkpt::platform
