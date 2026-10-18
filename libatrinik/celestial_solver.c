/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Celestial ray transport and diffuse relaxation over an abstract lattice,
 * shared by the server and the client.
 */

#include "celestial_solver.h"
#include "toolkit.h"

#include <stdlib.h>
#include <string.h>

static bool observe(const celestial_solver_lattice_t *lattice, void *map) {
    return map == NULL || lattice->ops->observe == NULL ||
           lattice->ops->observe(lattice->context, map);
}

static bool observe_event(const celestial_solver_lattice_t *lattice,
                          const celestial_solver_event_t *event) {
    for (size_t i = 0; i < event->face_count; i++) {
        if (!observe(lattice, event->faces[i].from.map) ||
            !observe(lattice, event->faces[i].to.map)) {
            return false;
        }
    }
    return true;
}

static bool resolve(const celestial_solver_lattice_t *lattice, void *origin, int x, int y, int z,
                    celestial_solver_cell_t *cell) {
    *cell = (celestial_solver_cell_t){.x = x, .y = y, .z = z};
    if (!lattice->ops->resolve(lattice->context, origin, x, y, z, cell) || cell->map == NULL) {
        cell->map = NULL;
        return false;
    }
    return true;
}

bool celestial_solver_direction_init(const celestial_solver_lattice_t *lattice,
                                     celestial_solver_cursor_t *cursor, void *origin, int x,
                                     int y, int dx, int dy, int dz, unsigned max_events) {
    if (cursor == NULL) {
        return false;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->failed = true;
    celestial_solver_cell_t start;
    if (max_events == 0 || max_events > CELESTIAL_SOLVER_MAX_EVENTS || dx < -65536 ||
        dx > 65536 || dy < -65536 || dy > 65536 || dz < -65536 || dz > 65536 ||
        (dx == 0 && dy == 0 && dz == 0) || !resolve(lattice, origin, x, y, 0, &start)) {
        return false;
    }
    cursor->origin = origin;
    cursor->position[0] = x;
    cursor->position[1] = y;
    cursor->direction[0] = dx;
    cursor->direction[1] = dy;
    cursor->direction[2] = dz;
    cursor->limit = max_events;
    cursor->failed = false;
    return true;
}

bool celestial_solver_segment_init(const celestial_solver_lattice_t *lattice,
                                   celestial_solver_cursor_t *cursor, void *origin, int x, int y,
                                   int dx, int dy, int dz) {
    if (dx < -4096 || dx > 4096 || dy < -4096 || dy > 4096 ||
        dz <= -CELESTIAL_SOLVER_LEVELS || dz >= CELESTIAL_SOLVER_LEVELS) {
        return false;
    }
    bool stationary = dx == 0 && dy == 0 && dz == 0;
    if (!celestial_solver_direction_init(lattice, cursor, origin, x, y, stationary ? 1 : dx, dy,
                                         dz, CELESTIAL_SOLVER_MAX_EVENTS)) {
        return false;
    }
    cursor->segment = true;
    cursor->endpoint[0] = x + dx;
    cursor->endpoint[1] = y + dy;
    cursor->endpoint[2] = dz;
    return true;
}

int celestial_solver_next(const celestial_solver_lattice_t *lattice,
                          celestial_solver_cursor_t *cursor, celestial_solver_event_t *event) {
    if (cursor == NULL || event == NULL || cursor->failed) {
        return -1;
    }
    memset(event, 0, sizeof(*event));
    if (cursor->segment &&
        memcmp(cursor->position, cursor->endpoint, sizeof(cursor->position)) == 0) {
        return 0;
    }
    if (cursor->events == cursor->limit) {
        cursor->failed = true;
        return -1;
    }
    int first = -1;
    unsigned axes = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (cursor->direction[axis] == 0) {
            continue;
        }
        if (first < 0) {
            first = axis;
            axes = 1U << axis;
            continue;
        }
        uint64_t left = (2ULL * cursor->crossed[axis] + 1) * (unsigned)abs(cursor->direction[first]);
        uint64_t right = (2ULL * cursor->crossed[first] + 1) * (unsigned)abs(cursor->direction[axis]);
        if (left < right) {
            first = axis;
            axes = 1U << axis;
        } else if (left == right) {
            axes |= 1U << axis;
        }
    }
    celestial_solver_cell_t cells[8];
    event->valid = true;
    event->axes = axes;
    for (unsigned mask = 0; mask < 8; mask++) {
        if ((mask & ~axes) != 0) {
            continue;
        }
        int point[3];
        for (int axis = 0; axis < 3; axis++) {
            point[axis] = cursor->position[axis] +
                          ((mask & (1U << axis)) != 0 ? (cursor->direction[axis] > 0 ? 1 : -1) : 0);
        }
        if (!resolve(lattice, cursor->origin, point[0], point[1], point[2], &cells[mask])) {
            event->valid = false;
        }
        if (mask != 0) {
            event->cells[event->cell_count++] = cells[mask];
        }
    }
    for (unsigned mask = 0; mask < 8; mask++) {
        if ((mask & ~axes) != 0) {
            continue;
        }
        for (int axis = 0; axis < 3; axis++) {
            unsigned bit = 1U << axis;
            if ((axes & bit) == 0 || (mask & bit) != 0) {
                continue;
            }
            celestial_solver_face_t *face = &event->faces[event->face_count++];
            face->from = cells[mask];
            face->to = cells[mask | bit];
            face->normal[axis] = cursor->direction[axis] > 0 ? 1 : -1;
        }
    }
    for (int axis = 0; axis < 3; axis++) {
        if ((axes & (1U << axis)) != 0) {
            cursor->position[axis] += cursor->direction[axis] > 0 ? 1 : -1;
            cursor->crossed[axis]++;
        }
    }
    cursor->events++;
    event->destination = cells[axes];
    event->terminal = cursor->segment &&
                      memcmp(cursor->position, cursor->endpoint, sizeof(cursor->position)) == 0;
    if (!event->valid) {
        cursor->failed = true;
    }
    return 1;
}

