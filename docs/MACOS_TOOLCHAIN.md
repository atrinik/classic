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
selects the macOS SDK. The platform libc++ headers come first from
`usr/include/c++/v1` in the selected SDK, with the selected Apple Clang
toolchain's `include/c++/v1` as a
fallback for older Xcode layouts. Named SDKs resolve through `xcrun` without
overwriting the caller's SDK selection. The downloaded LLVM cohort's libc++
headers are not used with the system libc++ runtime. Mach-O LLD is selected by its
absolute path, following [LLD's native macOS interface](https://lld.llvm.org/MachO/index.html).
The caller's LLVM `CMAKE_PROGRAM_PATH` is propagated into CMake's nested compiler
checks, so those checks select the same pinned LLVM utilities.

Linux CMake-script fixtures verify selection, version rejection, platform-header
selection and missing-runtime failures. They do not prove that a release archive
runs on macOS, that SDK probes compile, or that Metal works. Native acceptance
requires successful preparation/probes and the production client/GPU harness
build on each supported Mac architecture, followed by the unchanged Metal
reference and minimum hardware qualification rows in
`.github/workflows/gpu-qualification.yml`. These rows retain their GPU checks,
stress runs, lifecycle recovery, production fixtures and human-approved goldens.

## Hosted native CPU qualification

`.github/workflows/llvm-macos-qualification.yml` supplies a bounded manual and
reusable CPU qualification lane. Its Linux job produces the existing locked
shader cohort for the exact selected source SHA; its native `macos-15` job
checks Darwin/ARM64 before preparing the pinned LLVM archive and running the
C/C++ SDK probes. GitHub currently documents this standard runner as an
[ARM64 M1 host with three CPUs and 7 GiB RAM](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The [runner inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md)
provides CMake, Ninja, Homebrew and Xcode; the lane records the actual image,
OS, selected SDK and disk space instead of inferring them from the label.

The native job installs ARM64 Homebrew bottles for the current application
prerequisites: SDL3, SDL3_image, SDL3_ttf, SDL3_mixer, OpenSSL 3, libxml2,
libidn2, curl, zlib, Readline and Python 3.12. Python supplies the protocol,
client and validation tooling interpreter. Readline retains the toolkit's
interactive console feature. Flex, MiniUPnPc, libgd, Check and libpcpnatpmp
belong to the server build and are not acquired by this Mac lane.
The official [SDL3](https://formulae.brew.sh/formula/sdl3),
[SDL3_mixer](https://formulae.brew.sh/formula/sdl3_mixer) and
[OpenSSL 3](https://formulae.brew.sh/formula/openssl@3) formulae currently supply
bottles meeting Classic's required versions. CMake remains the authority for
required versions at execution. Homebrew dependencies and hosted SDK images
can change; the exact installed versions and formula metadata are retained
with each qualification run. This lane does not create a released dependency
producer or qualify different dependency versions by inference.

Before configuring Classic, `tools/ci/collect_macos_curl.py` compiles a small
native probe with the same LLVM toolchain against the explicitly selected
Homebrew curl headers and library. It records the runtime provider via
`dladdr`, header/runtime versions, features, protocols, SSL and resolver identity,
default CA paths, curl configuration, Mach-O dependencies and retained bottle
formula/receipt. Normal HTTPS requests to `https://curl.se/` must pass both
with the provider's default trust and the locked Classic CA bundle. Peer and
hostname verification stay enabled; the probe uses no proxies, credentials or
redirects. A provider/header mismatch or failed verified request fails the lane.
The absent output directory preserves previous runs rather than replacing them.

This evidence is a prerequisite for selecting a source-built Mac curl/c-ares
cohort. It does not qualify Homebrew's threaded resolver for Classic cancellation,
prove Apple SecTrust solely from the SSL version, or establish source-built
dependency acceptance. The exact feature, protocol and trust baseline must be
preserved when the resolver provider changes. Linux and Windows curl policies
do not establish the native Mac baseline.

The integrated root selects `ATRINIK_BUILD_CLIENT=ON` and
`ATRINIK_BUILD_SERVER=OFF`, building the supported macOS protocol, libatrinik,
pathfinding, client and test targets through LLVM 23.1.3. The server access
backend deliberately supports Linux and Windows only; this lane preserves that
restriction. Server, worldmaker and server Python-plugin acceptance remains
required in the Linux and Windows lanes. Existing client/library feature,
warning and optimization defaults remain in use, including Release optimization
and warnings as errors. Builds and tests use two jobs. Locked sound, content and
resources use their existing verification helpers. CTest excludes the
`gpu-conformance` and `performance` labels; native CPU discovery must include
protocol, library, pathfinding and client tests and reject server tests before
tests run. GPU test executables
still compile. The source-only GPU fixture provenance helper runs explicitly alongside
locked runtime acquisition, while hardware GPU tests, benchmarks and Metal
reference/minimum qualification remain separate acceptance requirements.

Each run retains source identity, SDK/host and dependency inventory, native
probe/configure/build logs, CMake cache and compile commands, selected test
inventory, JUnit results and failure logs in the
`macos-llvm-cpu-qualification-SHA` artifact. Downloaded LLVM/source archives are
removed only after verified preparation and successful probes to recover the
runner's limited disk space. Missing prerequisites, failed probes, absent test
groups, build failures and test failures fail the lane. A source fixture pass
or workflow definition alone is not native acceptance. Intel macOS remains a
separate qualification target.

A new manual workflow must exist on the repository's default branch before
GitHub accepts a direct `workflow_dispatch`, as documented in
[manual workflow execution](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).
Before that, an existing manual workflow may call this local reusable workflow
from the selected source branch. The coordinator owns publication, caller
integration and authorized dispatch; this lane has only read permissions and
publishes no application packages.
