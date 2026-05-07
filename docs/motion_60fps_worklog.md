# Motion and Preview FPS Worklog

Date: 2026-05-07

## Goal

Keep viewport interaction, polygon-visible motion, and animation updates running at a configurable target rate, defaulting to 60 FPS, even when path-tracing rays or DXR dispatches are too expensive to finish at that cadence.

The motivating evidence is a D3D12 DXR viewport screenshot showing about 27 FPS while the path tracer is still accumulating samples. The target is not necessarily 60 new path-traced samples per second; it is 60 Hz scene interaction and motion presentation, with path tracing allowed to progress asynchronously or at a lower sample cadence.

## Working Hypothesis

The current application has a `--ui-present-hz` setting, but it defaults to 30 Hz and appears to throttle preview publication. The fix likely needs two pieces:

1. Make the UI/motion target rate explicit, default to 60 Hz, and keep it configurable.
2. Prevent expensive path-tracing sample dispatch/readback from owning the frame cadence used for scene motion and viewport presentation.

## Evidence and Attempts

### Attempt 1 - Code search

Search targets:

- Qt render loop and FPS reporting.
- `ui_present_hz` configuration.
- D3D12/DXR sample dispatch and readback cadence.
- Render coordinator publish throttling.

Initial observations:

- `run.ps1` exposes `-UiPresentHz`, but examples still use 30 Hz.
- `Config.h` defaults `ui_present_hz` to 30.
- `RenderCoordinatorConfig::publish_hz` also defaults to 30.
- README describes `--ui-present-hz` as the preview present rate, but defaults are not aligned with the requested 60 FPS target.

### Attempt 2 - First decoupling patch

Changes made:

- Changed the runtime `ui_present_hz` default from 30 to 60.
- Changed `RenderCoordinatorConfig::publish_hz` default from 30 to 60.
- Updated CLI/docs text so `--ui-present-hz` is described as the UI/motion and preview publish rate.
- Moved D3D12 and Vulkan Qt preview renderers onto the existing background `RenderCoordinator` path.
- Changed the initial Qt background decision to use the tracer factory's `background` flag instead of only checking for `TiledCpuPathTracer`.
- Changed the Qt interactive frame target from a hard-coded 16 ms constant to a value derived from `--ui-present-hz`.
- Raised the Qt viewport repaint check timer from a 16 ms interval to 8 ms so dirty repaint requests are not capped below high present targets.
- Reduced Qt event polling from a 16 ms budget per loop to a 1 ms budget so event processing and render-loop sleeping do not double-throttle the UI toward 30 FPS.
- Added a scoped Windows 1 ms timer-resolution request for window mode so `std::this_thread::sleep_for` can pace near 16.6 ms instead of rounding toward 31 ms on default system timer granularity.
- Replaced the Qt loop's single remaining-duration sleep with a tight timestamp wait after both `Sleep(1)` and `yield()` still overslept below the requested cadence on the test machine. This trades one busy CPU core during the preview wait for accurate interactive pacing.
- Changed the UI frame-time value used by overlays/status panels to report frame-to-frame cadence instead of only pre-wait CPU work time.
- Added a Qt render-loop frame profile log with measured loop elapsed time and per-section averages for validation.
- Removed smooth pixmap scaling from the Qt viewport paint path after D3D12 validation showed the remaining frame cost was Qt poll/paint, not render or physics.
- Split UI/motion cadence from heavy path-traced framebuffer uploads by limiting background framebuffer submissions to at most 30 Hz while the UI loop still targets `--ui-present-hz`.
- Adjusted pacing to skip the final wait when the UI frame has already consumed more than half the target budget; D3D12 validation showed near-budget frames were being pushed below 60 by scheduler slippage during the final wait.
- Changed physics motion transform publishing to allow dynamic acceleration updates only, and removed the full scene reload fallback from the physics motion path.

Expected behavior:

- With D3D12/DXR, a slow `render_sample_batch` GPU fence should no longer block Qt event processing because it runs on the coordinator thread.
- The UI/motion cadence should target `1000000 / ui_present_hz` microseconds.
- Moving objects through physics should no longer trigger a hidden full-scene reload when dynamic transform updates are unavailable.

Open verification:

- Build the Qt/D3D12 target.
- Run a short Qt smoke window with `--ui-present-hz 60`.
- Test a moving scene or gizmo/physics motion and confirm the overlay UI FPS is near 60 while path tracing may publish fewer completed samples.

### Validation results

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes. Vulkan emits existing missing-`pNext` initializer warnings.
- 60 Hz null/no-physics smoke: `--backend null --scene assets\scenes\material_gauntlet.json --frames 120 --ui-present-hz 60` measured `elapsed_loop_ms=1997.216300`, `avg_total_frame_ms=16.643469`.
- 30 Hz configurable smoke: `--backend null --scene assets\scenes\material_gauntlet.json --frames 90 --ui-present-hz 30` measured `elapsed_loop_ms=2960.827400`, `avg_total_frame_ms=32.898082`.
- D3D12-DXR dynamic physics smoke: `--backend d3d12-dxr --frames 600 --ui-present-hz 60` measured `elapsed_loop_ms=10400.968400`, `avg_total_frame_ms=17.334947`, with a one-time startup/reveal max frame of `669.610000` ms. Excluding that startup frame, steady-state average is about `16.25` ms per frame.
- The D3D12-DXR run used the background render coordinator and reported `render_published=1092`, `render_dropped=905`, `window_received=186`, `window_presented=186`, `window_dropped=0`, confirming path-traced frame delivery can fall behind while the UI loop continues.
- A compile error in the script incremental transform path used an undeclared `frameIndex`; it was corrected to `qtFrameCount`.

