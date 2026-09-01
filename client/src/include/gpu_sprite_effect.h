/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Backend-neutral GPU sprite instance ABI.
 *
 * The field order is authored once in shaders/sprite_effect_abi.inc and is
 * included by both this C declaration and the authoritative HLSL shader. A
 * sprite instance is a value object: it contains no process-local pointers,
 * handles, or addresses. Resource lifetime and CPU visibility policy stay in
 * the renderer and map painter.
 */

#ifndef GPU_SPRITE_EFFECT_H
#define GPU_SPRITE_EFFECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GPU_SPRITE_INSTANCE_ABI_VERSION UINT32_C(1)
#define GPU_SPRITE_INSTANCE_STRIDE UINT32_C(144)

/* Effect bits are intentionally stable wire-like feature bits, not C enum
 * values whose underlying width could vary between compilers. */
#define GPU_SPRITE_EFFECT_TINT (UINT32_C(1) << 0)
#define GPU_SPRITE_EFFECT_DARK (UINT32_C(1) << 1)
#define GPU_SPRITE_EFFECT_GRAY (UINT32_C(1) << 2)
#define GPU_SPRITE_EFFECT_RED (UINT32_C(1) << 3)
#define GPU_SPRITE_EFFECT_FOG (UINT32_C(1) << 4)
#define GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY (UINT32_C(1) << 5)
#define GPU_SPRITE_EFFECT_ROTATE (UINT32_C(1) << 6)
#define GPU_SPRITE_EFFECT_SCALE (UINT32_C(1) << 7)
#define GPU_SPRITE_EFFECT_STRETCH (UINT32_C(1) << 8)
#define GPU_SPRITE_EFFECT_GLOW (UINT32_C(1) << 9)
#define GPU_SPRITE_EFFECT_OUTLINE (UINT32_C(1) << 10)
#define GPU_SPRITE_EFFECT_MASK_INPUT (UINT32_C(1) << 11)
#define GPU_SPRITE_EFFECT_LIGHT_INPUT (UINT32_C(1) << 12)

#define GPU_SPRITE_EFFECT_COLOR_MODE_MASK \
    (GPU_SPRITE_EFFECT_DARK | GPU_SPRITE_EFFECT_GRAY | GPU_SPRITE_EFFECT_RED | \
     GPU_SPRITE_EFFECT_FOG)
#define GPU_SPRITE_EFFECT_KNOWN_MASK \
    (GPU_SPRITE_EFFECT_TINT | GPU_SPRITE_EFFECT_COLOR_MODE_MASK | \
     GPU_SPRITE_EFFECT_TRANSIENT_OVERLAY | GPU_SPRITE_EFFECT_ROTATE | \
     GPU_SPRITE_EFFECT_SCALE | GPU_SPRITE_EFFECT_STRETCH | GPU_SPRITE_EFFECT_GLOW | \
     GPU_SPRITE_EFFECT_OUTLINE | GPU_SPRITE_EFFECT_MASK_INPUT | \
     GPU_SPRITE_EFFECT_LIGHT_INPUT)

/* Texture flags describe the immutable binding selected by the CPU. The
 * shader never receives a texture pointer or a surface address. */
#define GPU_SPRITE_TEXTURE_ATLAS (UINT32_C(1) << 0)
#define GPU_SPRITE_TEXTURE_STANDALONE (UINT32_C(1) << 1)
#define GPU_SPRITE_TEXTURE_SOURCE_COLOR_KEY (UINT32_C(1) << 2)
#define GPU_SPRITE_TEXTURE_STRAIGHT_ALPHA (UINT32_C(1) << 3)
#define GPU_SPRITE_TEXTURE_PREMULTIPLIED_ALPHA (UINT32_C(1) << 4)
#define GPU_SPRITE_TEXTURE_NEAREST (UINT32_C(1) << 5)
#define GPU_SPRITE_TEXTURE_CLAMP_EDGE (UINT32_C(1) << 6)
#define GPU_SPRITE_TEXTURE_STORAGE_MASK \
    (GPU_SPRITE_TEXTURE_ATLAS | GPU_SPRITE_TEXTURE_STANDALONE)
#define GPU_SPRITE_TEXTURE_ALPHA_MASK \
    (GPU_SPRITE_TEXTURE_STRAIGHT_ALPHA | GPU_SPRITE_TEXTURE_PREMULTIPLIED_ALPHA)
#define GPU_SPRITE_TEXTURE_KNOWN_MASK \
    (GPU_SPRITE_TEXTURE_STORAGE_MASK | GPU_SPRITE_TEXTURE_SOURCE_COLOR_KEY | \
     GPU_SPRITE_TEXTURE_ALPHA_MASK | GPU_SPRITE_TEXTURE_NEAREST | \
     GPU_SPRITE_TEXTURE_CLAMP_EDGE)

/* The existing compact light key uses 19 value bits and one projected bit. */
#define GPU_SPRITE_LIGHTING_KEY_BITS UINT32_C(19)
#define GPU_SPRITE_LIGHTING_KEY_MASK ((UINT32_C(1) << GPU_SPRITE_LIGHTING_KEY_BITS) - 1U)
#define GPU_SPRITE_LIGHTING_KEY_KNOWN_MASK \
    ((UINT32_C(1) << (GPU_SPRITE_LIGHTING_KEY_BITS + 1U)) - 1U)
#define GPU_SPRITE_LIGHTING_KEY_DARK (GPU_SPRITE_LIGHTING_KEY_MASK - 1U)
#define GPU_SPRITE_LIGHTING_KEY_UNLIT GPU_SPRITE_LIGHTING_KEY_MASK
#define GPU_SPRITE_LIGHTING_KEY_PROJECTED (UINT32_C(1) << GPU_SPRITE_LIGHTING_KEY_BITS)

