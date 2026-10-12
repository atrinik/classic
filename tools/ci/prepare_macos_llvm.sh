#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

# Official release asset SHA-256 values retrieved from llvm/llvm-project's
# llvmorg-23.1.3 release metadata. All writes stay in a new caller-owned directory.
if test "$(uname -s)" != Darwin; then
  echo 'LLVM macOS preparation requires a native macOS host' >&2
  exit 1
fi
if test "$#" -ne 1; then
  echo 'usage: prepare_macos_llvm.sh ABSENT_INSTALL_DIRECTORY' >&2
  exit 1
fi
root=$1
case "${root}" in /*) ;; *) echo 'install directory must be absolute' >&2; exit 1 ;; esac
mkdir "${root}"
stage="${root}/stage"
mkdir "${stage}"
base=https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.3
source_archive=llvm-project-23.1.3.src.tar.xz
source_digest=c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34
# The full compiler-rt license includes runtime exceptions and third-party terms.
# Extract notices from verified upstream source on both native architectures.
curl --fail --location --retry 3 "${base}/${source_archive}" --output "${stage}/${source_archive}"
printf '%s  %s\n' "${source_digest}" "${stage}/${source_archive}" | shasum -a 256 --check
case "$(uname -m)" in
  arm64)
    archive=LLVM-23.1.3-macOS-ARM64.tar.xz
    digest=3dcfd88aec10be204a24037bcb0dc0ad777882e9dbe8ac60e2639060d8a794b1
    ;;
  x86_64)
    archive=${source_archive}
    digest=${source_digest}
    ;;
  *) echo 'unsupported native macOS architecture' >&2; exit 1 ;;
esac
if test "${archive}" != "${source_archive}"; then
  curl --fail --location --retry 3 "${base}/${archive}" --output "${stage}/${archive}"
  printf '%s  %s\n' "${digest}" "${stage}/${archive}" | shasum -a 256 --check
fi
if test "$(uname -m)" = arm64; then
  mkdir "${root}/llvm"
  tar -xf "${stage}/${archive}" -C "${root}/llvm" --strip-components=1
else
  # Upstream publishes no Intel macOS binary for this release. Apple Clang is
  # only the bootstrap compiler; Classic uses the resulting LLVM 23.1.3 cohort.
  tar -xf "${stage}/${archive}" -C "${stage}"
  sdk=$(xcrun --sdk macosx --show-sdk-path)
  bootstrap_c=$(xcrun --sdk macosx --find clang)
  bootstrap_cxx=$(xcrun --sdk macosx --find clang++)
  # Keep compile concurrency separate from memory-heavy LLVM link jobs.
  jobs=${ATRINIK_MACOS_LLVM_BUILD_JOBS:-2}
  case "${jobs}" in ''|*[!0-9]*|0) echo 'LLVM build jobs must be a positive integer' >&2; exit 1 ;; esac
  cmake -S "${stage}/llvm-project-23.1.3.src/llvm" -B "${stage}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${root}/llvm" \
    -DCMAKE_C_COMPILER="${bootstrap_c}" -DCMAKE_CXX_COMPILER="${bootstrap_cxx}" \
    -DCMAKE_OSX_SYSROOT="${sdk}" \
    '-DLLVM_ENABLE_PROJECTS=clang;lld' -DLLVM_ENABLE_RUNTIMES=compiler-rt \
    -DLLVM_TARGETS_TO_BUILD=Native -DLLVM_PARALLEL_COMPILE_JOBS="${jobs}" \
    -DLLVM_PARALLEL_LINK_JOBS=1
  cmake --build "${stage}/build" --parallel "${jobs}"
  cmake --install "${stage}/build"
fi
mkdir -p "${root}/llvm/share/licenses/llvm"
for name in llvm clang lld compiler-rt; do
  tar -xOf "${stage}/${source_archive}" "llvm-project-23.1.3.src/${name}/LICENSE.TXT" \
    > "${root}/llvm/share/licenses/llvm/${name}-LICENSE.TXT"
  test -s "${root}/llvm/share/licenses/llvm/${name}-LICENSE.TXT"
done

# Validate every selected tool and compiler-rt, and compile/link/run native
# C/C++ probes through the same toolchain and platform SDK as Classic.
cmake -S "$(dirname "$0")/macos_llvm_probe" -B "${stage}/probe" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$(cd "$(dirname "$0")/../.." && pwd)/cmake/toolchains/llvm-macos.cmake" \
  -DCMAKE_PROGRAM_PATH="${root}/llvm/bin"
cmake --build "${stage}/probe"
"${stage}/probe/llvm-c-probe"
"${stage}/probe/llvm-cxx-probe"
