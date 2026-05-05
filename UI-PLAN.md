Below is a deliberately granular Qt migration TODO list for porting the current raw C++/Win32 window strategy into a Qt-based shell while staying aligned with `plan.md`.

The key architectural reading is this: **Qt should become a platform/editor-shell implementation, not a renderer dependency**. `plan.md` explicitly requires hard separation between scene/material/benchmark code and backend/window internals, and defines platform responsibilities around window creation, event polling, timing, clipboard, filesystem, and native surface handles. ([GitHub][1]) The repo is still in early prototyping, and the current working spike is a single-file Win32/D3D12 Cornell-box path tracer used to validate the rendering loop before building the real module structure. ([GitHub][2]) Qt’s own docs support the pieces needed here: CMake integration via `Qt6::Widgets`, native widget/window handles, embedding `QWindow`s into widget UIs, and optional QRhi/QRhiWidget paths for accelerated APIs. ([Qt Documentation][3])

# Recommended migration shape

Do **not** port the renderer to Qt first. Port the **window shell** first.

The clean target shape should be:

```txt
pt_core
pt_scene
pt_render
pt_backends
pt_platform_headless
pt_platform_desktop_raw     // existing Win32/native fallback
pt_platform_qt              // new Qt implementation
pt_editor_qt                // later: docks, status bar, panels
ptapp
```

The first working target should be:

```txt
ptapp --window --platform qt --backend cpu
```

Then:

```txt
ptapp --window --platform qt --backend d3d12
```

And headless mode must remain independent:

```txt
ptapp --render --backend cpu
ptapp --headless
ptapp --doctor
ptapp --benchmark ...
```

`plan.md` treats benchmark mode as headless-capable, deterministic, fixed-seed, fixed-scene-hash, and structured-output oriented; the Qt port must not make benchmark execution depend on Qt widgets or the Qt event loop. ([GitHub][1])

## Status update (2026-05-04)

- Started implementation in `ptapp` and runtime config plumbing.
- Added `--platform <auto|raw|qt|headless>` parsing and validation.
- Added config/environment support for `platform` (`platform=` in config file and `PTAPP_PLATFORM` env var).
- Added runtime guard so `--platform qt` fails fast with a clear "not implemented" message until Qt backend lands.
- Added platform factory seam (`PlatformFactory`) and platform resolution logic (`auto` defaulting to `raw` for window mode and `headless` otherwise).
- Added Qt feature flags/presets/stubs so the codebase can compile with a staged Qt platform path.

## Status update (2026-05-05)

- Re-audited this TODO list against the completed Qt-platform pass.
- Decisions recorded: current Qt module name is `QtPlatform`; the CMake flag is `PT_ENABLE_QT`; Qt remains experimental/default-off; raw desktop remains the default `--window` platform until Qt reaches parity.
- Completed in code: Qt CMake flags/presets, guarded Qt discovery/setup, `pt_platform_qt`, `PlatformFactory`, `--platform` runtime selection, a `QMainWindow`-backed `QtPlatform`/`QtWindow`, CPU framebuffer blit, Qt input translation, Qt menu bar routing, native `QDockWidget` panels, native `QStatusBar` binding, `QSettings` dock layout persistence, Qt clipboard access, native handle exposure, DPI metrics, bounded GUI smoke flags, and runtime/build metadata for Qt support.
- Completed in model code and bound into the Qt shell: panel definitions, layout presets, panel visibility/dock/floating/collapse/move/resize mutations, status-bar model, inspector field schemas, benchmark panel model, panel property models, and layout JSON serialization.
- Completed in docs/tools: Qt port notes, platform contract, window lifecycle, native surface, threading, diagnostics, README updates, and local smoke scripts.
- Verified locally: Qt-disabled build, Qt-enabled Windows D3D12 build, non-GUI Qt-selected doctor/list/render paths, and `ptapp --window --platform qt --frames 5 --exit` using Qt's offscreen platform plugin.
- Still unimplemented by design: editable dock widgets, toolbar command widgets, QRhi, native D3D12 presentation inside Qt beyond the native-surface handoff, final packaging, and installer/deployment polish remain unchecked.

---

# Massive granular TODO list

## 0. Define the migration contract

* [x] Decide the official name of the Qt platform module: `QtPlatform` for the current staged module.
* [x] Decide the official CMake flag: `PT_ENABLE_QT`.
* [x] Decide whether Qt is experimental or first-class: experimental/default-off for now.
* [x] Decide whether the old raw desktop platform stays: yes, `PT_ENABLE_RAW_DESKTOP` remains default-on until Qt reaches parity.
* [x] Decide whether `--window` should automatically pick Qt when built with `PT_ENABLE_QT`: no for now; `auto` still resolves to raw for window mode.
* [x] Add explicit CLI override: `--platform raw`, `--platform qt`, `--platform headless`.
* [x] Add config-file equivalent: `platform=qt`.
* [x] Add build metadata field: `platform_shell=qt`.
* [x] Add runtime metadata field: `window_system=qt_widgets`.
* [x] Add diagnostics field: `qt_version`.
* [x] Add diagnostics field: `qt_platform_plugin`.
* [x] Add diagnostics field: `qt_high_dpi_policy`.
* [x] Add diagnostics field: `viewport_native_handle_valid`.
* [x] Add diagnostics field: `viewport_device_pixel_ratio`.
* [x] Add a README note that Qt is a shell/platform layer, not the renderer backend.
* [x] Add a `docs/qt_port.md` describing the porting strategy.
* [x] Add a `docs/platform_contract.md` if it does not already exist.
* [x] Add a `docs/window_lifecycle.md` describing Qt versus raw-native lifecycle differences.
* [x] Add a migration rule: no Qt includes in `scene`, `materials`, `pathtracer`, `benchmark`, or backend-neutral `render/interface`.
* [x] Add a migration rule: Qt code may talk to engine facades, not directly to renderer internals.
* [x] Add a migration rule: renderer backends receive only native handles/capability descriptors, never `QWidget*` or `QWindow*`.
* [x] Add a migration rule: editor commands remain serializable/replayable and do not become Qt signal side effects.

## 1. Audit the current raw window responsibilities

* [ ] Inventory every responsibility currently handled by `DesktopPlatform`.
* [ ] Inventory every responsibility currently handled by `DesktopWindow`.
* [ ] Inventory every responsibility currently handled by raw Win32 `WndProc`.
* [ ] Inventory every responsibility currently handled by `DesktopInput`.
* [ ] Inventory every responsibility currently handled by `DesktopClipboard`.
* [ ] Inventory every responsibility currently handled by `DesktopFileSystem`.
* [ ] Inventory every responsibility currently handled by `DesktopSurfaceProvider`.
* [ ] Mark each responsibility as one of: Qt-owned, engine-owned, backend-owned, or obsolete.
* [x] Map `CreateWindowExW` to Qt window/widget creation. Staged as top-level `QWidget` creation in `QtWindow::initialize`.
* [ ] Map `RegisterClassExW` to no-op under Qt.
* [x] Map `PeekMessageW` to Qt event processing. Staged as `QCoreApplication::processEvents()` in `QtWindow::poll_events`.
* [x] Map `WM_PAINT` to either `QWidget::paintEvent` or GPU swapchain present. Staged for CPU blit through `QWidget::paintEvent`/`QPainter`.
* [ ] Map `WM_ERASEBKGND` behavior to Qt no-background/opaque-widget flags.
* [x] Map `WM_CLOSE` to `QCloseEvent`. Staged through the internal QWidget close handler.
* [ ] Map `WM_DESTROY` to Qt widget destruction/lifetime handling.
* [x] Map `WM_SIZE` to `resizeEvent`.
* [x] Map `WM_SETFOCUS` and `WM_KILLFOCUS` to `focusInEvent` and `focusOutEvent`.
* [x] Map `WM_KEYDOWN` and `WM_KEYUP` to `keyPressEvent` and `keyReleaseEvent`.
* [x] Map `WM_MOUSEMOVE` to `mouseMoveEvent`.
* [x] Map button messages to `mousePressEvent` and `mouseReleaseEvent`.
* [x] Map `WM_MOUSEWHEEL` to `wheelEvent`.
* [ ] Map native menu command IDs to `QAction` callbacks.
* [x] Map `SetWindowTextW` to `QWidget::setWindowTitle`. Staged during Qt window initialization.
* [x] Map `InvalidateRect` to `QWidget::update`.
* [x] Map `StretchDIBits` / `StretchBlt` to `QPainter::drawImage` for the CPU preview path.
* [x] Map `HWND` exposure to `QWidget::winId()` or `QWindow::winId()`.
* [ ] Confirm where the current preview framebuffer is produced.
* [x] Confirm current framebuffer format: RGBA input, BGRA storage, alpha handling.
* [x] Confirm current overlay text rendering requirements.
* [x] Confirm whether overlay text should stay engine-rendered, Qt-rendered, or both.
* [x] Confirm whether current logs named `black_canvas_trace` should remain during Qt port. Current logging keeps them.
* [x] Confirm whether current raw menu bar code should be deleted or kept behind `PT_ENABLE_RAW_DESKTOP`. Current decision: keep it behind `PT_ENABLE_RAW_DESKTOP`.

