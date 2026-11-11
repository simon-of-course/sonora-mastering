#!/usr/bin/env bash
# Compiles every plugin/tests/test_*.cpp standalone (SDK-free) and runs it, twice:
#   pass 1: -O2 -Wall -Wextra -Werror
#   pass 2: -O1 -g with AddressSanitizer + UndefinedBehaviorSanitizer
# Usage: plugin/tests/run_tests.sh [filter-substring]
# Env:   CXX (default g++), SONORA_TEST_OUT (binary dir, default <repo>/plugin/tests/.bin),
#        SONORA_SKIP_SANITIZERS=1 to skip pass 2.
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$(cd "${TESTS_DIR}/.." && pwd)"
SRC_DIR="${PLUGIN_DIR}/src"
CXX="${CXX:-g++}"
OUT_DIR="${SONORA_TEST_OUT:-${TESTS_DIR}/.bin}"
FILTER="${1:-}"

mkdir -p "${OUT_DIR}"
# Keep build products out of git without touching tracked .gitignore files.
[[ -f "${OUT_DIR}/.gitignore" ]] || echo "*" > "${OUT_DIR}/.gitignore"

COMMON_FLAGS=(-std=c++17 -Wall -Wextra -Werror
    -I "${SRC_DIR}" -I "${SRC_DIR}/dsp" -I "${SRC_DIR}/ui" -I "${TESTS_DIR}")
PASS_NAMES=("release" "sanitize")
PASS_FLAGS=("-O2" "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all")

shopt -s nullglob
SOURCES=()
for f in "${TESTS_DIR}"/test_*.cpp; do
    if [[ -z "${FILTER}" || "$(basename "$f")" == *"${FILTER}"* ]]; then
        SOURCES+=("$f")
    fi
done

if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "run_tests.sh: no test_*.cpp found in ${TESTS_DIR} (filter: '${FILTER}')" >&2
    exit 1
fi

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:strict_string_checks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

RESULTS=()
FAILED=0
TOTAL=0

for pass in "${!PASS_NAMES[@]}"; do
    pname="${PASS_NAMES[$pass]}"
    if [[ "${pname}" == "sanitize" && "${SONORA_SKIP_SANITIZERS:-0}" == "1" ]]; then
        continue
    fi
    read -r -a pflags <<< "${PASS_FLAGS[$pass]}"
    echo "=================== pass: ${pname} (${PASS_FLAGS[$pass]}) ==================="
    for src in "${SOURCES[@]}"; do
        name="$(basename "${src}" .cpp)"
        bin="${OUT_DIR}/${name}.${pname}"
        TOTAL=$((TOTAL + 1))
        echo "--- [${pname}] compile ${name}"
        t0=$(date +%s)
        if ! "${CXX}" "${COMMON_FLAGS[@]}" "${pflags[@]}" -o "${bin}" "${src}" -lm -lpthread; then
            RESULTS+=("FAIL  [${pname}] ${name} (compile)")
            FAILED=$((FAILED + 1))
            continue
        fi
        echo "--- [${pname}] run ${name}"
        (cd "${OUT_DIR}" && "${bin}")
        rc=$?
        t1=$(date +%s)
        if [[ ${rc} -eq 0 ]]; then
            RESULTS+=("PASS  [${pname}] ${name} ($((t1 - t0)) s)")
        else
            RESULTS+=("FAIL  [${pname}] ${name} (exit ${rc})")
            FAILED=$((FAILED + 1))
        fi
    done
done

echo
echo "=================== summary ==================="
printf '%s\n' "${RESULTS[@]}"
echo "${TOTAL} run(s), $((TOTAL - FAILED)) passed, ${FAILED} failed"
[[ ${FAILED} -eq 0 ]]
