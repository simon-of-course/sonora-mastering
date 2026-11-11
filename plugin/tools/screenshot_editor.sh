#!/usr/bin/env bash
# Renders the real VSTGUI editors of all SONORA plug-ins with the VST3 SDK `editorhost`
# under a virtual X server (Xvfb) and saves cropped PNG screenshots.
#
# Usage: plugin/tools/screenshot_editor.sh <build-dir> <out-dir>
#   -> <out-dir>/master.png, shutter.png, drowner.png
# Env:   SONORA_SHOT_DELAY  seconds to let the editor paint after the window maps (default 3)
#        SONORA_SHOT_SCREEN Xvfb screen geometry (default 2400x1600x24)
# Needs: xvfb-run (xvfb), xprop/xwininfo (x11-utils), xdotool, import (imagemagick).
# Build editorhost with -DSONORA_BUILD_HOST_TOOLS=ON (default on Linux).
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <build-dir> <out-dir>" >&2
    exit 2
fi

BUILD_DIR="$(cd "$1" && pwd)"
mkdir -p "$2"
OUT_DIR="$(cd "$2" && pwd)"
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
DELAY="${SONORA_SHOT_DELAY:-3}"
SCREEN="${SONORA_SHOT_SCREEN:-2400x1600x24}"

# Re-run ourselves inside a private Xvfb. -noreset keeps interned atoms alive between clients.
if [[ -z "${SONORA_SHOT_INNER:-}" ]]; then
    for tool in xvfb-run xprop xwininfo xdotool import; do
        command -v "${tool}" >/dev/null || { echo "missing tool: ${tool}" >&2; exit 1; }
    done
    export SONORA_SHOT_INNER=1
    exec xvfb-run -a -s "-screen 0 ${SCREEN} -noreset -nolisten tcp" "${SELF}" "${BUILD_DIR}" "${OUT_DIR}"
fi

EDITORHOST="$(find "${BUILD_DIR}/bin" -type f -name editorhost -perm -u+x 2>/dev/null | head -n1 || true)"
if [[ -z "${EDITORHOST}" ]]; then
    echo "editorhost not found under ${BUILD_DIR}/bin (configure with -DSONORA_BUILD_HOST_TOOLS=ON and build target 'editorhost')" >&2
    exit 1
fi

# editorhost's X11 window refuses to open unless the _XEMBED_INFO atom already exists on the server.
xprop -root -f _XEMBED_INFO 32c -set _XEMBED_INFO "0, 1"

find_bundle() {
    find "${BUILD_DIR}/VST3" -maxdepth 2 -type d -name "$1.vst3" 2>/dev/null | head -n1
}

# Prints "W H X Y" of the deepest (plug-in) child window below the editorhost top-level window.
plug_geometry() {
    local win="$1" child
    while child="$(xwininfo -children -id "${win}" 2>/dev/null | awk '/^ +0x[0-9a-f]+ /{print $1; exit}')" && [[ -n "${child}" ]]; do
        win="${child}"
    done
    xwininfo -id "${win}" | awk '
        /Absolute upper-left X/ {x=$NF}
        /Absolute upper-left Y/ {y=$NF}
        /^ *Width:/ {w=$NF}
        /^ *Height:/ {h=$NF}
        END {print w, h, x, y}'
}

FAILED=0
shoot() {
    local target="$1" png="$2" bundle log pid wid w h x y
    bundle="$(find_bundle "${target}")"
    if [[ -z "${bundle}" ]]; then
        echo "FAIL ${target}: bundle not found under ${BUILD_DIR}/VST3" >&2
        FAILED=$((FAILED + 1)); return
    fi
    log="${OUT_DIR}/${png%.png}.editorhost.log"
    "${EDITORHOST}" "${bundle}" >"${log}" 2>&1 &
    pid=$!
    wid=""
    for _ in $(seq 1 100); do
        wid="$(xdotool search --onlyvisible --name '^Editor$' 2>/dev/null | head -n1 || true)"
        [[ -n "${wid}" ]] && break
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.2
    done
    if [[ -z "${wid}" ]]; then
        echo "FAIL ${target}: editor window did not appear (see ${log})" >&2
        kill "${pid}" 2>/dev/null || true; wait "${pid}" 2>/dev/null || true
        FAILED=$((FAILED + 1)); return
    fi
    sleep "${DELAY}"
    read -r w h x y <<<"$(plug_geometry "${wid}")"
    import -window root -crop "${w}x${h}+${x}+${y}" +repage "${OUT_DIR}/${png}"
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 25); do kill -0 "${pid}" 2>/dev/null || break; sleep 0.2; done
    kill -9 "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    # A uniformly coloured capture means the editor did not paint.
    if [[ "$(convert "${OUT_DIR}/${png}" -format '%[fx:standard_deviation]' info:)" == "0" ]]; then
        echo "FAIL ${target}: ${png} is a blank image" >&2
        FAILED=$((FAILED + 1)); return
    fi
    rm -f "${log}"
    echo "OK   ${target}: ${OUT_DIR}/${png} (${w}x${h})"
}

shoot SONORA_Master  master.png
shoot SONORA_Shutter shutter.png
shoot SONORA_Drowner drowner.png

[[ ${FAILED} -eq 0 ]]
