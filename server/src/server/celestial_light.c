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

/** @file Bounded server-side celestial radiance field. */

#include <global.h>

#include <celestial_lunar.h>
#include <celestial_dependencies.h>
#include <celestial_override.h>
#include <celestial_structure.h>
#include <initialization.h>
#include <light.h>
#include <map.h>
#include <object.h>
#include <plugin.h>
#include <region.h>
#include <server_main.h>
#include <spatial.h>
#include <tod.h>
#include <toolkit/celestial_closure.h>
#include <toolkit/celestial_model.h>
#include <toolkit/celestial_solver.h>
#include <toolkit/datetime.h>

#include <stdint.h>

#define CELESTIAL_SCALE CELESTIAL_MODEL_SCALE
#define CELESTIAL_DIRECT_REACH CELESTIAL_MODEL_DIRECT_REACH
#define CELESTIAL_SPILL_PASSES CELESTIAL_MODEL_SPILL_PASSES

/* All solve allocations, including dependency snapshots, share one ceiling. */
#define CELESTIAL_PENDING_MEMORY_MAX ((size_t)1923584)
/* A synchronous rebuild's reservation: 28 bytes per scratch cell for the
 * solver's working fields, and 16 per map cell for its value and RGB
 * outputs. Asynchronous admission leaves at least this much, plus dependency
 * bookkeeping, for the largest supported map. */
#define CELESTIAL_REBUILD_BYTES(width, height)                                             \
    ((size_t)28 * ((width) + 2 * CELESTIAL_MODEL_SPILL_PASSES) *                          \
         ((height) + 2 * CELESTIAL_MODEL_SPILL_PASSES) +                                  \
     (size_t)16 * (width) * (height))
#define CELESTIAL_CURRENT_SCRATCH_MAX ((size_t)61 * 64 * 64)
_Static_assert(CELESTIAL_CURRENT_SCRATCH_MAX >= CELESTIAL_REBUILD_BYTES(64, 64) + 39168,
               "the rebuild headroom covers the largest rebuild and its bookkeeping");
static size_t pending_memory, pending_memory_limit = CELESTIAL_PENDING_MEMORY_MAX;
static celestial_dependencies *active_dependencies;

static void celestial_spatial_observe(const mapstruct *map, void *context) {
    if (map != NULL) (void)celestial_dependencies_observe(context, map);
}

static bool celestial_observe(const mapstruct *map) {
    return map == NULL || active_dependencies == NULL ||
           celestial_dependencies_observe(active_dependencies, map);
}

#ifdef ATRINIK_TESTING
static uint64_t rebuild_count;
static bool current_reservation_fail;
void celestial_light_current_reservation_fail_for_test(bool fail) {
    current_reservation_fail = fail;
}
uint64_t celestial_light_rebuilds_for_test(void) {
    return rebuild_count;
}
#endif

/* Sources and transport arithmetic are libatrinik's, shared with clients that
 * compute celestial light themselves, so both produce identical fields. */
typedef celestial_model_t celestial_model;

void celestial_light_model_inputs(const mapstruct *map,
                                  uint64_t absolute_hour,
                                  celestial_model_inputs_t *inputs) {
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t phases;
    region_celestial_phases(profile, absolute_hour, &phases);
    (void)celestial_override_apply(profile->lunar_period, &phases.lunar);
    memset(inputs, 0, sizeof(*inputs));
    memcpy(inputs->day_linear, profile->day_linear, sizeof(inputs->day_linear));
    memcpy(inputs->night_linear, profile->night_linear, sizeof(inputs->night_linear));
    inputs->day_brightness = profile->day_brightness;
    inputs->night_brightness = profile->night_brightness;
    memcpy(inputs->moon_linear, profile->moon_linear, sizeof(inputs->moon_linear));
    inputs->moon_max = profile->moon_max;
    memcpy(inputs->starlight_linear, profile->starlight_linear, sizeof(inputs->starlight_linear));
    inputs->starlight_strength = profile->starlight_strength;
    inputs->lunar_period = profile->lunar_period;
    inputs->solar_hour = phases.solar;
    inputs->season_phase = phases.season;
    inputs->lunar_age = phases.lunar;
}

static void celestial_model_for_map(const mapstruct *map,
                                    uint64_t absolute_hour,
                                    celestial_model *model) {
    celestial_model_inputs_t inputs;
    celestial_light_model_inputs(map, absolute_hour, &inputs);
    celestial_model_evaluate(&inputs, model);
}

static uint16_t transmission_value(const char *value) {
    celestial_transmission_t transmission = celestial_structure_transmission(value);
    return transmission == CELESTIAL_TRANSMISSION_INVALID
               ? CELESTIAL_TRANSMISSION_OPAQUE
               : (uint16_t)transmission;
}

static uint16_t object_edge_coefficient(const object *op, uint8_t face) {
    if ((celestial_structure_faces(op) & face) == 0) {
        return CELESTIAL_SCALE;
    }

    if (op->type == DOOR || op->type == GATE) {
        bool closed = op->type == DOOR ? QUERY_FLAG(op, FLAG_DOOR_CLOSED)
                                       : QUERY_FLAG(op, FLAG_NO_PASS);
        return transmission_value(object_get_value(
            op, closed ? "celestial_transmission_closed" : "celestial_transmission_open"));
    }

    const char *transmission = object_get_value(op, "celestial_transmission");
    if (transmission != NULL) {
        return transmission_value(transmission);
    }
    return CELESTIAL_TRANSMISSION_OPAQUE;
}

static uint16_t edge_coefficient(const mapstruct *map, int x, int y, uint8_t face, bool *aperture) {
    const MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    uint16_t coefficient = CELESTIAL_SCALE;
    bool found = false;

    for (const object *op = space->first; op != NULL; op = op->above) {
        if ((celestial_structure_faces(op) & face) == 0) {
            continue;
        }
        found = true;
        if (aperture != NULL &&
            (op->type == DOOR || op->type == GATE ||
             object_get_value(op, "celestial_transmission") != NULL)) {
            *aperture = true;
        }
        coefficient = MIN(coefficient, object_edge_coefficient(op, face));
    }
    if (!found && face != CELESTIAL_FACE_DOWN &&
        (space->flags & P_BLOCKSVIEW) != 0) {
        coefficient = CELESTIAL_TRANSMISSION_OPAQUE;
    }
    return coefficient;
}

bool celestial_light_geometry_update(mapstruct *map, int x, int y) {
    if (map->celestial_schema != 1) {
        return false;
    }
    MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    uint16_t signature = UINT16_C(1) << 15;
    for (size_t i = 0; i < 5; i++) {
        bool aperture = false;
        uint16_t coefficient = edge_coefficient(map, x, y, (uint8_t)(1U << i), &aperture);
        uint16_t encoded;
        switch (coefficient) {
        case CELESTIAL_TRANSMISSION_OPAQUE: encoded = 0; break;
        case CELESTIAL_TRANSMISSION_GLASS: encoded = 1; break;
        case CELESTIAL_TRANSMISSION_GRATE: encoded = 2; break;
        case CELESTIAL_TRANSMISSION_OPEN: encoded = 3; break;
        default: HARD_ASSERT(false); encoded = 0; break;
        }
        signature |= encoded << (2 * i);
        if (aperture) {
            signature |= UINT16_C(1) << (10 + i);
        }
    }
    /* Unknown cells have the semantic open/no-aperture baseline. */
    uint16_t previous = space->celestial_geometry != 0
                            ? space->celestial_geometry : UINT16_C(0x83ff);
    bool changed = signature != previous;
    space->celestial_geometry = signature;
    return changed;
}

static bool celestial_sky_exit(const spatial_cell *cell) {
    if (cell->map == NULL || cell->map->celestial_sky_above == CELESTIAL_SKY_LINKED)
        return false;
    /* This checks sealed-cap openings/covered exceptions only after a real
     * upward exit. It cannot skip any intervening physical face. */
    return celestial_structure_cell_exposed(cell->map, cell->x, cell->y);
}

