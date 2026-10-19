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
 * CELESTIAL_STRUCTURE codec and the closure lattice of the shared solver.
 */

#include "celestial_closure.h"
#include "packet.h"
#include "toolkit.h"

#include <stdlib.h>
#include <string.h>

/** Coefficient of each two-bit transmission class. */
static const uint16_t class_coefficient[4] = {0, 192, 224, CELESTIAL_MODEL_SCALE};

void celestial_closure_free(celestial_closure_t *closure) {
    if (closure == NULL) {
        return;
    }
    /* A rejected build may record a count without its array. */
    for (size_t i = 0; closure->fragments != NULL && i < closure->count; i++) {
        free(closure->fragments[i].values);
    }
    free(closure->fragments);
    free(closure->index);
    free(closure->lattice);
    memset(closure, 0, sizeof(*closure));
}

static void *closure_lattice_create(const celestial_closure_t *closure);

bool celestial_closure_value_valid(uint32_t value) {
    if ((value & CELESTIAL_CLOSURE_OUTSIDE) != 0) {
        return value == CELESTIAL_CLOSURE_OUTSIDE_RECORD;
    }
    return value == 0 ||
           ((value & ~CELESTIAL_CLOSURE_VALUE_MASK) == 0 && (value & CELESTIAL_CLOSURE_IN) != 0 &&
            /* An upward exit to the sky has no cell above to continue into,
             * and leaves only through an open cover: the sky face skips the
             * cover, so a sealed cell claiming one would see the sky. */
            ((value & CELESTIAL_CLOSURE_SKY_EXIT) == 0 ||
             ((value & CELESTIAL_CLOSURE_CONTINUOUS_UP) == 0 &&
              (value & CELESTIAL_CLOSURE_COVER_OPEN) != 0)));
}

static size_t index_cells(const celestial_closure_t *closure) {
    return (size_t)closure->window_width * closure->window_height;
}

/** As celestial_closure_build(), setting *out_of_memory when it failed only
 * because an allocation did. */
static bool closure_build(celestial_closure_t *closure, uint32_t generation, int window_width,
                          int window_height, celestial_closure_fragment_t *fragments,
                          size_t count, bool *out_of_memory) {
    memset(closure, 0, sizeof(*closure));
    closure->generation = generation;
    closure->window_width = window_width;
    closure->window_height = window_height;
    closure->fragments = fragments;
    closure->count = count;
    bool valid = window_width > 0 && window_height > 0 && window_width <= 255 &&
                 window_height <= 255 && count <= UINT16_MAX && (count == 0 || fragments != NULL);
    if (valid) {
        closure->index = calloc((size_t)CELESTIAL_CLOSURE_DEPTHS * index_cells(closure),
                                sizeof(*closure->index));
        valid = closure->index != NULL;
        *out_of_memory = !valid;
    }
    for (size_t i = 0; valid && i < count; i++) {
        const celestial_closure_fragment_t *fragment = &fragments[i];
        valid = fragment->depth >= -CELESTIAL_CLOSURE_DEPTH_MAX &&
                fragment->depth <= CELESTIAL_CLOSURE_DEPTH_MAX && fragment->width != 0 &&
                fragment->height != 0 && fragment->x + fragment->width <= window_width &&
                fragment->y + fragment->height <= window_height && fragment->values != NULL;
        uint16_t *depth = valid ? &closure->index[(size_t)(fragment->depth +
                                                           CELESTIAL_CLOSURE_DEPTH_MAX) *
                                                  index_cells(closure)]
                                : NULL;
        for (int y = 0; valid && y < fragment->height; y++) {
            for (int x = 0; valid && x < fragment->width; x++) {
                uint16_t *cell = &depth[(size_t)(fragment->x + x) +
                                        (size_t)window_width * (fragment->y + y)];
                valid = *cell == 0 &&
                        celestial_closure_value_valid(
                            fragment->values[(size_t)x + (size_t)fragment->width * y]);
                *cell = (uint16_t)(i + 1);
            }
        }
    }
    /* A sky exit has no cell above it: a closure cell there, other than an
     * outside record, contradicts it, and solving would skip that cell. */
    for (size_t i = 0; valid && i < count; i++) {
        const celestial_closure_fragment_t *fragment = &fragments[i];
        for (int y = 0; valid && fragment->depth < CELESTIAL_CLOSURE_DEPTH_MAX &&
                        y < fragment->height;
             y++) {
            for (int x = 0; valid && x < fragment->width; x++) {
                if ((fragment->values[(size_t)x + (size_t)fragment->width * y] &
                     CELESTIAL_CLOSURE_SKY_EXIT) == 0) {
                    continue;
                }
                uint32_t above = celestial_closure_value(closure, fragment->depth + 1,
                                                         fragment->x + x, fragment->y + y);
                valid = above == 0 || above == CELESTIAL_CLOSURE_OUTSIDE_RECORD;
            }
        }
    }
    if (valid) {
        closure->lattice = closure_lattice_create(closure);
        valid = closure->lattice != NULL;
        *out_of_memory = !valid;
    }
    if (!valid) {
        celestial_closure_free(closure);
    }
    return valid;
}

