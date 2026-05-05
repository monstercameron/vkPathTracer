# Qt Threading Contract

Qt widgets are GUI-thread objects. The renderer may run work elsewhere, but it cannot mutate Qt widgets from a worker thread.

## Final ownership model

The Qt shell is a display and input shell. It is not a scene owner, render coordinator, or accumulation owner.

- UI thread owns `QApplication`, widgets, menus, dialogs, native Qt surfaces, repaint scheduling, and event translation.
- Render coordinator owns scene state, scene command application, active camera state, render resolution, accumulation buffers, accumulation reset policy, and sample/frame generation counters.
- CPU worker threads own ray batches while executing path tracing work. They return completed work to the render coordinator; they do not call Qt and they do not publish UI events.
- Qt receives display data only as throttled, complete RGBA8 preview frames for the CPU blit path.
- Native D3D12 presentation does not send RGBA8 frames through Qt. Qt owns the viewport surface; the D3D12 backend owns swapchain creation, resize, present, and teardown against the native handle.

This keeps Qt signal callbacks from becoming engine phases and keeps renderer progress independent from GUI repaint pacing.

## Latest-wins display queue

CPU preview frame delivery uses a latest-wins queue with depth 1.

The queue slot contains:

- RGBA8 pixels for one complete display frame
- logical and physical size
- display frame id
- scene/camera generation
- accumulation generation
- production timestamp

Rules:

- the render coordinator is the only producer of display frames
- the Qt UI thread is the only consumer of display frames
- if the slot is full, the producer replaces the old frame and increments the dropped display frame counter
- replacing a frame is not an error; it is the expected overload behavior
- queued GUI work is coalesced so at most one frame-update callback is pending for the viewport
- the UI thread discards stale generations and records the discard
- CPU workers never enqueue per-tile or per-pixel Qt events
- progress counters may be sampled or logged at frame cadence, but they must not spam the Qt event queue

## Frame handoff

Recommended CPU preview handoff:

1. CPU workers finish ray work for the current batch and return results to the render coordinator
2. render coordinator updates accumulation and produces a complete RGBA8 display frame at the configured display cadence
3. frame is copied into or swapped into the single latest-wins queue slot
4. if no UI update is pending, the coordinator posts one queued GUI-thread callback
5. GUI thread drains the latest queue slot and updates the viewport-owned image
6. viewport schedules repaint once
7. `paintEvent` blits the latest complete image
8. dropped, stale, and coalesced update counters are updated

Rules:

- never write into a `QImage` owned by the viewport from a worker thread
- never call `QWidget::update`, `resize`, `show`, `hide`, or `close` from a worker thread
- do not hold a framebuffer mutex while painting if the renderer might block on the same mutex
- drop old preview frames instead of queueing unbounded work
- use a frame generation counter to detect stale frame consumption
- post viewport updates at display-frame cadence, not tile completion cadence
- do not enqueue one Qt event per ray, pixel, sample, or tile
- do not let the GUI thread block CPU workers during normal preview pacing
- keep the queue depth at 1 unless the ownership model is revised and documented

## CPU blit versus native D3D12 presentation

CPU preview and native D3D12 presentation are different display paths.

CPU preview path:

- render coordinator owns accumulation and converts the current display state to RGBA8
- Qt receives only the latest complete RGBA8 frame through the depth-1 queue
- GUI thread owns the `QImage` or equivalent viewport image
- `paintEvent` performs a CPU blit of the latest complete image
- dropped display frames are acceptable when rendering outpaces repaint

Native D3D12 path:

- Qt owns the viewport widget and native surface lifetime
- backend receives the native handle and physical surface metrics
- backend owns swapchain, render targets, command submission, resize, present, and teardown
- Qt does not receive per-frame RGBA8 pixels for presentation
- Qt painting must be disabled or constrained so it cannot clear over the swapchain surface
- diagnostics still report present count, resize generation, device removal state, and shutdown order

## Render-thread stop

Close and shutdown must stop render work before Qt surface destruction.

Required order:

1. set `windowClosing`
2. set `renderStop`
3. stop accepting new scene/settings changes for that viewport
4. wake any sleeping render worker
5. join render worker
6. flush/wait idle on GPU backend if one is active
7. destroy swapchain or surface-dependent GPU resources
8. clear or generation-invalidate the display queue and pending queued UI frame updates
9. destroy Qt viewport widgets

If a queued frame update runs after the widget is destroyed, that is a lifecycle bug. Use weak ownership, generation checks, or disconnects during shutdown.

## D3D12 threading

For native D3D12 presentation:

- command recording and GPU submission may be backend-owned
- swapchain resize and destroy must be serialized with presentation
- close must prevent new presents before the handle is invalidated
- device-removed paths must stop the render loop before destroying Qt widgets
- Qt GUI thread should not block indefinitely waiting on GPU work during normal frame pacing

Long GPU waits are acceptable only during controlled shutdown or explicit device recovery.

## Event-loop interaction

Two run-loop models are acceptable, but they must be isolated:

- phase 1: engine-owned loop calls Qt event processing from platform polling
- phase 2: Qt `exec()` drives a timer that calls an engine tick adapter

In either model:

- engine phases remain explicit
- render preparation does not run inside arbitrary Qt signal callbacks
- command application remains engine-owned
- benchmark and headless runs do not use the Qt event loop

## Diagnostics

Threading diagnostics should include:

- render thread started/stopped
- close event received
- render stop requested
- render thread joined
- GPU flush started/completed
- display queue capacity
- pending queued UI update state
- dropped display frame count
- stale display frame discard count
- coalesced UI update count
- frame upload time on GUI thread
- frame production time on render thread

These logs make render-thread stop failures diagnosable instead of intermittent.