## 2. CMake and dependency setup

* [x] Add `option(PT_ENABLE_QT "Enable Qt desktop platform/editor shell" OFF)`.
* [x] Add `option(PT_ENABLE_QT_EDITOR "Enable Qt editor widgets" OFF)`.
* [x] Add `option(PT_ENABLE_QT_RHI "Enable experimental Qt QRhi viewport" OFF)`.
* [x] Add `option(PT_ENABLE_RAW_DESKTOP "Enable raw native desktop platform" ON)`.
* [x] Add `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)` inside `if(PT_ENABLE_QT)`. Implemented as quiet discovery plus a fatal error if Qt6 is missing.
* [x] Use `qt_standard_project_setup()` when Qt is enabled.
* [x] Link Qt only to Qt-specific targets, not global `ptapp` if avoidable.
* [x] Add `Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets` to `pt_platform_qt`.
* [x] Add `CMAKE_AUTOMOC ON` through Qt setup.
* [ ] Add `CMAKE_AUTOUIC ON` only if using `.ui` files.
* [ ] Add `CMAKE_AUTORCC ON` only if using `.qrc` resources.
* [ ] Add a Qt minimum version decision.
* [ ] If using `QRhiWidget`, require Qt `>= 6.7`, because Qt documents `QRhiWidget` as available since Qt 6.7. ([Qt Documentation][4])
* [x] Keep Qt optional in all presets.
* [x] Add `windows-clangcl-d3d12-qt-debug` preset.
* [x] Add `windows-clangcl-d3d12-qt-release` preset.
* [x] Add `desktop-clang-qt-debug` preset.
* [x] Add `desktop-clang-qt-release` preset.
* [x] Add `PT_ENABLE_QT=ON` to Qt presets.
* [ ] Add `PT_ENABLE_EDITOR=ON` only when testing the editor shell.
* [x] Keep `desktop-clang-benchmark` Qt-free unless explicitly requested.
* [x] Ensure `ptapp --headless` builds without Qt.
* [x] Ensure `ptapp --render` builds without Qt.
* [x] Ensure `ptapp --doctor` builds without Qt.
* [x] Add a CMake configure message showing whether Qt is enabled.
* [x] Add Qt version to generated `build_info.generated.h`.
* [x] Add Qt platform flag to generated build info.
* [x] Update feature flag CSV to include `PT_ENABLE_QT`.
* [ ] Add packaging TODOs for Qt platform plugins.
* [ ] Add Windows deploy helper using `windeployqt` later.
* [x] Do not make `Qt6_DIR` mandatory unless `PT_ENABLE_QT=ON`.
* [x] Add clear CMake error if `PT_ENABLE_QT=ON` but Qt is missing.
* [x] Verify CMake still works with existing Vulkan/D3D12 flags.
* [x] Verify CMake still works with `PT_ENABLE_CPU_RAYTRACER=ON`.
* [x] Verify Qt target compiles with C++23.
* [x] Verify no Qt headers leak into non-Qt public headers.

## 3. Target and file layout

* [x] Create `src/platform/qt/`.
* [x] Add `src/platform/qt/QtPlatform.h`.
* [x] Add `src/platform/qt/QtPlatform.cpp`.
* [ ] Add `src/platform/qt/QtWindow.h`.
* [ ] Add `src/platform/qt/QtWindow.cpp`.
* [ ] Add `src/platform/qt/QtInput.h`.
* [ ] Add `src/platform/qt/QtInput.cpp`.
* [ ] Add `src/platform/qt/QtEvents.h`.
* [ ] Add `src/platform/qt/QtEvents.cpp`.
* [ ] Add `src/platform/qt/QtClipboard.h`.
* [ ] Add `src/platform/qt/QtClipboard.cpp`.
* [ ] Add `src/platform/qt/QtFileSystem.h`.
* [ ] Add `src/platform/qt/QtFileSystem.cpp`.
* [ ] Add `src/platform/qt/QtTimeSource.h`.
* [ ] Add `src/platform/qt/QtTimeSource.cpp`.
* [ ] Add `src/platform/qt/QtNativeSurfaceProvider.h`.
* [ ] Add `src/platform/qt/QtNativeSurfaceProvider.cpp`.
* [ ] Add `src/platform/qt/QtEventTranslator.h`.
* [ ] Add `src/platform/qt/QtEventTranslator.cpp`.
* [ ] Add `src/platform/qt/QtViewportWidget.h`.
* [ ] Add `src/platform/qt/QtViewportWidget.cpp`.
* [ ] Add `src/editor/qt/` only after platform window works.
* [ ] Add `src/editor/qt/QtMainWindow.h`.
* [ ] Add `src/editor/qt/QtMainWindow.cpp`.
* [ ] Add `src/editor/qt/QtMenuBuilder.h`.
* [ ] Add `src/editor/qt/QtMenuBuilder.cpp`.
* [ ] Add `src/editor/qt/QtDockPanels.h`.
* [ ] Add `src/editor/qt/QtDockPanels.cpp`.
* [x] Keep Qt editor files out of `PT_CPU_SOURCES` unless Qt is enabled. No Qt editor files are staged yet.
* [x] Create a `pt_platform_qt` static library.
* [ ] Create a `pt_editor_qt` static library later.
* [x] Link `ptapp` to `pt_platform_qt` only when `PT_ENABLE_QT`.
* [x] Avoid putting all Qt files directly into `ptapp`.
* [x] Add a compile definition `VKPT_HAS_QT=1` only to targets that need it.
* [ ] Add a compile definition `VKPT_QT_EDITOR=1` only to editor Qt target.
* [x] Keep `platform/Interfaces.h` Qt-free.

## 4. Platform abstraction alignment

* [ ] Re-read `plan.md` Platform API section and treat it as the port contract.
* [x] Confirm `IPlatform::initialize` can create a Qt-backed window. Staged in `QtPlatform::initialize`.
* [x] Confirm `IPlatform::shutdown` can safely tear down Qt widgets. Staged as hide/close; final destruction order still needs verification.
* [x] Confirm `IPlatform::is_headless` returns false for Qt.
* [x] Confirm `IPlatform::window` returns `QtWindow`.
* [x] Confirm `IPlatform::input` returns `QtInput`. Staged; event queue implementation is still partial.
* [x] Confirm `IPlatform::events` returns `QtEvents`.
* [x] Confirm `IPlatform::file_system` returns either existing filesystem or Qt-backed filesystem. Current staged implementation uses standard file IO in `QtFileSystem`.
* [x] Confirm `IPlatform::time_source` returns `QtTimeSource`.
* [x] Confirm `IPlatform::clipboard` returns `QtClipboard`. Current implementation uses Qt clipboard APIs with a cache fallback.
* [x] Confirm `IPlatform::native_surface` returns a Qt native-surface provider. Current `QtSurfaceProvider` exposes the Qt viewport native handle.
* [ ] Add `QtPlatform::initialize(QApplication*)` variant or ownership model.
* [x] Decide whether `QtPlatform` owns `QApplication`. Current staged decision: `QtPlatform` owns a singleton `QApplication` runtime.
* [ ] Recommended: app entry owns `QApplication`; `QtPlatform` owns windows/widgets.
* [x] Ensure only one `QApplication` exists. Staged with `QCoreApplication::instance()` guard and static runtime storage.
* [x] Ensure `QApplication` is created only on GUI path. Staged through the selected Qt window branch.
* [x] Ensure no `QApplication` is created during headless render. Current render/headless CLI paths do not initialize `QtPlatform`.
* [x] Ensure no `QApplication` is created during benchmark CI.
* [ ] Ensure engine services are created before Qt widgets need engine state.
* [ ] Ensure Qt widgets are destroyed before renderer backend shutdown if swapchain depends on widget handles.
* [ ] Ensure renderer backend is destroyed before native viewport handle disappears.
* [ ] Add an explicit shutdown order test.
* [x] Add a comment explaining Qt GUI-thread ownership.
* [x] Add a comment explaining native handle lifetime.
* [x] Add runtime assertions for using Qt platform without `QApplication`.
* [ ] Add runtime assertions for missing viewport widget.
* [ ] Add runtime assertions for null native surface handle.

