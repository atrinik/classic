#!/usr/bin/env bash

set -euo pipefail

output_directory=${1:-build/packages}
version=${ATRINIK_PACKAGE_VERSION:-}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH" >&2
  exit 1
fi
for variable in MXE_TOOLCHAIN_FILE MXE_RUNTIME_DIR; do
  if [[ -z ${!variable:-} || ! -e ${!variable} ]]; then
    echo "${variable} does not identify an installed MXE dependency" >&2
    exit 1
  fi
done

mxe_cmake=${MXE_TARGET:-x86_64-w64-mingw32.shared}-cmake
command -v "${mxe_cmake}" >/dev/null
python3 tools/dependencies.py sync
python3 tools/dependencies.py verify
mkdir -p "${output_directory}"

dependency_arguments=()
repository_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n ${repository_root} && -f ${repository_root}/protocol/CMakeLists.txt &&
    -f ${repository_root}/libatrinik/CMakeLists.txt ]]; then
  dependency_arguments+=(
    "-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=${repository_root}/protocol"
    "-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=${repository_root}/libatrinik"
  )
fi

"${mxe_cmake}" -S . -B build/windows-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${MXE_TOOLCHAIN_FILE}" \
  -DBUILD_TESTING=OFF \
  -DPACKAGE_TYPE=zip \
  -DPACKAGE_VERSION="${version}" \
  -DATRINIK_WINDOWS_RUNTIME_DIR="${MXE_RUNTIME_DIR}" \
  -DATRINIK_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
  "${dependency_arguments[@]}"
cmake --build build/windows-release --parallel "$(nproc)"
cpack --config build/windows-release/CPackConfig.cmake \
  -G ZIP -B "${output_directory}"

mapfile -t packages < <(find "${output_directory}" -maxdepth 1 \
  -type f -name 'atrinik-classic-client-*-windows-x86_64.zip' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
  echo "Expected exactly one Windows client package, found ${#packages[@]}" >&2
  exit 1
fi
