#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 OUTPUT_DIRECTORY" >&2
  exit 2
fi
if [[ -z ${GITHUB_REPOSITORY:-} ]]; then
  echo "GITHUB_REPOSITORY is required to verify the recovery attestation" >&2
  exit 2
fi

output=$1
if [[ -e ${output} ]]; then
  echo "refusing to replace existing recovery directory: ${output}" >&2
  exit 1
fi

# This is the last attested bundle before the Content history cutover. The
# Python staging boundary reuses only archive names and digests that still
# match the current locks; the historical Content archive is therefore ignored.
trusted_image=ghcr.io/atrinik/classic-dependencies@sha256:f71e7dce5893e3fa6734e067c02738925a3b5e31c201dc202c85eaaabd720685

gh attestation verify "oci://${trusted_image}" \
  --repo "${GITHUB_REPOSITORY}" \
  --bundle-from-oci
docker pull "${trusted_image}"
docker image inspect "${trusted_image}" >/dev/null
container=$(docker create "${trusted_image}" true)
cleanup() {
  docker rm "${container}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "${output}"
docker cp "${container}:/bundle/." "${output}"
test -f "${output}/manifest.json"
test -d "${output}/archives"
