#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 SOURCE_ROOT BUILD_IMAGE PREFIX_DIRECTORY" >&2
  exit 2
fi
source_root=$(realpath "$1")
build_image=$2
prefix_directory=$3
if [[ ${prefix_directory} != /* || ${prefix_directory} == / ]]; then
  echo "TLS prefix output must be an absolute task-owned directory" >&2
  exit 2
fi
cd "${source_root}"
if [[ ! ${build_image} =~ ^ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "Classic TLS bootstrap requires an immutable Classic build image" >&2
  exit 2
fi
# Do not silently reuse another job's prefix, even if its manifest looks current.
python3 - "${prefix_directory}" <<'CHECK'
from pathlib import Path
import sys
from tools.ci.bootstrap_classic_tls import require_empty_prefix
require_empty_prefix(Path(sys.argv[1]))
CHECK
mkdir -p "${prefix_directory}"
docker run --rm --user 0:0 \
  --volume "${source_root}:/workspace:ro" \
  --volume "${prefix_directory}:/opt/atrinik/tls" \
  --workdir /workspace \
  "${build_image}" \
  python3 tools/ci/bootstrap_classic_tls.py --jobs 2
