# Qt Diagnostics and Troubleshooting

Qt diagnostics should make platform failures distinguishable from renderer failures. A missing platform plugin, a black CPU preview, a stale native handle, and a D3D12 device removal are different failures and need different evidence.

## Baseline commands

Run these before diagnosing the Qt shell:

```sh
ptapp --version --json
ptapp --dump-config
ptapp --doctor
ptapp --list-backends
ptapp --ui-model-smoke
ptapp --ui-release-gate --json
```

Qt-enabled builds should report Qt support without opening a window. Headless diagnostics must not require Qt runtime plugins.

`--ui-model-smoke` is the deterministic editor-model check. It validates menu command creation, selection state transitions and JSON roundtrip, panel layout save/load, asset drop rejection, benchmark descriptor/score models, shortcut conflicts, status bar projection, and release-gate serialization without requiring a GPU or window.

`--ui-release-gate --json` prints every G70 release-gate item with one of three states:

- `passed`: covered by a deterministic model or app-shell check
- `deferred`: explicitly not complete at runtime yet, with the missing binding named
- `pending`: a release-gate bookkeeping failure; this should be zero

For local no-Qt coverage against an existing build:

```powershell
powershell -NoProfile -File tools/ui_qt_smoke.ps1 -NoBuild -SkipQt -DisabledBuildDir build\presets\windows-clangcl-d3d12-debug
```

That smoke includes `ptapp --ui-release-gate --json` and requires `"pending_count":0`.

Qt smoke commands:

