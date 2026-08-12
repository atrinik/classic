#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 COMPONENT SOURCE_ROOT" >&2
  exit 2
fi

component=$1
source_root=$(realpath "$2")
jobs=$(nproc)

case "${component}" in
  client | core | integrated | server) ;;
  *)
    echo "unsupported Linux Check component: ${component}" >&2
    exit 2
    ;;
esac

export CCACHE_BASEDIR=${source_root}
export CCACHE_COMPILERCHECK=content
export CCACHE_DIR=${CCACHE_DIR:-/cache/ccache}
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-500M}
export CCACHE_NOHASHDIR=true
export CCACHE_TEMPDIR=${CCACHE_TEMPDIR:-/tmp/classic-ccache-${component}}
export HOME=${HOME:-/tmp/classic-home}

mkdir -p "${HOME}" "${CCACHE_TEMPDIR}" "${source_root}/build/ci-evidence"
test -w "${CCACHE_DIR}"
test "$(stat --format='%a' "${CCACHE_DIR}")" = 1777
test "$(jq -r '.target' /usr/local/share/atrinik/classic-toolchain.json)" = classic-final
test "$(jq -r '.platform' /usr/local/share/atrinik/classic-toolchain.json)" = linux/amd64
test "$(jq -r '.tools.gcc' /usr/local/share/atrinik/classic-toolchain.json)" = 15.2.0

launcher=(
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)
sibling_sources=(
  -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="${source_root}/protocol"
  -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="${source_root}/libatrinik"
)

ccache --zero-stats >/dev/null

