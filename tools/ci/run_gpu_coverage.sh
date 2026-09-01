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
coverage_record=${coverage_build}/gpu-ui-closure.jsonl

expected_revision=$(git -C "${source_root}" rev-parse --verify HEAD)
if [[ ! ${expected_revision} =~ ^[0-9a-f]{40}$ ]]; then
  echo "GPU coverage requires an exact Git revision" >&2
  exit 2
fi
if [[ -n $(git -C "${source_root}" status --porcelain=v1 --untracked-files=normal) ]]; then
  echo "GPU coverage requires a clean source checkout" >&2
  exit 2
fi

if [[ ! ${build_image} =~ ^ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "build image must be an immutable versioned Atrinik Classic digest" >&2
  exit 2
fi
if [[ ! ${gpu_image} =~ ^ghcr\.io/atrinik/linux-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "GPU image must be an immutable versioned Atrinik Linux digest" >&2
  exit 2
fi
command -v docker >/dev/null

docker run --rm \
  --user "$(id -u):$(id -g)" \
  --network none \
  --env ATRINIK_BENCHMARK_REVISION="${expected_revision}" \
  --env ATRINIK_BENCHMARK_DIRTY=false \
  --volume "${source_root}:/workspace" \
  --workdir /workspace \
  "${build_image}" \
  sh -c '
      set -eu
      python3 server/tools/dependencies.py bundle-verify \
        --client-lock client/dependencies.lock.json \
        --server-lock server/dependencies.lock.json \
        --source-lock server/cmake/immutable_sources.lock.json \
        --bundle build/dependency-inputs
      python3 client/tools/dependencies.py sync \
        --cache build/dependency-inputs/downloads --refresh --offline
      python3 client/tools/dependencies.py verify
      cd client
      cmake --preset linux-coverage \
        -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=/workspace/protocol \
        -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=/workspace/libatrinik \
        -DATRINIK_GPU_SHADER_DIRECTORY=/workspace/build/gpu-shaders
      cmake --build --preset linux-coverage --parallel "$(nproc)"
      ctest --preset linux-coverage --output-on-failure --no-tests=error -LE performance
    '

if [[ ! -x ${coverage_build}/client-gpu-renderer-integration-tests ||
      ! -x ${coverage_build}/atrinik ]]; then
  echo "the complete client coverage build is unavailable after GPU coverage setup" >&2
  exit 2
fi

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
  sh -c "xvfb-run -a sh -c 'set -eu; \
    ./build/linux-coverage/client-gpu-renderer-integration-tests; \
    ./build/linux-coverage/atrinik --gpu-player-view \
      src/tests/fixtures/player_view/gpu-ui-closure.xml \
      > build/linux-coverage/gpu-ui-closure.jsonl; \
    if ATRINIK_GPU_CONFORMANCE_TEST_SUPPRESS_ROOT_GLYPH=intro_server_browser \
      ./build/linux-coverage/atrinik --gpu-player-view \
        src/tests/fixtures/player_view/gpu-ui-closure.xml >/dev/null 2>&1; then \
      echo GPU UI closure accepted a suppressed root glyph >&2; \
      exit 1; \
    fi'"

python3 "${client_root}/tools/verify_gpu_coverage_record.py" \
  --revision "${expected_revision}" "${coverage_record}"

docker run --rm \
  --user "$(id -u):$(id -g)" \
  --network none \
  --volume "${source_root}:${compiled_root}" \
  --workdir "${compiled_root}/client" \
  "${build_image}" \
  gcovr --root . --filter src/ --exclude src/tests/ \
    --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
    --print-summary --xml coverage.xml