```sh
ptapp --window --platform qt --backend cpu --scene assets/scenes/cornell_native.json
```

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json
```

## Required Qt diagnostic fields

Startup diagnostics should include:

- `qt_enabled`
- `qt_version`
- `qt_platform_plugin`
- `qt_plugin_path`
- `qt_high_dpi_policy` or effective DPI behavior
- primary screen name
- selected platform shell
- selected render backend
- viewport logical size
- viewport physical size
- viewport device pixel ratio
- viewport native handle validity
- native surface generation
- first frame delivered
- first non-black frame
- first present
- display queue capacity
- display queue pending state
- latest display frame id
- produced display frame count
- consumed display frame count
- dropped display frame count
- stale display frame discard count
- coalesced UI update count
- close event received
- shutdown order milestones

Backend diagnostics should include adapter/device information independently of Qt diagnostics.

## Display frame handoff diagnostics

CPU preview uses a latest-wins display queue with capacity 1. Diagnostics should make that behavior visible so intentional frame dropping is not mistaken for lost render work.

Required counters:

- `display_queue_capacity`, expected to be `1`
- `display_update_pending`, true when one queued GUI-thread update is already scheduled
- `display_frames_produced`
- `display_frames_consumed`
- `display_frames_dropped`
- `display_frames_stale`
- `display_updates_coalesced`
- latest produced display frame id and generation
- latest consumed display frame id and generation
- UI-thread blit or image upload time

Interpretation:

- dropped display frames are expected when the render coordinator produces frames faster than Qt repaints
- coalesced updates are expected when several display frames arrive before the GUI thread runs
- stale display frames point to scene, camera, accumulation, resize, or shutdown generation changes
- queue capacity above 1 is a contract change and must be documented with the ownership model
- there should be no per-tile, per-pixel, per-sample, or per-ray Qt event counters because those events must not be posted

## Missing Qt platform plugin

Typical symptom:

- app exits before opening a window
- Qt reports that no platform plugin could be initialized
- Windows mentions `windows` platform plugin
- Linux mentions `xcb` or Wayland plugin

Checks:

- confirm the executable is from a Qt-enabled preset
- confirm the Qt kit matches the compiler ABI
- run from a Qt developer environment
- set `QT_DEBUG_PLUGINS=1` for verbose plugin loading
- set `QT_PLUGIN_PATH` to the Qt kit plugin directory for local testing
- on Windows deployment builds, run `windeployqt` against `ptapp.exe`
- on Linux, install Qt platform plugin packages for the active window system

Do not debug renderer backends until the Qt platform plugin loads and a blank Qt window can open.

## Black viewport

First split the problem into CPU preview or native GPU presentation.

CPU preview checks:

- selected backend is `cpu`
- scene path exists
- frame width and height are non-zero
- frame byte count is at least `width * height * 4`
- frame format is RGBA8
- alpha is opaque or ignored by the chosen `QImage` format
- frame delivery runs on the GUI thread or uses a queued GUI-thread update
- latest-wins display queue capacity is 1
- display frame drops and update coalescing are counted
- the Qt event queue is not receiving per-tile or per-pixel progress events
- `paintEvent` runs after frame delivery
- first non-black frame is logged

D3D12 checks:

- native handle is valid at swapchain creation
- swapchain uses physical viewport size
- present calls succeed
- Qt does not clear or repaint over the swapchain surface
- resize does not leave the swapchain at zero size
- device-removed reason is logged if present fails
- no CPU RGBA8 display frame is expected for native D3D12 presentation unless an explicit debug readback path is enabled

If CPU preview works but D3D12 is black, focus on native surface and swapchain state. If CPU preview is also black, focus on frame production and frame handoff.

## CPU blit versus native presentation

For CPU preview, the render coordinator owns scene, camera, accumulation, and RGBA8 display frame production. Qt owns only the viewport image and repaint. Expected evidence is a produced RGBA8 frame, a consumed latest-wins queue entry, a GUI-thread blit, and a subsequent paint.

For native D3D12 presentation, Qt owns the widget and native surface lifetime, while the D3D12 backend owns the swapchain and presents directly. Expected evidence is native handle validity, swapchain size, successful present, and no Qt repaint over the swapchain. A missing CPU RGBA8 display frame is not a D3D12 bug in this path.

## Stale native handle

Typical symptom:

- D3D12 swapchain creation or present fails after resize, close, reparent, or DPI move
- handle logged earlier was non-null, but the owning Qt widget was destroyed or recreated

Checks:

- log the native handle at capture time and at swapchain creation time
- log surface generation with every handle
- invalidate backend surface state on widget destruction
- reacquire handles after viewport creation/recreation
- do not cache `HWND` beyond the lifetime of the Qt viewport
- stop rendering before close destroys the viewport

If a handle is valid only before `show()`, delay native surface capture until after the widget is shown and native creation is forced.

## High-DPI mismatch

Typical symptom:

- image appears stretched, blurry, clipped, offset, or only partially updated
- mouse input does not line up with the rendered image
- resize produces black borders or repeated accumulation artifacts

Checks:

- log logical size from Qt
- log physical framebuffer size
- log `devicePixelRatioF()`
- use logical coordinates for input
- use physical coordinates for render target and swapchain
- reset accumulation on physical size changes
- recreate swapchain on physical size changes
- test 100%, 125%, and 150% display scale
- test moving the window between monitors with different DPI

Do not compare the render target against the top-level window if the viewport is inside a menu/status/dock layout. Use the actual viewport widget.

## Qt shell surface readiness

The bounded Qt smoke runs with the offscreen platform and does not click through
the GUI. To verify the editor shell without interaction, emit one startup log
line after the main window has installed its central viewport, menu bar, status
bar, and core docks.

Expected marker shape:

```text
qt shell ready menu_bar=true status_bar=true dock_count=8 docks=scene_graph,inspector,materials,lights,camera,render_settings,diagnostics,performance
```

`tools/ui_qt_smoke.ps1` reads the bounded window stdout/stderr logs and checks:

- `menu_bar=true`
- `status_bar=true`
- `dock_count` is at least the configured minimum
- each required dock id is present in the marker

This keeps the smoke non-invasive and compatible with `QT_QPA_PLATFORM=offscreen`.

## Render thread does not stop

Typical symptom:

- app hangs on close
- window disappears but process remains alive
- crash occurs after close because a queued frame update touches a destroyed widget

Checks:

- close event logs exactly once
- `renderStop` is set
- render worker is woken if blocked
- render worker join starts and completes
- GPU flush starts and completes
- queued UI frame updates are disconnected or generation-checked
- pending display queue slot is cleared or generation-invalidated
- coalesced UI updates cannot touch the destroyed viewport
- swapchain is destroyed before the Qt native handle dies

The fix belongs in lifecycle/threading ownership, not in the renderer algorithm.

## D3D12 device removed

Typical symptom:

- present, resize, or command submission fails with device removal
- viewport may go black before shutdown

Required diagnostics:

- failing HRESULT
- `GetDeviceRemovedReason()` result
- selected adapter and LUID if available
- backend mode, hardware or WARP
- feature level
- swapchain size
- viewport logical/physical size and DPI scale
- native handle validity
- whether resize or close was active
- last successful present frame index

Recovery policy for the staged shell:

- stop presenting immediately
- log the reason
- destroy swapchain and surface-dependent resources in shutdown order
- allow a clean error or WARP fallback when explicitly supported
- do not continue using the old swapchain

## Quick triage map

| Symptom | Most likely area |
|---------|------------------|
| Qt app cannot start | Qt setup/deployment/platform plugin |
| `--platform qt` rejected | build preset or `PT_ENABLE_QT` |
| CPU preview black | frame production or RGBA handoff |
| D3D12 preview black but CPU preview works | native surface, swapchain, or present |
| CPU preview responsive but drops frames | expected latest-wins throttling if counters increase and latest frames display |
| Qt event queue grows with render work | missing update coalescing or per-tile/per-pixel event spam |
| image size/input mismatch | DPI/logical versus physical size |
| crash/hang on close | render-thread stop or stale queued UI update |
| device removed | backend/device/swapchain failure, often amplified by resize/close ordering |
