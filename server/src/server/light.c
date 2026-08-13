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

/**
 * @file
 * Lighting system.
 */

#include <global.h>
#include <initialization.h>
#include <server_main.h>
#include <light.h>
#include <object.h>

#define NR_LIGHT_MASK 10
#define MAX_LIGHT_SOURCE 13
#define MAX_LIGHT_RADIUS 4
#define LIGHT_DISTANCE_SCALE 256
/** Maximum unique maps visited by light_map_set_collect(). */
#define LIGHT_COLUMN_MAPS_MAX (1 + 2 * MAP2_MAX_DEPTH * (1 + TILED_NUM_DIR))
#define LIGHT_MAP_SET_MAX ((1 + TILED_NUM_DIR) * LIGHT_COLUMN_MAPS_MAX)

typedef struct light_map_set {
    mapstruct *maps[LIGHT_MAP_SET_MAX];
    size_t count;
} light_map_set;

/**
 * Convert authoritative raw map illumination to a perceptual client light
 * level.
 *
 * The anchor points keep low illumination visibly dark while giving local
 * light sources enough midrange contrast to illuminate their own and adjacent
 * tiles. Interpolation retains information from overlapping light sources.
 * Values above the brightest ambient level are saturated.
 *
 * @param raw_light Raw illumination returned by map_get_darkness().
 * @return Normalized light level: zero is unlit and 255 is fully lit.
 */
uint8_t light_level_from_raw(int raw_light) {
    static const struct {
        int raw;
        uint8_t level;
    } anchors[] = {
        {0, 0},
        {20, 45},
        {40, 80},
        {80, 120},
        {160, 165},
        {320, 215},
        {640, 245},
        {1280, 255},
    };

    if (raw_light <= anchors[0].raw) {
        return anchors[0].level;
    }

    for (size_t i = 1; i < arraysize(anchors); i++) {
        if (raw_light <= anchors[i].raw) {
            int raw_range = anchors[i].raw - anchors[i - 1].raw;
            int level_range = anchors[i].level - anchors[i - 1].level;
            int offset = raw_light - anchors[i - 1].raw;

            return anchors[i - 1].level + (offset * level_range + raw_range / 2) / raw_range;
        }
    }

    return UINT8_MAX;
}

/** Parse the authored six-digit, unprefixed RGB light tint. */
bool light_color_parse(const char *value, uint32_t *color) {
    if (value == NULL || color == NULL || strlen(value) != 6) {
        return false;
    }

    uint32_t parsed = 0;
    for (size_t i = 0; i < 6; i++) {
        unsigned char c = (unsigned char)value[i];
        uint32_t digit;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }

        parsed = (parsed << 4) | digit;
    }

    *color = parsed;
    return true;
}

/** Return round(numerator * multiplier / denominator) without overflowing. */
static uint64_t light_muldiv_round(uint64_t numerator, uint64_t multiplier, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    HARD_ASSERT(numerator <= denominator);

    uint64_t quotient = 0;
    uint64_t remainder = 0;
    uint64_t term_quotient = numerator / denominator;
    uint64_t term_remainder = numerator % denominator;

    while (multiplier != 0) {
        if (multiplier & 1) {
            quotient += term_quotient;
            if (term_remainder != 0 && remainder >= denominator - term_remainder) {
                remainder -= denominator - term_remainder;
                quotient++;
            } else {
                remainder += term_remainder;
            }
        }

        multiplier >>= 1;
        if (multiplier == 0) {
            break;
        }

        term_quotient *= 2;
        if (term_remainder != 0 && term_remainder >= denominator - term_remainder) {
            term_remainder -= denominator - term_remainder;
            term_quotient++;
        } else {
            term_remainder *= 2;
        }
    }

    if (remainder >= denominator / 2 + denominator % 2) {
        quotient++;
    }
    return quotient;
}

/** Return round(numerator / denominator) without overflowing the numerator. */
static uint64_t light_div_round(uint64_t numerator, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    uint64_t quotient = numerator / denominator;
    uint64_t remainder = numerator % denominator;
    return quotient + (remainder >= (denominator + 1) / 2 ? 1 : 0);
}

