/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
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
#include <initialization.h>
#include <server_main.h>
#include <light.h>
#include <object.h>
#include <spatial.h>
#include <celestial_structure.h>
#include <toolkit/map_protocol.h>
#include <region.h>
#include <toolkit/light_profile.h>
#include <los.h>

#define NR_LIGHT_MASK LIGHT_PROFILE_COUNT
#define MAX_LIGHT_SOURCE LIGHT_PROFILE_SOURCE_MAX
#define MAX_LIGHT_RADIUS LIGHT_PROFILE_RADIUS_MAX

/** Advances with every source change anywhere; a coarse cache entry confirmed
 * at the current value needs no revalidation. */
static uint64_t light_local_generation = 1;

/** Advances whenever a map's spaces are freed (light_local_forget()), so no
 * cached dependency list is read after one of its maps may be gone. */
static uint64_t light_local_frees = 1;

#ifdef ATRINIK_TESTING
/** Topology walks performed, for tests of the cache. */
static uint64_t light_local_walks;

uint64_t light_local_walks_for_test(void) {
    return light_local_walks;
}
#endif

/** Full queues publish early; this bounds scope bookkeeping, not reach. */
#define LIGHT_PENDING_MAX ((1 + 2 * MAP2_MAX_DEPTH) * (1 + TILED_NUM_DIR))

typedef struct light_map_set {
    mapstruct **maps;
    size_t count;
    size_t capacity;
} light_map_set;

static unsigned int light_batch_depth;
static struct {
    mapstruct *map;
    bool local;
    bool celestial;
} light_pending[LIGHT_PENDING_MAX];
static size_t light_pending_count;
static unsigned int light_unlink_depth;
static unsigned int light_retirement_depth;
static bool light_unlink_marks_pending;

void light_source_prepare_change(void) {
    light_map_unlink_flush();
    light_batch_flush();
}

void light_batch_begin(void) {
    HARD_ASSERT(light_batch_depth < UINT_MAX);
    light_batch_depth++;
}

void light_batch_flush(void) {
    if (light_unlink_depth == 0) {
        light_map_unlink_flush();
    }
    /* Remove each entry before publishing so rebuild/read barriers cannot
     * recursively flush that entry. Scopes never retain maps across lifecycle
     * operations or callbacks that can release them. */
    while (light_pending_count != 0) {
        size_t index = --light_pending_count;
        mapstruct *map = light_pending[index].map;
        bool local = light_pending[index].local;
        bool celestial = light_pending[index].celestial;
        if (local) {
            recalculate_light_sources(map);
        }
        if (celestial) {
            celestial_light_invalidate(map);
        }
    }
}

void light_batch_end(void) {
    HARD_ASSERT(light_batch_depth != 0);
    if (--light_batch_depth == 0) {
        light_batch_flush();
    }
}

void light_geometry_changed(mapstruct *map, bool local, bool celestial) {
    if (!local && !celestial) {
        return;
    }
    for (size_t i = 0; i < light_pending_count; i++) {
        if (light_pending[i].map == map) {
            light_pending[i].local |= local;
            light_pending[i].celestial |= celestial;
            return;
        }
    }
    if (light_pending_count == arraysize(light_pending)) {
        light_batch_flush();
    }
    light_pending[light_pending_count].map = map;
    light_pending[light_pending_count].local = local;
    light_pending[light_pending_count].celestial = celestial;
    light_pending_count++;
    if (light_batch_depth == 0) {
        light_batch_flush();
    }
}

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

/** Return round(numerator / denominator) without overflowing the numerator. */
static uint64_t light_div_round(uint64_t numerator, uint64_t denominator) {
    HARD_ASSERT(denominator != 0);
    uint64_t quotient = numerator / denominator;
    uint64_t remainder = numerator % denominator;
    return quotient + (remainder >= (denominator + 1) / 2 ? 1 : 0);
}

static uint16_t light_color_linear_component(uint32_t color, size_t channel) {
    return light_profile_color_component(color, channel);
}

void light_color_linearize(uint32_t color, uint16_t linear[3]) {
    HARD_ASSERT(linear != NULL);

    for (size_t channel = 0; channel < 3; channel++) {
        linear[channel] = light_color_linear_component(color, channel);
    }
}

static uint16_t light_raw_to_radiance(int64_t raw_light) {
    return light_profile_raw_to_radiance(raw_light);
}

void light_radiance_from_raw(const MapSpace *space,
                             int raw_light,
                             uint16_t *scalar_radiance,
                             uint16_t radiance[3]) {
    HARD_ASSERT(space != NULL);
    HARD_ASSERT(scalar_radiance != NULL);
    HARD_ASSERT(radiance != NULL);

    *scalar_radiance = light_raw_to_radiance(raw_light);
    int64_t effective_source_raw = 0;
    int64_t accumulated_source_raw = 0;
    if (space->light_source_color_weight > 0) {
        accumulated_source_raw = space->light_source_color_weight / UINT16_MAX;
        effective_source_raw =
            MIN(MAX(space->light_source_positive_value, 0), accumulated_source_raw);
    }
    int64_t channels[3];
    int64_t celestial_raw = MAX(space->celestial_light_value, 0);
    for (size_t channel = 0; channel < arraysize(channels); channel++) {
        uint64_t colored_raw = 0;
        if (effective_source_raw > 0) {
            uint64_t color_sum = (uint64_t)MAX(space->light_source_color[channel], 0);
            if (effective_source_raw < accumulated_source_raw) {
                colored_raw = light_muldiv_round(color_sum,
                                                 (uint64_t)effective_source_raw,
                                                 (uint64_t)space->light_source_color_weight);
            } else {
                colored_raw = light_div_round(color_sum, UINT16_MAX);
            }
        }
        int64_t local_raw = (int64_t)raw_light - celestial_raw;
        channels[channel] = MAX(local_raw - effective_source_raw + (int64_t)colored_raw +
                                    MAX(space->celestial_light_rgb[channel], 0),
                                0);
    }
    int64_t maximum = MAX(channels[0], MAX(channels[1], channels[2]));
    if (maximum > INT64_C(40959)) {
        for (size_t channel = 0; channel < arraysize(channels); channel++) {
            uint64_t scaled = light_muldiv_round((uint64_t)channels[channel],
                                                 UINT64_C(1) + UINT16_MAX,
                                                 (uint64_t)maximum);
            radiance[channel] = (uint16_t)MIN(scaled, UINT16_MAX);
        }
        return;
    }
    for (size_t channel = 0; channel < arraysize(channels); channel++) {
        radiance[channel] = light_raw_to_radiance(channels[channel]);
    }
}

/** Attenuation step of a coefficient, rounded up so the client never
 * transmits more light than the server. */
static uint64_t light_occluder_attenuation(uint16_t coefficient) {
    int attenuation = (SPATIAL_LIGHT_SCALE - coefficient + MAP2_OCCLUDER_ATTENUATION_STEP - 1) /
                      MAP2_OCCLUDER_ATTENUATION_STEP;
    return (uint64_t)MIN(attenuation, 15);
}

/** Authored transmission of an object face, by door and gate state. */
static uint16_t light_occluder_transmission(const object *op) {
    const char *value;
    if (op->type == DOOR || op->type == GATE) {
        bool shut = op->type == DOOR ? QUERY_FLAG(op, FLAG_DOOR_CLOSED)
                                     : QUERY_FLAG(op, FLAG_NO_PASS);
        value = object_get_value(op, shut ? "celestial_transmission_closed"
                                          : "celestial_transmission_open");
    } else {
        value = object_get_value(op, "celestial_transmission");
    }
    celestial_transmission_t transmission = celestial_structure_transmission(value);
    return transmission == CELESTIAL_TRANSMISSION_INVALID ? 0 : (uint16_t)transmission;
}

/**
 * MAP2 occluder bits contributed by one object that is being sent to the
 * viewer. Only the object's own flags and authored face transmission count,
 * never space-wide flags that also aggregate hidden or unsent objects.
 */