static bool celestial_face_continuous(const spatial_face *face) {
    if (face->from.map == face->to.map) return true;
    if (face->from.map == NULL || face->to.map == NULL ||
        face->from.map->celestial_schema != 1 || face->to.map->celestial_schema != 1) return false;
    int tile = face->normal[2] > 0 ? TILED_UP : face->normal[2] < 0 ? TILED_DOWN :
               face->normal[0] > 0 ? TILED_EAST : face->normal[0] < 0 ? TILED_WEST :
               face->normal[1] > 0 ? TILED_SOUTH : TILED_NORTH;
    static const int opposite[TILED_NUM] = {
        TILED_SOUTH, TILED_WEST, TILED_NORTH, TILED_EAST,
        TILED_SOUTHWEST, TILED_NORTHWEST, TILED_NORTHEAST, TILED_SOUTHEAST,
        TILED_DOWN, TILED_UP};
    const mapstruct *from = face->from.map, *to = face->to.map;
    return from->tile_map[tile] == to && to->tile_map[opposite[tile]] == from &&
           from->celestial_boundary[tile] == CELESTIAL_BOUNDARY_CONTINUOUS &&
           to->celestial_boundary[opposite[tile]] == CELESTIAL_BOUNDARY_CONTINUOUS &&
           memcmp(region_celestial_for_map(from)->digest,
                  region_celestial_for_map(to)->digest, 32) == 0;
}

void celestial_closure_begin(celestial_closure_accumulator_t *accumulator) {
    memset(accumulator, 0, sizeof(*accumulator));
    for (size_t face = 0; face < arraysize(accumulator->coefficient); face++) {
        accumulator->coefficient[face] = CELESTIAL_SCALE;
    }
}

void celestial_closure_accumulate(celestial_closure_accumulator_t *accumulator,
                                  const object *op) {
    accumulator->sent = true;
    /* Per face exactly as spatial_cell_light_face(), restricted to sent
     * objects. */
    uint8_t faces = celestial_structure_faces(op);
    if (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN)) {
        faces |= CELESTIAL_FACE_DOWN;
    }
    if (QUERY_FLAG(op, FLAG_BLOCKSVIEW)) {
        accumulator->blocker = true;
    }
    for (size_t i = 0; i < arraysize(accumulator->coefficient); i++) {
        uint8_t face = (uint8_t)(1U << i);
        if ((faces & face) == 0) {
            continue;
        }
        accumulator->found |= face;
        const char *value;
        if (op->type == DOOR || op->type == GATE) {
            bool closed = op->type == DOOR ? QUERY_FLAG(op, FLAG_DOOR_CLOSED)
                                           : QUERY_FLAG(op, FLAG_NO_PASS);
            value = object_get_value(op, closed ? "celestial_transmission_closed"
                                                : "celestial_transmission_open");
            accumulator->aperture |= face;
        } else {
            value = object_get_value(op, "celestial_transmission");
            if (value != NULL) {
                accumulator->aperture |= face;
            }
        }
        celestial_transmission_t transmission = celestial_structure_transmission(value);
        accumulator->coefficient[i] = MIN(accumulator->coefficient[i],
                                          transmission == CELESTIAL_TRANSMISSION_INVALID
                                              ? 0
                                              : (uint16_t)transmission);
    }
}

static uint32_t closure_class(uint16_t coefficient) {
    switch (coefficient) {
    case CELESTIAL_TRANSMISSION_GLASS: return 1;
    case CELESTIAL_TRANSMISSION_GRATE: return 2;
    case CELESTIAL_TRANSMISSION_OPEN: return 3;
    default: return 0;
    }
}

/**
 * The static downward face of a cell, from every object on it a viewer may
 * see, as spatial_cell_light_face() reads it. Floors and roofs belong to the
 * cell whether or not the camera draws their layer. Doors and gates count as
 * closed, or are skipped, since their state is shown only when sent. An object
 * drawn by direction counts only where MAP2 shows it at window position ax, ay.
 */
static uint16_t closure_static_down(mapstruct *map, int x, int y, const object *viewer, int ax,
                                    int ay, bool doors_closed, bool *aperture) {
    uint16_t coefficient = CELESTIAL_SCALE;
    for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
        /* A plugin decides per viewer whether MAP2 shows an object; one that
         * may hide it is never disclosed here, where no plugin is asked. */
        const object *head = HEAD(op);
        if ((QUERY_FLAG(op, FLAG_IS_INVISIBLE) && !QUERY_FLAG(viewer, FLAG_SEE_INVISIBLE)) ||
            (op->layer != LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN)) ||
            HAS_EVENT(head, EVENT_ASK_SHOW) || !map2_direction_shown(op, viewer, ax, ay)) {
            continue;
        }
        uint8_t faces = celestial_structure_faces(op);
        if (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN)) {
            faces |= CELESTIAL_FACE_DOWN;
        }
        if ((faces & CELESTIAL_FACE_DOWN) == 0) {
            continue;
        }
        const char *value;
        if (op->type == DOOR || op->type == GATE) {
            if (!doors_closed) {
                continue;
            }
            value = object_get_value(op, "celestial_transmission_closed");
            *aperture = true;
        } else {
            value = object_get_value(op, "celestial_transmission");
            if (value != NULL) {
                *aperture = true;
            }
        }
        celestial_transmission_t transmission = celestial_structure_transmission(value);
        coefficient = MIN(coefficient, transmission == CELESTIAL_TRANSMISSION_INVALID
                                           ? 0
                                           : (uint16_t)transmission);
    }
    return coefficient;
}

uint32_t celestial_closure_cell_value(const celestial_closure_accumulator_t *accumulator,
                                      mapstruct *map, int x, int y, const object *viewer, int ax,
                                      int ay) {
    uint32_t value = CELESTIAL_CLOSURE_IN;
    bool static_aperture = false;
    uint16_t static_down =
        closure_static_down(map, x, y, viewer, ax, ay, false, &static_aperture);
    for (size_t i = 0; i < arraysize(accumulator->coefficient); i++) {
        uint8_t face = (uint8_t)(1U << i);
        uint16_t coefficient = accumulator->coefficient[i];
        if (face == CELESTIAL_FACE_DOWN) {
            coefficient = MIN(coefficient, static_down);
            if (static_aperture) {
                value |= CELESTIAL_CLOSURE_APERTURE(i);
            }
        }
        /* A sent view blocker makes every unauthored cardinal face opaque,
         * as P_BLOCKSVIEW does for gameplay, from sent objects only. */
        if ((accumulator->found & face) == 0 && face != CELESTIAL_FACE_DOWN &&
            accumulator->blocker) {
            coefficient = 0;
        }
        value |= closure_class(coefficient) << CELESTIAL_CLOSURE_CLASS_SHIFT(i);
        if ((accumulator->aperture & face) != 0) {
            value |= CELESTIAL_CLOSURE_APERTURE(i);
        }
    }
    spatial_cell cell = {.map = map, .x = x, .y = y};
    if (spatial_cell_light_face(&cell, 0, 0, 1, NULL) == CELESTIAL_SCALE) {
        value |= CELESTIAL_CLOSURE_COVER_OPEN;
    }
    /* Seam continuity and the sky exit are authored structure and links,
     * judged exactly as transport judges each crossing. */
    static const struct {
        int8_t normal[3];
        uint32_t bit;
    } neighbors[] = {
        {{0, -1, 0}, CELESTIAL_CLOSURE_CONTINUOUS_NORTH},
        {{1, 0, 0}, CELESTIAL_CLOSURE_CONTINUOUS_EAST},
        {{0, 1, 0}, CELESTIAL_CLOSURE_CONTINUOUS_SOUTH},
        {{-1, 0, 0}, CELESTIAL_CLOSURE_CONTINUOUS_WEST},
        {{0, 0, 1}, CELESTIAL_CLOSURE_CONTINUOUS_UP},
        {{0, 0, -1}, CELESTIAL_CLOSURE_CONTINUOUS_DOWN},
    };
    for (size_t i = 0; i < arraysize(neighbors); i++) {
        spatial_face face = {.from = cell};
        memcpy(face.normal, neighbors[i].normal, sizeof(face.normal));
        if (!spatial_resolve(map, x + face.normal[0], y + face.normal[1], face.normal[2],
                             &face.to)) {
            face.to = (spatial_cell){0};
            if (face.normal[2] > 0) {
                value |= UINT32_C(3) << CELESTIAL_CLOSURE_OVERHEAD_SHIFT;
                if (celestial_sky_exit(&cell)) {
                    value |= CELESTIAL_CLOSURE_SKY_EXIT;
                }
            }
            continue;
        }
        if (face.normal[2] > 0) {
            /* The floor or roof above, which the client reads while that
             * cell is outside the closure: an indoor viewer's own ceiling is
             * cut away from view but still shades the room. */
            bool overhead_aperture = false;
            uint16_t overhead = closure_static_down(face.to.map, face.to.x, face.to.y, viewer, ax,
                                                    ay, true, &overhead_aperture);
            value |= closure_class(overhead) << CELESTIAL_CLOSURE_OVERHEAD_SHIFT;
            if (overhead_aperture) {
                value |= CELESTIAL_CLOSURE_OVERHEAD_APERTURE;
            }
        }
        if (celestial_face_continuous(&face)) {
            value |= neighbors[i].bit;
        }
    }
    return value;
}