uint32_t celestial_solver_sqrt(uint64_t value) {
    uint64_t root = 0, bit = UINT64_C(1) << 62;
    while (bit > value) {
        bit >>= 2;
    }
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

/** Direction of light travel from a source at the given azimuth. */
static void direction_delta(uint8_t direction, int *dx, int *dy) {
    static const int8_t delta[8][2] = {
        {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1},
    };
    *dx = 0;
    *dy = 0;
    if (direction >= CELESTIAL_MODEL_NORTH && direction <= CELESTIAL_MODEL_NORTHWEST) {
        *dx = delta[direction - CELESTIAL_MODEL_NORTH][0];
        *dy = delta[direction - CELESTIAL_MODEL_NORTH][1];
    }
}

void celestial_solver_ray_init(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray, void *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    memset(ray, 0, sizeof(*ray));
    /* Outside (0, 32768] there is no source above the horizon to trace. */
    if (elevation <= 0 || elevation > 32768) {
        ray->done = true;
        return;
    }
    int dx, dy;
    direction_delta(direction, &dx, &dy);
    uint64_t horizontal_squared = UINT64_C(32768) * 32768 - (uint64_t)elevation * elevation;
    uint32_t horizontal =
        celestial_solver_sqrt(horizontal_squared / (dx != 0 && dy != 0 ? 2 : 1));
    /* Azimuth names the source; the delta names light travel. */
    ray->done = !observe(lattice, map) ||
                !celestial_solver_direction_init(lattice, &ray->cursor, map, x, y,
                                                 -dx * (int)horizontal, -dy * (int)horizontal,
                                                 elevation, CELESTIAL_SOLVER_RAY_EVENTS);
}

void celestial_solver_ray_step(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray) {
    if (ray->done) {
        return;
    }
    celestial_solver_event_t event;
    if (celestial_solver_next(lattice, &ray->cursor, &event) != 1) {
        ray->done = true;
        return;
    }
    if (!observe_event(lattice, &event)) {
        ray->done = true;
        return;
    }
    if ((event.axes & 3) != 0 && ++ray->horizontal > CELESTIAL_MODEL_DIRECT_REACH) {
        ray->done = true;
        return;
    }
    uint16_t coefficient = CELESTIAL_MODEL_SCALE;
    bool aperture = false, sky = false;
    for (size_t i = 0; i < event.face_count; i++) {
        celestial_solver_face_t face = event.faces[i];
        if (face.normal[2] > 0 && lattice->ops->sky(lattice->context, &face)) {
            sky = true;
            continue;
        }
        if (!lattice->ops->continuous(lattice->context, &face)) {
            ray->done = true;
            return;
        }
        /* The backtrace walks from the receiver; light crosses the other way. */
        celestial_solver_cell_t swap = face.from;
        face.from = face.to;
        face.to = swap;
        for (size_t axis = 0; axis < 3; axis++) {
            face.normal[axis] = (int8_t)-face.normal[axis];
        }
        bool crossed_aperture = false;
        coefficient = MIN(coefficient,
                          lattice->ops->light(lattice->context, &face, &crossed_aperture));
        aperture |= crossed_aperture;
    }
    if (ray->count == CELESTIAL_SOLVER_RAY_EVENTS || coefficient == 0) {
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

int32_t celestial_solver_ray_value(const celestial_solver_ray_t *ray, int32_t source,
                                   int32_t *aperture) {
    return celestial_model_ray_value(ray->coefficients, ray->apertures, ray->count, ray->sky,
                                     source, aperture);
}

/** The map's dimensions; false, with both zeroed, when either is outside
 * 1..CELESTIAL_SOLVER_DIMENSION_MAX, so no size arithmetic can overflow. */
static bool dimensions(const celestial_solver_lattice_t *lattice, void *map, int *width,
                       int *height) {
    *width = 0;
    *height = 0;
    lattice->ops->dimensions(lattice->context, map, width, height);
    if (*width < 1 || *height < 1 || *width > CELESTIAL_SOLVER_DIMENSION_MAX ||
        *height > CELESTIAL_SOLVER_DIMENSION_MAX) {
        *width = 0;
        *height = 0;
        return false;
    }
    return true;
}

int celestial_solver_scratch_width(const celestial_solver_lattice_t *lattice, void *map) {
    int width, height;
    if (!dimensions(lattice, map, &width, &height)) {
        return 0;
    }
    return width + 2 * CELESTIAL_MODEL_SPILL_PASSES;
}

int celestial_solver_scratch_height(const celestial_solver_lattice_t *lattice, void *map) {
    int width, height;
    if (!dimensions(lattice, map, &width, &height)) {
        return 0;
    }
    return height + 2 * CELESTIAL_MODEL_SPILL_PASSES;
}

/** Resolve a scratch cell, reaching past the map's edge only across
 * continuous faces. */
static bool halo_cell(const celestial_solver_lattice_t *lattice, void *map, int x, int y,
                      celestial_solver_cell_t *cell) {
    if (!observe(lattice, map)) {
        return false;
    }
    int width, height;
    if (!dimensions(lattice, map, &width, &height)) {
        return false;
    }
    x -= CELESTIAL_MODEL_SPILL_PASSES;
    y -= CELESTIAL_MODEL_SPILL_PASSES;
    int start_x = MAX(0, MIN(x, width - 1));
    int start_y = MAX(0, MIN(y, height - 1));
    if (start_x != x || start_y != y) {
        celestial_solver_cursor_t cursor;
        if (!celestial_solver_segment_init(lattice, &cursor, map, start_x, start_y, x - start_x,
                                           y - start_y, 0)) {
            return false;
        }
        celestial_solver_event_t event;
        int status;
        while ((status = celestial_solver_next(lattice, &cursor, &event)) == 1) {
            if (!observe_event(lattice, &event) || !event.valid) {
                return false;
            }
            for (size_t face = 0; face < event.face_count; face++) {
                if (!lattice->ops->continuous(lattice->context, &event.faces[face])) {
                    return false;
                }
            }
        }
        if (status != 0) {
            return false;
        }
    }
    return resolve(lattice, map, x, y, 0, cell) && observe(lattice, cell->map);
}

void celestial_solver_halo_ray(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray, void *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    celestial_solver_cell_t cell;
    if (!halo_cell(lattice, map, x, y, &cell)) {
        memset(ray, 0, sizeof(*ray));
        ray->done = true;
        return;
    }
    celestial_solver_ray_init(lattice, ray, cell.map, cell.x, cell.y, direction, elevation);
}

uint16_t celestial_solver_spill(const celestial_solver_lattice_t *lattice, void *map, int x,
                                int y, int dx, int dy) {
    celestial_solver_cell_t cell;
    if (!halo_cell(lattice, map, x + dx, y + dy, &cell)) {
        return 0;
    }
    celestial_solver_cursor_t cursor;
    celestial_solver_event_t event;
    if (!celestial_solver_segment_init(lattice, &cursor, cell.map, cell.x, cell.y, -dx, -dy, 0) ||
        celestial_solver_next(lattice, &cursor, &event) != 1 || !observe_event(lattice, &event) ||
        !event.valid) {
        return 0;
    }
    for (size_t face = 0; face < event.face_count; face++) {
        if (!lattice->ops->continuous(lattice->context, &event.faces[face])) {
            return 0;
        }
    }
    uint16_t coefficient = CELESTIAL_MODEL_SCALE;
    for (size_t face = 0; face < event.face_count; face++) {
        coefficient = MIN(coefficient,
                          lattice->ops->light(lattice->context, &event.faces[face], NULL));
    }
    return coefficient;
}

/** Spill coefficients depend only on geometry. One level's solve resolves
 * all eight neighbors once, in the order its first relaxation pass did, and
 * reuses them for every pass and field. When the lattice declines the
 * memory, each pass evaluates them again. */
typedef struct spill_table {
    const celestial_solver_lattice_t *lattice;
    void *map;
    int width, height;
    uint16_t *coefficients;
    size_t bytes;
    /** Whether reserve charged bytes, so release returns exactly them. */
    bool charged;
    bool unavailable;
} spill_table;

static void spill_table_fill(spill_table *table) {
    size_t bytes = (size_t)table->width * table->height * 8 * sizeof(*table->coefficients);
    const celestial_solver_ops_t *ops = table->lattice->ops;
    if (ops->reserve != NULL && !ops->reserve(table->lattice->context, bytes)) {
        table->unavailable = true;
        return;
    }
    bool charged = ops->reserve != NULL;
    uint16_t *coefficients = calloc(bytes, 1);
    if (coefficients == NULL) {
        if (charged && ops->release != NULL) {
            ops->release(table->lattice->context, bytes);
        }
        table->unavailable = true;
        return;
    }
    table->charged = charged;
    table->bytes = bytes;
    table->coefficients = coefficients;
    for (int y = 0; y < table->height; y++) {
        for (int x = 0; x < table->width; x++) {
            uint16_t *cell = &coefficients[((size_t)x + (size_t)table->width * y) * 8];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 ||
                        x + dx >= table->width || y + dy >= table->height) {
                        continue;
                    }
                    int neighbor = (dy + 1) * 3 + dx + 1;
                    cell[neighbor > 4 ? neighbor - 1 : neighbor] =
                        celestial_solver_spill(table->lattice, table->map, x, y, dx, dy);
                }
            }
        }
    }
}

