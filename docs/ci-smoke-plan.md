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

- **cpu-tiny-render**
  - Run: `ptbench run --scene assets/scenes/cornell_native.json --backend cpu --renderer-path cpu-scalar --resolution 64x64 --spp 1 --output artifacts/ci/cpu_scalar`
  - Pass: `results.json` produced.

- **artifact-schema**
  - Run: verify required artifacts exist in the run output directory (`results.json`, `results.csv`, `metadata.json`, `scene_snapshot.json`, `shader_manifest.json`, `asset_manifest.json`, `beauty.png`, `beauty.exr`, `logs.jsonl`)

- **sanitizers (optional)**
  - Run: `desktop-clang-asan` and `desktop-clang-tsan` builds where supported.
  - Pass: configure/build completes; runtime smoke can be optional.

### Optional GPU jobs (manual / self-hosted)

- **vulkan-smoke**
  - Run: `ptbench run --scene assets/scenes/cornell_native.json --backend vulkan --renderer-path gpu-compute --resolution 64x64 --spp 1 --output artifacts/ci/vulkan_compute`
  - Pass: completes and produces artifacts.