## 5. Application entry and mode selection

* [ ] Refactor `main.cpp` enough to isolate CLI parsing from run modes.
* [x] Parse arguments before constructing Qt where possible.
* [ ] Preserve `argv` for Qt because `QApplication` may inspect Qt-specific args.
* [ ] Add `bool wantsGui = config.window || config.mode == DemoMode || config.mode == EditorMode`.
* [ ] Add `bool wantsQt = wantsGui && config.platform == "qt"`.
* [x] If `wantsQt`, create `QApplication`. Current staged implementation creates it inside `QtPlatform::initialize`.
* [x] If not `wantsQt`, keep current raw/headless path.
* [x] Add `--platform qt`.
* [x] Add `--platform raw`.
* [x] Add `--platform headless`.
* [x] Add invalid platform diagnostics.
* [x] Add default platform selection logic.
* [ ] Recommended default on Qt-enabled builds: `--window` picks Qt, `--headless` never does.
* [ ] Add `--qt-style <name>` optional later.
* [ ] Add `--qt-scale <factor>` optional later only if needed.
* [ ] Add `--qt-no-native-menubar` optional later if menu behavior differs by OS.
* [ ] Add `--ui-model-smoke --platform qt` path.
* [x] Add `--doctor` output for Qt build but keep it non-GUI.
* [x] Add `--doctor --platform qt` optional GUI-capability check.
* [x] Ensure `--version --json` reports Qt support without creating a window.
* [x] Ensure `--list-backends` does not require Qt.
* [x] Ensure `--check-backends` does not require Qt unless checking presentable surface.
* [x] Ensure `--render` ignores Qt even if Qt is built.
* [x] Ensure `--window --render` gives deterministic precedence or a clear error.
* [ ] Ensure `--benchmark --platform qt` is rejected unless explicitly supporting visual benchmark mode.
* [x] Ensure config dump includes selected platform.

## 6. Qt event-loop strategy

* [x] Decide first-phase event-loop model. Current staged model keeps the app-owned render loop.
* [x] Recommended phase 1: keep engine-owned loop, call `QCoreApplication::processEvents()` inside `QtWindow::poll_events`.
* [ ] Recommended phase 2: switch GUI to `QApplication::exec()` plus `QTimer` driving engine ticks.
* [ ] Add a `QtRunLoopAdapter` abstraction so this can change later.
* [x] Implement `QtWindow::poll_events()` by processing pending Qt events.
* [x] Ensure `poll_events()` returns false after close.
* [x] Ensure `poll_events()` does not block indefinitely.
* [ ] Ensure engine frame tick still matches `plan.md` frame lifecycle.
* [ ] Ensure InputPhase consumes Qt-translated events.
* [ ] Ensure CommandPhase remains engine-controlled.
* [ ] Ensure RenderPreparationPhase does not run inside arbitrary Qt signal callbacks.
* [ ] Ensure RenderSubmitPhase remains explicit.
* [ ] Ensure PresentOrExportPhase owns viewport update/present.
* [x] Avoid doing expensive rendering directly in `paintEvent`.
* [x] For CPU-blit preview, `paintEvent` should only draw the latest completed image.
* [ ] For GPU swapchain preview, Qt should not repaint over the swapchain surface.
* [x] Add a frame pump timer later: `QTimer` interval `0` or `16ms`. Staged as a 16 ms viewport repaint timer.
* [ ] Add a render budget parameter for interactive mode.
* [ ] Add frame pacing metric under Qt.
* [ ] Add diagnostics if Qt event processing takes too long.
* [ ] Add diagnostics if UI thread misses frame budget.
* [ ] Add diagnostics if renderer blocks Qt event loop.
* [x] Add test mode that runs Qt window for N frames then exits.

## 7. Qt window implementation

* [x] Implement `QtWindow : public IWindow`.
* [x] Give `QtWindow` a pointer/reference to `QtViewportWidget`.
* [x] Implement `initialize(width, height, title)`.
* [x] Implement `is_open`.
* [x] Implement `close`.
* [x] Implement `metrics`.
* [x] Implement `poll_events`.
* [x] Implement `resize`.
* [x] Implement `set_title`.
* [x] Implement `set_overlay_text`.
* [x] Implement `set_framebuffer_rgba`.
* [x] Implement `clear_framebuffer`.
* [x] Implement `native_handle`.
* [x] Implement `drain_events`.
* [x] Track `m_open`.
* [x] Track `m_focused`.
* [x] Track logical width/height.
* [ ] Track physical framebuffer width/height.
* [x] Track device pixel ratio.
* [x] Track last mouse position.
* [x] Track queued input events.
* [x] Emit initial resize event after showing.
* [x] Emit initial focus event after showing if focused.
* [x] Emit close event exactly once.
* [x] Avoid double-close during widget destruction.
* [x] Avoid posting events after shutdown.
* [ ] Ensure `QtWindow::close()` invokes Qt widget close on GUI thread.
* [ ] Ensure `QtWindow::set_title()` invokes Qt widget title update on GUI thread.
* [ ] Ensure `QtWindow::set_framebuffer_rgba()` does not mutate widget state from a worker thread.
* [ ] Add queued invocation if render thread produces frames.
* [ ] Add mutex or double-buffering for framebuffer transfer.
* [x] Add max framebuffer size guard.
* [x] Add invalid framebuffer format diagnostics.
* [x] Add black-frame trace diagnostics during early port.

## 8. Qt viewport widget

* [x] Create `QtViewportWidget : public QWidget`. Staged as an internal `QtViewportWindow : public QWidget`; final separate `QtViewportWidget` files remain open.
* [x] Enable mouse tracking.
* [x] Set focus policy to strong focus.
* [ ] Set size policy to expanding.
* [ ] Set minimum size, maybe `320x180`.
* [x] Store pointer to event sink or `QtWindow`.
* [x] Override `paintEvent`.
* [x] Override `resizeEvent`.
* [x] Override `closeEvent` only if top-level viewport; otherwise handle main window close. Staged for the current top-level QWidget path.
* [x] Override `keyPressEvent`.
* [x] Override `keyReleaseEvent`.
* [x] Override `mouseMoveEvent`.
* [x] Override `mousePressEvent`.
* [x] Override `mouseReleaseEvent`.
* [x] Override `wheelEvent`.
* [x] Override `focusInEvent`.
* [x] Override `focusOutEvent`.
* [ ] Override `enterEvent` if needed.
* [ ] Override `leaveEvent` if needed.
* [ ] Override `event` for touch/tablet events later.
* [x] Convert Qt logical coordinates to engine coordinates.
* [x] Decide whether engine input uses logical or physical pixels.
* [ ] Recommended: input positions in logical viewport pixels, render size in physical pixels.
* [ ] Apply `devicePixelRatioF()` to framebuffer sizing.
* [ ] Add `viewportLogicalSize()`.
* [ ] Add `viewportFramebufferSize()`.
* [x] Add `nativeWindowHandle()`.
* [x] Add `nativeInstanceHandle()` for Windows if D3D12 needs `HINSTANCE`.
* [x] Set widget attributes for CPU-blit path.
* [ ] Set different widget attributes for native GPU path.
* [ ] Ensure viewport does not flicker during resize.
* [ ] Ensure viewport draws a placeholder before first frame.
* [x] Ensure viewport draws overlay text if CPU-blit path.
* [ ] Ensure viewport can disable Qt painting for GPU swapchain path.
* [ ] Add context menu suppression unless desired.
* [ ] Add drag/drop later for assets.
* [ ] Add `QAccessible` names later for UI testing.

## 9. CPU framebuffer blit path first