/** Canonical IEC 61966-2-1 sRGB8 to scene-linear Q0.16 lookup. */
static const uint16_t srgb8_to_linear_q16[UINT8_MAX + 1] = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313,
    340, 367, 396, 427, 458, 491, 526, 562, 599, 637, 677, 718, 761, 805,
    851, 898, 947, 997, 1048, 1101, 1156, 1212, 1270, 1330, 1391, 1453,
    1517, 1583, 1651, 1720, 1790, 1863, 1937, 2013, 2090, 2170, 2250, 2333,
    2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258, 3360, 3464,
    3570, 3678, 3788, 3900, 4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864,
    4993, 5124, 5257, 5392, 5530, 5669, 5810, 5953, 6099, 6246, 6395, 6547,
    6700, 6856, 7014, 7174, 7335, 7500, 7666, 7834, 8004, 8177, 8352, 8528,
    8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417, 10619,
    10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754,
    12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874, 15122,
    15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727,
    18001, 18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577,
    20876, 21177, 21481, 21787, 22096, 22407, 22721, 23038, 23357, 23678,
    24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036,
    27386, 27739, 28094, 28452, 28813, 29176, 29542, 29911, 30282, 30656,
    31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143, 34544,
    34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706,
    39138, 39572, 40009, 40449, 40891, 41337, 41785, 42236, 42690, 43147,
    43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871,
    48359, 48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884,
    53401, 53921, 54445, 54971, 55500, 56032, 56567, 57105, 57646, 58190,
    58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795,
    64372, 64952, 65535,
};

static uint16_t light_color_linear_component(uint32_t color, size_t channel) {
    uint16_t components[3] = {
        srgb8_to_linear_q16[(color >> 16) & UINT8_MAX],
        srgb8_to_linear_q16[(color >> 8) & UINT8_MAX],
        srgb8_to_linear_q16[color & UINT8_MAX],
    };
    uint16_t peak = MAX(components[0], MAX(components[1], components[2]));
    return peak == 0 ? 0 : light_muldiv_round(components[channel], UINT16_MAX, peak);
}

static uint16_t light_raw_to_radiance(int64_t raw_light) {
    if (raw_light <= 0) {
        return 0;
    }
    if (raw_light >= INT64_C(40959)) {
        return UINT16_MAX;
    }
    return (uint16_t)((raw_light * 8 + 2) / 5);
}

void light_radiance_from_raw(const MapSpace *space,
                             int raw_light,
                             uint16_t *scalar_radiance,
                             uint16_t radiance[3]) {
    HARD_ASSERT(space != NULL);
    HARD_ASSERT(scalar_radiance != NULL);
    HARD_ASSERT(radiance != NULL);

    *scalar_radiance = light_raw_to_radiance(raw_light);
    int64_t effective_source_raw = 0;
    int64_t accumulated_source_raw = 0;
    if (space->light_source_color_weight > 0) {
        accumulated_source_raw = space->light_source_color_weight / UINT16_MAX;
        effective_source_raw =
            MIN(MAX(space->light_source_positive_value, 0), accumulated_source_raw);
    }
    int64_t channels[3];
    for (size_t channel = 0; channel < arraysize(channels); channel++) {
        uint64_t colored_raw = 0;
        if (effective_source_raw > 0) {
            uint64_t color_sum = (uint64_t)MAX(space->light_source_color[channel], 0);
            if (effective_source_raw < accumulated_source_raw) {
                colored_raw = light_muldiv_round(color_sum,
                                                 (uint64_t)effective_source_raw,
                                                 (uint64_t)space->light_source_color_weight);
            } else {
                colored_raw = light_div_round(color_sum, UINT16_MAX);
            }
        }
        channels[channel] = MAX((int64_t)raw_light - effective_source_raw + (int64_t)colored_raw, 0);
    }
    int64_t maximum = MAX(channels[0], MAX(channels[1], channels[2]));
    if (maximum > INT64_C(40959)) {
        for (size_t channel = 0; channel < arraysize(channels); channel++) {
            uint64_t scaled = light_muldiv_round((uint64_t)channels[channel],
                                                 UINT64_C(1) + UINT16_MAX,
                                                 (uint64_t)maximum);
            radiance[channel] = (uint16_t)MIN(scaled, UINT16_MAX);
        }
        return;
    }
    for (size_t channel = 0; channel < arraysize(channels); channel++) {
        radiance[channel] = light_raw_to_radiance(channels[channel]);
    }
}

typedef struct light_profile {
    int center;
    int radial_radius;
    int radial_power;
} light_profile_t;

/* Authored glow_radius remains a bounded profile selector. Center intensity is
 * preserved; radial support gains one cell where the old four-cell cap allows
 * it, giving interpolation enough samples without increasing worst-case work. */
