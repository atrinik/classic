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
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>
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

/** Coefficient of one face from its attenuation nibble: 15 is opaque. */
static uint16_t client_lighting_face_coefficient(uint32_t occluder, int face) {
    int attenuation = (occluder >> (16 + MAP2_OCCLUDER_FACE_ATTENUATION_BITS * face)) & 15;
    if (attenuation == 15) {
        return 0;
    }
    return (uint16_t)(256 - attenuation * MAP2_OCCLUDER_ATTENUATION_STEP);
}

/**
 * Coefficient of leaving a cell towards (sx, sy) through its exit face; a
 * diagonal step crosses two faces and takes the smaller, so the client never
 * transmits more light than the server.
 */
static uint16_t client_lighting_coefficient(uint32_t occluder, int sx, int sy) {
    uint16_t coefficient = 256;
    if (sy < 0) {
        coefficient = MIN(coefficient, client_lighting_face_coefficient(occluder, 0));
    } else if (sy > 0) {
        coefficient = MIN(coefficient, client_lighting_face_coefficient(occluder, 2));
    }
    if (sx > 0) {
        coefficient = MIN(coefficient, client_lighting_face_coefficient(occluder, 1));
    } else if (sx < 0) {
        coefficient = MIN(coefficient, client_lighting_face_coefficient(occluder, 3));
    }
    return coefficient;
}

/** Whether local light cannot leave a cell through the face towards (sx, sy). */
static bool client_lighting_face_closed(uint32_t occluder, int sx, int sy) {
    uint16_t faces = occluder >> MAP2_OCCLUDER_FACE_SHIFT;
    return (sy < 0 && (faces & 1U)) || (sx > 0 && (faces & 2U)) || (sy > 0 && (faces & 4U)) ||
           (sx < 0 && (faces & 8U));
}

/**
 * Transmit a value from (x, y) to (x + dx, y + dy). Every cell the ray leaves,
 * the source included, attenuates it in travel order, as the server takes a
 * crossed face's coefficient from its source cell; a diagonal step also
 * crosses both side cells' faces. A view blocker stops it, and so does a face
 * marked discontinuous. The destination is an exposed face and
 * keeps the light that reaches it.
 */
static int32_t client_lighting_path(int width,
                                    int height,
                                    const uint32_t *occluders,
                                    int x,
                                    int y,
                                    int dx,
                                    int dy,
                                    int32_t value) {
    /* The same supercover events as spatial_ray_next_resolved(): each step
     * advances the axis whose next boundary comes first, and only an exact tie
     * advances both, crossing the faces of both grazing side cells too. */
    const int direction[2] = {dx, dy};
    uint64_t crossed[2] = {0, 0};
    int32_t transmitted = value;
    int px = x, py = y;
    while ((px != x + dx || py != y + dy) && transmitted != 0) {
        int first = -1;
        unsigned axes = 0;
        for (int axis = 0; axis < 2; axis++) {
            if (direction[axis] == 0) {
                continue;
            }
            if (first < 0) {
                first = axis;
                axes = 1U << axis;
                continue;
            }
            uint64_t left = (2 * crossed[axis] + 1) * (uint64_t)abs(direction[first]);
            uint64_t right = (2 * crossed[first] + 1) * (uint64_t)abs(direction[axis]);
            if (left < right) {
                first = axis;
                axes = 1U << axis;
            } else if (left == right) {
                axes |= 1U << axis;
            }
        }
        int sx = (axes & 1U) ? (dx > 0 ? 1 : -1) : 0;
        int sy = (axes & 2U) ? (dy > 0 ? 1 : -1) : 0;
        int cx = px + sx, cy = py + sy;
        if (cx < 0 || cy < 0 || cx >= width || cy >= height) {
            return 0;
        }
        uint32_t previous = occluders != NULL ? occluders[(size_t)py * width + px] : 0;
        if (client_lighting_face_closed(previous, sx, sy)) {
            return 0;
        }
        uint16_t coefficient = client_lighting_coefficient(previous, sx, sy);
        if (sx != 0 && sy != 0) {
            uint32_t side_x = occluders != NULL ? occluders[(size_t)py * width + cx] : 0;
            uint32_t side_y = occluders != NULL ? occluders[(size_t)cy * width + px] : 0;
            if (client_lighting_face_closed(side_x, 0, sy) ||
                client_lighting_face_closed(side_y, sx, 0)) {
                return 0;
            }
            coefficient = MIN(coefficient, client_lighting_coefficient(side_x, 0, sy));
            coefficient = MIN(coefficient, client_lighting_coefficient(side_y, sx, 0));
        }
        transmitted = (int32_t)(((int64_t)transmitted * coefficient + 128) / 256);
        for (int axis = 0; axis < 2; axis++) {
            if (axes & (1U << axis)) {
                crossed[axis]++;
            }
        }
        px = cx;
        py = cy;
    }
    return transmitted;
}