bool celestial_closure_build(celestial_closure_t *closure, uint32_t generation, int window_width,
                             int window_height, celestial_closure_fragment_t *fragments,
                             size_t count) {
    bool out_of_memory = false;
    return closure_build(closure, generation, window_width, window_height, fragments, count,
                         &out_of_memory);
}

/** Number of runs of one fragment, each at most 255 equal row-major cells. */
static size_t fragment_runs(const celestial_closure_fragment_t *fragment) {
    size_t cells = (size_t)fragment->width * fragment->height, runs = 0;
    for (size_t i = 0; i < cells;) {
        size_t length = 1;
        while (i + length < cells && length < 255 &&
               fragment->values[i + length] == fragment->values[i]) {
            length++;
        }
        runs++;
        i += length;
    }
    return runs;
}

size_t celestial_closure_size(const celestial_closure_t *closure) {
    size_t size = 4 + 2;
    for (size_t i = 0; i < closure->count; i++) {
        size += 6 + 5 * fragment_runs(&closure->fragments[i]);
    }
    return size;
}

bool celestial_closure_write(packet_struct *packet, const celestial_closure_t *closure) {
    /* Only a closure from celestial_closure_build() or _read() is valid; it
     * always carries its lattice, so anything else is refused unread. */
    if (packet == NULL || closure == NULL || closure->lattice == NULL ||
        closure->count > UINT16_MAX ||
        celestial_closure_size(closure) > PACKET_PAYLOAD_MAX ||
        packet_writer_error(packet) != PACKET_ERROR_NONE) {
        return false;
    }
    packet_writer_mark_t mark;
    packet_writer_mark(packet, &mark);
    packet_writer_write_uint32(packet, closure->generation);
    packet_writer_write_uint16(packet, (uint16_t)closure->count);
    for (size_t i = 0; i < closure->count; i++) {
        const celestial_closure_fragment_t *fragment = &closure->fragments[i];
        packet_writer_write_int8(packet, fragment->depth);
        packet_writer_write_uint8(packet, fragment->x);
        packet_writer_write_uint8(packet, fragment->y);
        packet_writer_write_uint8(packet, fragment->width);
        packet_writer_write_uint8(packet, fragment->height);
        packet_writer_write_uint8(packet, fragment->slot);
        size_t cells = (size_t)fragment->width * fragment->height;
        for (size_t cell = 0; cell < cells;) {
            size_t length = 1;
            while (cell + length < cells && length < 255 &&
                   fragment->values[cell + length] == fragment->values[cell]) {
                length++;
            }
            uint32_t value = fragment->values[cell];
            packet_writer_write_uint8(packet, (uint8_t)length);
            packet_writer_write_uint32(packet, value);
            cell += length;
        }
    }
    if (packet_writer_error(packet) != PACKET_ERROR_NONE) {
        /* A partial payload is never left behind. */
        packet_writer_rollback(packet, &mark);
        return false;
    }
    return true;
}

