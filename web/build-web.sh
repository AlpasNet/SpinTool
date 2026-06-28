#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-web"
DIST_DIR="${ROOT_DIR}/dist-web"
LOG_FILE="${ROOT_DIR}/build-web.log"
ZIP_FILE="${ROOT_DIR}/SpinTool-Web.zip"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "Emscripten is not active. Run: source /path/to/emsdk/emsdk_env.sh" >&2
    exit 1
fi

cmake -E rm -rf "${BUILD_DIR}" "${DIST_DIR}"
rm -f "${LOG_FILE}" "${ZIP_FILE}"

emcmake cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_VERBOSE_MAKEFILE=ON \
    -DSPINTOOL_BUILD_WEB=ON 2>&1 | tee "${LOG_FILE}"

set +e
cmake --build "${BUILD_DIR}" --parallel 1 --verbose 2>&1 | tee -a "${LOG_FILE}"
build_status=${PIPESTATUS[0]}
set -e

if (( build_status != 0 )); then
    printf '\n================ FIRST BUILD ERRORS ================\n' >&2
    grep -nE '(^|[[:space:]])(fatal error:|error:|undefined reference|wasm-ld: error:|em\+\+: error:)' \
        "${LOG_FILE}" | head -40 >&2 || true
    printf '====================================================\n' >&2
    printf 'Complete log: %s\n' "${LOG_FILE}" >&2
    exit "${build_status}"
fi

# Create a clean, downloadable web package. Keep every runtime sidecar that
# Emscripten may emit now or in future builds (.data, .worker.js, maps, etc.).
mkdir -p "${DIST_DIR}"

for required_file in index.html index.js index.wasm; do
    source_file="${BUILD_DIR}/${required_file}"
    if [[ ! -s "${source_file}" ]]; then
        echo "Required WebAssembly output is missing or empty: ${source_file}" >&2
        echo "Files produced in ${BUILD_DIR}:" >&2
        find "${BUILD_DIR}" -maxdepth 2 -type f -printf '%P : %s bytes\n' | sort >&2
        exit 1
    fi
    cp "${source_file}" "${DIST_DIR}/${required_file}"
done

# Copy optional Emscripten runtime files without failing when none exist.
while IFS= read -r -d '' source_file; do
    base_name="$(basename "${source_file}")"
    case "${base_name}" in
        index.html|index.js|index.wasm) continue ;;
    esac
    cp "${source_file}" "${DIST_DIR}/${base_name}"
done < <(find "${BUILD_DIR}" -maxdepth 1 -type f \
    \( -name 'index.*' -o -name '*.data' -o -name '*.worker.js' \) -print0)

(
    cd "${DIST_DIR}"
    sha256sum ./* > SHA256SUMS.txt
)

if command -v zip >/dev/null 2>&1; then
    (
        cd "${DIST_DIR}"
        zip -9 -q -r "${ZIP_FILE}" .
    )
else
    (
        cd "${DIST_DIR}"
        cmake -E tar cf "${ZIP_FILE}" --format=zip -- .
    )
fi

printf '\nWeb build created in: %s\n' "${BUILD_DIR}"
printf 'Ready-to-upload package: %s\n' "${DIST_DIR}"
printf 'ZIP package: %s\n' "${ZIP_FILE}"
printf 'Run: python3 %s/serve.py %s\n' "${ROOT_DIR}/web" "${DIST_DIR}"
find "${DIST_DIR}" -maxdepth 1 -type f -printf '%f : %s bytes\n' | sort