uint64_t light_occluder_object_bits(const object *op) {
    HARD_ASSERT(op != NULL);
    const object *head = HEAD(op);
    uint64_t bits = 0;
    if (QUERY_FLAG(op, FLAG_BLOCKSVIEW)) {
        bits |= MAP2_OCCLUDER_BLOCKSVIEW;
    }
    if (QUERY_FLAG(op, FLAG_IS_FLOOR)) {
        bits |= MAP2_OCCLUDER_FLOOR;
    }
    if (object_is_roof_surface(op)) {
        bits |= MAP2_OCCLUDER_ROOF;
    }
    bool closed = head->type == DOOR && QUERY_FLAG(head, FLAG_DOOR_CLOSED);
    if (closed) {
        bits |= MAP2_OCCLUDER_DOOR_CLOSED;
    }
    uint8_t faces = celestial_structure_faces(op);
    /* Hidden wall-layer surfaces (roofs, camera surfaces) own a downward face
     * as spatial_cell_light_face() gives them, even without sky_boundary. */
    if (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN)) {
        faces |= CELESTIAL_FACE_DOWN;
    }
    /* Each cardinal face keeps its own coefficient, as spatial_cell_light_face()
     * does: an authored face transmits its authored value and is marked
     * authored. Whether an unauthored face is opaque depends on every sent
     * object of the cell, so a view blocker only records the fallback, which
     * light_occluder_finish() applies to faces no sent object authors. */
    static const uint8_t cardinal[4] = {CELESTIAL_FACE_NORTH, CELESTIAL_FACE_EAST,
                                        CELESTIAL_FACE_SOUTH, CELESTIAL_FACE_WEST};
    for (size_t face = 0; face < arraysize(cardinal); face++) {
        if ((faces & cardinal[face]) != 0) {
            bits |= light_occluder_attenuation(light_occluder_transmission(op))
                    << (LIGHT_OCCLUDER_HORIZONTAL_SHIFT +
                        MAP2_OCCLUDER_FACE_ATTENUATION_BITS * face);
            bits |= UINT64_C(1) << (LIGHT_OCCLUDER_AUTHORED_SHIFT + face);
        }
    }
    if (bits & MAP2_OCCLUDER_BLOCKSVIEW) {
        bits |= LIGHT_OCCLUDER_BLOCKER_FALLBACK;
    }
    /* The downward face is the slab between this cell and the one below, so a
     * glass floor attenuates only vertical crossings. */
    if ((faces & CELESTIAL_FACE_DOWN) != 0) {
        bits |= light_occluder_attenuation(light_occluder_transmission(op))
                << LIGHT_OCCLUDER_VERTICAL_SHIFT;
    }
    return bits;
}

/**
 * Whether a sent object's emitter opts in to client-side flicker, through the
 * boolean light_flicker attribute (usually an archetype default).
 */
bool light_emitter_flickers(const object *op) {
    const char *value = object_get_value(op, "light_flicker");
    return value != NULL && (strcmp(value, "1") == 0 || KEYWORD_IS_TRUE(value));
}

/**
 * Whether MAP2 may disclose an object's emitter to a viewer. Another player's
 * glow is derived from its private inventory, so only the viewer's own is.
 */
bool light_emitter_disclosed(const object *viewer, const object *op) {
    return op->type != PLAYER || op == viewer;
}

/** Combine the occluder bits of two sent objects of one cell: geometry and
 * faces are joined, and every attenuation nibble keeps the stronger one. */
uint64_t light_occluder_finish(uint64_t bits) {
    uint64_t finished = bits & ~LIGHT_OCCLUDER_INTERNAL;
    if (bits & LIGHT_OCCLUDER_BLOCKER_FALLBACK) {
        for (int face = 0; face < 4; face++) {
            if (!(bits & (UINT64_C(1) << (LIGHT_OCCLUDER_AUTHORED_SHIFT + face)))) {
                finished |= UINT64_C(15) << (LIGHT_OCCLUDER_HORIZONTAL_SHIFT +
                                             MAP2_OCCLUDER_FACE_ATTENUATION_BITS * face);
            }
        }
    }
    return finished;
}

uint64_t light_occluder_combine(uint64_t first, uint64_t second) {
    uint64_t combined = ((first | second) & UINT64_C(0xffff)) |
                        ((first | second) & LIGHT_OCCLUDER_INTERNAL);
    uint64_t vertical = MAX((first >> LIGHT_OCCLUDER_VERTICAL_SHIFT) & 15U,
                            (second >> LIGHT_OCCLUDER_VERTICAL_SHIFT) & 15U);
    combined |= vertical << LIGHT_OCCLUDER_VERTICAL_SHIFT;
    for (int face = 0; face < 4; face++) {
        int shift = LIGHT_OCCLUDER_HORIZONTAL_SHIFT + MAP2_OCCLUDER_FACE_ATTENUATION_BITS * face;
        combined |= MAX((first >> shift) & 15U, (second >> shift) & 15U) << shift;
    }
    return combined;
}

static int get_real_light_source_value(int l) {
    return light_profile_mask(l);
}

static bool light_map_set_contains(const light_map_set *set, const mapstruct *map) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->maps[i] == map) {
            return true;
        }
    }

    return false;
}

static void light_map_set_append(light_map_set *set, mapstruct *map) {
    if (set->count == set->capacity) {
        set->capacity = set->capacity == 0 ? 16 : set->capacity * 2;
        set->maps = xreallocarray(set->maps, set->capacity, sizeof(*set->maps));
    }
    set->maps[set->count++] = map;
}

static void light_map_set_add(light_map_set *set, mapstruct *map) {
    if (map != NULL && map->in_memory == MAP_IN_MEMORY && !light_map_set_contains(set, map)) {
        light_map_set_append(set, map);
    }
}

static mapstruct *light_loaded_tile(mapstruct *map, int tile) {
    mapstruct *tiled = map->tile_map[tile];

    if (tiled == NULL || tiled->in_memory != MAP_IN_MEMORY) {
        return NULL;
    }

    return tiled;
}

typedef struct light_graph_node light_graph_node;
typedef struct light_graph_edge {
    light_graph_node *source;
    int direction;
    struct light_graph_edge *next;
} light_graph_edge;

struct light_graph_node {
    mapstruct *map;
    light_graph_edge *incoming;
    bool visited;
    UT_hash_handle hh;
};

typedef struct light_graph {
    light_graph_node *nodes;
    light_graph_edge *edges;
    light_graph_node *index;
} light_graph;

static void light_graph_free(light_graph *graph) {
    HASH_CLEAR(hh, graph->index);
    free(graph->edges);
    free(graph->nodes);
}

/** Collect the weak resident component of the seeds with a fresh reverse-edge
 * index. Every resolved ray sample follows loaded tile pointers, so neither a
 * source nor a target outside this component can interact with its geometry.
 * Include incoming and path-less pointer edges: asymmetric links and stale
 * reverse path metadata must overestimate reach, never hide incoming light.
 * The temporary graph costs O(resident maps + tile edges); no cached topology
 * or map pointer survives this operation. */
static void light_component_collect(light_map_set *component, const light_map_set *seeds,
                                    light_graph *graph) {
    memset(component, 0, sizeof(*component));
    memset(graph, 0, sizeof(*graph));
    if (seeds->count == 0) {
        return;
    }
    size_t count = 0;
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        if (map->in_memory == MAP_IN_MEMORY) {
            count++;
        }
    }
    if (count == 0) {
        return;
    }
    light_graph_node *nodes = xcalloc(count, sizeof(*nodes));
    light_graph_edge *edges = xmallocarray(count, TILED_NUM * sizeof(*edges));
    light_graph_node **queue = xmallocarray(count, sizeof(*queue));
    light_graph_node *index = NULL;
    size_t node_count = 0;
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        if (map->in_memory == MAP_IN_MEMORY) {
            light_graph_node *node = &nodes[node_count++];
            node->map = map;
            HASH_ADD_PTR(index, map, node);
        }
    }
    size_t edge_count = 0;
    for (size_t i = 0; i < count; i++) {
        for (int direction = 0; direction < TILED_NUM; direction++) {
            mapstruct *neighbor = light_loaded_tile(nodes[i].map, direction);
            light_graph_node *target;
            HASH_FIND_PTR(index, &neighbor, target);
            if (target != NULL) {
                light_graph_edge *edge = &edges[edge_count++];
                edge->source = &nodes[i];
                edge->direction = direction;
                edge->next = target->incoming;
                target->incoming = edge;
            }
        }
    }
    size_t queued = 0;
    for (size_t i = 0; i < seeds->count; i++) {
        light_graph_node *node;
        HASH_FIND_PTR(index, &seeds->maps[i], node);
        if (node != NULL && !node->visited) {
            node->visited = true;
            queue[queued++] = node;
        }
    }
    for (size_t cursor = 0; cursor < queued; cursor++) {
        light_graph_node *node = queue[cursor];
        light_map_set_append(component, node->map);
        for (int direction = 0; direction < TILED_NUM; direction++) {
            mapstruct *neighbor = light_loaded_tile(node->map, direction);
            light_graph_node *target;
            HASH_FIND_PTR(index, &neighbor, target);
            if (target != NULL && !target->visited) {
                target->visited = true;
                queue[queued++] = target;
            }
        }
        for (light_graph_edge *edge = node->incoming; edge != NULL; edge = edge->next) {
            if (!edge->source->visited) {
                edge->source->visited = true;
                queue[queued++] = edge->source;
            }
        }
    }
    free(queue);
    graph->nodes = nodes;
    graph->edges = edges;
    graph->index = index;
}

