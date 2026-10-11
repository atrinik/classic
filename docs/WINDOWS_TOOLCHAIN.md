# Classic Windows toolchain

The Windows producer and native qualification cohort use LLVM-MinGW
`20261006` with LLVM `23.1.3`, UCRT, compiler-rt, LLD and the LLVM archive,
resource, import-library and inspection utilities. Target dependencies are
rebuilt in a fresh prefix with that cohort; do not reuse MXE/GCC object files,
static libraries or runtime directories. Image digests remain separately
qualified release inputs.

Cross-build consumers use plain CMake with these required producer variables:

```sh
export ATRINIK_WINDOWS_TOOLCHAIN_FILE=/opt/atrinik-windows/share/cmake/llvm-mingw.cmake
export ATRINIK_WINDOWS_TARGET=x86_64-w64-mingw32
export ATRINIK_WINDOWS_RUNTIME_DIR=/opt/atrinik-windows/bin
export ATRINIK_PACKAGE_VERSION=MAJOR.MINOR.PATCH
```

The producer exposes LLVM tools from `/opt/llvm-mingw/bin`. CMake, Ninja and
host Python remain host tools; pkg-config selects the freshly built Windows
dependency prefix. From the owning module directory, run
`tools/build-windows-package.sh OUTPUT_DIRECTORY`. Server packaging additionally
requires `ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR`,
`ATRINIK_WINDOWS_PYTHON_LIBRARY` and `ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR`, and
builds the host worldmaker separately before cross-compiling the server.
Dependency, shader, sound and source-profile qualification inputs keep their
existing verification contracts.

Packages stage the recursive DLL import closure with
`tools/ci/stage_windows_runtime.py --readobj llvm-readobj`. The inspector parses
LLVM COFF ordinary and delayed import blocks, requires x86-64 PE format, and
rejects unresolved imports, unsafe DLL names, duplicate runtime names and GCC
runtime DLLs. UCRT and Windows API-set DLLs are platform dependencies. The
installer owns the executable and server plugins; closure staging does not
replace those targets. The compatible external Python runtime retains its
own files and Microsoft runtime dependencies; every bundled Python DLL and
extension module is also inspected. Python API calls must retain the existing
allocation and CRT-resource ownership boundaries.

Both packages retain the producer's `windows-dependencies.json` and
`windows-pe-closure.json` below `licenses/`, every dependency notice listed by
that source inventory, and the complete LLVM-MinGW notices below
`licenses/llvm-mingw/`. Missing inventories, missing notices and unsafe notice
paths fail packaging. This includes compiler-rt and MinGW CRT notices even
when their code is linked statically.

Native Windows qualification requires the matching LLVM-MinGW cohort and its
fresh dependency prefix, selected explicitly by a native toolchain file. A
cross-build, hosted smoke or fixture parser test does not prove D3D12 hardware,
gameplay or audible behavior. Keep production packages separate from test-only
conformance builds and retain the workspace's Windows GPU preflight and cleanup
requirements.