/**
 * Check, without allocating, that the runs at data[pos] encode exactly cells
 * valid values: their framing and coverage are proven before a fragment's
 * array is allocated, so only a well-formed payload can run out of memory.
 */
static bool closure_runs_valid(const uint8_t *data, size_t len, size_t pos, size_t cells) {
    for (size_t cell = 0; cell < cells; pos += 5) {
        if (pos > len || len - pos < 5) {
            return false;
        }
        uint8_t length = data[pos];
        uint32_t value = (uint32_t)data[pos + 1] << 24 | (uint32_t)data[pos + 2] << 16 |
                         (uint32_t)data[pos + 3] << 8 | data[pos + 4];
        if (length == 0 || length > cells - cell || !celestial_closure_value_valid(value)) {
            return false;
        }
        cell += length;
    }
    return true;
}

bool celestial_closure_decode(const uint8_t *data, size_t len, int window_width,
                              int window_height, celestial_closure_t *closure,
                              bool *out_of_memory) {
    *out_of_memory = false;
    if (closure == NULL) {
        return false;
    }
    memset(closure, 0, sizeof(*closure));
    if (data == NULL) {
        return false;
    }
    packet_reader_t reader;
    packet_reader_init(&reader, data, len);
    uint32_t generation = packet_reader_read_uint32(&reader);
    uint16_t count = packet_reader_read_uint16(&reader);
    if (packet_reader_error(&reader) != PACKET_ERROR_NONE) {
        return false;
    }
    /* A fragment takes at least its header and one run: a count the payload
     * cannot hold, or more fragments than window cells over every depth, is
     * malformed before anything is allocated. */
    if ((size_t)count * (6 + 5) > len - 6 ||
        (window_width > 0 && window_height > 0 &&
         count > (size_t)window_width * window_height * CELESTIAL_CLOSURE_DEPTHS)) {
        return false;
    }
    celestial_closure_fragment_t *fragments =
        count != 0 ? calloc(count, sizeof(*fragments)) : NULL;
    if (count != 0 && fragments == NULL) {
        *out_of_memory = true;
        return false;
    }
    bool valid = true;
    size_t parsed = 0;
    /* Non-overlapping fragments cover at most every window cell of every
     * depth: a payload claiming more is rejected before it is allocated. */
    size_t total = 0, total_max = window_width > 0 && window_height > 0
                                      ? (size_t)window_width * window_height *
                                            CELESTIAL_CLOSURE_DEPTHS
                                      : 0;
    for (; valid && parsed < count; parsed++) {
        celestial_closure_fragment_t *fragment = &fragments[parsed];
        fragment->depth = packet_reader_read_int8(&reader);
        fragment->x = packet_reader_read_uint8(&reader);
        fragment->y = packet_reader_read_uint8(&reader);
        fragment->width = packet_reader_read_uint8(&reader);
        fragment->height = packet_reader_read_uint8(&reader);
        fragment->slot = packet_reader_read_uint8(&reader);
        size_t cells = (size_t)fragment->width * fragment->height;
        total += cells;
        valid = packet_reader_error(&reader) == PACKET_ERROR_NONE && cells != 0 &&
                fragment->x + fragment->width <= window_width &&
                fragment->y + fragment->height <= window_height && total <= total_max &&
                closure_runs_valid(data, len, reader.pos, cells);
        if (valid) {
            fragment->values = calloc(cells, sizeof(*fragment->values));
            valid = fragment->values != NULL;
            *out_of_memory = !valid;
        }
        for (size_t cell = 0; valid && cell < cells;) {
            uint8_t length = packet_reader_read_uint8(&reader);
            uint32_t value = packet_reader_read_uint32(&reader);
            valid = packet_reader_error(&reader) == PACKET_ERROR_NONE && length != 0 &&
                    length <= cells - cell && celestial_closure_value_valid(value);
            for (size_t i = 0; valid && i < length; i++) {
                fragment->values[cell + i] = value;
            }
            cell += length;
        }
    }
    valid = valid && packet_reader_finish(&reader);
    if (!valid) {
        for (size_t i = 0; i < parsed; i++) {
            free(fragments[i].values);
        }
        free(fragments);
        return false;
    }
    return closure_build(closure, generation, window_width, window_height, fragments, count,
                         out_of_memory);
}