#define LIGHT_RESOLVE_SPAN (2 * MAX_LIGHT_RADIUS + 1)

/** Resolutions of one source's mask box, shared by its target lookups and all
 * of its ray steps. spatial_resolve() depends only on map structure, so an
 * entry stays valid while the LOS geometry epoch is unchanged. Disabled while
 * a dependency observer must see every visit.
 *
 * A rebuild also shares one cache between the scalar, positive and color masks
 * of an origin. Their rays are identical, so each offset remembers its last
 * path value. Object geometry cannot change within one synchronous rebuild;
 * callers outside a rebuild use a private cache per mask. */
typedef struct light_resolve_cache {
    mapstruct *origin;
    int x, y;
    uint64_t epoch;
    bool enabled;
    uint8_t state[LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN];
    spatial_cell cells[LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN];
    bool path_known[LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN];
    int path_in[LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN];
    int path_out[LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN][LIGHT_RESOLVE_SPAN];
} light_resolve_cache;

enum { LIGHT_RESOLVE_UNKNOWN, LIGHT_RESOLVE_FOUND, LIGHT_RESOLVE_MISSING };

static void light_resolve_cache_reset(light_resolve_cache *cache) {
    memset(cache->state, LIGHT_RESOLVE_UNKNOWN, sizeof(cache->state));
    memset(cache->path_known, 0, sizeof(cache->path_known));
    cache->epoch = los_geometry_epoch();
}

static void light_resolve_cache_init(light_resolve_cache *cache, mapstruct *origin, int x, int y) {
    cache->origin = origin;
    cache->x = x;
    cache->y = y;
    cache->enabled = !spatial_observer_active();
    light_resolve_cache_reset(cache);
}

static bool light_resolve_cached(light_resolve_cache *cache, int x, int y, int z,
                                 spatial_cell *cell) {
    int ix = x - cache->x + MAX_LIGHT_RADIUS, iy = y - cache->y + MAX_LIGHT_RADIUS;
    int iz = z + MAX_LIGHT_RADIUS;
    if (!cache->enabled || ix < 0 || iy < 0 || iz < 0 || ix >= LIGHT_RESOLVE_SPAN ||
        iy >= LIGHT_RESOLVE_SPAN || iz >= LIGHT_RESOLVE_SPAN) {
        return spatial_resolve(cache->origin, x, y, z, cell);
    }
    if (cache->epoch != los_geometry_epoch()) {
        light_resolve_cache_reset(cache);
    }
    uint8_t *state = &cache->state[iz][iy][ix];
    if (*state == LIGHT_RESOLVE_UNKNOWN) {
        spatial_cell resolved;
        bool found = spatial_resolve(cache->origin, x, y, z, &resolved);
        if (cache->epoch != los_geometry_epoch()) {
            /* Resolution loaded a map. Keep nothing from before or during it. */
            light_resolve_cache_reset(cache);
            if (found) {
                *cell = resolved;
            }
            return found;
        }
        /* A failed resolution need not initialize its cell; keep only the state. */
        if (!found) {
            *state = LIGHT_RESOLVE_MISSING;
            return false;
        }
        cache->cells[iz][iy][ix] = resolved;
        *state = LIGHT_RESOLVE_FOUND;
    }
    if (*state == LIGHT_RESOLVE_MISSING) {
        return false;
    }
    *cell = cache->cells[iz][iy][ix];
    return true;
}

static bool light_ray_resolve(mapstruct *origin, int x, int y, int z, spatial_cell *cell,
                              void *context) {
    light_resolve_cache *cache = context;
    if (origin != cache->origin) {
        return spatial_resolve(origin, x, y, z, cell);
    }
    return light_resolve_cached(cache, x, y, z, cell);
}

static mapstruct *light_resolve_space(light_resolve_cache *cache, int x, int y, int z,
                                      int *rx, int *ry) {
    spatial_cell cell;
    if (!light_resolve_cached(cache, x, y, z, &cell)) return NULL;
    *rx = cell.x;
    *ry = cell.y;
    return cell.map;
}

/** Equal dimensions and support shorter than a map side bound every resolved
 * sample to V^k H or H V^k: at most one horizontal (possibly diagonal) edge
 * and at most three same-direction vertical edges. Diagonal composition and
 * commuting squares are not required. Retain full-component repair for
 * asymmetric pointers: map loading can overwrite an implicit reverse link,
 * whose old target is absent from the current directed reach. */
static bool light_topology_is_uniform(const light_map_set *component) {
    if (component->count == 0) {
        return true;
    }
    int width = MAP_WIDTH(component->maps[0]);
    int height = MAP_HEIGHT(component->maps[0]);
    if (width < 2 * MAX_LIGHT_RADIUS + 1 || height < 2 * MAX_LIGHT_RADIUS + 1) {
        return false;
    }
    for (size_t i = 0; i < component->count; i++) {
        mapstruct *map = component->maps[i];
        if (MAP_WIDTH(map) != width || MAP_HEIGHT(map) != height) {
            return false;
        }
        for (int direction = 0; direction < TILED_NUM; direction++) {
            mapstruct *neighbor = light_loaded_tile(map, direction);
            if (neighbor != NULL && neighbor->tile_map[map_tiled_reverse[direction]] != map) {
                return false;
            }
        }
    }
    return true;
}

/** Append one directed step, or its actual inverse. Incoming pointers may
 * branch and need not have reverse paths; never infer them from outgoing links. */
static void light_reach_step(light_map_set *result, const light_map_set *seeds,
                             const light_graph *graph, bool inverse, int first, int end) {
    for (size_t i = 0; i < seeds->count; i++) {
        mapstruct *map = seeds->maps[i];
        if (inverse) {
            light_graph_node *node;
            HASH_FIND_PTR(graph->index, &map, node);
            if (node == NULL) {
                continue;
            }
            for (light_graph_edge *edge = node->incoming; edge != NULL; edge = edge->next) {
                if (edge->direction >= first && edge->direction < end) {
                    light_map_set_add(result, edge->source->map);
                }
            }
        } else {
            for (int direction = first; direction < end; direction++) {
                light_map_set_add(result, light_loaded_tile(map, direction));
            }
        }
    }
}

static void light_reach_merge(light_map_set *result, const light_map_set *seeds) {
    for (size_t i = 0; i < seeds->count; i++) {
        light_map_set_add(result, seeds->maps[i]);
    }
}

static void light_reach_vertical(light_map_set *result, const light_map_set *seeds,
                                 const light_graph *graph, bool inverse) {
    light_reach_merge(result, seeds);
    int radius = MIN(MAX_LIGHT_RADIUS - 1, MAP2_MAX_DEPTH);
    for (int direction = TILED_UP; direction <= TILED_DOWN; direction++) {
        light_map_set frontier = {0};
        light_reach_merge(&frontier, seeds);
        for (int depth = 0; depth < radius && frontier.count != 0; depth++) {
            light_map_set next = {0};
            light_reach_step(&next, &frontier, graph, inverse, direction, direction + 1);
            light_reach_merge(result, &next);
            free(frontier.maps);
            frontier = next;
        }
        free(frontier.maps);
    }
}

