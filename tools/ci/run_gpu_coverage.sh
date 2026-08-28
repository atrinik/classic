#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 SOURCE_ROOT BUILD_IMAGE GPU_IMAGE" >&2
  exit 2
fi

source_root=$(realpath "$1")
build_image=$2
gpu_image=$3
client_root=${source_root}/client
coverage_build=${client_root}/build/linux-coverage

if [[ ! ${build_image} =~ ^ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "build image must be an immutable versioned Atrinik Classic digest" >&2
  exit 2
fi
if [[ ! ${gpu_image} =~ ^ghcr\.io/atrinik/linux-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "GPU image must be an immutable versioned Atrinik Linux digest" >&2
  exit 2
fi
if [[ ! -x ${coverage_build}/client-gpu-renderer-integration-tests ||
      ! -x ${coverage_build}/atrinik ]]; then
  echo "the complete client coverage build must exist before GPU coverage" >&2
  exit 2
fi
command -v docker >/dev/null

compiled_home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
  "${coverage_build}/CMakeCache.txt")
if [[ ${compiled_home} != /*/client ]]; then
  echo "client coverage build has an invalid compiled source root" >&2
  exit 2
fi
compiled_root=${compiled_home%/client}
if [[ ${compiled_root} == / || -z ${compiled_root} ]]; then
  echo "client coverage build resolved an unsafe compiled source root" >&2
  exit 2
fi

# Keep the shell as PID 1: xvfb-run waits indefinitely for its readiness
# signal when it directly owns the container's PID 1 slot.
docker run --rm \
  --user "$(id -u):$(id -g)" \
  --network none \
  --env ATRINIK_GPU_CONFORMANCE_DRIVER=vulkan \
  --env ATRINIK_GPU_CONFORMANCE_REQUIRED=1 \
  --env SDL_VIDEODRIVER=x11 \
  --env VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  --env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  --volume "${source_root}:${compiled_root}" \
  --workdir "${compiled_home}" \
  "${gpu_image}" \
  sh -c "xvfb-run -a ctest --test-dir build/linux-coverage --output-on-failure --no-tests=error -R '^client-gpu-renderer-integration$'"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  --network none \
  --volume "${source_root}:${compiled_root}" \
  --workdir "${compiled_root}/client" \
  "${build_image}" \
  gcovr --root . --filter src/ --exclude src/tests/ \
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
    --print-summary --xml coverage.xml
