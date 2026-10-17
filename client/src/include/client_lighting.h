/*************************************************************************
 * Atrinik client protocol-1085 local light evaluation.                  *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Evaluate the local light of visible emitters on the client and combine it
 * with the server's coarse level, following server/doc/CLIENT_LIGHTING.md.
 * This is the CPU reference for the GPU emitter stage.
 */

#ifndef CLIENT_LIGHTING_H
#define CLIENT_LIGHTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <toolkit/socket.h>

/** One visible emitter at a grid cell. */
typedef struct client_lighting_emitter {
    int x;
    int y;
    int8_t radius;
    /** MAP2 emitter flags; MAP2_EMITTER_FLICKER opts in to presentation flicker. */
    uint8_t flags;
    uint32_t color;
} client_lighting_emitter_t;

/**
 * Local light of one grid cell in signed raw light units. Each channel is the
 * scalar plus that channel's hue delta and may be negative; clamp only after
 * adding base radiance.
 */
typedef struct client_lighting_cell {
    int32_t scalar;
    int32_t rgb[3];
    /** The flickering emitters' contribution to the grouped scalar and rgb:
     * the field minus the field of the same tiles without them. */
    int32_t flicker_scalar;
    int32_t flicker_rgb[3];
} client_lighting_cell_t;

/** The MAP2 occluder bits local light evaluation reads: the closed north,
 * east, south and west faces. Geometry, roof, door and up/down face bits
 * matter only to other consumers. */
#define CLIENT_LIGHTING_OCCLUDER_FACES (UINT16_C(0x0f) << MAP2_OCCLUDER_FACE_SHIFT)

/** A cell's occluder for evaluation: the closed horizontal faces low, the
 * horizontal face attenuation nibbles (north, east, south, west) high. It
 * holds exactly the inputs evaluation reads, so equal words light equally. */
static inline uint32_t client_lighting_occluder(uint16_t bits, uint16_t horizontal) {
    return (uint32_t)(bits & CLIENT_LIGHTING_OCCLUDER_FACES) | (uint32_t)horizontal << 16;
}

/**
 * Evaluate emitters into a width-by-height grid.
 * Every emitter is evaluated; none is dropped for capacity.
 * @param occluders client_lighting_occluder() words per cell in row-major
 * order, or NULL for open geometry.
 * @param cells Output of width * height cells.
 */
void client_lighting_evaluate(int width,
                              int height,
                              const uint32_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells);

/**
 * Presentation flicker gain in Q0.8 (256 is steady) for a monotonic time in
 * milliseconds. Deterministic and bounded to 205..256, so flicker only dims.
 */
uint16_t client_lighting_flicker_gain(uint64_t time_ms);

/** Combine the client's emitter scalar with the server's signed coarse level. */
int32_t client_lighting_compose(int32_t emitter_raw, int8_t level);

#endif