* [x] Implement CPU preview path before native D3D12 swapchain.
* [x] Store latest RGBA8 frame in `QtWindow`. Staged through the internal QWidget/QImage buffer owned by the Qt window path.
* [x] Convert RGBA8 to `QImage`.
* [x] Decide whether to copy or wrap external memory.
* [x] Recommended first: copy into owned `QImage` for safety.
* [x] Ensure row stride is correct for the current RGBA8 copy path.
* [x] Ensure alpha is set to opaque.
* [x] Draw image scaled to viewport rect in `paintEvent`.
* [ ] Preserve aspect ratio decision: stretch, fit, or fill.
* [x] Recommended first: stretch exactly like current Win32 `StretchBlt` behavior.
* [ ] Add optional checkerboard/letterbox later.
* [x] Draw overlay text with `QPainter`.
* [ ] Draw status text: frame index, samples, backend, FPS.
* [ ] Draw black-frame diagnostic if frame is all zero.
* [x] Call `update()` when new framebuffer arrives. New frames set a dirty flag, call `update()`, and the 16 ms viewport `QTimer` also coalesces dirty updates.
* [x] Do not call `repaint()` in render loop.
* [x] Avoid allocating a new image every paint event.
* [x] Allocate/resize only when frame dimensions change.
* [ ] Add simple FPS counter.
* [ ] Add simple upload/copy timing.
* [ ] Add copy bandwidth metric.
* [ ] Add visual test pattern before path tracer integration.
* [ ] Add gradient test pattern.
* [ ] Add checkerboard test pattern.
* [x] Add Cornell CPU render preview. Staged in the Qt window path using the CPU tiled fallback.
* [ ] Add test for resize while CPU blitting.
* [ ] Add test for minimize/restore.
* [ ] Add test for high-DPI display.
* [ ] Add test for moving between monitors.
* [ ] Add test for 0-size resize events.
* [ ] Add test for closing while render thread is active.

## 10. Native GPU surface path

* [ ] Decide whether the GPU viewport is `QWidget`-native or `QWindow`-based.
* [ ] Recommended short term: `QWidget` viewport with native window handle for D3D12.
* [ ] Recommended long term: consider `QWindow` viewport if swapchain ownership becomes cleaner.
* [ ] Force native handle only on the viewport, not every widget.
* [ ] Use `Qt::WA_NativeWindow` on the viewport if using QWidget native surface.
* [ ] Call `winId()` only after the widget is created and shown.
* [ ] Re-query native handle after recreation-sensitive events.
* [ ] Do not store stale `WId` forever; Qt warns that native widget IDs can change at runtime. ([Qt Documentation][5])
* [ ] On Windows, reinterpret `WId` as `HWND` only inside Qt platform/native-surface code.
* [ ] Do not expose `HWND` to scene/editor/benchmark code.
* [x] Expose only `void* native_window_handle()` through `INativeSurfaceProvider`. Interface exists; Qt provider still returns null until native surface work lands.
* [ ] Add platform enum to native surface descriptor: Windows, X11, Wayland, Cocoa.
* [ ] Add physical surface extent to native surface descriptor.
* [ ] Add DPI scale to native surface descriptor.
* [ ] Add `surface_generation` counter.
* [ ] Increment generation when native handle changes.
* [ ] Notify renderer backend when surface generation changes.
* [ ] Add resize callback for swapchain recreation.
* [ ] Add minimize handling.
* [ ] Add occlusion handling later.
* [ ] Add device-lost recovery path.
* [ ] Add swapchain recreation lock.
* [ ] Add present failure diagnostics.
* [ ] Add validation that backend supports present to Qt surface.
* [ ] Add fallback to CPU-blit preview if native present fails.
* [ ] Add `--viewport-mode cpu-blit`.
* [ ] Add `--viewport-mode native-surface`.
* [ ] Add `--viewport-mode qt-rhi` later if desired.

## 11. D3D12-specific migration

* [ ] Extract D3D12 renderer setup from raw-window assumptions.
* [ ] Make D3D12 backend accept `NativeSurfaceDesc`.
* [ ] Put `HWND` extraction only in Qt platform or D3D12 backend boundary.
* [ ] Confirm `IDXGIFactory` swapchain creation path accepts child-window HWND.
* [ ] Confirm swapchain resize uses physical pixel dimensions.
* [ ] Confirm tearing flag behavior under Qt child window.
* [ ] Confirm vsync flag behavior under Qt.
* [ ] Confirm DWM composition behavior.
* [ ] Confirm alt-enter fullscreen is disabled or explicitly handled.
* [ ] Confirm Qt menu focus does not break viewport input.
* [ ] Confirm viewport focus returns after clicking render area.
* [ ] Confirm frame latency waitable object behavior still works.
* [ ] Confirm D3D12 debug layer logs are routed to diagnostics.
* [ ] Confirm WARP fallback still works.
* [ ] Confirm adapter selection still works.
* [ ] Confirm shader compile errors still show in diagnostics panel/log.
* [ ] Confirm resize does not present to old backbuffer.
* [ ] Confirm renderer waits for GPU idle before destroying old swapchain.
* [ ] Confirm closing window waits for in-flight frames.
* [ ] Confirm device removal writes crash artifact.
* [ ] Confirm current title-bar performance metrics move to Qt status bar/title.
* [ ] Add Qt status-bar metric string equivalent to current title update.
* [ ] Add menu action to copy adapter info.
* [ ] Add menu action to toggle WARP if runtime switching is supported later.
* [ ] Add menu action to dump D3D12 state.
* [ ] Add screenshot/export action.
* [ ] Add benchmark-start action that freezes scene before measuring.

## 12. Vulkan-specific future path

* [ ] Do not prioritize Vulkan-in-Qt before D3D12 parity if Windows/D3D12 is the immediate target.
* [ ] Add a `NativeSurfaceDesc` variant for Vulkan later.
* [ ] Investigate whether using Qt’s Vulkan helpers conflicts with current backend ownership.
* [ ] Avoid `QVulkanWindow` unless you want Qt to own much more Vulkan lifecycle.
* [ ] Note that Qt says `QVulkanWindow` may not eliminate the need for a fully custom `QWindow` subclass in advanced use cases. ([Qt Documentation][6])
* [ ] If embedding a `QWindow` into widgets, use `QWidget::createWindowContainer`.
* [ ] Document limitations of `createWindowContainer`.
* [ ] Add Vulkan surface creation path from Qt window handle.
* [ ] Add Linux X11 test.
* [ ] Add Linux Wayland test.
* [ ] Add macOS Metal test later.
* [ ] Keep Vulkan/Metal/WebGPU roadmap subordinate to platform contract.
* [ ] Add backend capability check: `supports_qt_surface`.
* [ ] Add backend capability check: `requires_qwindow`.
* [ ] Add backend capability check: `requires_native_widget`.
* [ ] Add backend capability check: `supports_offscreen_only`.
* [ ] Add validation error when selected backend cannot present to chosen Qt viewport mode.

## 13. QRhi / QRhiWidget decision path

* [ ] Decide whether QRhi is a viewport helper or a real backend.
* [ ] Recommended: do not mix QRhi into existing D3D12 backend initially.
* [ ] Add `QtRhiBackend` only if you intentionally want a Qt-managed rendering backend.
* [ ] Keep `QRhiWidget` behind `PT_ENABLE_QT_RHI`.
* [ ] Confirm Qt version is at least 6.7 for `QRhiWidget`.
* [ ] Add `--viewport-mode qt-rhi` only after CPU-blit and native-surface modes work.
* [ ] Do not pass `QRhi*` into scene/pathtracer code.
* [ ] Do not pass `QRhiTexture*` into material descriptors.
* [ ] Consider QRhi only for editor overlays or debug visualization.
* [ ] Consider QRhi only if cross-API viewport management becomes more valuable than backend ownership.
* [ ] Document that QRhiWidget can target Direct3D 11/12, Vulkan, Metal, and OpenGL through Qt’s RHI, but using it changes the ownership model. ([Qt Documentation][4])

## 14. Input translation

* [x] Create `QtEventTranslator` path. Staged inside `QtViewportWindow`/`QtWindow` event emitters until separate files are split out.
* [x] Map `QKeyEvent` to engine raw key code.
* [x] Decide whether engine codes are Qt keys, native virtual keys, or internal normalized keys. Current staged path keeps Qt key in `code` and native virtual key in `raw_code`; a normalized enum remains future work.
* [ ] Recommended: add internal normalized key enum eventually.
* [x] Preserve raw code for diagnostics.
* [x] Handle auto-repeat.
* [x] Match current Win32 behavior: ignore repeated keydown if desired.
* [x] Map modifiers needed by viewport controls: Shift/Ctrl are tracked for append selection and movement speed. Full modifier payload remains future work.
* [x] Map mouse buttons.
* [x] Map mouse wheel delta.
* [ ] Map high-resolution trackpad wheel later.
* [x] Map mouse position.
* [x] Map mouse delta.
* [x] Grab mouse input during Qt viewport drags so camera repositioning continues outside the widget bounds.
* [x] Map focus gained/lost.
* [x] Map close requested.
* [x] Map resize.
* [ ] Map menu command.
* [ ] Add event timestamp if useful.
* [ ] Add device ID later for multi-device input.
* [ ] Add gamepad path later, not in first port.
* [ ] Add touch path later, not in first port.
* [ ] Add tablet stylus path later, not in first port.
* [ ] Ensure UI widgets can consume events before camera controller.
* [x] Ensure viewport receives camera controls only when focused. Events come from the focused Qt viewport widget and focus loss clears held input state.
* [ ] Ensure menu shortcuts do not also trigger camera movement accidentally.
* [ ] Ensure text input controls do not send key events to camera.
* [ ] Add debug overlay showing last 10 input events.
* [ ] Add input-event JSON trace option.
* [ ] Add test for WASD camera movement.
* [ ] Add test for orbit drag.
* [ ] Add test for right-mouse drag.
* [ ] Add test for wheel zoom.
* [ ] Add test for key release after focus loss.
* [ ] Add test for close event.