Remaining risk:

- The Qt path-traced framebuffer display is intentionally capped below the UI loop to protect motion/input cadence. The path-traced image may update at a lower cadence than polygon overlays and editor interaction when D3D12 presentation is expensive.

### Attempt 3 - Polygon preview path

New request:

- Research and fix polygon rendering as well as UI pacing.
- Keep polygon-visible motion and animation at 60 FPS.
- Make the target frame rate configurable inside the Camera dock.

Additional findings:

- The Qt viewport previously rendered only the latest path-traced `QImage`, then selection/gizmo vector overlays.
- `ViewportPickable` contains triangle data, but it is rebuilt on a delayed path and is primarily for picking/collision.
- The runtime `RTSceneData` already has enough instance, local-vertex, index, material, and transform metadata to draw a fast editor polygon preview without rebuilding the BVH.
- Background transform updates were posted to the render coordinator, but the app-side `qtScene` metadata mirror was not updated on the background path. A polygon preview drawn from app-side scene metadata would therefore risk showing stale dynamic instance transforms unless that mirror was updated.

Changes made:

- Added `QtPolygonPreviewFrame` and `QtPolygonPreviewTriangle` to the Qt platform API.
- Added a Qt viewport paint layer that draws projected filled/wireframe polygons over the latest path-traced image and under selection/gizmo overlays.
- Built the polygon preview directly from `RTSceneData` instances every UI frame:
  - Static instances use world-space runtime vertices/indices.
  - Dynamic instances use local vertices/indices plus current `RTInstance` transform metadata.
  - Materials provide the base polygon color; selected entities get a highlight.
  - The preview has a 6000-triangle budget and proportional sampling for larger scenes so a huge mesh cannot force a full CPU raster pass every frame.
- Mirrored background instance transform updates into the app-side `qtScene` metadata before posting them to the render coordinator. This keeps polygon preview, selection bounds, and physics-facing scene metadata current without requiring a BVH rebuild.
- Added `RenderCoordinator::set_publish_hz()` so the Camera dock can change the preview cadence without restarting the render worker.
- Added Camera dock control `camera.preview.fps` labeled `Target FPS`, backed by `config.ui_present_hz` and clamped to 1..120 FPS.
- Added polygon preview counts to the render-loop profile and shutdown metadata so validation distinguishes UI-only pacing from active scene-polygon drawing.

Validation results after polygon patch:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke: `ptapp.exe --ui-model-smoke --platform headless --console --no-env-file` passes, including the Camera dock preview-FPS slider contract.
- 60 Hz null polygon smoke: `--backend null --scene assets\scenes\material_gauntlet.json --frames 120 --ui-present-hz 60` measured `elapsed_loop_ms=1994.887200`, `avg_total_frame_ms=16.624060`, `polygon_source_triangles=2`, `polygon_drawn_triangles=2`.
- 30 Hz configurable polygon smoke: `--backend null --scene assets\scenes\material_gauntlet.json --frames 90 --ui-present-hz 30` measured `elapsed_loop_ms=2965.882200`, `avg_total_frame_ms=32.954247`, `polygon_source_triangles=2`, `polygon_drawn_triangles=2`.
- D3D12-DXR dynamic physics, 600 frames: `--backend d3d12-dxr --frames 600 --ui-present-hz 60` measured `avg_total_frame_ms=18.130818` with `max_frame_ms=537.047900`, `polygon_source_triangles=48`, `polygon_drawn_triangles=48`, `window_dropped=0`. This short run includes startup/reveal cost and did not amortize to 60 FPS.
- D3D12-DXR dynamic physics, 1200 frames: `--backend d3d12-dxr --frames 1200 --ui-present-hz 60` measured `elapsed_loop_ms=19939.368300`, `avg_total_frame_ms=16.616140`, `polygon_source_triangles=48`, `polygon_drawn_triangles=48`, `polygon_decimated=false`, `window_received=437`, `window_presented=437`, `window_dropped=0`.
- D3D12 dynamic transform logs showed `applied_dynamic_accel_update`, confirming moving physics instances stayed on the dynamic acceleration path rather than full scene/BVH rebuild.

Current conclusion:

- The UI loop, polygon preview, and dynamic instance transform path now achieve the requested 60 FPS target in the dynamic D3D12 scene when measured over a run long enough to amortize startup/reveal.
- The Camera dock now exposes the same target as a runtime-configurable FPS slider.
- Path-traced framebuffer delivery remains intentionally decoupled and may publish fewer display images than the polygon preview cadence.

### Correction - Do not composite a separate polygon preview

User feedback on screenshots:

- The translucent polygon layer caused visible ghosting and wireframe artifacts over the path-traced image.
- The intended requirement was not a separate polygon renderer composited over path tracing.
- The intended requirement is one visual path: scene/camera/object updates should be decoupled from path-tracing accumulation, while the path tracer displays noisy updated frames and converges when it can.

Corrective changes:

- Disabled the separate polygon preview compositor so normal viewport output is only the path-traced image plus editor selection/gizmo overlays.
- Removed the Camera dock polygon-preview status row to avoid implying a separate visible renderer is active.
- Kept the app-side RT scene metadata mirroring for background transform updates, because that is still needed for coherent scene state while the render coordinator catches up.
- Removed the 30 Hz cap from Qt path-traced framebuffer handoff. The framebuffer handoff now targets the configured Camera/`ui_present_hz` cadence when the path tracer has frames available.

