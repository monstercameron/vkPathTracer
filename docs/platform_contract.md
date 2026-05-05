# Platform Contract

The platform layer creates a shell around the engine. It does not define what a scene is, how a material is represented, how a renderer backend schedules work, or how benchmark results are measured.

## Platform responsibilities

Every platform implementation, including Qt, raw desktop, and headless, is responsible for:

- application/window initialization and shutdown
- event polling or event-loop integration
- normalized input event delivery
- window metrics, including logical size and DPI scale where available
- clipboard access where the platform supports it
- filesystem hooks only when platform-specific behavior is required
- time source integration for interactive shell timing
- native surface handles for presentable GPU backends

The platform layer may fail early with clear diagnostics if the requested shell is unavailable, for example `--platform qt` in a build without `PT_ENABLE_QT`.

## Engine responsibilities

The engine owns:

- application modes
- scene state and scene identity
- command buffers and replay rules
- renderer backend selection
- render settings
- benchmark descriptors, execution, and output schemas
- diagnostics categories and artifact writing
- deterministic scheduling policy

Qt widgets and signals must feed this engine model. They must not become an alternate engine model.

## Backend boundary

Renderer backends receive backend-neutral data plus platform surface descriptors. They must not receive Qt objects.

Allowed:

- native window handle such as `HWND` on Windows
- native instance handle where required
- logical viewport size
- physical framebuffer size
- DPI scale
- surface generation or resize generation
- backend capability descriptor

Not allowed:

- `QWidget*`
- `QWindow*`
- `QObject*`
- Qt event objects
- Qt image or painter objects as backend inputs
- scene/editor Qt model objects

CPU preview is the exception only in direction: the engine may hand a completed RGBA8 image to the Qt shell for display. Qt still does not own the render algorithm.

## Qt migration rules

- No Qt includes in `scene`, `materials`, `pathtracer`, `benchmark`, or backend-neutral `render/interface` code.
- No Qt types in public engine interfaces.
- Qt code may depend on platform interfaces and editor facades.
- Qt code may enqueue commands; engine phases decide when commands are applied.
- Qt signal handlers must stay thin. They translate UI intent into engine input or command objects.
- Qt diagnostics views read diagnostics; they do not define diagnostics categories.
- Qt file dialogs pass paths to engine loaders; asset import stays engine-owned.
- Qt benchmark panels create benchmark descriptors; benchmark execution remains harness-owned.
- The raw desktop shell remains available until Qt reaches parity.
- Headless mode is not a Qt shell mode.

## Headless and benchmark guardrails

The following commands must work in a Qt-enabled build without creating `QApplication`:

```sh
ptapp --doctor
ptapp --dump-config
ptapp --list-backends
ptapp --headless --platform headless
ptapp --render --backend cpu --scene assets/scenes/cornell_native.json --output render_out.png
```

Benchmark timing must not include Qt repaint time unless a future visual benchmark mode records it as a separate UI metric. Benchmark result schemas remain stable across raw, Qt-enabled, and headless builds.

## Surface lifetime rule

A native surface handle is a borrowed platform value. The backend may use it only while the platform reports that the viewport is alive and the surface generation has not changed.

Required order for native GPU presentation shutdown:

1. signal render stop
2. stop accepting new frame submissions
3. join render thread or complete the engine-owned frame loop
4. flush/wait idle on the GPU backend
5. destroy swapchain and surface-dependent resources
6. destroy Qt widgets/native surface
7. destroy `QApplication` if the app owns it

This order is mandatory for stale handle and device-removed prevention.