static spatial_cell spatial_from_solver(const celestial_solver_cell_t *cell) {
    return (spatial_cell){.map = cell->map, .x = cell->x, .y = cell->y, .z = cell->z};
}

static spatial_face spatial_face_from_solver(const celestial_solver_face_t *face) {
    spatial_face converted = {.from = spatial_from_solver(&face->from),
                              .to = spatial_from_solver(&face->to)};
    memcpy(converted.normal, face->normal, sizeof(converted.normal));
    return converted;
}

static bool lattice_resolve(void *context, void *origin, int x, int y, int z,
                            celestial_solver_cell_t *cell) {
    (void)context;
    spatial_cell resolved;
    if (!spatial_resolve(origin, x, y, z, &resolved)) {
        return false;
    }
    *cell = (celestial_solver_cell_t){.map = resolved.map, .x = resolved.x, .y = resolved.y,
                                      .z = resolved.z};
    return true;
}

static bool lattice_observe(void *context, void *map) {
    (void)context;
    return celestial_observe(map);
}

static void lattice_dimensions(void *context, void *map, int *width, int *height) {
    (void)context;
    *width = ((mapstruct *)map)->width;
    *height = ((mapstruct *)map)->height;
}

static bool lattice_sky(void *context, const celestial_solver_face_t *face) {
    (void)context;
    spatial_cell from = spatial_from_solver(&face->from);
    return face->to.map == NULL && celestial_sky_exit(&from);
}

static bool lattice_continuous(void *context, const celestial_solver_face_t *face) {
    (void)context;
    spatial_face converted = spatial_face_from_solver(face);
    return celestial_face_continuous(&converted);
}

static uint16_t lattice_light(void *context, const celestial_solver_face_t *face,
                              bool *aperture) {
    (void)context;
    spatial_face converted = spatial_face_from_solver(face);
    return spatial_face_light(&converted, aperture);
}

/* Spill tables are charged to the shared transient limit. */
static bool lattice_reserve(void *context, size_t bytes) {
    (void)context;
    if (pending_memory > CELESTIAL_PENDING_MEMORY_MAX - bytes) {
        return false;
    }
    pending_memory += bytes;
    return true;
}

static void lattice_release(void *context, size_t bytes) {
    (void)context;
    HARD_ASSERT(pending_memory >= bytes);
    pending_memory -= bytes;
}

/* The server's lattice is its maps, read through the spatial resolver with
 * the structure predicates gameplay transport always used. */
static const celestial_solver_ops_t lattice_ops = {
    .resolve = lattice_resolve,
    .observe = lattice_observe,
    .dimensions = lattice_dimensions,
    .sky = lattice_sky,
    .continuous = lattice_continuous,
    .light = lattice_light,
    .reserve = lattice_reserve,
    .release = lattice_release,
};
static const celestial_solver_lattice_t lattice = {.ops = &lattice_ops};

static int32_t transmit(int32_t value, uint16_t coefficient) {
    return celestial_model_transmit(value, coefficient);
}

typedef celestial_solver_ray_t celestial_ray;

static void celestial_ray_step(celestial_ray *ray) {
    celestial_solver_ray_step(&lattice, ray);
}

static int32_t celestial_ray_value(const celestial_ray *ray, int32_t source, int32_t *aperture) {
    return celestial_solver_ray_value(ray, source, aperture);
}

static int celestial_scratch_width(mapstruct *map) {
    return celestial_solver_scratch_width(&lattice, map);
}

static int celestial_scratch_height(mapstruct *map) {
    return celestial_solver_scratch_height(&lattice, map);
}

static size_t celestial_scratch_cells(mapstruct *map) {
    return (size_t)celestial_scratch_width(map) * celestial_scratch_height(map);
}

static void celestial_halo_ray(celestial_ray *ray, mapstruct *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    celestial_solver_halo_ray(&lattice, ray, map, x, y, direction, elevation);
}

static uint16_t celestial_spill_coefficient(mapstruct *map, int x, int y, int dx, int dy) {
    return celestial_solver_spill(&lattice, map, x, y, dx, dy);
}

static void clear_map_field(mapstruct *map) {
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            space->celestial_light_value = 0;
            memset(space->celestial_light_rgb, 0, sizeof(space->celestial_light_rgb));
            space->celestial_light_next_value = 0;
            memset(space->celestial_light_next_rgb, 0, sizeof(space->celestial_light_next_rgb));
        }
    }
}

static uint64_t celestial_key(const mapstruct *map,
                              const region_celestial_profile_t *profile,
                              const region_celestial_phases_t *phases) {
    uint64_t hash = UINT64_C(1469598103934665603);
    region_celestial_phases_t effective_phases = *phases;
#define MIX_CELESTIAL(value)                                                                   \
    do {                                                                                        \
        hash ^= (uint64_t)(value);                                                              \
        hash *= UINT64_C(1099511628211);                                                        \
    } while (0)
    MIX_CELESTIAL(2); /* 3D transport algorithm; authored schema is unchanged. */
    for (size_t byte = 0; byte < sizeof(profile->digest); byte++) {
        MIX_CELESTIAL(profile->digest[byte]);
    }
    MIX_CELESTIAL(effective_phases.solar);
    MIX_CELESTIAL(effective_phases.season);
    (void)celestial_override_apply(profile->lunar_period, &effective_phases.lunar);
    MIX_CELESTIAL(effective_phases.lunar);
    celestial_override_state_t override;
    celestial_override_get(&override);
    MIX_CELESTIAL(override.revision);
    MIX_CELESTIAL(override.active);
    MIX_CELESTIAL(override.mode);
    MIX_CELESTIAL(override.value);
    MIX_CELESTIAL(override.period);
    MIX_CELESTIAL(map->celestial_structure_revision);
    MIX_CELESTIAL(map->celestial_schema);
    for (size_t i = 0; i < TILED_NUM; i++) {
        MIX_CELESTIAL(map->celestial_boundary[i]);
        MIX_CELESTIAL(map->celestial_tile_path_seen[i]);
        MIX_CELESTIAL((uintptr_t)map->tile_path[i]);
        bool relevant = i >= TILED_UP ||
            (map->celestial_boundary[i] != CELESTIAL_BOUNDARY_DISCONTINUOUS &&
             (map->celestial_tile_path_seen[i] ||
              map->celestial_boundary[i] != CELESTIAL_BOUNDARY_UNSET));
        if (relevant) {
            MIX_CELESTIAL((uintptr_t)map->tile_map[i]);
            MIX_CELESTIAL(map->tile_map[i] != NULL ? map->tile_map[i]->in_memory : 0);
        }
    }
#undef MIX_CELESTIAL
    return hash;
}

