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

GCC coverage keeps its normal object-adjacent `.gcda` files so gcovr needs no
relocation or lossy post-processing. GCC locks concurrent profile-file merges
on supported filesystems; coverage builds additionally use
`-fprofile-update=atomic`. Validation compares sequential and parallel test
counts and gcovr totals and stresses randomized four-way scheduling before the
parallel result is accepted.

Use `ctest --preset PRESET --schedule-random --repeat until-fail:COUNT` for a
stress pass. A failure's working files are attributable at
`build/PRESET/server-test-runtimes/TEST-NAME`; CTest stdout/stderr remains in
`Testing/Temporary/LastTest.log`.
