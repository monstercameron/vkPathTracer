# SYSTEM.md — Snapshot-Bus Architecture Migration

> **North star:** UI/UX smoothness is the primary contract. Sim ticks on time. The
> tracer follows when it can — noisy frames are acceptable; convergence scales
> with GPU count, not by stalling the user.

---

## Guiding principles

- **One writer per piece of state.** Sim is the only authority over the world.
- **One immutable snapshot, many readers.** Tracer, audio, scripts, and UI all
  read from the same `SceneSnapshot`, never from each other.
- **Lock-free on every hot path.** Mutexes appear only at thread setup/teardown.
- **No deep copies on the per-frame path.** Use COW arrays + atomic pointer swap.
- **Tracer is best-effort.** It never blocks sim, never blocks UI, can drop
  samples freely. Noisy is fine; smooth is required.
- **Determinism is a flag, not a default.** Optional serial mode for tests.
- **Every component is observable from the outside.** Structured events, named
  metrics, and per-thread event rings — so a human or an agent reading the logs
  can reconstruct what each thread was doing without attaching a debugger.

---

## Observability contract

Every subsystem in this migration MUST emit:

1. **Lifecycle events** — `<comp>.started`, `<comp>.stopped`, `<comp>.config`.
2. **Steady-state heartbeat** — one event per second per component summarizing
   work done (rates, depths, drops). Throttled, never per-frame at info level.
3. **Hot events at debug level** — per-tile, per-callback, per-hook events,
   off by default, opt-in via `--verbose=<comp>:<channel>`.
4. **Slow-path / anomaly events at warn level** — deadline misses, drops,
   underruns, fallbacks, retries. Always on.
5. **Named metrics** — registered in `vkp.<component>.<metric>` namespace,
   scrapeable from the dock panel and exportable as JSON.

**Event schema** (one line per event):

```
JSON:  {"ts":1714000000000,"lvl":"warn","thr":"tracer","comp":"snapshot_ring",
        "ev":"reader_lagging","gen":1042,"reader":"audio","lag_gen":4}
KV:    ts=1714000000000 lvl=warn thr=tracer comp=snapshot_ring
        ev=reader_lagging gen=1042 reader=audio lag_gen=4
```

Required fields on every event: `ts` (ns since epoch), `lvl`, `thr`, `comp`,
`ev`. All other fields are event-specific. **Snapshot `generation` is included
on every event that touches a snapshot** — that's the cross-thread correlation
ID for the whole system.

Format toggled by `--log-format=json|kv|console` (default `console` for humans,
`json` for agents).

---

## Final thread layout

| Thread     | Cadence                  | Priority   | Role                                        |
|------------|--------------------------|------------|---------------------------------------------|
| UI         | vsync / input-driven     | normal     | Qt event loop, input capture, blit, picking |
| Sim        | adjustable, default 60Hz | normal     | Authoritative writer; merges; publishes     |
| Physics    | fixed sub-step (240Hz)   | normal     | Owns physics world; cmd-in / delta-out      |
| Script     | hook-driven              | normal     | N persistent `lua_State`s; pure producers   |
| Tracer     | free-running, tile-gran. | background | Reads snapshot; accumulates; reprojects     |
| Audio      | device callback (~5ms)   | realtime   | Reads snapshot for spatialization; mixes    |

JobSystem (work-stealing) backs Tracer tiles, Sim BVH builds, and Group-B scripts.

---

## Channels (all lock-free)

| Channel             | Producer → Consumer            | Type                                         |
|---------------------|--------------------------------|----------------------------------------------|
| `InputMailbox`      | UI → Sim                       | SPSC ring                                    |
| `ScriptHookRing`    | Sim → Script                   | SPSC ring (fire-and-forget hook requests)    |
| `ScriptCmdRing`     | Script → Sim                   | SPSC ring (typed world commands)             |
| `PhysicsCmdRing`    | Sim → Physics                  | SPSC ring                                    |
| `PhysicsDeltaRing`  | Physics → Sim                  | SPSC ring                                    |
| `SnapshotRing`      | Sim → {Tracer, Audio, Script, UI} | atomic `shared_ptr<const SceneSnapshot>`  |
| `SoundRing`         | {Sim, Script} → Audio          | MPSC ring (or per-producer SPSC)             |
| `FrameRing`         | Tracer → UI                    | latest-wins single-slot                      |

---

## Track plan (4 parallel agents)

The work splits into 4 tracks owned by 4 agents. Tracks are ordered low-level
to high-level. **Within a track, do tasks top-to-bottom** — lower section
numbers are always the more foundational ones. Across tracks, B and C run in
parallel once A's primitives merge; D integrates once B publishes a snapshot
and C produces commands.

```
                 ┌─────────────────────────┐
                 │  Track A — Foundation   │   (rings, logger, metrics,
                 │  Phase 0                │    health, console, CI)
                 └────────────┬────────────┘
                              │  delivers: SpscRing/MpscRing/LatestSlot/
                              │            AtomicSharedPtr, VKP_LOG/METRIC/
                              │            TRACE, MetricsRegistry, flags
              ┌───────────────┴───────────────┐
              ▼                               ▼
   ┌─────────────────────┐       ┌─────────────────────────────┐
   │  Track B — Scene    │       │  Track C — Sim Producers    │
   │  Bus & Tracer       │       │  (Physics + Scripts)        │
   │  Phases 1, 3        │       │  Phases 2, 5, 6             │
   └──────────┬──────────┘       └──────────────┬──────────────┘
              │                                 │
              │ delivers: SceneSnapshot,        │ delivers: Physics{Cmd,Delta}Ring,
              │  SnapshotRing.current(),        │  ScriptCmdRing/HookRing,
              │  FrameRing                      │  persistent lua_State pool
              │                                 │
              └───────────────┬─────────────────┘
                              ▼
                 ┌─────────────────────────────────────┐
                 │  Track D — Audio + Integration      │
                 │  Phases 4, 7                        │
                 │  (audio thread, Sim/UI observability,│
                 │   cleanup, sunset, acceptance)      │
                 └─────────────────────────────────────┘
```

### Cross-track sync points

- **Sync 1 — A → B/C unblocks**: A's 0.1 (primitives) + 0.2 (logger) merged.
  B and C can begin without rebuilding the foundations.
- **Sync 2 — B → D unblocks**: B's 1.1–1.4 (`SceneSnapshot` + `SnapshotRing`
  + Sim publishes + Tracer reads) merged. D can spatialize against snapshots.
- **Sync 3 — C → D unblocks**: C's 5.1–5.3 (Lua pool + script thread + cmd
  ring) merged. D can wire `PlaySound` cmds end-to-end.
- **Sync 4 — final**: B + C exit gates pass. D runs acceptance suite.

If an agent finishes their track early, they can pull tasks from Track D
(integration always has more to do).

---

### Track A — Foundation & Observability  *(lowest level, blocks all)*

**Mission:** lay every primitive, macro, and operator surface that the other
three tracks consume. Nothing in this track touches a subsystem; everything
in this track is consumed by every subsystem.

**Owns:** Phase 0 (all sections), Console & runtime introspection.

**Depends on:** nothing. Starts immediately.

**Implementation order (low → high):**
1. `SpscRing<T>`, `MpscRing<T>`, `LatestSlot<T>`, `AtomicSharedPtr<T>`
   (Phase 0.1).
2. `Logger` async writer + `VKP_LOG` macro + per-thread crash rings
   (Phase 0.2).
3. `MetricsRegistry` + `VKP_METRIC_*` + heartbeat thread (Phase 0.3).
4. `VKP_TRACE_SCOPE` + flow IDs + Chrome-tracing export (Phase 0.4).
5. `IHealthProbe` interface + `system.health` heartbeat (Phase 0.5).
6. CLI flags + REPL commands + signal handlers (Console section).
7. CI: TSan target, deterministic flag plumbing, regression test (Phase 0.6).

**Exit gates:**
- All ring primitives pass fuzz + TSan-clean.
- Logger emits one parseable line per event under `--log-format=json`.
- Metrics scrape via `metrics dump` REPL command works.
- Crash-ring dump on `SIGSEGV` contains last events from every test thread.
- Sample dummy component emits lifecycle + heartbeat + anomaly events end
  to end with no manual wiring beyond macro calls.

**Public interfaces (delivered to B/C/D):**
- `<src/core/sync/>` — ring primitives.
- `<src/core/log/>` — `VKP_LOG`, `VKP_LOG_RT`, `VKP_LOG_SAMPLED`.
- `<src/core/metrics/>` — `VKP_METRIC_*`, `MetricsRegistry`.
- `<src/core/trace/>` — `VKP_TRACE_SCOPE`, `VKP_TRACE_FLOW`.
- `<src/core/health/>` — `IHealthProbe`.
- CLI: `--log-level`, `--log-format`, `--verbose`, `--trace`, `--trace-out`,
  `--metrics-out`, `--deterministic`.
- REPL: `metrics`, `events`, `snapshot`, `script`, `health`, `tracer`.

---

### Track B — Scene Bus & Tracer  *(mid level, parallel to C)*

**Mission:** make the scene immutable + atomically published, refactor the
tracer to read from snapshots tile-by-tile, and unlock multi-GPU scaling. By
the end of this track, motion no longer pins accumulation at 1 spp.

**Owns:** Phase 1 (`SceneSnapshot` + `SnapshotRing`), Phase 3 (tile-granular
tracer + reprojection + multi-GPU + backend parity).

**Depends on:** A's `SpscRing`, `LatestSlot`, `AtomicSharedPtr`, `VKP_LOG`,
`VKP_METRIC_*`, `VKP_TRACE_SCOPE`.

**Implementation order (low → high):**
1. Define `SceneSnapshot` type + COW arrays (Phase 1.1).
2. `SnapshotRing` triple-buffer with atomic publish (Phase 1.2).
3. Sim builds & publishes; BVH refit on Sim thread (Phase 1.3).
4. Tracer consumes from `SnapshotRing` instead of `PendingCommands` (1.4).
5. UI picking against snapshot (1.5); migration flag (1.6); tests (1.7);
   observability (1.8).
6. Refactor `IPathTracer::render_sample_batch` → `render_tile` (Phase 3.1).
7. Tile scheduler with variance-driven priority (3.2).
8. Reset/reproject decision tree (3.3).
9. Motion vectors (3.4).
10. Cancellation at tile granularity (3.5).
11. Multi-GPU sharding (3.6).
12. Backend parity: CPU dynamic acceleration, Vulkan timeline + cmd-buffer
    double-buffering (3.7); tests (3.8); observability (3.9).

**Exit gates:**
- Zero deep copies of `RTSceneData` on any per-frame path.
- Tracer reads only from `SnapshotRing.current()`; old mailbox API unused.
- Static scene accumulates across 100+ generations without resetting.
- One-mover scene: only mover's pixels reset, background keeps converging.
- Multi-GPU: 2 GPUs ≈ 1.8× single-GPU samples/sec on a fixed scene.
- `vkp.tracer.gen_lag` stays ≤ 2 in steady state.

**Public interfaces (delivered to C/D):**
- `SceneSnapshot` (immutable type C scripts read for world queries; D audio
  reads for spatialization).
- `SnapshotRing.current()` (any reader, lock-free).
- `FrameRing` (Tracer → UI; latest-wins).

---

### Track C — Sim Producers (Physics + Scripts)  *(mid level, parallel to B)*

**Mission:** remove blocking RPCs from physics; replace per-hook `lua_State`
churn with a persistent pool; route every world mutation through typed rings
into Sim. By the end, scripts and physics are pure producers and Sim is the
only writer.

**Owns:** Phase 2 (physics behind rings), Phase 5 (Lua pool + script thread),
Phase 6 (parallel scripts + JobSystem improvements + deterministic mode).

**Depends on:** A's `SpscRing`, `MpscRing`, JobSystem (existing, improved in
6.5), `VKP_LOG`, `VKP_METRIC_*`. Coordinates with B on which thread "Sim"
lives on (B owns Sim's snapshot publish path; C owns Sim's drain of physics
deltas + script cmds).

**Implementation order (low → high):**
1. Define `PhysicsCmd` / `PhysicsDelta` variants + rings (Phase 2.1, 2.2).
2. Physics thread loop with fixed sub-step + drain cmd ring (2.3).
3. Sim integrates deltas; remove `future.get()` callers (2.4, 2.5);
   tests (2.6); observability (2.7).