static bool collect_stack(mapstruct *map, mapstruct *levels[MAP2_LEVELS], size_t *count) {
    /* Sky exposure and transport use the resolved upper maps.
     * A lower map cannot affect this map, and may be hidden/unloaded beneath
     * an opaque floor. Solve only the requested map and its upper chain. */
    mapstruct *cursor = map;
    while (cursor != NULL) {
        if (*count >= MAP2_LEVELS || cursor->in_memory != MAP_IN_MEMORY ||
            cursor->width < 1 || cursor->height < 1 ||
            cursor->width > 64 || cursor->height > 64) {
            return false;
        }
        levels[(*count)++] = cursor;
        if (cursor->tile_map[TILED_UP] == NULL) {
            break;
        }
        cursor = cursor->tile_map[TILED_UP];
    }
    return *count != 0;
}

/** No rejected synchronous solve may leave readable radiance behind. */
static void celestial_clear_failed_stack(mapstruct *map) {
    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    (void)collect_stack(map, levels, &count);
    for (size_t i = 0; i < count; i++) {
        mapstruct *current = levels[i];
        clear_map_field(current);
        current->celestial_light_valid = false;
        current->celestial_light_keyframe_valid = false;
        if (current->celestial_light_generation_id == UINT64_MAX) {
            IMPOSSIBLE();
        }
        current->celestial_light_generation_id++;
        celestial_dependencies_field_release(current, false);
        celestial_dependencies_field_release(current, true);
    }
}

void celestial_light_invalidate(mapstruct *map) {
    if (map == NULL) {
        return;
    }
    celestial_dependencies_inputs_changed();
    celestial_dependencies_forget(map);
    celestial_light_forget(map);
    mapstruct *cursor = map;
    for (size_t i = 0; i < MAP2_LEVELS && cursor != NULL; i++) {
        if (cursor->celestial_structure_revision == UINT64_MAX) {
            IMPOSSIBLE();
        }
        cursor->celestial_structure_revision++;
        celestial_light_forget(cursor);
        cursor->celestial_light_valid = false;
        cursor->celestial_light_keyframe_valid = false;
        cursor = cursor->tile_map[TILED_UP];
    }
    cursor = map->tile_map[TILED_DOWN];
    for (size_t i = 0; i < MAP2_LEVELS && cursor != NULL; i++) {
        if (cursor->celestial_structure_revision == UINT64_MAX) {
            IMPOSSIBLE();
        }
        cursor->celestial_structure_revision++;
        celestial_light_forget(cursor);
        cursor->celestial_light_valid = false;
        cursor->celestial_light_keyframe_valid = false;
        cursor = cursor->tile_map[TILED_DOWN];
    }
}

void celestial_light_invalidate_all(void) {
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        if (map->celestial_schema != 1) {
            continue;
        }
        celestial_light_forget(map);
        /* The override state is part of celestial_key(). Keep the current
         * generation so the next ensure observes a real key transition. */
        map->celestial_light_keyframe_valid = false;
        map->celestial_light_next_key = 0;
    }
}

bool celestial_light_rebuild(mapstruct *map, uint64_t absolute_hour) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL) {
        return false;
    }
#ifdef ATRINIK_TESTING
    rebuild_count++;
#endif

    /* The synchronous fallback is server-thread exclusive. Its reserve covers
     * the largest one-level halo and solve outputs plus dependency
     * bookkeeping. */
    size_t scratch = CELESTIAL_REBUILD_BYTES(map->width, map->height);
    bool reservation_available = pending_memory <= CELESTIAL_PENDING_MEMORY_MAX - scratch;
#ifdef ATRINIK_TESTING
    reservation_available &= !current_reservation_fail;
#endif
    if (map->width < 1 || map->height < 1 || map->width > 64 || map->height > 64 ||
        !reservation_available) {
        celestial_clear_failed_stack(map);
        return false;
    }
    pending_memory += scratch;
    celestial_dependencies *dependencies = celestial_dependencies_begin(
        &pending_memory, CELESTIAL_PENDING_MEMORY_MAX, CELESTIAL_PENDING_MEMORY_MAX / 64);
    if (dependencies == NULL) {
        pending_memory -= scratch;
        celestial_clear_failed_stack(map);
        return false;
    }
    HARD_ASSERT(active_dependencies == NULL);
    active_dependencies = dependencies;
    spatial_observer observer = spatial_observer_exchange(
        (spatial_observer){celestial_spatial_observe, dependencies});
    (void)celestial_observe(map);
    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    char error[HUGE_BUF];
    bool stack_valid = collect_stack(map, levels, &count);
    for (size_t i = 0; i < count; i++) (void)celestial_observe(levels[i]);
    /* Levels solve one at a time: the reserve grows to the largest one, or
     * the stack fails as when the first reserve is unavailable. */
    for (size_t i = 0; stack_valid && i < count; i++) {
        size_t need = CELESTIAL_REBUILD_BYTES(levels[i]->width, levels[i]->height);
        if (need <= scratch) {
            continue;
        }
        if (pending_memory > CELESTIAL_PENDING_MEMORY_MAX - (need - scratch)) {
            (void)spatial_observer_exchange(observer);
            active_dependencies = NULL;
            celestial_dependencies_release(dependencies);
            pending_memory -= scratch;
            celestial_clear_failed_stack(map);
            return false;
        }
        pending_memory += need - scratch;
        scratch = need;
    }
    if (!stack_valid || !celestial_structure_validate_light_dependencies(map, VS(error))) {
        for (size_t i = 0; i < count; i++) {
            mapstruct *current = levels[i];
            clear_map_field(current);
            const region_celestial_profile_t *profile = region_celestial_for_map(current);
            region_celestial_phases_t phases;
            region_celestial_phases(profile, absolute_hour, &phases);
            uint64_t key = celestial_key(current, profile, &phases);
            if (current->celestial_light_generation_id == 0) {
                current->celestial_light_generation_id = 1;
            } else if (!current->celestial_light_valid || current->celestial_light_key != key ||
                       !celestial_dependencies_field_fresh(current, false)) {
                if (current->celestial_light_generation_id == UINT64_MAX) {
                    IMPOSSIBLE();
                }
                current->celestial_light_generation_id++;
            }
            current->celestial_light_key = key;
            current->celestial_light_valid = true;
            current->celestial_light_keyframe_valid = false;
        }
        (void)celestial_dependencies_finish(dependencies, levels, count, false);
        (void)spatial_observer_exchange(observer);
        active_dependencies = NULL;
        celestial_dependencies_release(dependencies);
        pending_memory -= scratch;
        return false;
    }

    for (size_t level = count; level-- > 0;) {
        mapstruct *current = levels[level];
        celestial_model model;
        celestial_model_for_map(current, absolute_hour, &model);
        size_t cells = (size_t)current->width * current->height;
        int32_t *values = xcalloc(cells, sizeof(*values));
        int32_t (*rgb)[3] = xcalloc(cells, sizeof(*rgb));
        if (!celestial_solver_solve(&lattice, current, &model, values, rgb)) {
            LOG(ERROR, "Out of memory solving the celestial field.");
            abort();
        }
        for (size_t cell = 0; cell < cells; cell++) {
            current->spaces[cell].celestial_light_value = values[cell];
            memcpy(current->spaces[cell].celestial_light_rgb, rgb[cell], sizeof(rgb[cell]));
        }
        free(rgb);
        free(values);

        region_celestial_phases_t phases;
        region_celestial_phases(region_celestial_for_map(current), absolute_hour, &phases);
        const region_celestial_profile_t *profile = region_celestial_for_map(current);
        uint64_t previous_key = current->celestial_light_key;
        bool previous_valid = current->celestial_light_valid &&
                              celestial_dependencies_field_fresh(current, false);
        current->celestial_light_key = celestial_key(current, profile, &phases);
        if (current->celestial_light_generation_id == 0) {
            current->celestial_light_generation_id = 1;
        } else if (!previous_valid || previous_key != current->celestial_light_key) {
            if (current->celestial_light_generation_id == UINT64_MAX) {
                IMPOSSIBLE();
            }
            current->celestial_light_generation_id++;
        }
        current->celestial_light_valid = true;
        current->celestial_light_keyframe_valid = false;
    }
    bool complete = celestial_dependencies_finish(dependencies, levels, count, false);
    if (!complete) {
        celestial_clear_failed_stack(map);
    }
    (void)spatial_observer_exchange(observer);
    active_dependencies = NULL;
    celestial_dependencies_release(dependencies);
    pending_memory -= scratch;
    return complete;
}