/** Geometry-independent R reach (or inverse R), including both resolver
 * orders even when the first currently succeeds. Reversing both orders gives
 * the same language with inverted edges, including asymmetric incoming links.
 * Missing links and blocked rays must not prune discovery: loading/removing
 * geometry can enable paths that currently contribute nothing. */
static void light_reach_collect(light_map_set *result, const light_map_set *seeds,
                                const light_graph *graph, bool inverse) {
    memset(result, 0, sizeof(*result));
    light_map_set vertical = {0};
    light_reach_vertical(&vertical, seeds, graph, inverse);
    light_reach_merge(result, &vertical);
    light_reach_step(result, &vertical, graph, inverse, 0, TILED_NUM_DIR);
    free(vertical.maps);
    light_map_set horizontal = {0};
    light_reach_merge(&horizontal, seeds);
    light_reach_step(&horizontal, seeds, graph, inverse, 0, TILED_NUM_DIR);
    light_reach_vertical(result, &horizontal, graph, inverse);
    free(horizontal.maps);
}

/** Authored v1 seams share the celestial transport policy. Legacy maps
 * retain their physical linked-map lighting without requiring v1 metadata. */
static bool light_face_continuous(const spatial_face *face) {
    const mapstruct *from = face->from.map, *to = face->to.map;
    if (from == NULL || to == NULL) {
        return false;
    }
    if (from == to || (from->celestial_schema == 0 && to->celestial_schema == 0)) {
        return true;
    }
    if (from->celestial_schema != 1 || to->celestial_schema != 1) {
        return false;
    }
    int tile = face->normal[2] > 0 ? TILED_UP : face->normal[2] < 0 ? TILED_DOWN :
               face->normal[0] > 0 ? TILED_EAST : face->normal[0] < 0 ? TILED_WEST :
               face->normal[1] > 0 ? TILED_SOUTH : TILED_NORTH;
    int reverse = map_tiled_reverse[tile];
    return from->tile_map[tile] == to && to->tile_map[reverse] == from &&
           from->celestial_boundary[tile] == CELESTIAL_BOUNDARY_CONTINUOUS &&
           to->celestial_boundary[reverse] == CELESTIAL_BOUNDARY_CONTINUOUS &&
           memcmp(region_celestial_for_map(from)->digest,
                  region_celestial_for_map(to)->digest, 32) == 0;
}

/**
 * MAP2 occluder face bits: faces of a sent cell that local light cannot cross.
 * Only static seam structure between two cells both in the viewer's view is
 * encoded; a face towards any other cell is closed, so the bits never depend
 * on whether some map is resident.
 */
