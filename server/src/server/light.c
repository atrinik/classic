/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
#include <server_main.h>
#include <light.h>
#include <object.h>

#define MAX_MASK_SIZE 81
#define NR_LIGHT_MASK 10
#define MAX_LIGHT_SOURCE 13
#define MAX_LIGHT_RADIUS 4
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

/** Resolve the authoritative scalar level and presentation-only RGB tint. */
void light_levels_from_raw(const MapSpace *space, int raw_light, uint8_t levels[3]) {
    HARD_ASSERT(space != NULL);
    HARD_ASSERT(levels != NULL);

    if (space->light_source_color_weight <= 0) {
        uint8_t neutral = light_level_from_raw(raw_light);
        levels[0] = levels[1] = levels[2] = neutral;
        return;
    }

    int64_t positive_weight_raw = space->light_source_color_weight / UINT8_MAX;
    int64_t effective_source_raw =
        MIN(MAX(space->light_source_positive_value, 0), positive_weight_raw);
    for (size_t channel = 0; channel < 3; channel++) {
        int64_t color_sum = MAX(space->light_source_color[channel], 0);
        color_sum = MIN(color_sum, space->light_source_color_weight);
        uint64_t colored_raw = light_muldiv_round((uint64_t)color_sum,
                                                  (uint64_t)effective_source_raw,
                                                  (uint64_t)space->light_source_color_weight);
        int64_t channel_raw = (int64_t)raw_light - effective_source_raw + (int64_t)colored_raw;
        if (channel_raw > INT_MAX) {
            channel_raw = INT_MAX;
        } else if (channel_raw < INT_MIN) {
            channel_raw = INT_MIN;
        }
        levels[channel] = light_level_from_raw((int)channel_raw);
    }
}

static int lmask_x[MAX_MASK_SIZE] = {
    0,  0,  1,  1,  1,  0,  -1, -1, -1, 0,  1,  2,  2,  2,  2,  2,  1,  0,  -1, -2, -2,
    -2, -2, -2, -1, 0,  1,  2,  3,  3,  3,  3,  3,  3,  3,  2,  1,  0,  -1, -2, -3, -3,
    -3, -3, -3, -3, -3, -2, -1, 0,  1,  2,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,
    2,  1,  0,  -1, -2, -3, -4, -4, -4, -4, -4, -4, -4, -4, -4, -3, -2, -1};

static int lmask_y[MAX_MASK_SIZE] = {
    0,  -1, -1, 0,  1,  1,  1,  0,  -1, -2, -2, -2, -1, 0, 1, 2, 2,  2,  2,  2,  1,
    0,  -1, -2, -2, -3, -3, -3, -3, -2, -1, 0,  1,  2,  3, 3, 3, 3,  3,  3,  3,  2,
    1,  0,  -1, -2, -3, -3, -3, 4,  4,  4,  4,  4,  3,  2, 1, 0, -1, -2, -3, -4, -4,
    -4, -4, -4, -4, -4, -4, -4, -3, -2, -1, 0,  1,  2,  3, 4, 4, 4,  4};

static int light_mask[MAX_LIGHT_SOURCE + 1] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9};

static int light_mask_width[NR_LIGHT_MASK] = {0, 1, 2, 2, 3, 3, 3, 4, 4, 4};

static int light_mask_size[NR_LIGHT_MASK] = {0, 9, 25, 25, 49, 49, 49, 81, 81, 81};

static int light_masks[NR_LIGHT_MASK][MAX_MASK_SIZE] = {
    {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    },
    {
        40, 20, 20, 20, 20, 20, 20, 20, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    },
    {
        80, 40, 40, 40, 40, 40, 40, 40, 40, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
        20, 20, 20, 20, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    },
    {
        160, 80, 80, 80, 80, 80, 80, 80, 80, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40,  40, 40, 40, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    },
    {
        160, 80, 80, 80, 80, 80, 80, 80, 80, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40,  40, 40, 40, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
        20,  20, 20, 20, 20, 20, 20, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    },
    {
        320, 160, 160, 160, 160, 160, 160, 160, 160, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
        80,  80,  80,  80,  40,  40,  40,  40,  40,  40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40,  40,  40,  40,  40,  40,  40,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,
    },
    {
        320, 160, 160, 160, 160, 160, 160, 160, 160, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
        80,  80,  80,  80,  40,  40,  40,  40,  40,  40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40,  40,  40,  40,  40,  40,  40,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,  0,  0,  0,  0,  0,
    },
    {
        320, 160, 160, 160, 160, 160, 160, 160, 160, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
        80,  80,  80,  80,  40,  40,  40,  40,  40,  40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
        40,  40,  40,  40,  40,  40,  40,  20,  20,  20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
        20,  20,  20,  20,  20,  20,  20,  20,  20,  20, 20, 20, 20, 20, 20, 20, 20, 20,
    },
    {
        640, 320, 320, 320, 320, 320, 320, 320, 320, 160, 160, 160, 160, 160, 160, 160, 160,
        160, 160, 160, 160, 160, 160, 160, 160, 80,  80,  80,  80,  80,  80,  80,  80,  80,
        80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  40,  40,
        40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,
        40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,
    },
    {
        1280, 640, 640, 640, 640, 640, 640, 640, 640, 160, 160, 160, 160, 160, 160, 160, 160,
        160,  160, 160, 160, 160, 160, 160, 160, 80,  80,  80,  80,  80,  80,  80,  80,  80,
        80,   80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  80,  40,  40,
        40,   40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,
        40,   40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,  40,
    }

};

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

static int light_mask_value(int intensity, int distance_squared) {
    static const int ring_index[MAX_LIGHT_RADIUS + 1] = {0, 1, 9, 25, 49};
    int radius = 0;

    while (radius * radius < distance_squared) {
        radius++;
    }

    return light_masks[intensity][ring_index[radius]];
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
    int radius = light_mask_width[intensity];
    int vertical_radius = MIN(radius, MAP2_MAX_DEPTH);

    for (int i = 0; i < light_mask_size[intensity]; i++) {
        int dx = lmask_x[i];
        int dy = lmask_y[i];

        for (int dz = -vertical_radius; dz <= vertical_radius; dz++) {
            int distance_squared = dx * dx + dy * dy + dz * dz;

            if (distance_squared > radius * radius) {
                continue;
            }

            int xt, yt;
            mapstruct *target = light_resolve_space(map, x + dx, y + dy, dz, &xt, &yt);

            if (target == NULL || (only_map != NULL && target != only_map) ||
                (only_maps != NULL && !light_map_set_contains(only_maps, target)) ||
                (other_only && target == map) || !light_path_is_clear(map, x, y, dx, dy, dz)) {
                continue;
            }

            int value = light_mask_value(intensity, distance_squared) * mod;
            MapSpace *space = GET_MAP_SPACE_PTR(target, xt, yt);
            if (component == LIGHT_MASK_SCALAR) {
                space->light_source_value += value;
            } else if (component == LIGHT_MASK_POSITIVE) {
                space->light_source_positive_value += value;
            } else {
                space->light_source_color[0] += (int64_t)value * ((color >> 16) & UINT8_MAX);
                space->light_source_color[1] += (int64_t)value * ((color >> 8) & UINT8_MAX);
                space->light_source_color[2] += (int64_t)value * (color & UINT8_MAX);
                space->light_source_color_weight += (int64_t)value * UINT8_MAX;
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
