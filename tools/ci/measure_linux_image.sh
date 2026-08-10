#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 IMAGE OUTPUT_DIRECTORY" >&2
  exit 2
fi

image=$1
output_dir=$2
if [[ ! ${image} =~ ^ghcr\.io/atrinik/classic-build:[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "image must be an immutable versioned Atrinik Classic build digest" >&2
  exit 2
fi

mkdir -p "${output_dir}"
cpu_count=$(nproc)
docker_client_version=$(docker version --format '{{.Client.Version}}')
docker_server_version=$(docker version --format '{{.Server.Version}}')
kernel=$(uname -srmo)
cold_pull_ms=0
warm_pull_ms=0
cold_start_ms=0
measure_ms() {
  local start end
  start=$(date +%s%N)
  "${@:2}"
  end=$(date +%s%N)
  printf -v "$1" '%d' "$(((end - start) / 1000000))"
}

docker image rm "${image}" >/dev/null 2>&1 || true
measure_ms cold_pull_ms docker pull "${image}"
measure_ms warm_pull_ms docker pull "${image}"

repository=${image%@*}
index=$(docker buildx imagetools inspect "${image}" --raw)
platform_digest=$(jq -er '
  .manifests[]
  | select(.platform.os == "linux" and .platform.architecture == "amd64")
  | select(.annotations["vnd.docker.reference.type"] != "attestation-manifest")
  | .digest
' <<<"${index}")
manifest=$(docker buildx imagetools inspect "${repository}@${platform_digest}" --raw)
compressed_bytes=$(jq -er '[.layers[].size] | add' <<<"${manifest}")
content_bytes=$(docker image inspect "${image}" --format '{{.Size}}')

measure_ms cold_start_ms docker run --rm "${image}" true
warm_samples=()
for _ in 1 2 3 4 5; do
  measure_ms sample docker run --rm "${image}" true
  warm_samples+=("${sample}")
done

evidence="${output_dir}/linux-image.tsv"
{
  printf 'runner_image_os\t%s\n' "${ImageOS:-unknown}"
  printf 'runner_image_version\t%s\n' "${ImageVersion:-unknown}"
  printf 'runner_arch\t%s\n' "${RUNNER_ARCH:-unknown}"
  printf 'kernel\t%s\n' "${kernel}"
  printf 'cpu_count\t%s\n' "${cpu_count}"
  printf 'docker_client_version\t%s\n' "${docker_client_version}"
  printf 'docker_server_version\t%s\n' "${docker_server_version}"
  printf 'image\t%s\n' "${image}"
  printf 'platform_manifest\t%s\n' "${platform_digest}"
  printf 'compressed_bytes\t%s\n' "${compressed_bytes}"
  printf 'content_bytes\t%s\n' "${content_bytes}"
  printf 'cold_pull_ms\t%s\n' "${cold_pull_ms}"
  printf 'warm_pull_ms\t%s\n' "${warm_pull_ms}"
  printf 'cold_start_ms\t%s\n' "${cold_start_ms}"
  for sample in "${warm_samples[@]}"; do
    printf 'warm_start_ms\t%s\n' "${sample}"
  done
} | tee "${evidence}"

if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
  {
    echo '### Immutable Classic Linux image'
    echo
    echo '| Measurement | Value |'
    echo '| --- | ---: |'
    printf '| Runner image | %s %s |\n' \
      "${ImageOS:-unknown}" "${ImageVersion:-unknown}"
    printf '| Runner architecture / CPUs | %s / %s |\n' \
      "${RUNNER_ARCH:-unknown}" "${cpu_count}"
    printf '| Docker client / server | %s / %s |\n' \
      "${docker_client_version}" "${docker_server_version}"
    printf '| Compressed amd64 layers | %s B |\n' "${compressed_bytes}"
    printf '| Docker content | %s B |\n' "${content_bytes}"
    printf '| Cold pull | %s ms |\n' "${cold_pull_ms}"
    printf '| Immediate warm pull | %s ms |\n' "${warm_pull_ms}"
    printf '| Cold startup | %s ms |\n' "${cold_start_ms}"
    printf '| Warm startup samples | %s ms |\n' "$(IFS=', '; echo "${warm_samples[*]}")"
  } >>"${GITHUB_STEP_SUMMARY}"
fi
