#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
umask 022

output_directory=${1:-${OUTPUT_DIRECTORY:-build/packages}}
version=${ATRINIK_PACKAGE_VERSION:-}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo 'ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH' >&2
  exit 1
fi
build_parallelism=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
if [[ ! ${build_parallelism} =~ ^[1-4]$ ]]; then
  echo 'CMAKE_BUILD_PARALLEL_LEVEL must be an integer from 1 to 4' >&2
  exit 1
fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo 'Classic AppImages require Linux x86_64' >&2
  exit 1
fi
for variable in ATRINIK_DEPENDENCY_DOWNLOADS ATRINIK_GPU_SHADER_DIRECTORY; do
  if [[ -z ${!variable:-} || ! -d ${!variable} || -L ${!variable} ]]; then
    echo "${variable} does not identify a regular staged directory" >&2
    exit 1
  fi
done
for source in ../protocol ../libatrinik; do
  if [[ ! -f ${source}/CMakeLists.txt ]]; then
    echo "Required sibling source is missing: ${source}" >&2
    exit 1
  fi
done
prefix=${ATRINIK_APPIMAGE_PREFIX:-/opt/atrinik-appimage}
[[ -f ${prefix}/packaging.lock.json && -x ${prefix}/tools/appimagetool ]]
revision=${ATRINIK_SOURCE_REVISION:-$(git rev-parse HEAD)}
[[ ${revision} =~ ^[0-9a-f]{40}$ ]]
discord_config_file=${ATRINIK_DISCORD_APPLICATION_ID_FILE:-}
if [[ -n ${discord_config_file} ]]; then
  discord_config_file=$(realpath -e "${discord_config_file}")
  [[ -f ${discord_config_file} && ! -L ${discord_config_file} ]]
fi
mkdir -p "${output_directory}"
output_directory=$(realpath "${output_directory}")
package=${output_directory}/atrinik-classic-client-${version}-linux-x86_64.AppImage
[[ ! -e ${package} && ! -L ${package} ]]
staging_directory=$(mktemp -d "${output_directory}/.atrinik-appimage.XXXXXX")
trap 'rm -rf -- "${staging_directory}"' EXIT
export CMAKE_PREFIX_PATH=${prefix}
export PKG_CONFIG_PATH=${prefix}/lib/pkgconfig
export LD_LIBRARY_PATH=${prefix}/lib
python3 tools/dependencies.py sync --cache "${ATRINIK_DEPENDENCY_DOWNLOADS}" --refresh --offline
python3 tools/dependencies.py verify
python3 tools/verify_gpu_fixture_provenance.py
build_directory=${ATRINIK_APPIMAGE_BUILD_DIRECTORY:-build/appimage-release}
cmake -S . -B "${build_directory}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF -DPACKAGE_TYPE=appimage \
  -DATRINIK_PACKAGE_VERSION="${version}" -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  "-DCMAKE_C_FLAGS=-ffile-prefix-map=$(realpath ..)=. -fdebug-prefix-map=$(realpath ..)=." \
  "-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=$(realpath ../protocol)" \
  "-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=$(realpath ../libatrinik)" \
  "-DATRINIK_GPU_SHADER_DIRECTORY=$(realpath "${ATRINIK_GPU_SHADER_DIRECTORY}")" \
  "-DATRINIK_DISCORD_APPLICATION_ID_FILE=${discord_config_file}"
cmake --build "${build_directory}" --parallel "${build_parallelism}"
appdir=${staging_directory}/Atrinik.AppDir
DESTDIR="${appdir}" cmake --install "${build_directory}" --component AtrinikClient
# The pinned deployment tool handles desktop integration; the explicit closure
# step also includes libraries excluded by its generic desktop policy and dlopen.
export NO_STRIP=1
"${prefix}/tools/linuxdeploy" --appdir "${appdir}" \
  --executable "${appdir}/usr/bin/atrinik" \
  --desktop-file "${appdir}/usr/share/applications/atrinik.desktop" \
  --icon-file "${appdir}/usr/share/pixmaps/atrinik.png"
python3 ../tools/ci/appimage/assemble.py "${appdir}" "${prefix}" "${version}" "${revision}"
# Always use the locked, already-verified runtime: appimagetool must not fetch it.
export ARCH=x86_64
env -u VERSION "${prefix}/tools/appimagetool" --mksquashfs-opt -no-xattrs \
  --mksquashfs-opt -processors --mksquashfs-opt "${build_parallelism}" --runtime-file "${prefix}/tools/runtime" \
  "${appdir}" "${staging_directory}/package.AppImage"
python3 ../tools/release/appimage.py "${staging_directory}/package.AppImage" "${version}"
chmod 0755 "${staging_directory}/package.AppImage"
mv -n -- "${staging_directory}/package.AppImage" "${package}"
[[ ! -e ${staging_directory}/package.AppImage ]]
