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
#include <celestial_override.h>
#include <celestial_structure.h>
#include <initialization.h>
#include <light.h>
#include <map.h>
#include <object.h>
#include <region.h>
#include <server_main.h>
#include <tod.h>
#include <toolkit/datetime.h>

#include <stdint.h>

#define CELESTIAL_SCALE 256
#define CELESTIAL_DIRECT_REACH 32
#define CELESTIAL_SPILL_PASSES 4

#ifdef ATRINIK_TESTING
static uint64_t rebuild_count;
uint64_t celestial_light_rebuilds_for_test(void) {
    return rebuild_count;
}
#endif

typedef struct celestial_model {
    int32_t direct;
    int32_t diffuse;
    int32_t moon;
    int32_t starlight;
    uint16_t solar_color[3];
    uint16_t moon_color[3];
    uint16_t starlight_color[3];
    uint16_t solar_brightness;
    uint8_t solar_direction;
    uint8_t moon_direction;
} celestial_model;

static const int32_t solar_elevation[HOURS_PER_DAY] = {
    -32768, -31651, -28378, -23170, -16384, -8481, 0, 8481,  16384, 23170, 28378, 31651,
    32768, 31651, 28378, 23170, 16384, 8481,  0, -8481, -16384, -23170, -28378, -31651,
};

static const uint8_t solar_azimuth[HOURS_PER_DAY] = {
    NORTH, NORTH, NORTHEAST, NORTHEAST, NORTHEAST, EAST, EAST, EAST,
    SOUTHEAST, SOUTHEAST, SOUTH, SOUTH, SOUTH, SOUTH, SOUTH, SOUTHWEST,
    SOUTHWEST, WEST, WEST, WEST, NORTHWEST, NORTHWEST, NORTHWEST, NORTH,
};

static const uint16_t season_factor[MONTHS_PER_YEAR] = {
    24576, 25600, 27648, 29952, 31744, 32768,
    32768, 31744, 29952, 27648, 25600, 24576,
};

static uint64_t celestial_round(uint64_t numerator, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    return numerator / denominator +
           (numerator % denominator >= (denominator + 1) / 2 ? 1 : 0);
}

static int32_t celestial_scale_value(int32_t value, uint32_t scale, uint32_t denominator) {
    if (value <= 0 || scale == 0) {
        return 0;
    }
    return (int32_t)celestial_round((uint64_t)value * scale, denominator);
}

static int32_t solar_elevation_at(uint16_t solar_hour, uint16_t season_phase) {
    int32_t elevation = solar_elevation[solar_hour];
    uint16_t factor = season_factor[season_phase / HOURS_PER_MONTH];
    uint64_t magnitude = celestial_round((uint64_t)abs(elevation) * factor, 32768);
    return elevation < 0 ? -(int32_t)magnitude : (int32_t)magnitude;
}

