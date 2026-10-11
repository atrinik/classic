/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SPATIAL_H
#define SPATIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <decls.h>

#define SPATIAL_STACK_LEVELS 13
#define SPATIAL_MAX_EVENTS 12288
#define SPATIAL_LIGHT_SCALE 256

typedef struct spatial_cell {
    mapstruct *map;
    int x, y, z; /* x/y map-local; z relative to the operation's origin. */
} spatial_cell;

/* A resolver may reuse coordinates validated by spatial_resolve within the
 * same synchronous, read-only server-thread operation. Its borrowed context
 * must outlive the call; no map/object mutation, plugin call or yield may
 * separate validation from use. Failure need not initialize the output cell. */
typedef bool (*spatial_resolver_fn)(mapstruct *origin, int x, int y, int z,
                                    spatial_cell *cell, void *context);

typedef struct spatial_face {
    spatial_cell from, to;
    int8_t normal[3]; /* Unit normal from 'from' to 'to'. */
} spatial_face;

typedef struct spatial_event {
    spatial_cell cells[7]; /* Every touched nonempty tied-axis subset. */
    size_t cell_count;
    spatial_face faces[12]; /* Every face at an edge/corner, including grazing. */
    size_t face_count;
    spatial_cell destination;
    unsigned axes; /* bit 0=x, 1=y, 2=z */
    bool valid; /* All touched cells resolved; missing cells have map=NULL. */
    bool terminal;
} spatial_event;

typedef struct spatial_cursor {
    mapstruct *origin;
    int position[3], direction[3], endpoint[3];
    uint32_t crossed[3], events, limit;
    bool segment, failed;
} spatial_cursor;

/* Optional server-thread observation scope. Every consulted non-NULL map,
 * including an unavailable dependency, is reported before dereferencing it.
 * The callback must be read-only and must not call spatial APIs. Restore the
 * returned previous observer before returning or yielding a resumable job. */
typedef struct spatial_observer {
    void (*visit)(const mapstruct *map, void *context);
    void *context;
} spatial_observer;
spatial_observer spatial_observer_exchange(spatial_observer observer);

typedef enum spatial_policy { SPATIAL_SIGHT, SPATIAL_COMBAT } spatial_policy;

/* Server-thread only: borrowed map/object pointers; no loads or mutations;
 * bounded temporary lookup storage. Cursors may only survive while the caller
 * retains map lifetime.
 * Invalid, ambiguous, over-bound or unavailable topology fails closed. */
bool spatial_resolve(mapstruct *origin, int x, int y, int z, spatial_cell *cell);
bool spatial_offset(mapstruct *origin, mapstruct *target, int *dx, int *dy, int *dz);
bool spatial_ray_init(spatial_cursor *cursor, mapstruct *origin, int x, int y,
                      int dx, int dy, int dz);
bool spatial_direction_init(spatial_cursor *cursor, mapstruct *origin, int x, int y,
                            int dx, int dy, int dz, unsigned max_events);
/* 1=one boundary event (even unresolved), 0=finished, -1=invalid/bound exceeded.
 * Direction components are integer ratios; Q15 vectors need no conversion. */
int spatial_ray_next(spatial_cursor *cursor, spatial_event *event);
/* Same exact traversal, with a caller-owned operation-local resolver. NULL
 * selects spatial_resolve. Every touched tie cell passes through the resolver. */
int spatial_ray_next_resolved(spatial_cursor *cursor, spatial_event *event,
                              spatial_resolver_fn resolver, void *context);
bool spatial_event_clear(const spatial_event *event, spatial_policy policy,
                         bool allow_terminal);
unsigned spatial_event_blocked_axes(const spatial_event *event, spatial_policy policy);
bool spatial_ray_clear(mapstruct *origin, int x, int y, int dx, int dy, int dz,
                       spatial_policy policy, bool allow_terminal);
/* Light surfaces own outgoing cardinal faces and the DOWN face of upper cells.
 * Return 0..256; all faces at one tie combine by minimum. A target's receiving
 * face stays illuminated because its outgoing face has not yet been crossed. */
uint16_t spatial_cell_light_face(const spatial_cell *cell, int nx, int ny, int nz,
                                 bool *aperture);
uint16_t spatial_face_light(const spatial_face *face, bool *aperture);
uint16_t spatial_event_light(const spatial_event *event);
int32_t spatial_light_attenuate(int32_t value, uint16_t coefficient);
#endif