Corrective validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- 60 Hz null smoke after disabling the compositor: `elapsed_loop_ms=1997.954900`, `avg_total_frame_ms=16.649624`, `polygon_drawn_triangles=0`, `polygon_active_frames=0`.
- D3D12-DXR dynamic physics after disabling the compositor: transform updates still use `applied_dynamic_accel_update`; `polygon_drawn_triangles=0`; path-traced frame handoff reached `window_received=793`, `window_presented=792`, `window_dropped=1` over 1200 UI frames.

### Correction - FPS counter must measure canvas paints

User feedback:

- The status bar FPS must report the canvas/viewport cadence, not the UI event-loop cadence.

Findings:

- The visible status bar previously used `UiRuntimeState::fps`, which was derived from app-loop timing.
- `window_presented` counts newly delivered path-traced framebuffers only. That is useful diagnostics, but it underreports viewport responsiveness when the canvas repaints while the path tracer catches up.

Changes made:

- Added a `painted` counter to `QtFramebufferStats`, incremented directly from `QtViewportWindow::paintEvent`.
- Changed the status bar `Canvas FPS` / `Canvas Frame` values to sample viewport paint deltas over a 250 ms interval.
- Kept `window received`, `window presented`, `window painted`, and `window dropped` in the Performance dock so canvas paint cadence and path-traced framebuffer handoff can be inspected separately.
- Kept UI-loop frame timing as `ui loop frame ms` in the Performance dock.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 600 --ui-present-hz 60` measured `window_painted=599`, `window_received=398`, `window_presented=398`, `window_dropped=0`, `canvas_fps=59.580435`, and `canvas_frame_ms=16.784033`.

Current conclusion:

- The visible FPS counter is now a canvas paint FPS counter. It no longer reports the general UI loop and no longer drops just because the path tracer publishes fewer new framebuffers than the viewport paints.

### Correction - Keep selection overlays coherent with rendered generations

User feedback:

- During rapid scene/object dragging, the selected object bounding box could jump ahead of the path-traced image for a split second.

Findings:

- Selection/gizmo boxes were rebuilt from the newest app-side `qtScene` metadata immediately.
- The path-traced framebuffer was still delivered asynchronously from `RenderCoordinator`.
- The Qt framebuffer handoff had frame IDs and counts, but not the render generation needed to tell whether the image underneath represented the same scene transform generation as the overlay.

Changes made:

- Added render generation tags to Qt framebuffer publication, presentation, and paint stats.
- Passed `DisplayFrame::generation` from `RenderCoordinator` into `QtWindow::set_framebuffer_rgba`.
- Added an overlay catch-up gate:
  - Render-affecting camera, scene, settings, scene-delta, and instance-transform posts mark the overlay as requiring the next render generation.
  - Selection/gizmo overlay updates are held while the latest submitted framebuffer generation is older than the required generation.
  - Once a matching/newer framebuffer is submitted, the overlay is refreshed so the box and path-traced image advance together.
- Added `overlay_stale_skips` and generation metadata to the render-loop diagnostics.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 600 --ui-present-hz 60` measured `canvas_fps=60.024955`, `window_painted=599`, `window_received=401`, `window_presented=401`, `latest_published_generation=3`, `latest_presented_generation=3`, `overlay_required_generation=0`, and `overlay_stale_skips=8`.

Current conclusion:

- The overlay no longer races ahead of stale path-traced frames. It can intentionally hold for a frame or two during rapid motion, but it should not visually detach from the image underneath.

### Correction - Frame pacing and FPS sampling

User feedback:

- During camera/object dragging, motion still felt hitchy.
- The status bar showed `Canvas FPS` above the configured 60 Hz target, for example `67.5`, so the counter could not be trusted.

Findings:

- The Qt render loop only slept when work finished in less than half of the frame target. A 60 Hz frame that finished in about 14 ms skipped pacing, which naturally produced about 67 FPS and uneven cadence.
- A short 250 ms canvas-rate sample made the status value sensitive to one extra paint in the window.
- Dirty dock-panel rebuilds could run too eagerly after scene edits, competing with viewport pacing.

Changes made:

- The Qt loop now always waits until the configured frame target when a frame finishes early.
- The wait path uses the existing precise spin-to-target helper; a hybrid sleep attempt overslept on Windows and was rejected during validation.
- Canvas FPS sampling now uses a retained 1.5 s paint-count window instead of a single short 250 ms interval.
- Status bar refresh remains 250 ms, but expensive dirty dock-panel refresh is throttled to 500 ms and idle dock-panel refresh remains 5 s.
- Added pacing diagnostics: `avg_pace_wait_ms`, `late_frames`, and `max_late_ms`.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- 60 Hz D3D12-DXR Qt offscreen run after the pacing fix painted 599 canvas frames in a 600-frame run, with the loop now pacing every early frame instead of allowing 14 ms frames to run uncapped.

Current conclusion:

- The specific `67.5 FPS at a 60 FPS target` failure is fixed at the source. Remaining hitches should now appear as `late_frames`/`max_late_ms` instead of being hidden by an inflated FPS counter.

### Correction - Drag-time transform flood and stale selection box

User feedback:

- Moving a scene model could still drop the canvas to very low FPS, with status showing frames around `148 ms`.
- The selected object's bounding box still visibly detached from the path-traced image while the model was being dragged.

