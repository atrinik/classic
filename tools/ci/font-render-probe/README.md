# Windows font dependency equivalence

This standalone C17 build compiles the same `client/src/tests/font_render.c`
source with the original pinned MXE producer and the LLVM candidate. It needs
only SDL3 and SDL3_ttf; it does not configure the game, shaders or compiler policy.
Pass each producer's existing toolchain file to CMake and build
`client-font-render-probe`. The integrated client exposes that target whenever
`BUILD_TESTING` is enabled, including MinGW.

Stage each executable and its own complete DLL closure in separate directories.
Use the same checked-out `client/fonts` directory and the same native Windows
runner for both invocations, with distinct empty output directories:

```text
baseline/client-font-render-probe.exe FONT_DIR BASELINE_OUTPUT_DIR
candidate/client-font-render-probe.exe FONT_DIR CANDIDATE_OUTPUT_DIR
python tools/ci/compare_font_render.py BASELINE_OUTPUT_DIR CANDIDATE_OUTPUT_DIR
```

The probe creates no window or GPU device and exclusively creates its outputs.
It fails on unavailable fonts, render/metric errors, existing output files or
exceeded bounds. Its 1,680 stable cases cover eight shipped fonts, 11/16/24 point,
normal/bold/italic/bold-italic/underline, solid/blended and seven UTF-8 samples
(`M`, `g`, `é`, `AV`, `ffi`, composed `Café`, decomposed `Café`). Nine glyph
records retain availability, bounds and advance. Each case also records text
dimensions, surface dimensions, height, ascent, descent and line skip.

Schema 1 contains ordered metadata in `manifest.json` and tightly packed
row-major RGBA8 `caseNNNN.rgba` files. RGB bytes for zero-alpha pixels are zero.
Surfaces are limited to 1,024 by 1,024, aggregate pixels to 128 MiB and metadata
to 2 MiB. The comparator requires the entire closed matrix and exact metadata
and pixel equality, with no tolerance or invented baseline metrics. Probe
outputs, producer/source identities and staged dependency inventories should
be retained as CI evidence, including on failure. Python fixture tests check
the comparator contract; they do not establish actual rendering equivalence.
