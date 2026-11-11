#!/usr/bin/env bash
# Fetches the VST3 SDK at the commit pinned in plugin/CMakeLists.txt (GIT_TAG) into <dest-dir>,
# with the same submodules FetchContent uses. Configure with
#   -DFETCHCONTENT_SOURCE_DIR_VST3SDK=<dest-dir>
# to build against it without a network fetch (used by CI together with actions/cache).
# Usage: plugin/tools/fetch_sdk.sh <dest-dir>      prints the pinned SDK commit with --print-tag
set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="$(sed -n 's/^[[:space:]]*GIT_TAG[[:space:]]\+\([0-9a-f]\{40\}\).*/\1/p' "${PLUGIN_DIR}/CMakeLists.txt" | head -n1)"
SUBMODULES=(base cmake pluginterfaces public.sdk vstgui4)
URL=https://github.com/steinbergmedia/vst3sdk.git

if [[ -z "${TAG}" ]]; then
    echo "fetch_sdk.sh: could not find a 40-char GIT_TAG in ${PLUGIN_DIR}/CMakeLists.txt" >&2
    exit 1
fi
if [[ "${1:-}" == "--print-tag" ]]; then
    echo "${TAG}"
    exit 0
fi
if [[ $# -ne 1 ]]; then
    echo "usage: $0 <dest-dir> | --print-tag" >&2
    exit 2
fi
DEST="$1"

if [[ -f "${DEST}/.sonora_sdk_tag" && "$(cat "${DEST}/.sonora_sdk_tag")" == "${TAG}" ]]; then
    echo "VST3 SDK ${TAG} already present in ${DEST}"
    exit 0
fi

rm -rf "${DEST}"
mkdir -p "${DEST}"
git -C "${DEST}" init -q
git -C "${DEST}" remote add origin "${URL}"
git -C "${DEST}" fetch -q --depth 1 origin "${TAG}"
git -C "${DEST}" -c advice.detachedHead=false checkout -q FETCH_HEAD
if ! git -C "${DEST}" submodule update -q --init --depth 1 --jobs 4 -- "${SUBMODULES[@]}"; then
    echo "shallow submodule fetch failed, retrying with full history" >&2
    git -C "${DEST}" submodule update -q --init --jobs 4 -- "${SUBMODULES[@]}"
fi
# Git metadata is not needed for the build and only bloats the CI cache.
find "${DEST}" -name .git -prune -exec rm -rf {} +
echo "${TAG}" > "${DEST}/.sonora_sdk_tag"
echo "VST3 SDK ${TAG} fetched into ${DEST}"
