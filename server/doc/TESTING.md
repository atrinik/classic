# Server test isolation

The Linux CTest presets run at most four server tests concurrently. A prepared
`server-test-runtime-seed` is build input only; no test executes from it. Before
each invocation, `tools/run_isolated_test.py` replaces only that test's exact
named directory below `server-test-runtimes/` and retains the resulting state
and logs after success or failure.

| Test family | Read-only input | Private writable resources | Processes and ports |
| --- | --- | --- | --- |
| `server-unit-*` | maps, content libraries, resources, plugins | copied `data`, config, `assets`, `TMPDIR`, Check XML/log output | one process group; `--unit` opens no game listener and port mapping is disabled |
| `server-lsan-smoke` | same as unit suites | same as unit suites, plus its own sanitizer output | one process group; leak checking uses `CK_FORK=no` |
| `server-plugin-python*` | non-Python maps, content libraries, resources, plugins | copied Python map tree and bytecode, selected `python_unit.py`, `data`, config, assets, temporary files, output | runner owns the CMake driver and server process group; plugin-unit mode opens no listener |
| `server-content-benchmark` | maps, content libraries, resources, plugins | copied `data`, config, assets, temporary files, benchmark output | offline benchmark process group; port mapping is disabled |
| `server-assetspath-migration` | maps, content libraries, resources, plugins | copied `data`, temporary configuration, per-test asset trees and `TMPDIR` | Python driver and every server child share one owned process group; worldmaker opens no listener; CTest reserves all four slots and each sanitizer-safe child remains bounded to 30 seconds |

The runner inherits CTest's environment, then sets `TMPDIR` to the private
runtime's `tmp` directory and `ATRINIK_TEST_ARTIFACT_DIR` to that runtime's
root. Unit entries additionally select only their own `ATRINIK_TEST_SUITE`.
The leak-smoke entry sets `ASAN_OPTIONS=detect_leaks=1` and `CK_FORK=no`; other
sanitizer entries inherit CI's bounded `ASAN_OPTIONS` and `UBSAN_OPTIONS`.
No entry allocates a listener port, and every explicit port-mapping surface is
disabled.

Every runner has a 270-second internal timeout inside CTest's 300-second outer
timeout. Pass, failure, interruption, and internal timeout wait for or terminate
the complete process group, escalating to `SIGKILL` after five seconds when
needed. Re-running a test reconstructs its private runtime from the unchanged
seed; other tests and their retained artifacts are not touched.

LLVM 23.1.3 coverage instruments production code with
`-fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic`. Atomic
counter updates preserve accurate counts when authentication and QUIC workers
execute instrumented functions concurrently. The same flags apply to shared
libatrinik code, the integrated build and standalone modules.

From the server directory in a full Classic checkout:

```sh
cmake --preset llvm-coverage
cmake --build --preset llvm-coverage
python3 ../tools/ci/llvm_coverage.py prepare --build-dir build/llvm-coverage
ctest --preset llvm-coverage --parallel 4 -LE performance
python3 ../tools/ci/llvm_coverage.py export --build-dir build/llvm-coverage \
  --source-root .. --component server --output coverage.lcov
```

The `llvm-coverage` preset selects a separate `build/llvm-coverage` cache so
existing compiler selections in `linux-coverage` trees remain intact. The
server coverage preset disables precompiled headers, matching native CI.
LLVM 23.1.3 emits different coverage mapping descriptors when shared sources
are compiled through the server precompiled header and directly into standalone
tools. Compiling coverage targets without precompiled headers keeps those maps
consistent for strict export, including zero-hit sources in unexecuted tools.
Normal builds, including the Release preset, retain their precompiled-header
default. Coverage instrumentation, test selection and export validation remain
the same.

The coverage test preset sets `LLVM_PROFILE_FILE` to the build's isolated
`llvm-profiles/%m-%p.profraw` path. The binary signature and process ID separate
parallel processes and instrumented plugins. `prepare` clears only that build's
previous profiles before a fresh pass; additional runtime probes retain the
existing profiles. Matching Clang, llvm-profdata and llvm-cov merge those files
and export LCOV for production server C sources, including plugins, while
excluding tests and generated dependency/build output. Missing profiles,
version mismatches and LLVM diagnostics fail collection. Compare sequential and
parallel LCOV counts and stress randomized four-way scheduling before accepting
parallel coverage evidence.

Use `ctest --preset PRESET --schedule-random --repeat until-fail:COUNT` for a
stress pass. A failure's working files are attributable at
`build/PRESET/server-test-runtimes/TEST-NAME`; CTest stdout/stderr remains in
`Testing/Temporary/LastTest.log`.

For a runtime idle/hang reproduction at Brynknot, the offline
`--provision_scenario` interface accepts `--provision_preset brynknot-idle`.
Use the wrapper's scenario workflow with an isolated state and protected password
file. This preset saves a normal player and savebed at
`/shattered_islands/world_0_70` `(20,8)` through `player_provision_scenario()`,
adds no extra inventory item, and preserves the world's current clock. It rejects
existing account/player destinations. `basic-player` retains its empty character
record and normal tutorial first-login behavior; lighting presets retain their
explicit clock and lamp setup. Provisioning opens no listener or live command
channel and must not be run against an active server's state.
