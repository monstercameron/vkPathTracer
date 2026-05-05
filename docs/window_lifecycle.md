# Window Lifecycle

Qt and raw desktop shells expose the same engine contract, but their lifecycles are different. The engine should see a stable platform interface either way.

## Raw desktop lifecycle

The raw desktop shell owns native window creation directly. On Windows this means the raw platform creates the window, processes native messages, exposes `HWND`, and blits preview frames through native drawing APIs.

Typical order:

1. parse config and select `--platform raw`
2. initialize raw platform
3. create native window
4. expose native handles
5. run engine frame loop
6. poll native events each frame
7. present or CPU-blit latest frame
8. on close, stop rendering and destroy native window

## Qt lifecycle

The Qt shell creates `QApplication` only for GUI runs that selected `--platform qt`. The current staged shell uses an engine-owned loop that pumps Qt events, which keeps the engine lifecycle explicit while the port is in progress.

Typical order:

1. parse config before creating Qt objects
2. select `--platform qt`
3. create `QApplication` only if one does not already exist
4. create the viewport widget/window
5. show the viewport and update metrics
6. expose native surface handles after the widget has a native handle
7. run the engine frame loop
8. call Qt event processing from platform polling
9. hand completed preview frames to the Qt viewport
10. on close, signal render stop
11. flush/destroy renderer resources that depend on the viewport
12. destroy Qt widgets
13. destroy `QApplication` only after Qt-owned objects are gone

Future editor mode may switch to `QApplication::exec()` with a timer-driven engine tick. That should be isolated behind a run-loop adapter so the engine phases do not move into arbitrary Qt signal callbacks.

## Headless lifecycle

Headless mode has no GUI event loop and no native surface:

1. parse config
2. select `--platform headless` or a non-window render path
3. initialize headless platform services
4. run diagnostics, render, validation, or benchmark
5. write artifacts
6. shutdown

Headless mode must not instantiate `QApplication`, load Qt platform plugins, or require Qt deployment.

## Resize and DPI

Qt reports logical widget sizes. Render targets need physical pixel sizes.

Rules:

- Use logical size for UI layout and input coordinates.
- Use physical size for framebuffer and swapchain dimensions.
- Compute physical size from logical size multiplied by `devicePixelRatioF()`.
- Store DPI scale in `WindowMetrics`.
- Reset accumulation when physical render size changes.
- Recreate swapchain on physical render size changes.
- Ignore or debounce zero-size/minimized transitions.
- Do not call recursive geometry-changing operations from resize handlers.

Moving a Qt window between monitors can change DPI without a traditional user resize. Treat that as a framebuffer-size change.

## Close and shutdown

Close is a multi-stage event, not just a widget callback.

Required behavior:

- emit close intent exactly once
- stop accepting new frame work
- request render-thread cancellation
- join render thread before widget destruction
- flush GPU work before native surface destruction
- release swapchain resources before the native handle dies
- prevent queued UI updates after shutdown starts

If the close button hides or destroys a widget before the renderer stops, the backend may hold a stale native handle. That is a platform bug.

## Stale handle prevention

Never cache native handles indefinitely. Cache a handle only together with surface generation and viewport lifetime state.

Reacquire or invalidate handles when:

- the viewport widget is destroyed
- the viewport is recreated
- the native surface is recreated
- a top-level window is reparented
- a DPI or platform transition forces surface recreation
- close/shutdown begins

Backends should validate that the handle is non-null immediately before swapchain creation.
