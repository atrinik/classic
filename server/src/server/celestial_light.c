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
#include <celestial_structure.h>
#include <initialization.h>
#include <light.h>
#include <map.h>
#include <object.h>
#include <region.h>
#include <tod.h>

#include <stdint.h>

#define CELESTIAL_SCALE 256
#define CELESTIAL_DIRECT_REACH 32
#define CELESTIAL_SPILL_PASSES 4

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
        }
    }
}

static uint64_t celestial_key(const mapstruct *map,
                              const region_celestial_profile_t *profile,
                              const region_celestial_phases_t *phases) {
    uint64_t hash = UINT64_C(1469598103934665603);
#define MIX_CELESTIAL(value)                                                                   \
    do {                                                                                        \
        hash ^= (uint64_t)(value);                                                              \
        hash *= UINT64_C(1099511628211);                                                        \
    } while (0)
    MIX_CELESTIAL(profile->revision);
    MIX_CELESTIAL(phases->solar);
    MIX_CELESTIAL(phases->season);
    MIX_CELESTIAL(phases->lunar);
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
    mapstruct *bottom = map;
    size_t seen = 0;
    while (bottom->tile_map[TILED_DOWN] != NULL) {
        if (seen++ >= MAP2_LEVELS || bottom->tile_map[TILED_DOWN]->in_memory != MAP_IN_MEMORY) {
            return false;
        }
        bottom = bottom->tile_map[TILED_DOWN];
    }

    mapstruct *cursor = bottom;
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
    mapstruct *cursor = map;
    for (size_t i = 0; i < MAP2_LEVELS && cursor != NULL; i++) {
        if (cursor->celestial_structure_revision == UINT64_MAX) {
            HARD_ASSERT(false);
        }
        cursor->celestial_structure_revision++;
        cursor->celestial_light_valid = false;
        cursor = cursor->tile_map[TILED_UP];
    }
    cursor = map->tile_map[TILED_DOWN];
    for (size_t i = 0; i < MAP2_LEVELS && cursor != NULL; i++) {
        if (cursor->celestial_structure_revision == UINT64_MAX) {
            HARD_ASSERT(false);
        }
        cursor->celestial_structure_revision++;
        cursor->celestial_light_valid = false;
        cursor = cursor->tile_map[TILED_DOWN];
    }
}

bool celestial_light_rebuild(mapstruct *map, uint64_t absolute_hour) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL) {
        return false;
    }

    mapstruct *levels[MAP2_LEVELS] = {0};
    size_t count = 0;
    char error[HUGE_BUF];
    if (!collect_stack(map, levels, &count) ||
        !celestial_structure_validate_topology(map, VS(error))) {
        for (size_t i = 0; i < count; i++) {
            clear_map_field(levels[i]);
            levels[i]->celestial_light_valid = true;
        }
        const region_celestial_profile_t *profile = region_celestial_for_map(map);
        region_celestial_phases_t phases;
        region_celestial_phases(profile, absolute_hour, &phases);
        map->celestial_light_key = celestial_key(map, profile, &phases);
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
        current->celestial_light_key = celestial_key(current, profile, &phases);
        current->celestial_light_valid = true;
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

void celestial_light_ensure(mapstruct *map) {
    if (map == NULL || map->celestial_schema != 1 || map->spaces == NULL) {
        return;
    }
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    region_celestial_phases_t phases;
    region_celestial_phases(profile, (uint64_t)todtick, &phases);
    uint64_t key = celestial_key(map, profile, &phases);
    if (!map->celestial_light_valid || map->celestial_light_key != key) {
        celestial_light_rebuild(map, (uint64_t)todtick);
    }
}