static const light_profile_t light_profiles[NR_LIGHT_MASK] = {
    {0, 0, 1},
    {40, 2, 1},
    {80, 3, 2},
    {160, 3, 2},
    {160, 4, 2},
    {320, 4, 2},
    {320, 4, 2},
    {320, 4, 2},
    {640, 4, 2},
    {1280, 4, 2},
};

static const int light_mask[MAX_LIGHT_SOURCE + 1] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9};

static int get_real_light_source_value(int l) {
    if (l > MAX_LIGHT_SOURCE) {
        return light_mask[MAX_LIGHT_SOURCE];
    }

    if (l < -MAX_LIGHT_SOURCE) {
        return -light_mask[MAX_LIGHT_SOURCE];
    }

    if (l < 0) {
        return -light_mask[-l];
    }

    return light_mask[l];
}

static bool light_map_set_contains(const light_map_set *set, const mapstruct *map) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->maps[i] == map) {
            return true;
        }
    }

    return false;
}

static void light_map_set_add(light_map_set *set, mapstruct *map) {
    if (map == NULL || map->in_memory != MAP_IN_MEMORY || light_map_set_contains(set, map)) {
        return;
    }

    SOFT_ASSERT(set->count < arraysize(set->maps), "Too many linked maps in lighting volume");

    if (set->count < arraysize(set->maps)) {
        set->maps[set->count++] = map;
    }
}

static mapstruct *light_loaded_tile(mapstruct *map, int tile) {
    mapstruct *tiled = map->tile_map[tile];

    if (tiled == NULL || tiled->in_memory != MAP_IN_MEMORY) {
        return NULL;
    }

    return tiled;
}

static void light_map_set_add_column(light_map_set *set, mapstruct *map) {
    light_map_set_add(set, map);

    for (int direction = TILED_UP; direction <= TILED_DOWN; direction++) {
        mapstruct *level = map;

        for (int depth = 0; depth < MAP2_MAX_DEPTH; depth++) {
            level = light_loaded_tile(level, direction);

            if (level == NULL) {
                break;
            }

            light_map_set_add(set, level);

            for (int side = 0; side < TILED_NUM_DIR; side++) {
                light_map_set_add(set, light_loaded_tile(level, side));
            }
        }
    }
}

static void light_map_set_collect(light_map_set *set, mapstruct *map) {
    memset(set, 0, sizeof(*set));
    light_map_set_add_column(set, map);

    for (int direction = 0; direction < TILED_NUM_DIR; direction++) {
        mapstruct *side = light_loaded_tile(map, direction);

        if (side != NULL) {
            light_map_set_add_column(set, side);
        }
    }
}

static mapstruct *light_vertical_map(mapstruct *map, int z) {
    int direction = z < 0 ? TILED_DOWN : TILED_UP;

    for (int depth = 0; depth < abs(z); depth++) {
        map = light_loaded_tile(map, direction);

        if (map == NULL) {
            return NULL;
        }
    }

    return map;
}

static mapstruct *light_resolve_space(mapstruct *map, int x, int y, int z, int *rx, int *ry) {
    mapstruct *level = light_vertical_map(map, z);

    *rx = x;
    *ry = y;

    if (level != NULL) {
        mapstruct *resolved = get_map_from_coord2(level, rx, ry);

        if (resolved != NULL) {
            return resolved;
        }
    }

    /* Some map sets only link their levels from each horizontal tile. Try
     * crossing the horizontal boundary before moving vertically as well. */
    *rx = x;
    *ry = y;
    mapstruct *side = get_map_from_coord2(map, rx, ry);

    if (side == NULL) {
        return NULL;
    }

    return light_vertical_map(side, z);
}

static bool light_space_has_floor(mapstruct *map, int x, int y) {
    for (object *tmp = GET_MAP_OB(map, x, y); tmp != NULL; tmp = tmp->above) {
        if (QUERY_FLAG(tmp, FLAG_IS_FLOOR)) {
            return true;
        }
    }

    return false;
}

static int light_round_div(int value, int divisor) {
    if (value < 0) {
        return -((-value + divisor / 2) / divisor);
    }

    return (value + divisor / 2) / divisor;
}

