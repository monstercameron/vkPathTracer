#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISABLED_PRESET="desktop-clang-debug"
QT_PRESET="desktop-clang-qt-debug"
DISABLED_BUILD_DIR=""
QT_BUILD_DIR=""
WIDTH=64
HEIGHT=64
SPP=2
WINDOW_SECONDS=8
WINDOW_FRAMES=5
UI_PRESENT_HZ=30
NO_BUILD=0
SKIP_QT=0
CMAKE_BIN="${CMAKE_BIN:-cmake}"

usage() {
  cat <<'USAGE'
Usage: tools/ui_qt_smoke.sh [options]

Options:
  --disabled-preset NAME   CMake preset for the Qt-disabled smoke.
  --qt-preset NAME         CMake preset for the Qt-enabled smoke.
  --disabled-build-dir DIR Existing/override build dir for the Qt-disabled smoke.
  --qt-build-dir DIR       Existing/override build dir for the Qt-enabled smoke.
  --width N                Render/window width.
  --height N               Render/window height.
  --spp N                  Render samples per pixel.
  --window-seconds N       Maximum bounded Qt window lifetime.
  --window-frames N        Frames to run before bounded Qt window exit.
  --ui-present-hz N        Optional Qt present rate to pass when supported (0 disables).
  --no-build               Skip configure/build and use existing binaries.
  --skip-qt                Skip Qt-enabled checks.
  -h, --help               Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --disabled-preset) DISABLED_PRESET="$2"; shift 2 ;;
    --qt-preset) QT_PRESET="$2"; shift 2 ;;
    --disabled-build-dir) DISABLED_BUILD_DIR="$2"; shift 2 ;;
    --qt-build-dir) QT_BUILD_DIR="$2"; shift 2 ;;
    --width) WIDTH="$2"; shift 2 ;;
    --height) HEIGHT="$2"; shift 2 ;;
    --spp) SPP="$2"; shift 2 ;;
    --window-seconds) WINDOW_SECONDS="$2"; shift 2 ;;
    --window-frames) WINDOW_FRAMES="$2"; shift 2 ;;
    --ui-present-hz) UI_PRESENT_HZ="$2"; shift 2 ;;
    --no-build) NO_BUILD=1; shift ;;
    --skip-qt) SKIP_QT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

PASS=0
FAIL=0
SKIP=0
CMD_OUTPUT=""
CMD_STATUS=0
SMOKE_DIR="${ROOT}/artifacts/ui_qt_smoke"
NOQT_EXR="${SMOKE_DIR}/noqt_smoke.exr"
QT_RENDER_PNG="${SMOKE_DIR}/qt_render_headless.png"
QT_RENDER_EXR="${SMOKE_DIR}/qt_render_headless.exr"
QT_STDOUT="${SMOKE_DIR}/qt_window_stdout.log"
QT_STDERR="${SMOKE_DIR}/qt_window_stderr.log"

safe_name() {
  printf '%s' "$1" | sed 's/[^A-Za-z0-9_.-]/_/g'
}

preset_build_dir() {
  local preset="$1"
  python - "$ROOT" "$preset" <<'PY' 2>/dev/null
import json
import sys
root, preset = sys.argv[1], sys.argv[2]
with open(f"{root}/CMakePresets.json", "r", encoding="utf-8") as f:
    data = json.load(f)
for item in data.get("configurePresets", []):
    if item.get("name") == preset and item.get("binaryDir"):
        print(item["binaryDir"].replace("${sourceDir}", root))
        break
PY
}

default_build_dir() {
  local preset="$1"
  local kind="$2"
  local dir=""
  if [[ "$NO_BUILD" -eq 1 ]]; then
    dir="$(preset_build_dir "$preset" || true)"
    if [[ -n "$dir" ]]; then
      printf '%s\n' "$dir"
      return
    fi
  fi
  printf '%s/build/ui_smoke_%s_%s\n' "$ROOT" "$kind" "$(safe_name "$preset")"
}

