## CI smoke plan (Gate 10 / F19)

Goal: catch broken headers/interfaces quickly without requiring a GPU.

### Required jobs

- **configure-only**
  - Run: `cmake --preset desktop-clang-debug`
  - Pass: configure completes.

- **build-debug**
  - Run: `cmake --build --preset desktop-clang-debug`
  - Pass: `ptapp` + `ptbench` build.

- **build-benchmark**
  - Run: `cmake --preset desktop-clang-benchmark` then `cmake --build --preset desktop-clang-benchmark`
  - Pass: `ptbench` builds.

- **doctor**
  - Run: `ptapp --doctor`
  - Pass: prints `doctor: ok`.

- **scene-validation**
  - Run: `ptbench validate-scene --scene assets/scenes/cornell_native.json`
  - Pass: `valid: yes`.

- **descriptor-echo**
  - Run: `ptbench run --desc artifacts/release_check/cornell_descriptor.json --echo-desc`
  - Pass: descriptor parses and echoes all resolved run fields.

- **cpu-tiny-render**
  - Run: `ptbench run --scene assets/scenes/cornell_native.json --backend cpu --renderer-path cpu-scalar --resolution 64x64 --spp 1 --output artifacts/ci/cpu_scalar`
  - Pass: `results.json` produced.

- **artifact-schema**
  - Run: `ptbench validate-artifacts --dir artifacts/ci/cpu_scalar`
  - Pass: required artifacts exist and are non-empty (`results.json`, `results.csv`, `metadata.json`, `scene_snapshot.json`, `shader_manifest.json`, `asset_manifest.json`, `beauty.png`, `beauty.exr`, `logs.jsonl`, `profiler_trace.json`).

- **thread-scaling**
  - Run: `ptbench thread-sweep --scene assets/scenes/cornell_native.json --workers 1,2 --resolution 64x64 --spp 1 --output artifacts/ci/thread_scaling`
  - Pass: rows are either `ok` or explicitly `skipped`; no failed rows.

- **shader-matrix**
  - Run: `ptbench shader-matrix --output artifacts/ci/shader_matrix`
  - Pass: registered shader compilers compile required variants; unavailable SDK/backend rows are `skipped`, not failed.

- **backend-experiment-skips**
  - Run: `ptbench backend-experiments --output artifacts/ci/backend_experiments`
  - Pass: Vulkan/D3D12/Metal/WebGPU experiment rows report `available` or actionable `skipped` reasons.

- **release-checklist**
  - Run: `ptbench release-check --scene-pack assets/scenes --output artifacts/ci/release_check`
  - Pass: release checklist JSON contains scene pack, CPU scalar, artifact contract, threaded render, backend skip, and UI external-coverage rows.

- **ui-release-smoke**
  - Run: `tools/ui_qt_smoke.ps1 -NoBuild -DisabledBuildDir build/presets/desktop-clang-debug` or `bash tools/ui_qt_smoke.sh --no-build --disabled-build-dir build/presets/desktop-clang-debug`
  - Pass: Qt-disabled UI model smoke passes; Qt bounded-window checks pass or skip cleanly when Qt is unavailable.

- **acceptance-proof-smokes**
  - Run: `cmake --build --preset desktop-clang-debug --target pt_observability_smoke pt_snapshot_bus_smoke pt_scripting_smoke pt_multi_gpu_accumulation_smoke pt_job_health_smoke pt_platform_contract_smoke`, then run the six produced executables.
  - Pass: observability validates lifecycle/heartbeat/anomaly event triplets, metrics-dock scrape/rate/sparkline source contract, bounded per-thread cadence with zero logger ring drops, bounded headless UI/sim metric limits, REPL script-list/status dispatch, and the selected hot-path audit; snapshot bus emits matching three-run `determinism.snapshot.outputs_hash` streams; scripting/audio reports no no-op callback or stream-ring underruns during the bounded short soak; multi-GPU accumulation verifies the hardware-independent scheduler/accumulation scaling contract; jobs health fails only on stale over-capacity queues with idle workers; platform contract verifies non-destructive event reads, explicit drains, platform status, queue watermarks, and deterministic input sources.

- **sanitizers (optional)**
  - Run: `desktop-clang-asan` and `desktop-clang-tsan` builds where supported.
  - Pass: configure/build completes; runtime smoke can be optional. On Windows `x86_64-pc-windows-msvc`, `desktop-clang-tsan` is expected to remain blocked until Clang supports `-fsanitize=thread` for that target or CI provides a Linux/self-hosted TSan runner.

### Optional GPU jobs (manual / self-hosted)

- **vulkan-smoke**
  - Run: `ptbench run --scene assets/scenes/cornell_native.json --backend vulkan --renderer-path gpu-compute --resolution 64x64 --spp 1 --output artifacts/ci/vulkan_compute`
  - Pass: completes and produces artifacts.

- **simd-and-tile-sweeps**
  - Run: `ptbench simd-sweep --rays 100000 --triangles 256 --output artifacts/ci/simd` and `ptbench tile-sweep --scene assets/scenes/cornell_native.json --workers 2 --resolution 64x64 --spp 1 --output artifacts/ci/tile`
  - Pass: unsupported SIMD modes are skipped with reasons; tile rows include throughput and normalized scores.

