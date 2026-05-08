# vkPathTracer Runtime Architecture

This document describes the runtime ownership model used by the snapshot-bus
migration. The main rule is that cross-thread state moves through explicit
channels. Long-lived workers must not keep direct pointers into the mutable app
scene graph.

## Snapshot Bus

```
Qt/App thread
  owns editable SceneDocument and UI state
  publishes render snapshots
        |
        v
SnapshotRing
  atomic latest shared_ptr<const RenderSceneSnapshot>
  triple-slot retention
  reader lag and drop metrics
        |
        +--> Render worker: reads current snapshot at tile boundaries
        +--> Audio callback: one current() load per callback
        +--> UI/picking: must read snapshots instead of mutable render copies
        +--> Scripts: may read immutable world/snapshot state and emit commands
```

`RenderSceneSnapshot` is immutable once published. Large arrays are stored in
copy-on-write wrappers so unchanged geometry, materials, lights, textures, and
environment data can be shared across generations. The snapshot carries revision
counters for topology, transform, camera, material, and wall-clock generation
metadata. Consumers compare revisions to decide whether they can continue,
reproject, reset moving pixels, invalidate shading, or fully reset accumulation.

`SnapshotRing` owns the latest published snapshot and a fixed three-entry
retention window. Readers may register by name. `current(reader_id)` updates
their observed generation so lag and drop counters can identify stalled
consumers.

## Channel Table

| Channel | Writer | Reader | Payload | Rule |
| --- | --- | --- | --- | --- |
| `SnapshotRing` | App/Sim publish point | Render, Audio, UI, Script | `shared_ptr<const RenderSceneSnapshot>` | Immutable after publish; readers keep only local shared pointers. |
| `FrameHandoff` | Render worker | UI thread | Resolved display frame | Latest-wins display handoff; never blocks render on UI presentation. |
| `RenderCoordinator` mailbox | UI/App thread | Render worker | Legacy camera/settings/scene commands | Transitional path only; new render-visible state should move through snapshots. |
| `ScriptThread` hooks | Sim/App thread | Script worker | Hook requests and world snapshots | Script worker emits command buffers; it must not mutate the app scene directly. |
| Audio command rings | App/Script | Audio system | Sound commands and control changes | Callback consumes bounded commands and must not allocate or block. |
| Metrics/log/trace | Any component | Observability tools | Counters, structured logs, trace events | Use component names and generation IDs so state can be reconstructed from logs. |

## Thread Ownership

| Thread | Owns | May read | May write | Blocking policy |
| --- | --- | --- | --- | --- |
| UI/App | Qt widgets, editable scene document, tool state, user input | Latest render frame, diagnostics, published snapshot for UI-only queries | Scene edits, snapshot publishes, command rings | May block on user-driven operations, but not inside frame presentation. |
| Sim | Physics step, script command application, snapshot build point | Previous immutable snapshot and editable sim state | New snapshot generation and sim-owned state | Fixed-step work should be bounded; no render/audio waits. |
| Render | Accumulation buffers, tracer-local acceleration data, tile schedule | `SnapshotRing.current()` at tile boundaries | `FrameHandoff`, render metrics | No UI waits. Long scene rebuilds should happen between tiles or before publish. |
| Audio callback | Device callback scratch state, active voices | One snapshot pointer loaded at callback start, audio command rings | Output PCM and audio metrics | No malloc, no file I/O, no locks on the real-time path. |
| Script worker | Script runtimes, deterministic job fan-out state | Immutable world/snapshot checkout | Script command buffers | No direct scene mutation; deterministic mode must keep stable job ordering. |
| Job workers | Short-lived range jobs | Job input captures | Job-local outputs only | Work stealing is allowed outside deterministic mode; workers must drain on shutdown. |
| Observability tools | In-memory metric/log/trace rings | Component diagnostics | Exported reports and doctor results | Collection should be non-invasive and bounded. |

## Determinism Context

Deterministic replay is represented by one `vkpt::core::DeterminismContext`
with `enabled`, `base_seed`, `frame_index`, and `scenario_id`. Subsystem config
surfaces that already had a local deterministic flag expose
`set_determinism(const DeterminismContext&)` so startup and replay harnesses can
copy one context into render settings, script execution, physics steps, audio
config, and the job system without each caller knowing the older local fields.

The compatibility fields remain in place for now. The setter is the preferred
entry point for new code because it preserves the replay seed and scenario ID
alongside the legacy enabled/disabled boolean.

## Adding A Subsystem

1. Pick a single owning thread for mutable state.
2. Decide how other threads observe it: immutable snapshot, SPSC/MPSC command
   ring, metrics/log stream, or a UI-only query on the owning thread.
3. Register any long-lived snapshot reader with a stable name and store only
   local `shared_ptr<const RenderSceneSnapshot>` values while processing.
4. Include generation IDs in logs, metrics, and command results so behavior can
   be tied back to a published snapshot.
5. Keep real-time and hot paths bounded: no blocking waits, unbounded queues,
   filesystem access, or dynamic allocation in render tile loops or audio
   callbacks.
6. Add a smoke test for the channel contract: publish/consume order, overflow or
   drop behavior, shutdown behavior, and any deterministic-mode guarantees.

New subsystem code must either publish to an explicit channel or read from one.
If a worker needs mutable app state, add a snapshot field or command payload
instead of passing the mutable owner across the thread boundary.

## JSON Log Schema

`--log-format=json` writes one JSON object per line. The observability smoke
parses the generated JSONL and validates this schema:

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `ts` | number | Yes | Non-negative monotonic timestamp in nanoseconds. |
| `lvl` | string | Yes | One of `trace`, `debug`, `info`, `warn`, `error`, `fatal`. |
| `thr` | string | Yes | Thread name, or a stringified thread-id hash. |
| `comp` | string | Yes | Component name such as `tracer`, `audio`, `metrics`, or `health`. |
| `ev` | string | Yes | Event name within the component namespace. |
| `coalesced` | number | No | Present only for collapsed bursts; value is at least 2. |
| Any other key | primitive | No | Event fields must be JSON null, boolean, number, or string. |

Reserved top-level keys are `ts`, `lvl`, `thr`, `comp`, `ev`, and `coalesced`.
Event-specific fields should avoid these names so downstream agents can read
the log without collision handling.
