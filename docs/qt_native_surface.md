# Qt Native Surface Contract

The Qt native surface path exists so GPU backends can present into a Qt-owned viewport. Qt remains the shell; D3D12/Vulkan/Metal remain renderer backends.

## Surface ownership

Qt owns the widget or window. The backend borrows a native handle.

The surface provider should expose:

- native window handle
- native instance handle where the backend requires one
- logical viewport size
- physical framebuffer size
- DPI scale
- surface generation
- validity flag

On Windows, the D3D12 swapchain path expects an `HWND`. Qt's `WId` maps to a platform native window identifier; validate and cast only behind Windows-specific compile guards.

## QWidget versus QWindow

Preferred first path:

- use a Qt-created viewport
- force a native handle only on the viewport if required
- do not embed the old raw Win32 window as the long-term path

`QWindow::fromWinId` can be useful for experiments, but it wraps a foreign native window and has platform-dependent behavior. Do not ship it as the main editor architecture.

If using a `QWindow` viewport inside widgets, isolate the `QWidget::createWindowContainer` behavior and document focus, resize, stacking, and platform limitations before relying on it broadly.

## D3D12 swapchain flow

Expected order:

1. Qt creates and shows the viewport.
2. Platform updates logical size, physical size, DPI scale, and native handle.
3. Surface provider reports a valid handle.
4. D3D12 backend creates device resources.
5. D3D12 backend creates swapchain using the borrowed native handle.
6. Engine submits render work.
7. Backend presents to the swapchain.
8. Qt does not paint over the native swapchain surface.
9. Resize invalidates the old swapchain dimensions.
10. Backend drains GPU work, resizes or recreates swapchain, then resumes presentation.
11. Close stops render work, destroys swapchain, then lets Qt destroy the widget.

## CPU preview flow

CPU preview does not use native presentation.

Expected order:

1. Engine renders or accumulates into an RGBA8 preview buffer.
2. Qt shell copies the completed frame or swaps an owned frame buffer.
3. Qt schedules repaint on the GUI thread.
4. `paintEvent` draws the most recent complete image.
5. No render work is performed inside `paintEvent`.

The CPU preview path is the baseline for black-viewport diagnosis because it removes swapchain and native handle variables.

## DPI and size rules

Use these values distinctly:

- logical size: Qt layout and input coordinates
- physical size: render target and swapchain dimensions
- DPI scale: conversion between logical and physical sizes

High-DPI requirements:

- update physical dimensions when `devicePixelRatioF()` changes
- reset accumulation on physical size changes
- recreate native swapchain on physical size changes
- log logical size, physical size, and DPI scale together
- compare the render target size to the central viewport, not the whole top-level window

## Stale handle rules

The backend must not use a handle after:

- close has started
- the viewport widget was destroyed
- a surface generation changed
- swapchain recreation started and old surface state was invalidated

The platform should expose enough diagnostics to answer:

- is the handle null?
- what surface generation produced it?
- what viewport owns it?
- what logical/physical size was current when it was captured?
- was a close or resize in progress?

## Black viewport checks

For CPU preview:

- confirm frame byte count is at least `width * height * 4`
- confirm width and height are non-zero
- confirm alpha is opaque or the image format ignores alpha as expected
- confirm the first frame contains non-black pixels
- confirm `paintEvent` is called after frame delivery

For D3D12:

- confirm the native handle is valid at swapchain creation
- confirm the swapchain size matches physical viewport size
- confirm present succeeds
- confirm Qt is not clearing/painting over the viewport
- confirm the first non-black frame is logged after present
- confirm device-removed diagnostics are emitted on present failure

## Device removed handling

D3D12 device removal must produce actionable diagnostics:

- HRESULT from the failing call
- `GetDeviceRemovedReason()` result
- selected adapter name and LUID if available
- backend mode, for example hardware or WARP
- surface handle validity
- swapchain size
- last resize generation
- whether close/shutdown was in progress

After device removal, do not continue presenting into the same swapchain. Tear down surface-dependent resources in the normal shutdown order and report a clean failure or fallback.
