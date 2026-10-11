# Native macOS LLVM toolchain

Classic uses upstream LLVM 23.1.3 on macOS: Clang, Mach-O LLD (`ld64.lld`),
LLVM archive/inspection tools, and compiler-rt builtins. Xcode supplies the
selected macOS SDK, system libc++ headers/runtime, frameworks and unwind runtime.
Apple Clang is only used to bootstrap the LLVM source installation on Intel;
it is rejected as a Classic compiler. Build-type optimization, application
dependencies, LTO and PGO remain unchanged.

Install CMake 3.21 or newer, Ninja, Python and the normal client dependencies,
and select the intended Xcode developer directory before preparing LLVM. The
self-hosted Metal runners need the same prerequisites. From the Classic root:

```sh
tools/ci/prepare_macos_llvm.sh /absolute/absent/task-llvm-directory
export PATH=/absolute/absent/task-llvm-directory/llvm/bin:$PATH
cmake -S client -B client/build/macos -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/llvm-macos.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DPACKAGE_TYPE=none
cmake --build client/build/macos
ctest --test-dir client/build/macos --output-on-failure
```

The install directory must be absent; preparation preserves failed stages for
inspection rather than replacing existing installations. The helper verifies
pinned SHA-256 values from the official
[LLVM 23.1.3 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.3)
metadata before extraction. Apple Silicon uses its official binary archive;
Intel builds Clang, LLD and compiler-rt from the pinned release source archive
because the release provides no Intel macOS binary. Intel bootstrap defaults to
two compile jobs and one link job; `ATRINIK_MACOS_LLVM_BUILD_JOBS` selects a
positive compile-job limit appropriate to the runner's available CPU/memory.
Preparation configures,
links and runs C and C++ probes through the same native toolchain. Every
selected compiler and utility must report exactly 23.1.3, and the compiler-rt
builtins archive must exist. Complete LLVM/runtime notices accompany the install.

Darwin builds automatically select this toolchain before `project()` when no
explicit toolchain was supplied. An explicit `CMAKE_OSX_SYSROOT`, deployment
target and architecture list are preserved. Without an SDK override, `xcrun`
selects the macOS SDK. The platform libc++ headers come from the selected Xcode
toolchain rather than the downloaded LLVM cohort. Mach-O LLD is selected by its
absolute path, following [LLD's native macOS interface](https://lld.llvm.org/MachO/index.html).

Linux CMake-script fixtures verify selection, version rejection, platform-header
selection and missing-runtime failures. They do not prove that a release archive
runs on macOS, that SDK probes compile, or that Metal works. Native acceptance
requires successful preparation/probes and the production client/GPU harness
build on each supported Mac architecture, followed by the unchanged Metal
reference and minimum hardware qualification rows in
`.github/workflows/gpu-qualification.yml`. These rows retain their GPU checks,
stress runs, lifecycle recovery, production fixtures and human-approved goldens.