bool celestial_closure_read(const uint8_t *data, size_t len, int window_width, int window_height,
                            celestial_closure_t *closure) {
    bool out_of_memory;
    return celestial_closure_decode(data, len, window_width, window_height, closure,
                                    &out_of_memory);
}

uint32_t celestial_closure_value(const celestial_closure_t *closure, int depth, int x, int y) {
    if (closure->index == NULL || depth < -CELESTIAL_CLOSURE_DEPTH_MAX ||
        depth > CELESTIAL_CLOSURE_DEPTH_MAX || x < 0 || y < 0 || x >= closure->window_width ||
        y >= closure->window_height) {
        return 0;
    }
    uint16_t index = closure->index[(size_t)(depth + CELESTIAL_CLOSURE_DEPTH_MAX) *
                                        index_cells(closure) +
                                    (size_t)x + (size_t)closure->window_width * y];
    if (index == 0) {
        return 0;
    }
    const celestial_closure_fragment_t *fragment = &closure->fragments[index - 1];
    return fragment->values[(size_t)(x - fragment->x) +
                            (size_t)fragment->width * (y - fragment->y)];
}

/* The lattice: a cell's map is its fragment, or for cells outside the
 * closure the open air of its depth, in window coordinates. Rays rise above
 * the closure's top depth, so open air extends past it. */
#define OUTSIDE_DEPTH_MIN (-CELESTIAL_CLOSURE_DEPTH_MAX)
#define OUTSIDE_DEPTHS (CELESTIAL_CLOSURE_DEPTHS + CELESTIAL_SOLVER_RAY_EVENTS + 1)

/** Every map handle the solver sees is one of these, so a handle is told
 * apart by its tag, never by where it points. */
typedef struct lattice_handle {
    /** The fragment of closure cells, or NULL for the open air of a depth. */
    const celestial_closure_fragment_t *fragment;
    int depth;
} lattice_handle;

typedef struct closure_lattice {
    /** The closure's fields as built: its fragments and index are heap
     * storage that survives the closure being moved, so solves only read
     * this context and may share it. */
    celestial_closure_t closure;
    /** One handle per closure fragment, in fragment order. */
    lattice_handle *fragments;
    lattice_handle outside[OUTSIDE_DEPTHS];
} closure_lattice;

static const celestial_closure_fragment_t *lattice_fragment(const closure_lattice *lattice,
                                                            const void *map) {
    (void)lattice;
    return map != NULL ? ((const lattice_handle *)map)->fragment : NULL;
}

/** A cell's closure value; zero for open air, for an outside record and for
 * unresolved cells. */
static uint32_t lattice_value(const closure_lattice *lattice, const celestial_solver_cell_t *cell) {
    const celestial_closure_fragment_t *fragment = lattice_fragment(lattice, cell->map);
    if (fragment == NULL) {
        return 0;
    }
    uint32_t value = fragment->values[(size_t)cell->x + (size_t)fragment->width * cell->y];
    return value == CELESTIAL_CLOSURE_OUTSIDE_RECORD ? 0 : value;
}