## 15. Menus, actions, and command routing

* [x] Replace raw Win32 menu creation with `QMenuBar` for the Qt shell.
* [x] Build menus from existing `UiModels` menu model if possible.
* [x] Preserve stable command IDs.
* [x] Map every `QAction` to an engine `EditorCommand` or `InputEvent::MenuCommand`.
* [x] Keep command execution deferred to CommandPhase. Qt actions enqueue `MenuCommand`; the app loop routes/logs them.
* [x] Do not mutate scene directly in Qt action lambda.
* [x] Add File menu.
* [ ] Add Open Scene action.
* [ ] Add Save Scene action later.
* [ ] Add Export PNG action.
* [ ] Add Export EXR action.
* [ ] Add Exit action.
* [ ] Add View menu.
* [ ] Add Reset Camera action.
* [ ] Add Toggle Debug Overlay action.
* [ ] Add Toggle Denoiser action later.
* [ ] Add Render menu.
* [ ] Add Start/Pause Progressive Render action.
* [ ] Add Reset Accumulation action.
* [ ] Add Benchmark menu.
* [ ] Add Run Benchmark action.
* [ ] Add Export Benchmark JSON action.
* [ ] Add Tools menu.
* [ ] Add Doctor action.
* [ ] Add Dump Config action.
* [ ] Add Copy Diagnostics action.
* [x] Add Help menu.
* [ ] Add About action with build metadata.
* [ ] Add keyboard shortcuts.
* [ ] Add shortcut conflict diagnostics.
* [x] Add action enabled/disabled state based on current mode. Edit selection commands now refresh from `SelectionState`.
* [x] Add menu state refresh each frame or on relevant state changes. Qt refreshes menu state after viewport selection changes.
* [ ] Add command serialization metadata.
* [ ] Add undoable editor command path later.
* [x] Add tests for menu command IDs. Existing UI model smoke validates command IDs and mappings.
* [ ] Add tests for command queue ordering.

## 16. Main window and editor shell

* [x] Create `QtMainWindow : public QMainWindow`.
* [x] Add central viewport widget.
* [x] Add status bar data model (`StatusBarModel`).
* [ ] Add native Qt `QStatusBar`.
* [x] Add menu bar.
* [ ] Add toolbar later.
* [x] Enable nested/tabbed dock options on `QtMainWindow`.
* [x] Add engine-owned panel definitions for viewport, scene tree, inspector, asset browser, material editor, script panel, benchmark panel, benchmark history, console, and status bar.
* [x] Add engine-owned panel visibility, docked, floating, collapsed, move, and resize mutation helpers.
* [x] Add serialized `UiLayoutDocument` for panel layouts.
* [ ] Add Qt dock area layout using `QDockWidget`.
* [ ] Bind Qt docks to `UiLayoutDocument`.
* [ ] Add Scene Graph dock. Engine panel definition exists as `scene_tree`; Qt widget pending.
* [ ] Add Inspector dock. Inspector schemas/value-state helpers exist; Qt widget pending.
* [ ] Add Materials dock. Engine panel definition exists as `material_editor`; Qt widget pending.
* [ ] Add Lights dock. Inspector light property schema exists; dedicated Qt dock pending.
* [ ] Add Camera dock. Inspector camera property schema exists; dedicated Qt dock pending.
* [ ] Add Render Settings dock. No dedicated Qt widget yet.
* [ ] Add Benchmark dock. `BenchmarkPanelModel` exists; Qt widget pending.
* [ ] Add Diagnostics dock. Console/log model pieces exist; Qt diagnostics widget pending.
* [ ] Add Performance dock. Debug/profiler layout preset exists; Qt widget pending.
* [ ] Add Debug Views dock. Runtime debug-view state exists; Qt widget pending.
* [ ] Add Asset Browser dock later.
* [ ] Add Timeline dock later.
* [ ] Add Scripting dock later.
* [ ] Add Physics dock later.
* [x] Add layout preset data model: default, benchmark, material authoring, scripting, asset management, debug/profiler, minimal viewport, and fullscreen overlay.
* [ ] Bind layout presets to actual Qt docking state.
* [ ] Add docking layout persistence in the Qt shell.
* [ ] Add `QSettings` layout save.
* [ ] Add layout restore.
* [ ] Add default layout reset.
* [x] Add compact/minimal viewport layout data model.
* [x] Add full editor/default layout data model.
* [x] Add benchmark layout data model.
* [ ] Add validation layout data model.
* [x] Add status text binding to engine runtime state through the viewport overlay.
* [ ] Bind `StatusBarModel` to native Qt status-bar widgets.
* [x] Add scene name display in viewport overlay/status model.
* [x] Add backend display in viewport overlay/status model.
* [x] Add samples display in viewport overlay/status model.
* [x] Add FPS/frame-time fields to `StatusBarModel`.
* [ ] Add FPS display in native Qt status bar.
* [ ] Add paths/sec display.
* [ ] Add GPU time display when available.
* [x] Add CPU frame-time field to `StatusBarModel`.
* [ ] Add CPU time display in native Qt status bar.
* [ ] Add memory display later.
* [x] Add selected entity count field to `StatusBarModel`.
* [x] Add selected entity display in viewport overlay.
* [ ] Add update throttling for UI labels.
* [x] Add engine-side panel/model contracts for scene graph and editor panels.
* [ ] Add Qt model/view adapters for scene graph later.
* [ ] Keep actual scene data in engine model, not Qt model.

## 17. Viewport resize and DPI

* [x] Use Qt logical size for widget layout.
* [ ] Use physical size for render target dimensions.
* [ ] Multiply by `devicePixelRatioF`.
* [x] Store DPI scale in `WindowMetrics`.
* [x] Emit resize event when logical size changes.
* [ ] Emit framebuffer resize event when physical size changes.
* [ ] Reset accumulation on physical render size change.
* [ ] Recreate swapchain on physical render size change.
* [ ] Do not recreate swapchain on every layout noise event.
* [ ] Debounce resize if native backend stalls.
* [x] Handle zero-width/zero-height resize.
* [ ] Handle minimized window.
* [ ] Handle hidden viewport.
* [ ] Handle moving between DPI-scaled monitors.
* [ ] Handle title/menu/status bar reducing viewport area.
* [ ] Confirm render dimensions match central viewport, not top-level window.
* [ ] Add diagnostics for logical versus physical dimensions.
* [ ] Add debug overlay showing logical/physical size.
* [ ] Add test at 100% scale.
* [ ] Add test at 125% scale.
* [ ] Add test at 150% scale.
* [ ] Add test with window resize drag.
* [ ] Add test with maximize/restore.
* [ ] Add test with moving between monitors.
* [ ] Avoid calling geometry-changing functions from inside resize events in recursive ways; Qt explicitly warns that geometry changes inside resize/move handlers can recurse. ([Qt Documentation][5])

## 18. Threading and frame production

