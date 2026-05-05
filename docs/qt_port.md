# Qt Port Notes

This document is the operating guide for the staged Qt UI shell. The goal is to make Qt a platform/editor shell without turning Qt into a renderer dependency.

## Current contract

Qt is optional and compile-time gated by `PT_ENABLE_QT`. The raw desktop platform and headless platform remain valid paths while the Qt shell matures.

The intended first milestones are:

1. `ptapp --window --platform qt --backend cpu`
2. `ptapp --window --platform qt --backend d3d12`
3. editor panels, menus, diagnostics, and benchmark launch UI after the viewport contract is stable

Headless render and benchmark execution stay independent:

```sh
ptapp --render --backend cpu --scene assets/scenes/cornell_native.json --output render_out.png
ptapp --headless --platform headless
ptapp --doctor
ptapp --ui-model-smoke
ptapp --ui-release-gate --json
```

The UI model smoke is the current editor contract gate. It is intentionally headless: the same models must drive raw desktop, Qt, and future web/canvas shells. The release gate reports runtime UI gaps as explicit deferrals instead of silently treating model-only coverage as a finished widget.

Current deterministic UI contracts:

- `SelectionState` uses stable entity IDs and roundtrips selected IDs, hover state, active primary entity, aggregate bounds, and per-item bounds through JSON.
- `MenuBar` carries enabled state and disabled reasons so native/Qt/web shells can explain unavailable actions.
- `UiLayoutDocument` owns dock/floating/visibility/collapse/move/resize state and persists JSON layouts.
- `BenchmarkPanelModel` carries the run descriptor, raw metrics, workload cost, normalized score, calibration actions, result summary, and artifact location.
- `StatusBarModel` is derived from runtime state, selection state, and optional benchmark score.
- `UiReleaseGateItem` records passed, deferred, and pending release-gate evidence for the G70 checklist.

## Final ownership model

The Qt shell is a platform/editor shell. It owns Qt objects and user-facing presentation, but it does not own renderer state.

- UI thread owns `QApplication`, widgets, menus, dialogs, native Qt surfaces, repaint scheduling, and event translation.
- Render coordinator owns scene state, active camera state, scene command application, render resolution, accumulation buffers, accumulation reset policy, and sample/frame generations.
- CPU workers own ray batches while tracing. They return completed work to the render coordinator and never call Qt.
- CPU preview sends Qt only throttled, complete RGBA8 display frames.
- Display frame handoff is latest-wins with queue depth 1. If a newer frame arrives before Qt consumes the old one, the old frame is dropped and counted.
- Qt update callbacks are coalesced. There must not be one Qt event per tile, pixel, sample, or ray.
- Native D3D12 presentation bypasses the RGBA8 CPU blit path: Qt owns the surface lifetime, while the backend owns the swapchain and presents directly.

## Qt prerequisites

Install Qt 6 with Core, Gui, and Widgets. The CMake setup looks for `Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets` only when `PT_ENABLE_QT=ON`.

Windows setup notes:

- Use a Qt kit that matches the compiler ABI. For `clang-cl`, use the MSVC 64-bit Qt kit.
- Set `Qt6_DIR` to the kit's `lib/cmake/Qt6` directory, or configure from a Qt developer shell.
- For local deployment outside the Qt shell environment, run `windeployqt` against the built `ptapp.exe` or set `QT_PLUGIN_PATH` to the kit plugin directory.

Linux setup notes:

- Install Qt 6 development packages, including Widgets.
- Ensure the platform plugin for the active window system is installed, usually `xcb` for X11 or Wayland plugins for Wayland sessions.

macOS setup notes:

- Use a Qt 6 kit compatible with the selected compiler.
- Bundle/deployment rules are not finalized; keep command-line and benchmark packages separate from GUI packages.

## Qt presets

Use the existing CMake presets rather than hand-writing cache flags:

```sh
cmake --preset desktop-clang-qt-debug
cmake --build --preset desktop-clang-qt-debug --target ptapp
```

```sh
cmake --preset desktop-clang-qt-release
cmake --build --preset desktop-clang-qt-release --target ptapp
```

For the Windows D3D12 Qt path:

```sh
cmake --preset windows-clangcl-d3d12-qt-debug
cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp
```

```sh
cmake --preset windows-clangcl-d3d12-qt-release
cmake --build --preset windows-clangcl-d3d12-qt-release --target ptapp
```

`desktop-clang-benchmark` and `headless-benchmark-release` must remain Qt-free unless a future preset explicitly opts into GUI dependencies for a separate visual benchmark tool.

## CPU preview example

The CPU preview route is the first Qt viewport target because it does not require native swapchain presentation:

```sh
./build/presets/desktop-clang-qt-debug/bin/ptapp \
  --window \
  --platform qt \
  --backend cpu \
  --scene assets/scenes/cornell_native.json \
  --window-width 1280 --window-height 720 \
  --ui-present-hz 30
```

