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
prerequisites: SDL3, SDL3_image, SDL3_ttf, Opusfile, OpenSSL 3, libxml2,
libidn2, curl, zlib, Readline and Python 3.12. Python supplies the protocol,
client and validation tooling interpreter. Readline retains the toolkit's
interactive console feature. Flex, MiniUPnPc, libgd, Check and libpcpnatpmp
belong to the server build and are not acquired by this Mac lane.
The official [SDL3](https://formulae.brew.sh/formula/sdl3) and
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

`tools/ci/macos-curl-toolchain.json` records the authenticated ARM64 baseline
from native run `38122697026` at Classic
`8048191fc62554be68647463a18fc5b956d06f8d`: curl 8.22.0, 20 features and
25 protocols, OpenSSL 3.6.4 and Apple SecTrust, system zlib and Apple IDN,
native Kerberos/LDAP, HTTP/2 and HTTP/3, SSH2, compression and suffix policy.
The measured Homebrew resolver is threaded and does not meet Classic's
cancellation contract. The native helper replaces it with pinned c-ares 1.34.6
while retaining every measured feature/protocol and TLS policy:

```sh
python3 tools/ci/prepare_macos_curl.py \
  --prefix /absolute/absent/task-curl-directory \
  --llvm-prefix /absolute/task-llvm-directory/llvm \
  --baseline /absolute/curl-baseline/baseline.json \
  --ca-bundle "$PWD/client/ca-bundle.crt" --jobs 2
```

The helper is native ARM64 only; its absent prefix preserves interrupted
stages. Verified release archives and full notices accompany both shared
libraries. c-ares uses the existing native CMake toolchain; curl uses explicit
arguments translated from the measured formula's configure policy, preserving
Kerberos/LDAP and SDK zlib discovery. Both use LLVM 23.1.3, compiler-rt and
Mach-O LLD with native Release `-O3 -DNDEBUG`, without LTO/PGO or Linux unwind
flags. Generated resolver configuration must select c-ares and exclude the
threaded/hybrid backend. Unknown baseline changes or missing runtime
capabilities fail with named evidence rather than silently reducing features.

Select this prefix first in CMake and pkg-config, with
`CURL_NO_CURL_CMAKE=TRUE`, `CURL_ROOT`, explicit `CURL_INCLUDE_DIR` and
`CURL_LIBRARY_RELEASE` pointing to its headers and dylib. After configuring
Classic, verify selection before building:

```sh
python3 tools/ci/prepare_macos_curl.py \
  --prefix /absolute/task-curl-directory \
  --verify-consumer-cache build/macos-qualification/CMakeCache.txt
```

The helper binds selected headers and loaded curl/c-ares images to the owned
prefix, captures native dependency keg paths/hashes, and records system image
names plus SDK/OS identity. Many system dylibs reside only in the dyld shared
cache and cannot be hashed as filesystem files. Both baseline and candidate
probes record loaded images and bounded Mach-O dependency commands from memory.
The schema-2 baseline is bound to the same source, repository, workflow run,
attempt and job as the candidate. Each directly linked curl API and
`ares_version` must belong to the selected provider. The only permitted extra
curl image is the exact OS install name `/usr/lib/libcurl.4.dylib`, when both
snapshots prove the same loaded OS dependency chain from the recorded platform
roots. Other extra curl or c-ares providers fail qualification. An unresolved
weak dependency or `@rpath` name cannot establish that chain.

The helper requires ordinary public
HTTPS with default trust and the locked Classic bundle, rejects that endpoint
with a fresh unrelated CA, and checks a private loopback TLS chain with trusted,
untrusted and wrong-hostname cases. Peer and hostname checks stay enabled;
private CA tests do not opt back into native CA trust. SecTrust and OpenSSL
fallback configuration are retained, while public success alone cannot prove
which trust engine executed.

Upload only the helper's `evidence/` directory and the existing application
reports. Private fixture keys remain in `stage/private-tls/`, outside uploaded
evidence. The owned absolute dylib install names describe a CI-local dependency
cohort; they do not qualify a relocatable Mac package. Helper probes and cache
selection do not qualify Classic cancellation or the application: the unchanged
`libatrinik-curl` stalled-DNS tests and the supported Release/CPU suites must
also pass with this cohort. Intel, packaged Mac and GPU acceptance stay separate.

`tools/ci/prepare_macos_mixer.sh` builds the SHA-verified SDL3_mixer source
from the existing packaging lock with native LLVM and the same decoder options
as the Linux and Windows producers. Homebrew's mixer enables additional
decoders and does not satisfy Classic's exact supported set: WAV, STBVORBIS,
OPUS, VOC, AIFF, AU, DRMP3, SINEWAVE and RAW. The preparation probe checks that
set, decodes the verified Opus fixture at its legacy `.mid` path into nonzero
PCM and starts playback on SDL's dummy audio device. The application selects
that qualified prefix explicitly. These checks retain sound features and
headless test coverage; audible output still requires separate native proof.

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