uint16_t light_occluder_faces(mapstruct *map, int x, int y, uint8_t in_view) {
    HARD_ASSERT(map != NULL);
    static const int8_t normals[6][3] = {
        {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
    };
    uint16_t bits = 0;
    for (size_t i = 0; i < arraysize(normals); i++) {
        uint16_t bit = (uint16_t)(1U << (MAP2_OCCLUDER_FACE_SHIFT + i));
        if ((in_view & (1U << i)) == 0) {
            bits |= bit;
            continue;
        }
        int rx = x + normals[i][0], ry = y + normals[i][1];
        mapstruct *target;
        if (normals[i][2] != 0) {
            /* Vertical crossings use the linked level, like the light path. */
            target = map->tile_map[normals[i][2] > 0 ? TILED_UP : TILED_DOWN];
            if (target != NULL && (target->in_memory != MAP_IN_MEMORY ||
                                   rx >= MAP_WIDTH(target) || ry >= MAP_HEIGHT(target))) {
                target = NULL;
            }
        } else {
            target = get_map_from_coord2(map, &rx, &ry);
        }
        spatial_face face = {
            .from = {.map = map, .x = x, .y = y, .z = 0},
            .to = {.map = target, .x = rx, .y = ry, .z = normals[i][2]},
            .normal = {normals[i][0], normals[i][1], normals[i][2]},
        };
        /* A neighbor in view is resident; a failure here is a broken seam. */
        if (target == NULL || target->spaces == NULL || !light_face_continuous(&face)) {
            bits |= bit;
            continue;
        }
        /* A sealed virtual sky cover closes the upward face of the lower cell. */
        const spatial_cell *lower = normals[i][2] > 0 ? &face.from
                                    : normals[i][2] < 0 ? &face.to : NULL;
        if (lower != NULL && spatial_cell_light_face(lower, 0, 0, 1, NULL) == 0) {
            bits |= bit;
        }
    }
    return bits;
}

/** A terminal roof/wall sample represents its incident surface, not the
 * inaccessible contents behind it. A plain floor still covers its contents. */
static bool light_terminal_surface(const spatial_cell *cell) {
    if ((GET_MAP_FLAGS(cell->map, cell->x, cell->y) & P_BLOCKSVIEW) != 0) return true;
    for (const object *op = GET_MAP_OB(cell->map, cell->x, cell->y);
         op != NULL; op = op->above) {
        if (object_is_roof_surface(op)) return true;
    }
    return false;
}

/** The radial mask stays authoritative; physical crossings attenuate it in
 * travel order before signed scalar and normalized color aggregation. */
static int light_path_trace(light_resolve_cache *cache, int dx, int dy, int dz, int value) {
    spatial_cursor ray;
    if (!spatial_ray_init(&ray, cache->origin, cache->x, cache->y, dx, dy, dz)) return 0;
    int sign = value < 0 ? -1 : 1;
    int32_t transmitted = abs(value);
    spatial_event event;
    int status;
    while ((status = cache->enabled ? spatial_ray_next_resolved(&ray, &event, light_ray_resolve, cache)
                                    : spatial_ray_next(&ray, &event)) == 1) {
        if (!event.valid) return 0;
        for (size_t i = 0; i < event.face_count; i++) {
            if (!light_face_continuous(&event.faces[i])) return 0;
        }
        uint16_t coefficient = SPATIAL_LIGHT_SCALE;
        bool incident = event.terminal && light_terminal_surface(&event.destination);
        for (size_t i = 0; i < event.face_count; i++) {
            const spatial_face *face = &event.faces[i];
            if (incident && face->normal[2] > 0 &&
                face->to.map == event.destination.map &&
                face->to.x == event.destination.x && face->to.y == event.destination.y &&
                face->to.z == event.destination.z) {
                /* Only the receiving slab is exempt. Grazing faces and
                 * virtual cover on the source side retain their coefficient. */
                coefficient = MIN(coefficient,
                    spatial_cell_light_face(&face->from, 0, 0, 1, NULL));
            } else {
                coefficient = MIN(coefficient, spatial_face_light(face, NULL));
            }
        }
        transmitted = spatial_light_attenuate(transmitted, coefficient);
        if (transmitted == 0) return 0;
    }
    return status == 0 ? sign * transmitted : 0;
}

static int light_path_value(light_resolve_cache *cache, int dx, int dy, int dz, int value) {
    if (!cache->enabled) {
        return light_path_trace(cache, dx, dy, dz, value);
    }
    if (cache->epoch != los_geometry_epoch()) {
        light_resolve_cache_reset(cache);
    }
    int ix = dx + MAX_LIGHT_RADIUS, iy = dy + MAX_LIGHT_RADIUS, iz = dz + MAX_LIGHT_RADIUS;
    if (cache->path_known[iz][iy][ix] && cache->path_in[iz][iy][ix] == value) {
        return cache->path_out[iz][iy][ix];
    }
    uint64_t epoch = cache->epoch;
    int result = light_path_trace(cache, dx, dy, dz, value);
    if (cache->epoch == epoch && epoch == los_geometry_epoch()) {
        cache->path_known[iz][iy][ix] = true;
        cache->path_in[iz][iy][ix] = value;
        cache->path_out[iz][iy][ix] = result;
    }
    return result;
}

static int light_mask_value(int intensity, int distance_squared) {
    return light_profile_value(intensity, distance_squared);
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
                              uint32_t color,
                              light_resolve_cache *shared) {
    if (intensity < 0) {
        mod = -mod;
    }

    intensity = abs(intensity);
    int radius = light_profile_radius(intensity);
    int vertical_radius = MIN(radius, MAP2_MAX_DEPTH);
    HARD_ASSERT(radius <= MAX_LIGHT_RADIUS);
    light_resolve_cache local, *cache = shared;
    if (cache == NULL) {
        cache = &local;
        light_resolve_cache_init(cache, map, x, y);
    }
    HARD_ASSERT(cache->origin == map && cache->x == x && cache->y == y);

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            for (int dz = -vertical_radius; dz <= vertical_radius; dz++) {
                int distance_squared = dx * dx + dy * dy + dz * dz;

                if (distance_squared > radius * radius) {
                    continue;
                }
                int value = light_mask_value(intensity, distance_squared) * mod;
                if (value == 0) {
                    continue;
                }

                int xt, yt;
                mapstruct *target = light_resolve_space(cache, x + dx, y + dy, dz, &xt, &yt);

                if (target == NULL || (only_map != NULL && target != only_map) ||
                    (only_maps != NULL && !light_map_set_contains(only_maps, target)) ||
                    (other_only && target == map)) {
                    continue;
                }

                value = light_path_value(cache, dx, dy, dz, value);
                if (value == 0) continue;
                MapSpace *space = GET_MAP_SPACE_PTR(target, xt, yt);
                if (component == LIGHT_MASK_SCALAR) {
                    space->light_source_value += value;
                } else if (component == LIGHT_MASK_POSITIVE) {
                    space->light_source_positive_value += value;
                } else {
                    space->light_source_color[0] +=
                        (int64_t)value * light_color_linear_component(color, 0);
                    space->light_source_color[1] +=
                        (int64_t)value * light_color_linear_component(color, 1);
                    space->light_source_color[2] +=
                        (int64_t)value * light_color_linear_component(color, 2);
                    space->light_source_color_weight += (int64_t)value * UINT16_MAX;
                }
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
    /* Only coarse levels whose walk reaches this map are invalidated. */
    map->light_local_epoch++;
    light_local_generation++;
    int new_mask = get_real_light_source_value(*counter);

    if (map->in_memory != MAP_LOADING && old_mask != new_mask) {
        if (old_mask != 0) {
            light_mask_adjust(map, x, y, old_mask, -1, NULL, NULL, false, component, 0, NULL);
        }
        if (new_mask != 0) {
            light_mask_adjust(map, x, y, new_mask, 1, NULL, NULL, false, component, 0, NULL);
        }
    }
    light_origin_list_update(map, space);
}

/** Add or remove a legacy scalar light source at one map space. */
void adjust_light_source(mapstruct *map, int x, int y, int light) {
    light_map_unlink_flush();
    light_batch_flush();
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
                          color,
                          NULL);
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
    light_batch_flush();
    /* Rebuild the complete bounded linked volume now that this map's floors
     * and blockers exist. Rebuilding also makes repeated lifecycle calls
     * idempotent instead of adding the same source masks twice. */
    recalculate_light_sources(map);
}

#ifdef ATRINIK_TESTING
static uint64_t light_rebuild_count;
static size_t light_rebuild_target_maps;
static size_t light_rebuild_source_maps;

size_t light_rebuild_target_maps_for_test(void) {
    return light_rebuild_target_maps;
}

size_t light_rebuild_source_maps_for_test(void) {
    return light_rebuild_source_maps;
}

uint64_t light_rebuild_count_for_test(void) {
    return light_rebuild_count;
}
#endif

/** Rebuild exactly the supplied resident targets in one publication. */
static void light_rebuild_targets(const light_map_set *maps, const light_map_set *component,
                                 const light_graph *graph,
                                 bool bounded_sources, bool complete_component) {
    if (maps->count == 0) {
        return;
    }
    HARD_ASSERT(!complete_component || (!bounded_sources && maps->count == component->count));
#ifdef ATRINIK_TESTING
    light_rebuild_count++;
    light_rebuild_target_maps = maps->count;
#endif
    for (size_t i = 0; i < maps->count; i++) {
        mapstruct *target = maps->maps[i];

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

    /* Cleared targets and incoming sources differ. Uniform map dimensions
     * and reciprocal pointers permit bounded inverse reach without commuting
     * links. Other shapes replay the complete weak component. */
    light_map_set sources = {0};
    if (bounded_sources) {
        light_reach_collect(&sources, maps, graph, true);
    }
    const light_map_set *origins = bounded_sources ? &sources : component;
#ifdef ATRINIK_TESTING
    light_rebuild_source_maps = origins->count;
#endif
    /* A full-component clear covers every resident map a replay ray can
     * resolve: all its horizontal and vertical edges stay within that weak
     * component. Omit the per-sample linear target lookup only in this case.
     * Bounded footprints and partial post-unlink repairs retain the fence. */
    const light_map_set *write_targets = complete_component ? NULL : maps;
    for (size_t i = 0; i < origins->count; i++) {
        mapstruct *source_map = origins->maps[i];
        if (source_map->first_light == NULL) {
            continue;
        }

        for (MapSpace *tmp = source_map->first_light; tmp != NULL; tmp = tmp->next_light) {
            ptrdiff_t origin = tmp - source_map->spaces;
            int x = (int)(origin % MAP_WIDTH(source_map));
            int y = (int)(origin / MAP_WIDTH(source_map));
            light_resolve_cache cache;
            light_resolve_cache_init(&cache, source_map, x, y);
            int scalar_mask = get_real_light_source_value(tmp->light_source);
            if (scalar_mask != 0) {
                light_mask_adjust(source_map,
                                  x,
                                  y,
                                  scalar_mask,
                                  1,
                                  NULL,
                                  write_targets,
                                  false,
                                  LIGHT_MASK_SCALAR,
                                  0,
                                  &cache);
            }
            if (tmp->light_source_positive != 0) {
                light_mask_adjust(source_map,
                                  x,
                                  y,
                                  get_real_light_source_value(tmp->light_source_positive),
                                  1,
                                  NULL,
                                  write_targets,
                                  false,
                                  LIGHT_MASK_POSITIVE,
                                  0,
                                  &cache);
            }
            for (object *source = tmp->first; source != NULL; source = source->above) {
                if (source->glow_radius <= 0) {
                    continue;
                }
                uint32_t color = source->light_color;
                light_mask_adjust(source_map,
                                  x,
                                  y,
                                  get_real_light_source_value(source->glow_radius),
                                  1,
                                  NULL,
                                  write_targets,
                                  false,
                                  LIGHT_MASK_COLOR,
                                  color,
                                  &cache);
            }
        }
    }
    free(sources.maps);
}

/** Select a complete pre-change footprint or its whole resident component. */
static void light_geometry_targets_collect(light_map_set *maps, mapstruct *map,
                                           bool bounded_sources,
                                           const light_map_set *component,
                                           const light_graph *graph) {
    memset(maps, 0, sizeof(*maps));
    if (map == NULL || map->in_memory != MAP_IN_MEMORY) {
        return;
    }
    if (bounded_sources) {
        /* Every ray that samples changed geometry has its source in inverse R.
         * Its endpoint can use a different resolver order/column, so expand R
         * again from those sources. This bounds horizontal reach to two seams
         * while retaining noncommuting linked-depth paths. */
        light_map_set seeds = {.maps = &map, .count = 1};
        light_map_set sources;
        light_reach_collect(&sources, &seeds, graph, true);
        light_reach_collect(maps, &sources, graph, false);
        free(sources.maps);
    } else {
        /* Narrow/mixed dimensions can cross several horizontal seams. No
         * coverage or graph-size cap may omit a connected resident target. */
        if (component->count != 0) {
            maps->maps = xmallocarray(component->count, sizeof(*maps->maps));
            memcpy(maps->maps, component->maps, component->count * sizeof(*maps->maps));
            maps->count = maps->capacity = component->count;
        }
    }
}

/** Rebuild source illumination after opaque map geometry changes. */
void recalculate_light_sources(mapstruct *map) {
    if (map == NULL || map->in_memory != MAP_IN_MEMORY) {
        return;
    }
    light_map_set seeds = {.maps = &map, .count = 1};
    light_map_set component;
    light_graph graph;
    light_component_collect(&component, &seeds, &graph);
    bool bounded_sources = light_topology_is_uniform(&component);
    light_map_set maps;
    light_geometry_targets_collect(&maps, map, bounded_sources, &component, &graph);
    light_rebuild_targets(&maps, &component, &graph, bounded_sources, !bounded_sources);
    free(maps.maps);
    light_graph_free(&graph);
    free(component.maps);
}

void light_map_unlink_begin(bool retiring_all) {
    HARD_ASSERT(light_unlink_depth < UINT_MAX);
    light_unlink_depth++;
    if (retiring_all) {
        HARD_ASSERT(light_retirement_depth == 0);
        light_retirement_depth = light_unlink_depth;
    }
}

void light_map_unlink_flush(void) {
    if (!light_unlink_marks_pending) {
        return;
    }
    light_unlink_marks_pending = false;
    light_map_set targets = {0};
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        if (!map->local_light_unlink_pending) {
            continue;
        }
        /* Clear even a swapped map's mark. Deleted maps have already left
         * first_map, so no retained pointer can outlive their ownership. */
        map->local_light_unlink_pending = false;
        light_map_set_add(&targets, map);
    }
    if (targets.count != 0) {
        /* The old component can split at unlink. Seed every marked survivor
         * so the current component union still includes all incoming origins. */
        light_map_set component;
        light_graph graph;
        light_component_collect(&component, &targets, &graph);
        light_rebuild_targets(&targets, &component, &graph,
                              light_topology_is_uniform(&component), false);
        light_graph_free(&graph);
        free(component.maps);
    }
    free(targets.maps);
}

void light_map_unlink_end(void) {
    HARD_ASSERT(light_unlink_depth != 0);
    if (light_retirement_depth == light_unlink_depth) {
        light_retirement_depth = 0;
    }
    if (--light_unlink_depth == 0) {
        light_map_unlink_flush();
    }
}

void light_map_prepare_unlink(mapstruct *map) {
    light_map_unlink_begin(false);
    /* Complete retirement has no surviving map and invokes no gameplay
     * callbacks. Skip both survivor marking and per-map publications, avoiding
     * quadratic scans while free_all_maps() destroys the entire map list. */
    if (light_retirement_depth != 0) {
        return;
    }
    /* The departing map's sources are withdrawn through their old topology.
     * Only surviving sources can leave stale contributions routed through it. */
    light_map_set seeds = {.maps = &map, .count = 1};
    light_map_set component;
    light_graph graph;
    light_component_collect(&component, &seeds, &graph);
    bool surviving_origin = false;
    for (size_t i = 0; i < component.count; i++) {
        mapstruct *source = component.maps[i];
        if (source != map && source->first_light != NULL) {
            surviving_origin = true;
            break;
        }
    }
    if (!surviving_origin) {
        light_graph_free(&graph);
        free(component.maps);
        return;
    }
    light_map_set targets;
    light_geometry_targets_collect(&targets, map, light_topology_is_uniform(&component),
                                   &component, &graph);
    for (size_t i = 0; i < targets.count; i++) {
        if (targets.maps[i] != map) {
            targets.maps[i]->local_light_unlink_pending = true;
            light_unlink_marks_pending = true;
        }
    }
    free(targets.maps);
    light_graph_free(&graph);
    free(component.maps);
}

/**
 * Remove light sources list from a map.
 * @param map
 * The map to remove from.
 */
void remove_light_source_list(mapstruct *map) {
    light_batch_flush();
    MapSpace *tmp;

    for (tmp = map->first_light; tmp; tmp = tmp->next_light) {
        ptrdiff_t origin = tmp - map->spaces;
        int x = (int)(origin % MAP_WIDTH(map));
        int y = (int)(origin / MAP_WIDTH(map));

        int scalar_mask = get_real_light_source_value(tmp->light_source);
        if (scalar_mask != 0) {
            light_mask_adjust(map,
                              x,
                              y,
                              scalar_mask,
                              -1,
                              NULL,
                              NULL,
                              true,
                              LIGHT_MASK_SCALAR,
                              0,
                              NULL);
        }
        int positive_mask = get_real_light_source_value(tmp->light_source_positive);
        if (positive_mask != 0) {
            light_mask_adjust(map,
                              x,
                              y,
                              positive_mask,
                              -1,
                              NULL,
                              NULL,
                              true,
                              LIGHT_MASK_POSITIVE,
                              0,
                              NULL);
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
                                      color,
                                      NULL);
                }
            }
        }
    }

    map->first_light = NULL;
}