#define GPU_SPRITE_OWNER_UNSET UINT8_MAX
#define GPU_SPRITE_DEPTH_UNSET UINT8_MAX

typedef struct gpu_sprite_instance {
#define GPU_SPRITE_ABI_FLOAT4(_name) float _name[4];
#define GPU_SPRITE_ABI_UINT(_name) uint32_t _name;
#include "../../shaders/sprite_effect_abi.inc"
#undef GPU_SPRITE_ABI_UINT
#undef GPU_SPRITE_ABI_FLOAT4
} gpu_sprite_instance_t;

_Static_assert(sizeof(float) == sizeof(uint32_t), "GPU sprite scalar widths must match");
_Static_assert(offsetof(gpu_sprite_instance_t, destination) == 0, "destination ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, uv) == 16, "uv ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, modulation) == 32, "modulation ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_flags) == 48, "effect flags ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, texture_flags) == 52, "texture flags ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, lighting_key) == 56, "lighting key ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, owner_depth) == 60, "owner/depth ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, texture_metadata) == 64,
               "texture metadata ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, transform) == 80, "transform ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_color) == 96, "effect color ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_parameters) == 112,
               "effect parameters ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_time) == 128, "effect time ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_phase) == 132,
               "effect phase ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, effect_seed) == 136, "effect seed ABI offset");
_Static_assert(offsetof(gpu_sprite_instance_t, abi_version) == 140, "ABI version offset");
_Static_assert(sizeof(gpu_sprite_instance_t) == GPU_SPRITE_INSTANCE_STRIDE,
               "GPU sprite instance stride must remain 144 bytes");
_Static_assert(sizeof(gpu_sprite_instance_t) % 16 == 0,
               "GPU sprite instance must end on a 16-byte lane");

/** Pack the opaque semantic owner and map depth into the ABI word. */
static inline uint32_t gpu_sprite_owner_depth_pack(uint8_t owner, uint8_t depth) {
    return (uint32_t)owner | ((uint32_t)depth << 8U);
}

/** Read the owner byte from an ABI owner/depth word. */
static inline uint8_t gpu_sprite_owner_depth_owner(uint32_t owner_depth) {
    return (uint8_t)(owner_depth & UINT32_C(0xff));
}

/** Read the depth byte from an ABI owner/depth word. */
static inline uint8_t gpu_sprite_owner_depth_depth(uint32_t owner_depth) {
    return (uint8_t)((owner_depth >> 8U) & UINT32_C(0xff));
}

/** Return whether mutually exclusive color presentation bits are well formed. */
static inline bool gpu_sprite_effect_color_mode_valid(uint32_t flags) {
    uint32_t modes = flags & GPU_SPRITE_EFFECT_COLOR_MODE_MASK;
    return modes == 0 || (modes & (modes - 1U)) == 0;
}

/** Validate the fixed metadata portion of one instance before upload. */
static inline bool gpu_sprite_instance_valid(const gpu_sprite_instance_t *instance) {
    if (instance == NULL || instance->abi_version != GPU_SPRITE_INSTANCE_ABI_VERSION ||
        (instance->effect_flags & ~GPU_SPRITE_EFFECT_KNOWN_MASK) != 0 ||
        !gpu_sprite_effect_color_mode_valid(instance->effect_flags) ||
        (instance->texture_flags & ~GPU_SPRITE_TEXTURE_KNOWN_MASK) != 0 ||
        (instance->texture_flags & GPU_SPRITE_TEXTURE_STORAGE_MASK) == 0 ||
        (instance->texture_flags & GPU_SPRITE_TEXTURE_STORAGE_MASK) ==
            GPU_SPRITE_TEXTURE_STORAGE_MASK ||
        (instance->texture_flags & GPU_SPRITE_TEXTURE_ALPHA_MASK) == 0 ||
        (instance->texture_flags & GPU_SPRITE_TEXTURE_ALPHA_MASK) ==
            GPU_SPRITE_TEXTURE_ALPHA_MASK ||
        (instance->texture_flags & (GPU_SPRITE_TEXTURE_NEAREST |
                                    GPU_SPRITE_TEXTURE_CLAMP_EDGE)) !=
            (GPU_SPRITE_TEXTURE_NEAREST | GPU_SPRITE_TEXTURE_CLAMP_EDGE) ||
        (instance->lighting_key & ~GPU_SPRITE_LIGHTING_KEY_KNOWN_MASK) != 0 ||
        (instance->owner_depth & UINT32_C(0xffff0000)) != 0) {
        return false;
    }
    return true;
}

/** Initialize deterministic zero/default effect inputs for one instance. */
static inline void gpu_sprite_instance_init(gpu_sprite_instance_t *instance) {
    if (instance == NULL) {
        return;
    }
    memset(instance, 0, sizeof(*instance));
    instance->modulation[0] = 1.0f;
    instance->modulation[1] = 1.0f;
    instance->modulation[2] = 1.0f;
    instance->modulation[3] = 1.0f;
    instance->transform[1] = 1.0f;
    instance->transform[2] = 1.0f;
    instance->effect_color[0] = 1.0f;
    instance->effect_color[1] = 1.0f;
    instance->effect_color[2] = 1.0f;
    instance->effect_color[3] = 1.0f;
    instance->effect_parameters[0] = 1.0f;
    instance->lighting_key = GPU_SPRITE_LIGHTING_KEY_UNLIT;
    instance->owner_depth =
        gpu_sprite_owner_depth_pack(GPU_SPRITE_OWNER_UNSET, GPU_SPRITE_DEPTH_UNSET);
    instance->abi_version = GPU_SPRITE_INSTANCE_ABI_VERSION;
}

#endif
