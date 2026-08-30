#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 SOURCE_ROOT DOWNLOAD_CACHE OUTPUT_DIRECTORY" >&2
  exit 2
fi

source_root=$(realpath "$1")
download_cache=$(realpath -m "$2")
output_directory=$(realpath -m "$3")
toolchain_directory=$(realpath -m "${output_directory}.toolchain")
if [[ ! -f ${source_root}/client/shaders/map.hlsl ||
      ! -f ${source_root}/client/shaders/SHA256SUMS ]]; then
  echo "source root does not contain the governed GPU shader inputs" >&2
  exit 2
fi
if [[ ${output_directory} == / || ${toolchain_directory} == / ]]; then
  echo "refusing unsafe GPU shader output" >&2
  exit 2
fi

python3 "${source_root}/client/tools/prepare_gpu_shader_toolchain.py" \
  --cache "${download_cache}" \
  --output "${toolchain_directory}"

if [[ -e ${output_directory} ]]; then
  validation_header=$(mktemp)
  trap 'rm -f "${validation_header}"' EXIT
  python3 "${source_root}/client/tools/embed_gpu_shaders.py" \
    --input "${output_directory}" \
    --manifest "${source_root}/client/shaders/SHA256SUMS" \
    --output "${validation_header}"
  echo "GPU shader cohort current: ${output_directory}"
  exit 0
fi

mkdir -p "$(dirname "${output_directory}")"
staging=$(mktemp -d "${output_directory}.staging.XXXXXX")
validation_header=$(mktemp)
trap 'rm -rf "${staging}"; rm -f "${validation_header}"' EXIT
LD_LIBRARY_PATH="${toolchain_directory}/dxc/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  "${source_root}/client/tools/generate_gpu_shaders.sh" \
    "${toolchain_directory}/dxc/bin/dxc" \
    "${toolchain_directory}/spirv-cross/bin/spirv-cross" \
    "${staging}"
cmp "${source_root}/client/shaders/SHA256SUMS" "${staging}/SHA256SUMS"
python3 "${source_root}/client/tools/embed_gpu_shaders.py" \
  --input "${staging}" \
  --manifest "${source_root}/client/shaders/SHA256SUMS" \
  --output "${validation_header}"
mv "${staging}" "${output_directory}"
rm -f "${validation_header}"
trap - EXIT
echo "GPU shader cohort prepared: ${output_directory}"