/**
 * Signed coarse local light of the fixed 3x3 block containing a cell, aligned
 * to map-local coordinates; edge remainders join the adjacent block. Each
 * block's sources are pooled before propagation, the strongest positive and
 * negative pooled contributions compete by magnitude, and the result is
 * quantized down to whole 1/8-daylight levels. No block's level depends on
 * where inside a block a source sits; neither hue nor count is kept. The
 * block never reads another map.
 */
/**
 * Block of one axis: three cells aligned to the map, except that a one- or
 * two-cell remainder at the edge joins its neighbor, so no block is narrower
 * than three cells unless the map is.
 */
static void light_local_block(int coordinate, int size, int *start, int *end) {
    *start = coordinate - coordinate % 3;
    if (*start + 3 > size) {
        *start = MAX(0, *start - 3);
    }
    *end = *start + 6 > size ? size : *start + 3;
}

/**
 * Pooled sources of a block: the strongest positive and the strongest negative
 * single-source mask. Each emitting object counts with its own glow radius,
 * and light a space's counter holds without an object counts as one more
 * source; sources are never summed with one another. A viewer's known light,
 * even on the same tile as a private source, therefore cannot combine with it
 * through the nonlinear mask and reveal its radius by saturation: it reveals
 * only the maximum, which the level already discloses.
 */
static void light_local_block_sources(mapstruct *map, int bx, int ex, int by, int ey,
                                      int *positive, int *negative) {
    *positive = 0;
    *negative = 0;
    for (int cy = by; cy < ey; cy++) {
        for (int cx = bx; cx < ex; cx++) {
            const MapSpace *space = GET_MAP_SPACE_PTR(map, cx, cy);
            int64_t residual = space->light_source;
            for (const object *op = GET_MAP_OB(map, cx, cy); op != NULL; op = op->above) {
                if (op->glow_radius == 0) {
                    continue;
                }
                int mask = get_real_light_source_value(op->glow_radius);
                *positive = MAX(*positive, mask);
                *negative = MIN(*negative, mask);
                residual -= op->glow_radius;
            }
            if (residual != 0) {
                int mask = get_real_light_source_value(
                    (int)MAX(INT32_MIN, MIN(INT32_MAX, residual)));
                *positive = MAX(*positive, mask);
                *negative = MIN(*negative, mask);
            }
        }
    }
}

/**
 * The resident neighbor of a map across one seam that local light crosses: a
 * reciprocal link, and continuity as light_face_continuous() requires (two
 * schema-0 maps, or two celestial-v1 maps with continuous boundaries and equal
 * effective profiles). Never loads a map.
 */
static mapstruct *light_local_seam(mapstruct *map, int tile) {
    mapstruct *next = map->tile_map[tile];
    int reverse = map_tiled_reverse[tile];
    if (next == NULL || next->in_memory != MAP_IN_MEMORY || next->spaces == NULL ||
        next->tile_map[reverse] != map) {
        return NULL;
    }
    if (map->celestial_schema == 0 && next->celestial_schema == 0) {
        return next;
    }
    if (map->celestial_schema != 1 || next->celestial_schema != 1 ||
        map->celestial_boundary[tile] != CELESTIAL_BOUNDARY_CONTINUOUS ||
        next->celestial_boundary[reverse] != CELESTIAL_BOUNDARY_CONTINUOUS ||
        memcmp(region_celestial_for_map(map)->digest, region_celestial_for_map(next)->digest,
               32) != 0) {
        return NULL;
    }
    return next;
}

/* Each map keeps the level of every one of its blocks, valid while the
 * sources of every map the block's walk reaches and the geometry epoch are
 * unchanged: every cell and sub-layer of a block shares its level. */

/** One map placed relative to the target block: cell (cx, cy) lies at
 * (cx + ox, cy + oy) on the target map, on any level of its linked stack. */
typedef struct light_local_region {
    mapstruct *map;
    int ox, oy;
} light_local_region;