case "${component}" in
  core)
    cmake -S "${source_root}/protocol" -B "${source_root}/protocol/build/check" \
      -G Ninja -DCMAKE_BUILD_TYPE=Release "${launcher[@]}"
    cmake --build "${source_root}/protocol/build/check" --parallel "${jobs}"
    ctest --test-dir "${source_root}/protocol/build/check" --output-on-failure

    pushd "${source_root}/libatrinik" >/dev/null
    cmake --preset linux-coverage \
      -DATRINIK_PROTOCOL_SOURCE_DIR="${source_root}/protocol" \
      "${launcher[@]}"
    cmake --build --preset linux-coverage --parallel "${jobs}"
    ctest --preset linux-coverage
    gcovr --root . --filter '.*\.c$' --exclude '.*/tests/.*' \
      --exclude '.*/build/.*' \
      --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
      --print-summary --xml coverage.xml
    cmake --preset linux-sanitizers \
      -DATRINIK_PROTOCOL_SOURCE_DIR="${source_root}/protocol" \
      "${launcher[@]}"
    cmake --build --preset linux-sanitizers --parallel "${jobs}"
    env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --preset linux-sanitizers
    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${PWD}/build/install" \
      -DATRINIK_PROTOCOL_SOURCE_DIR="${source_root}/protocol" \
      "${launcher[@]}"
    cmake --build build/release --parallel "${jobs}"
    cmake --install build/release
    cmake -S tests/consumer -B build/consumer \
      -DCMAKE_PREFIX_PATH="${PWD}/build/install" "${launcher[@]}"
    cmake --build build/consumer --parallel "${jobs}"
    build/consumer/libatrinik-consumer
    popd >/dev/null
    ;;
  server)
    pushd "${source_root}/server" >/dev/null
    python3 -m unittest discover -s tools/tests -p 'test_*.py'
    python3 tools/dependencies.py sync
    python3 tools/dependencies.py verify
    cmake --preset linux-coverage \
      -DENABLE_PRECOMPILED_HEADERS=OFF \
      "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-coverage --parallel "${jobs}"
    ctest --preset linux-coverage --parallel 4
    gcovr --root . --filter 'src/' --exclude 'src/tests/' \
      --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
      --print-summary --xml coverage.xml
    cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    ctest --preset linux-release --parallel 4
    cmake --preset linux-sanitizers "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-sanitizers --parallel "${jobs}"
    env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --preset linux-sanitizers --parallel 4
    popd >/dev/null
    ;;
  client)
    pushd "${source_root}/client" >/dev/null
    python3 -m unittest discover -s tools/tests -p 'test_*.py'
    python3 tools/dependencies.py sync
    python3 tools/dependencies.py verify
    cmake --preset linux-coverage \
      -DENABLE_PRECOMPILED_HEADERS=OFF \
      "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-coverage --parallel "${jobs}"
    ctest --preset linux-coverage
    gcovr --root . --filter 'src/' --exclude 'src/tests/' \
      --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
      --print-summary --xml coverage.xml
    cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    ctest --preset linux-release
    lighting_base_sha=${ATRINIK_LIGHTING_BASE_SHA:-}
    if [[ -n ${lighting_base_sha} ]] &&
      ! git -C "${source_root}" diff --quiet "${lighting_base_sha}" HEAD -- \
        client/src/client/lighting.c \
        client/src/client/lighting_transfer.c \
        client/src/client/player_view.c \
        client/src/gui/widgets/map.c \
        client/src/include/lighting.h \
        protocol/schema/game-commands.json; then
      baseline_root="${source_root}/build/ci-lighting-baseline"
      instrumentation_patch="${source_root}/build/ci-lighting-benchmark.patch"
      cleanup_lighting_baseline() {
        git -C "${source_root}" worktree remove --force "${baseline_root}" >/dev/null 2>&1 || true
      }
      trap cleanup_lighting_baseline EXIT
      git -C "${source_root}" worktree add --detach "${baseline_root}" "${lighting_base_sha}"
      git -C "${source_root}" diff --binary "${lighting_base_sha}" HEAD -- \
        client/src/client/main.c \
        client/src/client/player_view.c \
        client/src/include/player_view.h >"${instrumentation_patch}"
      if ! grep -q -- '--player-view-benchmark' \
        "${baseline_root}/client/src/client/player_view.c"; then
        git -C "${baseline_root}" apply "${instrumentation_patch}"
      fi

      pushd "${baseline_root}/client" >/dev/null
      python3 tools/dependencies.py sync
      python3 tools/dependencies.py verify
      cmake --preset linux-release \
        "${launcher[@]}" \
        -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="${baseline_root}/protocol" \
        -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="${baseline_root}/libatrinik"
      cmake --build --preset linux-release --target atrinik --parallel "${jobs}"
      popd >/dev/null

      python3 "${source_root}/client/tools/benchmark_lighting_regression.py" \
        --baseline-client "${baseline_root}/client/build/linux-release/atrinik" \
        --baseline-manifest \
          "${baseline_root}/client/src/tests/fixtures/player_view/colored-smooth.xml" \
        --candidate-client "${source_root}/client/build/linux-release/atrinik" \
        --candidate-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/colored-smooth.xml" \
        --samples 3 \
        --output "${source_root}/build/ci-evidence/lighting-frame-time.json"
      git -C "${source_root}" worktree remove --force "${baseline_root}"
      trap - EXIT
    else
      printf '%s\n' '{"schema_version":1,"skipped":true}' \
        >"${source_root}/build/ci-evidence/lighting-frame-time.json"
    fi
    cmake --preset linux-sanitizers "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-sanitizers --parallel "${jobs}"
    env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --preset linux-sanitizers
    popd >/dev/null
    ;;
  integrated)
    pushd "${source_root}" >/dev/null
    cmake --preset linux-release \
      -DBUILD_TESTING=OFF \
      "${launcher[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    popd >/dev/null
    ;;
esac

stats_file="${source_root}/build/ci-evidence/ccache-${component}.tsv"
ccache --print-stats | tee "${stats_file}"
cacheable_calls=$(awk -F '\t' '
  $1 == "cache_miss" || $1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" {
    calls += $2
  }
  END { print calls + 0 }
' "${stats_file}")
if [[ ${cacheable_calls} -lt 1 ]]; then
  echo "${component} validation made no cacheable compiler calls" >&2
  exit 1
fi
ccache --show-config
ccache --show-stats