* [x] Decide first threading model. Current staged path keeps Qt on the main/UI thread and can render CPU samples on a background worker.
* [x] Recommended first: UI thread owns Qt, render work remains controlled by existing app loop.
* [x] Do not update QWidget from worker thread.
* [x] If background CPU render exists, use bounded latest-wins handoff with separate pending/display storage. Implemented for the Qt CPU-blit path.
* [x] Use mutex around frame copy or lock-free swap buffer.
* [x] Use Qt queued signal to notify new frame. The Qt handoff coalesces queued GUI-thread drains with `QMetaObject::invokeMethod`.
* [x] Ensure renderer thread stops before closing viewport. Staged by stopping/joining the Qt CPU worker before `platform->shutdown()`.
* [x] Ensure close event signals render thread shutdown. Qt close marks the window closed; the Qt render loop exits, sets the CPU worker stop flag, and joins before platform shutdown.
* [x] Ensure render thread join happens before Qt object destruction.
* [ ] Ensure GPU backend flush happens before native surface destruction.
* [x] Add atomic `renderStop`.
* [x] Add atomic `windowClosing`/liveness equivalent. The Qt CPU preview uses a render-stop flag plus a shared liveness gate so post-close publishes are ignored safely.
* [x] Add frame generation counter. Qt framebuffer stats track published and presented frame ids.
* [x] Add dropped-frame counter.
* [x] Add UI-thread frame-upload handoff. Qt coalesces GUI-thread drains and uses the viewport update/repaint timer instead of worker-thread painting.
* [x] Add render-thread frame-production timer. CPU preview publish cadence is throttled by `ui_present_hz`.
* [x] Avoid UI stale-frame consumption by replacing pending frames before presentation and recording dropped-frame counters.
* [x] Add diagnostics when render thread outruns UI. Latest-wins replacement increments and logs dropped frame counters.
* [ ] Add diagnostics when UI starves render.
* [x] Add maximum queued frames of 1 for preview. The Qt CPU preview handoff uses one latest-frame slot guarded by a mutex.
* [ ] Add tile-progress callback later.
* [ ] Add cancellation when scene changes.
* [ ] Add cancellation when render settings change.
* [ ] Add accumulation reset when camera changes.
* [ ] Add accumulation reset when material/light changes.
* [ ] Add accumulation reset when viewport size changes.

## 18A. Threaded CPU preview and blit handoff

The goal for this pass is a clear ownership model: Qt owns widgets and presentation, the render coordinator owns scene/camera/accumulation, CPU workers own ray work, and the only data crossing into Qt is a throttled latest display frame.

* [x] Define the Qt UI thread rule in code comments: no path tracing or film resolve in Qt paint/event handlers.
* [x] Define the render thread rule in code comments: render code may publish display frames but must not directly mutate Qt widget state.
* [x] Rename the mental model from "send rays to canvas" to "publish display frames to viewport".
* [x] Keep ray/sample/tile data inside renderer-owned film buffers.
* [x] Keep the Qt viewport API accepting RGBA8 display frames only.
* [x] Add a latest-wins frame handoff for the Qt CPU-blit path.
* [x] Make the Qt frame handoff thread-safe for calls from a render thread.
* [x] Ensure `QtWindow::set_framebuffer_rgba()` does not touch QWidget/QImage paint state directly from non-UI threads.
* [x] Use a bounded frame queue with effective depth 1.
* [x] Drop stale unpublished UI frames when the render thread outruns the UI thread.
* [x] Track received frame count.
* [x] Track presented frame count.
* [x] Track dropped frame count.
* [x] Track latest published frame id.
* [x] Track latest presented frame id.
* [x] Track latest display frame dimensions.
* [x] Track latest display frame sample index where available.
* [x] Coalesce Qt update requests so the render thread cannot spam the event queue.
* [x] Let Qt `update()` coalesce paints instead of calling `repaint()`.
* [x] Keep `paintEvent()` limited to drawing the most recent immutable display image plus overlay.
* [x] Avoid per-pixel CPU format conversion when the incoming frame is already RGBA8.
* [x] Copy frame rows with `memcpy` or equivalent row-copy, not a per-channel loop.
* [x] Preserve row-stride validation before accepting a display frame.
* [x] Preserve invalid-size and short-buffer diagnostics.
* [x] Cap preview publish rate independently of path tracer sample rate.
* [x] Default the CPU preview publish cap to 30 Hz.
* [x] Publish the first few frames immediately so the viewport is not blank while accumulation starts.
* [x] Add a CLI/config knob for preview present rate if it fits existing config patterns.
* [x] Clamp preview present rate to a sane range.
* [x] Report the effective preview present rate in runtime metadata or logs.
* [x] Ensure render thread shutdown happens before Qt viewport destruction.
* [x] Ensure pending frame handoff is cleared during `QtWindow::destroy()`.
* [x] Ensure publishing after close is ignored safely.
* [x] Avoid holding the frame handoff mutex while painting.
* [x] Avoid holding the frame handoff mutex while path tracing.
* [x] Avoid resolving LDR output every sample when the UI publish interval has not elapsed.
* [x] Keep camera/update commands separate from display-frame publication.
* [x] Keep resize/reset accumulation work owned by render coordinator, not Qt paint code.
* [x] Add a diagnostic log line for first Qt display frame received.
* [x] Add a diagnostic log line for first Qt display frame presented.
* [x] Add periodic diagnostic logging for received/presented/dropped frames.
* [x] Add overlay/status text with sample count, frame size, publish cap, and dropped frame count.
* [x] Add smoke coverage for bounded Qt window startup with the threaded handoff enabled.
* [x] Add smoke coverage for Qt-selected render remaining headless/non-GUI.
* [x] Update `docs/qt_threading.md` with the final UI/render/worker ownership model.
* [x] Update `docs/qt_diagnostics.md` with the new handoff counters.
* [x] Update `docs/qt_port.md` with the CPU-blit handoff contract.
* [x] Update README if new CLI/config options are introduced.
* [x] Mark the completed 18A items only after build and runtime smoke pass.

## 19. Diagnostics integration

* [x] Route Qt startup failures into existing logger.
* [x] Log Qt version.
* [x] Log Qt platform plugin name.
* [x] Log screen name.
* [x] Log device pixel ratio.
* [x] Log viewport native handle.
* [x] Log viewport logical size.
* [ ] Log viewport physical size.
* [x] Log selected viewport mode.
* [ ] Log selected backend.
* [ ] Log surface generation changes.
* [ ] Log swapchain creation.
* [ ] Log swapchain resize.
* [ ] Log swapchain destroy.
* [ ] Log first non-black frame.
* [ ] Log first present.
* [ ] Log paint events during CPU-blit mode.
* [ ] Log excessive paint events.
* [ ] Log renderer/device errors.
* [x] Log Qt close event.
* [x] Log shutdown order.
* [x] Add diagnostics dock.
* [ ] Add copy diagnostics button.
* [ ] Add save diagnostics button.
* [ ] Add filter by category.
* [ ] Add filter by severity.
* [ ] Match `plan.md` diagnostic categories: build, backend, shader, scene schema, asset import, benchmark, editor command, performance, determinism. ([GitHub][1])
* [x] Add startup failure report for Qt initialization failure.
* [ ] Add crash recorder fields for Qt surface state.
* [x] Add black-canvas trace parity with raw window path.
* [ ] Add `--qt-trace-events` optional flag.
* [ ] Add `--qt-trace-paint` optional flag.
* [ ] Add `--qt-trace-input` optional flag.

## 20. Benchmark and determinism guardrails

* [ ] Keep benchmark harness independent of `QApplication`.
* [ ] Keep benchmark result schema unchanged.
* [ ] Keep benchmark image output independent of viewport.
* [ ] Keep benchmark timing independent of Qt repaint timing.
* [ ] Prevent live Qt UI mutation during benchmark mode.
* [ ] Add mode transition: Demo → Benchmark freezes scene.
* [ ] Add mode transition: Benchmark → Demo restores viewport controls.
* [ ] Record whether benchmark was launched from GUI.
* [ ] Record whether benchmark displayed live preview.
* [ ] Do not include Qt event-loop time in core renderer timing unless clearly separated.
* [ ] Add separate UI timing if visual benchmark mode exists.
* [ ] Add warning if benchmark is run with visible window and not strict mode.
* [x] Add engine-owned benchmark panel model carrying `BenchmarkRunDesc`, raw metrics, workload cost, score, calibration actions, summary, and artifact location.
* [x] Add GUI benchmark panel that creates `BenchmarkRunDesc`.
* [ ] Add GUI benchmark panel that validates desc before launch.
* [ ] Add GUI benchmark panel that shows warmup/measure/resolve phases.
* [ ] Add GUI benchmark panel that opens output folder.
* [ ] Add GUI benchmark panel that copies JSON summary.
* [ ] Add GUI benchmark panel that shows pass/fail.
* [ ] Ensure benchmark controller remains engine-owned, not Qt-owned.
* [ ] Ensure benchmark output still writes JSON/CSV/PNG/EXR as planned. ([GitHub][1])
* [ ] Add regression test: benchmark still runs in a Qt-enabled build without launching Qt.

## 21. Scene and editor data binding

