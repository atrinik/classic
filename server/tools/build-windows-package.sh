#!/usr/bin/env bash

set -euo pipefail

output_directory=${1:-build/packages}
version=${ATRINIK_PACKAGE_VERSION:-}
if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ATRINIK_PACKAGE_VERSION must be MAJOR.MINOR.PATCH" >&2
  exit 1
fi
for variable in MXE_TOOLCHAIN_FILE MXE_RUNTIME_DIR \
    ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR ATRINIK_WINDOWS_PYTHON_LIBRARY \
    ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR; do
  if [[ -z ${!variable:-} || ! -e ${!variable} ]]; then
    echo "${variable} does not identify an installed build dependency" >&2
    exit 1
  fi
done

mxe_cmake=${MXE_TARGET:-x86_64-w64-mingw32.shared}-cmake
command -v "${mxe_cmake}" >/dev/null
python3 tools/dependencies.py sync
python3 tools/dependencies.py verify

dependency_arguments=()
native_compiler_arguments=()
monorepo_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n ${monorepo_root} && -f ${monorepo_root}/protocol/CMakeLists.txt &&
    -f ${monorepo_root}/libatrinik/CMakeLists.txt ]]; then
  dependency_arguments+=(
    "-DFETCHCONTENT_SOURCE_DIR_ATRINIK_PROTOCOL=${monorepo_root}/protocol"
    "-DFETCHCONTENT_SOURCE_DIR_LIBATRINIK=${monorepo_root}/libatrinik"
  )
fi
if command -v ccache >/dev/null; then
  native_compiler_arguments+=(
    "-DCMAKE_C_COMPILER_LAUNCHER=$(command -v ccache)"
  )
fi

package_root=build/windows-package-root
region_build=build/windows-region-generator
region_runtime=${package_root}/region-runtime
region_data=${package_root}/region-data
cmake -E remove_directory "${package_root}"
cmake -E remove_directory build/windows-release
cmake -E make_directory "${package_root}"
cmake -E copy_directory runtime/content/maps "${package_root}/maps"
cmake -E copy_directory runtime/content/lib "${package_root}/lib"

cmake -S . -B "${region_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DPACKAGE_TYPE=none \
  -DPACKAGE_VERSION="${version}" \
  "${native_compiler_arguments[@]}" \
  "${dependency_arguments[@]}"
cmake --build "${region_build}" \
  --target atrinik-server plugin_arena plugin_python --parallel "$(nproc)"

region_assets=${package_root}/region-assets
cmake -E make_directory "${region_runtime}" "${region_assets}" \
  "${region_data}/tmp"
cmake -E copy "${region_build}/atrinik-server" \
  "${region_build}/libplugin_arena.so" \
  "${region_build}/libplugin_python.so" server.cfg permissions.cfg \
  "${region_runtime}"
cmake -E copy_directory install_data "${region_data}"
repository_root=$(pwd)
(
  cd "${region_runtime}"
  ./atrinik-server \
    --worldmaker \
    --libpath="${repository_root}/${package_root}/lib" \
    --mapspath="${repository_root}/${package_root}/maps" \
    --datapath="${repository_root}/${region_data}" \
    --assetspath="${repository_root}/${region_assets}" \
    --resourcespath="${repository_root}/resources"
)
test -d "${region_assets}/client-maps"
cmake -E copy_directory "${region_assets}/client-maps" \
  "${package_root}/client-maps"
cmake -E remove_directory "${region_runtime}"
cmake -E remove_directory "${region_data}"
cmake -E remove_directory "${region_assets}"

"${mxe_cmake}" -S . -B build/windows-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${MXE_TOOLCHAIN_FILE}" \
  -DBUILD_TESTING=OFF \
  -DPACKAGE_TYPE=zip \
  -DPACKAGE_VERSION="${version}" \
  -DATRINIK_WINDOWS_RUNTIME_DIR="${MXE_RUNTIME_DIR}" \
  -DATRINIK_WINDOWS_PYTHON_INCLUDE_DIR="${ATRINIK_WINDOWS_PYTHON_INCLUDE_DIR}" \
  -DATRINIK_WINDOWS_PYTHON_LIBRARY="${ATRINIK_WINDOWS_PYTHON_LIBRARY}" \
  -DATRINIK_WINDOWS_PYTHON_RUNTIME_DIR="${ATRINIK_WINDOWS_PYTHON_RUNTIME_DIR}" \
  -DATRINIK_SERVER_PACKAGE_ROOT="${repository_root}/${package_root}" \
  "${dependency_arguments[@]}"
cmake --build build/windows-release --parallel "$(nproc)"
mkdir -p "${output_directory}"
cpack --config build/windows-release/CPackConfig.cmake \
  -G ZIP -B "${output_directory}"

mapfile -t packages < <(find "${output_directory}" -maxdepth 1 \
  -type f -name 'atrinik-classic-server-*-windows-x86_64.zip' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
  echo "Expected exactly one Windows server package, found ${#packages[@]}" >&2
  exit 1
fi