/** Add one unsigned profile, transmitted from its tile, to the accumulators. */
static void client_lighting_add(int width,
                                int height,
                                const uint32_t *occluders,
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
/** Which tiles and emitters one accumulation pass groups. */
typedef enum client_lighting_pass {
    /** Every tile with all its emitters. */
    CLIENT_LIGHTING_PASS_ALL,
    /** Only tiles with a flickering emitter, with all their emitters. */
    CLIENT_LIGHTING_PASS_FLICKER_TILES,
    /** Only tiles with a flickering emitter, without their flickering ones. */
    CLIENT_LIGHTING_PASS_FLICKER_TILES_STEADY,
} client_lighting_pass_t;

static bool client_lighting_emitter_flickers(const client_lighting_emitter_t *emitter) {
    return emitter->radius > 0 && (emitter->flags & MAP2_EMITTER_FLICKER) != 0;
}

static void client_lighting_accumulate(int width,
                                       int height,
                                       const uint32_t *occluders,
                                       const client_lighting_emitter_t *emitters,
                                       size_t count,
                                       client_lighting_pass_t pass,
                                       client_lighting_accumulator_t *accumulators) {
    size_t i = 0;
    while (i < count) {
        int x = emitters[i].x, y = emitters[i].y;
        size_t end = i;
        bool tile_flickers = false;
        for (; end < count && emitters[end].x == x && emitters[end].y == y; end++) {
            tile_flickers |= client_lighting_emitter_flickers(&emitters[end]);
        }
        if (pass != CLIENT_LIGHTING_PASS_ALL && !tile_flickers) {
            i = end;
            continue;
        }
        int radius = 0, positive_radius = 0;
        int64_t linear[LIGHT_PROFILE_COUNT][3] = {{0}};
        int64_t multiplicity[LIGHT_PROFILE_COUNT] = {0};
        bool any = false;
        for (; i < end; i++) {
            const client_lighting_emitter_t *emitter = &emitters[i];
            if (pass == CLIENT_LIGHTING_PASS_FLICKER_TILES_STEADY &&
                client_lighting_emitter_flickers(emitter)) {
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
                              const uint32_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells) {
    size_t total = (size_t)width * height;
    memset(cells, 0, total * sizeof(*cells));
    if (count == 0 || width <= 0 || height <= 0) {
        return;
    }

    /* Sort a copy by tile so grouping stays linear in the emitter count. The
     * client's fatal allocators keep a failure from caching a dark field. */
    client_lighting_emitter_t *sorted = xmallocarray(count, sizeof(*sorted));
    client_lighting_accumulator_t *all = xcalloc(total, sizeof(*all));
    client_lighting_accumulator_t *flicker = xcalloc(total, sizeof(*flicker));
    client_lighting_accumulator_t *flicker_steady = xcalloc(total, sizeof(*flicker_steady));
    memcpy(sorted, emitters, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), client_lighting_emitter_compare);
    client_lighting_accumulate(width, height, occluders, sorted, count, CLIENT_LIGHTING_PASS_ALL,
                               all);
    client_lighting_accumulate(width, height, occluders, sorted, count,
                               CLIENT_LIGHTING_PASS_FLICKER_TILES, flicker);
    client_lighting_accumulate(width, height, occluders, sorted, count,
                               CLIENT_LIGHTING_PASS_FLICKER_TILES_STEADY, flicker_steady);
    for (size_t index = 0; index < total; index++) {
        client_lighting_resolve(&all[index], &cells[index].scalar, cells[index].rgb);
        /* The flicker field is the flickering emitters' contribution to the
         * grouped field, E - E_s: accumulators add across tiles, so the steady
         * field swaps only the flickering tiles' terms. Flicker therefore
         * dims each tile towards its steady light and never past it. */
        client_lighting_accumulator_t steady = all[index];
        steady.scalar += flicker_steady[index].scalar - flicker[index].scalar;
        steady.positive += flicker_steady[index].positive - flicker[index].positive;
        steady.weight += flicker_steady[index].weight - flicker[index].weight;
        for (size_t channel = 0; channel < 3; channel++) {
            steady.color[channel] += flicker_steady[index].color[channel] -
                                     flicker[index].color[channel];
        }
        int32_t steady_scalar, steady_rgb[3];
        client_lighting_resolve(&steady, &steady_scalar, steady_rgb);
        cells[index].flicker_scalar = cells[index].scalar - steady_scalar;
        for (size_t channel = 0; channel < 3; channel++) {
            cells[index].flicker_rgb[channel] = cells[index].rgb[channel] - steady_rgb[channel];
        }
    }
    free(flicker_steady);
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
    /* The coarse level is a floor (a ceiling for darkness) under the emitter
     * field, never a cap: it bins the strongest single source while the field
     * groups a tile's sent emitters, which may exceed that bin. */
    /* An emitter field of the opposite sign that is at least as strong as the
     * floor is exact sent light and wins: the coarse sign comes from single
     * sources, so grouped sent emitters can outweigh it. */
    if (level > 0 && local < level * step && -local < level * step) {
        local = level * step;
    } else if (level < 0 && local > level * step && local < -level * step) {
        local = level * step;
    }
    return local;
}