static void spill_table_release(spill_table *table) {
    if (table->coefficients != NULL) {
        free(table->coefficients);
        if (table->charged && table->lattice->ops->release != NULL) {
            table->lattice->ops->release(table->lattice->context, table->bytes);
        }
    }
}

/** A level's spill table, filled on the relaxation's first coefficient
 * request so an unlit field never allocates it. */
static uint16_t relax_spill(int x, int y, int dx, int dy, void *context) {
    spill_table *table = context;
    if (table->coefficients == NULL && !table->unavailable) {
        spill_table_fill(table);
    }
    if (table->coefficients == NULL) {
        return celestial_solver_spill(table->lattice, table->map, x, y, dx, dy);
    }
    int neighbor = (dy + 1) * 3 + dx + 1;
    return table->coefficients[((size_t)x + (size_t)table->width * y) * 8 +
                               (neighbor > 4 ? neighbor - 1 : neighbor)];
}

static void transport_line(const celestial_solver_lattice_t *lattice, void *map,
                           const celestial_model_t *model, int width, int height, int32_t *values,
                           int32_t *apertures, bool moon) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            celestial_solver_ray_t ray;
            celestial_solver_halo_ray(lattice, &ray, map, x, y,
                                      moon ? model->moon_direction : model->solar_direction,
                                      moon ? model->moon_elevation : model->solar_elevation);
            while (!ray.done) {
                celestial_solver_ray_step(lattice, &ray);
            }
            int index = x + width * y;
            values[index] = celestial_solver_ray_value(&ray, moon ? model->moon : model->direct,
                                                       &apertures[index]);
        }
    }
}

