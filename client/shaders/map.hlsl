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

struct LightQuad {
    int4 x;
    int4 y;
    uint4 scalar;
    uint4 red;
    uint4 green;
    uint4 blue;
    uint owner;
    uint3 padding;
};

StructuredBuffer<LightQuad> light_quads : register(t0, space0);
StructuredBuffer<uint> light_forward_lut : register(t1, space0);
StructuredBuffer<uint> light_inverse_lut : register(t2, space0);

cbuffer LightVertexUniforms : register(b0, space1) {
    float2 light_viewport;
    float2 light_vertex_padding;
};

struct LightVertexOutput {
    float4 position : SV_Position;
    nointerpolation uint quad_id : TEXCOORD0;
    nointerpolation uint half_index : TEXCOORD1;
};

LightVertexOutput light_vertex(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    static const uint corners[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    uint corner = corners[vertex_id];
    LightQuad quad = light_quads[instance_id];
    float2 pixel = float2(quad.x[corner], quad.y[corner]);
    LightVertexOutput output;
    output.position = float4(pixel.x * 2.0 / light_viewport.x - 1.0,
                             1.0 - pixel.y * 2.0 / light_viewport.y,
                             0.0,
                             1.0);
    output.quad_id = instance_id;
    output.half_index = vertex_id / 3u;
    return output;
}

int light_edge(int2 a, int2 b, int2 point_twice) {
    int2 a_twice = a * 2;
    return (point_twice.x - a_twice.x) * (b.y - a.y) -
           (point_twice.y - a_twice.y) * (b.x - a.x);
}

uint light_bilinear(uint4 values, uint64_t u, uint64_t v, uint64_t scale) {
    uint64_t inverse_u = scale - u;
    uint64_t inverse_v = scale - v;
    uint64_t upper = (uint64_t(values.x) * inverse_u + uint64_t(values.y) * u + scale / 2u) /
                     scale;
    uint64_t lower = (uint64_t(values.w) * inverse_u + uint64_t(values.z) * u + scale / 2u) /
                     scale;
    return uint((upper * inverse_v + lower * v + scale / 2u) / scale);
}

uint light_neutral_linear(uint radiance) {
    if (radiance > 2048u) {
        return 65535u;
    }
    uint lower;
    uint low_level;
    uint level_range;
    uint shift;
    if (radiance <= 32u) {
        lower = 0u; low_level = 0u; level_range = 45u; shift = 5u;
    } else if (radiance <= 64u) {
        lower = 32u; low_level = 45u; level_range = 35u; shift = 5u;
    } else if (radiance <= 128u) {
        lower = 64u; low_level = 80u; level_range = 40u; shift = 6u;
    } else if (radiance <= 256u) {
        lower = 128u; low_level = 120u; level_range = 45u; shift = 7u;
    } else if (radiance <= 512u) {
        lower = 256u; low_level = 165u; level_range = 50u; shift = 8u;
    } else if (radiance <= 1024u) {
        lower = 512u; low_level = 215u; level_range = 30u; shift = 9u;
    } else {
        lower = 1024u; low_level = 245u; level_range = 10u; shift = 10u;
    }
    uint range = 1u << shift;
    uint numerator = (low_level << shift) + (radiance - lower) * level_range;
    uint code = numerator >> shift;
    uint remainder = numerator & (range - 1u);
    if (code >= 255u || remainder == 0u) {
        return light_forward_lut[code];
    }
    uint low = light_forward_lut[code];
    uint high = light_forward_lut[code + 1u];
    return low + (remainder * (high - low) + range / 2u) / range;
}

uint light_channel(uint source, uint scalar, uint radiance) {
    if (scalar == 0u) {
        return 0u;
    }
    uint neutral = light_neutral_linear(scalar);
    uint illumination = min(65535u,
                            uint((uint64_t(radiance) * neutral + scalar / 2u) / scalar));
    uint source_linear = light_forward_lut[source];
    uint product = (source_linear * illumination + 32767u) / 65535u;
    return light_inverse_lut[product];
}

Texture2D<float4> light_albedo : register(t0, space2);
Texture2D<uint> light_owner : register(t1, space2);
SamplerState light_albedo_sampler : register(s0, space2);
SamplerState light_owner_sampler : register(s1, space2);

float4 light_fragment(LightVertexOutput input) : SV_Target0 {
    int2 pixel = int2(input.position.xy);
    LightQuad quad = light_quads[input.quad_id];
    if (light_owner.Load(int3(pixel, 0)) != quad.owner) {
        discard;
    }

    uint b_index = input.half_index == 0u ? 1u : 2u;
    uint c_index = input.half_index == 0u ? 2u : 3u;
    int2 a = int2(quad.x.x, quad.y.x);
    int2 b = int2(quad.x[b_index], quad.y[b_index]);
    int2 c = int2(quad.x[c_index], quad.y[c_index]);
    int area_signed = light_edge(a, b, c * 2);
    int orientation = area_signed < 0 ? -1 : 1;
    uint64_t area = uint64_t(abs(area_signed));
    int2 point_twice = pixel * 2 + 1;
    uint64_t weight_b = uint64_t(orientation * light_edge(c, a, point_twice));
    uint64_t weight_c = uint64_t(orientation * light_edge(a, b, point_twice));
    uint64_t u = input.half_index == 0u ? weight_b + weight_c : weight_b;
    uint64_t v = input.half_index == 0u ? weight_c : weight_b + weight_c;
    uint scalar = light_bilinear(quad.scalar, u, v, area);
    uint red = light_bilinear(quad.red, u, v, area);
    uint green = light_bilinear(quad.green, u, v, area);
    uint blue = light_bilinear(quad.blue, u, v, area);

    float4 albedo = light_albedo.Load(int3(pixel, 0));
    uint4 source = uint4(floor(saturate(albedo) * 255.0 + 0.5));
    uint3 lit = uint3(light_channel(source.r, scalar, red),
                      light_channel(source.g, scalar, green),
                      light_channel(source.b, scalar, blue));
    return float4(float3(lit) / 255.0, albedo.a);
}