typedef struct celestial_field_snapshot {
    mapstruct *map;
    celestial_dependencies *dependencies;
    size_t count;
    int32_t *value;
    int32_t(*rgb)[3];
    uint64_t key;
    uint64_t generation;
    bool valid;
} celestial_field_snapshot_t;

/** Preserve the current field while the next hourly field is solved. */
static bool celestial_field_snapshot_save(mapstruct *map,
                                          celestial_field_snapshot_t *snapshot) {
    snapshot->map = map;
    snapshot->dependencies = celestial_dependencies_field_get(map, false);
    snapshot->count = (size_t)map->width * map->height;
    snapshot->value = xcalloc(snapshot->count, sizeof(*snapshot->value));
    snapshot->rgb = xcalloc(snapshot->count, sizeof(*snapshot->rgb));
    snapshot->key = map->celestial_light_key;
    snapshot->generation = map->celestial_light_generation_id;
    snapshot->valid = map->celestial_light_valid;
    for (size_t i = 0; i < snapshot->count; i++) {
        snapshot->value[i] = map->spaces[i].celestial_light_value;
        memcpy(snapshot->rgb[i], map->spaces[i].celestial_light_rgb, sizeof(snapshot->rgb[i]));
    }
    return true;
}

static bool celestial_field_snapshot_restore(celestial_field_snapshot_t *snapshot) {
    for (size_t i = 0; i < snapshot->count; i++) {
        snapshot->map->spaces[i].celestial_light_value = snapshot->value[i];
        memcpy(snapshot->map->spaces[i].celestial_light_rgb,
               snapshot->rgb[i],
               sizeof(snapshot->rgb[i]));
    }
    snapshot->map->celestial_light_key = snapshot->key;
    snapshot->map->celestial_light_generation_id = snapshot->generation;
    snapshot->map->celestial_light_valid = snapshot->valid;
    bool bound = true;
    if (snapshot->dependencies != NULL) {
        bound = celestial_dependencies_field_bind(snapshot->dependencies, snapshot->map, false);
        celestial_dependencies_release(snapshot->dependencies);
    } else {
        celestial_dependencies_field_release(snapshot->map, false);
    }
    free(snapshot->value);
    free(snapshot->rgb);
    snapshot->value = NULL;
    snapshot->rgb = NULL;
    if (!bound) {
        snapshot->map->celestial_light_valid = false;
        celestial_dependencies_field_release(snapshot->map, false);
    }
    return bound;
}

uint64_t celestial_light_generation(const mapstruct *map) {
    if (map == NULL || !map->celestial_light_valid || map->celestial_light_generation_id == 0) {
        return 0;
    }
    return map->celestial_light_generation_id;
}

/* A queued solve never mutates the current field. Each step evaluates one
 * crossing, cell, or bounded stage with identical ray arithmetic and Jacobi
 * pass order. No directional component is copied from another level's field.
 * All work is server-thread
 * owned; forget() removes references before map storage can be recycled. */
typedef struct celestial_pending_level {
    mapstruct *map;
    MapSpace *spaces;
    int width, height;
    size_t cells;
    uint64_t key;
    int32_t *value;
    int32_t (*rgb)[3];
} celestial_pending_level_t;

typedef enum celestial_pending_stage {
    CELESTIAL_PREPARE,
    CELESTIAL_EXPOSE,
    CELESTIAL_SUN,
    CELESTIAL_MOON,
    CELESTIAL_DIFFUSE,
    CELESTIAL_APERTURE,
    CELESTIAL_STARS,
    CELESTIAL_OUTPUT,
    CELESTIAL_COMMIT
} celestial_pending_stage_t;

typedef struct celestial_pending {
    struct celestial_pending *next;
    mapstruct *owner;
    uint64_t source_hour;
    size_t count, level, cell, steps;
    size_t reservation;
    uint64_t validated_batch;
    celestial_dependencies *dependencies;
    celestial_pending_level_t levels[MAP2_LEVELS];
    celestial_pending_stage_t stage;
    celestial_model model;
    int32_t *direct, *diffuse, *aperture, *moon, *moon_aperture, *stars, *next_values;
    int pass;
    celestial_ray ray;
    bool ray_active;
    bool diffuse_active, aperture_active, stars_active;
} celestial_pending_t;

static celestial_pending_t *pending_head, *pending_tail;
static uint64_t pending_batch;


static void pending_free_scratch(celestial_pending_t *job) {
    free(job->direct);
    free(job->diffuse);
    free(job->aperture);
    free(job->moon);
    free(job->moon_aperture);
    free(job->stars);
    free(job->next_values);
    job->direct = job->diffuse = job->aperture = NULL;
    job->moon = job->moon_aperture = job->stars = job->next_values = NULL;
}

static void pending_free(celestial_pending_t *job) {
    celestial_dependencies_release(job->dependencies);
    pending_free_scratch(job);
    for (size_t i = 0; i < job->count; i++) {
        free(job->levels[i].value);
        free(job->levels[i].rgb);
    }
    HARD_ASSERT(pending_memory >= job->reservation);
    pending_memory -= job->reservation;
    free(job);
}

void celestial_light_forget(mapstruct *map) {
    celestial_pending_t **link = &pending_head;
    pending_tail = NULL;
    while (*link != NULL) {
        celestial_pending_t *job = *link;
        bool retained = celestial_dependencies_references_map(job->dependencies, map);
        for (size_t i = 0; i < job->count; i++) {
            retained |= job->levels[i].map == map;
        }
        if (retained) {
            *link = job->next;
            pending_free(job);
        } else {
            pending_tail = job;
            link = &job->next;
        }
    }
}

static uint64_t key_for_hour(const mapstruct *map, uint64_t hour) {
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t phases;
    region_celestial_phases(profile, hour, &phases);
    return celestial_key(map, profile, &phases);
}

static bool pending_fresh(const celestial_pending_t *job) {
    if (job->source_hour != (uint64_t)todtick ||
        !celestial_dependencies_fresh(job->dependencies)) {
        return false;
    }
    for (size_t i = 0; i < job->count; i++) {
        const celestial_pending_level_t *level = &job->levels[i];
        const mapstruct *map = level->map;
        if (map->in_memory != MAP_IN_MEMORY || map->spaces != level->spaces ||
            map->width != level->width || map->height != level->height ||
            map->tile_map[TILED_UP] != (i + 1 < job->count ? job->levels[i + 1].map : NULL) ||
            key_for_hour(map, job->source_hour + 1) != level->key) {
            return false;
        }
    }
    return true;
}

static void pending_append(celestial_pending_t *job) {
    job->next = NULL;
    if (pending_tail != NULL) {
        pending_tail->next = job;
    } else {
        pending_head = job;
    }
    pending_tail = job;
}

static void pending_ray_reset(celestial_pending_t *job) {
    job->cell = 0;
    job->ray_active = false;
}

