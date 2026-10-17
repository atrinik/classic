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
#include <region.h>
#include <server_main.h>
#include <spatial.h>
#include <tod.h>
#include <toolkit/celestial_model.h>
#include <toolkit/datetime.h>

#include <stdint.h>

#define CELESTIAL_SCALE CELESTIAL_MODEL_SCALE
#define CELESTIAL_DIRECT_REACH CELESTIAL_MODEL_DIRECT_REACH
#define CELESTIAL_SPILL_PASSES CELESTIAL_MODEL_SPILL_PASSES

/* All solve allocations, including dependency snapshots, share one ceiling. */
#define CELESTIAL_PENDING_MEMORY_MAX ((size_t)1923584)
#define CELESTIAL_CURRENT_SCRATCH_MAX ((size_t)45 * 64 * 64)
static size_t pending_memory, pending_memory_limit = CELESTIAL_PENDING_MEMORY_MAX;
static celestial_dependencies *active_dependencies;

static void celestial_spatial_observe(const mapstruct *map, void *context) {
    if (map != NULL) (void)celestial_dependencies_observe(context, map);
}

static bool celestial_observe(const mapstruct *map) {
    return map == NULL || active_dependencies == NULL ||
           celestial_dependencies_observe(active_dependencies, map);
}

static bool celestial_observe_event(const spatial_event *event) {
    for (size_t i = 0; i < event->face_count; i++) {
        if (!celestial_observe(event->faces[i].from.map) ||
            !celestial_observe(event->faces[i].to.map)) return false;
    }
    return true;
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

static int32_t transmit(int32_t value, uint16_t coefficient) {
    return celestial_model_transmit(value, coefficient);
}

static void direction_delta(uint8_t direction, int *dx, int *dy) {
    *dx = 0;
    *dy = 0;
    switch (direction) {
    case NORTH:
        *dy = 1;
        break;
    case NORTHEAST:
        *dx = -1;
        *dy = 1;
        break;
    case EAST:
        *dx = -1;
        break;
    case SOUTHEAST:
        *dx = -1;
        *dy = -1;
        break;
    case SOUTH:
        *dy = -1;
        break;
    case SOUTHWEST:
        *dx = 1;
        *dy = -1;
        break;
    case WEST:
        *dx = 1;
        break;
    case NORTHWEST:
        *dx = 1;
        *dy = 1;
        break;
    default:
        HARD_ASSERT(false);
    }
}

/* A backtrace has at most 32 horizontal events and thirteen vertical exits.
 * Store coefficients rather than pre-rounded gains: radiance rounds separately
 * at each physical crossing in source-to-receiver order. */
#define CELESTIAL_RAY_EVENTS (CELESTIAL_DIRECT_REACH + MAP2_LEVELS)
typedef struct celestial_ray {
    spatial_cursor cursor;
    uint16_t coefficients[CELESTIAL_RAY_EVENTS];
    bool apertures[CELESTIAL_RAY_EVENTS];
    unsigned count, horizontal;
    bool done, sky;
} celestial_ray;

static uint32_t celestial_sqrt(uint64_t value) {
    uint64_t root = 0, bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

static void celestial_ray_init(celestial_ray *ray, mapstruct *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    memset(ray, 0, sizeof(*ray));
    if (elevation <= 0) {
        ray->done = true;
        return;
    }
    int dx, dy;
    direction_delta(direction, &dx, &dy);
    uint64_t horizontal_squared = UINT64_C(32768) * 32768 -
                                  (uint64_t)elevation * elevation;
    uint32_t horizontal = celestial_sqrt(horizontal_squared /
                                        (dx != 0 && dy != 0 ? 2 : 1));
    /* Azimuth names the source; direction_delta names light travel. */
    ray->done = !celestial_observe(map) || !spatial_direction_init(&ray->cursor, map, x, y,
                                        -dx * (int)horizontal, -dy * (int)horizontal,
                                        elevation, CELESTIAL_RAY_EVENTS);
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

/* One bounded crossing per call, also used by the runtime deadline scheduler. */
static void celestial_ray_step(celestial_ray *ray) {
    if (ray->done) return;
    spatial_event event;
    if (spatial_ray_next(&ray->cursor, &event) != 1) {
        ray->done = true;
        return;
    }
    if (!celestial_observe_event(&event)) {
        ray->done = true;
        return;
    }
    if ((event.axes & 3) != 0 && ++ray->horizontal > CELESTIAL_DIRECT_REACH) {
        ray->done = true;
        return;
    }
    uint16_t coefficient = CELESTIAL_SCALE;
    bool aperture = false, sky = false;
    for (size_t i = 0; i < event.face_count; i++) {
        spatial_face face = event.faces[i];
        if (face.to.map == NULL && face.normal[2] > 0 &&
            celestial_sky_exit(&face.from)) {
            sky = true;
            continue;
        }
        if (!celestial_face_continuous(&face)) {
            ray->done = true;
            return;
        }
        spatial_cell swap = face.from;
        face.from = face.to;
        face.to = swap;
        for (size_t axis = 0; axis < 3; axis++) face.normal[axis] = -face.normal[axis];
        bool crossed_aperture = false;
        coefficient = MIN(coefficient, spatial_face_light(&face, &crossed_aperture));
        aperture |= crossed_aperture;
    }
    if (ray->count == CELESTIAL_RAY_EVENTS || coefficient == 0) {
        ray->done = true;
        return;
    }
    ray->coefficients[ray->count] = coefficient;
    ray->apertures[ray->count++] = aperture;
    if (sky) {
        ray->sky = true;
        ray->done = true;
    }
}

static int32_t celestial_ray_value(const celestial_ray *ray, int32_t source,
                                   int32_t *aperture) {
    return celestial_model_ray_value(ray->coefficients, ray->apertures, ray->count, ray->sky,
                                     source, aperture);
}

/* Four spill cells around the output plus each seed's 32-cell directional
 * backtrace give the frozen 36-cell dependency halo. Only one level's seed
 * buffers exist at once, including for thirteen-member stacks. */
static int celestial_scratch_width(const mapstruct *map) {
    return map->width + 2 * CELESTIAL_SPILL_PASSES;
}

static int celestial_scratch_height(const mapstruct *map) {
    return map->height + 2 * CELESTIAL_SPILL_PASSES;
}

static size_t celestial_scratch_cells(const mapstruct *map) {
    return (size_t)celestial_scratch_width(map) * celestial_scratch_height(map);
}

static bool celestial_halo_cell(mapstruct *map, int x, int y, spatial_cell *cell) {
    if (!celestial_observe(map)) return false;
    x -= CELESTIAL_SPILL_PASSES;
    y -= CELESTIAL_SPILL_PASSES;
    int start_x = MAX(0, MIN(x, map->width - 1));
    int start_y = MAX(0, MIN(y, map->height - 1));
    if (start_x != x || start_y != y) {
        spatial_cursor cursor;
        if (!spatial_ray_init(&cursor, map, start_x, start_y, x - start_x, y - start_y, 0))
            return false;
        spatial_event event;
        int status;
        while ((status = spatial_ray_next(&cursor, &event)) == 1) {
            if (!celestial_observe_event(&event) || !event.valid) return false;
            for (size_t face = 0; face < event.face_count; face++) {
                if (!celestial_face_continuous(&event.faces[face])) return false;
            }
        }
        if (status != 0) return false;
    }
    return spatial_resolve(map, x, y, 0, cell) && celestial_observe(cell->map);
}

static void celestial_halo_ray(celestial_ray *ray, mapstruct *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    spatial_cell cell;
    if (!celestial_halo_cell(map, x, y, &cell)) {
        memset(ray, 0, sizeof(*ray));
        ray->done = true;
        return;
    }
    celestial_ray_init(ray, cell.map, cell.x, cell.y, direction, elevation);
}

static uint16_t celestial_spill_coefficient(mapstruct *map, int x, int y, int dx, int dy) {
    spatial_cell cell;
    if (!celestial_halo_cell(map, x + dx, y + dy, &cell)) return 0;
    spatial_cursor cursor;
    spatial_event event;
    if (!spatial_ray_init(&cursor, cell.map, cell.x, cell.y, -dx, -dy, 0) ||
        spatial_ray_next(&cursor, &event) != 1 ||
        !celestial_observe_event(&event) || !event.valid) return 0;
    for (size_t face = 0; face < event.face_count; face++) {
        if (!celestial_face_continuous(&event.faces[face])) return 0;
    }
    return spatial_event_light(&event);
}

static void transport_line(mapstruct *map, const celestial_model *model,
                           int32_t *values, int32_t *apertures, bool moon) {
    int width = celestial_scratch_width(map), height = celestial_scratch_height(map);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            celestial_ray ray;
            celestial_halo_ray(&ray, map, x, y,
                               moon ? model->moon_direction : model->solar_direction,
                               moon ? model->moon_elevation : model->solar_elevation);
            while (!ray.done) celestial_ray_step(&ray);
            int index = x + width * y;
            values[index] = celestial_ray_value(&ray, moon ? model->moon : model->direct,
                                                 &apertures[index]);
        }
    }
}

static uint16_t relax_spill(int x, int y, int dx, int dy, void *context) {
    return celestial_spill_coefficient(context, x, y, dx, dy);
}

static void relax_diffuse(mapstruct *map, int32_t *values) {
    if (!celestial_model_relax(values, celestial_scratch_width(map), celestial_scratch_height(map),
                               relax_spill, map)) {
        LOG(ERROR, "Out of memory relaxing the celestial field.");
        abort();
    }
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

static void publish_map_field(mapstruct *map,
                              const celestial_model *model,
                              int32_t *direct,
                              int32_t *diffuse,
                              int32_t *aperture_direct,
                              int32_t *moon,
                              int32_t *moon_aperture,
                              int32_t *starlight) {
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            int index = x + CELESTIAL_SPILL_PASSES + celestial_scratch_width(map) *
                        (y + CELESTIAL_SPILL_PASSES);
            int32_t value, rgb[3];
            celestial_model_publish(model, direct[index], diffuse[index], aperture_direct[index],
                                    moon[index], moon_aperture[index], starlight[index], &value,
                                    rgb);
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            space->celestial_light_value = value;
            memcpy(space->celestial_light_rgb, rgb, sizeof(rgb));
        }
    }
}

