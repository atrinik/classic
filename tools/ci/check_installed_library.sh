#!/usr/bin/env bash
# Copyright 2026 The Atrinik Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: $0 IMAGE ARCHIVE VERSION REVISION NEW_OUTPUT_DIRECTORY" >&2
  exit 2
fi
image=$1
archive=$(realpath "$2")
version=$3
revision=$4
output=$(realpath -m "$5")
if [[ ! ${image} =~ ^ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ||
      ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ||
      ! ${revision} =~ ^[0-9a-f]{40}$ || ! -f ${archive} ]]; then
  echo "invalid installed-library input coordinates" >&2
  exit 2
fi
# Never reuse or overwrite another invocation's outputs.
mkdir -m 700 "${output}"

docker run --rm --pull never -i \
  --network none \
  --user "$(id -u):$(id -g)" \
  --cap-drop ALL \
  --security-opt no-new-privileges \
  --read-only \
  --tmpfs /tmp:rw,nosuid,nodev \
  --mount "type=bind,source=${archive},target=/input/library.tar.gz,readonly" \
  --mount "type=bind,source=${output},target=/output" \
  --workdir /output \
  "${image}" bash -s -- "${version}" "${revision}" <<'CONSUMER'
set -euo pipefail
version=$1
revision=$2
mkdir extracted
tar -xzf /input/library.tar.gz -C extracted
library_source="${PWD}/extracted/atrinik-classic-libatrinik-${version}"
# Check generated archive identity before configuring its native sources.
for metadata_root in "${library_source}" "${library_source}/dependencies/protocol"; do
  test "$(cat "${metadata_root}/VERSION")" = "${version}"
  test "$(cat "${metadata_root}/SOURCE_REVISION")" = "${revision}"
done
# Inspect the imported target after find_package in each root configure. This
# checks the actual resolved provider, rather than requesting a linker override.
cat > provider-guard.cmake <<'CMAKE'
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  function(atrinik_check_installed_curl_provider)
    if(NOT TARGET CURL::libcurl)
      message(FATAL_ERROR "unqualified CURL::libcurl provider: missing imported target")
    endif()
    get_target_property(configurations CURL::libcurl IMPORTED_CONFIGURATIONS)
    set(properties IMPORTED_LOCATION)
    foreach(configuration IN LISTS configurations)
      string(TOUPPER "${configuration}" configuration)
      list(APPEND properties "IMPORTED_LOCATION_${configuration}")
    endforeach()
    set(locations)
    foreach(property IN LISTS properties)
      get_target_property(location CURL::libcurl "${property}")
      if(location AND NOT location MATCHES "-NOTFOUND$")
        file(REAL_PATH "${location}" resolved)
        if(NOT EXISTS "${resolved}" OR NOT resolved MATCHES "^/usr/local/lib(64)?/libcurl\\.(so|a)($|\\.)")
          message(FATAL_ERROR "unqualified CURL::libcurl provider: ${property}=${resolved}")
        endif()
        list(APPEND locations "${resolved}")
      endif()
    endforeach()
    if(NOT locations)
      message(FATAL_ERROR "unqualified CURL::libcurl provider: missing imported location")
    endif()
    get_target_property(includes CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
    set(headers)
    foreach(include IN LISTS includes)
      if(EXISTS "${include}/curl/curl.h")
        file(REAL_PATH "${include}/curl/curl.h" header)
        if(NOT header STREQUAL "/usr/local/include/curl/curl.h")
          message(FATAL_ERROR "unqualified CURL::libcurl provider: header=${header}")
        endif()
        list(APPEND headers "${header}")
      endif()
    endforeach()
    if(NOT headers)
      message(FATAL_ERROR "unqualified CURL::libcurl provider: missing qualified headers")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/curl-provider.txt"
      "target=CURL::libcurl\nlocations=${locations}\nheaders=${headers}\n")
  endfunction()
  cmake_language(DEFER CALL atrinik_check_installed_curl_provider)
endif()
CMAKE
provider_guard="${PWD}/provider-guard.cmake"
cmake -S "${library_source}" -B library \
  -DCMAKE_PROJECT_INCLUDE="${provider_guard}" \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PWD}/install"
cmake --build library --parallel "$(nproc)"
cmake --install library
cmake -S "${library_source}/tests/consumer" -B consumer \
  -DCMAKE_PROJECT_INCLUDE="${provider_guard}" \
  -DATRINIK_CONSUMER_VERSION="${version}" \
  -DCMAKE_PREFIX_PATH="${PWD}/install"
cmake --build consumer --parallel "$(nproc)"
consumer/libatrinik-consumer
# The qualified image also carries Ubuntu curl. Force that discoverable provider
# in fresh SDK and downstream configurations and require our exact guard failure.
system_curl=/usr/lib/x86_64-linux-gnu/libcurl.so
system_include=/usr/include/x86_64-linux-gnu
test -f "${system_curl}"
test -f "${system_include}/curl/curl.h"
for scope in library consumer; do
  if [[ ${scope} == library ]]; then
    negative_source=${library_source}
  else
    negative_source=${library_source}/tests/consumer
  fi
  if cmake -S "${negative_source}" -B "${scope}-system-curl" \
    -DCMAKE_PROJECT_INCLUDE="${provider_guard}" \
    -DBUILD_TESTING=OFF \
    -DATRINIK_CONSUMER_VERSION="${version}" \
    -DCMAKE_PREFIX_PATH="${PWD}/install" \
    -DCURL_NO_CURL_CMAKE=TRUE \
    -DCURL_LIBRARY_RELEASE="${system_curl}" \
    -DCURL_LIBRARY_DEBUG="${system_curl}" \
    -DCURL_INCLUDE_DIR="${system_include}" >"${scope}-system-curl.log" 2>&1; then
    echo "system curl incorrectly accepted by ${scope}" >&2
    exit 1
  fi
  grep -q 'unqualified CURL::libcurl provider:' "${scope}-system-curl.log"
done
CONSUMER