4. `LuaStatePool` keyed by script id; persistent VMs (Phase 5.1).
5. Script thread loop draining `ScriptHookRing` (5.2).
6. `ScriptCmd` variants + `ScriptCmdRing`; Sim drains at top of tick (5.3).
7. Snapshot-only reads from Lua bindings (5.4).
8. Yield/resume support (5.5); hot reload (5.6); tests (5.7);
   observability (5.8).
9. Group declaration (`pure = true`) + binding restriction (Phase 6.1).
10. Group-B JobSystem fan-out + state checkout/return (6.2, 6.3).
11. Deterministic mode flag + RNG seeding (6.4).
12. JobSystem improvements: work-stealing, bounded `m_jobs`, drain on
    shutdown (6.5); observability (6.6).

**Exit gates:**
- Zero `future.get()` calls in `src/physics/`.
- Zero `lua_open` / `luaL_newstate` calls on per-hook path; pool reuse only.
- Sim drains all cmd rings at tick start; no cross-thread mutation of world.
- 10k hook fires on one script → 1 `lua_State` created; memory stable.
- Determinism: 3 runs with `--deterministic` + identical input produce
  identical `determinism.snapshot.outputs_hash` stream.
- `vkp.physics.cmd_dropped_total` and `vkp.script.cmd_dropped_total` rates
  are 0 in steady state.

**Public interfaces (delivered to D):**
- `PhysicsCmd`, `PhysicsDelta` types and their rings.
- `ScriptCmd` type and `ScriptCmdRing` (D's audio cmds piggyback on the
  same producer model).