static void celestial_model_for_map(const mapstruct *map,
                                    uint64_t absolute_hour,
                                    celestial_model *model) {
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t phases;
    celestial_lunar_input lunar_input;
    celestial_lunar_sample lunar;

    memset(model, 0, sizeof(*model));
    region_celestial_phases(profile, absolute_hour, &phases);
    model->solar_direction = solar_azimuth[phases.solar];

    int32_t elevation = solar_elevation_at(phases.solar, phases.season);
    uint16_t factor = season_factor[phases.season / HOURS_PER_MONTH];
    int32_t twilight = -(int32_t)celestial_round(UINT64_C(8481) * factor, 32768);
    if (elevation > 0) {
        model->direct = celestial_scale_value(elevation, 960, 32768);
        model->diffuse = 64 + celestial_scale_value(elevation, 256, 32768);
    } else if (elevation == 0) {
        model->diffuse = 64;
    } else if (elevation >= twilight) {
        model->diffuse = 16;
    }

    uint32_t daylight = (uint32_t)MAX(elevation, 0);
    uint32_t inverse = 32768 - MIN(daylight, 32768U);
    uint64_t brightness_numerator =
        (uint64_t)profile->night_brightness * inverse +
        (uint64_t)profile->day_brightness * MIN(daylight, 32768U);
    uint16_t brightness = (uint16_t)celestial_round(brightness_numerator, 32768);
    for (size_t channel = 0; channel < 3; channel++) {
        uint64_t color_numerator =
            (uint64_t)profile->night_linear[channel] * inverse +
            (uint64_t)profile->day_linear[channel] * MIN(daylight, 32768U);
        uint16_t color = (uint16_t)celestial_round(color_numerator, 32768);
        model->solar_color[channel] = color;
    }
    model->solar_brightness = brightness;

    region_celestial_lunar_input(profile, absolute_hour, &lunar_input);
    (void)celestial_override_apply(profile->lunar_period, &lunar_input.lunar_age);
    if (!celestial_lunar_evaluate(&lunar_input, &lunar)) {
        return;
    }
    model->moon_direction = lunar.azimuth;
    model->moon = lunar.moon_strength;
    model->starlight = lunar.starlight_strength;
    memcpy(model->moon_color, profile->moon_linear, sizeof(model->moon_color));
    memcpy(model->starlight_color,
           profile->starlight_linear,
           sizeof(model->starlight_color));
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

static uint8_t incoming_face(int dx, int dy);

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

static uint16_t step_coefficient(const mapstruct *map, int x, int y, int dx, int dy,
                                 bool *aperture) {
    if (dx != 0 && dy != 0) {
        bool horizontal_aperture = false;
        bool vertical_aperture = false;
        uint16_t horizontal = edge_coefficient(
            map, x, y, dx < 0 ? CELESTIAL_FACE_EAST : CELESTIAL_FACE_WEST,
            &horizontal_aperture);
        uint16_t vertical = edge_coefficient(
            map, x, y, dy < 0 ? CELESTIAL_FACE_SOUTH : CELESTIAL_FACE_NORTH,
            &vertical_aperture);
        if (aperture != NULL) {
            *aperture = horizontal_aperture || vertical_aperture;
        }
        return MIN(horizontal, vertical);
    }
    return edge_coefficient(map, x, y, incoming_face(dx, dy), aperture);
}

static int32_t transmit(int32_t value, uint16_t coefficient) {
    return (int32_t)celestial_round((uint64_t)MAX(value, 0) * coefficient, CELESTIAL_SCALE);
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

static uint8_t incoming_face(int dx, int dy) {
    if (dx < 0) {
        return CELESTIAL_FACE_EAST;
    }
    if (dx > 0) {
        return CELESTIAL_FACE_WEST;
    }
    if (dy < 0) {
        return CELESTIAL_FACE_SOUTH;
    }
    return CELESTIAL_FACE_NORTH;
}

static int32_t *cell_at(int32_t *values, const mapstruct *map, int x, int y) {
    return &values[x + map->width * y];
}

static void transport_line(const mapstruct *map,
                           const uint8_t *exposed,
                           const celestial_model *model,
                           int32_t *values,
                           int32_t *aperture_values,
                           uint8_t direction,
                           bool use_moon) {
    int dx, dy;
    direction_delta(direction, &dx, &dy);
    int width = map->width;
    int height = map->height;
    int starts = (dx != 0 ? height : width) + (dy != 0 ? width : 0);

    for (int start = 0; start < starts; start++) {
        int x;
        int y;
        if (dx != 0 && start < height) {
            x = dx < 0 ? width - 1 : 0;
            y = start;
        } else if (dy != 0) {
            int offset = dx != 0 ? start - height : start;
            x = offset;
            y = dy < 0 ? height - 1 : 0;
            if (dx != 0 && (x < 0 || x >= width)) {
                continue;
            }
            if (dx == 0 && offset >= width) {
                continue;
            }
        } else {
            continue;
        }

        int32_t direct = 0;
        int32_t aperture_direct = 0;
        uint8_t cooldown = 0;
        while (x >= 0 && y >= 0 && x < width && y < height) {
            int index = x + width * y;
            if (cooldown == 0 && exposed[index]) {
                direct = use_moon ? model->moon : model->direct;
            }
            *cell_at(values, map, x, y) = MAX(*cell_at(values, map, x, y), direct);
            aperture_values[index] = MAX(aperture_values[index], aperture_direct);

            bool aperture = false;
            uint16_t coefficient = step_coefficient(map, x, y, dx, dy, &aperture);
            direct = transmit(direct, coefficient);
            aperture_direct = transmit(aperture_direct, coefficient);
            if (aperture) {
                aperture_direct = direct;
            }
            if (coefficient < CELESTIAL_SCALE) {
                cooldown = CELESTIAL_DIRECT_REACH;
            } else if (cooldown != 0) {
                cooldown--;
            }
            x += dx;
            y += dy;
        }
    }
}

static void relax_diffuse(const mapstruct *map, int32_t *values) {
    size_t count = (size_t)map->width * map->height;
    int32_t *next = xcalloc(count, sizeof(*next));
    int32_t *original = values;

    for (int pass = 0; pass < CELESTIAL_SPILL_PASSES; pass++) {
        memcpy(next, values, count * sizeof(*next));
        for (int y = 0; y < map->height; y++) {
            for (int x = 0; x < map->width; x++) {
                int32_t best = values[x + map->width * y];
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                            x + dx >= map->width || y + dy >= map->height) {
                            continue;
                        }
                        uint16_t coefficient = step_coefficient(map, x, y, dx, dy, NULL);
                        int32_t candidate = transmit(
                            values[(x + dx) + map->width * (y + dy)], coefficient);
                        candidate = transmit(candidate, dx != 0 && dy != 0 ? 181 : 192);
                        best = MAX(best, candidate);
                    }
                }
                next[x + map->width * y] = best;
            }
        }
        int32_t *swap = values;
        values = next;
        next = swap;
    }

    if (values != original) {
        memcpy(original, values, count * sizeof(*original));
        free(values);
    } else {
        free(next);
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
    MIX_CELESTIAL(profile->revision);
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
        MIX_CELESTIAL((uintptr_t)map->tile_map[i]);
        MIX_CELESTIAL(map->tile_map[i] != NULL ? map->tile_map[i]->in_memory : 0);
    }