/**
 * Most maps one coarse level may pool. Real topologies stay far below it (a
 * 13-level stack of one-cell maps within reach places 1053); a component
 * that exceeds it, such as an unbounded legacy link cycle or stack, fails
 * closed to level 0 for every target in it, so no rebuild is unbounded and
 * the levels of such a component still agree.
 */
#define LIGHT_LOCAL_REGIONS_MAX 4096

/* Open-addressed sets, cleared by advancing a stamp, so a walk and its
 * distinct-map listing stay linear in the regions they place. Sized above
 * twice the region bound, they never fill. */
#define LIGHT_LOCAL_SET_SIZE 16384

typedef struct light_local_set_entry {
    const mapstruct *map;
    int ox, oy;
    uint32_t stamp;
} light_local_set_entry;

static light_local_set_entry light_local_visited[LIGHT_LOCAL_SET_SIZE];
static light_local_set_entry light_local_distinct[LIGHT_LOCAL_SET_SIZE];
static uint32_t light_local_set_stamp;

/** Insert (map, ox, oy) into a set; false when it was already present. */
static bool light_local_set_insert(light_local_set_entry *set, const mapstruct *map, int ox,
                                   int oy) {
    uint64_t key = (uint64_t)(uintptr_t)map * UINT64_C(0x9e3779b97f4a7c15) ^
                   (uint64_t)(uint32_t)ox * UINT64_C(0xbf58476d1ce4e5b9) ^
                   (uint64_t)(uint32_t)oy * UINT64_C(0x94d049bb133111eb);
    for (size_t slot = (size_t)(key >> 32) % LIGHT_LOCAL_SET_SIZE;;
         slot = (slot + 1) % LIGHT_LOCAL_SET_SIZE) {
        light_local_set_entry *entry = &set[slot];
        if (entry->stamp != light_local_set_stamp) {
            *entry = (light_local_set_entry){.map = map, .ox = ox, .oy = oy,
                                             .stamp = light_local_set_stamp};
            return true;
        }
        if (entry->map == map && entry->ox == ox && entry->oy == oy) {
            return false;
        }
    }
}

/** Start a new walk: both sets become empty. */
static void light_local_set_reset(void) {
    if (++light_local_set_stamp == 0) {
        memset(light_local_visited, 0, sizeof(light_local_visited));
        memset(light_local_distinct, 0, sizeof(light_local_distinct));
        light_local_set_stamp = 1;
    }
}

typedef struct light_local_cache_entry {
    /** Zero until the block's level is first computed. */
    uint64_t geometry_epoch;
    uint64_t source_hash;
    /** light_local_generation when the level was last confirmed current. */
    uint64_t validated;
    /** light_local_frees when the entry was walked: a later free may have
     * released one of its dependency maps. */
    uint64_t frees;
    /** The distinct maps the block's walk reached, valid for its geometry
     * epoch: revalidation hashes only their source epochs, never rewalks. */
    mapstruct **deps;
    size_t deps_count, deps_capacity;
    int8_t level;
} light_local_cache_entry;


void light_local_forget(mapstruct *map) {
    /* Any cached walk may hold this map: walk again before reading one. */
    light_local_frees++;
    for (size_t i = 0; i < map->light_local_cache_blocks; i++) {
        free(map->light_local_cache[i].deps);
    }
    FREE_AND_NULL_PTR(map->light_local_cache);
    map->light_local_cache_blocks = 0;
}

/** Hash of the source epochs of a cached block's dependency maps. */
static uint64_t light_local_deps_hash(mapstruct *const *deps, size_t count) {
    /* Everything the block's walk and pooling read from its maps: their
     * sources, and for each the links the walk may follow, whether the
     * linked map is resident, and the profile digest that seam continuity
     * compares. A change anywhere else, such as another map being saved,
     * leaves the signature and so the cached level intact. */
    uint64_t hash = UINT64_C(1469598103934665603);
#define LIGHT_LOCAL_MIX(value) (hash = (hash ^ (uint64_t)(value)) * UINT64_C(1099511628211))
    for (size_t i = 0; i < count; i++) {
        LIGHT_LOCAL_MIX((uintptr_t)deps[i]);
        LIGHT_LOCAL_MIX(deps[i]->light_local_epoch);
        for (int tile = 0; tile < TILED_NUM; tile++) {
            const mapstruct *next = deps[i]->tile_map[tile];
            LIGHT_LOCAL_MIX((uintptr_t)next);
            if (next == NULL) {
                continue;
            }
            LIGHT_LOCAL_MIX(next->in_memory == MAP_IN_MEMORY && next->spaces != NULL);
            LIGHT_LOCAL_MIX(next->tile_map[map_tiled_reverse[tile]] == deps[i]);
            if (next->celestial_schema == 1) {
                uint64_t digest;
                memcpy(&digest, region_celestial_for_map(next)->digest, sizeof(digest));
                LIGHT_LOCAL_MIX(digest);
            }
        }
    }
#undef LIGHT_LOCAL_MIX
    return hash;
}

static bool light_local_walk(mapstruct *map, int bx, int ex, int by, int ey,
                             const light_local_region **regions, size_t *count);
static int8_t light_local_level_pool(const light_local_region *regions, size_t count, int bx,
                                     int ex, int by, int ey);

int8_t light_local_level(mapstruct *map, int x, int y) {
    HARD_ASSERT(map != NULL && map->spaces != NULL);
    /* A block of fewer than nine cells would locate a hidden source too
     * precisely: such a map reports no coarse light (fail closed). */
    if (MAP_WIDTH(map) < 3 || MAP_HEIGHT(map) < 3) {
        return 0;
    }
    int bx, by, ex, ey;
    light_local_block(x, MAP_WIDTH(map), &bx, &ex);
    light_local_block(y, MAP_HEIGHT(map), &by, &ey);
    /* A block's walk depends only on topology, which the geometry epoch
     * tracks, so each entry keeps the distinct maps it reached. A lookup
     * returns at once when no source changed anywhere since the level was
     * confirmed, rehashes only its own maps' source epochs otherwise, and
     * walks and pools again only when the topology or one of those sources
     * changed. Every cell of a block shares one entry. */
    /* Every block of a map has its own entry, so no working set of players
     * and maps evicts another's; the cache lives and dies with the map. */
    size_t blocks_x = (size_t)MAX(1, MAP_WIDTH(map) / 3);
    size_t blocks_y = (size_t)MAX(1, MAP_HEIGHT(map) / 3);
    if (map->light_local_cache == NULL) {
        map->light_local_cache_blocks = blocks_x * blocks_y;
        map->light_local_cache =
            xcalloc(map->light_local_cache_blocks, sizeof(*map->light_local_cache));
    }
    size_t index = MIN((size_t)by / 3, blocks_y - 1) * blocks_x +
                   MIN((size_t)bx / 3, blocks_x - 1);
    HARD_ASSERT(index < map->light_local_cache_blocks);
    light_local_cache_entry *entry = &map->light_local_cache[index];
    uint64_t geometry = los_geometry_epoch();
    /* An entry whose maps may since have been freed is never read: it walks
     * again. Otherwise nothing changed when neither the source generation
     * nor the geometry epoch moved, and a change elsewhere only costs
     * rehashing this block's own maps. */
    if (entry->geometry_epoch != 0 && entry->frees == light_local_frees) {
        if (entry->validated == light_local_generation && entry->geometry_epoch == geometry) {
            return entry->level;
        }
        if (light_local_deps_hash(entry->deps, entry->deps_count) == entry->source_hash) {
            entry->validated = light_local_generation;
            entry->geometry_epoch = geometry;
            return entry->level;
        }
    }
    const light_local_region *regions;
    size_t count;
    int8_t level = 0;
    entry->deps_count = 0;
    if (light_local_walk(map, bx, ex, by, ey, &regions, &count)) {
        level = light_local_level_pool(regions, count, bx, ex, by, ey);
        /* Distinct maps, in walk order; a map placed at several offsets
         * appears once. */
        for (size_t i = 0; i < count; i++) {
            if (!light_local_set_insert(light_local_distinct, regions[i].map, 0, 0)) {
                continue;
            }
            if (entry->deps_count == entry->deps_capacity) {
                entry->deps_capacity = entry->deps_capacity == 0 ? 8 : entry->deps_capacity * 2;
                entry->deps = xreallocarray(entry->deps, entry->deps_capacity, sizeof(*entry->deps));
            }
            entry->deps[entry->deps_count++] = regions[i].map;
        }
    }
    entry->geometry_epoch = geometry;
    entry->frees = light_local_frees;
    entry->source_hash = light_local_deps_hash(entry->deps, entry->deps_count);
    entry->validated = light_local_generation;
    entry->level = level;
    return level;
}