Findings:

- Active gizmo drag flushed transform updates every UI loop iteration.
- Each flush rebuilt render transform updates, posted work to the render coordinator, scheduled selection-bound refresh, and attempted to update the overlay.
- That meant the UI could show overlay bounds from the freshest editor transform while the image was still rendering an older transform generation.

Changes made:

- Gizmo drag render-transform publishes are throttled to about 30 Hz while the mouse is held down.
- Mouse release still forces the final pending transform immediately, so the final object position is not lost.
- Selection overlay drawing is hidden during active gizmo drag and remains hidden while render-generation catch-up is pending.
- Deferred pickable/selection-bound refresh work is skipped during active drag and refreshed on commit/catch-up.
- Mouse-move event logging is coalesced so only the latest mouse move in an input batch updates the UI event log/status.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen smoke still exits cleanly. This smoke does not simulate live gizmo drag, so manual validation should focus on whether the selection box disappears during drag and reappears aligned after the noisy render catches up.

Current conclusion:

- The visible stale-box failure is addressed by not drawing a predictive overlay during active model drag.
- The drag-time frame spikes should be reduced because expensive render/selection update work is no longer performed every mouse-loop tick.

### Correction - Drag input backlog and first-move latency

User feedback:

- Scene motion is much closer, but active model dragging still feels slightly hitchy and has noticeable initial move latency.

Findings:

- Mouse moves were coalesced only for the UI event log. The app still processed every mouse-move event in the input batch.
- Each processed drag move rewrote editor transforms and could queue render-transform work, so a fast mouse could make the app consume stale intermediate positions before drawing the newest one.
- The first render-transform publish usually happened later in the same frame after the full input batch had been processed.

Changes made:

- Consecutive Qt mouse-move events are now coalesced at the window event queue, preserving the latest pointer position and summing relative deltas.
- The app loop also compacts consecutive mouse-move runs as a second guard for any non-Qt event sources.
- The first active gizmo-drag transform update now flushes immediately after the first transform write, instead of waiting until the end of input processing.
- Per-drag `viewport_gizmo_drag` UI events are throttled to 250 ms so the event log does not compete with frame pacing during a rapid drag.
- Added `coalesced_mouse_moves` to the Qt render-loop profile so real drag sessions can show whether stale input was dropped.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 600 --ui-present-hz 60` exited cleanly with `window_painted=599`, `canvas_fps=60.657814`, `canvas_frame_ms=16.485922`, and `coalesced_mouse_moves=0`.

Current conclusion:

- The automated offscreen run does not synthesize live gizmo drag, so manual validation should focus on initial drag response and whether `coalesced_mouse_moves` rises during rapid movement.
- The expected behavior is one latest-position scene update per canvas frame, not one scene mutation for every raw mouse event.

### Correction - Chunky model motion cadence

User feedback:

- Models now move quickly without killing FPS, but their visible motion still advances in chunky steps instead of smooth 60 Hz motion.

Findings:

- Active gizmo transform publishes were still limited by a hard-coded 33 ms throttle, which caps visible path-traced transform updates around 30 Hz.
- D3D12 interactive preview defaults to 8 rays per pixel per dispatch and runs temporal/denoise passes on readback samples.
- DXR dynamic transform updates also refit the TLAS immediately, which puts extra GPU synchronization work on the same path that should be serving drag motion.

Changes made:

- Removed the hard-coded 33 ms gizmo-transform throttle.
- Gizmo transform publishes are now limited to one publish per app/canvas frame, so `--ui-present-hz 60` can publish 60 transform updates per second and other target FPS values scale with the camera/viewport target.
- The transform update source frame is now propagated into `InstanceTransformUpdateOptions` for render-worker diagnostics.
- Added `gizmo_transform_flushes` and `gizmo_transform_same_frame_skips` to the Qt render-loop profile.
- D3D12 dynamic motion updates now enter a short fast-motion mode:
  - render with 1 ray per pixel for the first motion samples,
  - disable temporal AA and denoiser work during those fast motion samples,
  - clear temporal history while transforms are moving,
  - defer DXR TLAS refits during active motion and render the fast preview through the compute path using the updated instance/dynamic BVH buffers,
  - apply the deferred DXR TLAS update after motion settles so DXR convergence can resume from the final transform.
- D3D12 accumulation reset is now CPU-side bookkeeping for this path. The compute and DXR shaders overwrite the accumulation buffer when `sample_index == 0`, removing a separate GPU clear command and its waits from every transform reset.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12 compute Qt offscreen run: `--backend d3d12 --frames 240 --ui-present-hz 60` exited cleanly with `window_painted=239`, `window_received=161`, `window_presented=161`, `window_dropped=0`, `canvas_fps=59.977162`, and `canvas_frame_ms=16.673013`.
- D3D12-DXR offscreen validation was blocked twice by device startup failure before rendering (`CreateCommandQueue(DIRECT) hr=0x887A0005`, then `create_film_buffer` device removed). That run did not exercise the changed render path.

Current conclusion:

- The previous 30 Hz editor publish cap is removed.
- Manual drag validation should now check whether `gizmo_transform_flushes` rises roughly once per painted frame during movement and whether model motion is visibly smoother.
- If visible movement still chunks, the next bottleneck is the actual render-worker display frame rate under fast-motion samples rather than the editor publish cadence.

### Correction - Backend motion upload fence

User feedback:

- Moving items are still throttled: the scene can keep painting, but object motion itself still advances in visible steps.

Findings:

- The D3D12 motion path still uploaded the changed instance buffer and dynamic-instance BVH through a separate command list.
- That upload path called `wait_for_gpu()` before and after the copy, so every motion transform could serialize `upload transform -> wait -> render frame -> wait`.
- This explains a chunky object cadence even after removing the editor-side 33 ms transform publish cap.

Changes made:

- D3D12 editor/physics/script motion updates now stage the CPU scene mirror immediately and mark a pending instance upload instead of submitting a separate GPU copy command.
- The next compute or DXR render command list emits the pending instance-buffer and dynamic-BVH copies before dispatch, keeping transform upload and image generation in one GPU submission.
- Pending motion ranges are coalesced so multiple transform updates before a render upload one combined instance range plus the latest dynamic BVH.
- Non-motion transform updates still use the immediate upload path.
- Fast-motion samples now clamp path depth to one bounce in addition to one ray per pixel and disabled denoise/temporal work, prioritizing drag cadence over convergence.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12 compute Qt offscreen run: `--backend d3d12 --frames 240 --ui-present-hz 60` exited cleanly with dynamic transform updates applied, `window_painted=239`, `window_received=158`, `window_presented=157`, `window_dropped=1`, `canvas_fps=59.975315`, and `canvas_frame_ms=16.673526`.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 120 --ui-present-hz 60` initialized DXR BLAS/TLAS, exited cleanly with `window_painted=119`, `window_received=78`, `window_presented=78`, `window_dropped=0`, `canvas_fps=59.976667`, and `canvas_frame_ms=16.673151`.