static bool light_path_is_clear(mapstruct *map, int x, int y, int dx, int dy, int dz) {
    int steps = MAX(abs(dx), MAX(abs(dy), abs(dz)));
    int previous_x = 0, previous_y = 0, previous_z = 0;
    int previous_rx, previous_ry;
    mapstruct *previous_map = light_resolve_space(map, x, y, 0, &previous_rx, &previous_ry);

    if (previous_map == NULL) {
        return false;
    }

    for (int step = 1; step <= steps; step++) {
        int current_x = light_round_div(dx * step, steps);
        int current_y = light_round_div(dy * step, steps);
        int current_z = light_round_div(dz * step, steps);

        if (current_x == previous_x && current_y == previous_y && current_z == previous_z) {
            continue;
        }

        int current_rx, current_ry;
        mapstruct *current_map = light_resolve_space(map,
                                                     x + current_x,
                                                     y + current_y,
                                                     current_z,
                                                     &current_rx,
                                                     &current_ry);

        if (current_map == NULL) {
            return false;
        }

        bool target = step == steps;

        if (current_z != previous_z) {
            mapstruct *upper_map;
            int upper_x, upper_y;

            if (current_z > previous_z) {
                upper_map = current_map;
                upper_x = current_rx;
                upper_y = current_ry;
            } else {
                upper_map = previous_map;
                upper_x = previous_rx;
                upper_y = previous_ry;
            }

            /* A floor owns the boundary below its map level. Opaque target
             * cells are allowed to receive light on their exposed face, but
             * the ray never continues through them. */
            if (light_space_has_floor(upper_map, upper_x, upper_y) &&
                !(target && (GET_MAP_FLAGS(current_map, current_rx, current_ry) & P_BLOCKSVIEW))) {
                return false;
            }
        }

        if (!target && (GET_MAP_FLAGS(current_map, current_rx, current_ry) & P_BLOCKSVIEW)) {
            return false;
        }

        previous_x = current_x;
        previous_y = current_y;
        previous_z = current_z;
        previous_map = current_map;
        previous_rx = current_rx;
        previous_ry = current_ry;
    }

    return true;
}

/** Calculate the deterministic monotonic radial profile at one distance. */
static int radial_light_mask_value(int intensity, int distance_squared) {
    static const uint16_t distances[MAX_LIGHT_RADIUS * MAX_LIGHT_RADIUS + 1] = {
        0, 256, 362, 443, 512, 572, 627, 677, 724, 768, 809, 849, 886, 923, 957, 991, 1024,
    };
    const light_profile_t *profile = &light_profiles[intensity];
    uint32_t radius = (uint32_t)profile->radial_radius * LIGHT_DISTANCE_SCALE;
    HARD_ASSERT(distance_squared >= 0 && distance_squared < (int)arraysize(distances));
    uint32_t distance = distances[distance_squared];
    if (distance >= radius) {
        return 0;
    }

    uint64_t numerator = radius - distance;
    uint64_t denominator = radius;
    for (int power = 1; power < profile->radial_power; power++) {
        numerator *= radius - distance;
        denominator *= radius;
    }

    return (int)(((uint64_t)profile->center * numerator + denominator / 2) / denominator);
}

static int light_mask_value(int intensity, int distance_squared) {
    return radial_light_mask_value(intensity, distance_squared);
}

typedef enum light_mask_component {
    LIGHT_MASK_SCALAR,
    LIGHT_MASK_POSITIVE,
    LIGHT_MASK_COLOR,
} light_mask_component_t;

static void light_mask_adjust(mapstruct *map,
                              int x,
                              int y,
                              int intensity,
                              int mod,
                              mapstruct *only_map,
                              const light_map_set *only_maps,
                              bool other_only,
                              light_mask_component_t component,
                              uint32_t color) {
    if (intensity < 0) {
        mod = -mod;
    }

    intensity = abs(intensity);
    int radius = light_profiles[intensity].radial_radius;
    int vertical_radius = MIN(radius, MAP2_MAX_DEPTH);

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            for (int dz = -vertical_radius; dz <= vertical_radius; dz++) {
                int distance_squared = dx * dx + dy * dy + dz * dz;

                if (distance_squared > radius * radius) {
                    continue;
                }
                int value = light_mask_value(intensity, distance_squared) * mod;
                if (value == 0) {
                    continue;
                }

                int xt, yt;
                mapstruct *target = light_resolve_space(map, x + dx, y + dy, dz, &xt, &yt);

                if (target == NULL || (only_map != NULL && target != only_map) ||
                    (only_maps != NULL && !light_map_set_contains(only_maps, target)) ||
                    (other_only && target == map) || !light_path_is_clear(map, x, y, dx, dy, dz)) {
                    continue;
                }

                MapSpace *space = GET_MAP_SPACE_PTR(target, xt, yt);
                if (component == LIGHT_MASK_SCALAR) {
                    space->light_source_value += value;
                } else if (component == LIGHT_MASK_POSITIVE) {
                    space->light_source_positive_value += value;
                } else {
                    space->light_source_color[0] +=
                        (int64_t)value * light_color_linear_component(color, 0);
                    space->light_source_color[1] +=
                        (int64_t)value * light_color_linear_component(color, 1);
                    space->light_source_color[2] +=
                        (int64_t)value * light_color_linear_component(color, 2);
                    space->light_source_color_weight += (int64_t)value * UINT16_MAX;
                }
            }
        }
    }
}

