#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 COMPONENT SOURCE_ROOT" >&2
  exit 2
fi

component=$1
source_root=$(realpath "$2")
jobs=$(nproc)
validation_exit=0

case "${component}" in
  client | client-benchmark | core | integrated | server | server-benchmark) ;;
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

dependency_bundle=${source_root}/build/dependency-inputs
dependency_downloads=${dependency_bundle}/downloads
if [[ ${component} != core ]]; then
  python3 "${source_root}/server/tools/dependencies.py" bundle-verify \
    --client-lock "${source_root}/client/dependencies.lock.json" \
    --server-lock "${source_root}/server/dependencies.lock.json" \
    --source-lock "${source_root}/server/cmake/immutable_sources.lock.json" \
    --bundle "${dependency_bundle}"
fi
if [[ ${component} == server || ${component} == server-benchmark || ${component} == integrated ]]; then
  pcpnatpmp_source=$(python3 "${source_root}/server/tools/dependencies.py" source \
    --source-lock "${source_root}/server/cmake/immutable_sources.lock.json" \
    --source-name libpcpnatpmp \
    --cache "${source_root}/build/dependency-source-cache" \
    --downloads "${dependency_downloads}" \
    --offline)
  sibling_sources+=(
    -DFETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP="${pcpnatpmp_source}"
  )