Current conclusion:

- The remaining backend-side per-motion upload fence has been removed from the drag hot path.
- Manual drag validation should now focus on whether object position advances every painted frame rather than in upload-limited chunks.

### Correction - Motion display cadence and readback interval

User feedback:

- The object updates still appear in chunks and are not aligned to the selected 60 fps motion preview rate.

Findings:

- Fast-motion rendering still used the normal D3D12 interactive readback interval.
- With the default interval of 4, a renderer producing 60 fast samples per second would still only refresh the displayed LDR image about every fourth sample, which presents as roughly 15 Hz object motion.
- The Qt framebuffer handoff was paced from the last successful submit time. If the renderer missed a slot, the next available frame could be submitted immediately, drifting away from the selected preview cadence and producing uneven frame spacing.
- The fast-motion window was only 8 samples, so the backend could fall back into a heavier convergence sample during small gaps between drag updates.

Changes made:

- D3D12 fast-motion samples now force an LDR readback every sample.
- The fast-motion sample window was extended to 32 samples and refreshes on each editor/physics/script motion update, reducing the chance of a heavy convergence sample interrupting active movement.
- Qt background framebuffer handoff now uses a fixed next-submit clock based on `ui_present_hz` instead of `time since last successful submit`, so received/presented frames are aligned to the selected preview cadence.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 160 --ui-present-hz 60` initialized DXR BLAS/TLAS, applied dynamic transform updates, and exited with `window_painted=159`, `window_received=145`, `window_presented=144`, `window_dropped=1`, `canvas_fps=59.896030`, and `canvas_frame_ms=16.695597`.

Current conclusion:

- The previous readback interval could directly explain visible 15 Hz motion chunking during a 60 Hz drag. That path now reads back every fast-motion frame.
- Live validation should check whether `window_received` and visible object updates stay close to the selected preview rate while dragging, not just whether `canvas_fps` is near 60.

### Correction - Stop tying visible motion to path-traced frame completion

User feedback:

- Model motion still feels like it is moved in batches instead of as a smooth real-time animation.

Findings:

- The visible object position was still fundamentally tied to completed path-traced frame publication.
- Even with 1 ray per pixel, one bounce, forced readback, and command coalescing, any missed render-worker slot appears as a model-position hold followed by a jump.
- That is the wrong contract for direct manipulation: the viewport display clock should own active object motion, while the path tracer converges behind it.

Changes made:

- Re-enabled the full-frame real-time shaded scene preview during active motion instead of using it as a transparent overlay.
- The preview replaces the stale path-traced frame while motion is active, so there is no old path-traced model underneath and no ghosted double image.
- The preview stays active until either the short motion hold expires or the path-traced generation has caught up to the latest submitted transform.
- The path tracer still receives the same dynamic transform updates and catches up/converges after motion, but it no longer owns the visible motion cadence during direct manipulation.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 160 --ui-present-hz 60` initialized the viewport and exited cleanly with `window_painted=159`, `canvas_fps=59.986918`, and `canvas_frame_ms=16.670301`.

Current conclusion:

- This changes active motion from render-worker-paced to display-clock-paced.
- Live validation should now focus on whether the visible selected model moves smoothly at the configured preview FPS, accepting that the image will be a real-time shaded preview during the drag and return to path tracing when the render catches up.

### Correction - Live editor transforms for the raster scene

User feedback:

- The desired architecture is a 60 Hz underlying 3D scene, with the path tracer decoupled and spending available time on rays. The visible model motion must not be driven by path-tracer batches.

Findings:

- The real-time raster scene was still reading transforms from `qtScene.instances`.
- `qtScene.instances` is updated when transform updates are published to the render coordinator, so the raster scene could still inherit render/update batching.
- The live editor document already has the current drag transform immediately after mouse input; the raster scene should read from that source instead.

Changes made:

- The raster scene preview now resolves each RT instance's transform from the live editor scene document/world snapshot when drawing.
- Gizmo writes mark raster motion immediately, before any render-coordinator publish.
- Path-tracer transform publish remains coalesced and asynchronous, but it no longer controls the visible raster motion transform.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke passes.
- D3D12-DXR Qt offscreen run: `--backend d3d12-dxr --frames 160 --ui-present-hz 60` exits cleanly with `window_painted=159`, `canvas_fps=60.003591`, and `canvas_frame_ms=16.665669`.

Current conclusion:

- Active model movement should now be driven by live editor transforms on the raster/display path, not by path-tracer frame completion or render-coordinator transform batching.

### Reference review - WebGL pathtracer architecture

User feedback:

- Review `C:\Users\Cam\Desktop\pathtracer` and match its motion/accumulation architecture because that app moves smoothly while rays accumulate asynchronously.

Reference findings:

- The WebGL app has one `requestAnimationFrame` display loop.
- Pointer movement writes the selected object's temporary transform immediately with `setTemporaryTranslation(...)`.
- That same pointer move only marks path-tracer scene uniforms dirty and calls `clearSamples(false)`.
- The next animation frame uploads the latest dirty scene uniforms, renders the configured `raysPerPixel` samples, and then displays the current accumulation texture.
- Selection boxes read the same current object transform, so the box and rendered object share the same motion source.
- The path tracer does not own object motion cadence. It accumulates bounded work inside the display frame cadence.

Architecture correction for this repo:

- The Qt canvas/display loop must be the authoritative clock for camera and model motion at `ui_present_hz`.
- Editor/world transforms are the authoritative visible state during interaction.
- The path tracer is a refinement worker that consumes latest scene state and resets accumulation on generation changes.
- Completed path-traced frames may only replace the realtime scene layer when their generation is current.
- Background ray work must not free-run hard enough to steal pacing from the canvas loop.

Changes made:

- Qt frame pacing now processes non-input Qt events while waiting for the next canvas tick, allowing paint and queued framebuffer delivery to occur in the same paced interval instead of after a full extra loop.
- The background render coordinator now paces sample-batch starts to the configured `publish_hz`/`ui_present_hz` interval. It no longer free-runs unlimited path-tracing batches between display publications.
- The shutdown status summary now records `canvas_fps` and `canvas_frame_ms` so offscreen runs expose the same canvas-paint measurement shown in the status bar.
- While re-running the deterministic UI gate, fixed `EnsureQtFallbackLightingEntities` storing a reference into `SceneDocument::entities` and then appending children before reading `group.id`; the group id is now captured before child insertion so fallback lights stay parented under the hidden group.

Expected effect:

- Moving objects should be driven by the realtime scene layer each canvas frame.
- Path tracing should spend at most one sample batch per configured canvas interval and converge opportunistically instead of saturating the frame pipeline.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke: `ptapp.exe --ui-model-smoke --platform headless --no-env-file` exits `0` and reports `ui model smoke: ok`.
- D3D12 Qt offscreen: `--backend d3d12 --frames 160 --ui-present-hz 60` exits `0` with `window_painted=159`, `canvas_fps=59.9679`, `canvas_frame_ms=16.6756`, `window_received=101`, `window_presented=101`, `window_dropped=0`.
- D3D12-DXR Qt offscreen: `--backend d3d12-dxr --frames 160 --ui-present-hz 60` exits `0` with `selected_renderer_path=d3d12_dxr`, `window_painted=159`, `canvas_fps=59.9554`, `canvas_frame_ms=16.6791`, `window_received=100`, `window_presented=99`, `window_dropped=1`.

Current conclusion:

- The reference app's contract is now reflected more directly: canvas/frame pacing owns interaction, and background ray work is paced by the canvas target instead of free-running.
- These offscreen runs validate canvas paint cadence, not hand-driven gizmo smoothness. Manual validation should now focus on whether the selected model's visible realtime scene layer advances every painted frame during drag.

### Correction - Motion layer should not look like polygon debug

User feedback:

- The realtime motion layer still showed visible ray/polygon facets.

Findings:

- Full-frame motion replacement was still shading each triangle from its face normal.
- Even with edges disabled, per-triangle shading made the temporary realtime layer read as a polygon/debug view while dragging.

Changes made:

