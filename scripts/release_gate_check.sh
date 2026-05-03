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

run_step "Doctor" "${PTAPP}" --doctor
run_step "Crash test artifacts" "${PTAPP}" --headless --crash-test

run_step "Scene validation (pack)" "${PTBENCH}" release-check --scene-pack assets/scenes --output artifacts/release_check
run_step "Shader matrix" "${PTBENCH}" shader-matrix --output artifacts/release_check
run_step "GPU mem pressure (sim)" "${PTBENCH}" gpu-mem-pressure --max-mb 256 --step-mb 64 --output artifacts/release_check

echo
echo "release gate: ok"