* [x] Build read-only scene graph dock first.
* [x] Show entity names.
* [x] Show stable IDs.
* [x] Show component badges.
* [x] Show transform values.
* [x] Show material assignment.
* [x] Show light assignment.
* [x] Show camera assignment.
* [x] Add selection model.
* [x] Add inspector property schemas for transform, material, light, camera, and script fields.
* [x] Add inspector field value-state helpers for exact, mixed, and unsupported values.
* [x] Convert Qt selection to engine selection command. Left-click viewport picking now emits `SelectEntityCommand`/`ClearSelectionCommand`.
* [x] Do not mutate scene from selection callback except through command buffer. Viewport pick updates `SelectionState` and overlay bounds only.
* [x] Add CPU viewport-picking fallback using scene object bounds.
* [x] Use triangle-level mesh picking after bounds broad phase and reject backface-culled triangles.
* [x] Add selected-object bounding box overlay in the Qt viewport.
* [x] Draw selected-object bounds as a projected 3D wireframe box in the Qt viewport.
* [x] Add selected-object viewport gizmo overlays for translate, rotate, scale, and universal transform modes.
* [ ] Add alpha-mask/material-opacity aware viewport picking when scene materials expose opacity.
* [ ] Make transform gizmo handles mutate selected entity transforms and rebuild the render scene.
* [x] Reproject selected bounds after camera movement and resize.
* [x] Add inspector read-only view.
* [ ] Add inspector editing later.
* [ ] Add transform editor later.
* [ ] Add material editor later.
* [ ] Add light editor later.
* [ ] Add camera editor later.
* [x] Add first-pass lens and film camera fields to the Qt camera and inspector docks.
* [x] Make authored camera aperture/focus/exposure/white-balance fields editable through dock property edits.
* [x] Make authored iris blades, iris roundness, iris rotation, and anamorphic squeeze affect CPU camera ray sampling.
* [x] Add GPU camera ray support for authored aperture, focus distance, iris, and anamorphic controls.
* [x] Add pick-to-focus and focus-plane overlay.
* [x] Add saved camera shot controls.
* [x] Add render settings panel.
* [ ] Add samples-per-pixel control.
* [x] Add max-depth control.
* [x] Add NEE toggle.
* [x] Add MIS toggle.
* [ ] Add accumulation reset button.
* [x] Add tone mapper dropdown.
* [x] Add exposure slider.
* [x] Add gamma and clamp-output controls.
* [x] Add output transform dropdown.
* [x] Wire D3D12/Vulkan film resolve settings to match CPU resolve.
* [ ] Add debug view selector.
* [ ] Add command preview diagnostics.
* [ ] Add undo stack after command layer is stable.
* [ ] Add redo stack.
* [ ] Add dirty-state marker.
* [ ] Add save prompt later.
* [ ] Add editor command serialization test.
* [ ] Ensure editor follows `plan.md` rule that commands are replayable, serializable, and diagnosable. ([GitHub][1])

## 22. File dialogs and asset flow

* [ ] Add Qt file dialog only in Qt editor layer.
* [ ] Keep engine filesystem abstraction independent.
* [ ] Add Open Scene action.
* [ ] Convert selected path to engine virtual/absolute path.
* [ ] Validate scene before loading.
* [ ] Report scene parse errors in diagnostics dock.
* [ ] Add recent files list.
* [ ] Add recent scene persistence.
* [ ] Add Export PNG dialog.
* [ ] Add Export EXR dialog.
* [ ] Add Export Benchmark JSON dialog.
* [ ] Add Open Artifact Folder action.
* [ ] Add drag-and-drop scene file support later.
* [ ] Add drag-and-drop texture support later.
* [ ] Add asset browser later.
* [ ] Add missing-asset dialog later.
* [ ] Add asset import progress later.
* [ ] Keep asset import lifecycle aligned with `plan.md`: load assets, validate scene, instantiate runtime, schedule uploads. ([GitHub][1])

## 23. Clipboard, filesystem, time

* [x] Implement `QtClipboard` using Qt clipboard APIs.
* [x] Preserve existing text clipboard semantics.
* [ ] Add copy diagnostics action.
* [ ] Add copy benchmark summary action.
* [ ] Add copy scene hash action.
* [ ] Add copy shader hash action.
* [ ] Decide whether `QtFileSystem` is needed or existing `DesktopFileSystem` is enough.
* [ ] Recommended first: keep existing filesystem unless Qt-specific paths are needed.
* [ ] Add Qt standard paths later for config/cache/artifacts.
* [ ] Implement `QtTimeSource` using `QElapsedTimer` or existing chrono source.
* [ ] Recommended first: keep existing chrono source to minimize change.
* [ ] Add Qt timer only for GUI frame pump.
* [ ] Ensure time source remains deterministic-safe for benchmark.
* [ ] Ensure GUI wall-clock time does not influence strict deterministic mode.
* [ ] Add tests for clipboard if feasible.
* [ ] Add tests for file read under Qt-enabled build.
* [ ] Add tests for time monotonicity.

## 24. Native handle and embedding decisions

* [ ] Prefer Qt-created native viewport over embedding the old raw Win32 window.
* [ ] Avoid `QWindow::fromWinId` as the normal migration path.
* [ ] Use `QWindow::fromWinId` only for temporary experiments embedding the existing raw window.
* [ ] Document that Qt allows wrapping a foreign native window, but warns that manipulating/observing such windows is platform-dependent and untested. ([Qt Documentation][7])
* [ ] If experimenting with embedding raw Win32 window, isolate it in `experiments/qt_embed_raw`.
* [ ] Do not ship foreign-window embedding as the long-term editor architecture.
* [ ] If using `QWindow` viewport, embed it into `QMainWindow` via `QWidget::createWindowContainer`.
* [ ] Document `createWindowContainer` limitations before using it broadly.
* [ ] If using `QWidget` viewport, force native widget handle only on viewport.
* [ ] Confirm Windows `WId` maps to `HWND` for the embedding/native-handle path. Qt’s window embedding docs list Windows `WId` as `HWND`. ([Qt Documentation][8])
* [ ] Confirm macOS handle type before Metal work.
* [ ] Confirm X11 handle type before Linux Vulkan work.
* [ ] Confirm Wayland limitations before Linux Wayland work.
* [ ] Add per-platform compile guards.
* [ ] Add per-platform runtime diagnostics.
* [ ] Add fallback error for unsupported Qt/native surface combination.

## 26. Tests

* [ ] Add compile test: Qt disabled.
* [ ] Add compile test: Qt enabled.
* [ ] Add compile test: Qt enabled + CPU ray tracer.
* [ ] Add compile test: Qt enabled + D3D12.
* [ ] Add compile test: Qt enabled + benchmark.
* [x] Add runtime smoke: `ptapp --version --json`.
* [x] Add runtime smoke: `ptapp --doctor`.
* [x] Add runtime smoke: `ptapp --headless`.
* [x] Add runtime smoke: `ptapp --render --backend cpu`.
* [x] Add runtime smoke: `ptapp --window --platform qt --frames 5 --exit`.
* [ ] Add Qt event translation unit tests.
* [ ] Add key event translation test.
* [ ] Add mouse move translation test.
* [ ] Add mouse button translation test.
* [ ] Add wheel translation test.
* [ ] Add resize translation test.
* [ ] Add close translation test.
* [ ] Add menu command translation test.
* [ ] Add framebuffer upload test.
* [ ] Add QImage format test.
* [ ] Add black-frame detection test.
* [ ] Add first-frame nonblack test with CPU render.
* [ ] Add DPI size conversion test.
* [ ] Add native handle non-null test where platform supports it.
* [ ] Add shutdown-order test.
* [ ] Add render-thread stop test.
* [ ] Add benchmark-no-Qt test.
* [ ] Add scene-load-through-Qt-window test.
* [ ] Add screenshot/export action test later.
* [ ] Add editor command serialization test later.
* [ ] Add CI job with `PT_ENABLE_QT=OFF`.
* [ ] Add CI job with `PT_ENABLE_QT=ON` if CI image has Qt.
* [x] Add local Windows test script.
* [x] Add local Qt deployment smoke script.
* [ ] Add manual QA checklist for resize/focus/input.

## 27. Packaging and deployment