/** Keep an origin discoverable while either scalar or positive masks exist. */
static void light_origin_list_update(mapstruct *map, MapSpace *space) {
    bool linked =
        space->next_light != NULL || space->prev_light != NULL || map->first_light == space;
    bool active = get_real_light_source_value(space->light_source) != 0 ||
                  get_real_light_source_value(space->light_source_positive) != 0;
    if (active == linked) {
        return;
    }

    if (!active) {
        if (space->prev_light != NULL) {
            space->prev_light->next_light = space->next_light;
        } else {
            HARD_ASSERT(map->first_light == space);
            map->first_light = space->next_light;
        }
        if (space->next_light != NULL) {
            space->next_light->prev_light = space->prev_light;
        }
        space->prev_light = NULL;
        space->next_light = NULL;
        return;
    }

    space->next_light = map->first_light;
    if (map->first_light != NULL) {
        map->first_light->prev_light = space;
    }
    map->first_light = space;
}

/** Adjust one grouped scalar component at a source origin. */
static void adjust_grouped_light_source(mapstruct *map,
                                        int x,
                                        int y,
                                        int32_t *counter,
                                        int light,
                                        light_mask_component_t component) {
    MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    int old_mask = get_real_light_source_value(*counter);
    *counter += light;
    int new_mask = get_real_light_source_value(*counter);

    if (map->in_memory != MAP_LOADING && old_mask != new_mask) {
        if (old_mask != 0) {
            light_mask_adjust(map, x, y, old_mask, -1, NULL, NULL, false, component, 0);
        }
        if (new_mask != 0) {
            light_mask_adjust(map, x, y, new_mask, 1, NULL, NULL, false, component, 0);
        }
    }
    light_origin_list_update(map, space);
}

/** Add or remove a legacy scalar light source at one map space. */
void adjust_light_source(mapstruct *map, int x, int y, int light) {
    MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
    adjust_grouped_light_source(map, x, y, &space->light_source, light, LIGHT_MASK_SCALAR);
}

/** Add or remove one identified source's scalar mask and RGB contribution. */
void adjust_light_source_color(mapstruct *map,
                               int x,
                               int y,
                               int radius,
                               uint32_t color,
                               int direction) {
    HARD_ASSERT(map != NULL);
    HARD_ASSERT(direction == -1 || direction == 1);

    if (radius == 0) {
        return;
    }

    adjust_light_source(map, x, y, radius * direction);
    /* Darkness sources remain entirely achromatic: raw_light already carries
     * their signed scalar effect, while the RGB accumulators describe only
     * positive presentation tint. */
    if (radius > 0) {
        MapSpace *space = GET_MAP_SPACE_PTR(map, x, y);
        adjust_grouped_light_source(map,
                                    x,
                                    y,
                                    &space->light_source_positive,
                                    radius * direction,
                                    LIGHT_MASK_POSITIVE);
    }
    if (radius > 0 && map->in_memory != MAP_LOADING) {
        light_mask_adjust(map,
                          x,
                          y,
                          get_real_light_source_value(radius),
                          direction,
                          NULL,
                          NULL,
                          false,
                          LIGHT_MASK_COLOR,
                          color);
    }
}

/**
 * Check light source list of specified map.
 * This will also check all tiled maps attached
 * to the map.
 * @param map
 * The map to check.
 */
void check_light_source_list(mapstruct *map) {
    /* Rebuild the complete bounded linked volume now that this map's floors
     * and blockers exist. Rebuilding also makes repeated lifecycle calls
     * idempotent instead of adding the same source masks twice. */
    recalculate_light_sources(map);
}