static bool pending_ray_step(celestial_pending_t *job, bool moon) {
    celestial_pending_level_t *level = &job->levels[job->level];
    if (!job->ray_active) {
        celestial_halo_ray(&job->ray, level->map, (int)(job->cell % celestial_scratch_width(level->map)),
                           (int)(job->cell / celestial_scratch_width(level->map)),
                           moon ? job->model.moon_direction : job->model.solar_direction,
                           moon ? job->model.moon_elevation : job->model.solar_elevation);
        job->ray_active = true;
    }
    celestial_ray_step(&job->ray);
    if (job->ray.done) {
        int32_t *values = moon ? job->moon : job->direct;
        int32_t *apertures = moon ? job->moon_aperture : job->aperture;
        values[job->cell] = celestial_ray_value(&job->ray,
            moon ? job->model.moon : job->model.direct, &apertures[job->cell]);
        if (!moon) job->aperture_active |= apertures[job->cell] != 0;
        job->ray_active = false;
        if (++job->cell == celestial_scratch_cells(level->map)) return true;
    }
    return false;
}

static void pending_relax_step(celestial_pending_t *job, int32_t **values) {
    celestial_pending_level_t *level = &job->levels[job->level];
    int x = (int)(job->cell % celestial_scratch_width(level->map));
    int y = (int)(job->cell / celestial_scratch_width(level->map));
    int32_t best = (*values)[job->cell];
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                x + dx >= celestial_scratch_width(level->map) || y + dy >= celestial_scratch_height(level->map)) {
                continue;
            }
            uint16_t coefficient = celestial_spill_coefficient(level->map, x, y, dx, dy);
            int32_t candidate = transmit((*values)[x + dx + celestial_scratch_width(level->map) * (y + dy)], coefficient);
            candidate = transmit(candidate, dx != 0 && dy != 0 ? 181 : 192);
            best = MAX(best, candidate);
        }
    }
    job->next_values[job->cell++] = best;
    if (job->cell == celestial_scratch_cells(level->map)) {
        int32_t *swap = *values;
        *values = job->next_values;
        job->next_values = swap;
        job->cell = 0;
        if (++job->pass == CELESTIAL_SPILL_PASSES) {
            job->pass = 0;
            job->stage++;
        }
    }
}

/** Return true only after all next fields have been copied and published. */
static bool pending_step(celestial_pending_t *job) {
    celestial_pending_level_t *level = &job->levels[job->level];
    int x = (int)(job->cell % level->width);
    int y = (int)(job->cell / level->width);
    job->steps++;
    switch (job->stage) {
    case CELESTIAL_PREPARE:
        job->diffuse_active = job->aperture_active = job->stars_active = false;
        celestial_model_for_map(level->map, job->source_hour + 1, &job->model);
        job->direct = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->direct));
        job->diffuse = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->diffuse));
        job->aperture = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->aperture));
        job->moon = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->moon));
        job->moon_aperture = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->moon_aperture));
        job->stars = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->stars));
        job->next_values = xcalloc(celestial_scratch_cells(level->map), sizeof(*job->next_values));
        level->value = xcalloc(level->cells, sizeof(*level->value));
        level->rgb = xcalloc(level->cells, sizeof(*level->rgb));
        job->stage = CELESTIAL_EXPOSE;
        break;
    case CELESTIAL_EXPOSE:
        if (!job->ray_active) {
            celestial_halo_ray(&job->ray, level->map,
                (int)(job->cell % celestial_scratch_width(level->map)),
                (int)(job->cell / celestial_scratch_width(level->map)), NORTH, 32768);
            job->ray_active = true;
        }
        celestial_ray_step(&job->ray);
        if (job->ray.done) {
            job->diffuse[job->cell] = celestial_ray_value(&job->ray, job->model.diffuse, NULL);
            job->stars[job->cell] = celestial_ray_value(&job->ray, job->model.starlight, NULL);
            job->diffuse_active |= job->diffuse[job->cell] != 0;
            job->stars_active |= job->stars[job->cell] != 0;
            job->ray_active = false;
            if (++job->cell == celestial_scratch_cells(level->map)) {
                job->stage = CELESTIAL_SUN;
                pending_ray_reset(job);
            }
        }
        break;
    case CELESTIAL_SUN:
    case CELESTIAL_MOON:
        if ((job->stage == CELESTIAL_MOON && job->model.moon == 0) ||
            pending_ray_step(job, job->stage == CELESTIAL_MOON)) {
            job->stage++;
            pending_ray_reset(job);
        }
        break;
    case CELESTIAL_DIFFUSE:
        if (job->diffuse_active) pending_relax_step(job, &job->diffuse);
        else job->stage++;
        break;
    case CELESTIAL_APERTURE:
        if (job->aperture_active) pending_relax_step(job, &job->aperture);
        else job->stage++;
        break;
    case CELESTIAL_STARS:
        if (job->stars_active) pending_relax_step(job, &job->stars);
        else job->stage++;
        break;
    case CELESTIAL_OUTPUT: {
        size_t cell = job->cell;
        size_t sample = x + CELESTIAL_SPILL_PASSES + celestial_scratch_width(level->map) *
                        (y + CELESTIAL_SPILL_PASSES);
        celestial_model_publish(&job->model, job->direct[sample], job->diffuse[sample],
                                job->aperture[sample], job->moon[sample],
                                job->moon_aperture[sample], job->stars[sample],
                                &level->value[cell], level->rgb[cell]);
        if (++job->cell == level->cells) {
            job->cell = 0;
            pending_free_scratch(job);
            if (job->level != 0) {
                job->level--;
                job->stage = CELESTIAL_PREPARE;
            } else {
                job->stage = CELESTIAL_COMMIT;
            }
        }
        break;
    }
    case CELESTIAL_COMMIT:
        level->spaces[job->cell].celestial_light_next_value = level->value[job->cell];
        memcpy(level->spaces[job->cell].celestial_light_next_rgb,
               level->rgb[job->cell], sizeof(level->rgb[job->cell]));
        if (++job->cell == level->cells) {
            job->cell = 0;
            if (++job->level == job->count) {
                mapstruct *maps[MAP2_LEVELS];
                for (size_t i = 0; i < job->count; i++) maps[i] = job->levels[i].map;
                if (!celestial_dependencies_finish(job->dependencies, maps, job->count, true))
                    return true;
                for (size_t i = 0; i < job->count; i++) {
                    mapstruct *map = job->levels[i].map;
                    map->celestial_light_next_key = job->levels[i].key;
                    map->celestial_light_next_hour = job->source_hour + 1;
                    /* Readiness changes the transmitted endpoint pair, even
                     * when the current scalar/RGB field remains identical. */
                    if (map->celestial_light_generation_id == UINT64_MAX) {
                        IMPOSSIBLE();
                    }
                    map->celestial_light_generation_id++;
                    map->celestial_light_keyframe_valid = true;
                }
                return true;
            }
        }
        break;
    }
    return false;
}

static void pending_process(size_t limit, uint64_t deadline) {
    if (pending_batch == UINT64_MAX) {
        IMPOSSIBLE();
    }
    pending_batch++;
    while (pending_head != NULL && limit-- != 0 &&
           (deadline == 0 || datetime_monotonic_us() < deadline)) {
        celestial_pending_t *job = pending_head;
        pending_head = job->next;
        if (pending_head == NULL) {
            pending_tail = NULL;
        }
        if (job->validated_batch != pending_batch && !pending_fresh(job)) {
            pending_free(job);
            continue;
        }
        job->validated_batch = pending_batch;
        HARD_ASSERT(active_dependencies == NULL);
        active_dependencies = job->dependencies;
        spatial_observer observer = spatial_observer_exchange(
            (spatial_observer){celestial_spatial_observe, job->dependencies});
        bool finished = pending_step(job);
        (void)spatial_observer_exchange(observer);
        active_dependencies = NULL;
        if (finished) pending_free(job);
        else pending_append(job);
    }
}