static bool lattice_resolve(void *context, void *origin, int x, int y, int z,
                            celestial_solver_cell_t *cell) {
    closure_lattice *lattice = context;
    if (origin == NULL) {
        return false;
    }
    const lattice_handle *handle = origin;
    int depth = handle->depth + z;
    if (handle->fragment != NULL) {
        x += handle->fragment->x;
        y += handle->fragment->y;
    }
    if (depth < OUTSIDE_DEPTH_MIN || depth >= OUTSIDE_DEPTH_MIN + OUTSIDE_DEPTHS) {
        return false;
    }
    const celestial_closure_t *closure = &lattice->closure;
    if (depth <= CELESTIAL_CLOSURE_DEPTH_MAX && x >= 0 && y >= 0 && x < closure->window_width &&
        y < closure->window_height) {
        uint16_t index = closure->index[(size_t)(depth + CELESTIAL_CLOSURE_DEPTH_MAX) *
                                            index_cells(closure) +
                                        (size_t)x + (size_t)closure->window_width * y];
        if (index != 0) {
            const celestial_closure_fragment_t *found = &closure->fragments[index - 1];
            int local_x = x - found->x, local_y = y - found->y;
            if (found->values[(size_t)local_x + (size_t)found->width * local_y] != 0) {
                *cell = (celestial_solver_cell_t){
                    .map = &lattice->fragments[index - 1], .x = local_x, .y = local_y, .z = z};
                return true;
            }
        }
    }
    *cell = (celestial_solver_cell_t){
        .map = &lattice->outside[depth - OUTSIDE_DEPTH_MIN], .x = x, .y = y, .z = z};
    return true;
}

static void lattice_dimensions(void *context, void *map, int *width, int *height) {
    closure_lattice *lattice = context;
    const celestial_closure_fragment_t *fragment = lattice_fragment(lattice, map);
    if (fragment != NULL) {
        *width = fragment->width;
        *height = fragment->height;
    } else {
        *width = lattice->closure.window_width;
        *height = lattice->closure.window_height;
    }
}

static bool lattice_sky(void *context, const celestial_solver_face_t *face) {
    closure_lattice *lattice = context;
    uint32_t from = lattice_value(lattice, &face->from);
    if (from != 0) {
        return (from & CELESTIAL_CLOSURE_SKY_EXIT) != 0;
    }
    /* Open air above open air is the sky. */
    return face->from.map != NULL && lattice_value(lattice, &face->to) == 0;
}

/** Continuity bit of a face leaving a cell along a unit normal. */
static uint32_t continuity_bit(const int8_t normal[3]) {
    return normal[0] > 0   ? CELESTIAL_CLOSURE_CONTINUOUS_EAST
           : normal[0] < 0 ? CELESTIAL_CLOSURE_CONTINUOUS_WEST
           : normal[1] > 0 ? CELESTIAL_CLOSURE_CONTINUOUS_SOUTH
           : normal[1] < 0 ? CELESTIAL_CLOSURE_CONTINUOUS_NORTH
           : normal[2] > 0 ? CELESTIAL_CLOSURE_CONTINUOUS_UP
                           : CELESTIAL_CLOSURE_CONTINUOUS_DOWN;
}

static bool lattice_continuous(void *context, const celestial_solver_face_t *face) {
    closure_lattice *lattice = context;
    if (face->from.map == NULL || face->to.map == NULL) {
        return face->from.map == face->to.map;
    }
    /* A face is one seam whichever way light crosses it: every closure cell
     * on it must call it continuous, so malformed, one-sided bits can never
     * make it passable in one direction only. */
    int8_t reverse[3] = {(int8_t)-face->normal[0], (int8_t)-face->normal[1],
                         (int8_t)-face->normal[2]};
    uint32_t from = lattice_value(lattice, &face->from);
    uint32_t to = lattice_value(lattice, &face->to);
    /* A seam between two profiles is never continuous, as on the server,
     * where continuity requires equal profiles, which share a slot. */
    if (from != 0 && to != 0 &&
        lattice_fragment(lattice, face->from.map)->slot !=
            lattice_fragment(lattice, face->to.map)->slot) {
        return false;
    }
    return (from == 0 || (from & continuity_bit(face->normal)) != 0) &&
           (to == 0 || (to & continuity_bit(reverse)) != 0);
}

/** Coefficient of a cell's face (0 down .. 4 west), open air transmitting
 * fully. */
static uint16_t cell_face(const closure_lattice *lattice, const celestial_solver_cell_t *cell,
                          int face, bool *aperture) {
    if (cell->map == NULL) {
        return 0;
    }
    uint32_t value = lattice_value(lattice, cell);
    if (value == 0) {
        return CELESTIAL_MODEL_SCALE;
    }
    if (aperture != NULL && (value & CELESTIAL_CLOSURE_APERTURE(face)) != 0) {
        *aperture = true;
    }
    return class_coefficient[(value >> CELESTIAL_CLOSURE_CLASS_SHIFT(face)) & 3U];
}

