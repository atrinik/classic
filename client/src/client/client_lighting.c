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

/** Order emitters by tile, then radius and color. */
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
 * Transmit a value from (x, y) to (x + dx, y + dy). Every cell the ray leaves,
 * the source included, attenuates it in travel order, as the server takes a
 * crossed face's coefficient from its source cell; a view blocker stops it, and
 * so does a face marked discontinuous. The destination is an exposed face and
 * keeps the light that reaches it.
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
        uint16_t coefficient = client_lighting_coefficient(previous);
        transmitted = (int32_t)(((int64_t)transmitted * coefficient + 128) / 256);
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
                                const int64_t linear[3]) {
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
                /* For color, linear is the multiplicity-weighted sum of a
                 * profile's constituent colors and sign their count. */
                for (size_t channel = 0; channel < 3; channel++) {
                    cell->color[channel] += (int64_t)value * linear[channel];
                }
                cell->weight += (int64_t)sign * value * UINT16_MAX;
            }
        }
    }
}

/**
 * Accumulate one emitter set, sorted by tile, as the server's per-space
 * counters do. Every positive constituent adds its own profile with its own
 * color; constituents sharing a resolved profile share its value at every
 * distance and its rays, so they fold exactly into one walk whose color is
 * their summed linear color. A tile therefore costs at most
 * LIGHT_PROFILE_COUNT color walks however many emitters it carries.
 */
static void client_lighting_accumulate(int width,
                                       int height,
                                       const uint16_t *occluders,
                                       const client_lighting_emitter_t *emitters,
                                       size_t count,
                                       bool flicker_only,
                                       client_lighting_accumulator_t *accumulators) {
    size_t i = 0;
    while (i < count) {
        int x = emitters[i].x, y = emitters[i].y;
        int radius = 0, positive_radius = 0;
        int64_t linear[LIGHT_PROFILE_COUNT][3] = {{0}};
        int64_t multiplicity[LIGHT_PROFILE_COUNT] = {0};
        bool any = false;
        for (; i < count && emitters[i].x == x && emitters[i].y == y; i++) {
            const client_lighting_emitter_t *emitter = &emitters[i];
            if (flicker_only &&
                (emitter->radius <= 0 || !(emitter->flags & MAP2_EMITTER_FLICKER))) {
                continue;
            }
            any = true;
            radius += emitter->radius;
            if (emitter->radius <= 0) {
                continue;
            }
            positive_radius += emitter->radius;
            int profile = light_profile_mask(emitter->radius);
            multiplicity[profile]++;
            for (size_t channel = 0; channel < 3; channel++) {
                linear[profile][channel] += light_profile_color_component(emitter->color, channel);
            }
        }
        if (!any) {
            continue;
        }
        int scalar_mask = light_profile_mask(radius);
        client_lighting_add(width, height, occluders, accumulators, x, y, abs(scalar_mask),
                            scalar_mask < 0 ? -1 : 1, 0, NULL);
        client_lighting_add(width, height, occluders, accumulators, x, y,
                            light_profile_mask(positive_radius), 1, 1, NULL);
        for (int profile = 1; profile < LIGHT_PROFILE_COUNT; profile++) {
            if (multiplicity[profile] != 0) {
                client_lighting_add(width, height, occluders, accumulators, x, y, profile,
                                    (int)multiplicity[profile], 2, linear[profile]);
            }
        }
    }
}

/**
 * Resolve color as the server's radiance resolver does for local light: the
 * capped positive part takes the accumulated hue and the rest is neutral.
 * Channels stay signed; the caller clamps them only after composition.
 */
static void client_lighting_resolve(const client_lighting_accumulator_t *a,
                                    int32_t *scalar,
                                    int32_t rgb[3]) {
    int64_t accumulated = a->weight > 0 ? a->weight / UINT16_MAX : 0;
    int64_t effective = a->weight > 0 ? (a->positive > 0 ? a->positive : 0) : 0;
    if (effective > accumulated) {
        effective = accumulated;
    }
    *scalar = (int32_t)a->scalar;
    for (size_t channel = 0; channel < 3; channel++) {
        int64_t colored = 0;
        if (effective > 0) {
            colored = (a->color[channel] * effective + a->weight / 2) / a->weight;
        }
        rgb[channel] = (int32_t)(a->scalar - effective + colored);
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

    /* Sort a copy by tile so grouping stays linear in the emitter count. */
    client_lighting_emitter_t *sorted = malloc(count * sizeof(*sorted));
    client_lighting_accumulator_t *all = calloc(total, sizeof(*all));
    client_lighting_accumulator_t *flicker = calloc(total, sizeof(*flicker));
    if (sorted == NULL || all == NULL || flicker == NULL) {
        free(sorted);
        free(all);
        free(flicker);
        return;
    }
    memcpy(sorted, emitters, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), client_lighting_emitter_compare);
    client_lighting_accumulate(width, height, occluders, sorted, count, false, all);
    client_lighting_accumulate(width, height, occluders, sorted, count, true, flicker);
    for (size_t index = 0; index < total; index++) {
        client_lighting_resolve(&all[index], &cells[index].scalar, cells[index].rgb);
        client_lighting_resolve(&flicker[index], &cells[index].flicker_scalar,
                                cells[index].flicker_rgb);
    }
    free(flicker);
    free(all);
    free(sorted);
}

/** Flicker keyframes: a new dip every this many milliseconds. */
#define CLIENT_LIGHTING_FLICKER_STEP_MS 125
/** Deepest dip in Q0.8: gains stay within 256 - 51 .. 256. */
#define CLIENT_LIGHTING_FLICKER_DIP_MAX 51

static uint32_t client_lighting_flicker_hash(uint64_t step) {
    uint64_t z = step + UINT64_C(0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return (uint32_t)(z ^ (z >> 31));
}

uint16_t client_lighting_flicker_gain(uint64_t time_ms) {
    uint64_t step = time_ms / CLIENT_LIGHTING_FLICKER_STEP_MS;
    uint32_t fraction =
        (uint32_t)(time_ms % CLIENT_LIGHTING_FLICKER_STEP_MS) * 256 / CLIENT_LIGHTING_FLICKER_STEP_MS;
    uint32_t from = client_lighting_flicker_hash(step) % (CLIENT_LIGHTING_FLICKER_DIP_MAX + 1);
    uint32_t to = client_lighting_flicker_hash(step + 1) % (CLIENT_LIGHTING_FLICKER_DIP_MAX + 1);
    uint32_t dip = (from * (256 - fraction) + to * fraction + 128) / 256;
    return (uint16_t)(256 - dip);
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
