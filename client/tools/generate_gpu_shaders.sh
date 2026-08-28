#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 DXC SPIRV_CROSS OUTPUT_DIRECTORY" >&2
    exit 2
fi

dxc=$1
spirv_cross=$2
output=$3
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
shader=$source_dir/shaders/map.hlsl

mkdir -p "$output"

compile_stage() {
    name=$1
    profile=$2
    entry=$3
    stage=$4
    "$dxc" -T "$profile" -E "$entry" -O3 -Ges -Fo "$output/$name.dxil" "$shader"
    "$dxc" -T "$profile" -E "$entry" -O3 -Ges -spirv \
        -fspv-target-env=vulkan1.0 -fvk-use-dx-layout \
        -Fo "$output/$name.spv" "$shader"
    "$spirv_cross" "$output/$name.spv" --msl --msl-version 20200 \
        --rename-entry-point "$entry" main0 "$stage" \
        --output "$output/$name.msl"
    storage_name=
    uniform_name=
    case $name in
        light_vertex)
            storage_name=vertex_light_quads
            uniform_name=LightVertexUniforms
            ;;
        world_vertex)
            storage_name=world_instances
            uniform_name=WorldVertexUniforms
            ;;
    esac
    if [ -n "$storage_name" ]; then
        if grep -q "$storage_name \[\[buffer(1)\]\]" "$output/$name.msl" &&
            grep -q "$uniform_name \[\[buffer(0)\]\]" "$output/$name.msl"; then
            :
        elif grep -q "$storage_name \[\[buffer(0)\]\]" "$output/$name.msl" &&
            grep -q "$uniform_name \[\[buffer(1)\]\]" "$output/$name.msl"; then
            sed -e "s/$storage_name \[\[buffer(0)\]\]/$storage_name [[buffer(1)]]/" \
                -e "s/$uniform_name \[\[buffer(1)\]\]/$uniform_name [[buffer(0)]]/" \
                "$output/$name.msl" > "$output/$name.msl.bound"
            mv "$output/$name.msl.bound" "$output/$name.msl"
        else
            echo "unexpected $name MSL resource bindings" >&2
            exit 1
        fi
    fi
    if [ "$name" = world_vertex ]; then
        if grep -q 'world_instances \[\[buffer(1)\]\]' "$output/$name.msl" &&
            grep -q 'WorldSlotUniforms \[\[buffer(2)\]\]' "$output/$name.msl"; then
            sed -e 's/world_instances \[\[buffer(1)\]\]/world_instances [[buffer(2)]]/' \
                -e 's/WorldSlotUniforms \[\[buffer(2)\]\]/WorldSlotUniforms [[buffer(1)]]/' \
                "$output/$name.msl" > "$output/$name.msl.bound"
            mv "$output/$name.msl.bound" "$output/$name.msl"
        fi
        if ! grep -q 'WorldVertexUniforms \[\[buffer(0)\]\]' "$output/$name.msl" ||
            ! grep -q 'WorldSlotUniforms \[\[buffer(1)\]\]' "$output/$name.msl" ||
            ! grep -q 'world_instances \[\[buffer(2)\]\]' "$output/$name.msl"; then
            echo "unexpected $name slot-indirection bindings" >&2
            exit 1
        fi
    fi
    awk '{ sub(/[[:space:]]+$/, ""); lines[NR] = $0 } END { last = NR; while (last > 0 && lines[last] == "") last--; for (i = 1; i <= last; i++) print lines[i] }' \
        "$output/$name.msl" > "$output/$name.msl.normalized"
    mv "$output/$name.msl.normalized" "$output/$name.msl"
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
