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
    /** Level within the evaluated stack. */
    int z;
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
 * east, south, west, upward and downward faces, and whether the cell blocks
 * view or is a roof surface, whose slab an upward step ending on it skips.
 * Floor and door bits matter only to other consumers. */
#define CLIENT_LIGHTING_OCCLUDER_FACES (UINT16_C(0x3f) << MAP2_OCCLUDER_FACE_SHIFT)
#define CLIENT_LIGHTING_OCCLUDER_BITS                                                      \
    (CLIENT_LIGHTING_OCCLUDER_FACES | MAP2_OCCLUDER_BLOCKSVIEW | MAP2_OCCLUDER_ROOF)

/** A cell's occluder for evaluation: the bits it reads low, the horizontal
 * face attenuation nibbles (north, east, south, west) from bit 16 and the
 * vertical attenuation of the downward face from bit 32. It holds exactly the
 * inputs evaluation reads, so equal words light equally. */
static inline uint64_t client_lighting_occluder(uint16_t bits, uint16_t horizontal,
                                                uint8_t vertical) {
    return (uint64_t)(bits & CLIENT_LIGHTING_OCCLUDER_BITS) | (uint64_t)horizontal << 16 |
           (uint64_t)(vertical & MAP2_OCCLUDER_VERTICAL_MASK) << 32;
}

/** Most emitters one evaluation composes: well above any real scene, it
 * bounds the work of one synchronous rebuild (and keeps every grouped sum
 * within 64 bits). A stack sending more composes no emitter field and falls
 * back to its coarse levels. */
#define CLIENT_LIGHTING_EMITTERS_MAX ((size_t)2048)

/**
 * Evaluate the local light of one level of a stack of levels, each a
 * width-by-height grid, as the server's 3D transport does: every emitter of
 * every level reaches the target level within its profile, through the
 * occluders of the cells between. Within CLIENT_LIGHTING_EMITTERS_MAX
 * every emitter in the stack is evaluated and none is dropped; a larger set
 * is not evaluated at all (see count).
 * @param occluders client_lighting_occluder() words, level by level, each in
 * row-major order (levels * width * height), or NULL for open geometry.
 * Level z + 1 lies above z.
 * @param emitters Emitters with their level in z: count elements, mandatory
 * when count is positive (an unchecked precondition). An emitter outside
 * the stack (z outside 0..levels-1, or x, y outside the grid) is ignored.
 * @param count Emitters given; above CLIENT_LIGHTING_EMITTERS_MAX none are
 * evaluated and every cell is left zero (the coarse levels then apply).
 * @param cells Output of width * height cells of the target level,
 * mandatory for a positive grid (an unchecked precondition).
 */
void client_lighting_evaluate_stack(int levels,
                                    int target,
                                    int width,
                                    int height,
                                    const uint64_t *occluders,
                                    const client_lighting_emitter_t *emitters,
                                    size_t count,
                                    client_lighting_cell_t *cells);

/** client_lighting_evaluate_stack() of a single level; emitters' z is 0. */
void client_lighting_evaluate(int width,
                              int height,
                              const uint64_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells);

/**
 * Presentation flicker gain in Q0.8 (256 is steady) for a monotonic time in
 * milliseconds. Deterministic and bounded to 205..256, so flicker only dims.
 */
uint16_t client_lighting_flicker_gain(uint64_t time_ms);

/** Combine the client's emitter scalar with the server's signed coarse level.
 * Any int32_t emitter value is accepted: comparisons are made in 64 bits. */
int32_t client_lighting_compose(int32_t emitter_raw, int8_t level);

#endif