fi

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
    python3 tools/dependencies.py sync \
      --cache "${dependency_downloads}" --refresh --offline
    python3 tools/dependencies.py verify
    cmake --preset linux-coverage \
      -DENABLE_PRECOMPILED_HEADERS=OFF \
      "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-coverage --parallel "${jobs}"
    ctest --preset linux-coverage --parallel 4 -LE performance
    gcovr --root . --filter 'src/' --exclude 'src/tests/' \
      --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
      --print-summary --xml coverage.xml
    cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    ctest --preset linux-release --parallel 4 -LE performance
    cmake --preset linux-sanitizers "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-sanitizers --parallel "${jobs}"
    env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --preset linux-sanitizers --parallel 4 -LE performance
    popd >/dev/null
    ;;
  client)
    pushd "${source_root}/client" >/dev/null
    python3 -m unittest discover -s tools/tests -p 'test_*.py'
    python3 tools/dependencies.py sync \
      --cache "${dependency_downloads}" --refresh --offline
    python3 tools/dependencies.py verify
    cmake --preset linux-coverage \
      -DENABLE_PRECOMPILED_HEADERS=OFF \
      "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-coverage --parallel "${jobs}"
    ctest --preset linux-coverage -LE performance
    gcovr --root . --filter 'src/' --exclude 'src/tests/' \
      --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
      --print-summary --xml coverage.xml
    cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    ctest --preset linux-release -LE performance
    cmake --preset linux-sanitizers "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-sanitizers --parallel "${jobs}"
    env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
      ctest --preset linux-sanitizers -LE performance
    popd >/dev/null
    ;;
  client-benchmark)
    pushd "${source_root}/client" >/dev/null
    evidence_dir="${source_root}/build/ci-evidence"
    movement_evidence="${evidence_dir}/movement-frame-time.json"
    movement_baseline_schema="${evidence_dir}/movement-baseline-schema.py"
    lighting_evidence="${evidence_dir}/lighting-frame-time.json"
    rm -f "${movement_baseline_schema}"
    python3 tools/dependencies.py sync \
      --cache "${dependency_downloads}" --refresh --offline
    python3 tools/dependencies.py verify
    env ATRINIK_BENCHMARK_REVISION="${ATRINIK_BENCHMARK_REVISION:-unknown}" \
      ATRINIK_BENCHMARK_DIRTY=false \
      cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --target atrinik --parallel "${jobs}"
    if [[ ! -f ${source_root}/client/src/client/player_view.c ]]; then
      printf '%s\n' \
        '{"schema_version":1,"skipped":true,"reason":"gpu-hardware-qualification-required"}' \
        >"${lighting_evidence}"
      python3 "${source_root}/client/tools/benchmark_movement_regression.py" skip \
        --reason gpu-hardware-qualification-required \
        --output "${movement_evidence}"
    else
    benchmark_base_sha=${ATRINIK_BENCHMARK_BASE_SHA:-${ATRINIK_LIGHTING_BASE_SHA:-}}
    movement_event_name=${ATRINIK_MOVEMENT_EVENT_NAME:-unknown}
    movement_matrix=${ATRINIK_MOVEMENT_MATRIX:-fast}
    if [[ ${movement_matrix} != fast && ${movement_matrix} != full ]]; then
      echo "unsupported movement validation matrix: ${movement_matrix}" >&2
      exit 2
    fi

    lighting_paths=(
      client/src/client/lighting.c
      client/src/client/lighting_transfer.c
      client/src/client/player_view.c
      client/src/gui/widgets/map.c
      client/src/include/lighting.h
      protocol/schema/game-commands.json
    )
    movement_rendering_paths=(
      client/src/client/animations.c
      client/src/client/image.c
      client/src/client/image_codec.c
      client/src/client/lighting.c
      client/src/client/lighting_transfer.c
      client/src/client/settings.c
      client/src/client/sprite.c
      client/src/client/sprite_pixels.c
      client/src/client/texture.c
      client/src/client/tilestretcher.c
      client/src/client/video.c
      client/src/gui/misc/effects.c
      client/src/gui/toolkit/surface_primitives.c
      client/src/gui/widgets/map.c
      client/src/gui/widgets/minimap.c
      client/src/gui/widgets/render_profiler.c
      client/src/gui/widgets/texture.c
      client/src/include/effects.h
      client/src/include/animations.h
      client/src/include/image.h
      client/src/include/image_codec.h
      client/src/include/lighting.h
      client/src/include/map.h
      client/src/include/render_profiler.h
      client/src/include/sprite.h
      client/src/include/surface_primitives.h
      client/src/include/texture.h
      client/src/include/video.h
    )
    movement_paths=(
      "${movement_rendering_paths[@]}"
      .github/workflows/check.yml
      .github/workflows/pr-benchmarks.yml
      client/CMakeLists.txt
      client/src/client
      client/src/gui
      client/src/include
      client/src/tests/fixtures/player_view
      client/src/client/client.c
      client/src/client/client_command_queue.c
      client/src/client/commands.c
      client/src/client/main.c
      client/src/client/player_view.c
      client/src/client/socket.c
      client/src/cmake.txt
      client/src/include/client.h
      client/src/include/client_command_queue.h
      client/src/include/client_socket.h
      client/src/include/player_view.h
      cmake/AtrinikVersion.cmake
      client/src/tests/client_command_queue.c
      client/src/tests/fixtures/player_view/movement-colored-delta.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-five-depth.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-transition.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-discrete.xml
      client/src/tests/fixtures/player_view/movement-colored.xml
      client/src/tests/fixtures/player_view/movement-lighting-isolated.xml
      client/src/tests/fixtures/player_view/movement-lighting-static-delta.map2.hex
      client/src/tests/fixtures/player_view/README.md
      client/tools/benchmark_movement_regression.py
      client/tools/generate_movement_delta.py
      client/tools/generate_movement_five_depth.py
      client/tools/movement_benchmark_schema.py
      client/tools/tests/test_benchmark_movement_regression.py
      client/tools/tests/test_movement_fixture.py
      client/tools/tests/test_verify_movement_fault_injection.py
      client/tools/verify_movement_fault_injection.py
      client/tools/verify_movement_benchmark.py
      client/tools/verify_movement_probe_control.py
      libatrinik/tests/smoke.c
      libatrinik/math.c
      libatrinik/math.h
      protocol/schema/game-commands.json
      tools/ci/classify_changes.py
      tools/ci/run_linux_check.sh
      tools/tests/test_classify_changes.py
      tools/tests/test_workflow_contracts.py
    )
    movement_contract_paths=(
      client/src/tests/fixtures/player_view/movement-colored-delta.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-five-depth.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-transition.map2.hex
      client/src/tests/fixtures/player_view/movement-colored-discrete.xml
      client/src/tests/fixtures/player_view/movement-colored.xml
      client/src/tests/fixtures/player_view/movement-lighting-isolated.xml
      client/src/tests/fixtures/player_view/movement-lighting-static-delta.map2.hex
      client/tools/benchmark_movement_regression.py
      client/tools/generate_movement_delta.py
      client/tools/generate_movement_five_depth.py
      client/tools/movement_benchmark_schema.py
      client/tools/verify_movement_benchmark.py
      client/tools/verify_movement_probe_control.py
    )

    lighting_comparison=false
    lighting_base_missing_instrumentation=false
    movement_action=candidate-only
    comparison_note=event-has-no-comparison-base
    baseline_needed=false

    if [[ -n ${benchmark_base_sha} ]] &&
      ! git -C "${source_root}" diff --quiet "${benchmark_base_sha}" HEAD -- \
        "${lighting_paths[@]}"; then
      if git -C "${source_root}" grep -q --fixed-strings \
        -e '--player-view-benchmark' "${benchmark_base_sha}" -- \
        client/src/client/player_view.c; then
        lighting_comparison=true
        baseline_needed=true
      else
        lighting_base_missing_instrumentation=true
      fi
    fi

    if [[ ${movement_event_name} == pull_request || ${movement_event_name} == merge_group ]] &&
      [[ -n ${benchmark_base_sha} ]]; then
      if git -C "${source_root}" diff --quiet "${benchmark_base_sha}" HEAD -- \
        "${movement_paths[@]}"; then
        movement_action=skip
      elif ! git -C "${source_root}" cat-file -e \
        "${benchmark_base_sha}:client/tools/movement_benchmark_schema.py" ||
        ! git -C "${source_root}" cat-file -e \
          "${benchmark_base_sha}:client/tools/benchmark_movement_regression.py" ||
        ! git -C "${source_root}" grep -q --fixed-strings \
          -e '--player-view-movement-benchmark' "${benchmark_base_sha}" -- \
          client/src/client/player_view.c; then
        comparison_note=bootstrap-base-missing-movement-instrumentation
      elif ! git -C "${source_root}" diff --quiet \
        "${benchmark_base_sha}" HEAD -- "${movement_contract_paths[@]}"; then
        comparison_note=baseline-movement-schema-mismatch
        movement_action=compare
        baseline_needed=true
      else
        movement_action=compare
        baseline_needed=true
        comparison_note=performance-calibration-pending-sibling-integration
      fi
    fi
    if [[ ${movement_matrix} == full && ${movement_action} == compare ]]; then
      echo "the full movement matrix is candidate-only; refusing to omit its discrete/large contexts from a relative comparison" >&2
      exit 2
    fi

    baseline_root="${source_root}/build/ci-benchmark-baseline"
    if [[ ${baseline_needed} == true ]]; then
      # Invoked indirectly by the EXIT trap below.
      # shellcheck disable=SC2329
      cleanup_benchmark_baseline() {
        git -C "${source_root}" worktree remove --force "${baseline_root}" >/dev/null 2>&1 || true
      }
      trap cleanup_benchmark_baseline EXIT
      git -C "${source_root}" worktree add --detach \
        "${baseline_root}" "${benchmark_base_sha}"

      pushd "${baseline_root}/client" >/dev/null
      python3 "${source_root}/server/tools/dependencies.py" sync \
        --root "${baseline_root}/client" \
        --lock "${baseline_root}/client/dependencies.lock.json" \
        --cache "${dependency_downloads}" --refresh --offline
      python3 "${source_root}/server/tools/dependencies.py" verify \
        --root "${baseline_root}/client" \
        --lock "${baseline_root}/client/dependencies.lock.json"
      env ATRINIK_BENCHMARK_REVISION="${benchmark_base_sha}" \
        ATRINIK_BENCHMARK_DIRTY=false \
        cmake --preset linux-release \
        "${launcher[@]}" \
        -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="${baseline_root}/protocol" \
        -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="${baseline_root}/libatrinik"
      cmake --build --preset linux-release --target atrinik --parallel "${jobs}"
      popd >/dev/null
      if ! git -C "${baseline_root}" diff --quiet HEAD --; then
        echo "benchmark comparison build modified its immutable base source" >&2
        exit 2
      fi
      if [[ ${movement_action} == compare ]]; then
        cp -- "${baseline_root}/client/tools/movement_benchmark_schema.py" \
          "${movement_baseline_schema}"
      fi
    fi

    if [[ ${lighting_comparison} == true ]]; then
      command_status=0
      python3 "${source_root}/client/tools/benchmark_lighting_regression.py" \
        --baseline-client "${baseline_root}/client/build/linux-release/atrinik" \
        --baseline-manifest \
          "${baseline_root}/client/src/tests/fixtures/player_view/colored-smooth.xml" \
        --candidate-client "${source_root}/client/build/linux-release/atrinik" \
        --candidate-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/colored-smooth.xml" \
        --samples 3 \
        --output "${lighting_evidence}" || command_status=$?
      if [[ ${command_status} -ne 0 ]]; then
        printf '%s\n' \
          '{"schema_version":1,"error":true,"reason":"lighting-regression-generation-failed"}' \
          >"${lighting_evidence}"
        validation_exit=${command_status}
      fi
    elif [[ ${lighting_base_missing_instrumentation} == true ]]; then
      printf '%s\n' \
        '{"schema_version":1,"skipped":true,"reason":"lighting-base-missing-benchmark-instrumentation"}' \
        >"${lighting_evidence}"
    else
      printf '%s\n' \
        '{"schema_version":1,"skipped":true,"reason":"lighting-sensitive-files-unchanged-or-no-comparison-base"}' \
        >"${lighting_evidence}"
    fi

    probe_status=0
    if [[ ${movement_action} != skip ]]; then
      python3 "${source_root}/client/tools/verify_movement_probe_control.py" \
        "${source_root}/client/build/linux-release/atrinik" \
        "${source_root}/client/src/tests/fixtures/player_view/movement-lighting-isolated.xml" \
        || probe_status=$?
    fi
    if [[ ${probe_status} -ne 0 ]]; then
      validation_exit=${probe_status}
    fi

    command_status=0
    if [[ ${movement_action} == compare ]]; then
      python3 "${source_root}/client/tools/benchmark_movement_regression.py" compare \
        --baseline-client "${baseline_root}/client/build/linux-release/atrinik" \
        --baseline-manifest \
          "${baseline_root}/client/src/tests/fixtures/player_view/movement-colored.xml" \
        --baseline-schema "${movement_baseline_schema}" \
        --candidate-client "${source_root}/client/build/linux-release/atrinik" \
        --candidate-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/movement-colored.xml" \
        --discrete-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/movement-colored-discrete.xml" \
          --lighting-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/movement-lighting-isolated.xml" \
        --informational-performance \
        --comparison-note "${comparison_note}" \
        --baseline-revision "${benchmark_base_sha}" \
        --candidate-revision "${ATRINIK_BENCHMARK_REVISION:-unknown}" \
        --samples 3 \
        --output "${movement_evidence}" || command_status=$?
    elif [[ ${movement_action} == candidate-only ]]; then
      movement_matrix_arguments=(
        --discrete-manifest
        "${source_root}/client/src/tests/fixtures/player_view/movement-colored-discrete.xml"
      )
      if [[ ${movement_matrix} == full ]]; then
        movement_matrix_arguments+=(--full-matrix)
      fi
      python3 "${source_root}/client/tools/benchmark_movement_regression.py" candidate-only \
        --candidate-client "${source_root}/client/build/linux-release/atrinik" \
        --candidate-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/movement-colored.xml" \
        --candidate-revision "${ATRINIK_BENCHMARK_REVISION:-unknown}" \
        --comparison-note "${comparison_note}" \
        "${movement_matrix_arguments[@]}" \
        --lighting-manifest \
          "${source_root}/client/src/tests/fixtures/player_view/movement-lighting-isolated.xml" \
        --output "${movement_evidence}" || command_status=$?
    else
      python3 "${source_root}/client/tools/benchmark_movement_regression.py" skip \
        --reason movement-sensitive-files-unchanged \
        --output "${movement_evidence}" || command_status=$?
    fi
    if [[ ${command_status} -ne 0 ]]; then
      validation_exit=${command_status}
    fi

    if [[ ${baseline_needed} == true ]]; then
      git -C "${source_root}" worktree remove --force "${baseline_root}"
      trap - EXIT
    fi
    fi
    popd >/dev/null
    ;;
  server-benchmark)
    benchmark_base_sha=${ATRINIK_BENCHMARK_BASE_SHA:-}
    if [[ ! ${benchmark_base_sha} =~ ^[0-9a-f]{40}$ ]] ||
      ! git -C "${source_root}" cat-file -e "${benchmark_base_sha}^{commit}"; then
      echo "server benchmark requires a fetched full base commit ID" >&2
      exit 2
    fi

    pushd "${source_root}/server" >/dev/null
    python3 tools/dependencies.py sync \
      --cache "${dependency_downloads}" --refresh --offline
    python3 tools/dependencies.py verify
    cmake --preset linux-release "${launcher[@]}" "${sibling_sources[@]}"
    cmake --build --preset linux-release --parallel "${jobs}"
    popd >/dev/null

    baseline_root="${source_root}/build/ci-server-benchmark-baseline"
    # Invoked indirectly by the EXIT trap below.
    # shellcheck disable=SC2329
    cleanup_server_benchmark_baseline() {
      git -C "${source_root}" worktree remove --force "${baseline_root}" >/dev/null 2>&1 || true
    }
    trap cleanup_server_benchmark_baseline EXIT
    git -C "${source_root}" worktree add --detach \
      "${baseline_root}" "${benchmark_base_sha}"
    python3 "${source_root}/server/tools/dependencies.py" sync \
      --root "${baseline_root}/server" \
      --lock "${baseline_root}/server/dependencies.lock.json" \
      --cache "${dependency_downloads}" --refresh --offline
    python3 "${source_root}/server/tools/dependencies.py" verify \
      --root "${baseline_root}/server" \
      --lock "${baseline_root}/server/dependencies.lock.json"
    baseline_pcpnatpmp_source=$(python3 "${source_root}/server/tools/dependencies.py" source \
      --source-lock "${baseline_root}/server/cmake/immutable_sources.lock.json" \
      --source-name libpcpnatpmp \
      --cache "${source_root}/build/dependency-source-cache" \
      --downloads "${dependency_downloads}" \
      --offline)
    pushd "${baseline_root}/server" >/dev/null
    cmake --preset linux-release \
      "${launcher[@]}" \
      -DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL="${baseline_root}/protocol" \
      -DFETCHCONTENT_SOURCE_DIR_LIBATRINIK="${baseline_root}/libatrinik" \
      -DFETCHCONTENT_SOURCE_DIR_LIBPCPNATPMP="${baseline_pcpnatpmp_source}"
    cmake --build --preset linux-release --parallel "${jobs}"
    popd >/dev/null
    if ! git -C "${baseline_root}" diff --quiet HEAD --; then
      echo "server benchmark build modified its immutable base source" >&2
      exit 2
    fi

    ctest --test-dir "${baseline_root}/server/build/linux-release" \
      --verbose -R '^server-content-benchmark$' 2>&1 | \
      tee "${source_root}/build/ci-evidence/server-content-benchmark-base.log"
    ctest --test-dir "${source_root}/server/build/linux-release" \
      --verbose -R '^server-content-benchmark$' 2>&1 | \
      tee "${source_root}/build/ci-evidence/server-content-benchmark-head.log"
    # Markdown code spans are intentionally literal.
    # shellcheck disable=SC2016
    printf '%s\n' \
      '## Server content benchmark' \
      '' \
      "- Base: \`${benchmark_base_sha}\`" \
      "- Head: \`${ATRINIK_BENCHMARK_REVISION:-unknown}\`" \
      '- Result: both isolated content-loader probes completed successfully.' \
      '- Evidence: `server-content-benchmark-base.log` and `server-content-benchmark-head.log`.' \
      >"${source_root}/build/ci-evidence/server-content-benchmark-summary.md"

    git -C "${source_root}" worktree remove --force "${baseline_root}"
    trap - EXIT
    ;;
  integrated)
    pushd "${source_root}" >/dev/null
    cmake --preset linux-release \
      -DBUILD_TESTING=OFF \
      "${launcher[@]}" "${sibling_sources[@]}"
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
exit "${validation_exit}"
