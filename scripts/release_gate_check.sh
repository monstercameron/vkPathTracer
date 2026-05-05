#!/usr/bin/env bash
set -euo pipefail

run_step() {
  local name="$1"
  shift
  echo "== ${name} =="
  echo "$*"
  "$@"
}

run_step "Configure (debug)" cmake --preset desktop-clang-debug
run_step "Build (debug)" cmake --build --preset desktop-clang-debug

PTAPP="./build/presets/desktop-clang-debug/bin/ptapp"
PTBENCH="./build/presets/desktop-clang-debug/bin/ptbench"
RELEASE_DIR="artifacts/release_check"
DESCRIPTOR="${RELEASE_DIR}/cornell_descriptor.json"
mkdir -p "${RELEASE_DIR}"
cat >"${DESCRIPTOR}" <<'JSON'
{
  "scene_path": "assets/scenes/cornell_native.json",
  "backend": "cpu",
  "renderer_path": "cpu-scalar",
  "resolution": { "width": 64, "height": 64 },
  "samples_per_pixel": 1,
  "duration": 0,
  "warmup_frames": 0,
  "seed": 195936478,
  "output_directory": "artifacts/release_check/descriptor_echo",
  "reference_image": "",
  "tolerance_policy": "abs=0.001",
  "max_depth": 6,
  "worker_count": 0,
  "tile_height": 16,
  "deterministic": false
}
JSON

run_step "Doctor" "${PTAPP}" --doctor
run_step "Crash test artifacts" "${PTAPP}" --headless --crash-test

run_step "Descriptor parse/echo" "${PTBENCH}" run --desc "${DESCRIPTOR}" --echo-desc
run_step "Scene validation (pack)" "${PTBENCH}" release-check --scene-pack assets/scenes --output "${RELEASE_DIR}"
run_step "Artifact contract" "${PTBENCH}" validate-artifacts --dir artifacts/release_check/cpu_scalar_cornell
run_step "Thread scaling sweep" "${PTBENCH}" thread-sweep --scene assets/scenes/cornell_native.json --workers 1,2 --resolution 64x64 --spp 1 --output artifacts/release_check/thread_scaling
run_step "Backend experiment skips" "${PTBENCH}" backend-experiments --output artifacts/release_check
run_step "Shader matrix" "${PTBENCH}" shader-matrix --output artifacts/release_check
run_step "GPU mem pressure (sim)" "${PTBENCH}" gpu-mem-pressure --max-mb 256 --step-mb 64 --output artifacts/release_check

if [[ -f "./tools/ui_qt_smoke.sh" ]]; then
  run_step "UI release gate smoke" bash ./tools/ui_qt_smoke.sh --no-build --disabled-build-dir ./build/presets/desktop-clang-debug
fi

echo
echo "release gate: ok"

