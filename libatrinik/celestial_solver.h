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
/** Largest origin coordinate magnitude a cursor accepts, so every step and
 * endpoint stays inside int; larger origins fail initialization. */
#define CELESTIAL_SOLVER_COORDINATE_MAX (1 << 24)
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

/** Largest map width or height a lattice may report: the wire's 8-bit
 * closure window, and far above the server's 64-cell maps. */
#define CELESTIAL_SOLVER_DIMENSION_MAX 255

/*
 * Arguments: unless an entry point says otherwise, every pointer it takes
 * (lattice, its ops, cursor, ray, event, model and output buffers) is
 * mandatory and must not be NULL, and the ops must set resolve, dimensions,
 * sky, continuous and light; observe, reserve and release are optional.
 * Entry points validate values (sizes, directions, coordinates), not these
 * preconditions.
 */

/** The structure a lattice exposes to the solver. */
typedef struct celestial_solver_ops {
    /** Resolve a point given in the origin map's coordinates (z relative to
     * it). On failure the cell is treated as unresolved. */
    bool (*resolve)(void *context, void *origin, int x, int y, int z,
                    celestial_solver_cell_t *cell);
    /** Report a consulted map; returning false fails the operation closed.
     * May be NULL. Never called with a NULL map. */
    bool (*observe)(void *context, void *map);
    /** The map's width and height, each 1..CELESTIAL_SOLVER_DIMENSION_MAX;
     * the solver rejects a lattice reporting anything else. */
    void (*dimensions)(void *context, void *map, int *width, int *height);
    /** Whether an upward face, as met walking from the receiver, leaves for
     * the sky. Its 'from' cell is always resolved; its 'to' cell may be
     * unresolved (map NULL): that is how a ray reaches the sky above the
     * topmost level, and sky must accept it. */
    bool (*sky)(void *context, const celestial_solver_face_t *face);
    /** Whether light may cross the face between its two cells. Called only
     * with both cells resolved: a face with an unresolved cell ends the ray,
     * unless another face of the same crossing leaves for the sky, which
     * makes it open sky. */
    bool (*continuous)(void *context, const celestial_solver_face_t *face);
    /** Light coefficient 0..256 of light crossing the face from 'from' to
     * 'to', both resolved, for a face continuous accepted; *aperture (when
     * not NULL) reports a door, gate or authored transmission on it. */
    uint16_t (*light)(void *context, const celestial_solver_face_t *face, bool *aperture);
    /** Charge the optional spill table a solve may build; false declines it
     * and the solve evaluates spill coefficients again on each pass. May be
     * NULL to allow it. The solver's fixed working fields, six int32 values
     * per scratch cell ((width + 8) x (height + 8)), and the relaxation
     * buffer are not charged: bound them through the dimensions the lattice
     * reports, as the server reserves them before a rebuild. */
    bool (*reserve)(void *context, size_t bytes);
    /** Return memory charged by reserve; called only for bytes a reserve
     * call accepted. The two hooks work as a pair: with either NULL, neither
     * is called. */
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
 * (CELESTIAL_MODEL_NORTH..NORTHWEST) and Q15 elevation, 1..32768; any other
 * elevation, or any other direction, leaves the ray done. */
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
/** Scratch grid size of a map, or 0 when its dimensions are invalid. */
int celestial_solver_scratch_width(const celestial_solver_lattice_t *lattice, void *map);
int celestial_solver_scratch_height(const celestial_solver_lattice_t *lattice, void *map);

/**
 * Solve one map's celestial field under the given model: sky exposure,
 * sun and moon transport, diffuse relaxation and publish, in the server's
 * order. Writes width × height values and RGB triples, row-major.
 * The lattice, its ops with resolve, dimensions, sky, continuous and light
 * set, the model and both output buffers are mandatory and must not be NULL;
 * observe, reserve and release are optional. The model must come from
 * celestial_model_evaluate() of valid inputs, such as a decoded CELESTIAL
 * slot's: its source values are then bounded so the publish's sums cannot
 * overflow, which a handcrafted model is not checked for.
 * Returns false only when the lattice reports invalid dimensions or an
 * allocation fails; the outputs are then unspecified.
 */
bool celestial_solver_solve(const celestial_solver_lattice_t *lattice, void *map,
                            const celestial_model_t *model, int32_t *values,
                            int32_t (*rgb)[3]);

/** A celestial_solver_solve() run in bounded steps (opaque). */
typedef struct celestial_solver_job celestial_solver_job_t;

/** Progress of a stepped solve. */
typedef enum celestial_solver_status {
    /** More steps remain. */
    CELESTIAL_SOLVER_PENDING,
    /** The outputs are written. */
    CELESTIAL_SOLVER_DONE,
    /** An allocation failed; the outputs are unspecified. */
    CELESTIAL_SOLVER_FAILED,
} celestial_solver_status_t;

/**
 * Start a stepped solve with celestial_solver_solve()'s inputs and result.
 * The job keeps copies of the lattice handle and the model, but the
 * lattice's context, the map and both outputs must outlive it, and the
 * lattice must answer the same until the job is freed.
 * @return NULL when the lattice reports invalid dimensions or an
 * allocation fails.
 */
celestial_solver_job_t *celestial_solver_job_create(const celestial_solver_lattice_t *lattice,
                                                    void *map, const celestial_model_t *model,
                                                    int32_t *values, int32_t (*rgb)[3]);

/**
 * Run one step: one scratch row of the sky, sun or moon rays, of the spill
 * table, of one diffuse relaxation pass, or of the publish, so the work per
 * call is bounded by one row of the window. Steps after DONE return DONE
 * again.
 */
celestial_solver_status_t celestial_solver_job_step(celestial_solver_job_t *job);

/** Free a job at any step; NULL is ignored. */
void celestial_solver_job_free(celestial_solver_job_t *job);

#endif
