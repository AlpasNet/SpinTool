#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-web"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "Emscripten is not active. Run: source /path/to/emsdk/emsdk_env.sh" >&2
    exit 1
fi

cmake -E rm -rf "${BUILD_DIR}"
emcmake cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPINTOOL_BUILD_WEB=ON
cmake --build "${BUILD_DIR}" --parallel

printf '\nWeb build created in: %s\n' "${BUILD_DIR}"
printf 'Run: python3 %s/serve.py %s\n' "${ROOT_DIR}/web" "${BUILD_DIR}"
