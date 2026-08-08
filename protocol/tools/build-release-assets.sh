#!/usr/bin/env bash

set -euo pipefail
if [[ $# -ne 1 || ! $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 MAJOR.MINOR.PATCH" >&2
  exit 2
fi

tools/package-release.sh "v$1" build/release
find build/release -maxdepth 1 -type f -name '*.whl' -delete
repository_root=$(pwd)
source_epoch=$(git show -s --format=%ct "v$1^{commit}")
wheel_source=$(mktemp -d)
trap 'rm -rf "${wheel_source}"' EXIT
git archive "v$1" | tar -xf - -C "${wheel_source}"
(
  cd "${wheel_source}"
  SOURCE_DATE_EPOCH="${source_epoch}" \
    SETUPTOOLS_SCM_PRETEND_VERSION="$1" \
    python3 -m build --wheel --outdir "${repository_root}/build/release"
)
mapfile -t wheels < <(find build/release -maxdepth 1 \
  -type f -name '*.whl' -print)
if [[ ${#wheels[@]} -ne 1 ]]; then
  echo "Expected exactly one protocol wheel, found ${#wheels[@]}" >&2
  exit 1
fi
(
  cd build/release
  sha256sum ./*.whl >>SHA256SUMS
)