* [ ] Decide whether Qt is provided by system install, vcpkg, Conan, or manual Qt installer.
* [ ] Document Windows Qt setup.
* [ ] Document Linux Qt setup.
* [ ] Document macOS Qt setup later.
* [ ] Add `QT_PLUGIN_PATH` diagnostic.
* [ ] Add runtime diagnostic for missing platform plugin.
* [ ] Add Windows deployment script.
* [ ] Add `windeployqt` integration later.
* [ ] Add artifact layout for Qt build.
* [ ] Add note that headless build does not require Qt deployment.
* [ ] Add release packaging split: CLI/headless package and GUI package.
* [ ] Add license/deployment policy review.
* [ ] Add third-party notices path.
* [ ] Add CI packaging smoke later.
* [ ] Add start-menu/icon metadata later.
* [ ] Add app icon later.
* [ ] Add macOS bundle metadata later.
* [ ] Add Linux desktop file later.
* [ ] Add crash-log location docs.
* [ ] Add artifact output location docs.

## 28. Documentation updates

* [x] Update README build presets table with Qt presets.
* [x] Add Qt prerequisites.
* [x] Add `--platform qt` examples.
* [x] Add CPU preview example.
* [x] Add D3D12 Qt preview example.
* [x] Add troubleshooting section: missing Qt platform plugin.
* [x] Add troubleshooting section: black viewport.
* [x] Add troubleshooting section: stale native handle.
* [x] Add troubleshooting section: high-DPI mismatch.
* [x] Add troubleshooting section: render thread not stopping.
* [x] Add troubleshooting section: D3D12 device removed.
* [x] Add architecture note: Qt is platform/editor shell. README currently explains that `--platform` controls the shell only, separate from `--backend`.
* [x] Add architecture note: benchmark remains headless.
* [x] Add architecture note: renderer backends do not own scene identity.
* [x] Add architecture note: scene/material code must not include Qt.
* [x] Add `docs/qt_port.md` migration checklist.
* [x] Add `docs/qt_native_surface.md`.
* [x] Add `docs/qt_threading.md`.
* [x] Add `docs/qt_diagnostics.md`.
* [ ] Add `docs/editor_shell.md` later.
* [ ] Add screenshots once UI is stable.

## 29. Cleanup and refactoring

* [ ] Split current monolithic `main.cpp` run modes.
* [ ] Extract CLI parsing into `AppConfig`.
* [ ] Extract headless render path into `RunHeadlessRender`.
* [ ] Extract raw window path into `RunRawWindow`.
* [ ] Extract Qt window path into `RunQtWindow`.
* [ ] Extract benchmark path into `RunBenchmark`.
* [ ] Extract doctor path into `RunDoctor`.
* [ ] Extract crash-test path.
* [ ] Extract UI runtime state update.
* [ ] Extract title/status formatting.
* [ ] Extract menu model creation.
* [ ] Extract render preview controller.
* [ ] Extract background render thread controller.
* [ ] Extract framebuffer handoff class.
* [ ] Remove Win32 includes from app main where possible.
* [ ] Move raw Win32 menu code into raw platform module.
* [ ] Move Qt menu code into Qt editor module.
* [ ] Delete duplicated key/mouse mapping.
* [ ] Add shared input normalization tests.
* [x] Add shared platform factory.
* [ ] Add `CreatePlatform(config)` function.
* [ ] Add `CreateWindowShell(config)` only if needed.
* [x] Add compile guards around raw platform.
* [x] Add compile guards around Qt platform.
* [ ] Remove temporary trace spam after Qt preview is stable.
* [ ] Keep useful black-canvas diagnostics behind flag.

## 30. Acceptance gates

### Gate A — Build contract

* [x] `PT_ENABLE_QT=OFF` build still works.
* [x] `PT_ENABLE_QT=ON` build works.
* [x] `--version --json` reports Qt support.
* [x] `--doctor` works without opening a Qt window.
* [x] `--render` works without opening a Qt window.
* [x] No Qt headers appear in scene/material/pathtracer/benchmark core.

### Gate B — Minimal Qt window

* [x] `ptapp --window --platform qt` opens a Qt window.
* [ ] Window title shows project/build info.
* [x] Close button exits cleanly.
* [x] Resize events are logged.
* [x] Focus events are logged.
* [x] Keyboard events are logged.
* [x] Mouse events are logged.
* [ ] No crash on minimize/restore.
* [ ] No crash on rapid resize.
* [x] No crash on close during startup.

### Gate C — CPU preview parity

* [ ] Qt viewport displays CPU-rendered Cornell frame.
* [ ] Progressive accumulation updates visibly.
* [ ] Overlay/status text is readable.
* [ ] Resize resets accumulation.
* [x] Camera input works. Qt viewport supports right/middle-drag orbit, mouse-wheel dolly, `F` FPS mode, and FPS WASD/QE movement.
* [ ] Menu command reaches engine command queue.
* [ ] First non-black frame is logged.
* [ ] CPU preview works on high-DPI display.
* [x] Shutdown joins render thread cleanly.

### Gate D — Native D3D12 viewport

* [x] Qt viewport exposes valid `HWND`.
* [ ] D3D12 swapchain creates against Qt viewport.
* [ ] D3D12 frame presents into Qt viewport.
* [ ] Resize recreates swapchain.
* [ ] Device lost path logs correctly.
* [ ] WARP fallback works.
* [ ] Shader compile errors show in logs/diagnostics.
* [ ] No Qt repaint flicker over swapchain.
* [ ] Close destroys swapchain before widget handle dies.
* [ ] Existing protopt visual behavior is matched.

### Gate E — Editor shell

* [x] QMainWindow exists.
* [x] Viewport is central widget.
* [x] Menu bar works.
* [x] Status bar works. `StatusBarModel` is projected into a native Qt `QStatusBar`.
* [x] Diagnostics dock works. Native Qt dock shows live runtime, selection, and tracer diagnostics.
* [x] Scene graph dock shows current scene. Native Qt dock shows entity names, stable IDs, and component summaries.
* [x] Inspector dock shows selected object read-only. Native Qt dock shows selected entity, bounds, transform, material, light, and camera fields.
* [ ] Render settings panel can reset accumulation. Native read-only panel exists; reset action/control is still pending.
* [x] Benchmark panel can create a benchmark run descriptor. Native Qt dock shows the live default `BenchmarkRunDesc`; launch flow is pending.
* [ ] Editor commands are deferred through command buffer.

### Gate F — Benchmark safety

* [ ] Headless benchmark works in Qt-enabled build.
* [ ] Benchmark does not instantiate QApplication.
* [ ] Benchmark JSON schema unchanged.
* [ ] Benchmark image hash unchanged for CPU reference.
* [ ] GUI-launched benchmark freezes scene before run.
* [ ] GUI-launched benchmark records clear metadata.
* [ ] GUI-launched benchmark does not mix UI repaint time into renderer timing.
* [ ] CI benchmark target remains Qt-free by default.

---

# Highest-priority implementation order

Do this in this exact order:

1. **Add `PT_ENABLE_QT` and build a blank Qt window.**
2. **Implement `QtPlatform`, `QtWindow`, `QtInput`, and `QtNativeSurfaceProvider`.**
3. **Make `ptapp --window --platform qt` open/close/resize cleanly.**
4. **Add CPU framebuffer blit into a `QWidget` viewport.**
5. **Wire camera/input events through the existing normalized input system.**
6. **Add Qt menu/status bar without scene mutation side effects.**
7. **Only then add D3D12 native-surface presentation.**
8. **Only after D3D12 works, build the larger editor shell.**

The most important rule: **Qt owns widgets, layout, menus, docks, dialogs, and GUI event delivery. The engine owns lifecycle, modes, scene state, commands, rendering contracts, benchmarks, and diagnostics.**

[1]: https://github.com/monstercameron/vkPathTracer/blob/main/plan.md "vkPathTracer/plan.md at main · monstercameron/vkPathTracer · GitHub"
[2]: https://github.com/monstercameron/vkPathTracer/tree/main/experiments/protopt "vkPathTracer/experiments/protopt at main · monstercameron/vkPathTracer · GitHub"
[3]: https://doc.qt.io/qt-6/cmake-get-started.html "Getting started with CMake | Build with CMake | Qt 6.11.0"
[4]: https://doc.qt.io/qt-6/qrhiwidget.html "QRhiWidget Class | Qt Widgets | Qt 6.11.0"
[5]: https://doc.qt.io/qt-6/qwidget.html "QWidget Class | Qt Widgets | Qt 6.11.0"
[6]: https://doc.qt.io/qt-6/qvulkanwindow.html "QVulkanWindow Class | Qt GUI | Qt 6.11.0"
[7]: https://doc.qt.io/qt-6/qwindow.html "QWindow Class | Qt GUI | Qt 6.11.0"
[8]: https://doc.qt.io/qt-6/qtdoc-demos-windowembedding-example.html "Window Embedding Example | Qt 6.11"
