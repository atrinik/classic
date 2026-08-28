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
    uint world_lighting_key;
    uint3 world_fragment_padding;
};

struct WorldFragmentOutput {
    float4 albedo : SV_Target0;
    uint lighting_key : SV_Target1;
};

WorldFragmentOutput world_fragment(WorldVertexOutput input) {
    float4 color = world_texture.Sample(world_sampler, input.uv) * world_modulation;
    if (color.a <= 0.0) {
        discard;
    }
    WorldFragmentOutput output;
    output.albedo = color;
    output.lighting_key = world_lighting_key;
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
Texture2D<uint> final_lighting_key : register(t1, space2);
Texture2DArray<uint4> final_light : register(t2, space2);
SamplerState final_sampler : register(s0, space2);
SamplerState final_lighting_key_sampler : register(s1, space2);
SamplerState final_light_sampler : register(s2, space2);

StructuredBuffer<uint> final_forward_lut : register(t3, space2);
StructuredBuffer<uint> final_inverse_lut : register(t4, space2);

uint light_mul_divide(uint value, uint multiplier, uint divisor);

uint final_neutral_linear(uint radiance) {
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
        return final_forward_lut[code];
    }
    uint low = final_forward_lut[code];
    uint high = final_forward_lut[code + 1u];
    return low + (remainder * (high - low) + range / 2u) / range;
}

uint final_channel(uint source, uint scalar, uint radiance) {
    if (scalar == 0u) {
        return 0u;
    }
    uint neutral = final_neutral_linear(scalar);
    uint illumination = min(65535u, light_mul_divide(radiance, neutral, scalar));
    uint source_linear = final_forward_lut[source];
    uint product = light_mul_divide(source_linear, illumination, 65535u);
    return final_inverse_lut[product];
}

