/*
 * Authoritative Atrinik GPU map shader source.
 *
 * This file is compiled offline to pinned SPIR-V, DXIL, and MSL artifacts.
 * The client never invokes a runtime shader compiler.
 */

struct WorldVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 modulation : COLOR0;
    nointerpolation uint lighting_key : TEXCOORD1;
};

cbuffer WorldVertexUniforms : register(b0, space1) {
    float2 world_viewport;
    float2 world_vertex_padding;
};

struct WorldInstance {
    float4 destination;
    float4 uv;
    float4 modulation;
    uint4 lighting_key_padding;
};

StructuredBuffer<WorldInstance> world_instances : register(t0, space0);

cbuffer WorldSlotUniforms : register(b1, space1) {
    uint4 world_instance_slots[64];
};

WorldVertexOutput world_vertex(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    static const float2 corners[6] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
    };
    uint stable_slot = world_instance_slots[instance_id / 4][instance_id % 4];
    WorldInstance instance = world_instances[stable_slot];
    float2 corner = corners[vertex_id];
    float2 pixel = instance.destination.xy + corner * instance.destination.zw;
    WorldVertexOutput output;
    output.position = float4(pixel.x * 2.0 / world_viewport.x - 1.0,
                             1.0 - pixel.y * 2.0 / world_viewport.y,
                             0.0,
                             1.0);
    output.uv = instance.uv.xy + corner * instance.uv.zw;
    output.modulation = instance.modulation;
    output.lighting_key = instance.lighting_key_padding.x;
    return output;
}

Texture2D<float4> world_texture : register(t0, space2);
SamplerState world_sampler : register(s0, space2);
StructuredBuffer<uint> world_projected_light_rows : register(t1, space2);

static const uint WORLD_PROJECTED_LIGHT_FLAG = 524288u;
static const uint WORLD_PROJECTED_LIGHT_OWNER_MASK = 15u;
static const uint WORLD_LIGHT_OWNER_COUNT = 13u;

struct WorldFragmentOutput {
    float4 albedo : SV_Target0;
    uint lighting_key : SV_Target1;
};

WorldFragmentOutput world_fragment(WorldVertexOutput input) {
    float4 color = world_texture.Sample(world_sampler, input.uv) * input.modulation;
    if (color.a <= 0.0) {
        discard;
    }
    WorldFragmentOutput output;
    output.albedo = color;
    if ((input.lighting_key & WORLD_PROJECTED_LIGHT_FLAG) != 0u) {
        uint owner = input.lighting_key & WORLD_PROJECTED_LIGHT_OWNER_MASK;
        uint row = uint(input.position.y);
        output.lighting_key = world_projected_light_rows[row * WORLD_LIGHT_OWNER_COUNT + owner];
    } else {
        output.lighting_key = input.lighting_key;
    }
    return output;
}

struct FinalVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FinalVertexOutput final_vertex(uint vertex_id : SV_VertexID) {
    static const float2 corners[6] = {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
    };
    float2 corner = corners[vertex_id];
    FinalVertexOutput output;
    output.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    output.uv = corner;
    return output;
}

struct LightQuad {
    int4 x;
    int4 y;
    uint4 scalar;
    uint4 red;
    uint4 green;
    uint4 blue;
    uint4 owner_padding;
};

struct LightRow {
    uint upper_offset;
    uint upper_count;
    int upper_y;
    uint upper_padding;
    uint lower_offset;
    uint lower_count;
    int lower_y;
    uint lower_padding;
    uint owner;
    int sample_y;
    uint2 padding;
};

struct LightSpan {
    int first_x;
    int last_x;
    uint quad;
    uint padding;
};

Texture2D<float4> final_albedo : register(t0, space2);
Texture2D<uint> final_lighting_key : register(t1, space2);
SamplerState final_sampler : register(s0, space2);

StructuredBuffer<LightQuad> final_light_quads : register(t2, space2);
StructuredBuffer<LightRow> final_light_rows : register(t3, space2);
StructuredBuffer<LightSpan> final_light_spans : register(t4, space2);
StructuredBuffer<uint> final_forward_lut : register(t5, space2);
StructuredBuffer<uint> final_inverse_lut : register(t6, space2);

uint light_mul_divide(uint value, uint multiplier, uint divisor);
uint light_bilinear(uint4 values, uint u, uint v, uint scale);
uint light_weighted_divide(uint left,
                           uint left_weight,
                           uint right,
                           uint right_weight,
                           uint divisor);
uint light_extrapolate(uint left, uint right, uint progress, uint divisor);
int light_edge(int2 a, int2 b, int2 point_twice);

