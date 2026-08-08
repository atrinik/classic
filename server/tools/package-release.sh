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
staging_directory=$(mktemp -d)
trap 'rm -rf "${staging_directory}"' EXIT
git archive --format=tar --prefix="${package}/" "${tag}" \
  | tar -xf - -C "${staging_directory}"
printf '%s\n' "${version}" >"${staging_directory}/${package}/VERSION"
tar --sort=name --owner=0 --group=0 --numeric-owner \
  --mtime="@$(git show -s --format=%ct "${tag}^{commit}")" \
  -czf "${output_directory}/${package}.tar.gz" \
  -C "${staging_directory}" "${package}"