float4 final_fragment(FinalVertexOutput input) : SV_Target0 {
    uint key = final_lighting_key.Load(int3(input.position.xy, 0));
    uint encoded_owner = key & 255u;
    float4 albedo = final_albedo.Sample(final_sampler, input.uv);
    if (encoded_owner == 0u) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    if (encoded_owner == 255u) {
        return albedo;
    }
    uint owner = encoded_owner - 1u;
    int sample_y = int((key >> 8u) & 65535u);
    sample_y -= sample_y >= 32768 ? 65536 : 0;
    uint height;
    uint width;
    uint layers;
    final_light.GetDimensions(width, height, layers);
    sample_y = clamp(sample_y, 0, int(height) - 1);
    uint4 light = final_light.Load(int4(int(input.position.x), sample_y, int(owner), 0));
    uint4 source = uint4(floor(saturate(albedo) * 255.0 + 0.5));
    uint3 lit = uint3(final_channel(source.r, light.x, light.y),
                      final_channel(source.g, light.x, light.z),
                      final_channel(source.b, light.x, light.w));
    return float4(float3(lit) / 255.0, albedo.a);
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

StructuredBuffer<LightQuad> vertex_light_quads : register(t0, space0);
StructuredBuffer<LightQuad> fragment_light_quads : register(t0, space2);

cbuffer LightVertexUniforms : register(b0, space1) {
    float2 light_viewport;
    uint light_first_quad;
    uint light_vertex_padding;
};

struct LightVertexOutput {
    float4 position : SV_Position;
    nointerpolation uint quad_id : TEXCOORD0;
    nointerpolation uint half_index : TEXCOORD1;
};

LightVertexOutput light_vertex(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    static const uint corners[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    uint corner = corners[vertex_id];
    uint quad_id = light_first_quad + instance_id;
    LightQuad quad = vertex_light_quads[quad_id];
    float2 pixel = float2(quad.x[corner], quad.y[corner]);
    LightVertexOutput output;
    output.position = float4(pixel.x * 2.0 / light_viewport.x - 1.0,
                             1.0 - pixel.y * 2.0 / light_viewport.y,
                             0.0,
                             1.0);
    output.quad_id = quad_id;
    output.half_index = vertex_id / 3u;
    return output;
}

int light_edge(int2 a, int2 b, int2 point_twice) {
    int2 a_twice = a * 2;
    return (point_twice.x - a_twice.x) * (b.y - a.y) -
           (point_twice.y - a_twice.y) * (b.x - a.x);
}

bool light_u64_less_equal(uint left_high,
                          uint left_low,
                          uint right_high,
                          uint right_low) {
    return left_high < right_high || (left_high == right_high && left_low <= right_low);
}

void light_mul_wide(uint left, uint right, out uint low, out uint high) {
    uint left_low = left & 65535u;
    uint left_high = left >> 16u;
    uint right_low = right & 65535u;
    uint right_high = right >> 16u;
    uint low_low = left_low * right_low;
    uint low_high = left_low * right_high;
    uint high_low = left_high * right_low;
    uint high_high = left_high * right_high;
    low = low_low;
    high = high_high + (low_high >> 16u) + (high_low >> 16u);
    uint addend = (low_high & 65535u) << 16u;
    uint previous = low;
    low += addend;
    high += low < previous ? 1u : 0u;
    addend = (high_low & 65535u) << 16u;
    previous = low;
    low += addend;
    high += low < previous ? 1u : 0u;
}

uint light_weighted_divide(uint left,
                           uint left_weight,
                           uint right,
                           uint right_weight,
                           uint divisor) {
    uint left_low;
    uint left_high;
    uint right_low;
    uint right_high;
    light_mul_wide(left, left_weight, left_low, left_high);
    light_mul_wide(right, right_weight, right_low, right_high);
    uint numerator_low = left_low + right_low;
    uint numerator_high = left_high + right_high + (numerator_low < left_low ? 1u : 0u);
    uint rounding = divisor / 2u;
    uint rounded_low = numerator_low + rounding;
    numerator_high += rounded_low < numerator_low ? 1u : 0u;
    numerator_low = rounded_low;

    /* Every lighting sample is uint16_t, so the rounded quotient is bounded
     * to 16 bits. Compare trial products as two 32-bit limbs instead of
     * requiring the optional shaderInt64 feature on Vulkan. */
    uint quotient = 0u;
    for (int bit = 15; bit >= 0; bit--) {
        uint candidate = quotient | (1u << uint(bit));
        uint product_low;
        uint product_high;
        light_mul_wide(candidate, divisor, product_low, product_high);
        if (light_u64_less_equal(product_high,
                                 product_low,
                                 numerator_high,
                                 numerator_low)) {
            quotient = candidate;
        }
    }
    return quotient;
}

uint light_mul_divide(uint value, uint multiplier, uint divisor) {
    return light_weighted_divide(value, multiplier, 0u, 0u, divisor);
}

uint light_bilinear(uint4 values, uint u, uint v, uint scale) {
    uint inverse_u = scale - u;
    uint inverse_v = scale - v;
    uint upper = light_weighted_divide(values.x, inverse_u, values.y, u, scale);
    uint lower = light_weighted_divide(values.w, inverse_u, values.z, u, scale);
    return light_weighted_divide(upper, inverse_v, lower, v, scale);
}

uint4 light_fragment(LightVertexOutput input) : SV_Target0 {
    int2 pixel = int2(input.position.xy);
    LightQuad quad = fragment_light_quads[input.quad_id];

    uint b_index = input.half_index == 0u ? 1u : 2u;
    uint c_index = input.half_index == 0u ? 2u : 3u;
    int2 a = int2(quad.x.x, quad.y.x);
    int2 b = int2(quad.x[b_index], quad.y[b_index]);
    int2 c = int2(quad.x[c_index], quad.y[c_index]);
    int area_signed = light_edge(a, b, c * 2);
    if (area_signed == 0) {
        discard;
    }
    int orientation = area_signed < 0 ? -1 : 1;
    uint area = uint(abs(area_signed));
    int2 point_twice = pixel * 2 + 1;
    uint weight_b = uint(orientation * light_edge(c, a, point_twice));
    uint weight_c = uint(orientation * light_edge(a, b, point_twice));
    uint u = input.half_index == 0u ? weight_b + weight_c : weight_b;
    uint v = input.half_index == 0u ? weight_c : weight_b + weight_c;
    uint scalar = light_bilinear(quad.scalar, u, v, area);
    uint red = light_bilinear(quad.red, u, v, area);
    uint green = light_bilinear(quad.green, u, v, area);
    uint blue = light_bilinear(quad.blue, u, v, area);
    return uint4(scalar, red, green, blue);
}
