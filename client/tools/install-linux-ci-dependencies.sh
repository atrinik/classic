#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 INSTALL_PREFIX" >&2
  exit 2
fi

prefix=$(realpath -m "$1")
apt_prefix=()
if ((EUID != 0)); then
  apt_prefix=(sudo)
fi
"${apt_prefix[@]}" apt-get update -qq
"${apt_prefix[@]}" env DEBIAN_FRONTEND=noninteractive \
  apt-get install -y -qq --no-install-recommends \
  build-essential ca-certificates cmake curl libcurl4-openssl-dev libsdl3-dev \
  libsdl3-image-dev libsdl3-ttf-dev libssl-dev libxml2-dev ninja-build \
  pkgconf python3 zlib1g-dev

export CMAKE_PREFIX_PATH=${prefix}
export LD_LIBRARY_PATH=${prefix}/lib
export PKG_CONFIG_PATH=${prefix}/lib/pkgconfig

if [[ -f ${prefix}/.atrinik-sdl-mixer-1 ]]; then
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf -- "${work}"' EXIT

fetch() {
  local name=$1
  local url=$2
  local sha256=$3
  curl --fail --location --silent --show-error "${url}" \
    --output "${work}/${name}.tar.gz"
  echo "${sha256}  ${work}/${name}.tar.gz" | sha256sum -c -
  tar -xzf "${work}/${name}.tar.gz" -C "${work}"
}

build_project() {
  local source=$1
  shift
  cmake -S "${work}/${source}" -B "${work}/${source}-build" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    "$@"
  cmake --build "${work}/${source}-build" --parallel
  cmake --install "${work}/${source}-build"
}

fetch SDL3_mixer-3.2.4 \
  https://github.com/libsdl-org/SDL_mixer/releases/download/release-3.2.4/SDL3_mixer-3.2.4.tar.gz \
  182a07c745375e113dc740d43964ff21b0be29f29f59876c4dbc4db3d32f6901
build_project SDL3_mixer-3.2.4 \
  -DSDLMIXER_DEPS_SHARED=OFF \
  -DSDLMIXER_EXAMPLES=OFF \
  -DSDLMIXER_FLAC=OFF \
  -DSDLMIXER_GME=OFF \
  -DSDLMIXER_INSTALL=ON \
  -DSDLMIXER_MIDI=OFF \
  -DSDLMIXER_MOD=OFF \
  -DSDLMIXER_MP3=ON \
  -DSDLMIXER_MP3_DRMP3=ON \
  -DSDLMIXER_MP3_MPG123=OFF \
  -DSDLMIXER_OPUS=OFF \
  -DSDLMIXER_TESTS=OFF \
  -DSDLMIXER_VORBIS_STB=ON \
  -DSDLMIXER_VORBIS_VORBISFILE=OFF \
  -DSDLMIXER_WAVPACK=OFF

pkg-config --atleast-version=3.4.0 sdl3
pkg-config --atleast-version=3.2.0 sdl3-image
pkg-config --atleast-version=3.2.0 sdl3-ttf
pkg-config --exact-version=3.2.4 sdl3-mixer
touch "${prefix}/.atrinik-sdl-mixer-1"
