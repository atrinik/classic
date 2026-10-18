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
 * Celestial ray transport and diffuse relaxation over an abstract lattice,
 * shared by the server (reading its maps) and the client (reading its
 * celestial closure), so both solve bit-identical fields from the same
 * structure.
 */

#ifndef TOOLKIT_CELESTIAL_SOLVER_H
#define TOOLKIT_CELESTIAL_SOLVER_H

#include "celestial_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Most boundary events one traversal may take, as the server's spatial
 * traversal bounds them. */
#define CELESTIAL_SOLVER_MAX_EVENTS 12288
/** Vertical extent of a solved stack and of a ray's vertical travel. */
#define CELESTIAL_SOLVER_LEVELS 13
/** Crossings one backtrace records: its horizontal reach plus every level. */
#define CELESTIAL_SOLVER_RAY_EVENTS (CELESTIAL_MODEL_DIRECT_REACH + CELESTIAL_SOLVER_LEVELS)

/**
 * One lattice cell: an opaque map handle and coordinates local to it; z is
 * relative to the traversal's origin. A NULL map is an unresolved cell.
 */
typedef struct celestial_solver_cell {
    void *map;
    int x, y, z;
} celestial_solver_cell_t;

/** A face between two cells, with the unit normal from 'from' to 'to'. */
typedef struct celestial_solver_face {
    celestial_solver_cell_t from, to;
    int8_t normal[3];
} celestial_solver_face_t;

/** One boundary event of a traversal, with every cell and face a tie
 * touches. */
typedef struct celestial_solver_event {
    celestial_solver_cell_t cells[7];
    size_t cell_count;
    celestial_solver_face_t faces[12];
    size_t face_count;
    celestial_solver_cell_t destination;
    /** Crossed axes: bit 0 x, 1 y, 2 z. */
    unsigned axes;
    /** Every touched cell resolved. */
    bool valid;
    bool terminal;
} celestial_solver_event_t;

/** An exact integer traversal along a direction or to a segment endpoint. */
typedef struct celestial_solver_cursor {
    void *origin;
    int position[3], direction[3], endpoint[3];
    uint32_t crossed[3], events, limit;
    bool segment, failed;
} celestial_solver_cursor_t;

/** The structure a lattice exposes to the solver. */
typedef struct celestial_solver_ops {
    /** Resolve a point given in the origin map's coordinates (z relative to
     * it). On failure the cell is treated as unresolved. */
    bool (*resolve)(void *context, void *origin, int x, int y, int z,
                    celestial_solver_cell_t *cell);
    /** Report a consulted map; returning false fails the operation closed.
     * May be NULL. Never called with a NULL map. */
    bool (*observe)(void *context, void *map);
    /** The map's width and height. */
    void (*dimensions)(void *context, void *map, int *width, int *height);
    /** Whether an upward face, as met walking from the receiver, leaves for
     * the sky. */
    bool (*sky)(void *context, const celestial_solver_face_t *face);
    /** Whether light may cross the face between its two cells. */
    bool (*continuous)(void *context, const celestial_solver_face_t *face);
    /** Light coefficient 0..256 of light crossing the face from 'from' to
     * 'to'; *aperture (when not NULL) reports a door, gate or authored
     * transmission on it. */
    uint16_t (*light)(void *context, const celestial_solver_face_t *face, bool *aperture);
    /** Charge transient solve memory; false declines it. May be NULL to
     * allow every allocation. */
    bool (*reserve)(void *context, size_t bytes);
    /** Return memory charged by reserve; called only for bytes a reserve
     * call accepted, never when reserve is NULL. May be NULL. */
    void (*release)(void *context, size_t bytes);
} celestial_solver_ops_t;

/**
 * A celestial lattice: its operations and their context. Both are owned by
 * the caller and must outlive every solver call that receives the lattice
 * and every ray or cursor initialized from it. The solver calls the
 * operations synchronously on the calling thread, never retains them past a
 * call, and does not reenter a solver entry point from an operation. Nothing
 * in the solver is shared between calls, so lattices may be solved on
 * different threads concurrently as long as each operation set and context
 * is used by one thread at a time; operations that mutate their context
 * (observe, reserve, release) need the caller's synchronization otherwise.
 */
typedef struct celestial_solver_lattice {
    const celestial_solver_ops_t *ops;
    void *context;
} celestial_solver_lattice_t;

/** One backtrace from a receiver towards a source, recording coefficients
 * rather than gains: radiance rounds at each crossing in source order. */
typedef struct celestial_solver_ray {
    celestial_solver_cursor_t cursor;
    uint16_t coefficients[CELESTIAL_SOLVER_RAY_EVENTS];
    bool apertures[CELESTIAL_SOLVER_RAY_EVENTS];
    unsigned count, horizontal;
    bool done, sky;
} celestial_solver_ray_t;

bool celestial_solver_direction_init(const celestial_solver_lattice_t *lattice,
                                     celestial_solver_cursor_t *cursor, void *origin, int x,
                                     int y, int dx, int dy, int dz, unsigned max_events);
bool celestial_solver_segment_init(const celestial_solver_lattice_t *lattice,
                                   celestial_solver_cursor_t *cursor, void *origin, int x, int y,
                                   int dx, int dy, int dz);
/** 1 for one boundary event (even unresolved), 0 when finished, -1 when
 * invalid or over its bound. */
int celestial_solver_next(const celestial_solver_lattice_t *lattice,
                          celestial_solver_cursor_t *cursor, celestial_solver_event_t *event);

/** Integer square root, rounded down. */
uint32_t celestial_solver_sqrt(uint64_t value);

/** Start a backtrace from a map cell towards a source at the given direction
 * (CELESTIAL_MODEL_NORTH..NORTHWEST) and Q15 elevation. */
void celestial_solver_ray_init(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray, void *map, int x, int y,
                               uint8_t direction, int32_t elevation);
/** As celestial_solver_ray_init(), from a cell of the map's scratch grid,
 * which extends CELESTIAL_MODEL_SPILL_PASSES cells past each edge. */
void celestial_solver_halo_ray(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray, void *map, int x, int y,
                               uint8_t direction, int32_t elevation);
/** Take one bounded crossing. */
void celestial_solver_ray_step(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray);
int32_t celestial_solver_ray_value(const celestial_solver_ray_t *ray, int32_t source,
                                   int32_t *aperture);
/** Coefficient of diffuse spill into scratch cell (x, y) from its neighbor
 * (x + dx, y + dy). */
uint16_t celestial_solver_spill(const celestial_solver_lattice_t *lattice, void *map, int x,
                                int y, int dx, int dy);
int celestial_solver_scratch_width(const celestial_solver_lattice_t *lattice, void *map);
int celestial_solver_scratch_height(const celestial_solver_lattice_t *lattice, void *map);

/**
 * Solve one map's celestial field under the given model: sky exposure,
 * sun and moon transport, diffuse relaxation and publish, in the server's
 * order. Writes width × height values and RGB triples, row-major.
 * Returns false only when an allocation fails; the outputs are then
 * unspecified.
 */
bool celestial_solver_solve(const celestial_solver_lattice_t *lattice, void *map,
                            const celestial_model_t *model, int32_t *values,
                            int32_t (*rgb)[3]);

#endif