/**
 * Pool every block of one source map region within reach of the target block
 * [bx, ex) x [by, ey). The region's cell (cx, cy) lies at (cx + ox, cy + oy) in
 * the target map's coordinates; its blocks need not align with the target's,
 * because each reaches it by the gap between the two rectangles. The region
 * may be any level of the target's linked stack: levels form one vertical
 * bucket, so only the horizontal gap counts.
 */
static void light_local_pool_region(mapstruct *source, int ox, int oy, int bx, int ex, int by,
                                    int ey, int32_t *positive, int32_t *negative) {
    /* A map narrower than a block would pin a source to fewer than nine
     * cells: it contributes nothing (fail closed). */
    if (MAP_WIDTH(source) < 3 || MAP_HEIGHT(source) < 3) {
        return;
    }
    int reach = MAX_LIGHT_RADIUS;
    int x0 = MAX(0, bx - reach - ox), x1 = MIN(MAP_WIDTH(source), ex + reach - ox);
    int y0 = MAX(0, by - reach - oy), y1 = MIN(MAP_HEIGHT(source), ey + reach - oy);
    for (int cy = y0; cy < y1;) {
        int sy, sey;
        light_local_block(cy, MAP_HEIGHT(source), &sy, &sey);
        for (int cx = x0; cx < x1;) {
            int sx, sex;
            light_local_block(cx, MAP_WIDTH(source), &sx, &sex);
            int gap_x = MAX(0, MAX(sx + ox - (ex - 1), bx - (sex - 1 + ox)));
            int gap_y = MAX(0, MAX(sy + oy - (ey - 1), by - (sey - 1 + oy)));
            int distance_squared = gap_x * gap_x + gap_y * gap_y;
            int strongest[2];
            light_local_block_sources(source, sx, sex, sy, sey, &strongest[0], &strongest[1]);
            for (int sign = 0; sign < 2; sign++) {
                int mask = strongest[sign];
                if (mask == 0) {
                    continue;
                }
                int radius = light_profile_radius(abs(mask));
                if (distance_squared > radius * radius) {
                    continue;
                }
                int32_t value = light_mask_value(abs(mask), distance_squared);
                if (mask > 0) {
                    *positive = MAX(*positive, value);
                } else {
                    *negative = MIN(*negative, -value);
                }
            }
            cx = sex;
        }
        cy = sey;
    }
}



static bool light_local_walk(mapstruct *map, int bx, int ex, int by, int ey,
                             const light_local_region **regions_out, size_t *count_out) {
    /* Sources are pooled per block before propagation and reach a block by
     * the horizontal gap between the two blocks, which depends only on block
     * geometry, so a source's position inside its block never changes any
     * block's level. Every map within the largest radius counts, reached
     * through continuous horizontal seams and linked levels in any order (a
     * level linked only from a neighbor counts too); no map is ever loaded
     * for it. The linked levels of a stack form one vertical bucket: a
     * source on any of them counts at its horizontal gap alone, so every
     * level of the stack gets the same level and the levels never reveal
     * which floor a hidden source is on. Regions are keyed by map and
     * horizontal offset, which also ends cyclic level links, and a
     * component placing more than LIGHT_LOCAL_REGIONS_MAX maps fails closed. */
    /* Placed maps grow with the reachable topology, never truncated: each
     * (map, offset) is placed once and offsets stay within reach, so the set
     * is finite however small the maps are. */
    static light_local_region *regions;
    static size_t capacity;
    static const int tiles[] = {TILED_WEST,      TILED_EAST,      TILED_NORTH,
                                TILED_SOUTH,     TILED_NORTHEAST, TILED_SOUTHEAST,
                                TILED_SOUTHWEST, TILED_NORTHWEST, TILED_UP,
                                TILED_DOWN};
    int reach = MAX_LIGHT_RADIUS;
    size_t count = 0;
    if (capacity == 0) {
        capacity = 64;
        regions = xmallocarray(capacity, sizeof(*regions));
    }
#ifdef ATRINIK_TESTING
    light_local_walks++;
#endif
    light_local_set_reset();
    regions[count++] = (light_local_region){.map = map};
    light_local_set_insert(light_local_visited, map, 0, 0);
    for (size_t i = 0; i < count; i++) {
        light_local_region region = regions[i];
        for (size_t t = 0; t < arraysize(tiles); t++) {
            light_local_region next = region;
            next.map = light_local_seam(region.map, tiles[t]);
            if (next.map == NULL) {
                continue;
            }
            if (tiles[t] == TILED_UP || tiles[t] == TILED_DOWN) {
                /* Blocks align only across levels of the same dimensions. */
                if (MAP_WIDTH(next.map) != MAP_WIDTH(region.map) ||
                    MAP_HEIGHT(next.map) != MAP_HEIGHT(region.map)) {
                    continue;
                }
            } else {
                /* Diagonal seams move on both axes, as the light topology
                 * crosses them directly. */
                bool west = tiles[t] == TILED_WEST || tiles[t] == TILED_SOUTHWEST ||
                            tiles[t] == TILED_NORTHWEST;
                bool east = tiles[t] == TILED_EAST || tiles[t] == TILED_NORTHEAST ||
                            tiles[t] == TILED_SOUTHEAST;
                bool north = tiles[t] == TILED_NORTH || tiles[t] == TILED_NORTHEAST ||
                             tiles[t] == TILED_NORTHWEST;
                bool south = tiles[t] == TILED_SOUTH || tiles[t] == TILED_SOUTHEAST ||
                             tiles[t] == TILED_SOUTHWEST;
                next.ox += west ? -MAP_WIDTH(next.map) : east ? MAP_WIDTH(region.map) : 0;
                next.oy += north ? -MAP_HEIGHT(next.map) : south ? MAP_HEIGHT(region.map) : 0;
            }
            if (next.ox >= ex + reach || next.ox + MAP_WIDTH(next.map) <= bx - reach ||
                next.oy >= ey + reach || next.oy + MAP_HEIGHT(next.map) <= by - reach) {
                continue;
            }
            /* Fail closed for the whole component past the bound. */
            if (count == LIGHT_LOCAL_REGIONS_MAX &&
                light_local_set_insert(light_local_visited, next.map, next.ox, next.oy)) {
                return false;
            }
            if (count < LIGHT_LOCAL_REGIONS_MAX &&
                light_local_set_insert(light_local_visited, next.map, next.ox, next.oy)) {
                if (count == capacity) {
                    capacity *= 2;
                    regions = xreallocarray(regions, capacity, sizeof(*regions));
                }
                regions[count++] = next;
            }
        }
    }
    *regions_out = regions;
    *count_out = count;
    return true;
}

static int8_t light_local_level_pool(const light_local_region *regions, size_t count, int bx,
                                     int ex, int by, int ey) {
    int32_t positive = 0, negative = 0;
    for (size_t i = 0; i < count; i++) {
        light_local_pool_region(regions[i].map, regions[i].ox, regions[i].oy, bx, ex, by, ey,
                                &positive, &negative);
    }
    return light_local_level_from_extrema(positive, negative);
}

int8_t light_local_level_from_extrema(int32_t positive, int32_t negative) {
    /* Each sign is quantized before they compete, so a viewer's known light of
     * the opposite sign learns only the hidden extremum's level, never where
     * it lies inside its 1/8-daylight bin. Equal levels go to light. */
    int light = (int)MIN(MAX(positive, 0) / MAP2_LOCAL_LIGHT_LEVEL_RAW,
                         MAP2_LOCAL_LIGHT_LEVEL_MAX);
    int dark = (int)MIN(MAX(-(int64_t)negative, 0) / MAP2_LOCAL_LIGHT_LEVEL_RAW,
                        MAP2_LOCAL_LIGHT_LEVEL_MAX);
    return (int8_t)(dark > light ? -dark : light);
}
