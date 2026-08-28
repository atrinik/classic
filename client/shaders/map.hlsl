/*
 * Authoritative Atrinik GPU map shader source.
 *
 * This file is compiled offline to pinned SPIR-V, DXIL, and MSL artifacts.
 * The client never invokes a runtime shader compiler.
 */

struct WorldVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

cbuffer WorldVertexUniforms : register(b0, space1) {
    float4 world_destination;
    float4 world_uv;
    float2 world_viewport;
    float2 world_vertex_padding;
};

WorldVertexOutput world_vertex(uint vertex_id : SV_VertexID) {
    static const float2 corners[6] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
    };
    float2 corner = corners[vertex_id];
    float2 pixel = world_destination.xy + corner * world_destination.zw;
    WorldVertexOutput output;
    output.position = float4(pixel.x * 2.0 / world_viewport.x - 1.0,
                             1.0 - pixel.y * 2.0 / world_viewport.y,
                             0.0,
                             1.0);
    output.uv = world_uv.xy + corner * world_uv.zw;
    return output;
}

Texture2D<float4> world_texture : register(t0, space2);
SamplerState world_sampler : register(s0, space2);

cbuffer WorldFragmentUniforms : register(b0, space3) {
    float4 world_modulation;
    uint world_owner;
    uint3 world_fragment_padding;
};

struct WorldFragmentOutput {
    float4 albedo : SV_Target0;
    uint owner : SV_Target1;
};

WorldFragmentOutput world_fragment(WorldVertexOutput input) {
    float4 color = world_texture.Sample(world_sampler, input.uv) * world_modulation;
    if (color.a <= 0.0) {
        discard;
    }
    WorldFragmentOutput output;
    output.albedo = color;
    output.owner = world_owner;
    return output;
}

struct FinalVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FinalVertexOutput final_vertex(uint vertex_id : SV_VertexID) {
    static const float2 corners[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0),
    };
    float2 corner = corners[vertex_id];
    FinalVertexOutput output;
    output.position = float4(corner.x - 1.0, 1.0 - corner.y, 0.0, 1.0);
    output.uv = corner;
    return output;
}

Texture2D<float4> final_albedo : register(t0, space2);
Texture2D<uint> final_owner : register(t1, space2);
SamplerState final_sampler : register(s0, space2);
SamplerState final_owner_sampler : register(s1, space2);

float4 final_fragment(FinalVertexOutput input) : SV_Target0 {
    /* Owner is deliberately fetched even before compact light-grid wiring so
     * all platform artifacts prove the exact integer attachment contract. */
    uint owner = final_owner.Load(int3(input.position.xy, 0));
    float4 albedo = final_albedo.Sample(final_sampler, input.uv);
    return owner == 255u ? float4(0.0, 0.0, 0.0, 0.0) : albedo;
}
