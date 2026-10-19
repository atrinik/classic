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
    /* Bounded origins leave every cursor step and endpoint inside int. */
    if (max_events == 0 || max_events > CELESTIAL_SOLVER_MAX_EVENTS || dx < -65536 ||
        dx > 65536 || dy < -65536 || dy > 65536 || dz < -65536 || dz > 65536 ||
        x < -CELESTIAL_SOLVER_COORDINATE_MAX || x > CELESTIAL_SOLVER_COORDINATE_MAX ||
        y < -CELESTIAL_SOLVER_COORDINATE_MAX || y > CELESTIAL_SOLVER_COORDINATE_MAX ||
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
    /* Outside (0, 32768] there is no source above the horizon to trace, and
     * an azimuth outside the eight directions names no source at all. */
    if (elevation <= 0 || elevation > 32768 || direction < CELESTIAL_MODEL_NORTH ||
        direction > CELESTIAL_MODEL_NORTHWEST) {
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
    /* Sky is asked once per upward face, before anything else: an event
     * that leaves for the sky also crosses, on a tied direction, grazing
     * faces between the unresolved cells above the top level, which are
     * open sky themselves and must not cancel the exit. */
    bool leaves[sizeof(event.faces) / sizeof(event.faces[0])] = {false};
    for (size_t i = 0; i < event.face_count; i++) {
        if (event.faces[i].normal[2] > 0 &&
            lattice->ops->sky(lattice->context, &event.faces[i])) {
            leaves[i] = true;
            sky = true;
        }
    }
    for (size_t i = 0; i < event.face_count; i++) {
        celestial_solver_face_t face = event.faces[i];
        if (leaves[i]) {
            continue;
        }
        /* An unresolved cell (a ray reaching a missing map) is crossed only
         * on the way to a claimed sky; otherwise the ray ends there, so
         * continuous and light only ever see resolved cells. */
        if (face.from.map == NULL || face.to.map == NULL) {
            if (sky) {
                continue;
            }
            ray->done = true;
            return;
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
        /* MIN evaluates its arguments twice: the callback runs once. */
        uint16_t crossing = lattice->ops->light(lattice->context, &face, &crossed_aperture);
        coefficient = MIN(coefficient, crossing);
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

/** Resolve a scratch cell of a map of the given (snapshot) size, reaching
 * past the map's edge only across continuous faces. */
static bool halo_cell(const celestial_solver_lattice_t *lattice, void *map, int width,
                      int height, int x, int y, celestial_solver_cell_t *cell) {
    if (!observe(lattice, map)) {
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

static void halo_ray(const celestial_solver_lattice_t *lattice, celestial_solver_ray_t *ray,
                     void *map, int width, int height, int x, int y, uint8_t direction,
                     int32_t elevation) {
    celestial_solver_cell_t cell;
    if (!halo_cell(lattice, map, width, height, x, y, &cell)) {
        memset(ray, 0, sizeof(*ray));
        ray->done = true;
        return;
    }
    celestial_solver_ray_init(lattice, ray, cell.map, cell.x, cell.y, direction, elevation);
}

void celestial_solver_halo_ray(const celestial_solver_lattice_t *lattice,
                               celestial_solver_ray_t *ray, void *map, int x, int y,
                               uint8_t direction, int32_t elevation) {
    int width, height;
    if (!dimensions(lattice, map, &width, &height)) {
        memset(ray, 0, sizeof(*ray));
        ray->done = true;
        return;
    }
    halo_ray(lattice, ray, map, width, height, x, y, direction, elevation);
}

static uint16_t spill_coefficient(const celestial_solver_lattice_t *lattice, void *map,
                                  int width, int height, int x, int y, int dx, int dy) {
    celestial_solver_cell_t cell;
    if (!halo_cell(lattice, map, width, height, x + dx, y + dy, &cell)) {
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
        uint16_t crossing = lattice->ops->light(lattice->context, &event.faces[face], NULL);
        coefficient = MIN(coefficient, crossing);
    }
    return coefficient;
}

uint16_t celestial_solver_spill(const celestial_solver_lattice_t *lattice, void *map, int x,
                                int y, int dx, int dy) {
    int width, height;
    if (!dimensions(lattice, map, &width, &height)) {
        return 0;
    }
    return spill_coefficient(lattice, map, width, height, x, y, dx, dy);
}

/** Spill coefficients depend only on geometry. One level's solve resolves
 * all eight neighbors once, in the order its first relaxation pass did, and
 * reuses them for every pass and field. When the lattice declines the
 * memory, each pass evaluates them again. */
typedef struct spill_table {
    const celestial_solver_lattice_t *lattice;
    void *map;
    /** Scratch size, and the map size of the solve's dimension snapshot. */
    int width, height, map_width, map_height;
    uint16_t *coefficients;
    size_t bytes;
    /** Whether reserve charged bytes, so release returns exactly them. */
    bool charged;
    bool unavailable;
} spill_table;

/** Allocate a spill table, or mark it unavailable so the relaxation
 * computes each coefficient instead. */
static void spill_table_allocate(spill_table *table) {
    size_t bytes = (size_t)table->width * table->height * 8 * sizeof(*table->coefficients);
    const celestial_solver_ops_t *ops = table->lattice->ops;
    /* The hooks charge as a pair: a charge nothing could return is never
     * taken. */
    bool charged = ops->reserve != NULL && ops->release != NULL;
    if (charged && !ops->reserve(table->lattice->context, bytes)) {
        table->unavailable = true;
        return;
    }
    uint16_t *coefficients = calloc(bytes, 1);
    if (coefficients == NULL) {
        if (charged) {
            ops->release(table->lattice->context, bytes);
        }
        table->unavailable = true;
        return;
    }
    table->charged = charged;
    table->bytes = bytes;
    table->coefficients = coefficients;
}

/** Fill one scratch row of an allocated spill table. */
static void spill_table_fill_row(spill_table *table, int y) {
    for (int x = 0; x < table->width; x++) {
        uint16_t *cell = &table->coefficients[((size_t)x + (size_t)table->width * y) * 8];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if ((dx == 0 && dy == 0) || x + dx < 0 || y + dy < 0 || x + dx >= table->width ||
                    y + dy >= table->height) {
                    continue;
                }
                int neighbor = (dy + 1) * 3 + dx + 1;
                cell[neighbor > 4 ? neighbor - 1 : neighbor] =
                    spill_coefficient(table->lattice, table->map, table->map_width,
                                      table->map_height, x, y, dx, dy);
            }
        }
    }
}

static void spill_table_release(spill_table *table) {
    if (table->coefficients != NULL) {
        free(table->coefficients);
        if (table->charged) {
            table->lattice->ops->release(table->lattice->context, table->bytes);
        }
        table->coefficients = NULL;
        table->charged = false;
    }
}

/** A coefficient from the spill table filled before the relaxation, or
 * computed when the table was unavailable. */
static uint16_t relax_spill(int x, int y, int dx, int dy, void *context) {
    spill_table *table = context;
    if (table->coefficients == NULL) {
        return spill_coefficient(table->lattice, table->map, table->map_width,
                                 table->map_height, x, y, dx, dy);
    }
    int neighbor = (dy + 1) * 3 + dx + 1;
    return table->coefficients[((size_t)x + (size_t)table->width * y) * 8 +
                               (neighbor > 4 ? neighbor - 1 : neighbor)];
}

/** One row of a sun or moon transport. */
static void transport_row(const celestial_solver_lattice_t *lattice, void *map,
                          const celestial_model_t *model, int map_width, int map_height,
                          int width, int y, int32_t *values, int32_t *apertures, bool moon) {
    for (int x = 0; x < width; x++) {
        celestial_solver_ray_t ray;
        halo_ray(lattice, &ray, map, map_width, map_height, x, y,
                 moon ? model->moon_direction : model->solar_direction,
                 moon ? model->moon_elevation : model->solar_elevation);
        while (!ray.done) {
            celestial_solver_ray_step(lattice, &ray);
        }
        int index = x + width * y;
        values[index] =
            celestial_solver_ray_value(&ray, moon ? model->moon : model->direct, &apertures[index]);
    }
}

/** Stages of a solve, in the server's order. */
typedef enum solver_stage {
    STAGE_SKY,
    STAGE_SUN,
    STAGE_MOON,
    /** The spill table, filled only when a relaxed field is lit. */
    STAGE_SPILL,
    /** Each lit relaxed field, pass by pass. */
    STAGE_RELAX,
    STAGE_PUBLISH,
    STAGE_DONE,
} solver_stage;

struct celestial_solver_job {
    celestial_solver_lattice_t lattice;
    void *map;
    celestial_model_t model;
    int32_t *values;
    int32_t (*rgb)[3];
    /** The dimension snapshot sizing every buffer and the publication. */
    int map_width, map_height, width, height;
    int32_t *direct, *diffuse, *aperture_direct, *moon, *moon_aperture, *starlight;
    /** The other grid of a relaxation pass. */
    int32_t *relax_next;
    spill_table spill;
    solver_stage stage;
    /** Next row of the stage, and the field and pass being relaxed. */
    int row;
    int relaxed, pass;
};

/* A relaxation's passes alternate grids and end on the field's own. */
_Static_assert(CELESTIAL_MODEL_SPILL_PASSES % 2 == 0, "relaxation must end on its own grid");

/** The relaxed fields in the server's order: diffuse, aperture direct and
 * starlight. */
static int32_t *job_relaxed(celestial_solver_job_t *job, int relaxed) {
    return relaxed == 0 ? job->diffuse : relaxed == 1 ? job->aperture_direct : job->starlight;
}

static bool grid_lit(const int32_t *values, size_t cells) {
    for (size_t i = 0; i < cells; i++) {
        if (values[i] != 0) {
            return true;
        }
    }
    return false;
}

/** Advance to the next lit relaxed field from relaxed on, or to the
 * publish; an unlit field relaxes to itself. */
static void job_next_relaxed(celestial_solver_job_t *job, int relaxed) {
    size_t cells = (size_t)job->width * job->height;
    while (relaxed < 3 && !grid_lit(job_relaxed(job, relaxed), cells)) {
        relaxed++;
    }
    job->relaxed = relaxed;
    job->pass = 0;
    job->row = 0;
    if (relaxed == 3) {
        spill_table_release(&job->spill);
        job->stage = STAGE_PUBLISH;
    } else {
        job->stage = STAGE_RELAX;
    }
}

celestial_solver_job_t *celestial_solver_job_create(const celestial_solver_lattice_t *lattice,
                                                    void *map, const celestial_model_t *model,
                                                    int32_t *values, int32_t (*rgb)[3]) {
    int map_width, map_height;
    if (!dimensions(lattice, map, &map_width, &map_height)) {
        return NULL;
    }
    celestial_solver_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return NULL;
    }
    /* One snapshot sizes every buffer and the publication: a lattice whose
     * dimensions change between steps cannot make them disagree. */
    job->lattice = *lattice;
    job->map = map;
    job->model = *model;
    job->values = values;
    job->rgb = rgb;
    job->map_width = map_width;
    job->map_height = map_height;
    job->width = map_width + 2 * CELESTIAL_MODEL_SPILL_PASSES;
    job->height = map_height + 2 * CELESTIAL_MODEL_SPILL_PASSES;
    size_t cells = (size_t)job->width * job->height;
    job->direct = calloc(cells, sizeof(*job->direct));
    job->diffuse = calloc(cells, sizeof(*job->diffuse));
    job->aperture_direct = calloc(cells, sizeof(*job->aperture_direct));
    job->moon = calloc(cells, sizeof(*job->moon));
    job->moon_aperture = calloc(cells, sizeof(*job->moon_aperture));
    job->starlight = calloc(cells, sizeof(*job->starlight));
    job->relax_next = calloc(cells, sizeof(*job->relax_next));
    job->spill = (spill_table){.lattice = &job->lattice, .map = map, .width = job->width,
                               .height = job->height, .map_width = map_width,
                               .map_height = map_height};
    if (job->direct == NULL || job->diffuse == NULL || job->aperture_direct == NULL ||
        job->moon == NULL || job->moon_aperture == NULL || job->starlight == NULL ||
        job->relax_next == NULL) {
        celestial_solver_job_free(job);
        return NULL;
    }
    return job;
}

celestial_solver_status_t celestial_solver_job_step(celestial_solver_job_t *job) {
    const celestial_solver_lattice_t *lattice = &job->lattice;
    const celestial_model_t *model = &job->model;
    int width = job->width;
    switch (job->stage) {
    case STAGE_SKY:
        for (int x = 0; x < width; x++) {
            int index = x + width * job->row;
            celestial_solver_ray_t ray;
            halo_ray(lattice, &ray, job->map, job->map_width, job->map_height, x, job->row,
                     CELESTIAL_MODEL_NORTH, 32768);
            while (!ray.done) {
                celestial_solver_ray_step(lattice, &ray);
            }
            job->diffuse[index] = celestial_solver_ray_value(&ray, model->diffuse, NULL);
            job->starlight[index] = celestial_solver_ray_value(&ray, model->starlight, NULL);
        }
        break;
    case STAGE_SUN:
        transport_row(lattice, job->map, model, job->map_width, job->map_height, width, job->row,
                      job->direct, job->aperture_direct, false);
        break;
    case STAGE_MOON:
        transport_row(lattice, job->map, model, job->map_width, job->map_height, width, job->row,
                      job->moon, job->moon_aperture, true);
        break;
    case STAGE_SPILL:
        if (job->row == 0) {
            spill_table_allocate(&job->spill);
        }
        if (job->spill.coefficients == NULL) {
            /* Unavailable: the relaxation computes each coefficient. */
            job_next_relaxed(job, 0);
            return CELESTIAL_SOLVER_PENDING;
        }
        spill_table_fill_row(&job->spill, job->row);
        if (++job->row == job->height) {
            job_next_relaxed(job, 0);
        }
        return CELESTIAL_SOLVER_PENDING;
    case STAGE_RELAX: {
        int32_t *field = job_relaxed(job, job->relaxed);
        const int32_t *from = job->pass % 2 == 0 ? field : job->relax_next;
        int32_t *to = job->pass % 2 == 0 ? job->relax_next : field;
        celestial_model_relax_row(from, to, width, job->height, job->row, relax_spill,
                                  &job->spill);
        if (++job->row == job->height) {
            job->row = 0;
            if (++job->pass == CELESTIAL_MODEL_SPILL_PASSES) {
                job_next_relaxed(job, job->relaxed + 1);
            }
        }
        return CELESTIAL_SOLVER_PENDING;
    }
    case STAGE_PUBLISH:
        /* One output row per step, like every other stage. */
        for (int x = 0; x < job->map_width; x++) {
            int index = x + CELESTIAL_MODEL_SPILL_PASSES +
                        width * (job->row + CELESTIAL_MODEL_SPILL_PASSES);
            size_t output = (size_t)x + (size_t)job->map_width * job->row;
            celestial_model_publish(model, job->direct[index], job->diffuse[index],
                                    job->aperture_direct[index], job->moon[index],
                                    job->moon_aperture[index], job->starlight[index],
                                    &job->values[output], job->rgb[output]);
        }
        if (++job->row < job->map_height) {
            return CELESTIAL_SOLVER_PENDING;
        }
        job->stage = STAGE_DONE;
        return CELESTIAL_SOLVER_DONE;
    case STAGE_DONE:
        return CELESTIAL_SOLVER_DONE;
    }
    /* A ray stage advances by rows; the moon stage runs only under a moon,
     * and the spill table is filled only for a lit relaxed field, so an
     * unlit one never allocates it. */
    if (++job->row == job->height) {
        job->row = 0;
        job->stage++;
        if (job->stage == STAGE_MOON && model->moon == 0) {
            job->stage++;
        }
        if (job->stage == STAGE_SPILL) {
            size_t cells = (size_t)width * job->height;
            if (!grid_lit(job->diffuse, cells) && !grid_lit(job->aperture_direct, cells) &&
                !grid_lit(job->starlight, cells)) {
                job_next_relaxed(job, 3);
            }
        }
    }
    return CELESTIAL_SOLVER_PENDING;
}

void celestial_solver_job_free(celestial_solver_job_t *job) {
    if (job == NULL) {
        return;
    }
    spill_table_release(&job->spill);
    free(job->relax_next);
    free(job->starlight);
    free(job->moon_aperture);
    free(job->moon);
    free(job->aperture_direct);
    free(job->diffuse);
    free(job->direct);
    free(job);
}

bool celestial_solver_solve(const celestial_solver_lattice_t *lattice, void *map,
                            const celestial_model_t *model, int32_t *values,
                            int32_t (*rgb)[3]) {
    celestial_solver_job_t *job = celestial_solver_job_create(lattice, map, model, values, rgb);
    if (job == NULL) {
        return false;
    }
    celestial_solver_status_t status;
    do {
        status = celestial_solver_job_step(job);
    } while (status == CELESTIAL_SOLVER_PENDING);
    celestial_solver_job_free(job);
    return status == CELESTIAL_SOLVER_DONE;
}