/** Rebuild source illumination after opaque map geometry changes. */
void recalculate_light_sources(mapstruct *map) {
    light_map_set maps;
    light_map_set_collect(&maps, map);

    for (size_t i = 0; i < maps.count; i++) {
        mapstruct *target = maps.maps[i];

        for (int y = 0; y < MAP_HEIGHT(target); y++) {
            for (int x = 0; x < MAP_WIDTH(target); x++) {
                GET_MAP_SPACE_PTR(target, x, y)->light_source_value = 0;
                GET_MAP_SPACE_PTR(target, x, y)->light_source_positive_value = 0;
                memset(GET_MAP_SPACE_PTR(target, x, y)->light_source_color,
                       0,
                       sizeof(GET_MAP_SPACE_PTR(target, x, y)->light_source_color));
                GET_MAP_SPACE_PTR(target, x, y)->light_source_color_weight = 0;
            }
        }
    }

    for (size_t i = 0; i < maps.count; i++) {
        mapstruct *source_map = maps.maps[i];

        for (MapSpace *tmp = source_map->first_light; tmp != NULL; tmp = tmp->next_light) {
            if (tmp->first != NULL) {
                int scalar_mask = get_real_light_source_value(tmp->light_source);
                if (scalar_mask != 0) {
                    light_mask_adjust(source_map,
                                      tmp->first->x,
                                      tmp->first->y,
                                      scalar_mask,
                                      1,
                                      NULL,
                                      &maps,
                                      false,
                                      LIGHT_MASK_SCALAR,
                                      0);
                }
                if (tmp->light_source_positive != 0) {
                    light_mask_adjust(source_map,
                                      tmp->first->x,
                                      tmp->first->y,
                                      get_real_light_source_value(tmp->light_source_positive),
                                      1,
                                      NULL,
                                      &maps,
                                      false,
                                      LIGHT_MASK_POSITIVE,
                                      0);
                }
            }
        }

        for (int y = 0; y < MAP_HEIGHT(source_map); y++) {
            for (int x = 0; x < MAP_WIDTH(source_map); x++) {
                for (object *source = GET_MAP_OB(source_map, x, y); source != NULL;
                     source = source->above) {
                    if (source->glow_radius == 0) {
                        continue;
                    }

                    if (source->glow_radius < 0) {
                        continue;
                    }
                    uint32_t color = source->light_color;
                    light_mask_adjust(source_map,
                                      x,
                                      y,
                                      get_real_light_source_value(source->glow_radius),
                                      1,
                                      NULL,
                                      &maps,
                                      false,
                                      LIGHT_MASK_COLOR,
                                      color);
                }
            }
        }
    }
}

/**
 * Remove light sources list from a map.
 * @param map
 * The map to remove from.
 */
void remove_light_source_list(mapstruct *map) {
    MapSpace *tmp;

    for (tmp = map->first_light; tmp; tmp = tmp->next_light) {
        if (!tmp->first) {
            continue;
        }

        int scalar_mask = get_real_light_source_value(tmp->light_source);
        if (scalar_mask != 0) {
            light_mask_adjust(map,
                              tmp->first->x,
                              tmp->first->y,
                              scalar_mask,
                              -1,
                              NULL,
                              NULL,
                              true,
                              LIGHT_MASK_SCALAR,
                              0);
        }
        int positive_mask = get_real_light_source_value(tmp->light_source_positive);
        if (positive_mask != 0) {
            light_mask_adjust(map,
                              tmp->first->x,
                              tmp->first->y,
                              positive_mask,
                              -1,
                              NULL,
                              NULL,
                              true,
                              LIGHT_MASK_POSITIVE,
                              0);
        }
    }

    if (map->spaces != NULL) {
        for (int y = 0; y < MAP_HEIGHT(map); y++) {
            for (int x = 0; x < MAP_WIDTH(map); x++) {
                for (object *source = GET_MAP_OB(map, x, y); source != NULL;
                     source = source->above) {
                    if (source->glow_radius == 0) {
                        continue;
                    }

                    if (source->glow_radius < 0) {
                        continue;
                    }
                    uint32_t color = source->light_color;
                    light_mask_adjust(map,
                                      x,
                                      y,
                                      get_real_light_source_value(source->glow_radius),
                                      -1,
                                      NULL,
                                      NULL,
                                      true,
                                      LIGHT_MASK_COLOR,
                                      color);
                }
            }
        }
    }

    map->first_light = NULL;
}