#undef MIX_CELESTIAL
    return hash;
}

static bool collect_stack(mapstruct *map, mapstruct *levels[MAP2_LEVELS], size_t *count) {
    /* Sky exposure and injection flow down from the resolved upper maps.
     * A lower map cannot affect this map, and may be hidden/unloaded beneath
     * an opaque floor. Solve only the requested map and its upper chain. */
    mapstruct *cursor = map;
    while (cursor != NULL) {
        if (*count >= MAP2_LEVELS || cursor->in_memory != MAP_IN_MEMORY) {
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

static void publish_map_field(mapstruct *map,
                              const celestial_model *model,
                              const uint8_t *exposed,
                              int32_t *direct,
                              int32_t *diffuse,
                              int32_t *aperture_direct,
                              int32_t *moon,
                              int32_t *moon_aperture,
                              int32_t *starlight,
                              const int32_t *injected,
                              const int32_t injected_rgb[][3]) {
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            int index = x + map->width * y;
            int32_t solar = celestial_scale_value(direct[index] + diffuse[index] +
                                                      aperture_direct[index],
                                                  model->solar_brightness,
                                                  256);
            int32_t moon_value = moon[index] + moon_aperture[index];
            int32_t value = solar + moon_value + starlight[index];
            MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
            int32_t rgb[3] = {0, 0, 0};
            for (size_t channel = 0; channel < 3; channel++) {
                rgb[channel] = celestial_scale_value(solar,
                                                      model->solar_color[channel],
                                                      UINT16_MAX);
                rgb[channel] += celestial_scale_value(moon_value,
                                                       model->moon_color[channel],
                                                       UINT16_MAX);
                rgb[channel] += celestial_scale_value(starlight[index],
                                                       model->starlight_color[channel],
                                                       UINT16_MAX);
            }
            if (!exposed[index] && injected[index] > value) {
                value = injected[index];
                memcpy(rgb, injected_rgb[index], sizeof(rgb));
            }
            space->celestial_light_value = value;
            memcpy(space->celestial_light_rgb, rgb, sizeof(rgb));
        }
    }
}

void celestial_light_invalidate(mapstruct *map) {
    if (map == NULL) {
        return;
    }
    celestial_light_forget(map);
    mapstruct *cursor = map;
    for (size_t i = 0; i < MAP2_LEVELS && cursor != NULL; i++) {
        if (cursor->celestial_structure_revision == UINT64_MAX) {
            HARD_ASSERT(false);
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
            HARD_ASSERT(false);
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

    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    char error[HUGE_BUF];
    if (!collect_stack(map, levels, &count) ||
        !celestial_structure_validate_light_dependencies(map, VS(error))) {
        for (size_t i = 0; i < count; i++) {
            mapstruct *current = levels[i];
            clear_map_field(current);
            const region_celestial_profile_t *profile = region_celestial_for_map(current);
            region_celestial_phases_t phases;
            region_celestial_phases(profile, absolute_hour, &phases);
            uint64_t key = celestial_key(current, profile, &phases);
            if (current->celestial_light_generation_id == 0) {
                current->celestial_light_generation_id = 1;
            } else if (!current->celestial_light_valid || current->celestial_light_key != key) {
                if (current->celestial_light_generation_id == UINT64_MAX) {
                    HARD_ASSERT(false);
                }
                current->celestial_light_generation_id++;
            }
            current->celestial_light_key = key;
            current->celestial_light_valid = true;
            current->celestial_light_keyframe_valid = false;
        }
        return false;
    }

    for (size_t level = count; level-- > 0;) {
        mapstruct *current = levels[level];
        size_t cells = (size_t)current->width * current->height;
        celestial_model model;
        celestial_model_for_map(current, absolute_hour, &model);
        uint8_t *exposed = xcalloc(cells, sizeof(*exposed));
        int32_t *direct = xcalloc(cells, sizeof(*direct));
        int32_t *diffuse = xcalloc(cells, sizeof(*diffuse));
        int32_t *aperture_direct = xcalloc(cells, sizeof(*aperture_direct));
        int32_t *moon = xcalloc(cells, sizeof(*moon));
        int32_t *moon_aperture = xcalloc(cells, sizeof(*moon_aperture));
        int32_t *starlight = xcalloc(cells, sizeof(*starlight));
        int32_t *injected = xcalloc(cells, sizeof(*injected));
        int32_t(*injected_rgb)[3] = xcalloc(cells, sizeof(*injected_rgb));

        mapstruct *upper = level + 1 < count ? levels[level + 1] : NULL;
        for (int y = 0; y < current->height; y++) {
            for (int x = 0; x < current->width; x++) {
                int index = x + current->width * y;
                exposed[index] = celestial_structure_cell_exposed(current, x, y);
                if (exposed[index]) {
                    diffuse[index] = model.diffuse;
                    starlight[index] = model.starlight;
                } else if (upper != NULL && x < upper->width && y < upper->height) {
                    const MapSpace *above = GET_MAP_SPACE_PTR(upper, x, y);
                    uint16_t coefficient = edge_coefficient(upper, x, y, CELESTIAL_FACE_DOWN, NULL);
                    injected[index] = transmit(above->celestial_light_value, coefficient);
                    for (size_t channel = 0; channel < 3; channel++) {
                        injected_rgb[index][channel] =
                            transmit(above->celestial_light_rgb[channel], coefficient);
                    }
                }
            }
        }

        region_celestial_phases_t phases;
        region_celestial_phases(region_celestial_for_map(current), absolute_hour, &phases);
        transport_line(current,
                       exposed,
                       &model,
                       direct,
                       aperture_direct,
                       model.solar_direction,
                       false);
        if (model.moon != 0) {
            transport_line(current,
                           exposed,
                           &model,
                           moon,
                           moon_aperture,
                           model.moon_direction,
                           true);
        }
        relax_diffuse(current, diffuse);
        relax_diffuse(current, aperture_direct);
        relax_diffuse(current, starlight);
        publish_map_field(current,
                          &model,
                          exposed,
                          direct,
                          diffuse,
                          aperture_direct,
                          moon,
                          moon_aperture,
                          starlight,
                          injected,
                          injected_rgb);

        const region_celestial_profile_t *profile = region_celestial_for_map(current);
        uint64_t previous_key = current->celestial_light_key;
        bool previous_valid = current->celestial_light_valid;
        current->celestial_light_key = celestial_key(current, profile, &phases);
        if (current->celestial_light_generation_id == 0) {
            current->celestial_light_generation_id = 1;
        } else if (!previous_valid || previous_key != current->celestial_light_key) {
            if (current->celestial_light_generation_id == UINT64_MAX) {
                HARD_ASSERT(false);
            }
            current->celestial_light_generation_id++;
        }
        current->celestial_light_valid = true;
        current->celestial_light_keyframe_valid = false;
        free(injected_rgb);
        free(injected);
        free(starlight);
        free(moon);
        free(moon_aperture);
        free(aperture_direct);
        free(diffuse);
        free(direct);
        free(exposed);
    }
    return true;
}

typedef struct celestial_field_snapshot {
    mapstruct *map;
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

static void celestial_field_snapshot_restore(celestial_field_snapshot_t *snapshot) {
    for (size_t i = 0; i < snapshot->count; i++) {
        snapshot->map->spaces[i].celestial_light_value = snapshot->value[i];
        memcpy(snapshot->map->spaces[i].celestial_light_rgb,
               snapshot->rgb[i],
               sizeof(snapshot->rgb[i]));
    }
    snapshot->map->celestial_light_key = snapshot->key;
    snapshot->map->celestial_light_generation_id = snapshot->generation;
    snapshot->map->celestial_light_valid = snapshot->valid;
    free(snapshot->value);
    free(snapshot->rgb);
    snapshot->value = NULL;
    snapshot->rgb = NULL;
}

uint64_t celestial_light_generation(const mapstruct *map) {
    if (map == NULL || !map->celestial_light_valid || map->celestial_light_generation_id == 0) {
        return 0;
    }
    return map->celestial_light_generation_id;
}

/* A queued solve never mutates the current field. Each step evaluates one
 * cell (or advances one bounded stage), with the original ray order and Jacobi
 * pass order. Upper-level injection reads this job's completed scratch field,
 * rather than a published field for a different hour. All work is server-thread
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
    celestial_pending_level_t levels[MAP2_LEVELS];
    celestial_pending_stage_t stage;
    celestial_model model;
    uint8_t *exposed;
    int32_t *direct, *diffuse, *aperture, *moon, *moon_aperture, *stars, *next_values;
    int pass, ray_start, ray_x, ray_y;
    int32_t ray_direct, ray_aperture;
    uint8_t ray_cooldown;
    bool ray_active;
} celestial_pending_t;

static celestial_pending_t *pending_head, *pending_tail;

/* Includes all output/scratch arrays and job descriptors, across all jobs. */
#define CELESTIAL_PENDING_MEMORY_MAX ((size_t)1923584)
#define CELESTIAL_CURRENT_SCRATCH_MAX ((size_t)45 * 64 * 64)
static size_t pending_memory, pending_memory_limit = CELESTIAL_PENDING_MEMORY_MAX;

static void pending_free_scratch(celestial_pending_t *job) {
    free(job->exposed);
    free(job->direct);
    free(job->diffuse);
    free(job->aperture);
    free(job->moon);
    free(job->moon_aperture);
    free(job->stars);
    free(job->next_values);
    job->exposed = NULL;
    job->direct = job->diffuse = job->aperture = NULL;
    job->moon = job->moon_aperture = job->stars = job->next_values = NULL;
}

static void pending_free(celestial_pending_t *job) {
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
        bool retained = false;
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
    if (job->source_hour != (uint64_t)todtick) {
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
    job->ray_start = 0;
    job->ray_active = false;
}

static bool pending_ray_step(celestial_pending_t *job, bool moon) {
    celestial_pending_level_t *level = &job->levels[job->level];
    int dx, dy;
    direction_delta(moon ? job->model.moon_direction : job->model.solar_direction, &dx, &dy);
    /* The synchronous traversal skips the second width of starts when
     * dx == 0. Enumerate only those rays that actually enter the map. */
    int starts = dx == 0 ? level->width
                         : level->height + (dy != 0 ? level->width : 0);
    if (!job->ray_active) {
        if (job->ray_start == starts) {
            return true;
        }
        int start = job->ray_start++;
        if (dx != 0 && start < level->height) {
            job->ray_x = dx < 0 ? level->width - 1 : 0;
            job->ray_y = start;
        } else {
            job->ray_x = dx != 0 ? start - level->height : start;
            job->ray_y = dy < 0 ? level->height - 1 : 0;
        }
        job->ray_direct = job->ray_aperture = 0;
        job->ray_cooldown = 0;
        job->ray_active = true;
    }
    int index = job->ray_x + level->width * job->ray_y;
    if (job->ray_cooldown == 0 && job->exposed[index]) {
        job->ray_direct = moon ? job->model.moon : job->model.direct;
    }
    int32_t *values = moon ? job->moon : job->direct;
    int32_t *apertures = moon ? job->moon_aperture : job->aperture;
    values[index] = MAX(values[index], job->ray_direct);
    apertures[index] = MAX(apertures[index], job->ray_aperture);
    bool aperture = false;
    uint16_t coefficient = step_coefficient(level->map, job->ray_x, job->ray_y, dx, dy, &aperture);
    job->ray_direct = transmit(job->ray_direct, coefficient);
    job->ray_aperture = transmit(job->ray_aperture, coefficient);
    if (aperture) {
        job->ray_aperture = job->ray_direct;
    }
    if (coefficient < CELESTIAL_SCALE) {
        job->ray_cooldown = CELESTIAL_DIRECT_REACH;
    } else if (job->ray_cooldown != 0) {
        job->ray_cooldown--;
    }
    job->ray_x += dx;
    job->ray_y += dy;
    job->ray_active = job->ray_x >= 0 && job->ray_y >= 0 &&
                      job->ray_x < level->width && job->ray_y < level->height;
    return false;
}

static void pending_relax_step(celestial_pending_t *job, int32_t **values) {
    celestial_pending_level_t *level = &job->levels[job->level];
    int x = (int)(job->cell % level->width);
    int y = (int)(job->cell / level->width);
    int32_t best = (*values)[job->cell];
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                x + dx >= level->width || y + dy >= level->height) {
                continue;
            }
            uint16_t coefficient = step_coefficient(level->map, x, y, dx, dy, NULL);
            int32_t candidate = transmit((*values)[x + dx + level->width * (y + dy)], coefficient);
            candidate = transmit(candidate, dx != 0 && dy != 0 ? 181 : 192);
            best = MAX(best, candidate);
        }
    }
    job->next_values[job->cell++] = best;
    if (job->cell == level->cells) {
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
        celestial_model_for_map(level->map, job->source_hour + 1, &job->model);
        job->exposed = xcalloc(level->cells, sizeof(*job->exposed));
        job->direct = xcalloc(level->cells, sizeof(*job->direct));
        job->diffuse = xcalloc(level->cells, sizeof(*job->diffuse));
        job->aperture = xcalloc(level->cells, sizeof(*job->aperture));
        job->moon = xcalloc(level->cells, sizeof(*job->moon));
        job->moon_aperture = xcalloc(level->cells, sizeof(*job->moon_aperture));
        job->stars = xcalloc(level->cells, sizeof(*job->stars));
        job->next_values = xcalloc(level->cells, sizeof(*job->next_values));
        level->value = xcalloc(level->cells, sizeof(*level->value));
        level->rgb = xcalloc(level->cells, sizeof(*level->rgb));
        job->stage = CELESTIAL_EXPOSE;
        break;
    case CELESTIAL_EXPOSE:
        job->exposed[job->cell] = celestial_structure_cell_exposed(level->map, x, y);
        if (job->exposed[job->cell]) {
            job->diffuse[job->cell] = job->model.diffuse;
            job->stars[job->cell] = job->model.starlight;
        }
        if (++job->cell == level->cells) {
            job->cell = 0;
            job->stage = CELESTIAL_SUN;
            pending_ray_reset(job);
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
        pending_relax_step(job, &job->diffuse);
        break;
    case CELESTIAL_APERTURE:
        pending_relax_step(job, &job->aperture);
        break;
    case CELESTIAL_STARS:
        pending_relax_step(job, &job->stars);
        break;
    case CELESTIAL_OUTPUT: {
        size_t cell = job->cell;
        int32_t solar = celestial_scale_value(job->direct[cell] + job->diffuse[cell] +
                                                 job->aperture[cell],
                                             job->model.solar_brightness, 256);
        int32_t moon = job->moon[cell] + job->moon_aperture[cell];
        level->value[cell] = solar + moon + job->stars[cell];
        for (size_t channel = 0; channel < 3; channel++) {
            level->rgb[cell][channel] = celestial_scale_value(solar, job->model.solar_color[channel], UINT16_MAX) +
                celestial_scale_value(moon, job->model.moon_color[channel], UINT16_MAX) +
                celestial_scale_value(job->stars[cell], job->model.starlight_color[channel], UINT16_MAX);
        }
        if (!job->exposed[cell] && job->level + 1 < job->count) {
            const celestial_pending_level_t *upper = &job->levels[job->level + 1];
            if (x < upper->width && y < upper->height) {
                size_t above = x + upper->width * y;
                uint16_t coefficient = edge_coefficient(upper->map, x, y, CELESTIAL_FACE_DOWN, NULL);
                int32_t injected = transmit(upper->value[above], coefficient);
                if (injected > level->value[cell]) {
                    level->value[cell] = injected;
                    for (size_t channel = 0; channel < 3; channel++) {
                        level->rgb[cell][channel] = transmit(upper->rgb[above][channel], coefficient);
                    }
                }
            }
        }
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
                for (size_t i = 0; i < job->count; i++) {
                    mapstruct *map = job->levels[i].map;
                    map->celestial_light_next_key = job->levels[i].key;
                    map->celestial_light_next_hour = job->source_hour + 1;
                    /* Readiness changes the transmitted endpoint pair, even
                     * when the current scalar/RGB field remains identical. */
                    if (map->celestial_light_generation_id == UINT64_MAX) {
                        HARD_ASSERT(false);
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
    while (pending_head != NULL && limit-- != 0 &&
           (deadline == 0 || datetime_monotonic_us() < deadline)) {
        celestial_pending_t *job = pending_head;
        pending_head = job->next;
        if (pending_head == NULL) {
            pending_tail = NULL;
        }
        if (!pending_fresh(job) || pending_step(job)) {
            pending_free(job);
        } else {
            pending_append(job);
        }
    }
}

void celestial_light_process_pending(uint64_t budget_us) {
    if (budget_us != 0) {
        uint64_t now = datetime_monotonic_us();
        pending_process(SIZE_MAX, budget_us > UINT64_MAX - now ? UINT64_MAX : now + budget_us);
    }
}

#ifdef ATRINIK_TESTING
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
        ready &= levels[i]->celestial_light_keyframe_valid &&
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
        largest = MAX(largest, cells);
    }
    if (largest > (SIZE_MAX - reservation) / 29) {
        return false;
    }
    reservation += largest * 29;
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
    for (size_t i = 0; i < count; i++) {
        mapstruct *current = levels[i];
        job->levels[i] = (celestial_pending_level_t){
            .map = current, .spaces = current->spaces, .width = current->width,
            .height = current->height, .cells = (size_t)current->width * current->height,
            .key = key_for_hour(current, absolute_hour + 1)};
        if (current->celestial_light_keyframe_valid) {
            if (current->celestial_light_generation_id == UINT64_MAX) {
                HARD_ASSERT(false);
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
    if (map->celestial_light_keyframe_valid && map->celestial_light_next_key == next_key &&
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
        celestial_field_snapshot_restore(&snapshots[i]);
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
    if (!map->celestial_light_valid || map->celestial_light_key != key) {
        mapstruct *levels[MAP2_LEVELS] = {0};
        size_t count = 0;
        bool ready = collect_stack(map, levels, &count);
        for (size_t i = 0; i < count; i++) {
            ready &= levels[i]->celestial_light_keyframe_valid &&
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
                HARD_ASSERT(false);
            }
            current->celestial_light_generation_id++;
            current->celestial_light_key = current->celestial_light_next_key;
            current->celestial_light_valid = true;
            current->celestial_light_keyframe_valid = false;
        }
    }
}
