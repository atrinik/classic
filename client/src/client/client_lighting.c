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
    size_t end;
} client_lighting_group_t;

/** Distinct color constituents per tile that keep their own profile. */
#define CLIENT_LIGHTING_COLORS_MAX 8

/** Order emitters by tile, then radius and color, so equal ones are adjacent. */
static int client_lighting_emitter_compare(const void *left, const void *right) {
    const client_lighting_emitter_t *a = left, *b = right;
    if (a->y != b->y) {
        return a->y < b->y ? -1 : 1;
    }
    if (a->x != b->x) {
        return a->x < b->x ? -1 : 1;
    }
    if (a->radius != b->radius) {
        return a->radius < b->radius ? -1 : 1;
    }
    return a->color < b->color ? -1 : a->color > b->color ? 1 : 0;
}

typedef struct client_lighting_accumulator {
    int64_t scalar;
    int64_t positive;
    int64_t color[3];
    int64_t weight;
} client_lighting_accumulator_t;

/**
 * Coefficient of crossing a cell from its attenuation alone: 15 is opaque, so
 * an unauthored view blocker stops light while authored glass or a grate that
 * also blocks view still transmits.
 */
static uint16_t client_lighting_coefficient(uint16_t occluder) {
    int attenuation = (occluder >> MAP2_OCCLUDER_ATTENUATION_SHIFT) & 15;
    if (attenuation == 15) {
        return 0;
    }
    return (uint16_t)(256 - attenuation * MAP2_OCCLUDER_ATTENUATION_STEP);
}

/** Whether local light cannot leave a cell through the face towards (sx, sy). */
static bool client_lighting_face_closed(uint16_t occluder, int sx, int sy) {
    uint16_t faces = occluder >> MAP2_OCCLUDER_FACE_SHIFT;
    return (sy < 0 && (faces & 1U)) || (sx > 0 && (faces & 2U)) || (sy > 0 && (faces & 4U)) ||
           (sx < 0 && (faces & 8U));
}

/**
 * Transmit a value from (x, y) to (x + dx, y + dy). Cells strictly between
 * attenuate in travel order; a view blocker stops the ray, and so does a face
 * marked discontinuous on the cell it leaves. The destination is an exposed
 * face and keeps the light that reaches it.
 */
static int32_t client_lighting_path(int width,
                                    int height,
                                    const uint16_t *occluders,
                                    int x,
                                    int y,
                                    int dx,
                                    int dy,
                                    int32_t value) {
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int32_t transmitted = value;
    int px = x, py = y;
    for (int step = 1; step <= steps && transmitted != 0; step++) {
        int cx = x + (dx * step * 2 + (dx < 0 ? -steps : steps)) / (steps * 2);
        int cy = y + (dy * step * 2 + (dy < 0 ? -steps : steps)) / (steps * 2);
        if (cx < 0 || cy < 0 || cx >= width || cy >= height) {
            return 0;
        }
        uint16_t previous = occluders != NULL ? occluders[(size_t)py * width + px] : 0;
        if (client_lighting_face_closed(previous, cx - px, cy - py)) {
            return 0;
        }
        if (step < steps) {
            uint16_t occluder = occluders != NULL ? occluders[(size_t)cy * width + cx] : 0;
            uint16_t coefficient = client_lighting_coefficient(occluder);
            transmitted = (int32_t)(((int64_t)transmitted * coefficient + 128) / 256);
        }
        px = cx;
        py = cy;
    }
    return transmitted;
}

/** Add one unsigned profile, transmitted from its tile, to the accumulators. */
static void client_lighting_add(int width,
                                int height,
                                const uint16_t *occluders,
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
                /* For color, sign carries the multiplicity of identical
                 * constituents. */
                for (size_t channel = 0; channel < 3; channel++) {
                    cell->color[channel] += (int64_t)sign * value *
                                            light_profile_color_component(color, channel);
                }
                cell->weight += (int64_t)sign * value * UINT16_MAX;
            }
        }
    }
}

void client_lighting_evaluate(int width,
                              int height,
                              const uint16_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells) {
    size_t total = (size_t)width * height;
    memset(cells, 0, total * sizeof(*cells));
    if (count == 0 || width <= 0 || height <= 0) {
        return;
    }

    /* Sort a copy by tile so grouping and constituent scans stay linear in
     * the number of emitters a server can send. */
    client_lighting_emitter_t *sorted = malloc(count * sizeof(*sorted));
    client_lighting_group_t *groups = calloc(count, sizeof(*groups));
    if (sorted == NULL || groups == NULL) {
        free(sorted);
        free(groups);
        return;
    }
    memcpy(sorted, emitters, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), client_lighting_emitter_compare);
    emitters = sorted;
    size_t group_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (group_count == 0 || groups[group_count - 1].x != emitters[i].x ||
            groups[group_count - 1].y != emitters[i].y) {
            groups[group_count++] = (client_lighting_group_t){
                .x = emitters[i].x, .y = emitters[i].y, .first = i, .end = i};
        }
        client_lighting_group_t *group = &groups[group_count - 1];
        group->end = i + 1;
        group->radius += emitters[i].radius;
        if (emitters[i].radius > 0) {
            group->positive_radius += emitters[i].radius;
        }
    }

    client_lighting_accumulator_t *accumulators = calloc(total, sizeof(*accumulators));
    if (accumulators == NULL) {
        free(groups);
        free(sorted);
        return;
    }
    for (size_t group = 0; group < group_count; group++) {
        const client_lighting_group_t *g = &groups[group];
        int scalar_mask = light_profile_mask(g->radius);
        client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                            abs(scalar_mask), scalar_mask < 0 ? -1 : 1, 0, 0);
        client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                            light_profile_mask(g->positive_radius), 1, 1, 0);
        /* Identical constituents add one weighted profile; at most
         * CLIENT_LIGHTING_COLORS_MAX distinct colors per tile keep their own
         * profile, and light beyond that stays in the neutral scalar. */
        size_t distinct = 0;
        for (size_t i = g->first; i < g->end; i++) {
            if (emitters[i].radius <= 0 || (i > g->first && emitters[i].radius == emitters[i - 1].radius &&
                                            emitters[i].color == emitters[i - 1].color)) {
                continue;
            }
            if (distinct++ == CLIENT_LIGHTING_COLORS_MAX) {
                break;
            }
            int multiplicity = 1;
            while (i + multiplicity < g->end && emitters[i + multiplicity].radius == emitters[i].radius &&
                   emitters[i + multiplicity].color == emitters[i].color) {
                multiplicity++;
            }
            client_lighting_add(width, height, occluders, accumulators, g->x, g->y,
                                light_profile_mask(emitters[i].radius), multiplicity, 2,
                                emitters[i].color);
        }
    }

    /* Resolve color as the server's radiance resolver does for local light:
     * the capped positive part takes the accumulated hue and the rest is
     * neutral. Channels stay signed here; the caller clamps them only after
     * adding base radiance, as the server clamps its complete aggregate. */
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
            cells[index].rgb[channel] = (int32_t)(a->scalar - effective + colored);
        }
    }
    free(accumulators);
    free(groups);
    free(sorted);
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
