#!/usr/bin/env bash
# One-command Linux build: configure + build all SONORA plug-ins (plus validator/editorhost and
# the SDK-free tests), then run Steinberg's validator on every bundle and CTest.
#
# Usage: plugin/tools/build_linux.sh [build-dir] [--deps] [--clean] [--debug] [--no-tests]
#   build-dir  default /root/sonora-build when writable, else <repo>/build-linux
#   --deps     apt-get install the required packages first (needs root/sudo)
#   --clean    delete CMake cache before configuring (keeps the fetched SDK in _deps)
# Env: JOBS (default: nproc), CMAKE_EXTRA_ARGS (appended to the configure line)
set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_DIR="$(cd "${PLUGIN_DIR}/.." && pwd)"
BUILD_TYPE=Release
BUILD_DIR=""
DEPS=0
CLEAN=0
TESTS=ON

for arg in "$@"; do
    case "${arg}" in
        --deps) DEPS=1 ;;
        --clean) CLEAN=1 ;;
        --debug) BUILD_TYPE=Debug ;;
        --no-tests) TESTS=OFF ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        -*) echo "unknown option ${arg}" >&2; exit 2 ;;
        *) BUILD_DIR="${arg}" ;;
    esac
done
if [[ -z "${BUILD_DIR}" ]]; then
    if [[ -w /root ]]; then BUILD_DIR=/root/sonora-build; else BUILD_DIR="${REPO_DIR}/build-linux"; fi
fi
JOBS="${JOBS:-$(nproc)}"

APT_PACKAGES=(cmake ninja-build pkg-config g++ git
    libx11-xcb-dev libxcb-util-dev libxcb-cursor-dev libxcb-xkb-dev libxcb-keysyms1-dev
    libxkbcommon-dev libxkbcommon-x11-dev libfontconfig1-dev libfreetype-dev libcairo2-dev
    libpango1.0-dev libgtkmm-3.0-dev libsqlite3-dev libexpat1-dev)

if [[ ${DEPS} -eq 1 ]]; then
    SUDO=""; [[ $(id -u) -ne 0 ]] && SUDO=sudo
    ${SUDO} apt-get update -qq
    DEBIAN_FRONTEND=noninteractive ${SUDO} apt-get install -y -qq "${APT_PACKAGES[@]}"
fi

if [[ ${CLEAN} -eq 1 ]]; then
    rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles"
fi

GENERATOR=()
command -v ninja >/dev/null && GENERATOR=(-G Ninja)

t0=$(date +%s)
echo "==> configure ${BUILD_DIR} (${BUILD_TYPE})"
# shellcheck disable=SC2086
cmake -S "${PLUGIN_DIR}" -B "${BUILD_DIR}" "${GENERATOR[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSMTG_CREATE_PLUGIN_LINK=OFF \
    -DSONORA_BUILD_HOST_TOOLS=ON \
    -DSONORA_BUILD_TESTS="${TESTS}" \
    ${CMAKE_EXTRA_ARGS:-}

echo "==> build (-j${JOBS})"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --parallel "${JOBS}"
t1=$(date +%s)

VALIDATOR="$(find "${BUILD_DIR}/bin" -type f -name validator -perm -u+x | head -n1)"
FAILED=0
SUMMARY=()
for plugin in SONORA_Master SONORA_Shutter SONORA_Drowner; do
    bundle="$(find "${BUILD_DIR}/VST3" -maxdepth 2 -type d -name "${plugin}.vst3" | head -n1)"
    log="${BUILD_DIR}/validator_${plugin}.log"
    echo "==> validator ${plugin}"
    if [[ -n "${bundle}" ]] && "${VALIDATOR}" "${bundle}" >"${log}" 2>&1; then
        SUMMARY+=("PASS validator ${plugin}: $(grep -E 'Result:' "${log}" | tail -n1)")
    else
        SUMMARY+=("FAIL validator ${plugin} (log: ${log})")
        tail -n 40 "${log}" 2>/dev/null || true
        FAILED=$((FAILED + 1))
    fi
done

if [[ "${TESTS}" == "ON" ]]; then
    echo "==> ctest"
    if ctest --test-dir "${BUILD_DIR}" -C "${BUILD_TYPE}" --output-on-failure --no-tests=ignore; then
        SUMMARY+=("PASS ctest")
    else
        SUMMARY+=("FAIL ctest")
        FAILED=$((FAILED + 1))
    fi
fi

echo
echo "==================== build_linux summary ===================="
echo "build dir : ${BUILD_DIR}"
echo "build time: $((t1 - t0)) s"
echo "bundles   : ${BUILD_DIR}/VST3/${BUILD_TYPE}/SONORA_{Master,Shutter,Drowner}.vst3"
printf '%s\n' "${SUMMARY[@]}"
[[ ${FAILED} -eq 0 ]]