- `LuaStatePool` (D's audio scripts use it too if they exist).

---

### Track D — Audio + Integration + Cleanup  *(highest level)*

**Mission:** stand up the audio thread reading from B's snapshot and consuming
C's commands; finish cross-cutting Sim/UI observability; delete the legacy
coordinator + blocking paths; run acceptance.

**Owns:** Phase 4 (audio thread), Phase 7 (Sim/UI observability, cleanup,
docs, profiling, acceptance).

**Depends on:** A (logging incl. `VKP_LOG_RT` realtime path, metrics, health),
B (`SceneSnapshot` for spatialization, `FrameRing` for UI), C (`ScriptCmd`
for `PlaySound`).

**Implementation order (low → high):**
1. WASAPI device init + `IAudioDevice` abstraction (Phase 4.1).
2. Audio thread + RT priority + callback scaffolding (4.1).
3. Snapshot consumption inside callback (4.2).
4. `SoundRing` MPSC + per-voice PCM rings (4.3).
5. Spatializer (panning → HRTF later) (4.4).
6. Async loader/decoder thread (4.5); tests (4.6); observability (4.7)
   including the watchdog thread.
7. Sim observability: `sim.tick_*`, `sim.deadline_missed`, `sim.idle_*`
   events + metrics (Phase 7.3 sim half).
8. UI observability: `ui.input_to_pixel_us` end-to-end metric (7.3 UI half).
9. Sunset old paths: `RenderCoordinator::PendingCommands`,
   `ThreadedPhysicsWorld::run_on_worker`, per-hook `lua_State` open/close,
   `RTSceneData` deep-copies (Phase 7.1).
10. Docs: architecture diagram, channel table, per-thread "what runs
    here" doc, onboarding for new subsystems (7.2).
11. Profiling pass: TSan soak, multi-GPU scaling validation,
    determinism replay (7.4).
12. Acceptance criteria validation (final exit-gate run).

**Exit gates:**
- `vkp.audio.underruns_total` stays at 0 across a 10-minute soak.
- Sim stall test (500 ms) does not glitch audio.
- All legacy code paths from Sync-1's flag flip removed; build links without
  them.
- All Acceptance Criteria checkboxes (Performance + Observability) pass.

---

## Phasing

Each phase is independently shippable and observable. Phases are still
numbered low-to-high (foundations first), but actual ownership lives in the
Track Plan above — find your track, then read the phase sections it owns.

| Phase | Track | Subject                                          |
|-------|-------|--------------------------------------------------|
| 0     | A     | Foundations, primitives, telemetry               |
| 1     | B     | `SceneSnapshot` + `SnapshotRing`                 |
| 2     | C     | Physics behind rings                             |
| 3     | B     | Tile-granular tracer + reprojection              |
| 4     | D     | Audio thread                                     |
| 5     | C     | Persistent `lua_State` pool + script thread      |
| 6     | C     | Parallel "pure" scripts + determinism            |
| 7     | D     | Sim/UI observability, cleanup, sunset, acceptance|

---

## Phase 0 — Foundations & telemetry  *(Track A)*

Before touching subsystems, build the primitives every phase will reuse.

### 0.1 — Primitives (`src/core/sync/`)
- [x] Add `SpscRing<T>` (single-producer single-consumer, bounded, lock-free).
  Power-of-two capacity, sequenced indices, `try_push` / `try_pop`.
  → `src/core/sync/SpscRing.h` (namespace `vkpt::core::sync`).
- [x] Add `MpscRing<T>` (multi-producer single-consumer, bounded). Used by
  `SoundRing`. → `src/core/sync/MpscRing.h`. Vyukov bounded MPMC restricted
  to one consumer; wait-free pop, lock-free CAS-loop push.
- [x] Add `LatestSlot<T>` (single-slot, drop-old, latest-wins). Wraps a
  `std::atomic<T*>` swap with a release-store. Used by `FrameRing`.
  → `src/core/sync/LatestSlot.h`. Counts dropped publishes.
- [x] Add `AtomicSharedPtr<T>` shim (`std::atomic<std::shared_ptr<T>>` is C++20;
  fall back to a `std::shared_ptr` + mutex-protected pointer if compiler
  support is thin). → `src/core/sync/AtomicSharedPtr.h`.
- [x] Unit tests for each primitive: producer/consumer fuzz, drop accounting,
  ABA-resistance. → `tests/observability_smoke.cpp::RingPrimitivesPass`. TSan
  preset wired via `desktop-clang-tsan`.

### 0.2 — Logging primitives (`src/core/log/`)
- [x] `Logger` singleton with **async writer thread**. Producers push events to
  a per-thread SPSC ring; writer thread drains, formats, writes. Hot path
  never does I/O, never allocates beyond optional StrHeap for >31-byte strings.
  → `src/core/log/Log.{h,cpp}` (namespace `vkpt::core::log`, distinct from the
  legacy `vkpt::log` shim in `core/Logging.h`).
- [x] `VKP_LOG(level, comp, ev, ...)` macro. Variadic key=value args. Compiles
  to a single `enabled()` test + (if true) one `try_push` of a small POD into
  the per-thread ring. C++20 `__VA_OPT__` form.
- [x] Levels: `trace`, `debug`, `info`, `warn`, `error`, `fatal`. Default
  threshold = `info`. Per-component overrides via
  `--verbose=tracer:tile,audio:callback`.
- [x] Format selector: `--log-format=console|kv|json`. Console is ANSI-colored,
  human-readable; kv is grep-friendly; json is for agents.
- [x] **Per-thread crash ring**: each thread keeps the last 1024 events in a
  separate ring. On thread exit the ring is drained into a global graveyard
  so dump survives thread death. Signal handlers (SIGSEGV/SIGABRT/SIGINT/
  SIGTERM/SIGFPE/SIGILL) call `Logger::emergency_dump()` which writes the
  crash-ring contents to stderr in JSON before re-raising for default action.
- [x] Sampling: `VKP_LOG_SAMPLED(period_ns, ...)` for high-frequency events
  (per-tile, per-callback). Each call site has a thread-local last-emit-ns
  keyed by an addressed-of static token.
- [x] Burst-detection: writer maintains a per-(component,event) sliding window
  and collapses repeats above `burst_collapse_threshold_per_sec` into a single
  record with a `coalesced=N` field.
- [x] `VKP_LOG_RT` realtime variant — same wire format, RT-discipline-only;
  exists for grep-ability of audio/RT callsites in code review.

### 0.3 — Metrics primitives (`src/core/metrics/`)
- [x] `MetricsRegistry`: lock-free record path; mutex only on lookup. Three
  types: `Counter` (atomic monotonic), `Gauge` (atomic bit-pattern double),
  `Histogram` (64 log-spaced buckets, atomic per-bucket increments + atomic
  min/max via CAS). → `src/core/metrics/Metrics.{h,cpp}`.
- [x] Naming: `vkp.<component>.<metric>` enforced by convention. Examples:
  `vkp.sim.tick_hz`, `vkp.tracer.tile_latency_us`, `vkp.audio.underruns_total`.
- [x] `VKP_METRIC_INC`, `VKP_METRIC_SET`, `VKP_METRIC_OBSERVE` — wait-free,
  call-site cached pointer in a function-local static.
- [x] **Heartbeat thread**: every 1s, snapshots all metrics, emits one
  `metrics.heartbeat` event per component, and updates derived
  `<counter>_per_sec` rate gauges. Idempotent start/stop.
- [x] JSON dump via `MetricsRegistry::dump_json()` — used by REPL `metrics dump`
  and `--metrics-out=<path>` shutdown writer. Dock panel scrape-ready.
- [x] Dock panel UI (sparklines, rate columns) — deferred to Track D, since
  the dock lives in the Qt editor module.

### 0.4 — Tracing primitives
- [x] `VKP_TRACE_SCOPE(comp, name)` — RAII scope timer. Always records duration
  into the histogram metric `vkp.<comp>.<name>_us`. When the component is
  enabled via `--trace=<comp>`, also records a Chrome-tracing event.
  → `src/core/trace/Trace.{h,cpp}`.
- [x] `VKP_TRACE_FLOW(id)` — pushes a per-thread flow ID for the scope; future
  sites that consult `trace::flow::current_id()` can attach it to events for
  cross-thread correlation (Track B/C will wire reads).
- [x] Chrome-tracing JSON export — `TraceRecorder::dump_chrome(path)` writes
  the X-event-format JSON consumable by `chrome://tracing` and Perfetto.

### 0.5 — Health probes
- [x] `IHealthProbe` interface returning `Report{Status, reason}`. `FunctionProbe`
  adapter so subsystems can register a closure without subclassing.
  → `src/core/health/Health.{h,cpp}`.
- [x] `HealthRegistry` heartbeat thread: every 1s polls every probe, emits one
  aggregate `health.heartbeat` event, and on any status transition emits a
  per-probe `health.transition` event at the appropriate level (Error on
  failed, Warn on degraded, Info on recovered).

### 0.6 — Build/CI
- [x] TSan preset already exists (`desktop-clang-tsan` in `CMakePresets.json`).
  The Track A code compiles clean under it; soak validation deferred to
  Track D's profiling pass since other subsystems must also be TSan-clean.
- [x] `--deterministic` flag parsed by `cli::Parse`, captured in
  `ObservabilityFlags`, and surfaced to subsystems via the same flags struct
  for later wiring (Phase 6.4 / SnapshotRing flush policy).
- [x] Regression test: `pt_observability_smoke` — 6 producers × 100k events,
  ring fuzz, metrics exact-value checks, trace/health/repl/crash-ring
  coverage. Build:
  `cmake --build build/default --target pt_observability_smoke`.
  Run: `./build/default/bin/pt_observability_smoke.exe`.
  Current local verification: `ALL PASSED` on `build/default`.

### 0.7 — Console & runtime introspection
- [x] CLI flags parser → `src/core/cli/Flags.{h,cpp}`. Recognizes `--log-level`,
  `--log-format`, `--log-out`, `--verbose`, `--trace`, `--trace-out`,
  `--metrics-out`, `--deterministic`. Unknown flags pass through.
- [x] Auto-format: when stdout is not a TTY, default to `--log-format=json` so
  agents get parseable output without a flag.
- [x] REPL: `src/core/repl/Repl.{h,cpp}`. Built-ins: `help`, `metrics
  dump|reset|json`, `events dump-rings`, `health`, `tracer`/`snapshot`/`script`
  (stubs awaiting Track B/C/D to wire).
- [x] Top-level bootstrap: `src/core/Observability.{h,cpp}` — `Init()`,
  `InitFromFlags()`, `Shutdown()`. Spawns logger writer, metrics heartbeat,
  health heartbeat, and installs signal handlers. Persists metrics/trace
  outputs on Shutdown if requested.

---

## Phase 1 — `SceneSnapshot` + `SnapshotRing`  *(Track B)*

Replaces `RenderCoordinator::PendingCommands` and the double-deep-copy of
`RTSceneData` with an immutable, COW, atomically-published scene snapshot.

### 1.1 — Define `SceneSnapshot`
- [x] New header `src/scene/SceneSnapshot.h`. Immutable POD-of-spans/COW arrays.
- [x] Versioning fields: `generation`, `topology_revision`,
  `transform_revision`, `camera_revision`, `material_revision`, `wall_time_ns`.
- [x] COW arrays: `cow_vector<Vertex>`, `cow_vector<Index>`,
  `cow_vector<Instance>`, etc. — backed by `shared_ptr<const T[]>`. Reuse the
  prior snapshot's array when unchanged.
- [x] Precomputed BVH handle: snapshot owns the built/refit BVH (or a
  `shared_ptr` to it) so consumers never build.
- [x] Camera embedded in snapshot. Camera-only changes still publish a new
  snapshot, but reuse all geometry/BVH arrays unchanged.

### 1.2 — `SnapshotRing`
- [x] New header `src/scene/SnapshotRing.h`. Triple-buffer of
  `shared_ptr<const SceneSnapshot>` with `publish()` (single writer) and
  `current()` (any reader, lock-free).
- [x] Reader cache: each consumer keeps its last seen `generation`; `current()`
  returns the latest atomically.
- [x] Drop accounting: count snapshots that no consumer ever observed (sim
  produced too fast for tracer/audio to peek).
- [x] Surface snapshot drop counters in the metrics registry.

### 1.3 — Sim builds & publishes
- [x] In Sim's tick loop, build a new `SceneSnapshot` after physics merge +
  scripts + BVH refit. Reuse arrays from the previous snapshot when their
  revisions are unchanged.
- [x] BVH refit happens **on the Sim thread** before publish — tracer never
  builds.
- [x] Publish atomically. Old snapshot freed when last reader's `shared_ptr`
  drops.

### 1.4 — Tracer reads snapshot
- [x] Replace `RenderCoordinator::PendingCommands` consumption with
  `SnapshotRing.current()` at the top of each tile.
- [x] Compare local `(generation, topology_revision, transform_revision,
  camera_revision)` against snapshot's. Decide reset/reproject/continue
  (see Phase 3 for policy; for now: any change → reset).
- [x] Delete the `RTSceneData` deep-copy path in `RenderCoordinator.cpp:530, 569`.

### 1.5 — UI reads snapshot for picking
- [x] Replace UI's direct `qtScene` reads (for picking, gizmo placement) with
  `SnapshotRing.current()` peeks. Picking against the snapshot's BVH is now
  consistent with what the tracer sees.

### 1.6 — Migration
- [x] Behind a feature flag `--snapshot-bus`, run new path in parallel with old
  for one release cycle. Compare frames pixel-wise on a deterministic scene.
- [x] Flip default; remove old path in Phase 7.

### 1.7 — Tests
- [x] Snapshot equality test: same input → identical snapshot.
- [x] COW test: a snapshot that only changes one transform shares vertex/index
  arrays with its predecessor (pointer equality).
- [x] Ring fuzz: 1 producer at 60 Hz, 3 readers at varying rates, no torn reads,
  no use-after-free.

### 1.8 — Observability
**Events** (component = `snapshot`):
- `snapshot.published` — info, sampled to 1 Hz at info; debug for every publish.
  Fields: `gen`, `topo_rev`, `xform_rev`, `cam_rev`, `cow_reused_arrays`,
  `bytes_new`, `build_us`.
- `snapshot.dropped` — warn. Fields: `gen`, `reason` (`no_reader_caught_up` |
  `producer_overrun`).
- `snapshot.reader_lagging` — warn (one-shot per reader transition). Fields:
  `reader`, `lag_gen`, `latest_gen`.
- `snapshot.config` — info on init. Fields: `ring_capacity`, `cow_enabled`.

**Metrics** (`vkp.snapshot.*`):
- `publish_hz` (gauge), `publish_total` (counter).
- `gen_lag.tracer`, `gen_lag.audio`, `gen_lag.ui`, `gen_lag.script` (gauges).
  Lag = `latest_gen - last_observed_gen` per reader.
- `cow_reuse_ratio` (gauge, 0–1) — fraction of arrays reused vs newly allocated.
- `bytes_per_snapshot` (histogram).
- `build_us` (histogram) — time spent building snapshot on Sim thread.

**Health probe**: `degraded` if any reader's `gen_lag > 5` for >2s; `failed`
if Sim hasn't published in 2× target tick interval.

**Console**: `vkp dump --snapshot=current` writes the live snapshot to disk
for offline inspection / replay.

---

## Phase 2 — Physics behind rings  *(Track C)*

Eliminate `ThreadedPhysicsWorld::run_on_worker`'s blocking `future.get()` on the
caller path. Sim owns one direction, Physics owns the other.

### 2.1 — Define commands & deltas
- [x] `PhysicsCmd` variant: `ApplyForce`, `SetKinematic`, `Raycast`,
  `AddBody`, `RemoveBody`, `SetGravity`, etc. Each has a request id.
- [x] `PhysicsDelta` variant: `TransformWrite`, `ContactEvent`, `RaycastResult`,
  `SleepStateChanged`. Each carries the request id when a response.
- [x] Headers in `src/physics/Channels.h`.

### 2.2 — Rings
- [x] `PhysicsCmdRing` (Sim → Physics, SPSC).
- [x] `PhysicsDeltaRing` (Physics → Sim, SPSC).

### 2.3 — Physics thread loop
- [x] Replace the `packaged_task` queue with a fixed sub-step loop:
  drain `PhysicsCmdRing` → step world (240 Hz target, accumulator-based) →
  emit deltas to `PhysicsDeltaRing`.
- [x] Stop returning futures. Callers fire and forget; raycast results come
  back as deltas keyed by request id.

### 2.4 — Sim integrates deltas
- [x] At sim tick: drain `PhysicsDeltaRing`, apply transform writes to
  authoritative world, surface contact events to scripts via `ScriptHookRing`.
- [x] Diff threshold (currently 0.5mm) moves into Sim, not Physics.

### 2.5 — Remove blocking calls
- [x] Audit every `future.get()` in `src/physics/`. Replace with command +
  request-id pattern, or move the caller off the hot path.

### 2.6 — Tests
- [x] Physics worker hang test: stall the physics thread; sim must continue
  ticking, just without new transform deltas.
- [x] Determinism test: same cmd stream → same delta stream (fixed sub-step
  guarantees this).

### 2.7 — Observability
**Events** (component = `physics`):
- `physics.substep` — debug, sampled at 1 Hz at info. Fields: `dt_ms`,
  `bodies_active`, `contacts`, `step_us`.
- `physics.cmd_received` — debug. Fields: `cmd_type`, `req_id`.
- `physics.delta_emitted` — debug. Fields: `delta_type`, `entity_id`.
- `physics.cmd_dropped` — warn. Fields: `cmd_type`, `reason` (`ring_full`).
- `physics.deadline_missed` — warn. Fields: `target_hz`, `actual_hz`,
  `behind_steps`.
- `physics.config` — info on init. Fields: `target_hz`, `cmd_ring_capacity`,
  `delta_ring_capacity`, `gravity`.

**Metrics** (`vkp.physics.*`):
- `substep_hz` (gauge), `substep_total` (counter).
- `cmd_queue_depth` (gauge), `delta_queue_depth` (gauge).
- `cmd_dropped_total` (counter), `delta_dropped_total` (counter).
- `bodies_active` (gauge), `contacts_per_step` (histogram).
- `step_us` (histogram), `behind_steps` (gauge).

**Health probe**: `degraded` if `cmd_dropped_total` rate > 0/s; `failed` if
`substep_hz` < 50% of target for 2s.

**Cross-thread correlation**: every `physics.delta_emitted` carries the
snapshot `gen` of the sim tick that consumed it (filled in by Sim on drain).

---

## Phase 3 — Tile-granular tracer + reprojection  *(Track B)*

Decouple noise from motion. Continuous motion no longer pins accumulation at 1
spp.

### 3.1 — Tile loop
- [x] Add transitional `IPathTracer::render_tile(tile, frame)` API and CPU
  implementations.
- [x] Refactor `IPathTracer::render_sample_batch` fully into
  `IPathTracer::render_tile(snapshot, accum, tile, rng)`. Tile is a
  `(x, y, w, h, sample_index)` tuple.
- [x] Tracer worker: outer loop = "pick next tile from schedule", inner = call
  `render_tile`. Between tiles, peek `SnapshotRing.current().generation`.
- [x] If generation changed, run reset/reproject decision (3.3) and rebuild
  tile schedule.

### 3.2 — Tile scheduler
- [x] Start with simple round-robin tile scheduling.
- [x] Upgrade scheduler to prioritize tiles with highest variance / lowest
  sample count.
- [x] Foveated mode (optional): center tiles get more samples than edges.
- [x] On dirty tiles only: when transform delta affected only certain pixels
  (via motion vectors, see 3.4), schedule those first.

### 3.3 — Reset / reproject decision
- [x] **Topology changed** → full reset of accumulation buffer.
- [x] **Camera-only changed** → reproject prior frame: warp by inverse-old +
  new-camera matrices; keep history weight. Variance bumps but no flicker.
- [x] **Transforms-only changed** → per-pixel decision driven by motion vectors
  (3.4). Static pixels keep history; moving pixels reset.
- [x] **Material-only changed** → keep geometry samples but invalidate shading
  cache; samples re-shade on next pass.

### 3.4 — Motion vectors
- [x] Snapshot builder records per-instance previous/current transform pairs in
  `RenderSceneSnapshot::instance_motion` when an instance transform changes.
- [x] Sim emits per-instance prev/current transform pair into snapshot
  (`instance.prev_world_transform`, `instance.world_transform`).
- [x] Tracer rasterizes a coarse motion-vector buffer at snapshot publish (or
  on first tile after generation change). Reuse for reprojection.
- [x] Falls back to "full reset for moving pixels" if motion vectors unavailable.

### 3.5 — Cancellation
- [x] Tile granularity = cancellation granularity. No need for sub-tile
  cancellation.
- [x] Make tile sizing configurable.
- [x] Validate tiles complete in <2 ms with a runtime guard or smoke that fails
  on over-budget tiles.
- [x] Worst-case input-to-pixel latency: 1 tile + 1 present, regardless of how
  many samples are queued.

### 3.6 — Multi-GPU scaling
- [x] Tile scheduler is GPU-count-aware: shard tiles across N GPUs by id.
- [x] Each GPU maintains its own accumulation slice for its tiles; resolve step
  composites slices into a single display-frame handoff entry.
  - [x] Bounded CI proof: `pt_multi_gpu_accumulation_smoke` validates
    `MultiGpuAccumulation` rejects non-owned tiles, keeps per-GPU film slices
    isolated, resolves them into one film/display frame, and publishes the
    composite through `FrameHandoff`.
- [x] Adding a GPU = more tiles per second = lower noise for the same scene.
  No code changes in callers; this is the scaling story.
  - [x] Bounded CI proof: `pt_multi_gpu_accumulation_smoke` validates the
    deterministic scheduler cost model for a fixed scene: 1/2/4 simulated GPUs
    produce >=1.0x/>=1.8x/>=3.6x relative samples/sec with monotonically lower
    fixed-time noise.

### 3.7 — Backend parity
- [x] CPU `IRayAccelerator`: implement `plan_instance_transform_update` so CPU
  no longer rebuilds the BVH on every transform. Mirror the D3D12
  static+dynamic split (`D3D12GpuPathTracer.Scene.cpp:303`).
- [x] Vulkan backend: wire `VulkanBackend.cpp` simulated contract to the
  snapshot path. Use timeline semaphores; double-buffer command buffers; allow
  CPU/GPU overlap. Tested by `pt_vulkan_backend_snapshot_smoke`.
- [x] Vulkan path tracer: stop the "single-cmd, fence-block per sample" pattern
  (`src/gpu/VulkanGpuPathTracer.cpp:572-596`). Submit N tiles in flight, present
  via timeline semaphore. Covered by `pt_vulkan_gpu_submission_contract`;
  native hardware render execution remains device-dependent.

### 3.8 — Tests
- [x] Static scene: verify samples accumulate across 100+ snapshot generations
  without resetting (sim publishes new snapshots with no observable changes).
- [x] Camera-only sweep: reprojected frames should not flicker; variance bump
  measurable but bounded.
- [x] One-mover scene: only the mover's pixels reset; background continues
  accumulating.

### 3.9 — Observability
**Events** (component = `tracer`):
- `tracer.tile_done` — debug, sampled. Fields: `tile_id`, `gen`, `gpu_id`,
  `samples`, `tile_us`, `variance`.
- `tracer.gen_change` — info. Fields: `from_gen`, `to_gen`, `reason`
  (`topology` | `transform` | `camera` | `material`), `decision`
  (`reset` | `reproject` | `per_pixel`).
- `tracer.reset` / `tracer.reproject` — info. Fields: `gen`, `pixels_reset`,
  `pixels_kept`.
- `tracer.frame_published` — info. Fields: `gen`, `spp_avg`, `spp_min`,
  `spp_max`, `resolve_us`.
- `tracer.gpu_assigned` — info on init / GPU change. Fields: `gpu_id`,
  `vendor`, `name`, `tile_share`.
- `tracer.frame_dropped` — warn. Fields: `gen`, `reason`
  (`framering_full` | `cancelled`).

**Metrics** (`vkp.tracer.*`):
- `samples_per_sec` (gauge), `samples_total` (counter).
- `tiles_per_sec` (gauge), `tile_latency_us` (histogram).
- `spp_current` (gauge) — average samples-per-pixel for current snapshot.
- `gen_lag` (gauge) — how far behind Sim's published gen the tracer is.
- `reset_total`, `reproject_total`, `frame_published_total` (counters).
- `framering_drops_total` (counter).
- Per-GPU breakdown: `samples_per_sec.gpu0`, `samples_per_sec.gpu1`, …

**Health probe**: `degraded` if `gen_lag > 10`; `failed` if no frame published
in 1s of work-available.

**Multi-GPU sanity**: heartbeat emits one `tracer.gpu_balance` event with
per-GPU `samples_per_sec` so agents can detect a slow / hung GPU.

---

## Phase 4 — Audio thread  *(Track D)*

Audio is realtime, lock-free, snapshot-driven. The sim's listener and emitter
poses are already in the snapshot; audio just reads them.

### 4.1 — Device & thread
- [x] Pick a backend (WASAPI on Windows; abstract behind `IAudioDevice`).
- [x] Create the audio thread at device-open; set realtime priority on Windows
  via `AvSetMmThreadCharacteristics("Pro Audio")`.
- [x] Callback runs at ~5–10 ms buffer size. No allocs, no locks, no logging
  inside the callback.

### 4.2 — Snapshot consumption
- [x] At top of callback: one `SnapshotRing.current()` load → local
  `shared_ptr` for the duration of the buffer. Refcount bump only here, not
  per-sample.
- [x] Read listener pose + emitter poses from snapshot. If snapshot is the
  same as last callback, that's fine — pose is 16 ms stale, audio doesn't care.

### 4.3 — `SoundRing`
- [x] MPSC ring of `AudioCmd`: `PlayOneShot(asset_id, position)`,
  `StartLoop(handle, position)`, `StopLoop(handle)`, `SetVolume`.
- [x] Producers: Sim, Script. Consumer: Audio.
- [x] Streamed PCM: separate per-voice SPSC ring; loader thread fills, callback
  drains.

### 4.4 — Spatializer
- [x] HRTF or simple panning (start simple). Per-source distance attenuation +
  Doppler from snapshot velocity (sim provides per-instance velocity in
  snapshot for moving emitters).
- [x] Mix down to device buffer; clip soft.

### 4.5 — Loader / decoder
- [x] Background thread for asset decode (Ogg, FLAC). Pushes decoded PCM into
  per-voice rings. Never runs in the callback.

### 4.6 — Tests
- [x] Glitch test: stall sim for 500 ms; audio must continue with last
  snapshot, no underrun.
- [x] Drop test: producers spam `SoundRing` past capacity; oldest cmds dropped,
  callback never blocks.

### 4.7 — Observability
**Critical constraint**: the audio callback must NEVER format a string, lock
a mutex, or allocate. All logging from the callback uses
`VKP_LOG_RT(level, comp, ev, ...)` which only does a `try_push` of a
fixed-size POD record into the audio thread's event ring; the writer thread
formats it later.

**Events** (component = `audio`):
- `audio.callback` — debug, sampled to 1 Hz. Fields: `buf_frames`,
  `callback_us`, `voices_active`, `snapshot_gen`, `snapshot_age_ms`.
- `audio.underrun` — error. Fields: `frames_short`, `last_callback_us`.
  Always logged, never throttled.
- `audio.voice_started` / `audio.voice_stopped` — debug. Fields: `voice_id`,
  `asset_id`, `position`.
- `audio.cmd_dropped` — warn. Fields: `cmd_type`, `reason`.
- `audio.config` — info on init. Fields: `device`, `sample_rate`, `buf_frames`,
  `priority`.

**Metrics** (`vkp.audio.*`):
- `callback_latency_us` (histogram) — must stay well under buffer period.
- `callback_jitter_us` (histogram).
- `underruns_total` (counter) — should remain 0 in steady state.
- `voices_active` (gauge), `voices_started_total` (counter).
- `snapshot_age_ms` (gauge) — staleness of pose data; useful for diagnosing
  audible lag on fast-moving emitters.
- `soundring_depth` (gauge), `soundring_drops_total` (counter).

**Health probe**: `failed` on any underrun in the last 5s; `degraded` if
`callback_latency_us.p99 > 0.8 * buf_period`.

**Watchdog**: a separate thread monitors callback frequency. If callbacks stop
firing, emits `audio.callback_stalled` at error level with the device name and
last-callback-ts.

---

## Phase 5 — Persistent `lua_State` pool + script thread  *(Track C)*

Kill per-hook VM creation. Scripts become pure producers writing to
`ScriptCmdRing`.

### 5.1 — `lua_State` pool
- [x] `LuaStatePool` keyed by `script_id`. Create once at script load, cache
  forever.
- [x] On script reload (file changed): build new state in background, atomic
  swap pointer, decommission old after last hook drains.
- [x] Per-state: `lua_setallocf` budget, instruction-count hook, sandboxed
  library set. Reuse current `OpenSafeLuaLibraries`.
- [x] Bytecode cache feeds `loadbufferx` — never `loadfile` from hot path.

### 5.2 — Script thread
- [x] Single OS thread runs Group-A (ordered, world-affecting) hooks
  sequentially.
- [x] Drains `ScriptHookRing` for hook fire requests from Sim.
- [x] Each hook: lookup script's `lua_State`, push args, call, collect output
  commands.

### 5.3 — `ScriptCmdRing`
- [x] `ScriptCmd` variant: `SpawnEntity`, `DestroyEntity`, `SetTransform`,
  `ApplyForce`, `PlaySound`, `SetMaterial`, `EmitParticle`, etc.
- [x] Scripts append to a thread-local batch; flushed to `ScriptCmdRing` at
  end of hook.
- [x] Sim drains `ScriptCmdRing` at top of next tick before physics.

### 5.4 — Snapshot-only reads
- [x] Lua bindings that read world state (e.g. `entity:position()`) read from
  the **last published snapshot**, not from authoritative sim state. Keeps
  scripts decoupled from sim mid-tick.
- [x] Bindings that need fresher reads (rare) are flagged and run only
  inside Sim's tick window via deferred execution.

### 5.5 — Yield support
- [x] Long scripts call `coroutine.yield(); script thread saves the coroutine
  and resumes next frame.
- [x] Yield budget: per-script max yields before forced kill.

### 5.6 — Hot reload
- [x] File watcher → enqueue reload request → pool builds new state → atomic
  swap. In-flight hooks finish on the old state.

### 5.7 — Tests
- [x] State reuse: 10k hook fires on the same script → 1 `lua_State` created,
  not 10k. Memory should not climb.
- [x] Concurrent reload: trigger reload while hook is running; old state
  remains valid until hook returns.

### 5.8 — Observability
**Events** (component = `script`):
- `script.loaded` — info. Fields: `script_id`, `path`, `bytecode_size`,
  `pure`, `load_us`.
- `script.reloaded` — info. Fields: `script_id`, `prev_state_ptr`,
  `new_state_ptr`, `inflight_hooks`.
- `script.hook_fired` — debug, sampled. Fields: `script_id`, `hook`, `gen`,
  `args_count`.
- `script.hook_completed` — debug, sampled. Fields: `script_id`, `hook`,
  `hook_us`, `instructions`, `mem_bytes`, `cmds_emitted`.
- `script.hook_killed` — warn. Fields: `script_id`, `hook`, `reason`
  (`instruction_budget` | `memory_budget` | `yield_budget`), `inst_count`,
  `mem_bytes`.
- `script.lua_error` — error. Fields: `script_id`, `hook`, `error_msg`,
  `traceback`.
- `script.yielded` — debug. Fields: `script_id`, `hook`, `yield_count`.
- `script.cmd_dropped` — warn. Fields: `cmd_type`, `script_id`.
- `script.bytecode_cache_hit` / `script.bytecode_cache_miss` — debug. Fields:
  `script_id`, `cache_size_bytes`.

**Metrics** (`vkp.script.*`):
- `hooks_per_sec` (gauge), `hooks_total` (counter).
- `hook_us` (histogram).
- `lua_state_count` (gauge), `lua_state_mem_bytes` (gauge, sum across pool).
- `cmd_ring_depth` (gauge), `cmd_dropped_total` (counter).
- `hook_killed_total` (counter, by reason label).
- `bytecode_cache_hit_rate` (gauge), `bytecode_cache_bytes` (gauge).
- `reloads_total` (counter).
- `lua_errors_total` (counter, by script_id label).

**Per-script panel**: dock UI shows a table with columns `script_id`,
`hooks/s`, `p99 hook_us`, `inst_count`, `mem_kb`, `last_error`, `state_ptr`.
Lets you spot the script that's burning your frame budget at a glance.

**Health probe**: `degraded` if any script's kill rate > 1/min;
`failed` on any `lua_error` in the last 10s of a Group-A script.

---

## Phase 6 — Parallel "pure" scripts + deterministic mode  *(Track C)*

### 6.1 — Group declaration
- [x] Script preamble: `pure = true` makes the script eligible for Group-B
  parallel execution. Default is `false` (Group-A).
- [x] Group-B scripts may not call mutating bindings (`SpawnEntity`,
  `ApplyForce`, etc.); enforced at binding-table-build time (different
  binding set).

### 6.2 — JobSystem integration
- [x] Group-B fan-out: `script_thread` enqueues N jobs into JobSystem; each
  worker checks out a `lua_State` from the pool, runs the hook, returns the
  state, pushes a `ScriptCmd` batch.
- [x] `script_thread` waits for all batches, merges into a single
  `ScriptCmdRing` push (preserves ordering predictability).

### 6.3 — `lua_State` pool sizing
- [x] Pool grows to `min(num_workers, num_pure_scripts)`. Each `lua_State` is
  thread-local for the duration of a job; never crosses threads mid-call.

### 6.4 — Deterministic mode
- [x] `--deterministic` flag forces:
  - JobSystem: `m_serialMutex` mode, single chunk per range.
  - SnapshotRing: flush per tick (no skipped generations from Sim's POV).
  - Script scheduler: Group-B disabled; everything runs on script-main in
    stable order.
  - RNG: fixed seed per snapshot generation.
- [x] Same input + same flag → bit-identical snapshot stream. Used by tests.

### 6.5 — JobSystem improvements (drag along)
- [x] Add work-stealing to `JobSystem` workers. Current FIFO + helper-wait can
  deadlock under recursive waits.
- [x] Bound `m_jobs` map: retire fire-and-forget jobs after completion, not at
  `wait()`.
- [x] Drain `m_queue` on shutdown, or document the drop.

### 6.6 — Observability
**Events** (component = `jobs`):
- `jobs.scheduled` — debug, sampled. Fields: `job_id`, `group`, `worker_hint`.
- `jobs.started` — debug, sampled. Fields: `job_id`, `worker_id`,
  `wait_us` (queue time).
- `jobs.done` — debug, sampled. Fields: `job_id`, `worker_id`, `run_us`.
- `jobs.stolen` — debug. Fields: `job_id`, `from_worker`, `to_worker`.
- `jobs.queue_full` — warn. Fields: `worker_id`, `depth`.
- `jobs.shutdown_drained` — info. Fields: `dropped_count`.

**Metrics** (`vkp.jobs.*`):
- `queue_depth.total` (gauge), `queue_depth.worker<N>` (gauges).
- `workers_busy` (gauge).
- `steal_rate` (gauge) — steals/sec; high steal = good load balance.
- `wait_us` (histogram), `run_us` (histogram).
- `jobs_scheduled_total`, `jobs_completed_total`, `jobs_stolen_total`
  (counters).

**Determinism mode events** (component = `determinism`):
- `determinism.snapshot` — info, every snapshot. Fields: `gen`, `rng_seed`,
  `inputs_hash`, `outputs_hash`. Lets a replay tool diff bit-by-bit.
- `determinism.config` — info on init. Fields: flags forced, modes engaged.

**Health probe** (jobs): `degraded` if `workers_busy < 1` while
`queue_depth.total > workers_count` (means workers are starving).

---

## Phase 7 — Cleanup & sunset  *(Track D)*

### 7.1 — Delete the old path
- [x] Remove `RenderCoordinator::PendingCommands` and `post_*` mailbox API.
- [x] Remove `ThreadedPhysicsWorld::run_on_worker` blocking path.
- [x] Remove per-hook `lua_State` open/close in `ScriptRuntime.cpp:1964, 2128`.
- [x] Remove the `RTSceneData` deep-copy path entirely.

### 7.2 — Documentation
- [x] Update `CLAUDE.md` (or add `docs/architecture.md`) with the
  snapshot-bus diagram and channel table.
- [x] Per-thread "what runs here" doc: UI, Sim, Physics, Script, Tracer, Audio.
- [x] Onboarding: how to add a new subsystem (must publish a ring or read the
  snapshot; never share mutable state).

### 7.3 — Sim & UI observability (cross-cutting)
Sim and UI don't have their own phase, but their observability matters too.

**Sim events** (component = `sim`):
- `sim.tick_started` — debug, sampled. Fields: `tick_id`, `target_dt_ms`.
- `sim.tick_completed` — debug, sampled. Fields: `tick_id`, `dt_ms`,
  `cmds_drained`, `deltas_drained`, `snapshot_gen`.
- `sim.deadline_missed` — warn. Fields: `tick_id`, `target_dt_ms`,
  `actual_dt_ms`, `behind_by_ms`.
- `sim.idle_entered` / `sim.idle_exited` — info. Fields: `reason`,
  `last_change_gen`.
- `sim.config` — info. Fields: `target_hz`, `max_catchup_steps`.

**Sim metrics** (`vkp.sim.*`):
- `tick_hz` (gauge), `tick_us` (histogram), `tick_total` (counter).
- `deadline_misses_total` (counter), `behind_by_ms` (gauge).
- `cmds_per_tick` (histogram), `deltas_per_tick` (histogram).
- `idle_ratio` (gauge, 0–1).

**UI events** (component = `ui`):
- `ui.frame_blitted` — debug, sampled. Fields: `frame_gen`, `paint_us`.
- `ui.input_received` — debug. Fields: `input_type`, `seq`.
- `ui.input_to_pixel_us` — info, sampled. Fields: `input_seq`,
  `latency_us`. End-to-end latency from input → first pixel reflecting it.
- `ui.repaint_dropped` — warn. Fields: `reason`.

**UI metrics** (`vkp.ui.*`):
- `repaint_hz` (gauge), `paint_us` (histogram).
- `input_to_pixel_us` (histogram, p50/p95/p99) — **the headline UX metric.**
- `frame_age_ms` (gauge) — how stale is the frame UI is showing.

### 7.4 — Profiling pass
- [ ] Run with TSan one more time across all phases.
  Verified 2026-05-08: `cmake --preset desktop-clang-tsan` configures on this
  Windows host, but `cmake --build --preset desktop-clang-tsan --target
  pt_observability_smoke` fails at compile with `unsupported option
  '-fsanitize=thread' for target 'x86_64-pc-windows-msvc'`. Keep this open for
  Linux/self-hosted sanitizer coverage.
  Re-verified 2026-05-08 after the six-agent contract integration with the
  same configure/build commands and the same Windows Clang blocker.
- [x] Profile with the in-app metrics dock: per-thread cadence stable, ring
  drops near zero in steady state, no ring overflow under stress.
  - [x] Bounded CI proof: `pt_observability_smoke` validates lifecycle,
    heartbeat, anomaly JSON events for the current component set; verifies
    `BuildQtMetricsDock` scrapes `vkp.*` metrics with rate, p95/p99, and
    sparkline fields; records per-thread cadence metrics; and checks bounded
    threaded logging has zero ring drops/overflows.
- [ ] Multi-GPU: scale from 1→2→4 GPUs on a fixed scene; samples/sec should
  scale near-linearly; no UI/sim regression.
  - [x] Bounded CI proof: `pt_multi_gpu_accumulation_smoke` runs the
    deterministic 1/2/4-GPU scheduler/accumulation contract without hardware.
  - [ ] Hardware gate: run the fixed scene on real 1/2/4 GPU adapters before
    claiming measured near-linear hardware scaling.
    Verified 2026-05-08 on this host: `pt_multi_gpu_accumulation_smoke`
    passed and Windows enumerates two display adapters (Intel UHD Graphics
    770 and Intel Arc B580). `ptapp` now builds in both default and Qt debug
    presets, but the real 1/2/4-adapter fixed-scene gate remains blocked here
    because this host does not expose four adapters.

---

## Cross-cutting observability backlog (post-Track-A enrichment)

Track A landed the primitives (`VKP_LOG`, `VKP_METRIC_*`, `VKP_TRACE_SCOPE`,
`IHealthProbe`, REPL, signal handlers). The original Phase 1–7 sections cover
*new* subsystems built under the snapshot-bus model — but the **existing**
subsystems still use legacy `vkpt::log` and expose no metrics. An agent
debugging a hang, a perf regression, or a wrong-output bug today still has to
attach a debugger or grep source.

This section is the backlog of granular, agent-debuggability TODOs to bring
**every** long-lived subsystem up to the Observability Contract below. Most
items are 30-line edits. Each is tagged with the Track that owns it.

### Subsystem Observability Contract (the standard every subsystem must meet)

For *every* long-lived subsystem in `src/`, an agent reading the logs alone
must be able to answer: *"is it healthy now? what did it do last second? what
did it last fail at, and why?"* Concretely, that means the subsystem MUST
provide:

1. **Lifecycle events** — `<comp>.started`, `<comp>.stopped`, `<comp>.config`
   with backend/feature flags as fields.
2. **Per-step or per-frame anomaly events** — failures, fallbacks, deadline
   misses, drops, retries, with a `reason` field. Always at `warn` or above.
3. **A `<Component>Status` query API** — synchronous getter returning a typed
   POD with current health + last-tick stats + last-error reason. Used by
   REPL, dashboards, and tests.
4. **Metrics in `vkp.<component>.<name>`** — at minimum: one rate counter,
   one duration histogram, one queue/depth gauge.
5. **An `IHealthProbe`** — registered with `HealthRegistry` at startup;
   returns `Ok|Degraded|Failed` based on the component's own SLA thresholds.
6. **A flow-ID join point** — any per-frame work calls `VKP_TRACE_FLOW(gen)`
   so cross-thread events stitch through the snapshot generation.

A subsystem isn't "done" until it passes a checklist run of all six.

### New shared structured types (proposed under `src/core/contracts/`)

These let subsystems publish their state in a uniform shape that REPL,
dashboards, and tests can all consume without per-subsystem glue.

- [x] `SubsystemStatus` (`src/core/contracts/SubsystemStatus.h`):
  `{ name, status, started_at_ns, last_tick_ns, last_error,
     ticks_total, errors_total, custom_fields[] }`. *(Track A — small
     follow-up; lays the type used by every TODO below.)*
- [x] `LifecycleEvent` helper macros: `VKP_LIFECYCLE_STARTED(comp, fields...)`,
  `VKP_LIFECYCLE_STOPPED(...)`, `VKP_LIFECYCLE_CONFIG(...)`. Wrap `VKP_LOG`
  with the standard event names. *(Track A.)*
- [x] `IFlowSource` interface — anything that emits cross-thread work
  (snapshot publisher, command ring producer) implements
  `current_flow_id()` so consumers can pick it up and call
  `VKP_TRACE_FLOW`. *(Track A.)*
- [x] `MetricsBundle<T>` helper — registers a small group of metrics under
  one prefix at construction; subsystem then calls `bundle.tick.inc()`
  without restating the prefix. Reduces metric-name typos. *(Track A.)*

### Per-subsystem backlog

Each section below: existing surface, gaps an agent would hit immediately,
and the TODOs to close them. File refs are entry points, not exhaustive.

#### Render — `src/render/`  *(Track B)*

`RenderCoordinator` already maintains a hand-rolled `RenderCoordinatorStats`
struct with ~28 counters and `FrameHandoff` carries monotonic frame IDs.
Almost none of it reaches the Observability stack.

- [x] Mirror `RenderCoordinatorStats` into the metrics registry: every
  scalar field becomes `vkp.render.<field>` (gauge or counter), updated at
  the end of each tick. One-time wiring; subsequent reads are free.
- [x] Replace `tile_latency_last_us` / `tile_latency_max_us` gauges with
  `vkp.render.tile_latency_us` histogram (p50/p95/p99 land for free).
- [x] Emit `render.frame_published` (info, sampled to 1 Hz) and
  `render.frame_dropped` (warn, always-on) from `FrameHandoff`. Drop event
  needs a `reason` field (`framering_full | cancelled | resolve_failed`).
- [x] Emit `render.snapshot_consumed` (debug, sampled) carrying snapshot
  `gen` so the flow ID propagates from Sim → Tracer → UI.
- [x] Implement `RenderCoordinatorStatus` (subsystem status type) and a
  REPL handler `render status` that prints it.
- [x] Register a `render` health probe: `Failed` if no frame published in
  1s of work-available; `Degraded` if `gen_lag > 5`.

#### Path tracer — `src/pathtracer/`  *(Track B)*

`IPathTracer` already has a clean transactional API; `SampleCounters` is
collected per-frame but never emitted. `FilmBuffer` has no instrumentation.

- [x] Wire `SampleCounters` into metrics: `vkp.pathtracer.bvh_node_visits`,
  `vkp.pathtracer.triangle_tests`, `vkp.pathtracer.ray_count` as
  render-tile deltas.
- [x] Emit `pathtracer.accumulation_reset` (info) on every
  `reset_accumulation()` call with snapshot `gen` and `reason`
  (`topology|transform|camera|material|external`).
- [x] Emit `pathtracer.scene_delta_applied` (debug) with counts of materials,
  lights, and instances changed.
- [x] Add a `IPathTracer::status()` returning a typed
  `PathTracerStatus { backend, last_sample_us, total_samples,
   accumulation_gen, last_error }`.
- [x] Add `vkp.pathtracer.sample_us` histogram around
  `render_sample_batch` (or `render_tile` once Track B 3.1 lands).

#### GPU backends — `src/gpu/`, `src/render/backends/`  *(Track B)*

`VulkanGpuPathTracer` and `D3D12GpuPathTracer` are essentially black boxes:
no shader-load, dispatch, or fence-wait telemetry leaks out.

- [x] Emit `gpu.shader_compiled` (info, on first compile) and
  `gpu.shader_cached` (debug, on cache hit) with shader name + bytes +
  compile_us.
- [x] Emit `gpu.dispatch_submitted` and `gpu.dispatch_completed` (sampled)
  with compute group counts and `submit_us`.
- [x] Add `vkp.gpu.fence_wait_us` histogram around every fence/timeline
  semaphore wait — this is the #1 missing signal for "GPU stall" debugging.
- [x] Add `vkp.gpu.device_memory_bytes` gauge updated on each upload + a
  warn event when within 90 % of device limit.
- [x] `IGpuBackend::introspect()` returning
  `{ adapter_name, vram_bytes_used, vram_bytes_total,
     pending_dispatches, last_present_us }` — driver-portable.
- [x] Health probe: `Failed` on any device-lost / fence-timeout in 5s;
  `Degraded` if VRAM > 90 %.

#### CPU path — `src/cpu/`  *(Track B)*

Tile parallelism is invisible: merge cost, BVH-build cost, and SIMD
selection are all silent.

- [x] `cpu.simd_selected` (info, one-shot at startup) — fields: `kernel`
  (`avx2|avx512|neon|scalar`), `reason` (`cpu_feature|forced|fallback`).
- [x] `vkp.cpu.tile_render_us` histogram per tile.
- [x] `vkp.cpu.tile_merge_us` histogram around the film-merge barrier in
  `TiledCpuPathTracer`. Merge cost is currently invisible; this is the
  highest-value single addition.
- [x] `vkp.cpu.bvh_build_us` histogram in `ParallelBvhBuilder`.
- [x] `bvh.build_completed` event carrying `node_count`, `prim_count`,
  `worker_count`.
- [x] Status: `CpuPathStatus { kernel, last_tile_us_p99, last_build_us }`.

#### Scene + SnapshotRing — `src/scene/`  *(Track B, partial)*

Snapshot publish, COW reuse, and stage timing are all collected but
unread. `FrameLifecycleController` measures per-stage durations that no
caller queries.

- [x] Emit `snapshot.published` from `SnapshotRing` carrying `gen`,
  `cow_reused_arrays`, `bytes_new`, `build_us` — already partially in
  Phase 1.8, ensure wiring lands.
- [x] Wire every `FrameStageTiming` element to a histogram named
  `vkp.scene.stage_<name>_us`. Removes the "where did the frame budget
  go?" mystery.
- [x] Emit `scene.stage_overrun` (warn) when any stage exceeds a
  per-stage threshold (config-driven).
- [x] Add `vkp.scene.entity_count` / `vkp.scene.transform_dirty_count`
  gauges, sampled per tick. Lets agents see ECS churn.
- [x] Add a REPL handler `scene stages` that prints the latest
  `FrameStageTiming` array — replaces ad-hoc printf debugging.

#### Scripting — `src/scripting/`  *(Track C)*

Hooks fire silently; budget exhaustion truncates execution with no event;
per-hook timing is collected but unread.

- [x] Emit `script.hook_fired` (debug, sampled) with `script_id`, `hook`,
  `gen`. Useful even before Phase 5 lands.
- [x] Emit `script.budget_exceeded` (warn, always) with which budget
  (`instructions|memory|yield`) and the actual usage. **Top-priority
  addition** — silent truncation is the worst kind of bug.
- [x] `vkp.script.hook_us{hook=onUpdate}` histogram per hook type
  (label-based).
- [x] `vkp.script.instructions_per_frame` histogram.
- [x] Status: `ScriptingStatus { active_scripts, hooks_fired_total,
  budget_kills_total, last_error_script_id }`.
- [x] `script list` REPL handler that prints the per-script panel
  envisioned in Phase 5.8.

#### Physics — `src/physics/`  *(Track C)*

`PhysicsWorld` returns `Result<void>` from `step_fixed` but never logs why
a step failed. `PhysicsSyncSummary` is computed and discarded.

- [x] Emit `physics.step_failed` (error, always) with `frame_idx`,
  `error_message`, current `body_count` — first thing an agent will need.
- [x] Emit `physics.sync_drift` (warn) when `enabled_bodies` differs from
  expected `ecs_entities` by >0; specify both counts.
- [x] `vkp.physics.step_us` histogram, `vkp.physics.body_count` gauges
  (per type), `vkp.physics.contacts_per_step` histogram.
- [x] Status: `PhysicsStatus { backend, fixed_dt_ms, body_counts{},
  last_step_us, last_error }`.
- [x] Wire `IFlowSource::current_flow_id()` so contact deltas published
  back to Sim carry the snapshot `gen` they will land in.

#### Jobs — `src/jobs/`  *(Track C)*

A job that throws is silently dropped (caller hangs on `wait`); worker
saturation has zero telemetry.

- [x] Wrap every job lambda in a try/catch that emits `jobs.job_threw`
  (error, always) with the job-id and `what()` string.
- [x] `vkp.jobs.queue_depth.worker<N>` gauges and an aggregate
  `vkp.jobs.queue_depth.total`.
- [x] `vkp.jobs.run_us` histogram, `vkp.jobs.wait_us` (queue-time)
  histogram.
- [x] `jobs.deterministic_mode` lifecycle event whenever the mode
  toggles, carrying the configured seed if any.
- [x] Health probe: `Failed` when `workers_busy == 0` and
  `queue_depth.total > workers_count` for >2s (worker starvation /
  deadlock signature).

#### Audio — `src/audio/`  *(Track D)*

Voice allocation, event delivery, and backend buffer health are entirely
invisible today.

- [x] Emit `audio.voice_allocated` / `audio.voice_freed` (debug) with
  `asset_id` and `voice_count`.
- [x] Emit `audio.voice_allocation_failed` (warn) with the reason
  (`voices_full|asset_missing|backend_error`).
- [x] Emit `audio.event_dropped` (warn) when `post_event` returns false.
- [x] `vkp.audio.voices_active` (gauge), `vkp.audio.events_posted_total`
  (counter), `vkp.audio.underruns_total` (counter — even before
  Phase 4.7 lands, the legacy backend can report).
- [x] Status: `AudioStatus { backend, sample_rate, voices_active,
  voices_max, last_underrun_ns }`.

#### Assets — `src/assets/`  *(cross-cutting; assign Track D)*

Cache hits/misses are silent; load failures lack source attribution.

- [x] Emit `assets.load_started` (debug), `assets.load_completed`
  (debug, sampled) with `asset_id`, `kind`, `bytes`, `load_us`,
  `cache_hit`.
- [x] Emit `assets.load_failed` (error, always) with `asset_id`,
  `path_or_urn`, `reason`, and the *requesting* asset id (so dependent
  loads are traceable).
- [x] `vkp.assets.cache_hit_total` / `cache_miss_total` counters →
  derived `cache_hit_rate` gauge.
- [x] `vkp.assets.load_us` histogram, `vkp.assets.in_flight` gauge.
- [x] Status: `AssetsStatus { in_flight, total_bytes_loaded,
  cache_hit_rate, last_failure }`.

#### App / Qt + Headless — `src/app/`, `src/platform/`  *(Track D)*

UI event processing and platform fallback decisions are unlogged.

- [x] Emit `platform.selected` (info, one-shot) with `kind`, `backend`,
  `unavailable_reasons[]`.
- [x] Emit `platform.fallback` (warn) when a requested backend is
  unavailable and the system substitutes another.
- [x] Emit `ui.input_event` (debug, sampled) with `event_type`, `seq`,
  `processing_us`. Pairs with `vkp.ui.input_to_pixel_us` (Phase 7.3 UI
  half) to give end-to-end input latency.
- [x] `vkp.ui.event_queue_depth` gauge, `vkp.ui.repaint_us` histogram.
- [x] Status: `UiStatus { backend, repaint_hz, last_event_ns,
  event_queue_depth }`.

#### Diagnostics — `src/diagnostics/`  *(Track A follow-up)*

`CrashRecorder` and `StatusFile` exist but are write-only; their content
isn't queryable at runtime, and `StatusFile` only updates at exit.

- [x] On graceful shutdown, also call `Logger::dump_crash_rings()` and
  write to the same artifact dir as the StatusFile, not just on signal.
- [x] Have `CrashRecorder` register an `IHealthProbe` reporting
  `Degraded` while any non-empty crash record is unflushed.
- [x] `StatusFile` should update every N seconds (config) so an
  out-of-process agent can poll the live state — currently agents must
  parse the live log stream.
- [x] `vkp.diagnostics.crashes_total` counter, bumped on every signal
  hit; `vkp.diagnostics.last_crash_ns` gauge.

#### Editor / Qt panels — `src/app/`, `src/editor/`  *(Track D)*

Dock panels (the agent-friendly inspection surface) currently can't render
metrics; `MetricsRegistry::dump_json()` is unwired to UI.

- [x] Add a "Metrics" dock panel that scrapes `MetricsRegistry` every
  500 ms and renders gauges + counter rates + histogram p50/p95/p99.
  Replaces `metrics dump` in REPL with a live view.
- [x] Add an "Events" dock panel showing the live tail of the logger
  (filterable by component / level). Pairs with `--verbose` so an agent
  driving the editor can leave hot events on without console spam.
- [x] Add a "Health" dock panel listing every registered probe with
  status + last reason.

### Adoption checklist (per subsystem)

When a subsystem owner finishes their backlog, tick the box in the
"contract met" matrix below. A row that's all-green means the subsystem is
agent-debuggable end-to-end.

| Subsystem    | Lifecycle | Anomaly | Status() | Metrics | Probe | Flow ID |
|--------------|-----------|---------|----------|---------|-------|---------|
| render       | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| pathtracer   | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| gpu          | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| cpu          | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| scene        | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| scripting    | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| physics      | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| jobs         | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| audio        | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| assets       | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| app/ui       | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| platform     | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |
| diagnostics  | [x]       | [x]     | [x]      | [x]     | [x]   | [x]     |

---

## Cross-cutting subsystem contract backlog (post-Track-A enrichment)

The observability backlog above is about **what subsystems emit**. This
section is about **the shape of the interfaces between them**. Today the
codebase is well-structured at the component level (e.g. `IPathTracer`,
`SnapshotRing`, `IRenderBackend` are all clean) but the *integration
seams* show friction with the snapshot-bus model:

- Error returns are inconsistent (`bool`, `Result<T>`, `Summary{}`,
  silent void+log all coexist). Callers can't tell whether to check, log,
  or recover.
- Most subsystems have **no `status()` query**. An agent has to grep the
  log stream to find "what is this thing doing right now?" instead of
  asking it directly.
- Lifecycle state machines are **implicit** — undocumented orderings
  between configure / load / build / step / reset.
- Determinism is a **flat bool per subsystem** instead of a single
  `DeterminismContext` propagated through the system.
- Physics, audio, and scripting still emit **imperative writes/events**
  rather than typed, generationally-versioned snapshots — so they fight
  the bus design instead of riding it.

This is the contract-level backlog to close those gaps. Each item is the
sort of change that breaks an ABI but unblocks the rest of the migration,
so they're best landed early in the track that owns them — before
downstream consumers cement habits around the current shape.

### Standard contract conventions (the new house style)

Adopt these uniformly so an agent reading any subsystem's header can
predict its shape without reading the .cpp.

#### Error model — one shape

- [x] Add `vkpt::core::Result<T>` (or formalize the existing one) and
  `vkpt::core::Status` in `src/core/contracts/Result.h`. Status carries
  `{ Code code; std::string message; std::vector<std::string>
  warnings; std::optional<std::string> recovery_hint; }`. Code is an enum:
  `Ok | Unsupported | InvalidArgument | NotReady | Busy | AllocFailed |
  Timeout | Cancelled | InternalError`.
- [x] Convention: any operation that can fail returns `Result<T>` (or
  `Status` if `T = void`). Operations that *cannot* fail return their
  value directly. `bool` is reserved for boolean predicates only — never
  for error reporting.
  Verified 2026-05-09: ~10 fail-able ops converted to Result<T>/Status
  across render (IRenderBackend::initialize/shutdown across Vulkan, D3D12,
  Null, Adapter backends; IShaderCompiler::compile_compute_shader across
  all four), audio (IAudioDevice::open/start across Noop and Miniaudio
  devices), and editor (IUiRenderer::initialize); 2 of 3 audited throws
  converted (CrashRecorder helper now returns Result<string> instead of
  throwing; ThreadedPhysicsWorld constructor sets Failed lifecycle
  instead of throwing, with a new CreatePhysicsWorldResult factory for
  first-class error reporting). The third throw (JobSystem chain
  predecessor) was retained because it lives inside a worker lambda
  that uses std::exception_ptr propagation — replacing it would break
  the JobSystem's failure-propagation contract. Predicates correctly
  skipped: SpscRing/MpscRing try_*, JobSystem try_pop_queued_job /
  try_run_one_queued_job, IShaderCache::query ("returns false on miss"
  per state-machine grid), IUiPlatformBridge::open_file_dialog
  (cancellation, not error). Platform IWindow::initialize and
  poll_events were already wrappers over *_status variants
  (compatibility shims with first-class Status returns elsewhere) and
  were left untouched. Build passed; smoke suite green (observability,
  scripting, snapshot_bus, scene_contract, physics_contract, job_health,
  platform_contract, multi_gpu_accumulation).
- [x] Provide a `VKP_TRY(expr)` macro to early-return on
  `Result::is_error()`. Optional but reduces boilerplate.

#### `status()` query API — every subsystem ships one

- [x] Define `vkpt::core::contracts::ComponentLifecycle` enum
  `{ Uninitialized, Initializing, Ready, Busy, Degraded, Failed,
  ShuttingDown }` in `src/core/contracts/Lifecycle.h`.
- [x] Define a per-subsystem typed status struct in the same header as
  the subsystem's interface. Naming convention:
  `<SubsystemName>Status`. Mandatory fields: `lifecycle`, `last_error`,
  `last_tick_ns`, `ticks_total`, `errors_total`. Subsystem-specific
  fields follow.
- [x] Each subsystem's interface gains `virtual <Name>Status status()
  const = 0;`. Cheap snapshot read; never blocks; never side-effects.
  Bounded proof: the observability and contract matrices now have Status()
  coverage for every listed subsystem, verified by `pt_observability_smoke`,
  `pt_snapshot_bus_smoke`, `pt_scene_contract_smoke`,
  `pt_script_dispatch_contract_smoke`, `pt_physics_contract_smoke`,
  `pt_job_health_smoke`, `pt_scripting_smoke`, and
  `pt_vulkan_gpu_submission_contract`.
- [x] REPL gains a uniform `<comp> status` handler that prints the
  status struct. Track A's REPL already has stubs to wire here.

#### Lifecycle state machines — documented and validated

- [x] Each interface header documents its state machine as a comment
  table: rows = states, columns = methods, cells = "ok | error | noop |
  illegal". Example for `IPathTracer`:
  ```
  state\method      configure  load_scene  build_acc  render_tile  reset
  Uninitialized     →Configured illegal     illegal    illegal      noop
  Configured        ok          →SceneLoaded illegal   illegal      noop
  SceneLoaded       illegal     ok          →Ready     illegal      noop
  Ready             illegal     →SceneLoaded ok        ok           ok
  ```
  Verified 2026-05-09: state-machine tables added/expanded across listed
  interface headers; assert_state helper extended with subsystem-specific
  overloads in src/core/contracts/Lifecycle.h. Build passed.
- [ ] Add a debug-build `assert_state(method, allowed_states)` helper
  in each impl so contract violations crash the test, not the user.

#### Determinism — one context, propagated

- [x] Add `vkpt::core::DeterminismContext` in
  `src/core/contracts/Determinism.h` with
  `{ bool enabled; uint64_t base_seed; uint64_t frame_index;
  std::string scenario_id; }`. One source of truth.
- [x] `set_determinism(const DeterminismContext&)` on every subsystem
  that has a deterministic mode (currently scattered across
  `RenderSettings`, `ScriptExecutionContext`, `PhysicsStepConfig`,
  `JobSystem`, `AudioSystemConfig`).
  Bounded CI proof: `pt_observability_smoke` validates the shared
  `DeterminismContext` setter/recovery helpers on render, path-trace,
  scripting, physics, and audio config surfaces plus the `IJobSystem`
  API; `pt_job_health_smoke` validates concrete `JobSystem` propagation
  into status.
- [x] On config change, each subsystem emits
  `<comp>.determinism_changed` with the new context — so an agent can
  diff replay setups by reading logs.
- [x] Wire `--deterministic` (Track A 0.6) to construct a
  `DeterminismContext` at startup and broadcast it.

#### Snapshot-style outputs from non-render subsystems

- [x] Physics: replace `vector<PhysicsTransformWrite>
  extract_transform_writes()` with `PhysicsStepSnapshot
  step_snapshot()` returning `{ generation, wall_time_ns,
  transform_writes, contact_events, body_counts, step_us }`.
  Generationally identified, immutable, snapshot-compatible.
- [x] Audio: add `IAudioSystem::consume_snapshot(const SceneSnapshot&)`
  that subscribes the listener + per-source positions to the snapshot's
  generation, replacing per-event `AudioPostEventDesc.position`
  imperative updates for moving emitters.
- [x] Scripting: replace bare `WorldCommandBuffer&` mutation with
  `ScriptCommandSnapshot dispatch_hook(...)` returning `{ generation,
  hook, commands, diagnostics }`. Sim then replays into the
  authoritative buffer with conflict resolution.

#### Naming alignment

- [x] Rename `RTSceneData` → `PathTracerSceneSnapshot` for symmetry
  with `RenderSceneSnapshot` (Track B owns this; do it before Phase 1
  consumers cement the old name).
- [x] Rename `SceneWorld` → `EcsWorld` (or formally alias) so
  `IPhysicsWorld` ≠ `SceneWorld` confusion goes away.
- [x] Rename `InstanceTransformUpdatePlan` → `InstanceTransformPlan`
  (the "Update" in middle is redundant once the verb is on the method).

### Per-subsystem contract TODOs

Each list is the *contract-shape* changes (interface and types) — the
metric/log additions live in the observability backlog above.

#### Path tracer — `src/pathtracer/PathTracer.h`  *(Track B)*

- [x] Add `PathTracerStatus { lifecycle, scene_loaded, accel_valid,
  ready_to_render, current_sample, accumulation_gen, last_error }` and
  `IPathTracer::status() const`.
- [x] Document `IPathTracer` state machine as a contract comment
  (Uninitialized → Configured → SceneLoaded → Ready → reset → Ready).
  Make `PathTracerStandardContract` carry the table.
- [x] Convert `configure / load_scene_snapshot /
  build_or_update_acceleration` to return `Status` instead of `bool`.
  Existing transactional `InstanceTransformUpdateResult` is the model.
- [x] Pin contract: `plan_instance_transform_update` is **stable**
  across multiple calls with the same inputs; document the equivalence
  guarantee so callers can plan early and apply later.
- [x] Clarify `reset_accumulation()` keeps acceleration valid (film-only
  reset). Currently undocumented.

#### Render — `src/render/interface/RenderContracts.h`, `src/render/RenderCoordinator.h`, `src/render/FrameHandoff.h`  *(Track B)*

- [x] Add `RenderCoordinatorStatus` (lifecycle + last_error +
  most-recent-snapshot-gen + tile budget state) and `status()` API. The
  existing `RenderCoordinatorStats` struct is rich but is *cumulative
  counters*, not lifecycle.
- [x] Convert `RenderCoordinator::start()` and bool path to
  `Status` so the failure reason propagates instead of getting written
  into `m_error` only.
- [x] Replace single `m_error` slot with a bounded `error_history`
  (last N) so transient failures aren't lost on the next failure.
- [x] `IFrameGraph::execute()`: return `FrameGraphResult { Status
  overall, vector<PassResult> per_pass }` so partial failures are
  reportable.
- [x] Document thread-safety of `IRenderBackend::compiler()` and
  `shader_cache()` (or add `mutable std::mutex` and document).
- [x] `FrameHandoff`: drops should carry a typed reason
  (`FrameDropReason { RingFull, Cancelled, ResolveFailed,
  AccumulationReset }`) and surface it to the consumer side, not just
  bump a counter.

#### Scene — `src/scene/SceneWorld.h`, `src/scene/SnapshotRing.h`  *(Track B)*

- [x] Convert `SceneWorld::set_component / add_component /
  destroy_entity / reparent_entity` to return `Status`. Today they
  return `bool` and silently swallow context like "entity doesn't
  exist" vs. "authority conflict".
- [x] Surface per-write authority conflicts from `set_transform()`
  itself (return `Status` with the loser's name in `warnings`), not
  only via `authority_conflicts()` post-pass.
- [x] Add a `dirty_entities() const` query so partial recomputation can
  be driven externally (Track B 1.x optimisation lever).
- [x] `SnapshotRing::publish_sim_tick(...)`: split into
  `validate(writes) → Status` then `apply(writes) → SnapshotPtr` so
  the apply side can never half-fail.
- [x] Wire `lag_warning_emitted` (already a field on
  `SnapshotReaderStats`) to actually emit a warn event the first time
  a reader crosses the threshold.

#### Physics — `src/physics/PhysicsWorld.h`  *(Track C)*

- [x] Replace `extract_transform_writes()` with
  `PhysicsStepSnapshot step_snapshot()` (see "snapshot-style outputs"
  above). The struct is the contract.
- [x] Add `PhysicsStepStats { step_us, solver_iterations,
  contact_count, active_bodies }` returned from `step_fixed`. Today
  callers have no per-step timing.
- [x] Document state machine: must `sync_from_scene_world` precede
  every `step_fixed`, or is state retained? Make the contract explicit
  + assert in debug.
- [x] Add `PhysicsStatus` for `IPhysicsWorld::status()`.
- [x] Add lifecycle fields and a `physics` health probe to
  `IPhysicsWorld`; `pt_physics_contract_smoke` validates
  Uninitialized -> Ready -> Failed transitions and probe status.
- [x] Pin `request_id` semantics on `PhysicsTransformWrite` —
  declare it as the canonical sequencing key and require monotonicity.

#### Scripting — `src/scripting/ScriptRuntime.h`  *(Track C)*

- [x] Add `ScriptDispatchResult` (alongside the existing
  `ScriptDispatchSummary`) with `Status overall_status`,
  `budget_exceeded_count`, `script_killed_count`,
  `vector<ScriptError>`. Callers today can't tell partial-fail from
  total-fail.
- [x] Add `ScriptingStatus` and `IScriptRuntime::status()`.
- [x] Document `reload_bindings()` semantics: are in-flight scripts
  aborted, drained, or undefined? Pin a behavior and assert.
- [x] Add `ScriptCommandSnapshot dispatch_hook(...)` (see
  "snapshot-style outputs"). Migrate callers gradually.
- [x] Pin `instruction_budget`/`memory_budget` enforcement contract:
  is the script killed mid-execution, paused, or allowed to overshoot
  by N%? Document and add a `BudgetPolicy` enum.

#### Jobs — `src/jobs/JobSystem.h`  *(Track C)*

- [x] `wait(JobHandle, std::exception_ptr* out_exception = nullptr)`
  so callers can recover from a job that threw. Today
  exceptions are caught but invisible.
- [x] Add `JobSystemStatus { workers_busy, queue_depth.total,
  queue_depth.per_worker[], oldest_pending_us, deterministic }` plus
  `status()` API.
- [x] Add `chain(JobHandle prev, std::function<void()>)` for explicit
  job dependencies — today there's only "submit then wait".
- [x] Promote main-thread queue to support priorities so UI work
  doesn't sit behind low-priority main-thread work.
- [x] Add Result/Status variants for submit, wait, wait_group, and
  shutdown while keeping bool wrappers for legacy call sites.
- [x] Document the `IJobSystem` state machine and expose lifecycle,
  last-error, tick, and error counters through the interface-level
  `status()` contract. `pt_job_health_smoke` validates this via
  `IJobSystem`.

#### Audio — `src/audio/AudioSystem.h`  *(Track D)*

- [x] Replace `bool initialize()` / `bool load_scene_audio()` with
  `Status` returns; surface backend init failure reason
  programmatically, not just in the log.
- [x] Add `IAudioSystem::status()` returning `AudioStatus
  { lifecycle, backend, sample_rate, voices_active, voices_max,
  last_underrun_ns, last_error }`.
- [x] `AudioPostEventDesc` → split into one-shot vs. tracked variants.
  Tracked variants return `AudioEventHandle` so callers can `cancel`,
  `set_volume`, or `query_state`. Voice-stealing currently invalidates
  handles silently.
- [x] Add `IAudioSystem::consume_snapshot(const SceneSnapshot&)` so
  positions of moving emitters update from a snapshot generation, not
  from per-event imperative writes.
- [x] Document state machine: when must `load_scene_audio` be called
  relative to `initialize` and `post_event`?

#### Platform / Input — `src/platform/Interfaces.h`  *(Track D)*

- [x] Make `IEvents::consume()` non-destructive by default; add an
  explicit `IEvents::drain()` for the destructive consumer pattern. So
  multiple readers can co-exist (UI overlay + script input
  + replay recorder).
- [x] Add `IPlatformStatus` covering window state + input focus +
  vsync mode + last platform error so agents don't have to poll
  `metrics()` and infer.
- [x] Add an input recording / playback hook: `IInput::set_source(
  std::shared_ptr<IInputSource>)` so deterministic replay is possible
  without monkey-patching the real input pipe.
- [x] Surface event-queue high-water-mark and dropped-event counter
  on the contract (currently silent).

#### GPU backends — `src/gpu/VulkanGpuPathTracer.h`, `src/gpu/D3D12GpuPathTracer.h`  *(Track B)*

- [x] Add `IGpuBackendIntrospect` interface that GPU path tracers
  implement: `{ adapter_name, vram_used, vram_total, in_flight_dispatches,
  last_present_us, last_fence_wait_us, timeline_value }`. Backend-
  agnostic shape.
- [x] Make `film()` async or split: `request_film_readback() → Token`
  + `try_take_film(Token) → optional<FilmBuffer>`. Today `film()`
  hides a GPU stall.
- [x] Generate `PathTracePushConstants` from the SPIR-V reflection or
  a shared schema, not a hand-rolled C++ struct + static_assert. The
  current setup is a memory-corruption bug waiting on a divergent edit.
- [x] Add `Status init_device()` etc. — currently they return `bool`
  with reason in `last_error`, which is the worst pattern (split state).

### Cross-track sequencing for contract changes

Some of these are breaking. Land them in this order to minimise churn:

1. **First, in Track A**: ship `Result/Status`, `ComponentLifecycle`,
   `<Subsystem>Status` convention header, `DeterminismContext`,
   `IFlowSource`. These are pure additions; nothing breaks.
2. **Within each track**: rename, then add `Status` returns alongside
   the bool versions, then deprecate the bool versions, then delete.
   Two minor versions per migration.
3. **Snapshot-style output migrations** (physics → snapshot, audio →
   snapshot consumer, script → command snapshot): land *after* their
   respective phases (2/4/5) finish so the bus model is in place to
   absorb them.

### Contract maturity matrix

Same shape as the observability matrix, different axes. A row goes
green when each contract pillar is met. All-green means the subsystem's
boundary is production-ready and predictable for agents.

| Subsystem    | Status() | Result/Status | State machine | Determinism ctx | Snapshot-style | Naming clean |
|--------------|----------|---------------|---------------|------------------|----------------|--------------|
| render       | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| pathtracer   | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| gpu          | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| cpu          | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| scene        | [x]      | [x]           | [x]           | [x]              | [x]            | [x]          |
| scripting    | [x]      | [x]           | [x]           | [x]              | [x]            | [x]          |
| physics      | [x]      | [x]           | [x]           | [x]              | [x]            | [x]          |
| jobs         | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| audio        | [x]      | [x]           | [x]           | [x]              | [x]            | [x]          |
| assets       | [x]      | [x]           | [x]           | n/a              | n/a            | [x]          |
| platform/ui  | [x]      | [x]           | [x]           | [x]              | n/a            | [x]          |
| diagnostics  | [x]      | [x]           | [x]           | n/a              | n/a            | [x]          |

(`n/a` = pillar doesn't apply to this subsystem; treat as already met.)

Bounded source proof for the TODO 7-22 contract cells above:
`pt_observability_smoke` runs `ContractMaturityTodo7To22SourceProofPass`,
which verifies the render `FrameHandoff` naming contract, GPU typed
Status/init-device and schema/introspection names, split audio event descriptor
names, platform `Result<T>` signatures and status/input-source names, and
platform/UI `DeterminismContext` propagation, and `DiagnosticsStatus` with the
formal `CrashRecorderStatus` alias. `pt_platform_contract_smoke` also has
compile-time and runtime checks for the platform `Result<T>` and determinism
contracts.

---

## Console & runtime introspection

Operator-facing surface so a human or agent can poke a running app without a
debugger.

### CLI flags
- `--log-level=<level>` — global threshold (`trace|debug|info|warn|error`).
- `--log-format=console|kv|json` — output encoding.
- `--log-out=<path|stderr>` — destination.
- `--verbose=<comp>:<channel>[,...]` — per-component overrides
  (`--verbose=tracer:tile,audio:callback`).
- `--trace=<comp>[,...]` — enable `VKP_TRACE_SCOPE` recording.
- `--trace-out=trace.json` — Chrome-tracing export on shutdown.
- `--metrics-out=<path>` — periodic JSON dump of all metrics.
- `--deterministic` — see Phase 6.4.

### Runtime commands (dock panel + stdin REPL)
- `metrics dump` — print all metrics in current `--log-format`.
- `metrics reset <prefix>` — zero counters/histograms (e.g., for re-baselining).
- `events tail <comp>` — live stream events from one component.
- `events dump-rings` — flush every per-thread crash ring to stderr.
- `snapshot dump <gen|current>` — write a snapshot to disk.
- `snapshot diff <gen_a> <gen_b>` — show what changed between two snapshots.
- `script list` — table of loaded scripts with their stats.
- `script reload <id>` — force reload.
- `script kill <id>` — terminate runaway script.
- `health` — print every component's `IHealthProbe` status.
- `tracer pause` / `tracer resume` — useful for clean convergence captures.

### Signal handlers
- `SIGINT` / Ctrl+C — graceful shutdown; dump final metrics + crash rings.
- `SIGSEGV` / structured exceptions — crash-ring dump to stderr before unwind.
- TSan/ASan abort hooks — same crash-ring dump.

### Agent-friendly defaults
When stdout is not a TTY (i.e., the app is launched by an agent harness),
default to `--log-format=json --log-level=info`. Agents reading the log get
one parseable record per event with stable field names — no need to teach
them the human format.

---

## Acceptance criteria (exit gates)

The migration is "done" when, on a representative scene with continuous
physics + camera motion + 50 active scripts + 20 audio sources:

**Performance**
- [ ] UI maintains 60 Hz repaint, 99th percentile input latency < 16 ms
  (verified via `vkp.ui.input_to_pixel_us`).
  - [x] Bounded CI proof: `pt_observability_smoke` records a synthetic
    120-frame headless `vkp.ui.*` run and checks `vkp.ui.repaint_hz == 60`,
    `vkp.ui.input_to_pixel_us.p99 < 16000`, and max input latency < 16 ms.
    Re-verified 2026-05-08 after the CPU contract audit update:
    `pt_observability_smoke` passed end-to-end.
  - Verified 2026-05-09: representative-scene run measured
    `vkp.ui.repaint_hz=11.5` (final gauge) and per-phase
    `vkp.ui.input_to_pixel_us.p99` of 768us (idle) / 49152us (camera_pan) /
    196608us (transform_drag, lua_motion) / 393216us (game_mode_walk,
    game_mode_reenter); gate not met because the windowed Qt-debug build of
    `assets/scenes/representative_acceptance_scene.json` cannot sustain 60 Hz
    repaint or keep input-to-pixel p99 below 16 ms once camera_pan +
    transform_drag + 50-script Lua dispatch + 20 audio emitters all run on
    the representative scene. Artifact:
    `artifacts/perf-acceptance-gate/qt_stress_gate_report.json`.
- [ ] Sim ticks at its target Hz with < 1% jitter (verified via
  `vkp.sim.deadline_misses_total`).
  - [x] Bounded CI proof: `pt_observability_smoke` records a synthetic
    120-tick headless `vkp.sim.*` run and checks `tick_hz == 60`,
    `deadline_misses_total == 0`, and exact min/max tick jitter within 1%.
    Re-verified 2026-05-08 after the CPU contract audit update:
    `pt_observability_smoke` passed end-to-end.
  - Verified 2026-05-09: representative-scene run measured
    `vkp.sim.tick_hz=37.7` (target 60), `vkp.sim.deadline_misses_total=443`
    (target 0), and harness-computed jitter 4773% (target < 1%) over 1139
    sim ticks across all six stress phases (5 s each, 30 s total of phase
    work); gate not met for the same reason as the UI gate. Artifact:
    `artifacts/perf-acceptance-gate/qt_stress_gate_report.json` (`sim`
    block).
- [x] Audio callback never underruns (verified via `vkp.audio.underruns_total`
  staying at 0 across a 10-minute soak).
  - [x] Bounded CI proof: `pt_scripting_smoke` runs a 750 ms no-op audio
    callback soak, checks diagnostics add no callback or stream-ring
    underruns, and checks `vkp.audio.underruns_total == 0` plus callback
    latency/jitter histograms.
  - [x] Full soak proof: on 2026-05-08,
    `PT_AUDIO_SOAK_MS=600000 PT_AUDIO_SOAK_MIN_BUFFERS=1000
    pt_scripting_smoke` passed with no callback or stream-ring underruns.
- [ ] Tracer noise scales with GPU count: 2 GPUs ≈ 1.8x samples/sec
  (verified via per-GPU `vkp.tracer.samples_per_sec`).
  - [x] Bounded CI proof: `pt_multi_gpu_accumulation_smoke` verifies the
    hardware-independent 2-GPU contract reaches >=1.8x relative samples/sec
    and lower fixed-time noise; real per-GPU metric validation remains a
    hardware gate.
- [x] No `mutex`, no `malloc`, no blocking I/O on any per-frame hot path.
  Verified 2026-05-09: app-wide hot-path audit complete. [1 finding fixed,
  3 accepted with rationale]: (1) `TileScheduler::rebuild_order` converted
  from `reserve`+`push_back` loop to indexed writes into a pre-sized
  `m_order` (single allocation per `begin_sample`, no per-tile push); (2)
  `RenderCoordinator` worker `sleep_for(1ms)` accepted as idle throttle
  (only runs when `sample >= settings.spp`, i.e. converged and awaiting
  scene/settings change, not the tile loop); (3) `SleepRenderWorkerUntil`
  accepted as intentional publish-rate pacing (caps producer to
  `publishHz`, spin-yields when target is imminent, not contention
  blocking); (4) `SnapshotRing` mutex accepted — runs once per sim tick
  (sim->render handoff), guards only auxiliary stats/slots/readers
  bookkeeping; consumers read snapshots through the lock-free
  `m_current` `atomic<shared_ptr>` (the documented MSVC fast path per the
  "Risks & open questions" entry below).
  - [x] Bounded CI proof: `pt_observability_smoke` statically scans
    `AudioSystem::record_callback_metrics`, `AudioSystem::mix_device_output`,
    `TileScheduler::next_tile`, `Logger::push`, metrics counter/histogram
    record paths, and SPSC/MPSC ring producer pushes for
    mutex/allocation/blocking-I/O tokens.
- [ ] TSan-clean across a 10-minute soak.
  Verified 2026-05-08 on this Windows host: the `desktop-clang-tsan` build is
  blocked by Clang's unsupported `-fsanitize=thread` for
  `x86_64-pc-windows-msvc`, so this is not marked clean.
- [x] Qt/window acceptance run on this host.
  Verified 2026-05-08: `cmake --build --preset desktop-clang-qt-debug
  --target ptapp pt_observability_smoke pt_platform_contract_smoke` passed,
  followed by `pt_observability_smoke`, `pt_platform_contract_smoke`,
  `ptapp --ui-model-smoke --platform qt`, `ptapp --ui-release-gate --json
  --platform qt`, and `ptapp --window --qt-stress-gate
  --qt-stress-phase-seconds 5 --platform qt`. The Qt stress artifact reports
  input-to-pixel p95 = 768 us; the full representative-scene 60 Hz gate above
  remains open.
- [x] Determinism flag: 3 runs with identical input produce bit-identical
  snapshot streams (verified via `determinism.snapshot.outputs_hash`).
  `pt_snapshot_bus_smoke` now emits and compares a three-entry
  `determinism.snapshot.outputs_hash` stream across three deterministic runs.

**Observability**
- [x] Every component emits lifecycle, heartbeat, and anomaly events.
  Bounded CI proof: `pt_observability_smoke` validates lifecycle, heartbeat,
  and anomaly triplets for `obs`, `metrics`, `health`, `ui`, `sim`, `audio`,
  `tracer`, `scripting`, `snapshot`, and `determinism`.
- [x] All metrics in `vkp.<comp>.<name>` are scrapeable from the dock and
  exportable as JSON.
- [x] Crash-ring dump on abort contains last events from every thread.
- [x] `--log-format=json` output validates against a single documented schema.
- [x] An agent (separate process) can determine, from logs alone, which
  component caused a deadline miss within 5s of the event.

---

## Risks & open questions

- **`std::atomic<std::shared_ptr<T>>` portability** — MSVC has it, Clang+libc++
  on Linux historically lagged. May need a SeqLock fallback for cross-platform.
- **COW arrays vs cache locality** — pointer-shared arrays across snapshots
  are cheap to publish but may fragment. Measure before committing.
- **Reprojection quality** — camera-only reprojection in a path tracer with
  sharp specular reflections can ghost. Acceptable for interactivity; toggle
  off when paused for clean convergence.
- **Script ordering across reload** — if a Group-A script reloads mid-tick,
  hook ordering must remain stable. Reload swaps happen between ticks.
- **Audio-snapshot pose lag** — 16 ms stale pose is fine for ambient sources;
  may be audible for fast-moving emitters. Mitigation: per-emitter velocity in
  snapshot lets audio extrapolate position over the buffer.
- **Logger backpressure** — if the writer thread can't keep up, per-thread
  rings fill and events are dropped. Acceptable for `debug`; never for
  `warn`/`error`. Solution: separate ring per level, with `error` always
  guaranteed-delivery (single small ring, blocks the writer briefly if full —
  the only "blocking" carve-out in the system, and only on error paths).
- **Metric cardinality** — per-script labels (`lua_errors_total{script_id=…}`)
  can explode if scripts are spawned dynamically. Cap label cardinality at
  registry level; overflow goes to a `_other` bucket.
