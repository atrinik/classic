#!/usr/bin/env bash

set -euo pipefail

repository_root=$(git rev-parse --show-toplevel)
descriptor=${repository_root}/dependencies.bundle.json
bundle_root=${repository_root}/build/dependency-bundle
tool=${repository_root}/tools/release/dependency_bundle.py

image=$(python3 "${tool}" --root "${repository_root}" \
  --descriptor "${descriptor}" show --field image)
digest=$(python3 "${tool}" --root "${repository_root}" \
  --descriptor "${descriptor}" show --field digest)
reference=${image}@${digest}

docker pull "${reference}"
docker image inspect "${reference}" >/dev/null
container=$(docker create "${reference}" true)
cleanup() {
  docker rm "${container}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cmake -E remove_directory "${bundle_root}"
cmake -E make_directory "${bundle_root}"
docker cp "${container}:/bundle/." "${bundle_root}"
python3 "${tool}" --root "${repository_root}" \
  --descriptor "${descriptor}" install --bundle "${bundle_root}"
cmake -E remove_directory "${bundle_root}"