static uint16_t lattice_light(void *context, const celestial_solver_face_t *face,
                              bool *aperture) {
    closure_lattice *lattice = context;
    if (aperture != NULL) {
        *aperture = false;
    }
    if (face->normal[2] != 0) {
        const celestial_solver_cell_t *upper = face->normal[2] > 0 ? &face->to : &face->from;
        const celestial_solver_cell_t *lower = face->normal[2] > 0 ? &face->from : &face->to;
        /* The floor above an authorized cell, when that cell is outside the
         * closure, is the one the authorized cell recorded. */
        uint16_t physical;
        uint32_t below = lower->map != NULL ? lattice_value(lattice, lower) : 0;
        if (upper->map != NULL && lattice_value(lattice, upper) == 0 && below != 0) {
            physical = class_coefficient[(below >> CELESTIAL_CLOSURE_OVERHEAD_SHIFT) & 3U];
            if (aperture != NULL && (below & CELESTIAL_CLOSURE_OVERHEAD_APERTURE) != 0) {
                *aperture = true;
            }
        } else {
            physical = cell_face(lattice, upper, 0, aperture);
        }
        uint16_t cover = 0;
        if (lower->map != NULL) {
            uint32_t value = lattice_value(lattice, lower);
            cover = value == 0 || (value & CELESTIAL_CLOSURE_COVER_OPEN) != 0
                        ? CELESTIAL_MODEL_SCALE
                        : 0;
        }
        return MIN(physical, cover);
    }
    int index = face->normal[0] < 0   ? 4
                : face->normal[0] > 0 ? 2
                : face->normal[1] < 0 ? 1
                : face->normal[1] > 0 ? 3
                                      : -1;
    if (index < 0) {
        return CELESTIAL_MODEL_SCALE;
    }
    return cell_face(lattice, &face->from, index, aperture);
}

static const celestial_solver_ops_t closure_ops = {
    .resolve = lattice_resolve,
    .dimensions = lattice_dimensions,
    .sky = lattice_sky,
    .continuous = lattice_continuous,
    .light = lattice_light,
};

/** The lattice context of a built closure: its handles, one per fragment,
 * and the open air around it, in one allocation. */
static void *closure_lattice_create(const celestial_closure_t *closure) {
    closure_lattice *context =
        malloc(sizeof(*context) + closure->count * sizeof(*context->fragments));
    if (context == NULL) {
        return NULL;
    }
    context->closure = *closure;
    context->closure.lattice = NULL;
    context->fragments = (lattice_handle *)(context + 1);
    for (size_t i = 0; i < closure->count; i++) {
        context->fragments[i] = (lattice_handle){.fragment = &closure->fragments[i],
                                                 .depth = closure->fragments[i].depth};
    }
    for (int i = 0; i < OUTSIDE_DEPTHS; i++) {
        context->outside[i] = (lattice_handle){.depth = OUTSIDE_DEPTH_MIN + i};
    }
    return context;
}

bool celestial_closure_solve(const celestial_closure_t *closure, size_t fragment,
                             const celestial_model_t *model, int32_t *values, int32_t (*rgb)[3]) {
    if (closure == NULL || fragment >= closure->count || model == NULL || values == NULL ||
        rgb == NULL) {
        return false;
    }
    closure_lattice *context = closure->lattice;
    if (context == NULL) {
        return false;
    }
    celestial_solver_lattice_t lattice = {.ops = &closure_ops, .context = context};
    const celestial_closure_fragment_t *solved = &closure->fragments[fragment];
    bool complete =
        celestial_solver_solve(&lattice, &context->fragments[fragment], model, values, rgb);
    if (complete) {
        size_t cells = (size_t)solved->width * solved->height;
        for (size_t cell = 0; cell < cells; cell++) {
            if (solved->values[cell] == 0) {
                values[cell] = 0;
                memset(rgb[cell], 0, sizeof(rgb[cell]));
            }
        }
    }
    return complete;
}
