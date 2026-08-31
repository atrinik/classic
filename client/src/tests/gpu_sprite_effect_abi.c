/*************************************************************************
 * Atrinik client GPU sprite-effect ABI tests.                          *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gpu_sprite_effect.h>

#define CHECK(_expression)                                                        \
    do {                                                                          \
        if (!(_expression)) {                                                     \
            fprintf(stderr, "GPU sprite ABI check failed at line %d: %s\n",      \
                    __LINE__,                                                    \
                    #_expression);                                               \
            return false;                                                         \
        }                                                                         \
    } while (0)

static uint32_t valid_texture_flags(void) {
    return GPU_SPRITE_TEXTURE_ATLAS | GPU_SPRITE_TEXTURE_SOURCE_COLOR_KEY |
           GPU_SPRITE_TEXTURE_STRAIGHT_ALPHA | GPU_SPRITE_TEXTURE_NEAREST |
           GPU_SPRITE_TEXTURE_CLAMP_EDGE;
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool test_layout(void) {
    CHECK(sizeof(gpu_sprite_instance_t) == GPU_SPRITE_INSTANCE_STRIDE);
    CHECK(offsetof(gpu_sprite_instance_t, destination) == 0);
    CHECK(offsetof(gpu_sprite_instance_t, uv) == 16);
    CHECK(offsetof(gpu_sprite_instance_t, modulation) == 32);
    CHECK(offsetof(gpu_sprite_instance_t, effect_flags) == 48);
    CHECK(offsetof(gpu_sprite_instance_t, texture_flags) == 52);
    CHECK(offsetof(gpu_sprite_instance_t, lighting_key) == 56);
    CHECK(offsetof(gpu_sprite_instance_t, owner_depth) == 60);
    CHECK(offsetof(gpu_sprite_instance_t, texture_metadata) == 64);
    CHECK(offsetof(gpu_sprite_instance_t, transform) == 80);
    CHECK(offsetof(gpu_sprite_instance_t, effect_color) == 96);
    CHECK(offsetof(gpu_sprite_instance_t, effect_parameters) == 112);
    CHECK(offsetof(gpu_sprite_instance_t, effect_time) == 128);
    CHECK(offsetof(gpu_sprite_instance_t, effect_phase) == 132);
    CHECK(offsetof(gpu_sprite_instance_t, effect_seed) == 136);
    CHECK(offsetof(gpu_sprite_instance_t, abi_version) == 140);
    return true;
}

static bool test_default_and_owner_depth(void) {
    static const uint32_t expected_default[GPU_SPRITE_INSTANCE_STRIDE / sizeof(uint32_t)] = {
        [8] = UINT32_C(0x3f800000),
        [9] = UINT32_C(0x3f800000),
        [10] = UINT32_C(0x3f800000),
        [11] = UINT32_C(0x3f800000),
        [14] = UINT32_C(0x0007ffff),
        [15] = UINT32_C(0x0000ffff),
        [21] = UINT32_C(0x3f800000),
        [22] = UINT32_C(0x3f800000),
        [24] = UINT32_C(0x3f800000),
        [25] = UINT32_C(0x3f800000),
        [26] = UINT32_C(0x3f800000),
        [27] = UINT32_C(0x3f800000),
        [28] = UINT32_C(0x3f800000),
        [35] = UINT32_C(1),
    };
    gpu_sprite_instance_t instance;
    memset(&instance, 0xa5, sizeof(instance));
    gpu_sprite_instance_init(&instance);

    CHECK(memcmp(&instance, expected_default, sizeof(expected_default)) == 0);
    CHECK(instance.effect_flags == 0);
    CHECK(instance.texture_flags == 0);
    CHECK(instance.lighting_key == GPU_SPRITE_LIGHTING_KEY_UNLIT);
    CHECK(instance.modulation[0] == 1.0f && instance.modulation[1] == 1.0f &&
          instance.modulation[2] == 1.0f && instance.modulation[3] == 1.0f);
    CHECK(instance.transform[0] == 0.0f && instance.transform[1] == 1.0f &&
          instance.transform[2] == 1.0f && instance.transform[3] == 0.0f);
    CHECK(instance.effect_color[0] == 1.0f && instance.effect_color[1] == 1.0f &&
          instance.effect_color[2] == 1.0f && instance.effect_color[3] == 1.0f);
    CHECK(instance.effect_parameters[0] == 1.0f && instance.effect_parameters[1] == 0.0f &&
          instance.effect_parameters[2] == 0.0f && instance.effect_parameters[3] == 0.0f);
    CHECK(instance.effect_time == 0 && instance.effect_phase == 0 && instance.effect_seed == 0);
    CHECK(instance.owner_depth ==
          gpu_sprite_owner_depth_pack(GPU_SPRITE_OWNER_UNSET, GPU_SPRITE_DEPTH_UNSET));
    CHECK(gpu_sprite_owner_depth_owner(instance.owner_depth) == GPU_SPRITE_OWNER_UNSET);
    CHECK(gpu_sprite_owner_depth_depth(instance.owner_depth) == GPU_SPRITE_DEPTH_UNSET);
    CHECK(instance.abi_version == GPU_SPRITE_INSTANCE_ABI_VERSION);
    CHECK(!gpu_sprite_instance_valid(&instance));

    uint32_t owner_depth = gpu_sprite_owner_depth_pack(7, 13);
    CHECK(owner_depth == UINT32_C(0x00000d07));
    CHECK(gpu_sprite_owner_depth_owner(owner_depth) == 7);
    CHECK(gpu_sprite_owner_depth_depth(owner_depth) == 13);

    instance.effect_time = 99;
    instance.owner_depth = gpu_sprite_owner_depth_pack(2, 4);
    gpu_sprite_instance_init(&instance);
    CHECK(memcmp(&instance, expected_default, sizeof(expected_default)) == 0);
    return true;
}

static bool test_effect_matrix(void) {
    static const uint32_t individual[] = {
        GPU_SPRITE_EFFECT_TINT,
        GPU_SPRITE_EFFECT_DARK,
        GPU_SPRITE_EFFECT_GRAY,
        GPU_SPRITE_EFFECT_RED,
        GPU_SPRITE_EFFECT_FOG,
        GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY,
        GPU_SPRITE_EFFECT_ROTATE,
        GPU_SPRITE_EFFECT_SCALE,
        GPU_SPRITE_EFFECT_STRETCH,
        GPU_SPRITE_EFFECT_GLOW,
        GPU_SPRITE_EFFECT_OUTLINE,
        GPU_SPRITE_EFFECT_MASK_INPUT,
        GPU_SPRITE_EFFECT_LIGHT_INPUT,
    };
    static const uint32_t combinations[] = {
        GPU_SPRITE_EFFECT_TINT | GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY,
        GPU_SPRITE_EFFECT_DARK | GPU_SPRITE_EFFECT_TINT,
        GPU_SPRITE_EFFECT_FOG | GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY,
        GPU_SPRITE_EFFECT_ROTATE | GPU_SPRITE_EFFECT_SCALE,
        GPU_SPRITE_EFFECT_STRETCH | GPU_SPRITE_EFFECT_MASK_INPUT,
        GPU_SPRITE_EFFECT_GLOW | GPU_SPRITE_EFFECT_OUTLINE | GPU_SPRITE_EFFECT_MASK_INPUT,
        GPU_SPRITE_EFFECT_LIGHT_INPUT | GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY,
    };

    gpu_sprite_instance_t instance;
    gpu_sprite_instance_init(&instance);
    instance.texture_flags = valid_texture_flags();
    CHECK(gpu_sprite_instance_valid(&instance));
    for (size_t index = 0; index < sizeof(individual) / sizeof(individual[0]); index++) {
        instance.effect_flags = individual[index];
        CHECK(gpu_sprite_instance_valid(&instance));
    }
    for (size_t index = 0; index < sizeof(combinations) / sizeof(combinations[0]); index++) {
        instance.effect_flags = combinations[index];
        CHECK(gpu_sprite_instance_valid(&instance));
    }

    instance.effect_flags = GPU_SPRITE_EFFECT_DARK | GPU_SPRITE_EFFECT_GRAY;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.effect_flags = GPU_SPRITE_EFFECT_KNOWN_MASK | (UINT32_C(1) << 31);
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.effect_flags = 0;
    instance.abi_version = GPU_SPRITE_INSTANCE_ABI_VERSION + 1U;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.abi_version = GPU_SPRITE_INSTANCE_ABI_VERSION;
    instance.lighting_key = GPU_SPRITE_LIGHTING_KEY_KNOWN_MASK + 1U;
    CHECK(!gpu_sprite_instance_valid(&instance));
    return true;
}

static bool test_texture_metadata_matrix(void) {
    gpu_sprite_instance_t instance;
    gpu_sprite_instance_init(&instance);

    instance.texture_flags = valid_texture_flags();
    CHECK(gpu_sprite_instance_valid(&instance));

    instance.texture_flags = (valid_texture_flags() & ~GPU_SPRITE_TEXTURE_ATLAS) |
                             GPU_SPRITE_TEXTURE_STANDALONE;
    CHECK(gpu_sprite_instance_valid(&instance));

    instance.texture_flags = (valid_texture_flags() & ~GPU_SPRITE_TEXTURE_STRAIGHT_ALPHA) |
                             GPU_SPRITE_TEXTURE_PREMULTIPLIED_ALPHA;
    CHECK(gpu_sprite_instance_valid(&instance));

    instance.texture_flags = valid_texture_flags() | GPU_SPRITE_TEXTURE_STANDALONE;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.texture_flags = valid_texture_flags() & ~GPU_SPRITE_TEXTURE_STRAIGHT_ALPHA;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.texture_flags = valid_texture_flags() | GPU_SPRITE_TEXTURE_PREMULTIPLIED_ALPHA;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.texture_flags = valid_texture_flags() & ~GPU_SPRITE_TEXTURE_NEAREST;
    CHECK(!gpu_sprite_instance_valid(&instance));
    instance.texture_flags = valid_texture_flags() & ~GPU_SPRITE_TEXTURE_CLAMP_EDGE;
    CHECK(!gpu_sprite_instance_valid(&instance));

    instance.texture_flags = valid_texture_flags();
    instance.owner_depth = UINT32_C(0x00010001);
    CHECK(!gpu_sprite_instance_valid(&instance));
    return true;
}

static bool test_serialized_value_is_pointer_free(void) {
    gpu_sprite_instance_t instance;
    gpu_sprite_instance_init(&instance);
    instance.texture_flags = valid_texture_flags();
    instance.effect_flags = GPU_SPRITE_EFFECT_ROTATE | GPU_SPRITE_EFFECT_SCALE;
    instance.destination[0] = 17.25f;
    instance.destination[3] = 48.0f;
    instance.uv[0] = 0.125f;
    instance.texture_metadata[2] = 0.000244140625f;
    instance.transform[0] = 0.5f;
    instance.transform[1] = 1.25f;
    instance.transform[2] = 0.75f;
    instance.effect_color[0] = 0.25f;
    instance.effect_parameters[1] = 0.5f;
    instance.effect_time = UINT32_C(1200);
    instance.effect_phase = UINT32_C(0x00008000);
    instance.effect_seed = UINT32_C(0x12345678);
    instance.owner_depth = gpu_sprite_owner_depth_pack(3, 9);
    CHECK(gpu_sprite_instance_valid(&instance));

    uint8_t bytes[GPU_SPRITE_INSTANCE_STRIDE];
    gpu_sprite_instance_t copy;
    memcpy(bytes, &instance, sizeof(bytes));
    memcpy(&copy, bytes, sizeof(copy));
    CHECK(memcmp(&instance, &copy, sizeof(instance)) == 0);
    uint32_t destination_x_bits;
    memcpy(&destination_x_bits, &copy.destination[0], sizeof(destination_x_bits));
    CHECK(destination_x_bits == float_bits(17.25f));
    CHECK(copy.effect_time == 1200 && copy.effect_phase == UINT32_C(0x00008000) &&
          copy.effect_seed == UINT32_C(0x12345678));
    CHECK(gpu_sprite_owner_depth_owner(copy.owner_depth) == 3 &&
          gpu_sprite_owner_depth_depth(copy.owner_depth) == 9);
    return true;
}

int main(void) {
    if (!test_layout() || !test_default_and_owner_depth() || !test_effect_matrix() ||
        !test_texture_metadata_matrix() || !test_serialized_value_is_pointer_free()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
