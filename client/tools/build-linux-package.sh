#!/usr/bin/env bash

set -euo pipefail

output_directory=${1:-build/packages}
version=${ATRINIK_PACKAGE_VERSION:-}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH" >&2
  exit 1
fi
if [[ $(uname -s) != Linux || $(dpkg --print-architecture) != amd64 ]]; then
  echo "Classic Debian packages require Linux amd64" >&2
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
discord_config_file=${ATRINIK_DISCORD_APPLICATION_ID_FILE:-}
if [[ -n ${discord_config_file} && ! -f ${discord_config_file} ]]; then
  echo "ATRINIK_DISCORD_APPLICATION_ID_FILE is not a regular file" >&2
  exit 1
fi
package=${output_directory}/atrinik-classic-client-${version}-linux-amd64.deb
if [[ -e ${package} ]]; then
  echo "Package output already exists: ${package}" >&2
  exit 1
fi

python3 tools/dependencies.py sync --cache "${ATRINIK_DEPENDENCY_DOWNLOADS}" --refresh --offline
python3 tools/dependencies.py verify
python3 tools/verify_gpu_fixture_provenance.py
mkdir -p "${output_directory}"

cmake -S . -B build/linux-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTING=OFF \
  -DPACKAGE_TYPE=deb \
  -DATRINIK_PACKAGE_VERSION="${version}" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  "-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=$(realpath ../protocol)" \
  "-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=$(realpath ../libatrinik)" \
  "-DATRINIK_GPU_SHADER_DIRECTORY=$(realpath "${ATRINIK_GPU_SHADER_DIRECTORY}")" \
  "-DATRINIK_DISCORD_APPLICATION_ID_FILE=${discord_config_file}"
cmake --build build/linux-release --parallel "$(nproc)"

production_executable=build/linux-release/atrinik
if [[ ! -f ${production_executable} ]]; then
  echo "Linux production executable is missing" >&2
  exit 1
fi
for test_marker in '--gpu-player-view' 'injected GPU conformance fault'; do
  if LC_ALL=C grep -aFq -- "${test_marker}" "${production_executable}"; then
    echo "Linux production executable contains test-only marker: ${test_marker}" >&2
    exit 1
  fi
done

cpack --config build/linux-release/CPackConfig.cmake -G DEB -B "${output_directory}"
if [[ ! -f ${package} ]]; then
  echo "Expected Linux client package is missing: ${package}" >&2
  exit 1
fi
