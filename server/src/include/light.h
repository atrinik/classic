/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

#ifndef LIGHT_H
#define LIGHT_H

#include <decls.h>

#include <stdint.h>
#include <toolkit/celestial_model.h>

/**
 * @file
 * Public declarations for the corresponding server module.
 */

/** Public API implemented in src/server/light.c. */

extern uint8_t light_level_from_raw(int raw_light);

/** Neutral authored light tint. */
#define LIGHT_COLOR_WHITE UINT32_C(0xffffff)

extern bool light_color_parse(const char *value, uint32_t *color);

/** Convert an authored sRGB color to normalized scene-linear Q0.16 channels. */
extern void light_color_linearize(uint32_t color, uint16_t linear[3]);

/** Resolve aggregate pre-tone Q5.11 radiance for one authorized map sample. */
extern void light_radiance_from_raw(const MapSpace *space,
                                    int raw_light,
                                    uint16_t *scalar_radiance,
                                    uint16_t radiance[3]);


/** Signed, source-anonymous coarse local light level of one cell (protocol 1085). */
extern int8_t light_local_level(mapstruct *map, int x, int y);

/** Free a map's coarse light cache, with the map. */
extern void light_local_forget(mapstruct *map);

#ifdef ATRINIK_TESTING
/** Coarse-light topology walks performed so far. */
extern uint64_t light_local_walks_for_test(void);
extern uint64_t light_local_rehashes_for_test(void);
#endif

/** The signed coarse level of a block's strongest positive and negative raw
 * contributions: each sign is quantized before they compete. */
extern int8_t light_local_level_from_extrema(int32_t positive, int32_t negative);

/** Bit position of the vertical attenuation in combined occluder bits; the
 * wire carries it as the occluder's trailing byte. */
#define LIGHT_OCCLUDER_VERTICAL_SHIFT 16
/** Horizontal face nibbles (north, east, south, west) of a server occluder. */
#define LIGHT_OCCLUDER_HORIZONTAL_SHIFT 32
/** Server-only: which cardinal faces a sent object authors, one bit each. */
#define LIGHT_OCCLUDER_AUTHORED_SHIFT 48
/** Server-only: a sent view blocker makes faces no sent object authors opaque. */
#define LIGHT_OCCLUDER_BLOCKER_FALLBACK (UINT64_C(1) << 52)
/** Server-only bits that light_occluder_finish() resolves and strips. */
#define LIGHT_OCCLUDER_INTERNAL ((UINT64_C(15) << LIGHT_OCCLUDER_AUTHORED_SHIFT) | \
                                 LIGHT_OCCLUDER_BLOCKER_FALLBACK)

/** MAP2 occluder bits of one object being sent to the viewer (protocol 1085). */
extern uint64_t light_occluder_object_bits(const object *op);

/** MAP2 occluder face bits of one cell; in_view bit i is set when the north,
 * east, south, west, upper or lower neighbor is in the viewer's view. */
extern uint16_t light_occluder_faces(mapstruct *map, int x, int y, uint8_t in_view);

/** Whether a sent object's emitter opts in to client flicker (protocol 1085). */
extern bool light_emitter_flickers(const object *op);

/** Whether MAP2 may disclose an object's emitter to a viewer (protocol 1085). */
extern bool light_emitter_disclosed(const object *viewer, const object *op);

/** Combine the occluder bits of two sent objects of one cell. */
extern uint64_t light_occluder_combine(uint64_t first, uint64_t second);

/** Resolve combined occluder bits of a cell for the wire: a face no sent
 * object authors is opaque if any sent object blocks view, as the server's
 * implicit fallback applies only when no object authors the face. */
extern uint64_t light_occluder_finish(uint64_t bits);

/** The shared celestial model's inputs for one map and gameplay hour: its
 * effective profile's light constants and phases, any lunar override applied. */
extern void celestial_light_model_inputs(const mapstruct *map,
                                         uint64_t absolute_hour,
                                         celestial_model_inputs_t *inputs);

/** Invalidate the bounded celestial field after structural/topology changes. */
extern void celestial_light_invalidate(mapstruct *map);

/** Invalidate all loaded celestial keyframes after a process-local override. */
extern void celestial_light_invalidate_all(void);

/** Publish the bounded celestial field for one absolute gameplay hour. */
extern bool celestial_light_rebuild(mapstruct *map, uint64_t absolute_hour);

/** Ensure the field used by map_get_darkness() matches the current hour/profile. */
extern void celestial_light_ensure(mapstruct *map);

/** Synchronously ensure both hourly fields (offline callers and solver oracle). */
extern bool celestial_light_keyframe_ensure(mapstruct *map, uint64_t absolute_hour);

/** Ensure the current field, promoting a prepared one at an hour boundary,
 * then queue the next hour's field of the map's stack on the budgeted solver
 * unless it is ready or queued. Called for every map a view update showed,
 * after its MAP2 publication. */
extern void celestial_light_prepare_next(mapstruct *map);

/** Advance queued cell work on the server thread within this monotonic budget. */
extern void celestial_light_process_pending(uint64_t budget_us);