On Windows:

```powershell
.\build\presets\desktop-clang-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend cpu `
  --scene assets\scenes\cornell_native.json `
  --window-width 1280 --window-height 720 `
  --ui-present-hz 30
```

Expected shell behavior:

- Qt owns the widget and repaint scheduling.
- The render coordinator owns scene, camera, accumulation, and RGBA8 display frame production.
- CPU workers trace rays and report completed work to the render coordinator only.
- The Qt viewport copies or safely swaps the latest completed RGBA8 frame from a depth-1 latest-wins queue.
- If the queue slot is full, the older display frame is dropped and the dropped frame counter increments.
- Qt update callbacks are coalesced so one pending GUI-thread update can represent several produced frames.
- `--ui-present-hz` controls the CPU preview display publish cap; it defaults to 30 Hz and is clamped by runtime config.
- `paintEvent` blits the latest completed image only; it does not run expensive render work.
- The Qt event queue does not receive per-tile, per-pixel, per-sample, or per-ray progress events.
- Resize of the physical framebuffer resets accumulation.
- Left-click viewport picking emits editor selection commands and shows selected-object bounding boxes.
- Right or middle drag controls the orbit camera; `F` toggles FPS camera mode with keyboard movement.

## D3D12 Qt preview example

The D3D12 route uses Qt only to create and own the native viewport surface:

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json `
  --window-width 1280 --window-height 720
```

Expected shell behavior:

- The Qt viewport exposes a valid native window handle after the widget is created and shown.
- The D3D12 backend receives only the native handle and surface metrics, not Qt types.
- Swapchain creation, resize, present, and teardown remain backend-owned.
- The CPU RGBA8 display queue is not part of native D3D12 presentation unless an explicit debug readback path is added.
- Qt painting is disabled or constrained so it does not paint over the D3D12 swapchain.
- On close, render submission stops and GPU work is flushed before the Qt native surface is destroyed.

If the command fails because the native handle is null, stale, or unsupported, treat that as a platform contract failure. See [`qt_native_surface.md`](qt_native_surface.md) and [`qt_diagnostics.md`](qt_diagnostics.md).

## Architecture notes

Qt may own:

- `QApplication` on GUI runs only
- windows, widgets, menus, docks, dialogs, and status bars
- GUI event delivery and translation into engine input events
- native surface exposure for presentable GPU backends
- viewport-owned CPU blit image used for RGBA8 preview display
- GUI diagnostics views

Qt must not own:

- scene identity or scene mutation rules
- active camera authority
- accumulation buffers, sample counters, or reset policy
- CPU ray work or tile scheduling
- display frame production cadence
- renderer backend object lifetime
- benchmark timing or benchmark output schema
- material descriptors or backend-neutral render interfaces
- command replay, serialization, or determinism policy

Benchmark and strict validation runs must not instantiate `QApplication`.

## Migration rules

- No Qt includes in scene, materials, path tracer, benchmark, or backend-neutral render interfaces.
- Qt code may call engine facades and submit engine commands; it must not mutate scene objects directly from Qt signal handlers.
- Qt signal handlers translate user intent into engine commands; the render coordinator applies those commands.
- CPU workers never call Qt, post Qt events, or own display frame buffers.
- CPU preview frame delivery uses a latest-wins queue with depth 1 and explicit dropped/stale/coalesced counters.
- No per-tile, per-pixel, per-sample, or per-ray Qt events are allowed.
- Renderer backends receive native handles and capability descriptors only.
- Native handles are valid only while the owning Qt viewport exists.
- Editor commands must remain serializable, replayable, and diagnosable.
- GUI-launched benchmark work creates an engine-owned benchmark descriptor; the benchmark harness still runs as the authority.
- `--platform` selects the shell. `--backend` selects rendering.
- `--render`, `--headless`, `--doctor`, and benchmark CI paths stay usable in Qt-enabled builds without launching a Qt window.

## Migration checklist

1. Keep raw/headless paths building with `PT_ENABLE_QT=OFF`.
2. Configure Qt only from Qt presets or `PT_ENABLE_QT=ON`.
3. Bring up a Qt window that opens, resizes, and closes cleanly.
4. Add CPU RGBA preview blit.
5. Enforce latest-wins queue depth 1, dropped display frame counters, stale generation counters, and coalesced UI updates.
6. Confirm CPU workers publish no per-tile, per-pixel, per-sample, or per-ray Qt events.
7. Add normalized input/event translation.
8. Add native surface exposure and D3D12 swapchain presentation.
9. Add diagnostics for Qt version, platform plugin, surface handle, logical size, physical size, DPI scale, first non-black frame, display queue counters, and shutdown order.
10. Add editor widgets after viewport lifecycle and threading are stable.