resolve_path() {
  local path="$1"
  case "$path" in
    "" ) printf '\n' ;;
    /* | [A-Za-z]:/* | [A-Za-z]:\\* ) printf '%s\n' "$path" ;;
    * ) printf '%s/%s\n' "$ROOT" "$path" ;;
  esac
}

is_windowsish() {
  case "$(uname -s 2>/dev/null || printf unknown)" in
    MINGW*|MSYS*|CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

ptapp_path() {
  local build_dir="$1"
  if is_windowsish; then
    printf '%s/bin/ptapp.exe\n' "$build_dir"
  else
    printf '%s/bin/ptapp\n' "$build_dir"
  fi
}

tail_text() {
  printf '%s' "$1" | tail -c 4000
}

pass_step() {
  printf '  [%s] [ok]\n' "$1"
  PASS=$((PASS + 1))
}

fail_step() {
  printf '  [%s] [FAIL] %s\n' "$1" "$2"
  FAIL=$((FAIL + 1))
}

skip_step() {
  printf '  [%s] [skip] %s\n' "$1" "$2"
  SKIP=$((SKIP + 1))
}

run_cmd() {
  CMD_OUTPUT="$("$@" 2>&1)"
  CMD_STATUS=$?
}

step_cmd() {
  local name="$1"
  local pattern="$2"
  shift 2
  run_cmd "$@"
  if [[ "$CMD_STATUS" -ne 0 ]]; then
    fail_step "$name" "exited ${CMD_STATUS}: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  if [[ -n "$pattern" ]] && ! grep -Eiq "$pattern" <<<"$CMD_OUTPUT"; then
    fail_step "$name" "output did not match /${pattern}/: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  pass_step "$name"
}

qt_unavailable_output() {
  grep -Eiq 'PT_ENABLE_QT=ON requires Qt6|Could not find.*Qt6|Qt6.*not found|No package.*Qt6' <<<"$1"
}

ptapp_supports_option() {
  local exe="$1"
  local option="$2"
  "$exe" --help 2>&1 | grep -Fq -- "$option"
}

qt_ui_present_hz_coverage() {
  local exe="$1"
  if ! [[ "$UI_PRESENT_HZ" =~ ^[0-9]+$ ]]; then
    fail_step "qt-ui-present-hz-option" "invalid --ui-present-hz value: ${UI_PRESENT_HZ}"
    return 1
  fi
  if [[ "$UI_PRESENT_HZ" -eq 0 ]]; then
    skip_step "qt-ui-present-hz-option" "disabled"
    return 1
  fi
  if ptapp_supports_option "$exe" "--ui-present-hz"; then
    pass_step "qt-ui-present-hz-option"
    return 0
  fi
  skip_step "qt-ui-present-hz-option" "binary help does not advertise --ui-present-hz"
  return 1
}

qt_window_smoke() {
  local exe="$1"
  local pass_ui_present_hz="${2:-0}"
  mkdir -p "$SMOKE_DIR"
  : >"$QT_STDOUT"
  : >"$QT_STDERR"

  if [[ -n "${VKPT_QT_QPA_PLATFORM:-}" ]]; then
    export QT_QPA_PLATFORM="$VKPT_QT_QPA_PLATFORM"
  elif [[ -z "${QT_QPA_PLATFORM:-}" && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    case "$(uname -s 2>/dev/null || printf unknown)" in
      Darwin|MINGW*|MSYS*|CYGWIN*) ;;
      *) export QT_QPA_PLATFORM=offscreen ;;
    esac
  fi

  local window_args=(--window --platform qt --backend cpu)
  if [[ "$pass_ui_present_hz" -eq 1 && "$UI_PRESENT_HZ" -gt 0 ]]; then
    window_args+=(--ui-present-hz "$UI_PRESENT_HZ")
  fi
  window_args+=(
    --frames "$WINDOW_FRAMES" --exit
    --window-width "$WIDTH" --window-height "$HEIGHT"
    --scene "${ROOT}/assets/scenes/cornell_native.json"
  )

  "$exe" "${window_args[@]}" >"$QT_STDOUT" 2>"$QT_STDERR" &
  local pid=$!
  local deadline=$((SECONDS + WINDOW_SECONDS))
  local ready=0
  local reason=""

  while (( SECONDS < deadline )); do
    if grep -q 'qt window open' "$QT_STDOUT" "$QT_STDERR" 2>/dev/null; then
      ready=1
      reason="stdout"
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.25
  done

  if [[ "$ready" -eq 0 ]] && ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null
    local early_code=$?
    if [[ "$early_code" -eq 0 ]]; then
      ready=1
      reason="bounded-exit-zero"
    elif [[ "$ready" -eq 1 ]]; then
      :
    else
      fail_step "qt-window-bounded" "process exited ${early_code}: $(tail_text "$(cat "$QT_STDOUT" "$QT_STDERR" 2>/dev/null)")"
      return 1
    fi
  fi

  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi

  if [[ "$ready" -ne 1 ]]; then
    fail_step "qt-window-bounded" "Qt window did not become ready within ${WINDOW_SECONDS}s: $(tail_text "$(cat "$QT_STDOUT" "$QT_STDERR" 2>/dev/null)")"
    return 1
  fi

  printf '    ready: %s\n' "$reason"
  if [[ "$pass_ui_present_hz" -eq 1 && "$UI_PRESENT_HZ" -gt 0 ]]; then
    printf '    ui-present-hz: %s\n' "$UI_PRESENT_HZ"
  fi
  pass_step "qt-window-bounded"
}

qt_render_headless() {
  local exe="$1"
  rm -f "$QT_RENDER_PNG" "$QT_RENDER_EXR"
  run_cmd "$exe" --render --platform qt --backend cpu \
    --width "$WIDTH" --height "$HEIGHT" --spp "$SPP" \
    --output "$QT_RENDER_PNG" --exr-output "$QT_RENDER_EXR"
  if [[ "$CMD_STATUS" -ne 0 ]]; then
    fail_step "qt-render-headless" "exited ${CMD_STATUS}: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  if ! grep -Eiq 'ui shell:[[:space:]]*headless[[:space:]]*\(Qt requested; GUI not initialized for render\)' <<<"$CMD_OUTPUT"; then
    fail_step "qt-render-headless" "Qt-selected render did not report the headless shell: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  if grep -Eiq 'qt window open' <<<"$CMD_OUTPUT"; then
    fail_step "qt-render-headless" "Qt-selected render unexpectedly opened a Qt window: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  if ! grep -Eiq 'render complete' <<<"$CMD_OUTPUT"; then
    fail_step "qt-render-headless" "output did not match /render complete/: $(tail_text "$CMD_OUTPUT")"
    return 1
  fi
  pass_step "qt-render-headless"
}

qt_render_headless_output() {
  if [[ ! -f "$QT_RENDER_EXR" ]]; then
    skip_step "qt-render-headless-output" "EXR was not produced by this binary"
  elif [[ "$(wc -c <"$QT_RENDER_EXR")" -lt 100 ]]; then
    fail_step "qt-render-headless-output" "render output is suspiciously small"
  else
    pass_step "qt-render-headless-output"
  fi
}

if [[ -z "$DISABLED_BUILD_DIR" ]]; then
  DISABLED_BUILD_DIR="$(default_build_dir "$DISABLED_PRESET" noqt)"
else
  DISABLED_BUILD_DIR="$(resolve_path "$DISABLED_BUILD_DIR")"
fi
if [[ -z "$QT_BUILD_DIR" ]]; then
  QT_BUILD_DIR="$(default_build_dir "$QT_PRESET" qt)"
else
  QT_BUILD_DIR="$(resolve_path "$QT_BUILD_DIR")"
fi

NOQT_PTAPP="$(ptapp_path "$DISABLED_BUILD_DIR")"
QT_PTAPP="$(ptapp_path "$QT_BUILD_DIR")"

mkdir -p "$SMOKE_DIR"
cd "$ROOT"

echo
echo "=== vkPathTracer UI/Qt smoke ==="
echo "  no-Qt preset : ${DISABLED_PRESET}"
echo "  no-Qt build  : ${DISABLED_BUILD_DIR}"
echo "  Qt preset    : ${QT_PRESET}"
echo "  Qt build     : ${QT_BUILD_DIR}"
echo

if [[ "$NO_BUILD" -eq 1 ]]; then
  skip_step "noqt-configure" "--no-build"
  skip_step "noqt-build" "--no-build"
  NOQT_BUILT=1
else
  NOQT_BUILT=0
  if step_cmd "noqt-configure" "" "$CMAKE_BIN" --preset "$DISABLED_PRESET" -B "$DISABLED_BUILD_DIR" -DPT_ENABLE_QT=OFF -DPT_ENABLE_QT_EDITOR=OFF; then
    if step_cmd "noqt-build" "" "$CMAKE_BIN" --build "$DISABLED_BUILD_DIR" --target ptapp; then
      NOQT_BUILT=1
    fi
  else
    skip_step "noqt-build" "configure failed"
  fi
fi

NOQT_CAN_RUN=0
if [[ "$NOQT_BUILT" -eq 1 ]]; then
  if [[ -x "$NOQT_PTAPP" || -f "$NOQT_PTAPP" ]]; then
    pass_step "noqt-binary"
    NOQT_CAN_RUN=1
  else
    fail_step "noqt-binary" "ptapp not found at ${NOQT_PTAPP}"
  fi
else
  skip_step "noqt-binary" "build did not complete"
fi

if [[ "$NOQT_CAN_RUN" -eq 1 ]]; then
  step_cmd "noqt-version-json" '"version"' "$NOQT_PTAPP" --version --json
  step_cmd "noqt-doctor" 'doctor:[[:space:]]*ok' "$NOQT_PTAPP" --doctor
  step_cmd "noqt-ui-model-smoke" 'ui model smoke:[[:space:]]*ok' "$NOQT_PTAPP" --ui-model-smoke
  step_cmd "noqt-ui-release-gate" '"pending_count"[[:space:]]*:[[:space:]]*0' "$NOQT_PTAPP" --ui-release-gate --json
  step_cmd "noqt-headless" 'headless platform initialized' "$NOQT_PTAPP" --headless
  rm -f "$NOQT_EXR"
  if step_cmd "noqt-render" 'render complete' "$NOQT_PTAPP" --render --backend cpu --width "$WIDTH" --height "$HEIGHT" --spp "$SPP" --exr-output "$NOQT_EXR"; then
    if [[ ! -f "$NOQT_EXR" ]]; then
      skip_step "noqt-render-output" "EXR was not produced by this binary"
    elif [[ "$(wc -c <"$NOQT_EXR")" -lt 100 ]]; then
      fail_step "noqt-render-output" "render output is suspiciously small"
    else
      pass_step "noqt-render-output"
    fi
  else
    skip_step "noqt-render-output" "render command failed"
  fi
else
  skip_step "noqt-version-json" "no runnable no-Qt binary"
  skip_step "noqt-doctor" "no runnable no-Qt binary"
  skip_step "noqt-ui-model-smoke" "no runnable no-Qt binary"
  skip_step "noqt-ui-release-gate" "no runnable no-Qt binary"
  skip_step "noqt-headless" "no runnable no-Qt binary"
  skip_step "noqt-render" "no runnable no-Qt binary"
  skip_step "noqt-render-output" "no runnable no-Qt binary"
fi

if [[ "$SKIP_QT" -eq 1 ]]; then
  skip_step "qt-configure" "--skip-qt"
  skip_step "qt-build" "--skip-qt"
  skip_step "qt-ui-present-hz-option" "--skip-qt"
  skip_step "qt-window-bounded" "--skip-qt"
  skip_step "qt-render-headless" "--skip-qt"
  skip_step "qt-render-headless-output" "--skip-qt"
elif [[ "$NO_BUILD" -eq 1 ]]; then
  skip_step "qt-configure" "--no-build"
  skip_step "qt-build" "--no-build"
  if [[ -x "$QT_PTAPP" || -f "$QT_PTAPP" ]]; then
    QT_PASS_UI_PRESENT_HZ=0
    if qt_ui_present_hz_coverage "$QT_PTAPP"; then
      QT_PASS_UI_PRESENT_HZ=1
    fi
    qt_window_smoke "$QT_PTAPP" "$QT_PASS_UI_PRESENT_HZ"
    if qt_render_headless "$QT_PTAPP"; then
      qt_render_headless_output
    else
      skip_step "qt-render-headless-output" "render command failed"
    fi
  else
    skip_step "qt-ui-present-hz-option" "Qt binary not found at ${QT_PTAPP}"
    skip_step "qt-window-bounded" "Qt binary not found at ${QT_PTAPP}"
    skip_step "qt-render-headless" "Qt binary not found at ${QT_PTAPP}"
    skip_step "qt-render-headless-output" "Qt binary not found at ${QT_PTAPP}"
  fi
else
  run_cmd "$CMAKE_BIN" --preset "$QT_PRESET" -B "$QT_BUILD_DIR" -DPT_ENABLE_QT=ON -DPT_ENABLE_QT_EDITOR=ON
  QT_CONFIGURED=0
  if [[ "$CMD_STATUS" -eq 0 ]]; then
    QT_CONFIGURED=1
    pass_step "qt-configure"
  elif qt_unavailable_output "$CMD_OUTPUT"; then
    skip_step "qt-configure" "Qt6 not available for preset ${QT_PRESET}"
  else
    fail_step "qt-configure" "cmake exited ${CMD_STATUS}: $(tail_text "$CMD_OUTPUT")"
  fi

  if [[ "$QT_CONFIGURED" -eq 1 ]]; then
    if step_cmd "qt-build" "" "$CMAKE_BIN" --build "$QT_BUILD_DIR" --target ptapp; then
      if [[ -x "$QT_PTAPP" || -f "$QT_PTAPP" ]]; then
        pass_step "qt-binary"
        QT_PASS_UI_PRESENT_HZ=0
        if qt_ui_present_hz_coverage "$QT_PTAPP"; then
          QT_PASS_UI_PRESENT_HZ=1
        fi
        qt_window_smoke "$QT_PTAPP" "$QT_PASS_UI_PRESENT_HZ"
        if qt_render_headless "$QT_PTAPP"; then
          qt_render_headless_output
        else
          skip_step "qt-render-headless-output" "render command failed"
        fi
      else
        fail_step "qt-binary" "ptapp not found at ${QT_PTAPP}"
        skip_step "qt-ui-present-hz-option" "Qt binary not built"
        skip_step "qt-window-bounded" "Qt binary not built"
        skip_step "qt-render-headless" "Qt binary not built"
        skip_step "qt-render-headless-output" "Qt binary not built"
      fi
    else
      skip_step "qt-binary" "Qt build failed"
      skip_step "qt-ui-present-hz-option" "Qt build failed"
      skip_step "qt-window-bounded" "Qt build failed"
      skip_step "qt-render-headless" "Qt build failed"
      skip_step "qt-render-headless-output" "Qt build failed"
    fi
  else
    skip_step "qt-build" "Qt configure did not complete"
    skip_step "qt-ui-present-hz-option" "Qt configure did not complete"
    skip_step "qt-window-bounded" "Qt configure did not complete"
    skip_step "qt-render-headless" "Qt configure did not complete"
    skip_step "qt-render-headless-output" "Qt configure did not complete"
  fi
fi

echo
if [[ "$FAIL" -eq 0 ]]; then
  echo "PASSED (${PASS} passed, ${SKIP} skipped)"
  exit 0
fi

echo "FAILED (${FAIL} failed, ${PASS} passed, ${SKIP} skipped)"
exit 1