uint final_neutral_linear(uint radiance) {
    if (radiance > 2048u) {
        return 65535u;
    }
    uint lower;
    uint low_level;
    uint level_range;
    uint shift;
    if (radiance <= 32u) {
        lower = 0u;
        low_level = 0u;
        level_range = 45u;
        shift = 5u;
    } else if (radiance <= 64u) {
        lower = 32u;
        low_level = 45u;
        level_range = 35u;
        shift = 5u;
    } else if (radiance <= 128u) {
        lower = 64u;
        low_level = 80u;
        level_range = 40u;
        shift = 6u;
    } else if (radiance <= 256u) {
        lower = 128u;
        low_level = 120u;
        level_range = 45u;
        shift = 7u;
    } else if (radiance <= 512u) {
        lower = 256u;
        low_level = 165u;
        level_range = 50u;
        shift = 8u;
    } else if (radiance <= 1024u) {
        lower = 512u;
        low_level = 215u;
        level_range = 30u;
        shift = 9u;
    } else {
        lower = 1024u;
        low_level = 245u;
        level_range = 10u;
        shift = 10u;
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

uint final_lighting_key_load(int2 position) {
    return final_lighting_key.Load(int3(position, 0));
}

bool final_light_triangle(LightQuad quad,
                          int2 point_twice,
                          uint half_index,
                          out uint u,
                          out uint v,
                          out uint area) {
    u = 0u;
    v = 0u;
    area = 0u;
    uint b_index = half_index == 0u ? 1u : 2u;
    uint c_index = half_index == 0u ? 2u : 3u;
    int2 a = int2(quad.x.x, quad.y.x);
    int2 b = int2(quad.x[b_index], quad.y[b_index]);
    int2 c = int2(quad.x[c_index], quad.y[c_index]);
    int area_signed = light_edge(a, b, c * 2);
    if (area_signed == 0) {
        return false;
    }
    int orientation = area_signed < 0 ? -1 : 1;
    int weight_b_signed = orientation * light_edge(c, a, point_twice);
    int weight_c_signed = orientation * light_edge(a, b, point_twice);
    area = uint(abs(area_signed));
    if (weight_b_signed < 0 || weight_c_signed < 0 ||
        uint(weight_b_signed + weight_c_signed) > area) {
        return false;
    }
    uint weight_b = uint(weight_b_signed);
    uint weight_c = uint(weight_c_signed);
    u = half_index == 0u ? weight_b + weight_c : weight_b;
    v = half_index == 0u ? weight_c : weight_b + weight_c;
    return true;
}

uint4 final_light_load(uint quad_id, int2 position) {
    LightQuad quad = final_light_quads[quad_id];
    int2 point_twice = position * 2 + 1;
    uint u;
    uint v;
    uint area;
    if (!final_light_triangle(quad, point_twice, 0u, u, v, area) &&
        !final_light_triangle(quad, point_twice, 1u, u, v, area)) {
        int2 nearest = clamp(position,
                             int2(min(min(quad.x.x, quad.x.y), min(quad.x.z, quad.x.w)),
                                  min(min(quad.y.x, quad.y.y), min(quad.y.z, quad.y.w))),
                             int2(max(max(quad.x.x, quad.x.y), max(quad.x.z, quad.x.w)),
                                  max(max(quad.y.x, quad.y.y), max(quad.y.z, quad.y.w))));
        point_twice = nearest * 2 + 1;
        if (!final_light_triangle(quad, point_twice, 0u, u, v, area) &&
            !final_light_triangle(quad, point_twice, 1u, u, v, area)) {
            uint closest = 0u;
            int2 delta = position - int2(quad.x.x, quad.y.x);
            uint distance = uint(dot(delta, delta));
            for (uint corner = 1u; corner < 4u; corner++) {
                delta = position - int2(quad.x[corner], quad.y[corner]);
                uint candidate = uint(dot(delta, delta));
                if (candidate <= distance) {
                    closest = corner;
                    distance = candidate;
                }
            }
            return uint4(quad.scalar[closest],
                         quad.red[closest],
                         quad.green[closest],
                         quad.blue[closest]);
        }
    }
    return uint4(light_bilinear(quad.scalar, u, v, area),
                 light_bilinear(quad.red, u, v, area),
                 light_bilinear(quad.green, u, v, area),
                 light_bilinear(quad.blue, u, v, area));
}

uint4 final_light_horizontal(uint offset, uint count, int row_y, int x) {
    /* Find the first span whose left edge is strictly right of x. Rows are
     * emitted in increasing, non-overlapping x order, so this is also the
     * split between the nearest left and right samples. */
    uint low = 0u;
    uint high = count;
    while (low < high) {
        uint middle = low + (high - low) / 2u;
        LightSpan span = final_light_spans[offset + middle];
        if (span.first_x <= x) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    uint right = low;
    if (right == 0u) {
        LightSpan span = final_light_spans[offset];
        return final_light_load(span.quad, int2(span.first_x, row_y));
    }
    uint left = right - 1u;
    LightSpan left_span = final_light_spans[offset + left];
    if (x <= left_span.last_x) {
        return final_light_load(left_span.quad, int2(x, row_y));
    }
    if (right == count) {
        LightSpan span = left_span;
        return final_light_load(span.quad, int2(span.last_x, row_y));
    }
    LightSpan right_span = final_light_spans[offset + right];
    uint4 left_light = final_light_load(left_span.quad, int2(left_span.last_x, row_y));
    uint4 right_light = final_light_load(right_span.quad, int2(right_span.first_x, row_y));
    uint divisor = uint(right_span.first_x - left_span.last_x);
    uint progress = uint(x - left_span.last_x);
    return uint4(light_extrapolate(left_light.x, right_light.x, progress, divisor),
                 light_extrapolate(left_light.y, right_light.y, progress, divisor),
                 light_extrapolate(left_light.z, right_light.z, progress, divisor),
                 light_extrapolate(left_light.w, right_light.w, progress, divisor));
}

uint4 final_light_row(uint row_id, int x) {
    LightRow row = final_light_rows[row_id];
    uint4 upper = final_light_horizontal(row.upper_offset, row.upper_count, row.upper_y, x);
    if (row.upper_y == row.lower_y) {
        return upper;
    }
    uint4 lower = final_light_horizontal(row.lower_offset, row.lower_count, row.lower_y, x);
    uint divisor = uint(row.lower_y - row.upper_y);
    uint progress = uint(clamp(row.sample_y, row.upper_y, row.lower_y) - row.upper_y);
    return uint4(light_extrapolate(upper.x, lower.x, progress, divisor),
                 light_extrapolate(upper.y, lower.y, progress, divisor),
                 light_extrapolate(upper.z, lower.z, progress, divisor),
                 light_extrapolate(upper.w, lower.w, progress, divisor));
}

float4 final_fragment(FinalVertexOutput input) : SV_Target0 {
    uint key = final_lighting_key_load(int2(input.position.xy));
    uint encoded_quad = key & 524287u;
    float4 albedo = final_albedo.Sample(final_sampler, input.uv);
    if (encoded_quad == 0u) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    if (encoded_quad == 524287u) {
        return albedo;
    }
    if (encoded_quad == 524286u) {
        return float4(0.0, 0.0, 0.0, albedo.a);
    }
    uint4 light = final_light_row(encoded_quad - 1u, int(input.position.x));
    uint4 source = uint4(floor(saturate(albedo) * 255.0 + 0.5));
    uint3 lit = uint3(final_channel(source.r, light.x, light.y),
                      final_channel(source.g, light.x, light.z),
                      final_channel(source.b, light.x, light.w));
    return float4(float3(lit) / 255.0, albedo.a);
}

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
    return (point_twice.x - a_twice.x) * (b.y - a.y) - (point_twice.y - a_twice.y) * (b.x - a.x);
}

bool light_u64_less_equal(uint left_high, uint left_low, uint right_high, uint right_low) {
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

uint light_wide_divide(uint numerator_low, uint numerator_high, uint divisor) {
    /* Every lighting sample is uint16_t, so the quotient is bounded to
     * 16 bits. Compare trial products as two 32-bit limbs instead of
     * requiring the optional shaderInt64 feature on Vulkan. */
    uint quotient = 0u;
    for (int bit = 15; bit >= 0; bit--) {
        uint candidate = quotient | (1u << uint(bit));
        uint product_low;
        uint product_high;
        light_mul_wide(candidate, divisor, product_low, product_high);
        if (light_u64_less_equal(product_high, product_low, numerator_high, numerator_low)) {
            quotient = candidate;
        }
    }
    return quotient;
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

    return light_wide_divide(numerator_low, numerator_high, divisor);
}

uint light_mul_divide_floor(uint value, uint multiplier, uint divisor) {
    uint numerator_low;
    uint numerator_high;
    light_mul_wide(value, multiplier, numerator_low, numerator_high);
    return light_wide_divide(numerator_low, numerator_high, divisor);
}

/* Match the deleted CPU field's signed-delta interpolation exactly. C integer
 * division truncated the delta toward zero, which is intentionally asymmetric
 * for increasing and decreasing sample pairs. */
uint light_extrapolate(uint left, uint right, uint progress, uint divisor) {
    if (right >= left) {
        return left + light_mul_divide_floor(right - left, progress, divisor);
    }
    return left - light_mul_divide_floor(left - right, progress, divisor);
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