- Full-frame realtime replacement now uses a stable per-object shade instead of per-triangle face-normal shading.
- Replacement-mode triangle outlines are drawn in the same color as the fill so raster cracks are covered without showing wire edges.
- Non-replacement overlay mode keeps the old shaded/outlined behavior for diagnostics.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0` and reports `ui model smoke: ok`.
- D3D12-DXR Qt offscreen exits `0` with `canvas_fps=59.9681`, `canvas_frame_ms=16.6755`, `window_painted=159`, and `selected_renderer_path=d3d12_dxr`.

### Correction - Remove full-frame polygon replacement

User feedback:

- The flat realtime raster fallback is not acceptable. The viewport should not switch to that solid polygon scene.

Change made:

- Disabled full-frame realtime polygon replacement by default. `updateQtPolygonPreview()` now clears the polygon preview and returns, so interaction stays on the path-traced/noisy framebuffer instead of showing the flat raster fallback.

Validation:

- Build passed after stopping the running `ptapp.exe` that was locking the output executable.
- UI model smoke exits `0` and reports `ui model smoke: ok`.
- D3D12-DXR Qt offscreen exits `0` with `selected_renderer_path=d3d12_dxr`, `window_painted=159`, `canvas_fps=59.9249`, and `canvas_frame_ms=16.6876`.

### Reference copy - Frame-tick-driven path tracing

User feedback:

- Review the reference path tracer again and copy how item movement works instead of substituting a flat polygon/raster scene.

Reference system copied:

- The WebGL reference uses one `requestAnimationFrame` loop as the movement/render authority.
- Input writes a temporary object transform immediately.
- The path tracer is only marked dirty and accumulation is reset.
- The next display frame uploads the latest transform and renders bounded ray work for that frame.
- There is no independent render cadence that can consume motion updates in batches.

Changes made:

- `RenderCoordinatorConfig` now supports `frame_tick_driven`.
- The Qt runtime starts background GPU/CPU coordinators in frame-tick-driven mode.
- The Qt canvas loop calls `request_frame_tick()` after input, physics, and script updates have posted their latest scene changes.
- In frame-tick-driven mode, the render coordinator waits for the next canvas tick and renders at most one latest-state sample for that tick. It no longer advances samples on its own timer.

Expected effect:

- Model motion remains path-traced/noisy rather than switching to a raster fallback.
- Transform updates are latest-wins and sampled once per canvas frame, matching the reference's RAF cadence more closely.
- If the renderer misses a tick, it skips old ticks instead of catching up through stale intermediate movement states.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0` and reports `ui model smoke: ok`.
- D3D12-DXR Qt offscreen exits `0` with `selected_renderer_path=d3d12_dxr`, `window_painted=156`, `canvas_fps=59.8516`, `canvas_frame_ms=16.708`, `render_published=100`, `render_dropped=2`, `window_received=98`, and `window_presented=97`.

Current conclusion:

- The background renderer is now driven by canvas ticks instead of an independent sample timer.
- The offscreen run confirms the canvas remains paced at about 60 FPS while path-traced publications are bounded by what ray work can finish on those ticks.

### Correction - GPU path must run in the display loop

User feedback:

- The frame-tick-driven background worker made performance worse and still did not feel like the reference.

Findings:

- The reference WebGL path tracer does not use a background render worker at all.
- Its frame loop directly performs input/physics, updates dirty scene uniforms, renders bounded ray work, and displays.
- My previous frame-tick worker copied the tick signal but kept an extra worker/handoff boundary that the reference does not have.

Changes made:

- D3D12 and Vulkan Qt preview tracers now run on the event/display loop instead of the background `RenderCoordinator`.
- The synchronous Qt render path is allowed to run during bounded offscreen frame-limit smokes, so validation exercises the same path used by interactive GPU preview.
- Removed the frame-tick-driven coordinator API and reverted background coordinators to their previous publish-capped behavior for CPU/background use.
- CPU tiled preview remains backgrounded because it is the blocking renderer; GPU preview now matches the reference loop shape.

Expected effect:

- Moving an item updates the editor transform, applies the transform directly to the active GPU tracer, resets accumulation, then the same canvas loop renders the next noisy path-traced sample.
- There is no flat polygon fallback and no background worker cadence between GPU scene motion and the displayed path-traced frame.

Validation after correcting the GPU path:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0` and reports `ui model smoke: ok`.
- D3D12-DXR Qt offscreen exits `0` with `selected_renderer_path=d3d12_dxr`, `polygon_drawn_triangles=0`, `polygon_active_frames=0`, `canvas_fps=55.3836`, `canvas_frame_ms=18.0559`, and `avg_render_ms=14.9702`.
- This proved the flat polygon fallback was gone, but stationary full-depth DXR work still exceeded the 16.67 ms target on this scene.

### Correction - Interactive path tracing should be noisy and bounded

User feedback:

- The viewport should not show the flat polygon fallback, and motion should feel like the reference path tracer: current scene state first, path tracing convergence second.

Findings:

- Qt interactive preview still defaulted GPU denoiser and temporal AA on. Temporal reuse can create ghosting during motion, and both passes add work that is not part of the reference movement contract.
- D3D12-DXR stationary full-depth sampling at 320x240 was around 15 ms before UI work, leaving too little frame budget.
- The continuous transform gate was using the legacy `update_instance_transforms()` path, which did not mark following samples as fast-motion samples.
- Camera updates also did not mark following samples as fast-motion samples, so camera orbit/drag could fall back to full-depth DXR work.

Changes made:

- Qt interactive preview now defaults `enable_denoiser=false` and `enable_temporal_aa=false`. The Render Settings dock can still re-enable them for quality work.
- D3D12 interactive preview defaults to `PT_D3D12_RAYS_PER_PIXEL=1` and `PT_D3D12_READBACK_INTERVAL=1` when the user has not overridden those environment variables.
- `update_instance_transforms()` now starts the D3D12 fast-motion sample window, matching the direct transform-update path.
- `update_camera_state()` now starts the same fast-motion sample window, so camera motion gets bounded low-latency samples.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0`.
- D3D12-DXR dynamic physics gate exits `0`: `dynamic_instances=9`, `transform_update_successes=16`, `full_rebuild_count=0`, `render_ms=8.3628`, `physics_step_ms=0.2406`, `transform_publish_ms=0.4511`, `total_rays=460800`.

Current conclusion:

- The moving-object path is now below the 16.67 ms frame budget for 60 Hz in the deterministic gate, with path-traced/noisy samples and no static BVH rebuilds.
- Stationary convergence can still spend heavier DXR work; interaction paths now force the bounded fast-motion path so the scene state is sampled from the latest transform instead of waiting on convergence.

