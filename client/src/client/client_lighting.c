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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <toolkit/light_profile.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>
#include <toolkit/socket.h>

/** Order emitters by tile (level, row, column), then radius and color. */
static int client_lighting_emitter_compare(const void *left, const void *right) {
    const client_lighting_emitter_t *a = left, *b = right;
    if (a->z != b->z) {
        return a->z < b->z ? -1 : 1;
    }
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

/** The stack being evaluated and the level whose cells are wanted. */
typedef struct client_lighting_grid {
    int levels, target, width, height;
    const uint64_t *occluders;
} client_lighting_grid_t;

static bool client_lighting_inside(const client_lighting_grid_t *grid, const int p[3]) {
    return p[0] >= 0 && p[1] >= 0 && p[2] >= 0 && p[0] < grid->width && p[1] < grid->height &&
           p[2] < grid->levels;
}

static uint64_t client_lighting_occluder_at(const client_lighting_grid_t *grid, const int p[3]) {
    if (grid->occluders == NULL) {
        return 0;
    }
    return grid->occluders[((size_t)p[2] * grid->height + p[1]) * grid->width + p[0]];
}

/** Coefficient of an attenuation nibble: 15 is opaque. */
static uint16_t client_lighting_attenuation(unsigned attenuation) {
    return attenuation == 15 ? 0 : (uint16_t)(256 - attenuation * MAP2_OCCLUDER_ATTENUATION_STEP);
}

/** Face index of a unit step for the closed-face bits: north, east, south,
 * west, up, down. */
static int client_lighting_face(int axis, int step) {
    if (axis == 0) {
        return step > 0 ? 1 : 3;
    }
    if (axis == 1) {
        return step > 0 ? 2 : 0;
    }
    return step > 0 ? 4 : 5;
}

static bool client_lighting_closed(uint64_t occluder, int face) {
    return ((occluder >> MAP2_OCCLUDER_FACE_SHIFT) >> face) & 1U;
}

/**
 * Light coefficient of one face crossed from 'from' to the neighbor along an
 * axis, or a negative value when light cannot cross it. A horizontal face
 * attenuates by the nibble of the cell light leaves; a vertical face by the
 * vertical attenuation of the upper cell, closed by either cell's bit. An
 * upward step ending on a view blocker or roof surface skips that receiving
 * slab, so its exposed underside is lit, as light_terminal_surface() decides.
 */
static int client_lighting_face_light(const client_lighting_grid_t *grid,
                                      const int from[3],
                                      int axis,
                                      int step,
                                      const int destination[3],
                                      bool terminal) {
    int to[3] = {from[0], from[1], from[2]};
    to[axis] += step;
    if (!client_lighting_inside(grid, from) || !client_lighting_inside(grid, to)) {
        return -1;
    }
    uint64_t leaving = client_lighting_occluder_at(grid, from);
    uint64_t entering = client_lighting_occluder_at(grid, to);
    if (axis < 2) {
        int face = client_lighting_face(axis, step);
        if (client_lighting_closed(leaving, face)) {
            return -1;
        }
        return client_lighting_attenuation((leaving >> (16 + MAP2_OCCLUDER_FACE_ATTENUATION_BITS *
                                                                 (unsigned)face)) & 15U);
    }
    uint64_t lower = step > 0 ? leaving : entering, upper = step > 0 ? entering : leaving;
    if (client_lighting_closed(lower, 4) || client_lighting_closed(upper, 5)) {
        return -1;
    }
    if (step > 0 && terminal && to[0] == destination[0] && to[1] == destination[1] &&
        to[2] == destination[2] &&
        (upper & (MAP2_OCCLUDER_BLOCKSVIEW | MAP2_OCCLUDER_ROOF)) != 0) {
        return 256;
    }
    return client_lighting_attenuation((upper >> 32) & 15U);
}

/**
 * Transmit a value from (x, y, z) to (x + dx, y + dy, z + dz) along the same
 * supercover events as spatial_ray_next_resolved(): each step advances the
 * axes whose next boundary comes first, and a tie crosses every face between
 * the touched cells, grazing ones included, each attenuating in travel order.
 * A cell outside the stack, or a closed face, stops the light; the
 * destination keeps the light that reaches it.
 */
static int32_t client_lighting_path(const client_lighting_grid_t *grid,
                                    int x,
                                    int y,
                                    int z,
                                    int dx,
                                    int dy,
                                    int dz,
                                    int32_t value) {
    const int direction[3] = {dx, dy, dz};
    const int destination[3] = {x + dx, y + dy, z + dz};
    uint64_t crossed[3] = {0, 0, 0};
    int position[3] = {x, y, z};
    int32_t transmitted = value;
    while ((position[0] != destination[0] || position[1] != destination[1] ||
            position[2] != destination[2]) &&
           transmitted != 0) {
        int first = -1;
        unsigned axes = 0;
        for (int axis = 0; axis < 3; axis++) {
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
        int step[3];
        for (int axis = 0; axis < 3; axis++) {
            step[axis] = (axes & (1U << axis)) ? (direction[axis] > 0 ? 1 : -1) : 0;
        }
        bool terminal = position[0] + step[0] == destination[0] &&
                        position[1] + step[1] == destination[1] &&
                        position[2] + step[2] == destination[2];
        int coefficient = 256;
        for (unsigned mask = 0; mask < 8; mask++) {
            if ((mask & ~axes) != 0) {
                continue;
            }
            int from[3];
            for (int axis = 0; axis < 3; axis++) {
                from[axis] = position[axis] + ((mask & (1U << axis)) ? step[axis] : 0);
            }
            for (int axis = 0; axis < 3; axis++) {
                if ((axes & (1U << axis)) == 0 || (mask & (1U << axis)) != 0) {
                    continue;
                }
                int face = client_lighting_face_light(grid, from, axis, step[axis], destination,
                                                      terminal);
                if (face < 0) {
                    return 0;
                }
                coefficient = MIN(coefficient, face);
            }
        }
        transmitted = (int32_t)(((int64_t)transmitted * coefficient + 128) / 256);
        for (int axis = 0; axis < 3; axis++) {
            if (axes & (1U << axis)) {
                crossed[axis]++;
                position[axis] += step[axis];
            }
        }
    }
    return transmitted;
}

/** Add one unsigned profile, transmitted from its tile to the target level, to
 * the accumulators. Its vertical reach is that of the server's transport. */
static void client_lighting_add(const client_lighting_grid_t *grid,
                                client_lighting_accumulator_t *accumulators,
                                int x,
                                int y,
                                int z,
                                int mask,
                                int sign,
                                int target,
                                const int64_t linear[3]) {
    if (mask == 0) {
        return;
    }
    int radius = light_profile_radius(mask);
    int dz = grid->target - z;
    if (abs(dz) > MIN(radius, MAP2_MAX_DEPTH)) {
        return;
    }
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int distance_squared = dx * dx + dy * dy + dz * dz;
            if (distance_squared > radius * radius || x + dx < 0 || y + dy < 0 ||
                x + dx >= grid->width || y + dy >= grid->height) {
                continue;
            }
            int32_t value = light_profile_value(mask, distance_squared);
            value = client_lighting_path(grid, x, y, z, dx, dy, dz, value);
            if (value == 0) {
                continue;
            }
            client_lighting_accumulator_t *cell =
                &accumulators[(size_t)(y + dy) * grid->width + (x + dx)];
            if (target == 0) {
                cell->scalar += (int64_t)sign * value;
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

static void client_lighting_accumulate(const client_lighting_grid_t *grid,
                                       const client_lighting_emitter_t *emitters,
                                       size_t count,
                                       client_lighting_pass_t pass,
                                       client_lighting_accumulator_t *accumulators) {
    size_t i = 0;
    while (i < count) {
        int x = emitters[i].x, y = emitters[i].y, z = emitters[i].z;
        size_t end = i;
        bool tile_flickers = false;
        for (; end < count && emitters[end].x == x && emitters[end].y == y &&
               emitters[end].z == z;
             end++) {
            tile_flickers |= client_lighting_emitter_flickers(&emitters[end]);
        }
        if (pass != CLIENT_LIGHTING_PASS_ALL && !tile_flickers) {
            i = end;
            continue;
        }
        /* Summed in 64 bits, so no count of co-located emitters overflows;
         * the profile clamps the sum anyway. */
        int64_t radius = 0, positive_radius = 0;
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
        int scalar_mask = light_profile_mask((int)MAX(INT_MIN, MIN(INT_MAX, radius)));
        client_lighting_add(grid, accumulators, x, y, z, abs(scalar_mask),
                            scalar_mask < 0 ? -1 : 1, 0, NULL);
        client_lighting_add(grid, accumulators, x, y, z,
                            light_profile_mask((int)MIN(INT_MAX, positive_radius)), 1, 1, NULL);
        for (int profile = 1; profile < LIGHT_PROFILE_COUNT; profile++) {
            if (multiplicity[profile] != 0) {
                client_lighting_add(grid, accumulators, x, y, z, profile,
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

void client_lighting_evaluate_stack(int levels,
                                    int target,
                                    int width,
                                    int height,
                                    const uint64_t *occluders,
                                    const client_lighting_emitter_t *emitters,
                                    size_t count,
                                    client_lighting_cell_t *cells) {
    /* A grid without cells, or one whose byte size cannot be represented,
     * has no output to clear. */
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / sizeof(*cells) / (size_t)height) {
        return;
    }
    size_t total = (size_t)width * height;
    memset(cells, 0, total * sizeof(*cells));
    if (count == 0 || target < 0 || target >= levels) {
        return;
    }
    client_lighting_grid_t grid = {.levels = levels, .target = target, .width = width,
                                   .height = height, .occluders = occluders};

    /* Sort a copy by tile so grouping stays linear in the emitter count. The
     * client's fatal allocators keep a failure from caching a dark field. */
    client_lighting_emitter_t *sorted = xmallocarray(count, sizeof(*sorted));
    client_lighting_accumulator_t *all = xcalloc(total, sizeof(*all));
    client_lighting_accumulator_t *flicker = xcalloc(total, sizeof(*flicker));
    client_lighting_accumulator_t *flicker_steady = xcalloc(total, sizeof(*flicker_steady));
    memcpy(sorted, emitters, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), client_lighting_emitter_compare);
    client_lighting_accumulate(&grid, sorted, count, CLIENT_LIGHTING_PASS_ALL, all);
    client_lighting_accumulate(&grid, sorted, count, CLIENT_LIGHTING_PASS_FLICKER_TILES, flicker);
    client_lighting_accumulate(&grid, sorted, count, CLIENT_LIGHTING_PASS_FLICKER_TILES_STEADY,
                               flicker_steady);
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

void client_lighting_evaluate(int width,
                              int height,
                              const uint64_t *occluders,
                              const client_lighting_emitter_t *emitters,
                              size_t count,
                              client_lighting_cell_t *cells) {
    client_lighting_evaluate_stack(1, 0, width, height, occluders, emitters, count, cells);
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
    /* Compared in 64 bits, so no emitter value overflows when negated. */
    int64_t local = emitter_raw;
    int64_t floor = (int64_t)level * step;
    /* The coarse level is a floor (a ceiling for darkness) under the emitter
     * field, never a cap: it bins the strongest single source while the field
     * groups a tile's sent emitters, which may exceed that bin. */
    /* An emitter field of the opposite sign that is at least as strong as the
     * floor is exact sent light and wins: the coarse sign comes from single
     * sources, so grouped sent emitters can outweigh it. */
    if (level > 0 && local < floor && -local < floor) {
        local = floor;
    } else if (level < 0 && local > floor && local < -floor) {
        local = floor;
    }
    return (int32_t)local;
}
