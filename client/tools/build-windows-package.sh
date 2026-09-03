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
dependency_downloads=${ATRINIK_DEPENDENCY_DOWNLOADS:-}
dependency_sync_arguments=()
profile_sound=${ATRINIK_PROFILE_SOUND_DIR:-}
if [[ -n ${profile_sound} ]]; then
  if [[ ! -d ${profile_sound} || -L ${profile_sound} ]]; then
    echo "ATRINIK_PROFILE_SOUND_DIR does not identify a regular directory" >&2
    exit 1
  fi
  expected_sound=$(realpath -e sound)
  selected_sound=$(realpath -e "${profile_sound}")
  if [[ ${selected_sound} != "${expected_sound}" ]]; then
    echo "ATRINIK_PROFILE_SOUND_DIR must identify the staged client sound directory" >&2
    exit 1
  fi
  mapfile -t invalid_sound_entries < <(
    find -P "${profile_sound}" ! -type d ! -type f -print -quit
  )
  mapfile -t profile_sound_files < <(
    find -P "${profile_sound}" -type f -print -quit
  )
  if [[ ${#invalid_sound_entries[@]} -ne 0 || ${#profile_sound_files[@]} -eq 0 ]]; then
    echo "ATRINIK_PROFILE_SOUND_DIR contains an invalid or empty tree" >&2
    exit 1
  fi
else
  if [[ -n ${dependency_downloads} ]]; then
    if [[ ! -d ${dependency_downloads} ]]; then
      echo "ATRINIK_DEPENDENCY_DOWNLOADS does not identify a staged archive bundle" >&2
      exit 1
    fi
    dependency_sync_arguments+=(--cache "${dependency_downloads}" --refresh --offline)
  fi
  python3 tools/dependencies.py sync "${dependency_sync_arguments[@]}"
  python3 tools/dependencies.py verify
fi
python3 tools/verify_gpu_fixture_provenance.py
mkdir -p "${output_directory}"

dependency_arguments=()
shader_directory=${ATRINIK_GPU_SHADER_DIRECTORY:-}
if [[ -n ${shader_directory} ]]; then
  if [[ ! -d ${shader_directory} || -L ${shader_directory} ]]; then
    echo "ATRINIK_GPU_SHADER_DIRECTORY does not identify a regular directory" >&2
    exit 1
  fi
  dependency_arguments+=("-DATRINIK_GPU_SHADER_DIRECTORY=${shader_directory}")
fi
discord_config_file=${ATRINIK_DISCORD_APPLICATION_ID_FILE:-}
if [[ -n ${ATRINIK_DISCORD_APPLICATION_ID_FILE:-} ]]; then
  if [[ ! -f ${ATRINIK_DISCORD_APPLICATION_ID_FILE} ]]; then
    echo "ATRINIK_DISCORD_APPLICATION_ID_FILE is not a regular file" >&2
    exit 1
  fi
fi
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
  -DATRINIK_PACKAGE_VERSION="${version}" \
  -DATRINIK_WINDOWS_RUNTIME_DIR="${MXE_RUNTIME_DIR}" \
  -DATRINIK_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
  "-DATRINIK_DISCORD_APPLICATION_ID_FILE=${discord_config_file}" \
  "${dependency_arguments[@]}"
cmake --build build/windows-release --parallel "$(nproc)"

mapfile -t production_executables < <(
  find build/windows-release -maxdepth 3 -type f -name atrinik.exe -print
)
if [[ ${#production_executables[@]} -ne 1 ]]; then
  echo "Expected exactly one Windows production executable, found ${#production_executables[@]}" >&2
  exit 1
fi
for test_marker in '--gpu-player-view' 'injected GPU conformance fault'; do
  if LC_ALL=C grep -aFq -- "${test_marker}" "${production_executables[0]}"; then
    echo "Windows production executable contains test-only marker: ${test_marker}" >&2
    exit 1
  fi
done

cpack --config build/windows-release/CPackConfig.cmake \
  -G ZIP -B "${output_directory}"

mapfile -t packages < <(find "${output_directory}" -maxdepth 1 \
  -type f -name 'atrinik-classic-client-*-windows-x86_64.zip' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
  echo "Expected exactly one Windows client package, found ${#packages[@]}" >&2
  exit 1
fi
