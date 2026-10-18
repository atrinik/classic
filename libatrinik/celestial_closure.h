/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * The celestial closure of a viewer (server/doc/CLIENT_CELESTIAL.md): the
 * CELESTIAL_STRUCTURE codec, and the lattice the shared solver reads to
 * solve the closure's cells, with every cell outside it open air.
 */

#ifndef TOOLKIT_CELESTIAL_CLOSURE_H
#define TOOLKIT_CELESTIAL_CLOSURE_H

#include "celestial_solver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Viewer-relative depths a closure covers. */
#define CELESTIAL_CLOSURE_DEPTH_MAX 6
#define CELESTIAL_CLOSURE_DEPTHS (2 * CELESTIAL_CLOSURE_DEPTH_MAX + 1)

/**
 * @defgroup CELESTIAL_CLOSURE_CELL Closure cell value bits
 * Faces are indexed 0 down, 1 north, 2 east, 3 south, 4 west.
 *@{*/
/** Transmission class of face i, two bits: 0 opaque, 1 glass, 2 grate, 3 open. */
#define CELESTIAL_CLOSURE_CLASS_SHIFT(face) (2 * (face))
/** Door, gate or authored transmission on face i. */
#define CELESTIAL_CLOSURE_APERTURE(face) (UINT32_C(1) << (10 + (face)))
/** Set on every cell of the closure; a zero value is outside it. */
#define CELESTIAL_CLOSURE_IN UINT32_C(0x8000)
/** The virtual cover above the cell lets sky light through. */
#define CELESTIAL_CLOSURE_COVER_OPEN UINT32_C(0x10000)
/** No cell lies above, and an upward exit reaches the sky. */
#define CELESTIAL_CLOSURE_SKY_EXIT UINT32_C(0x20000)
/** Continuity of the north, east, south, west, up and down faces. */
#define CELESTIAL_CLOSURE_CONTINUOUS_NORTH UINT32_C(0x40000)
#define CELESTIAL_CLOSURE_CONTINUOUS_EAST UINT32_C(0x80000)
#define CELESTIAL_CLOSURE_CONTINUOUS_SOUTH UINT32_C(0x100000)
#define CELESTIAL_CLOSURE_CONTINUOUS_WEST UINT32_C(0x200000)
#define CELESTIAL_CLOSURE_CONTINUOUS_UP UINT32_C(0x400000)
#define CELESTIAL_CLOSURE_CONTINUOUS_DOWN UINT32_C(0x800000)
/** Transmission class of the static floor or roof of the cell above, two
 * bits, read only while that cell is outside the closure; doors and gates
 * count as closed. */
#define CELESTIAL_CLOSURE_OVERHEAD_SHIFT 24
/** That face carries a door, gate or authored transmission. */
#define CELESTIAL_CLOSURE_OVERHEAD_APERTURE UINT32_C(0x4000000)
/** Solve the cell exactly as one outside the closure (open air) yet still
 * report it, as a closure too large to send stands in for its cells; valid
 * only as CELESTIAL_CLOSURE_OUTSIDE_RECORD. */
#define CELESTIAL_CLOSURE_OUTSIDE UINT32_C(0x8000000)
#define CELESTIAL_CLOSURE_OUTSIDE_RECORD (CELESTIAL_CLOSURE_IN | CELESTIAL_CLOSURE_OUTSIDE)
#define CELESTIAL_CLOSURE_VALUE_MASK UINT32_C(0xfffffff)
/*@}*/

/** One map's part of the closure at one depth: the bounding rectangle of its
 * closure cells within the MAP2 window. */
typedef struct celestial_closure_fragment {
    int8_t depth;
    uint8_t x, y, width, height;
    /** Profile slot of the fragment's map, or CELESTIAL_PROTOCOL_SLOT_NONE. */
    uint8_t slot;
    /** Row-major 27-bit cell values; zero for a cell outside the closure. */
    uint32_t *values;
} celestial_closure_fragment_t;

/** A decoded closure, with the lookup the solver's lattice uses. */
typedef struct celestial_closure {
    uint32_t generation;
    int window_width, window_height;
    size_t count;
    celestial_closure_fragment_t *fragments;
    /** Per depth, the fragment index + 1 of each window cell, or 0. */
    uint16_t *index;
} celestial_closure_t;

/** Free a closure's storage and reset it to empty. */
void celestial_closure_free(celestial_closure_t *closure);

/**
 * Build a closure from fragments, taking ownership of the heap-allocated
 * fragment array and of each fragment's value array. *closure is
 * overwritten, never freed: pass an empty one, freeing a populated closure
 * first.
 * @return False, freeing everything, for any fragment the codec would reject.
 */
bool celestial_closure_build(celestial_closure_t *closure, uint32_t generation, int window_width,
                             int window_height, celestial_closure_fragment_t *fragments,
                             size_t count);

/**
 * Encode CELESTIAL_STRUCTURE.
 * @return False, writing nothing, when the closure does not fit in one
 * packet payload, the packet already has a writer error, or the writer
 * fails part way (its limit or an allocation).
 */
bool celestial_closure_write(struct packet_struct *packet, const celestial_closure_t *closure);

/** Encoded size of the closure in bytes. */
size_t celestial_closure_size(const celestial_closure_t *closure);

/**
 * Decode CELESTIAL_STRUCTURE for a window of the given size.
 * @return False, leaving *closure empty, for truncation, trailing bytes, a
 * run of length 0, a run or rectangle exceeding its bounds, overlap, or a
 * value outside the closure value rules. *closure is overwritten, never
 * freed: pass an empty one, freeing a populated closure first.
 */
bool celestial_closure_read(const uint8_t *data, size_t len, int window_width, int window_height,
                            celestial_closure_t *closure);

/** Whether a decoded value is valid for a closure cell (or zero). */
bool celestial_closure_value_valid(uint32_t value);

/** The value of a window cell, zero when it is outside the closure. */
uint32_t celestial_closure_value(const celestial_closure_t *closure, int depth, int x, int y);

/**
 * Solve one fragment's cells under a model with the shared solver, every
 * cell outside the closure open air with an open sky exit. Writes
 * width × height values and RGB triples in the fragment's row-major order;
 * cells outside the closure are written as zero.
 * @return False only when an allocation fails.
 */
bool celestial_closure_solve(const celestial_closure_t *closure, size_t fragment,
                             const celestial_model_t *model, int32_t *values, int32_t (*rgb)[3]);

#endif
