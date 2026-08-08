#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-${ROOT_DIR}/build/server-debug}
JOBS=${JOBS:-$(nproc)}

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPACKAGE_TYPE=none
cmake --build "${BUILD_DIR}" --target atrinik-server --parallel "${JOBS}"

python3 "${ROOT_DIR}/tools/dependencies.py" sync
"${ROOT_DIR}/tools/prepare-runtime.sh" "${BUILD_DIR}"

(
    cd "${ROOT_DIR}"
    ./atrinik-server --worldmaker
)

map_count=$(find "${ROOT_DIR}/data/http/client-maps" \
    -maxdepth 1 -type f -name '*.png' | wc -l)
echo "Generated ${map_count} region maps in data/http/client-maps."