void celestial_light_process_pending(uint64_t budget_us) {
    if (budget_us != 0) {
        uint64_t now = datetime_monotonic_us();
        pending_process(SIZE_MAX, budget_us > UINT64_MAX - now ? UINT64_MAX : now + budget_us);
    }
}

#ifdef ATRINIK_TESTING
/* The conformance reference (CLIENT_CELESTIAL.md, Client solver and
 * conformance): the server solver on real maps and links, with every cell
 * outside the viewer's closure, and every point no map resolves, open air
 * under an open sky. Unresolved points keep the coordinates of the map the
 * traversal started from. */
typedef struct reference_air {
    mapstruct *origin;
    int z;
} reference_air;

typedef struct reference_lattice {
    celestial_reference_member_fn member;
    void *context;
    reference_air air[512];
    size_t air_count;
} reference_lattice;

static reference_air *reference_air_of(reference_lattice *lattice, const void *map) {
    /* Equality only: ordering a real map against this array is undefined. */
    for (size_t i = 0; i < lattice->air_count; i++) {
        if (map == &lattice->air[i]) {
            return &lattice->air[i];
        }
    }
    return NULL;
}

/** The disclosed record of a closure cell, zero outside the closure. */
static uint32_t reference_record(reference_lattice *lattice, const celestial_solver_cell_t *cell) {
    if (cell->map == NULL || reference_air_of(lattice, cell->map) != NULL) {
        return 0;
    }
    return lattice->member(lattice->context, cell->map, cell->x, cell->y);
}

static bool reference_inside(reference_lattice *lattice, const celestial_solver_cell_t *cell) {
    return reference_record(lattice, cell) != 0;
}

static bool reference_resolve(void *context, void *origin, int x, int y, int z,
                              celestial_solver_cell_t *cell) {
    reference_lattice *lattice = context;
    reference_air *air = reference_air_of(lattice, origin);
    mapstruct *real = air != NULL ? air->origin : origin;
    int base = air != NULL ? air->z : 0;
    spatial_cell resolved;
    if (spatial_resolve(real, x, y, base + z, &resolved)) {
        *cell = (celestial_solver_cell_t){.map = resolved.map, .x = resolved.x, .y = resolved.y,
                                          .z = z};
        return true;
    }
    size_t found = 0;
    while (found < lattice->air_count &&
           (lattice->air[found].origin != real || lattice->air[found].z != base + z)) {
        found++;
    }
    if (found == lattice->air_count) {
        HARD_ASSERT(found < arraysize(lattice->air));
        lattice->air[lattice->air_count++] = (reference_air){.origin = real, .z = base + z};
    }
    *cell = (celestial_solver_cell_t){.map = &lattice->air[found], .x = x, .y = y, .z = z};
    return true;
}

static void reference_dimensions(void *context, void *map, int *width, int *height) {
    if (reference_air_of(context, map) != NULL) {
        *width = *height = 1;
        return;
    }
    lattice_dimensions(context, map, width, height);
}

static bool reference_sky(void *context, const celestial_solver_face_t *face) {
    reference_lattice *lattice = context;
    if (!reference_inside(lattice, &face->from)) {
        return !reference_inside(lattice, &face->to);
    }
    spatial_cell from = spatial_from_solver(&face->from);
    return reference_air_of(lattice, face->to.map) != NULL && celestial_sky_exit(&from);
}

static bool reference_continuous(void *context, const celestial_solver_face_t *face) {
    reference_lattice *lattice = context;
    bool from = reference_inside(lattice, &face->from), to = reference_inside(lattice, &face->to);
    if (!from && !to) {
        return true;
    }
    if (reference_air_of(lattice, face->from.map) != NULL ||
        reference_air_of(lattice, face->to.map) != NULL) {
        return false;
    }
    return lattice_continuous(context, face);
}

/* Faces the viewer was shown come from the disclosed record; the virtual
 * cover is the map's authored structure, read from the map. */
static const uint16_t reference_class[4] = {CELESTIAL_TRANSMISSION_OPAQUE,
                                            CELESTIAL_TRANSMISSION_GLASS,
                                            CELESTIAL_TRANSMISSION_GRATE,
                                            CELESTIAL_TRANSMISSION_OPEN};

static uint16_t reference_record_face(uint32_t record, int face, bool *aperture) {
    if (aperture != NULL && (record & CELESTIAL_CLOSURE_APERTURE(face)) != 0) {
        *aperture = true;
    }
    return reference_class[(record >> CELESTIAL_CLOSURE_CLASS_SHIFT(face)) & 3U];
}

static uint16_t reference_light(void *context, const celestial_solver_face_t *face,
                                bool *aperture) {
    reference_lattice *lattice = context;
    if (aperture != NULL) {
        *aperture = false;
    }
    if (face->normal[2] != 0) {
        const celestial_solver_cell_t *upper = face->normal[2] > 0 ? &face->to : &face->from;
        const celestial_solver_cell_t *lower = face->normal[2] > 0 ? &face->from : &face->to;
        uint32_t upper_record = reference_record(lattice, upper);
        uint32_t lower_record = reference_record(lattice, lower);
        uint16_t physical = CELESTIAL_SCALE;
        if (upper_record != 0) {
            physical = reference_record_face(upper_record, 0, aperture);
        } else if (lower_record != 0) {
            physical = reference_class[(lower_record >> CELESTIAL_CLOSURE_OVERHEAD_SHIFT) & 3U];
            if (aperture != NULL && (lower_record & CELESTIAL_CLOSURE_OVERHEAD_APERTURE) != 0) {
                *aperture = true;
            }
        }
        uint16_t cover = CELESTIAL_SCALE;
        if (lower_record != 0) {
            spatial_cell real = spatial_from_solver(lower);
            cover = spatial_cell_light_face(&real, 0, 0, 1, NULL);
        }
        return MIN(physical, cover);
    }
    uint32_t record = reference_record(lattice, &face->from);
    if (record == 0) {
        return CELESTIAL_SCALE;
    }
    int index = face->normal[0] < 0 ? 4 : face->normal[0] > 0 ? 2 : face->normal[1] < 0 ? 1 : 3;
    return reference_record_face(record, index, aperture);
}

bool celestial_light_closure_reference_for_test(mapstruct *map, uint64_t absolute_hour,
                                                celestial_reference_member_fn member,
                                                void *context, int32_t *values,
                                                int32_t (*rgb)[3]) {
    static const celestial_solver_ops_t ops = {
        .resolve = reference_resolve,
        .dimensions = reference_dimensions,
        .sky = reference_sky,
        .continuous = reference_continuous,
        .light = reference_light,
    };
    reference_lattice *reference = xcalloc(1, sizeof(*reference));
    reference->member = member;
    reference->context = context;
    celestial_solver_lattice_t solver = {.ops = &ops, .context = reference};
    celestial_model model;
    celestial_model_for_map(map, absolute_hour, &model);
    bool solved = celestial_solver_solve(&solver, map, &model, values, rgb);
    free(reference);
    return solved;
}
#endif

#ifdef ATRINIK_TESTING
uint64_t celestial_light_pending_batch_exchange_for_test(uint64_t batch) {
    uint64_t previous = pending_batch;
    pending_batch = batch;
    return previous;
}

void celestial_light_process_steps_for_test(size_t steps) {
    pending_process(steps, 0);
}

size_t celestial_light_pending_steps_for_test(const mapstruct *map) {
    for (const celestial_pending_t *job = pending_head; job != NULL; job = job->next) {
        if (job->owner == map) {
            return job->steps;
        }
    }
    return 0;
}

size_t celestial_light_pending_memory_for_test(void) {
    return pending_memory;
}

void celestial_light_pending_memory_limit_for_test(size_t limit) {
    HARD_ASSERT(limit <= CELESTIAL_PENDING_MEMORY_MAX && pending_memory == 0);
    pending_memory_limit = limit;
}
#endif

