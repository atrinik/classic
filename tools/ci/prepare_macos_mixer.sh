#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

# Homebrew's mixer enables a different decoder set. Build the same locked
# source and decoder options used by the supported Linux/Windows producers.
if test "$(uname -s)" != Darwin; then
  echo 'Mixer preparation requires a native macOS host' >&2
  exit 1
fi
if test "$#" -ne 1; then
  echo 'usage: prepare_macos_mixer.sh ABSENT_INSTALL_DIRECTORY' >&2
  exit 1
fi
root=$1
case "${root}" in /*) ;; *) echo 'install directory must be absolute' >&2; exit 1 ;; esac
repository=$(cd "$(dirname "$0")/../.." && pwd)
mkdir "${root}"
stage="${root}/stage"
mkdir "${stage}"
python3 - "${repository}/tools/ci/appimage/packaging.lock.json" > "${stage}/source.txt" <<'PY'
import json
import sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
source, = (entry for entry in lock["sources"] if entry["name"] == "SDL3_mixer")
print(source["url"])
print(source["sha256"])
print(source["source_directory"])
PY
{ read -r url; read -r digest; read -r source; } < "${stage}/source.txt"
curl --fail --location --retry 3 "${url}" --output "${stage}/source.tar.gz"
printf '%s  %s\n' "${digest}" "${stage}/source.tar.gz" | shasum -a 256 --check
tar -xf "${stage}/source.tar.gz" -C "${stage}"
prefix=$(brew --prefix)
opusfile_prefix=$(brew --prefix opusfile)
opus_prefix=$(brew --prefix opus)
ogg_prefix=$(brew --prefix libogg)
PKG_CONFIG_PATH="${opusfile_prefix}/lib/pkgconfig:${opus_prefix}/lib/pkgconfig:${ogg_prefix}/lib/pkgconfig:${prefix}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export PKG_CONFIG_PATH
toolchain="${repository}/cmake/toolchains/llvm-macos.cmake"
cmake -S "${stage}/${source}" -B "${stage}/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${toolchain}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${root}" -DCMAKE_PREFIX_PATH="${prefix}" \
  -DBUILD_SHARED_LIBS=ON -DSDLMIXER_STRICT=ON \
  -DSDLMIXER_VENDORED=OFF -DSDLMIXER_DEPS_SHARED=OFF -DSDLMIXER_EXAMPLES=OFF \
  -DSDLMIXER_AIFF=ON -DSDLMIXER_WAVE=ON -DSDLMIXER_VOC=ON -DSDLMIXER_AU=ON \
  -DSDLMIXER_FLAC=OFF -DSDLMIXER_GME=OFF -DSDLMIXER_INSTALL=ON -DSDLMIXER_MIDI=OFF \
  -DSDLMIXER_MOD=OFF -DSDLMIXER_MP3=ON -DSDLMIXER_MP3_DRMP3=ON -DSDLMIXER_MP3_MPG123=OFF \
  -DSDLMIXER_OPUS=ON -DSDLMIXER_TESTS=OFF -DSDLMIXER_VORBIS_STB=ON \
  -DSDLMIXER_VORBIS_TREMOR=OFF -DSDLMIXER_VORBIS_VORBISFILE=OFF -DSDLMIXER_WAVPACK=OFF
cmake --build "${stage}/build" --parallel 2
cmake --install "${stage}/build"
cmake -S "${repository}/tools/ci/macos_mixer_probe" -B "${stage}/probe" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${toolchain}" -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=${root};${prefix}" "-DSDL3_mixer_DIR=${root}/lib/cmake/SDL3_mixer"
cmake --build "${stage}/probe" --parallel 2
cmake -DFIXTURE="${repository}/client/src/tests/fixtures/audio/opus-tone.mid" \
  -DCHECKSUM_FILE="${repository}/client/src/tests/fixtures/audio/opus-tone.sha256" \
  -P "${repository}/client/src/tests/fixtures/audio/verify.cmake"
"${stage}/probe/mixer-probe" "${repository}/client/src/tests/fixtures/audio/opus-tone.mid"