bool celestial_solver_solve(const celestial_solver_lattice_t *lattice, void *map,
                            const celestial_model_t *model, int32_t *values,
                            int32_t (*rgb)[3]) {
    int map_width, map_height;
    if (!dimensions(lattice, map, &map_width, &map_height)) {
        return false;
    }
    int width = celestial_solver_scratch_width(lattice, map);
    int height = celestial_solver_scratch_height(lattice, map);
    size_t cells = (size_t)width * height;
    int32_t *direct = calloc(cells, sizeof(*direct));
    int32_t *diffuse = calloc(cells, sizeof(*diffuse));
    int32_t *aperture_direct = calloc(cells, sizeof(*aperture_direct));
    int32_t *moon = calloc(cells, sizeof(*moon));
    int32_t *moon_aperture = calloc(cells, sizeof(*moon_aperture));
    int32_t *starlight = calloc(cells, sizeof(*starlight));
    bool complete = direct != NULL && diffuse != NULL && aperture_direct != NULL &&
                    moon != NULL && moon_aperture != NULL && starlight != NULL;
    if (complete) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int index = x + width * y;
                celestial_solver_ray_t ray;
                celestial_solver_halo_ray(lattice, &ray, map, x, y, CELESTIAL_MODEL_NORTH, 32768);
                while (!ray.done) {
                    celestial_solver_ray_step(lattice, &ray);
                }
                diffuse[index] = celestial_solver_ray_value(&ray, model->diffuse, NULL);
                starlight[index] = celestial_solver_ray_value(&ray, model->starlight, NULL);
            }
        }
        transport_line(lattice, map, model, width, height, direct, aperture_direct, false);
        if (model->moon != 0) {
            transport_line(lattice, map, model, width, height, moon, moon_aperture, true);
        }
        spill_table spill = {.lattice = lattice, .map = map, .width = width, .height = height};
        complete = celestial_model_relax(diffuse, width, height, relax_spill, &spill) &&
                   celestial_model_relax(aperture_direct, width, height, relax_spill, &spill) &&
                   celestial_model_relax(starlight, width, height, relax_spill, &spill);
        spill_table_release(&spill);
    }
    if (complete) {
        for (int y = 0; y < map_height; y++) {
            for (int x = 0; x < map_width; x++) {
                int index = x + CELESTIAL_MODEL_SPILL_PASSES +
                            width * (y + CELESTIAL_MODEL_SPILL_PASSES);
                size_t output = (size_t)x + (size_t)map_width * y;
                celestial_model_publish(model, direct[index], diffuse[index],
                                        aperture_direct[index], moon[index], moon_aperture[index],
                                        starlight[index], &values[output], rgb[output]);
            }
        }
    }
    free(starlight);
    free(moon_aperture);
    free(moon);
    free(aperture_direct);
    free(diffuse);
    free(direct);
    return complete;
}
