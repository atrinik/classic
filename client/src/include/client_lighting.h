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
} client_lighting_cell_t;

/**
 * Evaluate emitters into a width-by-height grid.
 * Every emitter is evaluated; none is dropped for capacity.
 * @param occluders MAP2 occluder bits per cell in row-major order, or NULL
 * for open geometry.
 * @param cells Output of width * height cells.
 */
void client_lighting_evaluate(int width,
                              int height,
                              const uint16_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells);

/** Combine the client's emitter scalar with the server's signed coarse level. */
int32_t client_lighting_compose(int32_t emitter_raw, int8_t level);

#endif
