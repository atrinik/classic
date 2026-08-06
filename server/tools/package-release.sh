#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 TAG OUTPUT_DIRECTORY" >&2
  exit 2
fi

tag=$1
output_directory=$2
if [[ ! ${tag} =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
  echo "invalid release tag: ${tag}" >&2
  exit 1
fi

version=${BASH_REMATCH[1]}
package=atrinik-server-${version}
mkdir -p "${output_directory}"
git archive --format=tar.gz --prefix="${package}/" \
  --output="${output_directory}/${package}.tar.gz" "${tag}"
(
  cd "${output_directory}"
  sha256sum "${package}.tar.gz" >SHA256SUMS
)