bool celestial_light_keyframe_request(mapstruct *map, uint64_t absolute_hour) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL ||
        absolute_hour != (uint64_t)todtick || absolute_hour == UINT64_MAX) {
        return false;
    }
    celestial_light_ensure(map);
    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    if (!collect_stack(map, levels, &count)) {
        return false;
    }
    bool ready = true;
    for (size_t i = 0; i < count; i++) {
        ready &= celestial_dependencies_field_fresh(levels[i], true) &&
                 levels[i]->celestial_light_keyframe_valid &&
                 levels[i]->celestial_light_next_hour == absolute_hour + 1 &&
                 levels[i]->celestial_light_next_key == key_for_hour(levels[i], absolute_hour + 1);
    }
    if (ready) {
        return true;
    }
    for (celestial_pending_t *job = pending_head; job != NULL; job = job->next) {
        if (pending_fresh(job)) {
            for (size_t i = 0; i < job->count; i++) {
                if (job->levels[i].map == map) {
                    /* A lower stack's solve already prepares this suffix. */
                    return false;
                }
            }
        }
    }
    celestial_light_forget(map);
    size_t reservation = sizeof(celestial_pending_t), largest = 0;
    for (size_t i = 0; i < count; i++) {
        if (levels[i]->width < 1 || levels[i]->height < 1 ||
            levels[i]->width > 64 || levels[i]->height > 64) {
            return false;
        }
        size_t cells = (size_t)levels[i]->width * levels[i]->height;
        if (cells > (SIZE_MAX - reservation) / 16) {
            return false;
        }
        reservation += cells * 16;
        largest = MAX(largest, celestial_scratch_cells(levels[i]));
    }
    if (largest > (SIZE_MAX - reservation) / 28) {
        return false;
    }
    reservation += largest * 28;
    size_t limit = MIN(pending_memory_limit,
                       CELESTIAL_PENDING_MEMORY_MAX - CELESTIAL_CURRENT_SCRATCH_MAX);
    if (pending_memory > limit || reservation > limit - pending_memory) {
        /* No waiting allocation: a later camera request retries after another
         * admitted job completes. Current lighting remains authoritative. */
        return false;
    }
    /* Content/exception validation can scan authored rectangles. Run it at
     * admission, after both readiness and capacity rejection fast paths. */
    char error[HUGE_BUF];
    if (!celestial_structure_validate_light_dependencies(map, VS(error))) {
        return false;
    }
    pending_memory += reservation;
    celestial_pending_t *job = xcalloc(1, sizeof(*job));
    job->reservation = reservation;
    job->owner = map;
    job->source_hour = absolute_hour;
    job->count = count;
    job->level = count - 1;
    job->dependencies = celestial_dependencies_begin(&pending_memory, limit,
                                                      CELESTIAL_PENDING_MEMORY_MAX / 64);
    if (job->dependencies == NULL) {
        pending_free(job);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!celestial_dependencies_observe(job->dependencies, levels[i])) {
            pending_free(job);
            return false;
        }
    }
    for (size_t i = 0; i < count; i++) {
        mapstruct *current = levels[i];
        job->levels[i] = (celestial_pending_level_t){
            .map = current, .spaces = current->spaces, .width = current->width,
            .height = current->height, .cells = (size_t)current->width * current->height,
            .key = key_for_hour(current, absolute_hour + 1)};
        if (current->celestial_light_keyframe_valid) {
            if (current->celestial_light_generation_id == UINT64_MAX) {
                IMPOSSIBLE();
            }
            current->celestial_light_generation_id++;
        }
        current->celestial_light_keyframe_valid = false;
    }
    pending_append(job);
    return false;
}

bool celestial_light_keyframe_ensure(mapstruct *map, uint64_t absolute_hour) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL) {
        return false;
    }

    celestial_light_ensure(map);
    if (!map->celestial_light_valid) {
        return false;
    }

    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t next_phases;
    region_celestial_phases(profile, absolute_hour + 1, &next_phases);
    uint64_t next_key = celestial_key(map, profile, &next_phases);
    if (celestial_dependencies_field_fresh(map, true) &&
        map->celestial_light_keyframe_valid && map->celestial_light_next_key == next_key &&
        map->celestial_light_next_hour == absolute_hour + 1) {
        return true;
    }

    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    if (!collect_stack(map, levels, &count)) {
        map->celestial_light_keyframe_valid = false;
        return false;
    }

    celestial_field_snapshot_t snapshots[MAP2_LEVELS] = {0};
    for (size_t i = 0; i < count; i++) {
        celestial_field_snapshot_save(levels[i], &snapshots[i]);
    }

    bool rebuilt = celestial_light_rebuild(map, absolute_hour + 1);
    if (rebuilt) {
        for (size_t i = 0; i < count; i++) {
            celestial_dependencies *next = celestial_dependencies_field_get(levels[i], false);
            rebuilt &= next != NULL && celestial_dependencies_field_bind(next, levels[i], true);
            celestial_dependencies_release(next);
            for (size_t cell = 0; cell < snapshots[i].count; cell++) {
                levels[i]->spaces[cell].celestial_light_next_value =
                    levels[i]->spaces[cell].celestial_light_value;
                memcpy(levels[i]->spaces[cell].celestial_light_next_rgb,
                       levels[i]->spaces[cell].celestial_light_rgb,
                       sizeof(levels[i]->spaces[cell].celestial_light_next_rgb));
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        rebuilt &= celestial_field_snapshot_restore(&snapshots[i]);
        levels[i]->celestial_light_keyframe_valid = rebuilt;
        levels[i]->celestial_light_next_key = key_for_hour(levels[i], absolute_hour + 1);
        levels[i]->celestial_light_next_hour = absolute_hour + 1;
    }
    return rebuilt;
}

void celestial_light_ensure(mapstruct *map) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL) {
        return;
    }
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t phases;
    region_celestial_phases(profile, (uint64_t)todtick, &phases);
    uint64_t key = celestial_key(map, profile, &phases);
    if (!map->celestial_light_valid || map->celestial_light_key != key ||
        !celestial_dependencies_field_fresh(map, false)) {
        mapstruct *levels[MAP2_LEVELS] = {0};
        size_t count = 0;
        bool ready = collect_stack(map, levels, &count);
        for (size_t i = 0; i < count; i++) {
            ready &= celestial_dependencies_field_fresh(levels[i], true) &&
                 levels[i]->celestial_light_keyframe_valid &&
                     levels[i]->celestial_light_next_hour == (uint64_t)todtick &&
                     levels[i]->celestial_light_next_key == key_for_hour(levels[i], (uint64_t)todtick);
        }
        celestial_light_forget(map);
        if (!ready) {
            celestial_light_rebuild(map, (uint64_t)todtick);
            return;
        }
        /* Promotion is a complete server-thread publication. No solver work
         * is repeated at the ordinary hour boundary. */
        for (size_t i = 0; i < count; i++) {
            mapstruct *current = levels[i];
            for (size_t cell = 0; cell < (size_t)current->width * current->height; cell++) {
                current->spaces[cell].celestial_light_value = current->spaces[cell].celestial_light_next_value;
                memcpy(current->spaces[cell].celestial_light_rgb,
                       current->spaces[cell].celestial_light_next_rgb,
                       sizeof(current->spaces[cell].celestial_light_rgb));
            }
            if (current->celestial_light_generation_id == UINT64_MAX) {
                IMPOSSIBLE();
            }
            current->celestial_light_generation_id++;
            celestial_dependencies *next = celestial_dependencies_field_get(current, true);
            bool bound = next != NULL && celestial_dependencies_field_bind(next, current, false);
            celestial_dependencies_release(next);
            if (!bound) {
                IMPOSSIBLE();
            }
            celestial_dependencies_field_release(current, true);
            current->celestial_light_key = current->celestial_light_next_key;
            current->celestial_light_valid = true;
            current->celestial_light_keyframe_valid = false;
        }
    }
}
