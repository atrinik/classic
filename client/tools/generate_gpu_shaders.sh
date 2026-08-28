#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 DXC SPIRV_CROSS OUTPUT_DIRECTORY" >&2
    exit 2
fi

dxc=$1
spirv_cross=$2
output=$3
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
shader=$source_dir/shaders/map.hlsl

mkdir -p "$output"

compile_stage() {
    name=$1
    profile=$2
    entry=$3
    stage=$4
    "$dxc" -T "$profile" -E "$entry" -O3 -Ges -Fo "$output/$name.dxil" "$shader"
    "$dxc" -T "$profile" -E "$entry" -O3 -Ges -spirv \
        -fspv-target-env=vulkan1.1 -fvk-use-dx-layout \
        -Fo "$output/$name.spv" "$shader"
    "$spirv_cross" "$output/$name.spv" --msl --msl-version 20200 \
        --rename-entry-point "$entry" main0 "$stage" \
        --output "$output/$name.msl"
}

compile_stage world_vertex vs_6_0 world_vertex vert
compile_stage world_fragment ps_6_0 world_fragment frag
compile_stage final_vertex vs_6_0 final_vertex vert
compile_stage final_fragment ps_6_0 final_fragment frag
compile_stage light_vertex vs_6_0 light_vertex vert
compile_stage light_fragment ps_6_0 light_fragment frag

(
    cd "$output"
    sha256sum ./*.dxil ./*.msl ./*.spv
) > "$output/SHA256SUMS"