/** Cancel every queued stack retaining this map before its storage is released. */
extern void celestial_light_forget(mapstruct *map);

/** Celestial structure of one cell built from the objects MAP2 sends to a
 * viewer (CLIENT_CELESTIAL.md, disclosure rule). */
typedef struct celestial_closure_accumulator {
    /** Coefficient of each face: down, north, east, south, west. */
    uint16_t coefficient[5];
    /** Faces some sent object authors, and faces with an aperture. */
    uint8_t found, aperture;
    /** A sent object blocks view: unauthored cardinal faces are opaque. */
    bool blocker;
    /** Some object of the cell was sent; a cell whose every object was
     * filtered is shown empty and stays outside the closure. */
    bool sent;
} celestial_closure_accumulator_t;

/** Start a cell's accumulator with every face open. */
extern void celestial_closure_begin(celestial_closure_accumulator_t *accumulator);

/** Add one object MAP2 sends for the cell. */
extern void celestial_closure_accumulate(celestial_closure_accumulator_t *accumulator,
                                         const object *op);

/** The cell's closure value (toolkit/celestial_closure.h): its faces from the
 * accumulated objects, and its sky cover, sky exit and continuity from the
 * map's authored structure and links. ax and ay are the cell's MAP2 window
 * position, which decides what a direction-drawn object shows. */
extern uint32_t celestial_closure_cell_value(const celestial_closure_accumulator_t *accumulator,
                                             mapstruct *map, int x, int y, const object *viewer,
                                             int ax, int ay);

/** Whether MAP2 shows a FLAG_DRAW_DIRECTION object at window position ax,
 * ay to the viewer; true for every other object. */
extern bool map2_direction_shown(const object *op, const object *viewer, int ax, int ay);

/** Update exact cell transport geometry and report a semantic change. */
extern bool celestial_light_geometry_update(mapstruct *map, int x, int y);

#ifdef ATRINIK_TESTING
/** The disclosed closure record of a map cell, zero outside the closure. */
typedef uint32_t (*celestial_reference_member_fn)(void *context, mapstruct *map, int x, int y);
/** Solve one map with the server solver on the disclosed closure: real maps,
 * links and authored cover, the faces the viewer was shown, and every cell
 * outside the closure open air under an open sky. */
extern bool celestial_light_closure_reference_for_test(mapstruct *map, uint64_t absolute_hour,
                                                       celestial_reference_member_fn member,
                                                       void *context, int32_t *values,
                                                       int32_t (*rgb)[3]);
#endif

#ifdef ATRINIK_TESTING
/** Swap the batch counter for isolated overflow tests with no pending jobs. */
extern uint64_t celestial_light_pending_batch_exchange_for_test(uint64_t batch);
extern void celestial_light_process_steps_for_test(size_t steps);
extern size_t celestial_light_pending_steps_for_test(const mapstruct *map);
extern void celestial_light_current_reservation_fail_for_test(bool fail);
extern uint64_t celestial_light_rebuilds_for_test(void);
extern size_t celestial_light_pending_memory_for_test(void);
extern void celestial_light_pending_memory_limit_for_test(size_t limit);
#endif

extern void adjust_light_source(mapstruct *map, int x, int y, int light);

extern void
adjust_light_source_color(mapstruct *map, int x, int y, int radius, uint32_t color, int direction);

extern void check_light_source_list(mapstruct *map);

extern void remove_light_source_list(mapstruct *map);

extern void recalculate_light_sources(mapstruct *map);

/** Short server-thread scopes only: balance every begin/end and finish before
 * changing map residency or releasing maps. Reads and source deltas flush the
 * pending work without ending the enclosing scope. */
extern void light_batch_begin(void);
extern void light_batch_end(void);
extern void light_batch_flush(void);

/** Publish changed effective geometry, coalescing each map within a scope. */
extern void light_geometry_changed(mapstruct *map, bool local, bool celestial);

/** Defer map-owned unlink marks across complete retirement; these scopes never
 * retain released map pointers. Reads and new source mutations force a flush.
 * retiring_all is reserved for free_all_maps(): it promises complete retirement
 * with no surviving map, lighting read or gameplay callback within the scope. */
extern void light_map_unlink_begin(bool retiring_all);
extern void light_map_unlink_end(void);
extern void light_map_unlink_flush(void);

/** Begin an unlink scope and mark affected survivors before topology changes. */
extern void light_map_prepare_unlink(mapstruct *map);

/** Flush geometry and unlink work before changing a resident emitter's radius
 * or color. Counter delta helpers cannot infer the object's previous fields. */
extern void light_source_prepare_change(void);

#ifdef ATRINIK_TESTING
/** Single-server-thread rebuild counter for deterministic teardown work tests. */
extern uint64_t light_rebuild_count_for_test(void);
/** Most recent rebuild discovery sizes, including maps without emitters. */
extern size_t light_rebuild_target_maps_for_test(void);
extern size_t light_rebuild_source_maps_for_test(void);
#endif

#endif