### Correction - Editor-moved meshes need dynamic transform storage

User feedback:

- Items in the scene should move freely without rebuilding the BVH.

Findings:

- Scene conversion only marked physics-dynamic or scripted meshes as dynamic-transform capable.
- Normal editor-moved mesh entities could therefore miss the dynamic transform update path and require a full fallback instead of behaving like the reference project's object-uniform update.

Change made:

- Mesh entities with authored transforms are now converted with dynamic transform storage. They keep local vertices/indices alongside their transformed triangles, allowing editor motion to update GPU instance/local BVH data without rebuilding the whole static acceleration structure.

Validation:

- D3D12-DXR dynamic physics gate now reports `dynamic_instances=9` for the Cornell scene and still reports `full_rebuild_count=0`.
- Continuous moving-frame render time is `8.3628 ms`, leaving frame budget for 60 Hz canvas interaction.

### Correction - Remove remaining motion batching

User feedback:

- Moving objects are better but still animate in chunks instead of smooth realtime motion.

Findings:

- The gizmo drag path could flush the first mouse-move transform immediately during input processing.
- Later mouse positions from the same canvas frame were queued but skipped by the same-frame flush guard, so the tracer often rendered an older transform and caught up one frame later.
- Physics/object animation publishing used a fixed 16 ms gate instead of the configured canvas frame target.

Changes made:

- Removed the early gizmo transform flush from `qtApplyGizmoDrag`; the frame now drains all mouse input first, then publishes the latest selected-object transform once before render.
- Physics transform publishing now uses `qtInteractiveFrameTarget()`, so animation cadence follows the camera dock target FPS instead of a hardcoded 16 ms interval.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0`.
- D3D12-DXR dynamic physics gate exits `0`: `dynamic_instances=9`, `transform_update_successes=16`, `full_rebuild_count=0`, `render_ms=9.2635`, `physics_step_ms=0.2474`, `transform_publish_ms=0.4972`.

### Correction - Stop convergence work from consuming multiple motion frames

User feedback:

- Motion still appeared batched, averaging roughly 12 FPS, like one visible scene update per five canvas frames.

Findings:

- The Qt GPU display-loop path still allowed up to 64 path-trace dispatches in a single canvas tick if the estimated batch time fit the frame budget.
- That prioritizes convergence and can make interaction feel like batches when each dispatch is nontrivial or when environment readback settings are stale.
- D3D12 interactive preview could still honor a non-1 `PT_D3D12_READBACK_INTERVAL`, causing display updates to appear at a 1:N ratio even when frames are being submitted.

Changes made:

- Clamped Qt interactive GPU preview to one path-trace dispatch per canvas frame.
- Forced D3D12 interactive preview (`spp == UINT_MAX`) to read back every sample, regardless of readback interval.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0`.
- D3D12-DXR dynamic physics gate exits `0`: `dynamic_instances=9`, `transform_update_successes=16`, `full_rebuild_count=0`, `render_ms=9.0455`.
- D3D12-DXR Qt offscreen run exits `0`: `render_published=120`, `window_painted=119`, `canvas_fps=60.2687`, `canvas_frame_ms=16.5924`, `avg_render_ms=1.3340`, `gizmo_transform_same_frame_skips=0`, `polygon_drawn_triangles=0`.

### Correction - Remove input-side and gizmo motion batching

User feedback:

- Item movement still appears chunked/batched rather than smooth at the selected 60 FPS canvas rate.

Findings:

- Qt high-frequency input compression was still enabled before `QApplication` construction.
- `QtWindow::queue_event()` merged consecutive mouse moves into one queued event.
- The Qt render loop had a second mouse-move run coalescer.
- Frame pacing waited with `ExcludeUserInputEvents`, so input could accumulate until the next explicit poll.
- Gizmo motion still collected touched IDs, skipped additional publishes within the same frame, and flushed once after the full input batch.
- Standard transform update options enabled coordinator coalescing for editor/physics/script motion.

Changes made:

- Disabled Qt high-frequency and tablet event compression.
- Removed `QtWindow` mouse-move queue merging and the render-loop mouse-move run coalescer.
- Pump all Qt events during frame pacing sleep so pointer events arrive continuously.
- Removed the per-frame gizmo touched-ID queue and same-frame publish skip.
- `qtApplyGizmoDrag()` now writes the scene transform and publishes RT instance transforms immediately for each drag event.
- Continuous editor, physics, and script transform options now set `coalesce=false`, so the render coordinator uses latest-wins motion updates instead of merging motion commands.

Validation:

- Build: `cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp` completes.
- UI model smoke exits `0`.
- D3D12-DXR dynamic physics gate exits `0`: `dynamic_instances=9`, `transform_update_successes=16`, `full_rebuild_count=0`, `render_ms=7.5253`, `physics_step_ms=0.2535`, `transform_publish_ms=0.4837`.
- D3D12-DXR Qt offscreen run exits `0`: `render_published=120`, `window_painted=119`, `canvas_fps=59.9424`, `canvas_frame_ms=16.6827`, `avg_render_ms=1.3616`, `coalesced_mouse_moves=0`, `gizmo_transform_same_frame_skips=0`, `polygon_drawn_triangles=0`.
- `--third-person-script-gate` exits `0` when run without `--frames`; the rejected `--frames` form is a command-line validation rule outside `--window`, not a transform-update failure.
