/*************************************************************************
 * Atrinik client protocol-1085 local light evaluation.                  *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <client_lighting.h>

#include <stdlib.h>
#include <string.h>

#include <toolkit/light_profile.h>
#include <toolkit/socket.h>

/** Emitters of one tile, grouped like the server's per-space counters. */
typedef struct client_lighting_group {
    int x;
    int y;
    int radius;
    int positive_radius;
    size_t first;
} client_lighting_group_t;

typedef struct client_lighting_accumulator {
    int64_t scalar;
    int64_t positive;
    int64_t color[3];
    int64_t weight;
} client_lighting_accumulator_t;

/** Coefficient of crossing a cell: 0 for view blockers, else its attenuation. */
static uint16_t client_lighting_coefficient(uint8_t occluder) {
    if (occluder & MAP2_OCCLUDER_BLOCKSVIEW) {
        return 0;
    }
    int attenuation = occluder >> MAP2_OCCLUDER_ATTENUATION_SHIFT;
    return (uint16_t)(256 - attenuation * MAP2_OCCLUDER_ATTENUATION_STEP);
}

/**
 * Transmit a value from (x, y) to (x + dx, y + dy). Cells strictly between
 * attenuate in travel order; a view blocker stops the ray. The destination is
 * an exposed face and keeps the light that reaches it.
 */
static int32_t client_lighting_path(int width,
                                    int height,
                                    const uint8_t *occluders,
                                    int x,
                                    int y,
                                    int dx,
                                    int dy,
                                    int32_t value) {
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int32_t transmitted = value;
    for (int step = 1; step < steps && transmitted != 0; step++) {
        int cx = x + (dx * step * 2 + (dx < 0 ? -steps : steps)) / (steps * 2);
        int cy = y + (dy * step * 2 + (dy < 0 ? -steps : steps)) / (steps * 2);
        if (cx < 0 || cy < 0 || cx >= width || cy >= height) {
            return 0;
        }
        uint8_t occluder = occluders != NULL ? occluders[(size_t)cy * width + cx] : 0;
        uint16_t coefficient = client_lighting_coefficient(occluder);
        transmitted = (int32_t)(((int64_t)transmitted * coefficient + 128) / 256);
    }
    return transmitted;
}

/** Add one unsigned profile, transmitted from its tile, to the accumulators. */
static void client_lighting_add(int width,
                                int height,
                                const uint8_t *occluders,
                                client_lighting_accumulator_t *accumulators,
                                int x,
                                int y,
                                int mask,
                                int sign,
                                int target,
                                uint32_t color) {
    if (mask == 0) {
        return;
    }
    int radius = light_profile_radius(mask);
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int distance_squared = dx * dx + dy * dy;
            if (distance_squared > radius * radius || x + dx < 0 || y + dy < 0 ||
                x + dx >= width || y + dy >= height) {
                continue;
            }
            int32_t value = light_profile_value(mask, distance_squared);
            value = client_lighting_path(width, height, occluders, x, y, dx, dy, value);
            if (value == 0) {
                continue;
            }
            client_lighting_accumulator_t *cell =
                &accumulators[(size_t)(y + dy) * width + (x + dx)];
            if (target == 0) {
                cell->scalar += sign * value;
            } else if (target == 1) {
                cell->positive += value;
            } else {
                for (size_t channel = 0; channel < 3; channel++) {
                    cell->color[channel] +=
                        (int64_t)value * light_profile_color_component(color, channel);
                }
                cell->weight += (int64_t)value * UINT16_MAX;
            }
        }
    }
}

void client_lighting_evaluate(int width,
                              int height,
                              const uint8_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells) {
    size_t total = (size_t)width * height;
    memset(cells, 0, total * sizeof(*cells));
    if (count == 0 || width <= 0 || height <= 0) {
        return;
    }
    if (count > CLIENT_LIGHTING_EMITTERS_MAX) {
        count = CLIENT_LIGHTING_EMITTERS_MAX;
    }

    client_lighting_group_t groups[CLIENT_LIGHTING_EMITTERS_MAX];
    size_t group_count = 0;
    for (size_t i = 0; i < count; i++) {
        size_t group = 0;
        while (group < group_count &&
               (groups[group].x != emitters[i].x || groups[group].y != emitters[i].y)) {
            group++;
        }
        if (group == group_count) {
            groups[group_count++] = (client_lighting_group_t){
                .x = emitters[i].x, .y = emitters[i].y, .first = i};
        }
        groups[group].radius += emitters[i].radius;
        if (emitters[i].radius > 0) {
            groups[group].positive_radius += emitters[i].radius;
        }
    }

    client_lighting_accumulator_t *accumulators = calloc(total, sizeof(*accumulators));
    if (accumulators == NULL) {
        return;
    }
    for (size_t group = 0; group < group_count; group++) {
        const client_lighting_group_t *g = &groups[group];
        int scalar_mask = light_profile_mask(g->radius);
        client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                            abs(scalar_mask), scalar_mask < 0 ? -1 : 1, 0, 0);
        client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                            light_profile_mask(g->positive_radius), 1, 1, 0);
        for (size_t i = g->first; i < count; i++) {
            if (emitters[i].x == g->x && emitters[i].y == g->y && emitters[i].radius > 0) {
                client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                                    light_profile_mask(emitters[i].radius), 1, 2,
                                    emitters[i].color);
            }
        }
    }

    /* Resolve color as the server's radiance resolver does for local light:
     * the capped positive part takes the accumulated hue, the rest is
     * neutral, and channels never go below zero. */
    for (size_t index = 0; index < total; index++) {
        const client_lighting_accumulator_t *a = &accumulators[index];
        int64_t accumulated = a->weight > 0 ? a->weight / UINT16_MAX : 0;
        int64_t effective = a->weight > 0 ? (a->positive > 0 ? a->positive : 0) : 0;
        if (effective > accumulated) {
            effective = accumulated;
        }
        cells[index].scalar = (int32_t)a->scalar;
        for (size_t channel = 0; channel < 3; channel++) {
            int64_t colored = 0;
            if (effective > 0) {
                colored = (a->color[channel] * effective + a->weight / 2) / a->weight;
            }
            int64_t value = a->scalar - effective + colored;
            cells[index].rgb[channel] = (int32_t)(value > 0 ? value : 0);
        }
    }
    free(accumulators);
}

int32_t client_lighting_compose(int32_t emitter_raw, int8_t level) {
    int32_t step = MAP2_LOCAL_LIGHT_LEVEL_RAW;
    int32_t local = emitter_raw;
    if (level > 0 && local < level * step) {
        local = level * step;
    } else if (level < 0 && local > level * step) {
        local = level * step;
    }
    if (level >= 0 && level < MAP2_LOCAL_LIGHT_LEVEL_MAX && local > step * (level + 1) - 1) {
        local = step * (level + 1) - 1;
    }
    if (level <= 0 && level > -MAP2_LOCAL_LIGHT_LEVEL_MAX && local < step * (level - 1) + 1) {
        local = step * (level - 1) + 1;
    }
    return local;
}