void celestial_light_invalidate(mapstruct *map) {
    if (map == NULL) {
        return;
    }
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

    /* The synchronous fallback is server-thread exclusive. Its existing
     * reserve covers the largest one-level halo plus dependency bookkeeping. */
    size_t scratch = 28 * celestial_scratch_cells(map);
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
        size_t cells = celestial_scratch_cells(current);
        celestial_model model;
        celestial_model_for_map(current, absolute_hour, &model);
        int32_t *direct = xcalloc(cells, sizeof(*direct));
        int32_t *diffuse = xcalloc(cells, sizeof(*diffuse));
        int32_t *aperture_direct = xcalloc(cells, sizeof(*aperture_direct));
        int32_t *moon = xcalloc(cells, sizeof(*moon));
        int32_t *moon_aperture = xcalloc(cells, sizeof(*moon_aperture));
        int32_t *starlight = xcalloc(cells, sizeof(*starlight));
        for (int y = 0; y < celestial_scratch_height(current); y++) {
            for (int x = 0; x < celestial_scratch_width(current); x++) {
                int index = x + celestial_scratch_width(current) * y;
                celestial_ray ray;
                celestial_halo_ray(&ray, current, x, y, NORTH, 32768);
                while (!ray.done) celestial_ray_step(&ray);
                diffuse[index] = celestial_ray_value(&ray, model.diffuse, NULL);
                starlight[index] = celestial_ray_value(&ray, model.starlight, NULL);
            }
        }

        region_celestial_phases_t phases;
        region_celestial_phases(region_celestial_for_map(current), absolute_hour, &phases);
        transport_line(current, &model, direct, aperture_direct, false);
        if (model.moon != 0) {
            transport_line(current, &model, moon, moon_aperture, true);
        }
        relax_diffuse(current, diffuse);
        relax_diffuse(current, aperture_direct);
        relax_diffuse(current, starlight);
        publish_map_field(current,
                          &model,
                          direct,
                          diffuse,
                          aperture_direct,
                          moon,
                          moon_aperture,
                          starlight);

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
        free(starlight);
        free(moon);
        free(moon_aperture);
        free(aperture_direct);
        free(diffuse);
        free(direct);
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
