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
 * Implements map type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <video.h>
#include <surface_primitives.h>
#include <client_socket.h>
#include <player.h>
#include <animations.h>
#include <map_transform.h>
#include <region_map.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>
#include <toolkit/bresenham.h>
#include <toolkit/clioptions.h>
#include <toolkit/path.h>

/**
 * Map cells.
 */
typedef struct map_cell_layer_record {
    struct map_cell_layer_record *next;
    uint8_t layer;
    uint8_t quick_pos;
    uint8_t flags;
    uint8_t roof;
    uint8_t draw_double;
    uint8_t alpha;
    uint8_t infravision;
    uint8_t anim_last;
    uint8_t anim_speed;
    uint8_t anim_facing;
    uint8_t anim_state;
    uint8_t glow_speed;
    uint8_t glow_state;
    int16_t face;
    int16_t height;
    int16_t zoom_x;
    int16_t zoom_y;
    int16_t align;
    int16_t rotate;
    map_visibility_fade_t visibility;
    char glow[COLOR_BUF];
} map_cell_layer_record_t;

typedef struct map_cell_sublayer_record {
    struct map_cell_sublayer_record *next;
    uint8_t sub_layer;
    uint8_t door;
    uint8_t exit;
    uint8_t priority;
    uint8_t secondpass;
    int32_t stretch;
} map_cell_sublayer_record_t;

typedef struct map_cell_actor_record {
    struct map_cell_actor_record *next;
    uint8_t sub_layer;
    uint8_t probe;
    uint8_t target_is_friend;
    uint8_t anim_flags;
    uint32_t target_object_count;
    char name[64];
    char color[COLOR_BUF];
} map_cell_actor_record_t;

typedef struct map_cell_light_record {
    struct map_cell_light_record *next;
    uint8_t sub_layer;
    uint8_t known;
    uint8_t rgb_explicit;
    uint8_t next_known;
    uint8_t next_rgb_explicit;
    uint16_t radiance;
    uint16_t rgb_radiance[3];
    uint16_t next_radiance;
    uint16_t next_rgb_radiance[3];
} map_cell_light_record_t;

typedef struct map_cell_light_keyframe_record {
    uint64_t generation;
    uint64_t start_seconds;
    uint64_t end_seconds;
    uint8_t flags;
    uint8_t valid;
} map_cell_light_keyframe_record_t;

/** Compact owner for sparse records associated with one populated coordinate. */
typedef struct map_cell {
    map_cell_layer_record_t *layers;
    map_cell_sublayer_record_t *sub_layers;
    map_cell_actor_record_t *actors;
    map_cell_light_record_t *lights;
    map_cell_light_keyframe_record_t *light_keyframe;
    /** Monotonic identity retained across transaction clones for painter reuse. */
    uint64_t painter_identity;
    uint8_t fow;
    uint8_t structural_fow;
    uint8_t stretch_dirty;
    int16_t stretch_top_height;
    int16_t stretch_upper_height;
    int16_t level_support_height;
    int16_t render_max_height;
} map_cell_t;

typedef struct map_cell_store {
    map_cell_t **slots;
    struct map_cell_header *headers;
    size_t count;
} map_cell_store_t;

/** Dense, compact state retained for every cache coordinate. */
typedef struct map_cell_header {
    uint32_t generation;
    uint16_t revision;
    int16_t support_height;
    uint8_t occupancy;
    uint8_t fow;
    uint8_t structural_fow;
    uint8_t reserved;
} map_cell_header_t;

static map_cell_store_t *cells;
static map_cell_store_t *level_cells[MAP2_LEVELS];
static uint64_t map_cell_allocation_count;
static uint64_t map_cell_allocation_bytes;
static uint64_t map_cell_retained_bytes;
static uint64_t map_cell_peak_retained_bytes;
static uint64_t map_cell_next_painter_identity = 1;
static uint64_t level_lighting_revision[MAP2_LEVELS];
static size_t current_level_index = MAP2_DEPTH_INDEX(0);
static uint16_t map_level_mask;
static int map_width;
static int map_height;
static int map_cache_origin_x;
static int map_cache_origin_y;
static map_benchmark_statistics_t map_benchmark_statistics;
static uint64_t map_benchmark_animation_command_digest;
static bool map_benchmark_animation_command_digest_valid;
static uint32_t map_pending_redraw_reasons;
static bool map_animation_redraw_flag;
/* The temporal light presentation is invalidated at one-minute game-time
 * buckets, never by an exact per-frame clock value. */
static uint64_t map_temporal_lighting_bucket = UINT64_MAX;

typedef struct map_light_keyframe_stage {
    int depth;
    int x;
    int y;
    uint8_t scalar_bitmap;
    uint16_t scalar[NUM_SUB_LAYERS];
    uint8_t rgb_bitmap;
    uint16_t rgb[NUM_SUB_LAYERS][3];
} map_light_keyframe_stage_t;

static struct {
    bool active;
    uint64_t generation;
    uint64_t start_seconds;
    uint64_t end_seconds;
    uint8_t flags;
    size_t count;
    size_t capacity;
    map_light_keyframe_stage_t *entries;
} map_light_keyframe_transaction;

void map_redraw_request(map_redraw_reason_t reason) {
    HARD_ASSERT(reason != 0);
    map_pending_redraw_reasons |= (uint32_t)reason;
    if (reason == MAP_REDRAW_REASON_ANIMATION) {
        map_animation_redraw_flag = true;
    } else {
        map_redraw_flag = 1;
    }
}

bool map_redraw_due(void) {
    return map_redraw_flag != 0;
}

bool map_animation_redraw_due(void) {
    return map_animation_redraw_flag;
}

uint32_t map_redraw_pending_reasons(void) {
    if (!map_redraw_due() && !map_animation_redraw_due()) {
        return 0;
    }
    return map_pending_redraw_reasons != 0 ? map_pending_redraw_reasons
                                           : MAP_REDRAW_REASON_EXTERNAL;
}

void map_redraw_consume(void) {
    map_redraw_flag = 0;
    map_animation_redraw_flag = false;
    map_pending_redraw_reasons = 0;
}

void map_animation_redraw_consume(void) {
    map_animation_redraw_flag = false;
    map_pending_redraw_reasons &= ~(uint32_t)MAP_REDRAW_REASON_ANIMATION;
}

void map_benchmark_statistics_reset(void) {
    map_cell_allocation_count = 0;
    map_cell_allocation_bytes = 0;
    map_cell_peak_retained_bytes = map_cell_retained_bytes;
    map_benchmark_statistics = (map_benchmark_statistics_t){0};
    map_benchmark_animation_command_digest = 0;
    map_benchmark_animation_command_digest_valid = false;
}

void map_benchmark_statistics_get(map_benchmark_statistics_t *statistics) {
    HARD_ASSERT(statistics != NULL);
    *statistics = map_benchmark_statistics;
}

void map_benchmark_statistics_present(bool success) {
    map_benchmark_statistics.presents++;
    map_benchmark_statistics.present_failures += !success;
}

static map_cell_store_t *map_cell_store_create(size_t count);
static void map_cell_store_destroy(map_cell_store_t *store);
static map_cell_t *map_cell_store_slot(map_cell_store_t *store, size_t index, bool create);
static void map_cell_store_clear_slot(map_cell_store_t *store, size_t index);
static void map_cell_store_trim_slot(map_cell_store_t *store, size_t index);
static bool
map_cell_store_set_support_height(map_cell_store_t *store, size_t index, int16_t height);
static void
map_cell_store_set_fow(map_cell_store_t *store, size_t index, bool fow, bool structural_fow);
static void map_mark_stretch_dirty(int x, int y);
static int map_level_support_height(int x, int y, int depth);

static void *map_cell_record_allocate(size_t size) {
    void *record = xcalloc(1, size);
    map_cell_allocation_count++;
    map_cell_allocation_bytes += size;
    map_cell_retained_bytes += size;
    map_cell_peak_retained_bytes = MAX(map_cell_peak_retained_bytes, map_cell_retained_bytes);
    return record;
}

static void map_cell_record_release(void *record, size_t size) {
    if (record == NULL) {
        return;
    }
    HARD_ASSERT(map_cell_retained_bytes >= size);
    map_cell_retained_bytes -= size;
    free(record);
}

static map_cell_layer_record_t *map_cell_layer_record(map_cell_t *cell, int layer, bool create) {
    HARD_ASSERT(cell != NULL);
    HARD_ASSERT(layer >= 0 && layer < NUM_REAL_LAYERS);
    for (map_cell_layer_record_t *record = cell->layers; record != NULL; record = record->next) {
        if (record->layer == layer) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    map_cell_layer_record_t *record = map_cell_record_allocate(sizeof(*record));
    record->layer = (uint8_t)layer;
    record->next = cell->layers;
    cell->layers = record;
    return record;
}

static const map_cell_layer_record_t *map_cell_layer_record_read(const map_cell_t *cell,
                                                                 int layer) {
    static const map_cell_layer_record_t empty;
    map_cell_layer_record_t *record = map_cell_layer_record((map_cell_t *)cell, layer, false);
    return record != NULL ? record : &empty;
}

static void map_cell_layer_record_remove(map_cell_t *cell, int layer) {
    map_cell_layer_record_t **link = &cell->layers;
    while (*link != NULL) {
        map_cell_layer_record_t *record = *link;
        if (record->layer == layer) {
            *link = record->next;
            map_cell_record_release(record, sizeof(*record));
            return;
        }
        link = &record->next;
    }
}

static bool map_cell_layer_record_empty(const map_cell_layer_record_t *record) {
    return record->quick_pos == 0 && record->flags == 0 && record->roof == 0 &&
           record->draw_double == 0 && record->alpha == 0 && record->infravision == 0 &&
           record->anim_last == 0 && record->anim_speed == 0 && record->anim_facing == 0 &&
           record->anim_state == 0 && record->glow_speed == 0 && record->glow_state == 0 &&
           record->face == 0 && record->height == 0 && record->zoom_x == 0 && record->zoom_y == 0 &&
           record->align == 0 && record->rotate == 0 && !record->visibility.initialized &&
           record->glow[0] == '\0';
}

static map_cell_sublayer_record_t *
map_cell_sublayer_record(map_cell_t *cell, int sub_layer, bool create) {
    HARD_ASSERT(cell != NULL);
    HARD_ASSERT(sub_layer >= 0 && sub_layer < NUM_SUB_LAYERS);
    for (map_cell_sublayer_record_t *record = cell->sub_layers; record != NULL;
         record = record->next) {
        if (record->sub_layer == sub_layer) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    map_cell_sublayer_record_t *record = map_cell_record_allocate(sizeof(*record));
    record->sub_layer = (uint8_t)sub_layer;
    record->next = cell->sub_layers;
    cell->sub_layers = record;
    return record;
}

static const map_cell_sublayer_record_t *map_cell_sublayer_record_read(const map_cell_t *cell,
                                                                       int sub_layer) {
    static const map_cell_sublayer_record_t empty;
    map_cell_sublayer_record_t *record =
        map_cell_sublayer_record((map_cell_t *)cell, sub_layer, false);
    return record != NULL ? record : &empty;
}

static void map_cell_sublayer_record_trim(map_cell_t *cell, int sub_layer) {
    map_cell_sublayer_record_t **link = &cell->sub_layers;
    while (*link != NULL) {
        map_cell_sublayer_record_t *record = *link;
        if (record->sub_layer != sub_layer) {
            link = &record->next;
            continue;
        }
        if (record->door == 0 && record->exit == 0 && record->priority == 0 &&
            record->secondpass == 0 && record->stretch == 0) {
            *link = record->next;
            map_cell_record_release(record, sizeof(*record));
        }
        return;
    }
}

static map_cell_actor_record_t *
map_cell_actor_record(map_cell_t *cell, int sub_layer, bool create) {
    HARD_ASSERT(cell != NULL);
    HARD_ASSERT(sub_layer >= 0 && sub_layer < NUM_SUB_LAYERS);
    for (map_cell_actor_record_t *record = cell->actors; record != NULL; record = record->next) {
        if (record->sub_layer == sub_layer) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    map_cell_actor_record_t *record = map_cell_record_allocate(sizeof(*record));
    record->sub_layer = (uint8_t)sub_layer;
    record->next = cell->actors;
    cell->actors = record;
    return record;
}

static const map_cell_actor_record_t *map_cell_actor_record_read(const map_cell_t *cell,
                                                                 int sub_layer) {
    static const map_cell_actor_record_t empty;
    map_cell_actor_record_t *record = map_cell_actor_record((map_cell_t *)cell, sub_layer, false);
    return record != NULL ? record : &empty;
}

static void map_cell_actor_record_trim(map_cell_t *cell, int sub_layer) {
    map_cell_actor_record_t **link = &cell->actors;
    while (*link != NULL) {
        map_cell_actor_record_t *record = *link;
        if (record->sub_layer != sub_layer) {
            link = &record->next;
            continue;
        }
        if (record->probe == 0 && record->target_is_friend == 0 && record->anim_flags == 0 &&
            record->target_object_count == 0 && record->name[0] == '\0' &&
            record->color[0] == '\0') {
            *link = record->next;
            map_cell_record_release(record, sizeof(*record));
        }
        return;
    }
}

static map_cell_light_record_t *
map_cell_light_record(map_cell_t *cell, int sub_layer, bool create) {
    HARD_ASSERT(cell != NULL);
    HARD_ASSERT(sub_layer >= 0 && sub_layer < NUM_SUB_LAYERS);
    for (map_cell_light_record_t *record = cell->lights; record != NULL; record = record->next) {
        if (record->sub_layer == sub_layer) {
            return record;
        }
    }
    if (!create) {
        return NULL;
    }
    map_cell_light_record_t *record = map_cell_record_allocate(sizeof(*record));
    record->sub_layer = (uint8_t)sub_layer;
    record->next = cell->lights;
    cell->lights = record;
    return record;
}

static const map_cell_light_record_t *map_cell_light_record_read(const map_cell_t *cell,
                                                                 int sub_layer) {
    static const map_cell_light_record_t empty;
    map_cell_light_record_t *record = map_cell_light_record((map_cell_t *)cell, sub_layer, false);
    return record != NULL ? record : &empty;
}

static void map_cell_light_record_trim(map_cell_t *cell, int sub_layer) {
    map_cell_light_record_t **link = &cell->lights;
    while (*link != NULL) {
        map_cell_light_record_t *record = *link;
        if (record->sub_layer != sub_layer) {
            link = &record->next;
            continue;
        }
        if (record->known == 0 && record->rgb_explicit == 0 && record->next_known == 0 &&
            record->next_rgb_explicit == 0 && record->radiance == 0 &&
            record->rgb_radiance[0] == 0 && record->rgb_radiance[1] == 0 &&
            record->rgb_radiance[2] == 0 && record->next_radiance == 0 &&
            record->next_rgb_radiance[0] == 0 && record->next_rgb_radiance[1] == 0 &&
            record->next_rgb_radiance[2] == 0) {
            *link = record->next;
            map_cell_record_release(record, sizeof(*record));
        }
        return;
    }
}

static map_cell_light_keyframe_record_t *map_cell_light_keyframe_record(map_cell_t *cell,
                                                                        bool create) {
    HARD_ASSERT(cell != NULL);
    if (cell->light_keyframe == NULL && create) {
        cell->light_keyframe = map_cell_record_allocate(sizeof(*cell->light_keyframe));
    }
    return cell->light_keyframe;
}

static const map_cell_light_keyframe_record_t *
map_cell_light_keyframe_record_read(const map_cell_t *cell) {
    static const map_cell_light_keyframe_record_t empty;
    return cell->light_keyframe != NULL ? cell->light_keyframe : &empty;
}

static uint64_t map_cell_hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/** Stable semantic fingerprint used by compact MAP packet change tokens. */
static uint64_t map_cell_content_hash(const map_cell_t *cell) {
    if (cell == NULL) {
        return 0;
    }

    bool has_sparse_content = cell->layers != NULL || cell->sub_layers != NULL ||
                              cell->actors != NULL || cell->lights != NULL ||
                              cell->light_keyframe != NULL || cell->stretch_dirty != 0 ||
                              cell->stretch_top_height != 0 || cell->stretch_upper_height != 0 ||
                              cell->level_support_height != 0 || cell->render_max_height != 0;
    if (!has_sparse_content) {
        return 0;
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    hash = map_cell_hash_bytes(hash, &cell->stretch_dirty, sizeof(cell->stretch_dirty));
    hash = map_cell_hash_bytes(hash, &cell->stretch_top_height, sizeof(cell->stretch_top_height));
    hash =
        map_cell_hash_bytes(hash, &cell->stretch_upper_height, sizeof(cell->stretch_upper_height));
    hash =
        map_cell_hash_bytes(hash, &cell->level_support_height, sizeof(cell->level_support_height));
    hash = map_cell_hash_bytes(hash, &cell->render_max_height, sizeof(cell->render_max_height));
    for (int layer = 0; layer < NUM_REAL_LAYERS; layer++) {
        const map_cell_layer_record_t *record =
            map_cell_layer_record((map_cell_t *)cell, layer, false);
        if (record != NULL) {
            hash = map_cell_hash_bytes(hash,
                                       (const uint8_t *)record +
                                           offsetof(map_cell_layer_record_t, layer),
                                       sizeof(*record) - offsetof(map_cell_layer_record_t, layer));
        }
    }
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        const map_cell_sublayer_record_t *sub_record =
            map_cell_sublayer_record((map_cell_t *)cell, sub_layer, false);
        if (sub_record != NULL) {
            hash = map_cell_hash_bytes(
                hash,
                (const uint8_t *)sub_record + offsetof(map_cell_sublayer_record_t, sub_layer),
                sizeof(*sub_record) - offsetof(map_cell_sublayer_record_t, sub_layer));
        }
        const map_cell_actor_record_t *actor_record =
            map_cell_actor_record((map_cell_t *)cell, sub_layer, false);
        if (actor_record != NULL) {
            hash = map_cell_hash_bytes(
                hash,
                (const uint8_t *)actor_record + offsetof(map_cell_actor_record_t, sub_layer),
                sizeof(*actor_record) - offsetof(map_cell_actor_record_t, sub_layer));
        }
        const map_cell_light_record_t *light_record =
            map_cell_light_record((map_cell_t *)cell, sub_layer, false);
        if (light_record != NULL) {
            hash = map_cell_hash_bytes(
                hash,
                (const uint8_t *)light_record + offsetof(map_cell_light_record_t, sub_layer),
                sizeof(*light_record) - offsetof(map_cell_light_record_t, sub_layer));
        }
    }
    if (cell->light_keyframe != NULL) {
        hash = map_cell_hash_bytes(hash, cell->light_keyframe, sizeof(*cell->light_keyframe));
    }
    return hash;
}

static uint8_t map_cell_light_rgb_bitmap(const map_cell_t *cell) {
    uint8_t bitmap = 0;
    for (const map_cell_light_record_t *record = cell->lights; record != NULL;
         record = record->next) {
        if (record->rgb_explicit) {
            bitmap |= UINT8_C(1) << record->sub_layer;
        }
    }
    return bitmap;
}

static void map_cell_release_contents(map_cell_t *cell) {
    map_cell_layer_record_t *layer = cell->layers;
    while (layer != NULL) {
        map_cell_layer_record_t *next = layer->next;
        map_cell_record_release(layer, sizeof(*layer));
        layer = next;
    }
    map_cell_sublayer_record_t *sub_layer = cell->sub_layers;
    while (sub_layer != NULL) {
        map_cell_sublayer_record_t *next = sub_layer->next;
        map_cell_record_release(sub_layer, sizeof(*sub_layer));
        sub_layer = next;
    }
    map_cell_actor_record_t *actor = cell->actors;
    while (actor != NULL) {
        map_cell_actor_record_t *next = actor->next;
        map_cell_record_release(actor, sizeof(*actor));
        actor = next;
    }
    map_cell_light_record_t *light = cell->lights;
    while (light != NULL) {
        map_cell_light_record_t *next = light->next;
        map_cell_record_release(light, sizeof(*light));
        light = next;
    }
    map_cell_record_release(cell->light_keyframe, sizeof(*cell->light_keyframe));
    cell->layers = NULL;
    cell->sub_layers = NULL;
    cell->actors = NULL;
    cell->lights = NULL;
    cell->light_keyframe = NULL;
}

static void map_cell_release(map_cell_t *cell) {
    if (cell == NULL) {
        return;
    }
    map_cell_release_contents(cell);
    map_cell_record_release(cell, sizeof(*cell));
}

static map_cell_t *map_cell_clone(const map_cell_t *source) {
    if (source == NULL) {
        return NULL;
    }
    map_cell_t *clone = map_cell_record_allocate(sizeof(*clone));
    clone->painter_identity = source->painter_identity;
    clone->fow = source->fow;
    clone->structural_fow = source->structural_fow;
    clone->stretch_dirty = source->stretch_dirty;
    clone->stretch_top_height = source->stretch_top_height;
    clone->stretch_upper_height = source->stretch_upper_height;
    clone->level_support_height = source->level_support_height;
    clone->render_max_height = source->render_max_height;

    for (const map_cell_layer_record_t *record = source->layers; record != NULL;
         record = record->next) {
        map_cell_layer_record_t *copy = map_cell_record_allocate(sizeof(*copy));
        *copy = *record;
        copy->next = clone->layers;
        clone->layers = copy;
    }
    for (const map_cell_sublayer_record_t *record = source->sub_layers; record != NULL;
         record = record->next) {
        map_cell_sublayer_record_t *copy = map_cell_record_allocate(sizeof(*copy));
        *copy = *record;
        copy->next = clone->sub_layers;
        clone->sub_layers = copy;
    }
    for (const map_cell_actor_record_t *record = source->actors; record != NULL;
         record = record->next) {
        map_cell_actor_record_t *copy = map_cell_record_allocate(sizeof(*copy));
        *copy = *record;
        copy->next = clone->actors;
        clone->actors = copy;
    }
    for (const map_cell_light_record_t *record = source->lights; record != NULL;
         record = record->next) {
        map_cell_light_record_t *copy = map_cell_record_allocate(sizeof(*copy));
        *copy = *record;
        copy->next = clone->lights;
        clone->lights = copy;
    }
    if (source->light_keyframe != NULL) {
        clone->light_keyframe = map_cell_record_allocate(sizeof(*clone->light_keyframe));
        *clone->light_keyframe = *source->light_keyframe;
    }
    return clone;
}

/** Clear live presentation while retaining remembered static geometry. */
static void map_cell_clear_live_state(map_cell_t *cell) {
    HARD_ASSERT(cell != NULL);
    uint8_t remembered_layer_mask = 0;
    for (int object_layer = 1; object_layer <= NUM_LAYERS; object_layer++) {
        if (map_layer_is_remembered((uint8_t)object_layer)) {
            remembered_layer_mask |= UINT8_C(1) << (object_layer - 1);
        }
    }
    uint8_t live_layer_mask = (uint8_t)~remembered_layer_mask;

    map_cell_sublayer_record_t **sub_link = &cell->sub_layers;
    while (*sub_link != NULL) {
        map_cell_sublayer_record_t *record = *sub_link;
        record->door &= (uint8_t)~live_layer_mask;
        record->exit &= (uint8_t)~live_layer_mask;
        record->priority &= (uint8_t)~live_layer_mask;
        record->secondpass &= (uint8_t)~live_layer_mask;
        if (record->door == 0 && record->exit == 0 && record->priority == 0 &&
            record->secondpass == 0 && record->stretch == 0) {
            *sub_link = record->next;
            map_cell_record_release(record, sizeof(*record));
        } else {
            sub_link = &record->next;
        }
    }

    map_cell_actor_record_t *actor = cell->actors;
    while (actor != NULL) {
        map_cell_actor_record_t *next = actor->next;
        map_cell_record_release(actor, sizeof(*actor));
        actor = next;
    }
    cell->actors = NULL;

    map_cell_layer_record_t **layer_link = &cell->layers;
    while (*layer_link != NULL) {
        map_cell_layer_record_t *record = *layer_link;
        int object_layer = record->layer % NUM_LAYERS + 1;
        if (map_layer_is_remembered((uint8_t)object_layer) ||
            (record->visibility.initialized && record->visibility.alpha != 0)) {
            layer_link = &record->next;
            continue;
        }
        *layer_link = record->next;
        map_cell_record_release(record, sizeof(*record));
    }
}

/** Discard presentation light knowledge while retaining scalar cache values. */
static void map_cell_clear_light_state(map_cell_t *cell) {
    HARD_ASSERT(cell != NULL);
    map_cell_light_record_t **link = &cell->lights;
    while (*link != NULL) {
        map_cell_light_record_t *record = *link;
        record->known = 0;
        memset(record->rgb_radiance, 0, sizeof(record->rgb_radiance));
        record->rgb_explicit = 0;
        record->next_radiance = 0;
        record->next_known = 0;
        memset(record->next_rgb_radiance, 0, sizeof(record->next_rgb_radiance));
        record->next_rgb_explicit = 0;
        if (record->radiance == 0) {
            *link = record->next;
            map_cell_record_release(record, sizeof(*record));
        } else {
            link = &record->next;
        }
    }
    map_cell_record_release(cell->light_keyframe, sizeof(*cell->light_keyframe));
    cell->light_keyframe = NULL;
}

#ifdef ATRINIK_WIDGET_TESTS
bool widget_map_sparse_state_test(void) {
    const size_t cache_side = (MAP_MAX_SIZE + MAP_RENDER_OVERSCAN * 2U) * MAP_FOW_SIZE;
    const size_t slots = cache_side * cache_side;
    map_cell_store_t *stores[MAP2_LEVELS] = {0};
    uint64_t saved_allocation_count = map_cell_allocation_count;
    uint64_t saved_allocation_bytes = map_cell_allocation_bytes;
    uint64_t saved_retained_bytes = map_cell_retained_bytes;
    uint64_t saved_peak_retained_bytes = map_cell_peak_retained_bytes;

    bool success = true;
    for (size_t level = 0; level < MAP2_LEVELS; level++) {
        stores[level] = map_cell_store_create(slots);
    }
    uint64_t empty_bytes = map_cell_retained_bytes - saved_retained_bytes;
    uint64_t expected_empty_bytes =
        MAP2_LEVELS *
        (sizeof(map_cell_store_t) + slots * (sizeof(map_cell_t *) + sizeof(map_cell_header_t)));
    success = empty_bytes == expected_empty_bytes && empty_bytes < 64U * 1024U * 1024U;

    const size_t logical_cells = MAP_MAX_SIZE * MAP_MAX_SIZE;
    size_t populated_cells = 0;
    size_t layer_records = 0;
    size_t sublayer_records = 0;
    size_t actor_records = 0;
    size_t light_records = 0;
    size_t keyframe_records = 0;
    for (size_t level = 0; level < MAP2_LEVELS && success; level++) {
        for (size_t tile = 0; tile < logical_cells; tile++) {
            map_cell_t *record = map_cell_store_slot(stores[level], tile, true);
            success = record != NULL;
            if (record == NULL) {
                break;
            }
            populated_cells++;
            map_cell_layer_record(record, GET_MAP_LAYER(LAYER_FLOOR, 0), true)->face = 1;
            layer_records++;
            if (tile % 4U == 0) {
                map_cell_layer_record(record, GET_MAP_LAYER(LAYER_WALL, 0), true)->face = 2;
                layer_records++;
            }
            if (tile < 10U) {
                map_cell_layer_record(record, GET_MAP_LAYER(LAYER_EFFECT, 0), true)->face = 3;
                layer_records++;
            }
            if (level == MAP2_DEPTH_INDEX(0) && tile < 64U) {
                map_cell_layer_record(record, GET_MAP_LAYER(LAYER_LIVING, 0), true)->face = 4;
                map_cell_actor_record_t *actor = map_cell_actor_record(record, 0, true);
                actor->target_object_count = (uint32_t)tile + 1U;
                snprintf(VS(actor->name), "actor-%" PRIu64, (uint64_t)tile);
                layer_records++;
                actor_records++;
            }
            if (tile % 16U == 0) {
                map_cell_sublayer_record(record, 0, true)->door = UINT8_C(1) << (LAYER_WALL - 1);
                sublayer_records++;
            }
            map_cell_light_record_t *light = map_cell_light_record(record, 0, true);
            light->radiance = 128;
            light->known = 1;
            light_records++;
            if (tile % 32U == 0) {
                map_cell_light_keyframe_record(record, true)->valid = 1;
                keyframe_records++;
            }
        }
    }
    uint64_t populated_bytes = map_cell_retained_bytes - saved_retained_bytes;
    uint64_t expected_populated_bytes = empty_bytes + populated_cells * sizeof(map_cell_t) +
                                        layer_records * sizeof(map_cell_layer_record_t) +
                                        sublayer_records * sizeof(map_cell_sublayer_record_t) +
                                        actor_records * sizeof(map_cell_actor_record_t) +
                                        light_records * sizeof(map_cell_light_record_t) +
                                        keyframe_records * sizeof(map_cell_light_keyframe_record_t);
    success = success && populated_bytes == expected_populated_bytes;

    map_cell_t *clear_record = map_cell_store_slot(stores[MAP2_DEPTH_INDEX(0)], 0, false);
    map_cell_clear_live_state(clear_record);
    map_cell_clear_light_state(clear_record);
    success = success &&
              map_cell_layer_record(clear_record, GET_MAP_LAYER(LAYER_FLOOR, 0), false) != NULL &&
              map_cell_layer_record(clear_record, GET_MAP_LAYER(LAYER_LIVING, 0), false) == NULL &&
              map_cell_layer_record(clear_record, GET_MAP_LAYER(LAYER_EFFECT, 0), false) == NULL &&
              map_cell_actor_record(clear_record, 0, false) == NULL &&
              map_cell_light_record_read(clear_record, 0)->radiance == 128 &&
              !map_cell_light_record_read(clear_record, 0)->known &&
              clear_record->light_keyframe == NULL;
    populated_bytes = map_cell_retained_bytes - saved_retained_bytes;
    expected_populated_bytes -= 2U * sizeof(map_cell_layer_record_t) +
                                sizeof(map_cell_actor_record_t) +
                                sizeof(map_cell_light_keyframe_record_t);
    success = success && populated_bytes == expected_populated_bytes;

    for (size_t level = 0; level < MAP2_LEVELS && success; level++) {
        size_t index = logical_cells + level;
        map_cell_store_set_fow(stores[level], index, true, false);
        bool support_changed = map_cell_store_set_support_height(stores[level], index, 17);
        support_changed &= map_cell_store_set_support_height(stores[level], index, 0);
        success = map_cell_store_slot(stores[level], index, false) == NULL &&
                  stores[level]->headers[index].fow != 0 &&
                  stores[level]->headers[index].support_height == 0 && support_changed;
    }
    success = success && map_cell_retained_bytes - saved_retained_bytes == populated_bytes;

    size_t trim_index = logical_cells + MAP2_LEVELS + 1U;
    uint64_t before_trim = map_cell_retained_bytes;
    map_cell_t *trim_owner = map_cell_store_slot(stores[MAP2_DEPTH_INDEX(0)], trim_index, true);
    map_cell_layer_record_t *trim_layer =
        map_cell_layer_record(trim_owner, GET_MAP_LAYER(LAYER_EFFECT, 0), true);
    success = success && trim_layer != NULL && map_cell_retained_bytes > before_trim;
    map_cell_layer_record_remove(trim_owner, GET_MAP_LAYER(LAYER_EFFECT, 0));
    map_cell_store_trim_slot(stores[MAP2_DEPTH_INDEX(0)], trim_index);
    success = success && map_cell_retained_bytes == before_trim &&
              stores[MAP2_DEPTH_INDEX(0)]->headers[trim_index].occupancy == 0;

    for (size_t level = 0; level < MAP2_LEVELS; level++) {
        map_cell_store_destroy(stores[level]);
    }
    success = success && map_cell_retained_bytes == saved_retained_bytes;
    map_cell_allocation_count = saved_allocation_count;
    map_cell_allocation_bytes = saved_allocation_bytes;
    map_cell_retained_bytes = saved_retained_bytes;
    map_cell_peak_retained_bytes = saved_peak_retained_bytes;
    return success;
}

bool widget_map_transaction_abort_test(void) {
    for (size_t level = 0; level < arraysize(level_cells); level++) {
        if (level_cells[level] != NULL) {
            return false;
        }
    }

    map_width = 3;
    map_height = 3;
    map_cache_origin_x = 0;
    map_cache_origin_y = 0;
    current_level_index = MAP2_DEPTH_INDEX(0);
    map_level_mask = UINT16_C(1) << current_level_index;
    if (!map_select_level(0, true)) {
        return false;
    }
    uint64_t initial_retained_bytes = map_cell_retained_bytes;
    bool success = true;

    map_cell_snapshot_t empty_snapshot;
    map_cell_snapshot(0, 0, &empty_snapshot);
    size_t middle_index = (size_t)MAP_STARTY * (map_width * MAP_FOW_SIZE) + MAP_STARTX;
    map_cell_t *empty_owner = map_cell_store_slot(cells, middle_index, true);
    success = success && !map_cell_changed(0, 0, &empty_snapshot);
    map_cell_snapshot(0, 0, &empty_snapshot);
    map_cell_layer_record(empty_owner, GET_MAP_LAYER(LAYER_FLOOR, 0), true)->face = 1;
    success = success && map_cell_changed(0, 0, &empty_snapshot);
    map_cell_store_clear_slot(cells, middle_index);
    success = success && map_cell_retained_bytes == initial_retained_bytes;

    map_set_structural_support_height(0, 0, 37);
    success = success && map_cell_store_slot(cells, middle_index, false) == NULL &&
              map_level_support_height(MAP_STARTX, MAP_STARTY, 1) == 37 &&
              map_cell_retained_bytes == initial_retained_bytes;
    map_state_transaction_begin(false);
    map_clear_cell(0, 0, true);
    map_cell_t *replacement = map_cell_store_slot(cells, middle_index, true);
    map_cell_layer_record(replacement, GET_MAP_LAYER(LAYER_EFFECT, 0), true)->face = 2;
    success = success && map_level_support_height(MAP_STARTX, MAP_STARTY, 1) == 0;
    map_state_transaction_abort();
    success = success && map_cell_store_slot(cells, middle_index, false) == NULL &&
              map_level_support_height(MAP_STARTX, MAP_STARTY, 1) == 37 &&
              map_cell_retained_bytes == initial_retained_bytes;
    map_set_structural_support_height(0, 0, 0);

    map_mark_stretch_dirty(1, 1);
    adjust_tile_stretch();
    success = success && map_cell_retained_bytes == initial_retained_bytes;

    for (size_t attempt = 0; attempt < 2; attempt++) {
        map_state_transaction_begin(false);
        map_set_fow(0, 0, true);
        map_mark_stretch_dirty(1, 1);
        map_state_transaction_abort();
        success =
            success && !map_get_fow(0, 0) && map_cell_retained_bytes == initial_retained_bytes;
    }

    map_cell_store_destroy(level_cells[current_level_index]);
    level_cells[current_level_index] = NULL;
    cells = NULL;
    map_width = 0;
    map_height = 0;
    map_level_mask = 0;
    return success;
}

bool widget_map_light_keyframe_capacity_test(void) {
    int saved_width = map_width;
    int saved_height = map_height;
    const int wire_ceiling = 32;
    uint16_t scalar[NUM_SUB_LAYERS] = {0};
    uint16_t rgb[NUM_SUB_LAYERS][3] = {{0}};

    map_width = wire_ceiling;
    map_height = wire_ceiling;
    bool success = map_light_keyframe_transaction_begin(1, 1, 2, MAP2_LIGHT_KEYFRAME_SNAP);
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH && success; depth++) {
        for (int x = 0; x < map_width && success; x++) {
            for (int y = 0; y < map_height; y++) {
                success = map_light_keyframe_transaction_stage(depth, x, y, 0, scalar, 0, rgb);
                if (!success) {
                    break;
                }
            }
        }
    }
    success = success && !map_light_keyframe_transaction_stage(0, 0, 0, 0, scalar, 0, rgb);
    map_light_keyframe_transaction_abort();
    map_width = saved_width;
    map_height = saved_height;
    return success;
}

#endif
static map_cell_store_t *map_cell_store_create(size_t count) {
    map_cell_store_t *store = xcalloc(1, sizeof(*store));
    store->slots = xcalloc(count, sizeof(*store->slots));
    store->headers = xcalloc(count, sizeof(*store->headers));
    store->count = count;
    size_t bytes = sizeof(*store) + count * (sizeof(*store->slots) + sizeof(*store->headers));
    map_cell_allocation_count += 3;
    map_cell_allocation_bytes += bytes;
    map_cell_retained_bytes += bytes;
    map_cell_peak_retained_bytes = MAX(map_cell_peak_retained_bytes, map_cell_retained_bytes);
    return store;
}

static void map_cell_store_destroy(map_cell_store_t *store) {
    if (store == NULL) {
        return;
    }
    for (size_t i = 0; i < store->count; i++) {
        map_cell_release(store->slots[i]);
    }
    map_cell_retained_bytes -=
        sizeof(*store) + store->count * (sizeof(*store->slots) + sizeof(*store->headers));
    free(store->headers);
    free(store->slots);
    free(store);
}

static map_cell_store_t *map_cell_store_clone(const map_cell_store_t *store) {
    if (store == NULL) {
        return NULL;
    }
    map_cell_store_t *clone = map_cell_store_create(store->count);
    memcpy(clone->headers, store->headers, store->count * sizeof(*store->headers));
    for (size_t i = 0; i < store->count; i++) {
        clone->slots[i] = map_cell_clone(store->slots[i]);
    }
    return clone;
}

static map_cell_t *map_cell_store_slot(map_cell_store_t *store, size_t index, bool create) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(index < store->count);
    if (store->slots[index] == NULL && create) {
        store->slots[index] = map_cell_record_allocate(sizeof(*store->slots[index]));
        store->slots[index]->painter_identity = map_cell_next_painter_identity++;
        if (map_cell_next_painter_identity == 0) {
            map_cell_next_painter_identity = 1;
        }
        store->slots[index]->fow = store->headers[index].fow;
        store->slots[index]->structural_fow = store->headers[index].structural_fow;
        store->headers[index].occupancy = 1;
        store->headers[index].generation++;
        store->headers[index].revision++;
    }
    return store->slots[index];
}

static void map_cell_store_clear_slot(map_cell_store_t *store, size_t index) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(index < store->count);
    if (store->slots[index] != NULL) {
        map_cell_release(store->slots[index]);
        store->slots[index] = NULL;
        store->headers[index].occupancy = 0;
        store->headers[index].revision++;
    }
}

static bool map_cell_sparse_empty(const map_cell_t *cell) {
    return cell != NULL && cell->layers == NULL && cell->sub_layers == NULL &&
           cell->actors == NULL && cell->lights == NULL && cell->light_keyframe == NULL &&
           cell->stretch_dirty == 0 && cell->stretch_top_height == 0 &&
           cell->stretch_upper_height == 0 && cell->level_support_height == 0 &&
           cell->render_max_height == 0;
}

static void map_cell_store_trim_slot(map_cell_store_t *store, size_t index) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(index < store->count);
    if (map_cell_sparse_empty(store->slots[index])) {
        map_cell_store_clear_slot(store, index);
    }
}

static bool
map_cell_store_set_support_height(map_cell_store_t *store, size_t index, int16_t height) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(index < store->count);
    map_cell_header_t *header = &store->headers[index];
    if (header->support_height == height) {
        return false;
    }
    header->support_height = height;
    header->generation++;
    header->revision++;
    return true;
}

static void
map_cell_store_set_fow(map_cell_store_t *store, size_t index, bool fow, bool structural_fow) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(index < store->count);
    map_cell_header_t *header = &store->headers[index];
    if ((header->fow != 0) == fow && (header->structural_fow != 0) == structural_fow) {
        return;
    }
    header->fow = fow;
    header->structural_fow = structural_fow;
    header->generation++;
    header->revision++;
    if (store->slots[index] != NULL) {
        store->slots[index]->fow = fow;
        store->slots[index]->structural_fow = structural_fow;
    }
}

/** Shared immutable-by-convention value for an absent sparse cell. */
static map_cell_t map_empty_cell;
static map_cell_t map_fow_cell = {.fow = 1};
static map_cell_t map_structural_fow_cell = {.fow = 1, .structural_fow = 1};

/** Resolve one logical slot in the circular fog-of-war cache. */
static map_cell_t *map_cache_cell_at(map_cell_store_t *level,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     int origin_x,
                                     int origin_y,
                                     bool create) {
    HARD_ASSERT(level != NULL);
    HARD_ASSERT(x >= 0 && x < width);
    HARD_ASSERT(y >= 0 && y < height);

    int physical_x = (x + origin_x) % width;
    int physical_y = (y + origin_y) % height;
    size_t index = (size_t)physical_y * width + physical_x;
    bool existed = level->slots[index] != NULL;
    map_cell_t *cell = map_cell_store_slot(level, index, create);
    if (cell != NULL) {
        if (create && existed) {
            level->headers[index].generation++;
            level->headers[index].revision++;
        }
        return cell;
    }
    if (level->headers[index].structural_fow) {
        return &map_structural_fow_cell;
    }
    return level->headers[index].fow ? &map_fow_cell : &map_empty_cell;
}

static map_cell_t *map_cache_cell(map_cell_store_t *level, int x, int y) {
    return map_cache_cell_at(level,
                             x,
                             y,
                             map_width * MAP_FOW_SIZE,
                             map_height * MAP_FOW_SIZE,
                             map_cache_origin_x,
                             map_cache_origin_y,
                             false);
}

static map_cell_t *map_cache_cell_mutable(map_cell_store_t *level, int x, int y) {
    return map_cache_cell_at(level,
                             x,
                             y,
                             map_width * MAP_FOW_SIZE,
                             map_height * MAP_FOW_SIZE,
                             map_cache_origin_x,
                             map_cache_origin_y,
                             true);
}

static size_t map_cache_physical_index(int x, int y) {
    int width = map_width * MAP_FOW_SIZE;
    int height = map_height * MAP_FOW_SIZE;
    HARD_ASSERT(x >= 0 && x < width);
    HARD_ASSERT(y >= 0 && y < height);
    int physical_x = (x + map_cache_origin_x) % width;
    int physical_y = (y + map_cache_origin_y) % height;
    return (size_t)physical_y * (size_t)width + (size_t)physical_x;
}

static void map_clear_live_cell(map_cell_t *cell);
static void map_state_transaction_record_physical(size_t level, int x, int y);
static void map_state_transaction_record_cell(int x, int y);
static bool map_state_transaction_should_defer_level_free(void);
static void map_dirty_lighting_fow(int dx, int dy);

/** Mark the clipped logical rectangle as explored fog. */
static void map_cache_mark_fow(map_cell_store_t *level,
                               size_t level_index,
                               int x_start,
                               int x_end,
                               int y_start,
                               int y_end,
                               int width,
                               int height) {
    x_start = MAX(0, x_start);
    x_end = MIN(width, x_end);
    y_start = MAX(0, y_start);
    y_end = MIN(height, y_end);

    for (int x = x_start; x < x_end; x++) {
        for (int y = y_start; y < y_end; y++) {
            map_state_transaction_record_physical(level_index, x, y);
            int physical_x = (x + map_cache_origin_x) % width;
            int physical_y = (y + map_cache_origin_y) % height;
            size_t index = (size_t)physical_y * width + physical_x;
            map_cell_t *cell = map_cell_store_slot(level, index, false);
            if (cell != NULL && (!cell->fow || cell->structural_fow)) {
                map_clear_live_cell(cell);
                map_cell_clear_light_state(cell);
            }
            map_cell_store_set_fow(level, index, true, false);
            map_cell_store_trim_slot(level, index);
        }
    }
}

#define MAP_CELL_GET(_x, _y) map_cache_cell(cells, (_x), (_y))
#define MAP_CELL_GET_MIDDLE(_x, _y) MAP_CELL_GET((_x) + MAP_STARTX, (_y) + MAP_STARTY)
#define MAP_CELL_GET_MUTABLE(_x, _y) map_cache_cell_mutable(cells, (_x), (_y))
#define MAP_CELL_GET_MIDDLE_MUTABLE(_x, _y) \
    MAP_CELL_GET_MUTABLE((_x) + MAP_STARTX, (_y) + MAP_STARTY)

/** Vertical screen projection of one linked physical map level. */
#define MAP_LEVEL_PIXEL_HEIGHT 46

/** Radius of the nearest-known light sample search around an unseen cell. */
#define MAP_LIGHTING_FOW_SEARCH_RADIUS 6
/** Keep FOW invalidation rectangles small enough for diagonal projections. */
#define MAP_LIGHTING_FOW_SEGMENT 4
/** Cover the interpolated tile and the current player-height offset. */
#define MAP_LIGHTING_FOW_MARGIN 8

/** Primary-map outlines reveal silhouettes without exposing interiors. */
#define DOOR_HINT_RADIUS 3
#define MAP_OUTLINE_COLOR "ffc64a"
#define MAP_LIVING_OUTLINE_COLOR "53d8fb"

/** Select one protocol map depth, allocating its cache on demand. */
bool map_select_level(int depth, bool create) {
    if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH) {
        return false;
    }

    size_t index = (size_t)MAP2_DEPTH_INDEX(depth);
    if (level_cells[index] == NULL && create) {
        size_t count = (size_t)map_width * MAP_FOW_SIZE * (size_t)map_height * MAP_FOW_SIZE;
        level_cells[index] = map_cell_store_create(count);
    }

    current_level_index = index;
    cells = level_cells[index];
    return cells != NULL;
}

void map_set_level_mask(uint16_t mask) {
    map_level_mask = mask;

    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        size_t index = (size_t)MAP2_DEPTH_INDEX(depth);

        uint16_t bit = UINT16_C(1) << index;
        if (!(mask & bit) && level_cells[index] != NULL) {
            if (map_state_transaction_should_defer_level_free()) {
                level_lighting_revision[index]++;
                continue;
            }
            map_cell_store_destroy(level_cells[index]);
            level_cells[index] = NULL;
            level_lighting_revision[index]++;
        }
    }

    lighting_set_level_mask(mask);
    map_select_level(0, true);
}

unsigned int map_active_level_count(void) {
    unsigned int count = 0;
    for (unsigned int index = 0; index < MAP2_LEVELS; index++) {
        count += (map_level_mask & (UINT16_C(1) << index)) != 0;
    }
    return count;
}
/**
 * Map animation queue.
 */
static map_anim_t *first_anim = NULL;

/**
 * Current shown map: mapname, length, etc
 */
_mapdata MapData;

/**
 * Multi-part object data.
 */
_multi_part_obj MultiArchs[16];

/** Snapshot of the bounded cache and map metadata for one MAP2 packet. */
static struct {
    bool active;
    bool full_snapshot;
    int width;
    int height;
    int origin_x;
    int origin_y;
    size_t current_level_index;
    uint16_t level_mask;
    uint64_t lighting_revision[MAP2_LEVELS];
    map_cell_store_t *original_levels[MAP2_LEVELS];
    map_cell_store_t *full_snapshot_levels[MAP2_LEVELS];
    map_anim_t *animations;
    _mapdata map_data;
    uint32_t *region_fow_bitmap;
    size_t region_fow_bitmap_size;
    region_map_fow_tile_t *region_fow_tiles;
    size_t region_fow_tile_count;
    struct {
        size_t level;
        size_t index;
        bool existed;
        map_cell_header_t header;
        map_cell_t *value;
    } *cells;
    size_t cells_count;
    size_t cells_capacity;
} map_state_transaction;

static bool map_state_transaction_should_defer_level_free(void) {
    return map_state_transaction.active && !map_state_transaction.full_snapshot;
}

static void map_state_transaction_release_snapshot(void) {
    for (size_t i = 0; i < arraysize(map_state_transaction.full_snapshot_levels); i++) {
        map_cell_store_destroy(map_state_transaction.full_snapshot_levels[i]);
        map_state_transaction.full_snapshot_levels[i] = NULL;
    }
    map_anim_t *animation = map_state_transaction.animations;
    while (animation != NULL) {
        map_anim_t *next = animation->next;
        free(animation);
        animation = next;
    }
    map_state_transaction.animations = NULL;
    free(map_state_transaction.region_fow_bitmap);
    map_state_transaction.region_fow_bitmap = NULL;
    map_state_transaction.region_fow_bitmap_size = 0;
    for (size_t i = 0; i < map_state_transaction.region_fow_tile_count; i++) {
        free(map_state_transaction.region_fow_tiles[i].path);
    }
    free(map_state_transaction.region_fow_tiles);
    map_state_transaction.region_fow_tiles = NULL;
    map_state_transaction.region_fow_tile_count = 0;
    for (size_t i = 0; i < map_state_transaction.cells_count; i++) {
        map_cell_release(map_state_transaction.cells[i].value);
    }
    free(map_state_transaction.cells);
    map_state_transaction.cells = NULL;
    map_state_transaction.cells_count = 0;
    map_state_transaction.cells_capacity = 0;
}

static map_anim_t *map_anims_clone(void) {
    map_anim_t *clone_head = NULL;
    map_anim_t *clone_tail = NULL;

    for (map_anim_t *animation = first_anim; animation != NULL; animation = animation->next) {
        map_anim_t *clone = xmalloc(sizeof(*clone));
        *clone = *animation;
        clone->prev = clone_tail;
        clone->next = NULL;
        if (clone_tail == NULL) {
            clone_head = clone;
        } else {
            clone_tail->next = clone;
        }
        clone_tail = clone;
    }

    return clone_head;
}

static void map_state_transaction_snapshot_region_fow(void) {
    region_map_t *region_map = MapData.region_map;
    if (region_map == NULL || region_map->fow == NULL) {
        return;
    }

    if (region_map->fow->bitmap != NULL) {
        map_state_transaction.region_fow_bitmap_size = RM_MAP_FOW_BITMAP_SIZE(region_map);
        map_state_transaction.region_fow_bitmap =
            xmalloc(map_state_transaction.region_fow_bitmap_size);
        memcpy(map_state_transaction.region_fow_bitmap,
               region_map->fow->bitmap,
               map_state_transaction.region_fow_bitmap_size);
    }

    if (region_map->fow->tiles == NULL) {
        return;
    }

    map_state_transaction.region_fow_tile_count = utarray_len(region_map->fow->tiles);
    if (map_state_transaction.region_fow_tile_count == 0) {
        return;
    }

    map_state_transaction.region_fow_tiles =
        xcalloc(map_state_transaction.region_fow_tile_count,
                sizeof(*map_state_transaction.region_fow_tiles));
    for (size_t i = 0; i < map_state_transaction.region_fow_tile_count; i++) {
        region_map_fow_tile_t *source =
            (region_map_fow_tile_t *)utarray_eltptr(region_map->fow->tiles, i);
        map_state_transaction.region_fow_tiles[i] = *source;
        map_state_transaction.region_fow_tiles[i].path = xstrdup(source->path);
    }
}

static void map_state_transaction_restore_region_fow(void) {
    region_map_t *region_map = MapData.region_map;
    if (region_map == NULL || region_map->fow == NULL) {
        return;
    }

    if (map_state_transaction.region_fow_bitmap != NULL && region_map->fow->bitmap != NULL) {
        memcpy(region_map->fow->bitmap,
               map_state_transaction.region_fow_bitmap,
               map_state_transaction.region_fow_bitmap_size);
    }

    if (region_map->fow->tiles == NULL) {
        return;
    }

    for (size_t i = 0; i < utarray_len(region_map->fow->tiles); i++) {
        region_map_fow_tile_t *tile =
            (region_map_fow_tile_t *)utarray_eltptr(region_map->fow->tiles, i);
        free(tile->path);
    }
    utarray_clear(region_map->fow->tiles);
    for (size_t i = 0; i < map_state_transaction.region_fow_tile_count; i++) {
        utarray_push_back(region_map->fow->tiles, &map_state_transaction.region_fow_tiles[i]);
        map_state_transaction.region_fow_tiles[i].path = NULL;
    }
}

void map_state_transaction_begin(bool full_snapshot) {
    HARD_ASSERT(!map_state_transaction.active);

    map_state_transaction.full_snapshot = full_snapshot;
    map_state_transaction.width = map_width;
    map_state_transaction.height = map_height;
    map_state_transaction.origin_x = map_cache_origin_x;
    map_state_transaction.origin_y = map_cache_origin_y;
    map_state_transaction.current_level_index = current_level_index;
    map_state_transaction.level_mask = map_level_mask;
    map_state_transaction.map_data = MapData;
    map_state_transaction.animations = map_anims_clone();
    map_state_transaction_snapshot_region_fow();
    memcpy(map_state_transaction.original_levels, level_cells, sizeof(level_cells));
    memcpy(map_state_transaction.lighting_revision,
           level_lighting_revision,
           sizeof(level_lighting_revision));

    if (full_snapshot) {
        for (size_t i = 0; i < arraysize(level_cells); i++) {
            if (level_cells[i] == NULL) {
                continue;
            }
            map_state_transaction.full_snapshot_levels[i] = map_cell_store_clone(level_cells[i]);
        }
    }

    map_state_transaction.active = true;
}

void map_state_transaction_commit(void) {
    if (!map_state_transaction.active) {
        return;
    }

    if (!map_state_transaction.full_snapshot) {
        for (size_t i = 0; i < arraysize(level_cells); i++) {
            if (!(map_level_mask & (UINT16_C(1) << i))) {
                map_cell_store_destroy(level_cells[i]);
                level_cells[i] = NULL;
            }
        }
    }

    bool region_changed =
        strcmp(map_state_transaction.map_data.region_name, MapData.region_name) != 0;
    map_state_transaction_release_snapshot();
    map_state_transaction.active = false;
    if (region_changed) {
        region_map_update(MapData.region_map, MapData.region_name);
    }
}

void map_state_transaction_abort(void) {
    if (!map_state_transaction.active) {
        return;
    }

    if (map_state_transaction.full_snapshot) {
        for (size_t i = 0; i < arraysize(level_cells); i++) {
            map_cell_store_destroy(level_cells[i]);
            level_cells[i] = map_state_transaction.full_snapshot_levels[i];
            map_state_transaction.full_snapshot_levels[i] = NULL;
        }
    } else {
        for (size_t i = 0; i < map_state_transaction.cells_count; i++) {
            size_t level = map_state_transaction.cells[i].level;
            size_t index = map_state_transaction.cells[i].index;
            if (level_cells[level] != NULL) {
                if (map_state_transaction.cells[i].existed) {
                    map_cell_release(level_cells[level]->slots[index]);
                    level_cells[level]->slots[index] =
                        map_cell_clone(map_state_transaction.cells[i].value);
                } else {
                    map_cell_store_clear_slot(level_cells[level], index);
                }
                level_cells[level]->headers[index] = map_state_transaction.cells[i].header;
            }
        }
        for (size_t i = 0; i < arraysize(level_cells); i++) {
            if (level_cells[i] != map_state_transaction.original_levels[i]) {
                map_cell_store_destroy(level_cells[i]);
                level_cells[i] = map_state_transaction.original_levels[i];
            }
        }
    }

    map_width = map_state_transaction.width;
    map_height = map_state_transaction.height;
    map_cache_origin_x = map_state_transaction.origin_x;
    map_cache_origin_y = map_state_transaction.origin_y;
    current_level_index = map_state_transaction.current_level_index;
    map_level_mask = map_state_transaction.level_mask;
    memcpy(level_lighting_revision,
           map_state_transaction.lighting_revision,
           sizeof(level_lighting_revision));
    MapData = map_state_transaction.map_data;
    map_state_transaction_restore_region_fow();
    cells = level_cells[current_level_index];
    lighting_set_level_mask(map_level_mask);
    map_anims_clear();
    first_anim = map_state_transaction.animations;
    map_state_transaction.animations = NULL;

    map_state_transaction_release_snapshot();
    map_state_transaction.active = false;
}

/** Journal one cache cell before a normal MAP2 transaction mutates it. */
static void map_state_transaction_record_physical(size_t level, int x, int y) {
    if (!map_state_transaction.active || map_state_transaction.full_snapshot ||
        level >= arraysize(level_cells)) {
        return;
    }

    int width = map_width * MAP_FOW_SIZE;
    int height = map_height * MAP_FOW_SIZE;
    if (level_cells[level] == NULL || width <= 0 || height <= 0 || x < 0 || x >= width || y < 0 ||
        y >= height) {
        return;
    }

    int physical_x = (x + map_cache_origin_x) % width;
    int physical_y = (y + map_cache_origin_y) % height;
    size_t index = (size_t)physical_y * (size_t)width + (size_t)physical_x;
    for (size_t i = 0; i < map_state_transaction.cells_count; i++) {
        if (map_state_transaction.cells[i].level == level &&
            map_state_transaction.cells[i].index == index) {
            return;
        }
    }

    if (map_state_transaction.cells_count == map_state_transaction.cells_capacity) {
        map_state_transaction.cells_capacity = map_state_transaction.cells_capacity == 0
                                                   ? 64
                                                   : map_state_transaction.cells_capacity * 2;
        map_state_transaction.cells = xreallocarray(map_state_transaction.cells,
                                                    map_state_transaction.cells_capacity,
                                                    sizeof(*map_state_transaction.cells));
    }
    size_t snapshot = map_state_transaction.cells_count++;
    map_state_transaction.cells[snapshot].level = level;
    map_state_transaction.cells[snapshot].index = index;
    map_state_transaction.cells[snapshot].header = level_cells[level]->headers[index];
    map_cell_t *cell = map_cell_store_slot(level_cells[level], index, false);
    map_state_transaction.cells[snapshot].existed = cell != NULL;
    map_state_transaction.cells[snapshot].value = map_cell_clone(cell);
}

static void map_state_transaction_record_cell(int x, int y) {
    map_state_transaction_record_physical(current_level_index, x + MAP_STARTX, y + MAP_STARTY);
}

/**
 * Holds coordinates of the last map square the mouse was over.
 */
static int old_map_mouse_x = -1, old_map_mouse_y = -1;
/**
 * If true, show the mouse map square indicator.
 */
static bool map_show_mouse = false;
/**
 * When the right button was pressed on the map widget. -1 = not
 * pressed.
 */
static int right_click_ticks = -1;

/**
 * If true, will print tile coordinates.
 */
static bool tiles_debug = false;

#ifdef ATRINIK_WIDGET_TESTS
static bool map_interaction_test_active;
static int map_interaction_test_moves;
static int map_interaction_test_targets;
static int map_interaction_test_talks;
static bool map_ui_test_active;
static int map_ui_test_names;
static int map_ui_test_targets;
static bool map_animation_test_active;
static int map_animation_test_damage_draws;
static int map_animation_test_kill_draws;
static int map_animation_test_elevated_draws;
static int map_animation_test_source_floor_height;
static int map_animation_test_player_floor_height;
static int map_animation_test_layer_content_draws;
static int map_animation_test_expected_depth;
static int map_animation_test_expected_sub_layer;
static SDL_Surface *map_animation_test_death_texture;
#endif

static int get_top_floor_height(map_cell_t *cell, int sub_layer);

/**
 * Description of the --tiles_debug command.
 */
static const char *clioptions_option_tiles_debug_desc =
    "Enable map tiles debugging (shows tile coordinates).";
/** @copydoc clioptions_handler_func */
static bool clioptions_option_tiles_debug(const char *arg, char **errmsg) {
    tiles_debug = true;
    return true;
}

/** Parse the complete fixed-size multipart geometry table. */
static bool load_mapdef_line(const char *line, _multi_part_obj *object) {
    int values[34];
    const char *cursor = line;

    for (size_t i = 0; i < arraysize(values); i++) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            return false;
        }

        errno = 0;
        char *end;
        long value = strtol(cursor, &end, 10);
        int minimum = i < 2 ? 0 : -4096;
        if (errno != 0 || end == cursor || value < minimum || value > 4096) {
            return false;
        }
        values[i] = (int)value;
        cursor = end;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return false;
    }

    object->xlen = values[0];
    object->ylen = values[1];
    for (size_t i = 0; i < arraysize(object->part); i++) {
        object->part[i].xoff = values[2 + i * 2];
        object->part[i].yoff = values[3 + i * 2];
    }
    return true;
}

static bool load_mapdef_stream(FILE *stream) {
    _multi_part_obj parsed[arraysize(MultiArchs)];
    char line[MAX_BUF];
    memset(parsed, 0, sizeof(parsed));

    for (size_t i = 0; i < arraysize(parsed); i++) {
        if (fgets(line, sizeof(line), stream) == NULL) {
            return false;
        }
        if (strchr(line, '\n') == NULL && !feof(stream)) {
            return false;
        }
        char *newline = strchr(line, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        size_t line_length = strlen(line);
        if (line_length != 0 && line[line_length - 1] == '\r') {
            line[line_length - 1] = '\0';
        }
        if (!load_mapdef_line(line, &parsed[i])) {
            return false;
        }
    }

    while (fgets(line, sizeof(line), stream) != NULL) {
        for (const char *cp = line; *cp != '\0'; cp++) {
            if (!isspace((unsigned char)*cp)) {
                return false;
            }
        }
    }
    if (ferror(stream)) {
        return false;
    }

    memcpy(MultiArchs, parsed, sizeof(parsed));
    return true;
}

bool load_mapdef_file(const char *path) {
    HARD_ASSERT(path != NULL);

    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        return false;
    }
    bool success = load_mapdef_stream(stream);
    if (fclose(stream) != 0) {
        success = false;
    }
    return success;
}

void map_cell_snapshot(int x, int y, map_cell_snapshot_t *snapshot) {
    HARD_ASSERT(snapshot != NULL);
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    const map_cell_header_t *header = &cells->headers[index];
    *snapshot = (map_cell_snapshot_t){
        .content_hash = map_cell_content_hash(cells->slots[index]),
        .support_height = header->support_height,
        .fow = header->fow,
        .structural_fow = header->structural_fow,
    };
}

bool map_cell_changed(int x, int y, const map_cell_snapshot_t *snapshot) {
    HARD_ASSERT(snapshot != NULL);
    map_cell_snapshot_t current;
    map_cell_snapshot(x, y, &current);
    return snapshot->content_hash != current.content_hash ||
           snapshot->support_height != current.support_height || snapshot->fow != current.fow ||
           snapshot->structural_fow != current.structural_fow;
}

/**
 * Loads multi-arch object data offsets.
 */
void load_mapdef_dat(void) {

    clioption_t *cli;
    CLIOPTIONS_CREATE(cli, tiles_debug, "Enable map tiles debugging");

    FILE *stream = path_fopen(ARCHDEF_FILE, "r");

    if (stream == NULL) {
        LOG(BUG, "Can't open file %s", ARCHDEF_FILE);
        return;
    }
    if (!load_mapdef_stream(stream)) {
        LOG(BUG, "Invalid multipart geometry in %s", ARCHDEF_FILE);
    }

    fclose(stream);
}

/**
 * Clear the map.
 * @param hard
 * Hard reset
 */
void clear_map(bool hard) {
    map_light_keyframe_transaction_abort();

    /* Cache the map width and height. */
    map_width = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_WIDTH));
    map_height = MAP_LOOK_TO_WIRE_SIZE(setting_get_int(OPT_CAT_MAP, OPT_MAP_HEIGHT));

    for (size_t i = 0; i < arraysize(level_cells); i++) {
        if (level_cells[i] != NULL) {
            for (size_t cell = 0; cell < level_cells[i]->count; cell++) {
                map_cell_store_clear_slot(level_cells[i], cell);
                memset(&level_cells[i]->headers[cell], 0, sizeof(level_cells[i]->headers[cell]));
            }
            level_lighting_revision[i]++;
        }
    }

    map_level_mask = UINT16_C(1) << MAP2_DEPTH_INDEX(0);
    map_protocol_continuation_reset(&MapData.continuation);
    MapData.light_keyframe_generation = 0;
    MapData.light_keyframe_start_seconds = 0;
    MapData.light_keyframe_end_seconds = 0;
    MapData.light_keyframe_flags = 0;
    MapData.light_keyframe_valid = false;
    map_temporal_lighting_bucket = UINT64_MAX;
    map_cache_origin_x = 0;
    map_cache_origin_y = 0;
    map_select_level(0, true);
    sound_ambient_clear();
    map_anims_clear();

    if (hard) {
        region_map_reset(MapData.region_map);
        MapData.region_name[0] = '\0';
        MapData.region_longname[0] = '\0';
        MapData.region_has_map = false;
    }
}

/**
 * Update map size.
 *
 * @param w
 * New width.
 * @param h
 * New height.
 */
void map_update_size(int w, int h) {
    int old_w = map_width;
    int old_h = map_height;

    if (w != 0) {
        map_width = w;
    }

    if (h != 0) {
        map_height = h;
    }

    display_mapscroll((old_w - map_width) * (MAP_FOW_SIZE / 2),
                      (old_h - map_height) * (MAP_FOW_SIZE / 2),
                      old_w * MAP_FOW_SIZE,
                      old_h * MAP_FOW_SIZE);
}

/**
 * Scroll the map.
 * @param dx
 * X offset.
 * @param dy
 * Y offset.
 * @param old_w
 * Old width. 0 if width hasn't changed.
 * @param old_h
 * Old height. 0 if height hasn't changed.
 */
void display_mapscroll(int dx, int dy, int old_w, int old_h) {
    map_redraw_request(MAP_REDRAW_REASON_SCROLL);
    int width = map_width * MAP_FOW_SIZE;
    int height = map_height * MAP_FOW_SIZE;

    if (old_w != 0 && old_h != 0 && (old_w != width || old_h != height)) {
        int old_origin_x = map_cache_origin_x;
        int old_origin_y = map_cache_origin_y;

        for (size_t level = 0; level < arraysize(level_cells); level++) {
            if (level_cells[level] == NULL) {
                continue;
            }

            map_cell_store_t *old_cells = level_cells[level];
            map_cell_store_t *new_cells = map_cell_store_create((size_t)width * height);
            for (int x = 0; x < width; x++) {
                for (int y = 0; y < height; y++) {
                    int source_x = x + dx;
                    int source_y = y + dy;
                    if (source_x < 0 || source_x >= old_w || source_y < 0 || source_y >= old_h) {
                        continue;
                    }

                    int physical_x = (source_x + old_origin_x) % old_w;
                    int physical_y = (source_y + old_origin_y) % old_h;
                    size_t old_index = (size_t)physical_y * old_w + physical_x;
                    size_t new_index = (size_t)y * width + x;
                    new_cells->headers[new_index] = old_cells->headers[old_index];
                    map_cell_t *source = map_cell_store_slot(old_cells, old_index, false);
                    if (source != NULL) {
                        new_cells->slots[new_index] = map_cell_clone(source);
                    }
                }
            }

            map_cell_store_destroy(old_cells);
            level_cells[level] = new_cells;
            level_lighting_revision[level]++;
        }

        map_cache_origin_x = 0;
        map_cache_origin_y = 0;
    } else {
        if (abs(dx) >= width || abs(dy) >= height) {
            for (size_t level = 0; level < arraysize(level_cells); level++) {
                if (level_cells[level] != NULL) {
                    for (size_t cell = 0; cell < level_cells[level]->count; cell++) {
                        map_cell_store_clear_slot(level_cells[level], cell);
                        memset(&level_cells[level]->headers[cell],
                               0,
                               sizeof(level_cells[level]->headers[cell]));
                    }
                    level_lighting_revision[level]++;
                }
            }
            map_cache_origin_x = 0;
            map_cache_origin_y = 0;
        } else if (dx != 0 || dy != 0) {
            map_cache_origin_x = (map_cache_origin_x + dx + width) % width;
            map_cache_origin_y = (map_cache_origin_y + dy + height) % height;

            int view_x = map_width * (MAP_FOW_SIZE / 2);
            int view_y = map_height * (MAP_FOW_SIZE / 2);
            for (size_t level = 0; level < arraysize(level_cells); level++) {
                if (level_cells[level] == NULL) {
                    continue;
                }

                map_cell_store_t *level_cells_current = level_cells[level];

                /* Clear only the cache strips newly exposed by the scroll.
                 * The old implementation allocated and copied the complete
                 * five-window FOW cache for every map level on every step. */
                int clear_x_start = dx > 0 ? width - dx : 0;
                int clear_x_end = dx > 0 ? width : -dx;
                for (int x = clear_x_start; x < clear_x_end; x++) {
                    for (int y = 0; y < height; y++) {
                        map_state_transaction_record_physical(level, x, y);
                        int physical_x = (x + map_cache_origin_x) % width;
                        int physical_y = (y + map_cache_origin_y) % height;
                        size_t index = (size_t)physical_y * width + physical_x;
                        map_cell_store_clear_slot(level_cells_current, index);
                        memset(&level_cells_current->headers[index],
                               0,
                               sizeof(level_cells_current->headers[index]));
                    }
                }

                int clear_y_start = dy > 0 ? height - dy : 0;
                int clear_y_end = dy > 0 ? height : -dy;
                for (int y = clear_y_start; y < clear_y_end; y++) {
                    for (int x = 0; x < width; x++) {
                        map_state_transaction_record_physical(level, x, y);
                        int physical_x = (x + map_cache_origin_x) % width;
                        int physical_y = (y + map_cache_origin_y) % height;
                        size_t index = (size_t)physical_y * width + physical_x;
                        map_cell_store_clear_slot(level_cells_current, index);
                        memset(&level_cells_current->headers[index],
                               0,
                               sizeof(level_cells_current->headers[index]));
                    }
                }

                /* Cells leaving the visible window become explored FOW. The
                 * rest of the history already carries its prior FOW state. */
                int shifted_view_x = view_x - dx;
                int shifted_view_y = view_y - dy;
                int fow_x_start = dx > 0 ? shifted_view_x : view_x + map_width;
                int fow_x_end = dx > 0 ? view_x : shifted_view_x + map_width;
                map_cache_mark_fow(level_cells_current,
                                   level,
                                   fow_x_start,
                                   fow_x_end,
                                   shifted_view_y,
                                   shifted_view_y + map_height,
                                   width,
                                   height);

                int fow_y_start = dy > 0 ? shifted_view_y : view_y + map_height;
                int fow_y_end = dy > 0 ? view_y : shifted_view_y + map_height;
                map_cache_mark_fow(level_cells_current,
                                   level,
                                   shifted_view_x,
                                   shifted_view_x + map_width,
                                   fow_y_start,
                                   fow_y_end,
                                   width,
                                   height);
            }
        }

        lighting_scroll((dy - dx) * MAP_TILE_YOFF, -(dx + dy) * MAP_TILE_XOFF);
        map_dirty_lighting_fow(dx, dy);
    }

    map_select_level(0, true);

    sound_ambient_mapcroll(dx, dy);
    map_anims_mapscroll(dx, dy);
    cpl.target_object_index = 0;
}

/** Shift independently cached levels after moving through an up/down link. */
void map_level_scroll(int dz) {
    if (dz == 0) {
        return;
    }

    map_cell_store_t *shifted[MAP2_LEVELS] = {0};
    uint64_t shifted_revisions[MAP2_LEVELS] = {0};
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        int source_depth = depth + dz;

        if (source_depth >= -MAP2_MAX_DEPTH && source_depth <= MAP2_MAX_DEPTH) {
            shifted[MAP2_DEPTH_INDEX(depth)] = level_cells[MAP2_DEPTH_INDEX(source_depth)];
            shifted_revisions[MAP2_DEPTH_INDEX(depth)] =
                level_lighting_revision[MAP2_DEPTH_INDEX(source_depth)];
            level_cells[MAP2_DEPTH_INDEX(source_depth)] = NULL;
        }
    }

    for (size_t i = 0; i < arraysize(level_cells); i++) {
        map_cell_store_destroy(level_cells[i]);
        level_cells[i] = shifted[i];
        level_lighting_revision[i] = shifted_revisions[i];
    }

    if (dz > 0) {
        map_level_mask >>= dz;
    } else {
        map_level_mask <<= -dz;
    }
    map_level_mask &= (UINT16_C(1) << MAP2_LEVELS) - 1;
    lighting_level_scroll(dz);
    map_select_level(0, true);
    map_anims_clear();
}

/**
 * Update map's name.
 * @param name
 * New map name.
 */
void update_map_name(const char *name) {
    snprintf(MapData.name_new, sizeof(MapData.name_new), "%s", name);
    rich_presence_zone_changed();
}

/**
 * Update map's weather.
 * @param weather
 * New weather.
 */
void update_map_weather(const char *weather) {
    effect_start(weather);
}

/**
 * Update map's height difference rendering flag.
 */
void update_map_height_diff(uint8_t height_diff) {
    MapData.height_diff = height_diff;
}

/**
 * Update map's region name.
 * @param region_name
 * New region name.
 */
void update_map_region_name(const char *region_name) {
    if (strcmp(MapData.region_name, region_name) == 0) {
        return;
    }

    snprintf(VS(MapData.region_name), "%s", region_name);
    if (!map_state_transaction.active) {
        region_map_update(MapData.region_map, region_name);
    }
}

/**
 * Update map's region long name.
 * @param region_longname
 * New region long name.
 */
void update_map_region_longname(const char *region_longname) {
    snprintf(VS(MapData.region_longname), "%s", region_longname);
}

/**
 * Update map's path.
 * @param map_path
 * New map path.
 */
void update_map_path(const char *map_path) {
    snprintf(VS(MapData.map_path), "%s", map_path);
}

/**
 * Get player's direction.
 * @return
 * Player's direction.
 */
int map_get_player_direction(void) {
    map_cell_t *cell;
    int direction;

    cell = MAP_CELL_GET_MIDDLE(map_width - (map_width / 2) - 1, map_height - (map_height / 2) - 1);

    direction =
        map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_LIVING, MapData.player_sub_layer))
            ->anim_facing;

    if (direction == 0) {
        return 1;
    }

    return direction - 1;
}

/**
 * Get real map X/Y coordinates adjusted for player's position.
 * @param[out] x Will contain X coordinate.
 * @param[out] y Will contain Y coordinate.
 */
void map_get_real_coords(int *x, int *y) {
    *x = MapData.posx - (map_width / 2);
    *y = MapData.posy - (map_height / 2);
}

/**
 * Initialize map's data.
 * @param xl
 * Map width.
 * @param yl
 * Map height.
 * @param px
 * Player's X position.
 * @param py
 * Player's Y position.
 */
void init_map_data(int xl, int yl, int px, int py) {
    if (xl != -1) {
        MapData.xlen = xl;
    }

    if (yl != -1) {
        MapData.ylen = yl;
    }

    if (px != -1) {
        MapData.posx = px;
    }

    if (py != -1) {
        MapData.posy = py;
    }

    if (xl > 0) {
        clear_map(false);
    }
}

#define MAX_STRETCH 8
#define MAX_STRETCH_DIAG 12

/** Return one floor height used to join a stretched tile to its neighbor. */
static int map_cell_stretch_height(int x, int y, int w, int h, int sub_layer, int my_height) {
    if (x < 0 || x >= w || y < 0 || y >= h) {
        return 0;
    }

    map_cell_t *cell = map_cache_cell(cells, x, y);

    /* A negative floor beside stacked terrain joins to that terrain's top
     * floor. This used to infer stacked terrain from LAYER_EFFECT objects;
     * floor geometry itself is the authoritative source. */
    if (my_height < 0) {
        if (cell->stretch_upper_height != 0) {
            return cell->stretch_upper_height;
        }

        if (sub_layer != 0) {
            return cell->stretch_top_height;
        }
    }

    return map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height;
}

/**
 * Align tile stretch based on X/Y.
 * @param x
 * X position.
 * @param y
 * Y position.
 * @param w
 * Max width.
 * @param h
 * Max height.
 * @param sub_layer
 * Sub-layer.
 */
static void align_tile_stretch(int x, int y, int w, int h, int sub_layer) {
    int top, bottom, right, left, min_ht;
    int32_t stretch;
    int nw_height, n_height, ne_height, sw_height, s_height, se_height, w_height, e_height,
        my_height;

    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }

    my_height = map_cell_stretch_height(x, y, w, h, sub_layer, 0);
    nw_height = map_cell_stretch_height(x - 1, y - 1, w, h, sub_layer, my_height);
    n_height = map_cell_stretch_height(x, y - 1, w, h, sub_layer, my_height);
    ne_height = map_cell_stretch_height(x + 1, y - 1, w, h, sub_layer, my_height);
    sw_height = map_cell_stretch_height(x - 1, y + 1, w, h, sub_layer, my_height);
    s_height = map_cell_stretch_height(x, y + 1, w, h, sub_layer, my_height);
    se_height = map_cell_stretch_height(x + 1, y + 1, w, h, sub_layer, my_height);
    w_height = map_cell_stretch_height(x - 1, y, w, h, sub_layer, my_height);
    e_height = map_cell_stretch_height(x + 1, y, w, h, sub_layer, my_height);

    if (abs(my_height - e_height) > MAX_STRETCH) {
        e_height = my_height;
    }

    if (abs(my_height - se_height) > MAX_STRETCH_DIAG) {
        se_height = my_height;
    }

    if (abs(my_height - s_height) > MAX_STRETCH) {
        s_height = my_height;
    }

    if (abs(my_height - sw_height) > MAX_STRETCH_DIAG) {
        sw_height = my_height;
    }

    if (abs(my_height - w_height) > MAX_STRETCH) {
        w_height = my_height;
    }

    if (abs(my_height - nw_height) > MAX_STRETCH_DIAG) {
        nw_height = my_height;
    }

    if (abs(my_height - n_height) > MAX_STRETCH) {
        n_height = my_height;
    }

    if (abs(my_height - ne_height) > MAX_STRETCH_DIAG) {
        ne_height = my_height;
    }

    top = MAX(w_height, nw_height);
    top = MAX(top, n_height);
    top = MAX(top, my_height);

    bottom = MAX(s_height, se_height);
    bottom = MAX(bottom, e_height);
    bottom = MAX(bottom, my_height);

    right = MAX(n_height, ne_height);
    right = MAX(right, e_height);
    right = MAX(right, my_height);

    left = MAX(w_height, sw_height);
    left = MAX(left, s_height);
    left = MAX(left, my_height);

    min_ht = MIN(top, bottom);
    min_ht = MIN(min_ht, left);
    min_ht = MIN(min_ht, right);
    min_ht = MIN(min_ht, my_height);

    if (my_height < 0 && left == 0 && right == 0 && top == 0 && bottom == 0) {
        int top2 = MIN(w_height, nw_height);
        top2 = MIN(top2, n_height);
        top2 = MIN(top2, my_height);

        int bottom2 = MIN(s_height, se_height);
        bottom2 = MIN(bottom2, e_height);
        bottom2 = MIN(bottom2, my_height);

        int right2 = MIN(n_height, ne_height);
        right2 = MIN(right2, e_height);
        right2 = MIN(right2, my_height);

        int left2 = MIN(w_height, sw_height);
        left2 = MIN(left2, s_height);
        left2 = MIN(left2, my_height);

        top = top2 - top;
        bottom = bottom2 - bottom;
        right = right2 - right;
        left = left2 - left;

        min_ht = MIN(top, bottom);
        min_ht = MIN(min_ht, left);
        min_ht = MIN(min_ht, right);
        min_ht = MIN(min_ht, my_height);

        min_ht = abs(min_ht);
        top = abs(top);
        bottom = abs(bottom);
        left = abs(left);
        right = abs(right);
    }

    /* Normalize these... */
    top -= min_ht;
    bottom -= min_ht;
    left -= min_ht;
    right -= min_ht;

    stretch = abs(bottom) + (abs(left) << 8) + (abs(right) << 16) + (abs(top) << 24);
    map_cell_t *cell = map_cache_cell_mutable(cells, x, y);
    map_cell_sublayer_record_t *record = map_cell_sublayer_record(cell, sub_layer, stretch != 0);
    if (record != NULL) {
        record->stretch = stretch;
        map_cell_sublayer_record_trim(cell, sub_layer);
    }
}

/**
 * Adjust the tile stretch of a map.
 *
 * Scans the visible window and updates only cells marked dirty by incremental
 * map changes. A tile's stretch depends on its eight neighbors, so the setter
 * propagates dirtiness to that complete neighborhood.
 */
void adjust_tile_stretch(void) {
    int xoff, yoff, w, h, x, y, sub_layer;

    xoff = map_width * (MAP_FOW_SIZE / 2);
    yoff = map_height * (MAP_FOW_SIZE / 2);
    w = map_width * MAP_FOW_SIZE;
    h = map_height * MAP_FOW_SIZE;

    for (x = xoff; x < xoff + map_width; x++) {
        for (y = yoff; y < yoff + map_height; y++) {
            map_cell_t *cell = MAP_CELL_GET(x, y);
            if (!cell->stretch_dirty) {
                continue;
            }

            for (sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                align_tile_stretch(x, y, w, h, sub_layer);
            }
            cell->stretch_dirty = 0;
            map_cell_store_trim_slot(cells, map_cache_physical_index(x, y));
        }
    }
}

/** Mark every stretch result affected by one changed map cell. */
static void map_mark_stretch_dirty(int x, int y) {
    int cache_width = map_width * MAP_FOW_SIZE;
    int cache_height = map_height * MAP_FOW_SIZE;
    int cache_x = x + map_width * (MAP_FOW_SIZE / 2);
    int cache_y = y + map_height * (MAP_FOW_SIZE / 2);

    for (int neighbor_x = cache_x - 1; neighbor_x <= cache_x + 1; neighbor_x++) {
        for (int neighbor_y = cache_y - 1; neighbor_y <= cache_y + 1; neighbor_y++) {
            if (neighbor_x >= 0 && neighbor_x < cache_width && neighbor_y >= 0 &&
                neighbor_y < cache_height) {
                map_state_transaction_record_physical(current_level_index, neighbor_x, neighbor_y);
                MAP_CELL_GET_MUTABLE(neighbor_x, neighbor_y)->stretch_dirty = 1;
            }
        }
    }
}

/** Refresh the floor-only geometry summary used by the tilestretcher. */
static void map_update_stretch_geometry(map_cell_t *cell) {
    cell->stretch_top_height = 0;
    cell->stretch_upper_height = 0;
    cell->level_support_height = 0;

    for (int sub_layer = NUM_SUB_LAYERS - 1; sub_layer >= 0; sub_layer--) {
        int16_t height =
            map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height;
        cell->level_support_height = MAX(cell->level_support_height, height);
        if (height == 0) {
            continue;
        }

        if (cell->stretch_top_height == 0) {
            cell->stretch_top_height = height;
        }

        if (sub_layer != 0 && cell->stretch_upper_height == 0) {
            cell->stretch_upper_height = height;
        }
    }
}

/** Refresh the maximum elevation used for whole-cell screen rejection. */
static void map_update_render_height(map_cell_t *cell) {
    cell->render_max_height = 0;

    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        cell->render_max_height =
            MAX(cell->render_max_height,
                map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height);
        cell->render_max_height =
            MAX(cell->render_max_height,
                map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_EFFECT, sub_layer))->height);
    }
}

/** Clear live MAP2 presentation while retaining remembered static geometry. */
static void map_clear_live_cell(map_cell_t *cell) {
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = LAYER_ITEM; object_layer <= LAYER_EFFECT; object_layer++) {
            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            map_cell_layer_record_t *record = map_cell_layer_record(cell, layer, false);
            if (record == NULL) {
                continue;
            }
            map_visibility_fade_t *fade = &record->visibility;
            if (fade->initialized && fade->authorized) {
                map_visibility_fade_revoke(fade, LastTick);
            }
        }
    }
    map_cell_clear_live_state(cell);
    map_update_render_height(cell);
}

static bool map_visibility_transient_layer(int layer) {
    return layer == LAYER_ITEM || layer == LAYER_ITEM2 || layer == LAYER_LIVING ||
           layer == LAYER_EFFECT;
}

/** Clear one expired live layer after its presentation fade reaches zero. */
static void map_clear_expired_visibility_layer(map_cell_t *cell, int sub_layer, int object_layer) {
    int layer = GET_MAP_LAYER(object_layer, sub_layer);
    uint8_t object_layer_mask = UINT8_C(1) << (object_layer - 1);

    map_cell_sublayer_record_t *sub_record = map_cell_sublayer_record(cell, sub_layer, false);
    if (sub_record != NULL) {
        sub_record->door &= (uint8_t)~object_layer_mask;
        sub_record->exit &= (uint8_t)~object_layer_mask;
        sub_record->priority &= (uint8_t)~object_layer_mask;
        sub_record->secondpass &= (uint8_t)~object_layer_mask;
        map_cell_sublayer_record_trim(cell, sub_layer);
    }
    if (object_layer == LAYER_LIVING) {
        map_cell_actor_record_t *actor = map_cell_actor_record(cell, sub_layer, false);
        if (actor != NULL) {
            actor->anim_flags = 0;
            actor->probe = 0;
            actor->target_object_count = 0;
            actor->target_is_friend = 0;
            actor->name[0] = '\0';
            actor->color[0] = '\0';
            map_cell_actor_record_trim(cell, sub_layer);
        }
    }

    map_cell_layer_record_remove(cell, layer);
}

/**
 * Set data for map cell.
 *
 * Remembered static geometry is updated independently from live presentation.
 * @param x
 * X of the cell.
 * @param y
 * Y of the cell.
 * @param layer
 * Layer we're doing this for.
 * @param face
 * Face to set.
 * @param quick_pos
 * Is this a multi-arch?
 * @param obj_flags
 * Flags.
 * @param name
 * Player's name.
 * @param name_color
 * Player's name color.
 * @param height
 * Z position of the tile.
 * @param probe
 * Target's HP bar.
 * @param zoom
 * How much to zoom the face by.
 * @param align
 * X align.
 * @param rotate
 * Rotation in degrees.
 * @param infravision
 * Whether to show the object in red.
 */
void map_set_data(int x,
                  int y,
                  int layer,
                  int16_t face,
                  uint8_t quick_pos,
                  uint8_t obj_flags,
                  const char *name,
                  const char *name_color,
                  int16_t height,
                  uint8_t probe,
                  int16_t zoom_x,
                  int16_t zoom_y,
                  int16_t align,
                  uint8_t draw_double,
                  uint8_t alpha,
                  int16_t rotate,
                  uint8_t infravision,
                  uint32_t target_object_count,
                  uint8_t target_is_friend,
                  uint8_t anim_speed,
                  uint8_t anim_facing,
                  uint8_t anim_flags,
                  uint8_t anim_state,
                  uint8_t priority,
                  uint8_t secondpass,
                  uint8_t roof,
                  uint8_t door,
                  uint8_t exit,
                  const char *glow,
                  uint8_t glow_speed) {
    map_cell_t *cell;
    int sub_layer;

    map_state_transaction_record_cell(x, y);
    cell = MAP_CELL_GET_MIDDLE_MUTABLE(x, y);
    sub_layer = layer / NUM_LAYERS;
    int object_layer = (layer % NUM_LAYERS) + 1;
    const map_cell_layer_record_t *old_layer = map_cell_layer_record_read(cell, layer);
    bool stretch_geometry_changed =
        object_layer == LAYER_FLOOR && (old_layer->face != face || old_layer->height != height);
    bool lighting_geometry_changed =
        object_layer == LAYER_FLOOR && (old_layer->face != face || old_layer->height != height);
    bool render_height_changed = (object_layer == LAYER_FLOOR || object_layer == LAYER_EFFECT) &&
                                 old_layer->height != height;
    map_cell_layer_record_t *layer_record = map_cell_layer_record(cell, layer, true);
    map_cell_sublayer_record_t *sub_record = map_cell_sublayer_record(cell, sub_layer, true);
    map_cell_actor_record_t *actor_record =
        object_layer == LAYER_LIVING ? map_cell_actor_record(cell, sub_layer, true) : NULL;

    if (anim_speed != 0 && old_layer->face != face) {
        layer_record->anim_state = 0;
    }

    uint8_t object_layer_mask = UINT8_C(1) << (object_layer - 1);
    sub_record->priority &= (uint8_t)~object_layer_mask;
    sub_record->secondpass &= (uint8_t)~object_layer_mask;
    if (priority) {
        sub_record->priority |= object_layer_mask;
    }
    if (secondpass) {
        sub_record->secondpass |= object_layer_mask;
    }

    bool retain_visibility_fade = face == 0 && map_visibility_transient_layer(object_layer) &&
                                  old_layer->face != 0 && old_layer->visibility.initialized &&
                                  old_layer->visibility.alpha != 0;
    if (retain_visibility_fade) {
        map_visibility_fade_revoke(&layer_record->visibility, LastTick);
        sub_record->door &= (uint8_t)~object_layer_mask;
        sub_record->exit &= (uint8_t)~object_layer_mask;
        sub_record->priority &= (uint8_t)~object_layer_mask;
        sub_record->secondpass &= (uint8_t)~object_layer_mask;
        if (object_layer == LAYER_LIVING) {
            actor_record->probe = 0;
            actor_record->target_is_friend = 0;
            actor_record->anim_flags = 0;
            actor_record->target_object_count = 0;
            actor_record->name[0] = '\0';
            actor_record->color[0] = '\0';
            map_cell_actor_record_trim(cell, sub_layer);
        }
        map_cell_sublayer_record_trim(cell, sub_layer);
        map_cell_store_trim_slot(cells, map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY));
        return;
    }

    layer_record->face = face;
    layer_record->flags = obj_flags;
    layer_record->roof = roof;
    sub_record->door &= (uint8_t)~object_layer_mask;
    if (door) {
        sub_record->door |= object_layer_mask;
    }
    sub_record->exit &= (uint8_t)~object_layer_mask;
    if (exit) {
        sub_record->exit |= object_layer_mask;
    }

    layer_record->quick_pos = quick_pos;
    snprintf(VS(layer_record->glow), "%s", glow);
    layer_record->height = height;
    layer_record->zoom_x = zoom_x;
    layer_record->zoom_y = zoom_y;
    layer_record->align = align;
    layer_record->draw_double = draw_double;
    layer_record->alpha = alpha;
    if (map_visibility_transient_layer(object_layer)) {
        map_visibility_fade_t *fade = &layer_record->visibility;
        if (face == 0) {
            map_visibility_fade_revoke(fade, LastTick);
        } else {
            bool first_authoritative_record = !fade->initialized;
            map_visibility_fade_authorize(fade, UINT8_MAX, LastTick);
            /* The first complete MAP2 snapshot is the renderer's baseline;
             * transitions apply to later visibility enters/reappearances. */
            if (first_authoritative_record) {
                fade->alpha = UINT8_MAX;
                fade->from_alpha = UINT8_MAX;
                fade->target_alpha = UINT8_MAX;
                fade->transition_started = LastTick;
            }
        }
    }
    layer_record->rotate = rotate;
    layer_record->infravision = infravision;
    layer_record->glow_speed = glow_speed;

    if (stretch_geometry_changed) {
        map_update_stretch_geometry(cell);
        map_mark_stretch_dirty(x, y);
    }

    if (render_height_changed) {
        map_update_render_height(cell);
    }

    layer_record->anim_speed = anim_speed;
    layer_record->anim_facing = anim_facing;

    if (object_layer == LAYER_LIVING) {
        if (actor_record->target_object_count != target_object_count ||
            actor_record->target_is_friend != target_is_friend) {
            cpl.target_object_index = 0;
        }

        actor_record->probe = probe;
        actor_record->target_object_count = target_object_count;
        actor_record->target_is_friend = target_is_friend;
        snprintf(VS(actor_record->color), "%s", name_color);
        snprintf(VS(actor_record->name), "%s", name);

        if (anim_flags & ANIM_FLAG_ATTACKING && !(actor_record->anim_flags & ANIM_FLAG_ATTACKING)) {
            layer_record->anim_state = 0;
        } else if (anim_flags & ANIM_FLAG_MOVING &&
                   !(actor_record->anim_flags & ANIM_FLAG_MOVING)) {
            layer_record->anim_state = anim_state;
        }

        actor_record->anim_flags = anim_flags;
    }

    if (anim_speed != 0) {
        if (!check_animation_status(face)) {
            layer_record->face = 0;
            layer_record->anim_speed = 0;
        }
    } else {
        image_request_face(face);
    }

    if (face == 0 && layer_record->visibility.initialized && !layer_record->visibility.authorized &&
        layer_record->visibility.alpha == 0) {
        memset(&layer_record->visibility, 0, sizeof(layer_record->visibility));
    }
    if (object_layer == LAYER_LIVING) {
        map_cell_actor_record_trim(cell, sub_layer);
    }
    map_cell_sublayer_record_trim(cell, sub_layer);
    if (map_cell_layer_record_empty(layer_record)) {
        map_cell_layer_record_remove(cell, layer);
    }
    map_cell_store_trim_slot(cells, map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY));

    if (lighting_geometry_changed) {
        level_lighting_revision[current_level_index]++;
    }
}

/**
 * Clear map's cell.
 *
 * In reality, this only clears some data on the cell, and sets the FOW flag
 * to mark that the cell is actually FOW.
 * @param x X of the cell.
 * @param y Y of the cell.
 * @param hard Whether to discard cached geometry instead of retaining FOW.
 */
void map_clear_cell(int x, int y, bool hard) {
    map_state_transaction_record_cell(x, y);
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    map_cell_t *cell = map_cell_store_slot(cells, index, false);
    if (cell == NULL) {
        if (hard) {
            map_cell_store_set_support_height(cells, index, 0);
        }
        map_cell_store_set_fow(cells, index, true, false);
        return;
    }
    bool had_known_light = false;
    for (size_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        had_known_light |= map_cell_light_record_read(cell, sub_layer)->known != 0;
    }

    if (hard) {
        bool had_floor_geometry = false;
        for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
            had_floor_geometry |=
                map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->face != 0;
        }

        map_cell_store_clear_slot(cells, index);
        map_cell_store_set_support_height(cells, index, 0);
        map_cell_store_set_fow(cells, index, true, false);

        if (had_floor_geometry) {
            map_mark_stretch_dirty(x, y);
        }

        if (had_known_light || had_floor_geometry) {
            level_lighting_revision[current_level_index]++;
        }

        return;
    }

    map_clear_live_cell(cell);
    map_cell_clear_light_state(cell);
    map_cell_store_set_fow(cells, index, true, false);
    map_cell_store_trim_slot(cells, index);

    if (had_known_light) {
        level_lighting_revision[current_level_index]++;
    }
}

/** Store base-map elevation needed to project independently cached upper levels. */
void map_set_structural_support_height(int x, int y, int16_t height) {
    map_state_transaction_record_cell(x, y);
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    if (!map_cell_store_set_support_height(cells, index, height)) {
        return;
    }
    level_lighting_revision[current_level_index]++;
}

/** Apply an explicit server visibility state after a tile's layer deltas. */
void map_set_fow(int x, int y, bool fow) {
    map_state_transaction_record_cell(x, y);
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    map_cell_t *cell = map_cell_store_slot(cells, index, false);

    if (fow && cell != NULL && !cell->fow) {
        map_clear_cell(x, y, false);
    }

    map_cell_header_t *header = &cells->headers[index];
    if ((header->fow != 0) == fow && (header->structural_fow != 0) == fow) {
        return;
    }

    map_cell_store_set_fow(cells, index, fow, fow);
    level_lighting_revision[current_level_index]++;
}

/** Return the currently cached visibility state for one map tile. */
bool map_get_fow(int x, int y) {
    return MAP_CELL_GET_MIDDLE(x, y)->fow != 0;
}

/**
 * Set pre-tone Q5.11 scalar radiance for a map cell.
 * @param x
 * X of the cell.
 * @param y
 * Y of the cell.
 * @param sub_layer
 * Sub-layer.
 * @param radiance
 * Q5.11 scalar radiance to set.
 */
void map_set_light_radiance(int x, int y, int sub_layer, uint16_t radiance) {
    map_cell_t *cell;

    map_state_transaction_record_cell(x, y);
    cell = MAP_CELL_GET_MIDDLE_MUTABLE(x, y);
    map_cell_light_record_t *light = map_cell_light_record(cell, sub_layer, true);
    bool changed = !light->known || light->radiance != radiance;
    light->radiance = radiance;
    light->known = 1;
    if (!light->rgb_explicit) {
        for (size_t channel = 0; channel < 3; channel++) {
            light->rgb_radiance[channel] = radiance;
        }
    }
    /* A current endpoint without a timed extension is an authoritative
     * snap/rebase. The following keyframe extension, when present, replaces
     * this provisional target in the same tile transaction. */
    light->next_radiance = radiance;
    light->next_known = 0;
    memcpy(light->next_rgb_radiance, light->rgb_radiance, sizeof(light->next_rgb_radiance));
    if (changed && !cell->fow) {
        level_lighting_revision[current_level_index]++;
    }
}

/** Apply one complete per-tile colored-light state. */
void map_set_light_rgb_radiance(int x,
                                int y,
                                uint8_t bitmap,
                                const uint16_t rgb[NUM_SUB_LAYERS][3]) {
    map_state_transaction_record_cell(x, y);
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    map_cell_t *cell = map_cell_store_slot(cells, index, bitmap != 0);
    if (cell == NULL) {
        return;
    }
    bool changed = map_cell_light_rgb_bitmap(cell) != bitmap;

    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        bool explicit_rgb = (bitmap & (UINT8_C(1) << sub_layer)) != 0;
        map_cell_light_record_t *light = map_cell_light_record(cell, sub_layer, explicit_rgb);
        if (light == NULL) {
            continue;
        }
        uint16_t resolved[3];
        if (explicit_rgb) {
            memcpy(resolved, rgb[sub_layer], sizeof(resolved));
        } else {
            for (size_t channel = 0; channel < 3; channel++) {
                resolved[channel] = light->radiance;
            }
        }

        if (memcmp(light->rgb_radiance, resolved, sizeof(resolved)) != 0) {
            memcpy(light->rgb_radiance, resolved, sizeof(resolved));
            changed = true;
        }
        light->rgb_explicit = explicit_rgb;
        memcpy(light->next_rgb_radiance, light->rgb_radiance, sizeof(light->next_rgb_radiance));
        light->next_rgb_explicit = light->rgb_explicit;
        map_cell_light_record_trim(cell, sub_layer);
    }
    bool visible = !cell->fow;
    map_cell_store_trim_slot(cells, index);
    if (changed && visible) {
        level_lighting_revision[current_level_index]++;
    }
}

void map_set_light_keyframe(int x,
                            int y,
                            uint64_t generation,
                            uint64_t start_seconds,
                            uint64_t end_seconds,
                            uint8_t flags,
                            uint8_t scalar_bitmap,
                            const uint16_t scalar[NUM_SUB_LAYERS],
                            uint8_t rgb_bitmap,
                            const uint16_t rgb[NUM_SUB_LAYERS][3]) {
    map_state_transaction_record_cell(x, y);
    if (generation == 0 || start_seconds >= end_seconds) {
        return;
    }
    size_t index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
    map_cell_t *cell = map_cell_store_slot(cells, index, false);
    if (cell != NULL && map_cell_light_keyframe_record_read(cell)->valid &&
        generation < map_cell_light_keyframe_record_read(cell)->generation) {
        return;
    }
    cell = map_cell_store_slot(cells, index, true);

    map_cell_light_keyframe_record_t *keyframe = map_cell_light_keyframe_record(cell, true);
    bool changed = !keyframe->valid || keyframe->generation != generation ||
                   keyframe->start_seconds != start_seconds ||
                   keyframe->end_seconds != end_seconds || keyframe->flags != flags;
    keyframe->generation = generation;
    keyframe->start_seconds = start_seconds;
    keyframe->end_seconds = end_seconds;
    keyframe->flags = flags;
    keyframe->valid = 1;
    for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        bool scalar_present = (scalar_bitmap & (UINT8_C(1) << sub_layer)) != 0;
        bool rgb_present = (rgb_bitmap & (UINT8_C(1) << sub_layer)) != 0;
        map_cell_light_record_t *light =
            map_cell_light_record(cell, sub_layer, scalar_present || rgb_present);
        if (light == NULL) {
            continue;
        }
        uint16_t next_scalar =
            (scalar_bitmap & (UINT8_C(1) << sub_layer)) ? scalar[sub_layer] : light->radiance;
        if (!light->next_known || light->next_radiance != next_scalar) {
            changed = true;
        }
        light->next_radiance = next_scalar;
        light->next_known = 1;
        for (size_t channel = 0; channel < 3; channel++) {
            uint16_t next_rgb = (rgb_bitmap & (UINT8_C(1) << sub_layer))
                                    ? rgb[sub_layer][channel]
                                    : light->rgb_radiance[channel];
            if (light->next_rgb_radiance[channel] != next_rgb) {
                changed = true;
            }
            light->next_rgb_radiance[channel] = next_rgb;
        }
        light->next_rgb_explicit = (rgb_bitmap & (UINT8_C(1) << sub_layer)) != 0;
    }
    if (changed && !cell->fow) {
        level_lighting_revision[current_level_index]++;
    }
}

bool map_light_keyframe_transaction_begin(uint64_t generation,
                                          uint64_t start_seconds,
                                          uint64_t end_seconds,
                                          uint8_t flags) {
    if (generation == 0 || end_seconds <= start_seconds || flags == 0 ||
        (flags & ~(MAP2_LIGHT_KEYFRAME_CONTINUOUS | MAP2_LIGHT_KEYFRAME_SNAP)) != 0) {
        return false;
    }
    map_light_keyframe_transaction_abort();
    map_light_keyframe_transaction.active = true;
    map_light_keyframe_transaction.generation = generation;
    map_light_keyframe_transaction.start_seconds = start_seconds;
    map_light_keyframe_transaction.end_seconds = end_seconds;
    map_light_keyframe_transaction.flags = flags;
    return true;
}

bool map_light_keyframe_transaction_pending(void) {
    return map_light_keyframe_transaction.active;
}

bool map_light_keyframe_transaction_stage(int depth,
                                          int x,
                                          int y,
                                          uint8_t scalar_bitmap,
                                          const uint16_t scalar[NUM_SUB_LAYERS],
                                          uint8_t rgb_bitmap,
                                          const uint16_t rgb[NUM_SUB_LAYERS][3]) {
    if (!map_light_keyframe_transaction.active || depth < -MAP2_MAX_DEPTH ||
        depth > MAP2_MAX_DEPTH || x < 0 || x >= map_width || y < 0 || y >= map_height ||
        map_width <= 0 || map_height <= 0) {
        return false;
    }
    size_t maximum = (size_t)map_width * (size_t)map_height * MAP2_LEVELS;
    if (map_light_keyframe_transaction.count >= maximum) {
        return false;
    }
    if (map_light_keyframe_transaction.count == map_light_keyframe_transaction.capacity) {
        size_t capacity = map_light_keyframe_transaction.capacity == 0
                              ? MIN((size_t)256, maximum)
                              : MIN(map_light_keyframe_transaction.capacity * 2, maximum);
        map_light_keyframe_transaction.entries =
            xreallocarray(map_light_keyframe_transaction.entries,
                          capacity,
                          sizeof(*map_light_keyframe_transaction.entries));
        map_light_keyframe_transaction.capacity = capacity;
    }
    map_light_keyframe_stage_t *entry =
        &map_light_keyframe_transaction.entries[map_light_keyframe_transaction.count++];
    entry->depth = depth;
    entry->x = x;
    entry->y = y;
    entry->scalar_bitmap = scalar_bitmap;
    memcpy(entry->scalar, scalar, sizeof(entry->scalar));
    entry->rgb_bitmap = rgb_bitmap;
    memcpy(entry->rgb, rgb, sizeof(entry->rgb));
    return true;
}

void map_light_keyframe_transaction_commit(void) {
    if (!map_light_keyframe_transaction.active) {
        return;
    }
    for (size_t i = 0; i < map_light_keyframe_transaction.count; i++) {
        map_light_keyframe_stage_t *entry = &map_light_keyframe_transaction.entries[i];
        if (!map_select_level(entry->depth, false)) {
            continue;
        }
        map_set_light_keyframe(entry->x,
                               entry->y,
                               map_light_keyframe_transaction.generation,
                               map_light_keyframe_transaction.start_seconds,
                               map_light_keyframe_transaction.end_seconds,
                               map_light_keyframe_transaction.flags,
                               entry->scalar_bitmap,
                               entry->scalar,
                               entry->rgb_bitmap,
                               entry->rgb);
    }
    map_select_level(0, true);
    map_light_keyframe_transaction_abort();
}

void map_light_keyframe_transaction_abort(void) {
    free(map_light_keyframe_transaction.entries);
    memset(&map_light_keyframe_transaction, 0, sizeof(map_light_keyframe_transaction));
}

/**
 * Get the height of the topmost floor on the specified square.
 * @param x
 * X position.
 * @param y
 * Y position.
 * @return
 * The height.
 */
static int get_top_floor_height(map_cell_t *cell, int sub_layer) {
    int16_t height;

    height = map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height;

    return MAX(0, height);
}

static void map_animation_redraw_request(int layer) {
    map_redraw_request(MAP_REDRAW_REASON_ANIMATION);
    if (layer <= LAYER_FMASK) {
        map_redraw_flag = 1;
    }
}

static bool map_animate_object(map_cell_t *cell, int layer) {
    Animations *animation;
    map_cell_layer_record_t *record = map_cell_layer_record(cell, layer, false);

    if (record == NULL || record->face == 0 || record->anim_speed == 0 ||
        record->anim_facing == 0) {
        return false;
    }

    animation = animation_get(record->face);
    if (animation == NULL) {
        record->face = 0;
        record->anim_speed = 0;
        return true;
    }

    if (!(record->flags & FFLAG_SLEEP) && !(record->flags & FFLAG_PARALYZED)) {
        record->anim_state++;
        map_animation_redraw_request(layer);
    } else {
        return false;
    }

    /* If beyond drawable states, reset */
    if (record->anim_state >= animation->frame) {
        record->anim_state = 0;
    }
    return true;
}

/** Advance presentation-only live alpha without mutating MAP2 authority. */
static bool map_animate_visibility(int depth, int cache_x, int cache_y, map_cell_t *cell) {
    bool changed = false;
    int player_x = map_width * MAP_FOW_SIZE / 2;
    int player_y = map_height * MAP_FOW_SIZE / 2;
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = LAYER_ITEM; object_layer <= LAYER_EFFECT; object_layer++) {
            if (!map_visibility_transient_layer(object_layer)) {
                continue;
            }
            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            map_cell_layer_record_t *record = map_cell_layer_record(cell, layer, false);
            if (record == NULL) {
                continue;
            }
            map_visibility_fade_t *fade = &record->visibility;
            if (!fade->initialized) {
                continue;
            }
            if (!fade->authorized || record->face == 0 || cell->fow) {
                map_visibility_fade_revoke(fade, LastTick);
            } else {
                uint8_t target = map_visibility_field_alpha(
                    map_visibility_field_weight(cache_x - player_x, cache_y - player_y));
                bool local_player = depth == 0 && cache_x == player_x && cache_y == player_y &&
                                    object_layer == LAYER_LIVING &&
                                    sub_layer == MIN(MapData.player_sub_layer, NUM_SUB_LAYERS - 1);
                if (local_player) {
                    target = UINT8_MAX;
                }
                map_visibility_fade_set_target(fade, target, LastTick);
            }
            changed |= map_visibility_fade_advance(fade, LastTick);
            if (!fade->authorized && fade->alpha == 0 && record->face != 0) {
                map_clear_expired_visibility_layer(cell, sub_layer, object_layer);
            }
        }
    }
    if (changed) {
        map_animation_redraw_request(LAYER_LIVING);
    }
    return changed;
}

/** Publish a presentation-only record change to retained painter cohorts. */
static void map_cell_presentation_changed(size_t physical_index) {
    map_cell_header_t *header = &cells->headers[physical_index];
    header->revision++;
    if (header->revision == 0) {
        header->generation++;
    }
}

/**
 * Request one redraw when the visible timed-light blend enters a new game
 * minute.  The renderer still samples the exact bounded clock position, but
 * the redraw/cache invalidation boundary remains deliberately coarse and
 * deterministic.
 */
static void map_temporal_lighting_update(void) {
    if (!MapData.light_keyframe_valid ||
        MapData.light_keyframe_end_seconds <= MapData.light_keyframe_start_seconds) {
        map_temporal_lighting_bucket = UINT64_MAX;
        return;
    }

    uint64_t now;
    if (!telemetry_game_time_seconds(&now)) {
        return;
    }

    uint64_t duration = MapData.light_keyframe_end_seconds - MapData.light_keyframe_start_seconds;
    uint64_t progress = now <= MapData.light_keyframe_start_seconds
                            ? 0
                            : now - MapData.light_keyframe_start_seconds;
    progress = MIN(progress, duration);
    uint64_t bucket = (progress * UINT64_C(60)) / duration;
    if (bucket == map_temporal_lighting_bucket) {
        return;
    }
    map_temporal_lighting_bucket = bucket;

    bool endpoint_differs = false;
    size_t cell_count = (size_t)map_width * MAP_FOW_SIZE * map_height * MAP_FOW_SIZE;
    for (size_t level = 0; level < arraysize(level_cells) && !endpoint_differs; level++) {
        if (level_cells[level] == NULL || !(map_level_mask & (UINT16_C(1) << level))) {
            continue;
        }
        for (size_t index = 0; index < cell_count && !endpoint_differs; index++) {
            const map_cell_t *cell = map_cell_store_slot(level_cells[level], index, false);
            if (cell == NULL) {
                continue;
            }
            if (!map_cell_light_keyframe_record_read(cell)->valid) {
                continue;
            }
            for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                if (!map_cell_light_record_read(cell, sub_layer)->next_known) {
                    continue;
                }
                if (map_cell_light_record_read(cell, sub_layer)->radiance !=
                    map_cell_light_record_read(cell, sub_layer)->next_radiance) {
                    endpoint_differs = true;
                    break;
                }
                for (size_t channel = 0; channel < 3; channel++) {
                    if (map_cell_light_record_read(cell, sub_layer)->rgb_radiance[channel] !=
                        map_cell_light_record_read(cell, sub_layer)->next_rgb_radiance[channel]) {
                        endpoint_differs = true;
                        break;
                    }
                }
            }
        }
    }

    if (!endpoint_differs) {
        return;
    }
    for (size_t level = 0; level < arraysize(level_lighting_revision); level++) {
        level_lighting_revision[level]++;
    }
    map_redraw_request(MAP_REDRAW_REASON_LIGHTING);
}

void map_animate(void) {
    int x, y, layer;
    map_cell_t *cell;

    map_temporal_lighting_update();

    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        if (!(map_level_mask & (UINT16_C(1) << MAP2_DEPTH_INDEX(depth))) ||
            !map_select_level(depth, false)) {
            continue;
        }

        for (x = 0; x < map_width; x++) {
            for (y = 0; y < map_height; y++) {
                cell = MAP_CELL_GET_MIDDLE(x, y);
                size_t physical_index = map_cache_physical_index(x + MAP_STARTX, y + MAP_STARTY);
                bool cell_changed =
                    map_animate_visibility(depth, x + MAP_STARTX, y + MAP_STARTY, cell);

                if (cell->fow) {
                    map_cell_store_trim_slot(cells, physical_index);
                    if (cell_changed) {
                        map_cell_presentation_changed(physical_index);
                    }
                    continue;
                }

                for (layer = 0; layer < NUM_REAL_LAYERS; layer++) {
                    map_cell_layer_record_t *record = map_cell_layer_record(cell, layer, false);
                    if (record == NULL) {
                        continue;
                    }
                    if (record->glow_speed > 1) {
                        record->glow_state++;
                        map_animation_redraw_request(layer);
                        cell_changed = true;

                        if (record->glow_state > record->glow_speed) {
                            record->glow_state = 0;
                        }
                    }

                    if (record->anim_speed == 0) {
                        continue;
                    }

                    if (record->anim_last >= record->anim_speed) {
                        cell_changed |= map_animate_object(cell, layer);
                        record->anim_last = 1;
                    } else {
                        record->anim_last++;
                    }
                }
                map_cell_store_trim_slot(cells, physical_index);
                if (cell_changed) {
                    map_cell_presentation_changed(physical_index);
                }
            }
        }
    }

    map_select_level(0, true);
}

static uint16_t map_object_get_face(map_cell_t *cell, int layer) {
    int sub_layer, dir;
    Animations *animation;
    uint16_t face;

    const map_cell_layer_record_t *record = map_cell_layer_record_read(cell, layer);
    if (record->anim_speed == 0) {
        return record->face;
    }

    animation = animation_get(record->face);
    if (animation == NULL || record->anim_facing == 0) {
        return 0;
    }

    sub_layer = layer / NUM_LAYERS;
    dir = record->anim_facing - 1;

    if (animation->facings >= 25) {
        if (map_cell_actor_record_read(cell, sub_layer)->anim_flags & ANIM_FLAG_ATTACKING) {
            dir += 16;
        } else if (map_cell_actor_record_read(cell, sub_layer)->anim_flags & ANIM_FLAG_MOVING) {
            dir += 8;
        }
    }

    return animation_get_face(record->face, dir, record->anim_state, &face) ? face : 0;
}

/** Deferred UI annotation associated with a rendered map object. */
typedef struct map_annotation {
    map_cell_t *cell;
    sprite_effects_t effects;
    int32_t xl;
    int32_t yl;
    int32_t xoff;
    int32_t xoff2;
    int32_t xlen;
    int32_t bitmap_w;
    uint64_t record_identity;
    uint32_t cell_generation;
    uint16_t cell_revision;
    int16_t tile_x;
    int16_t tile_y;
    int8_t depth;
    uint8_t map_layer;
    uint8_t sub_layer;
} map_annotation_t;

/** One sprite deferred into the unified isometric painter order. */
typedef struct map_render_command {
    SDL_Surface *source;
    sprite_effects_t effects;
    int32_t x;
    int32_t y;
    int32_t bounds_x;
    int32_t bounds_y;
    int32_t bounds_w;
    int32_t bounds_h;
    int32_t sort_x;
    int32_t sort_y;
    int16_t tile_x;
    int16_t tile_y;
    size_t sequence;
    uint64_t record_identity;
    uint32_t cell_generation;
    uint16_t cell_revision;
    uint8_t record_layer;
    uint8_t object_layer;
    uint8_t sub_layer;
    int8_t depth;
    int16_t midx;
    int16_t midy;
    int32_t projection_y;
    int32_t player_height_offset;
    int32_t level_support_height;
    int32_t elevation;
    bool draw_double;
    bool door;
    bool roof;
    bool door_hint;
    bool exit;
    bool local_player;
    bool fogged; ///< The cached tile is outside the current visible area.
    bool ground;
    bool smooth_lighting;
    bool primary_level;
    SDL_Surface *living_occlusion_mask;
    bool transformed;
} map_render_command_t;

/** One deferred draw invocation retained even when it currently emits nothing. */
typedef struct map_render_candidate {
    size_t sequence;
    int16_t x;
    int16_t y;
    int16_t midx;
    int16_t midy;
    int32_t xpos;
    int32_t ypos;
    int32_t player_height_offset;
    int32_t level_support_height;
    uint8_t layer;
    uint8_t sub_layer;
    uint8_t alpha_forced;
    int8_t depth;
    bool smooth_lighting;
    bool lightmap_pending;
    bool ground_pass;
    bool primary_level;
} map_render_candidate_t;

/** Contiguous retained output and candidate ranges for one projected cell pass. */
typedef struct map_render_cohort {
    size_t candidates_first;
    size_t candidates_num;
    size_t commands_first;
    size_t commands_num;
    size_t annotations_first;
    size_t annotations_num;
    size_t tiles_first;
    size_t tiles_num;
    uint64_t record_identity;
    uint32_t cell_generation;
    uint16_t cell_revision;
    int16_t tile_x;
    int16_t tile_y;
    int8_t depth;
} map_render_cohort_t;

/** Output accumulated while traversing independently cached map levels. */
typedef struct map_render_context {
    map_render_command_t *commands;
    map_annotation_t *annotations;
    SDL_Rect *tiles;
    map_render_candidate_t *candidates;
    map_render_cohort_t *cohorts;
    size_t commands_num;
    size_t commands_capacity;
    size_t annotations_num;
    size_t annotations_capacity;
    size_t tiles_num;
    size_t tiles_capacity;
    size_t candidates_num;
    size_t candidates_capacity;
    size_t cohorts_num;
    size_t cohorts_capacity;
    size_t next_sequence;
    map_cell_t *target_cell;
    SDL_Rect target_rect;
    uint64_t target_identity;
    uint32_t target_generation;
    uint16_t target_revision;
    int16_t target_tile_x;
    int16_t target_tile_y;
    int8_t target_depth;
    uint8_t target_sub_layer;
    bool commands_sorted;
    bool capture_candidates;
} map_render_context_t;

static void map_render_context_sort(map_render_context_t *context);

static void map_benchmark_commands_accumulate(const map_render_command_t *commands,
                                              size_t commands_num,
                                              bool primary_surface,
                                              bool animation_only) {
    bool frame_stretch = false;
    bool frame_living = false;
    bool frame_door = false;
    bool frame_roof = false;
    uint64_t animation_digest = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < commands_num; index++) {
        const map_render_command_t *command = &commands[index];
        bool stretch = command->effects.stretch != 0;
        bool living = command->object_layer == LAYER_LIVING;
        map_benchmark_statistics.stretched_commands += stretch;
        map_benchmark_statistics.double_commands += command->draw_double;
        map_benchmark_statistics.living_commands += living;
        map_benchmark_statistics.door_commands += command->door;
        map_benchmark_statistics.roof_commands += command->roof;
        if (!primary_surface) {
            continue;
        }
        uint16_t depth_bit = UINT16_C(1) << MAP2_DEPTH_INDEX(command->depth);
        map_benchmark_statistics.command_depth_mask |= depth_bit;
        if (living)
            map_benchmark_statistics.living_depth_mask |= depth_bit;
        if (command->door)
            map_benchmark_statistics.door_depth_mask |= depth_bit;
        if (command->roof)
            map_benchmark_statistics.roof_depth_mask |= depth_bit;
        frame_stretch |= stretch;
        frame_living |= living;
        frame_door |= command->door;
        frame_roof |= command->roof;
        if (animation_only && (living || command->door || command->roof)) {
            animation_digest =
                map_cell_hash_bytes(animation_digest, &command->source, sizeof(command->source));
            animation_digest =
                map_cell_hash_bytes(animation_digest, &command->x, sizeof(command->x));
            animation_digest =
                map_cell_hash_bytes(animation_digest, &command->y, sizeof(command->y));
            animation_digest =
                map_cell_hash_bytes(animation_digest, &command->effects, sizeof(command->effects));
        }
    }
    if (primary_surface) {
        map_benchmark_statistics.primary_frames_with_stretch += frame_stretch;
        map_benchmark_statistics.primary_frames_with_living += frame_living;
        map_benchmark_statistics.primary_frames_with_door += frame_door;
        map_benchmark_statistics.primary_frames_with_roof += frame_roof;
    }
    if (animation_only) {
        map_benchmark_statistics.animation_reason_draws++;
        if (map_benchmark_animation_command_digest_valid &&
            animation_digest != map_benchmark_animation_command_digest) {
            map_benchmark_statistics.animation_command_transitions++;
        }
        map_benchmark_animation_command_digest = animation_digest;
        map_benchmark_animation_command_digest_valid = true;
    }
}

/** Stable geometry identity for one visible exit cue member. */
typedef struct map_exit_cue_key {
    int16_t tile_x;
    int16_t tile_y;
    int32_t x;
    int32_t y;
    int32_t elevation;
    int8_t depth;
    int16_t zoom_x;
    int16_t zoom_y;
    int16_t rotate;
    SDL_Surface *source;
    Uint64 source_generation;
    bool draw_double;
} map_exit_cue_key_t;

/** One cached outer perimeter for a connected group of exits. */
typedef struct map_exit_cue {
    SDL_Surface *surface;
    int32_t x;
    int32_t y;
    map_exit_cue_key_t *keys;
    size_t keys_num;
} map_exit_cue_t;

/** Grouped exit geometry retained between a full and animation-only redraw. */
typedef struct map_exit_cue_cache {
    map_exit_cue_t *groups;
    size_t groups_num;
    map_exit_cue_key_t *keys;
    size_t keys_num;
    bool valid;
} map_exit_cue_cache_t;

/** Geometry/effect identity that controls one living-occlusion result. */
typedef struct map_living_outline_key {
    SDL_Surface *source;
    Uint64 source_generation;
    int32_t x;
    int32_t y;
    uint32_t flags;
    uint32_t stretch;
    int16_t zoom_x;
    int16_t zoom_y;
    int16_t rotate;
    uint8_t dark_level;
    uint8_t alpha;
    uint8_t object_layer;
    bool draw_double;
    bool local_player;
    bool fogged;
} map_living_outline_key_t;

/** One retained final outline submitted without rebuilding a CPU effect. */
typedef struct map_living_outline {
    SDL_Surface *surface;
    int32_t x;
    int32_t y;
} map_living_outline_t;

/** Living silhouettes retained while their complete geometry cohort is stable. */
typedef struct map_living_outline_cache {
    map_living_outline_key_t *keys;
    size_t keys_num;
    map_living_outline_t *outlines;
    size_t outlines_num;
    bool valid;
} map_living_outline_cache_t;

static map_exit_cue_cache_t map_animation_exit_cues;
static map_living_outline_cache_t map_animation_living_outlines;

/**
 * Structure used to pass data between the rendering loops in map_draw_map()
 * and the actual rendering logic in draw_map_object().
 *
 * Try to keep this structure aligned whenever extending it.
 */
typedef struct map_render_data {
    int16_t x; ///< X index in the cells array.
    int16_t y; ///< Y index in the cells array.

    int16_t midx; ///< X index in the cells array of the middlemost cell.
    int16_t midy; ///< Y index in the cells array of the middlemost cell.

    int32_t xpos; ///< X coordinate where to render.
    int32_t ypos; ///< Y coordinate where to render.
    int32_t player_height_offset; ///< Player height offset.
    int32_t level_support_height; ///< Ground elevation supporting an upper level.

    map_cell_t *cell; ///< Cell that is being rendered.
    map_render_context_t *render_context; ///< Unified output for every physical level.

    uint8_t layer; ///< Layer to render on.
    uint8_t sub_layer; ///< Sub-layer to render on.
    uint8_t alpha_forced; ///< Force applying the specified alpha value.
    bool smooth_lighting; ///< Whether smooth world lighting is enabled.
    bool lightmap_pending; ///< Whether the ground lightmap has not been composited yet.
    bool defer_rendering; ///< Queue this sprite in the global painter order.
    bool ground_pass; ///< This command belongs to the cached linked-level ground pass.
    bool world_surface; ///< Whether this is a world-rendering surface.
    bool primary_level; ///< Whether this is the player's physical level.
    int8_t depth; ///< Linked-map depth relative to the player.
} map_render_data_t;

/** One map cell that passed the projection and viewport visibility checks. */
typedef struct map_visible_tile {
    int16_t x;
    int16_t y;
    int32_t xpos;
    int32_t ypos;
    int32_t level_support_height;
    map_cell_t *cell;
} map_visible_tile_t;

/**
 * Draw a single object on the map.
 *
 * @param surface
 * Surface to render on.
 * @param data
 * Rendering data. May be modified.
 */
static void draw_map_object(SDL_Surface *surface, map_render_data_t *data) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(data != NULL);

    map_render_context_t *capture = data->render_context;
    if (capture != NULL && capture->capture_candidates) {
        if (capture->candidates_num == capture->candidates_capacity) {
            capture->candidates_capacity =
                capture->candidates_capacity == 0 ? 512 : capture->candidates_capacity * 2;
            capture->candidates = xreallocarray(capture->candidates,
                                                capture->candidates_capacity,
                                                sizeof(*capture->candidates));
        }
        size_t candidate_index = capture->candidates_num++;
        capture->candidates[candidate_index] = (map_render_candidate_t){
            .sequence = candidate_index,
            .x = data->x,
            .y = data->y,
            .midx = data->midx,
            .midy = data->midy,
            .xpos = data->xpos,
            .ypos = data->ypos,
            .player_height_offset = data->player_height_offset,
            .level_support_height = data->level_support_height,
            .layer = data->layer,
            .sub_layer = data->sub_layer,
            .alpha_forced = data->alpha_forced,
            .depth = data->depth,
            .smooth_lighting = data->smooth_lighting,
            .lightmap_pending = data->lightmap_pending,
            .ground_pass = data->ground_pass,
            .primary_level = data->primary_level,
        };
    }

    uint8_t map_layer = GET_MAP_LAYER(data->layer, data->sub_layer);
    bool remembered = map_layer_is_remembered(data->layer);
    uint16_t face = map_object_get_face(data->cell, map_layer);
    if (face == 0 || face >= MAX_FACE_TILES) {
        return;
    }

    sprite_struct *face_sprite = image_get_sprite(face);
    if (face_sprite == NULL || face_sprite->bitmap == NULL) {
        return;
    }

    /* When rendering on the map surface, avoid rendering the object
     * when it's too high up and either in the FoW or the map has the
     * "height difference" feature enabled. */
    if (data->world_surface && ((data->cell->fow && !remembered) || MapData.height_diff) &&
        abs(get_top_floor_height(data->cell, data->sub_layer) - data->player_height_offset) >
            HEIGHT_MAX_RENDER) {
        return;
    }

    int bitmap_h = face_sprite->bitmap->h;
    int bitmap_w = face_sprite->bitmap->w;

    sprite_effects_t effects = {0};
    effects.rotate = map_cell_layer_record_read(data->cell, map_layer)->rotate;
    effects.zoom_x = map_cell_layer_record_read(data->cell, map_layer)->zoom_x;
    effects.zoom_y = map_cell_layer_record_read(data->cell, map_layer)->zoom_y;

    if (effects.rotate != 0) {
        rotozoomSurfaceSizeXY(bitmap_w,
                              bitmap_h,
                              effects.rotate,
                              effects.zoom_x != 0 ? effects.zoom_x / 100.0 : 1.0,
                              effects.zoom_y != 0 ? effects.zoom_y / 100.0 : 1.0,
                              &bitmap_w,
                              &bitmap_h);
    } else if ((effects.zoom_x != 0 && effects.zoom_x != 100) ||
               (effects.zoom_y != 0 && effects.zoom_y != 100)) {
        zoomSurfaceSize(bitmap_w,
                        bitmap_h,
                        effects.zoom_x != 0 ? effects.zoom_x / 100.0 : 1.0,
                        effects.zoom_y != 0 ? effects.zoom_y / 100.0 : 1.0,
                        &bitmap_w,
                        &bitmap_h);
    }

    int xlen;
    int xoff;
    int yl;
    int xl;
    /* Multi-part object? */
    if (map_cell_layer_record_read(data->cell, map_layer)->quick_pos) {
        uint8_t mnr = map_cell_layer_record_read(data->cell, map_layer)->quick_pos;
        uint8_t mid = mnr >> 4;
        mnr &= 0x0f;
        xlen = MultiArchs[mid].xlen;
        yl = data->ypos - MultiArchs[mid].part[mnr].yoff + MultiArchs[mid].ylen - bitmap_h;

        /* Center overlapping X borders */
        xl = 0;
        if (bitmap_w > MultiArchs[mid].xlen) {
            xl = (MultiArchs[mid].xlen - bitmap_w) >> 1;
        }

        xoff = data->xpos - MultiArchs[mid].part[mnr].xoff;
        xl += xoff;
    } else {
        /* Calculate offsets */
        xlen = MAP_TILE_POS_XOFF;
        yl = (data->ypos + MAP_TILE_POS_YOFF) - bitmap_h;
        xoff = xl = data->xpos;

        if (bitmap_w > MAP_TILE_POS_XOFF) {
            xl -= (bitmap_w - MAP_TILE_POS_XOFF) / 2;
        }
    }

    xl += map_cell_layer_record_read(data->cell, map_layer)->align;

    snprintf(VS(effects.glow), "%s", map_cell_layer_record_read(data->cell, map_layer)->glow);
    effects.glow_speed = map_cell_layer_record_read(data->cell, map_layer)->glow_speed;
    effects.glow_state = map_cell_layer_record_read(data->cell, map_layer)->glow_state;

    if (effect_has_overlay()) {
        BIT_SET(effects.flags, SPRITE_FLAG_EFFECTS);
    }

    if (data->cell->fow && !remembered &&
        (!data->cell->structural_fow || data->layer <= LAYER_FMASK)) {
        BIT_SET(effects.flags, SPRITE_FLAG_FOW);
    } else if (map_cell_layer_record_read(data->cell, map_layer)->infravision) {
        BIT_SET(effects.flags, SPRITE_FLAG_RED);
    } else if (map_cell_layer_record_read(data->cell, map_layer)->flags & FFLAG_INVISIBLE) {
        BIT_SET(effects.flags, SPRITE_FLAG_GRAY);
    } else if (data->world_surface && data->smooth_lighting && !data->lightmap_pending) {
        if (map_cell_layer_record_read(data->cell, map_layer)->roof) {
            BIT_SET(effects.flags, SPRITE_FLAG_SMOOTH_DARK_SURFACE);
        } else {
            BIT_SET(effects.flags, SPRITE_FLAG_SMOOTH_DARK);
            effects.smooth_dark_y =
                data->ypos + MAP_TILE_POS_YOFF -
                map_cell_layer_record_read(data->cell, GET_MAP_LAYER(LAYER_FLOOR, data->sub_layer))
                    ->height +
                data->player_height_offset;
        }
    } else if (!data->lightmap_pending) {
        BIT_SET(effects.flags, SPRITE_FLAG_DARK);
    }

    if (!data->world_surface) {
        BITMASK_CLEAR(effects.flags, BIT_MASK(SPRITE_FLAG_RED) | BIT_MASK(SPRITE_FLAG_FOW));
        BIT_SET(effects.flags, SPRITE_FLAG_DARK);
    }

    if (BIT_QUERY(effects.flags, SPRITE_FLAG_DARK)) {
        effects.dark_level =
            (UINT8_MAX - lighting_radiance_to_level(
                             map_cell_light_record_read(data->cell, data->sub_layer)->radiance)) *
            DARK_LEVELS / UINT8_MAX;
    }

    effects.alpha = map_cell_layer_record_read(data->cell, map_layer)->alpha;
    bool transient = map_visibility_transient_layer(data->layer);
    const map_visibility_fade_t *fade =
        &map_cell_layer_record_read(data->cell, map_layer)->visibility;
    if (transient) {
        if (!fade->initialized || fade->alpha == 0) {
            return;
        }
        if (effects.alpha != 0) {
            effects.alpha = MIN(effects.alpha, fade->alpha);
        } else {
            effects.alpha = fade->alpha;
        }
    }

    if (data->alpha_forced != 0) {
        if (effects.alpha != 0) {
            effects.alpha = MIN(effects.alpha, data->alpha_forced);
        } else {
            effects.alpha = data->alpha_forced;
        }
    }

    /* Stretch floor and floor mask layers. */
    if (data->layer <= LAYER_FMASK) {
        effects.stretch = map_cell_sublayer_record_read(data->cell, data->sub_layer)->stretch;
    }

    if (data->layer == LAYER_LIVING || data->layer == LAYER_EFFECT || data->layer == LAYER_ITEM ||
        data->layer == LAYER_ITEM2) {
        yl -= get_top_floor_height(data->cell, data->sub_layer);
    } else {
        yl -= map_cell_layer_record_read(data->cell, GET_MAP_LAYER(LAYER_FLOOR, data->sub_layer))
                  ->height;
    }

    yl += data->player_height_offset;

    /* Move the object up/down depending on its height, but only for
     * non-floor layers. */
    if (data->layer > LAYER_FLOOR) {
        yl -= map_cell_layer_record_read(data->cell, map_layer)->height;
    }

    const map_cell_header_t *cell_header =
        &cells->headers[map_cache_physical_index(data->x, data->y)];
    HARD_ASSERT(data->defer_rendering);
    {
        map_render_context_t *context = data->render_context;
        HARD_ASSERT(context != NULL);
        if (context->commands_num == context->commands_capacity) {
            context->commands_capacity =
                context->commands_capacity == 0 ? 256 : context->commands_capacity * 2;
            context->commands = xreallocarray(context->commands,
                                              context->commands_capacity,
                                              sizeof(*context->commands));
        }
        bool transformed = effects.rotate != 0 || (effects.zoom_x != 0 && effects.zoom_x != 100) ||
                           (effects.zoom_y != 0 && effects.zoom_y != 100);
        int bounds_x = xl;
        int bounds_y = yl;
        int bounds_w = bitmap_w;
        int bounds_h = bitmap_h;
        if (!transformed) {
            bounds_x += face_sprite->border_left;
            bounds_y += face_sprite->border_up;
            bounds_w -= face_sprite->border_left + face_sprite->border_right;
            bounds_h -= face_sprite->border_up + face_sprite->border_down;
        }
        if (map_cell_layer_record_read(data->cell, map_layer)->draw_double) {
            bounds_y -= 22;
            bounds_h += 22;
        }
        context->commands[context->commands_num] = (map_render_command_t){
            .source = face_sprite->bitmap,
            .effects = effects,
            .x = xl,
            .y = yl,
            .bounds_x = bounds_x,
            .bounds_y = bounds_y,
            .bounds_w = MAX(1, bounds_w),
            .bounds_h = MAX(1, bounds_h),
            .sort_x = data->xpos,
            /* Preserve the established world-tile traversal from the top corner
             * down. The physical level's 46-pixel display lift must not move
             * that tile earlier or later in painter order; levels sharing the
             * same world diagonal retain their low-to-high queue sequence. */
            .sort_y =
                data->ypos + data->depth * MAP_LEVEL_PIXEL_HEIGHT + data->level_support_height,
            .sequence =
                context->capture_candidates ? context->candidates_num - 1U : context->next_sequence,
            .record_identity = data->cell->painter_identity,
            .cell_generation = cell_header->generation,
            .cell_revision = cell_header->revision,
            .record_layer = map_layer,
            .tile_x = data->x,
            .tile_y = data->y,
            .object_layer = data->layer,
            .sub_layer = data->sub_layer,
            .depth = data->depth,
            .midx = data->midx,
            .midy = data->midy,
            .projection_y = data->ypos,
            .player_height_offset = data->player_height_offset,
            .level_support_height = data->level_support_height,
            .elevation =
                data->level_support_height +
                map_cell_layer_record_read(data->cell, GET_MAP_LAYER(LAYER_FLOOR, data->sub_layer))
                    ->height +
                (data->layer > LAYER_FLOOR
                     ? map_cell_layer_record_read(data->cell, map_layer)->height
                     : 0),
            .draw_double = map_cell_layer_record_read(data->cell, map_layer)->draw_double,
            .door = (map_cell_sublayer_record_read(data->cell, data->sub_layer)->door &
                     (UINT8_C(1) << (data->layer - 1))) != 0,
            .roof = map_cell_layer_record_read(data->cell, map_layer)->roof != 0,
            .exit = !data->cell->fow &&
                    (map_cell_sublayer_record_read(data->cell, data->sub_layer)->exit &
                     (UINT8_C(1) << (data->layer - 1))) != 0,
            .local_player = data->primary_level && data->x == data->midx && data->y == data->midy &&
                            data->layer == LAYER_LIVING &&
                            data->sub_layer == MIN(MapData.player_sub_layer, NUM_SUB_LAYERS - 1),
            .fogged = data->cell->fow && !remembered,
            .ground = data->ground_pass,
            .smooth_lighting = data->smooth_lighting,
            .primary_level = data->primary_level,
            .transformed = transformed,
        };
        context->next_sequence++;
        context->commands_num++;
    }

    /* Rest of the code deals with rendering on the map widget. */
    if (!data->world_surface) {
        return;
    }

    int xoff2;
    if (xlen == MAP_TILE_POS_XOFF) {
        xoff2 = (int)(((double)xlen / 100.0) * 25.0);
    } else {
        xoff2 = (int)(((double)xlen / 100.0) * 20.0);
    }

    if ((!transient || map_visibility_fade_interactive(fade)) &&
        ((data->layer == LAYER_LIVING &&
          map_cell_actor_record_read(data->cell, data->sub_layer)->name[0] != '\0') ||
         map_cell_layer_record_read(data->cell, map_layer)->flags != 0)) {
        map_render_context_t *context = data->render_context;
        if (context->annotations_num == context->annotations_capacity) {
            context->annotations_capacity =
                context->annotations_capacity == 0 ? 64 : context->annotations_capacity * 2;
            context->annotations = xreallocarray(context->annotations,
                                                 context->annotations_capacity,
                                                 sizeof(*context->annotations));
        }
        map_annotation_t *annotation = &context->annotations[context->annotations_num++];
        *annotation = (map_annotation_t){
            .cell = data->cell,
            .xl = xl,
            .yl = yl,
            .xoff = xoff,
            .xoff2 = xoff2,
            .xlen = xlen,
            .bitmap_w = bitmap_w,
            .record_identity = data->cell->painter_identity,
            .cell_generation = cell_header->generation,
            .cell_revision = cell_header->revision,
            .tile_x = data->x,
            .tile_y = data->y,
            .depth = data->depth,
            .map_layer = map_layer,
            .sub_layer = data->sub_layer,
        };
        annotation->effects.alpha = effects.alpha;
        annotation->effects.stretch = effects.stretch;
        annotation->effects.zoom_x = effects.zoom_x;
        annotation->effects.zoom_y = effects.zoom_y;
        annotation->effects.rotate = effects.rotate;
    }

    if (data->layer == LAYER_FLOOR && tiles_debug) {
        map_render_context_t *context = data->render_context;
        if (context->tiles_num == context->tiles_capacity) {
            context->tiles_capacity =
                context->tiles_capacity == 0 ? 128 : context->tiles_capacity * 2;
            context->tiles =
                xreallocarray(context->tiles, context->tiles_capacity, sizeof(*context->tiles));
        }
        context->tiles[context->tiles_num].x = xl;
        context->tiles[context->tiles_num].y = yl;
        context->tiles[context->tiles_num].w = data->x;
        context->tiles[context->tiles_num].h = data->y;
        context->tiles_num++;
    }

    if (data->primary_level && data->layer == LAYER_LIVING && !data->cell->fow &&
        (!transient || map_visibility_fade_interactive(fade)) &&
        map_cell_actor_record_read(data->cell, data->sub_layer)->probe != 0) {
        map_render_context_t *context = data->render_context;
        context->target_cell = data->cell;
        context->target_identity = data->cell->painter_identity;
        context->target_generation = cell_header->generation;
        context->target_revision = cell_header->revision;
        context->target_tile_x = data->x;
        context->target_tile_y = data->y;
        context->target_depth = data->depth;
        context->target_sub_layer = data->sub_layer;
        context->target_rect.x = xoff + xoff2;
        context->target_rect.y = yl - 9;
        context->target_rect.w = (xlen - xoff2 * 2);
        context->target_rect.h = 1;
    }
}

/** Draw names and status icons after world lighting has been composited. */
static void map_draw_annotations(SDL_Surface *surface, map_render_context_t *context) {
    HARD_ASSERT(context != NULL);

    for (size_t i = 0; i < context->annotations_num; i++) {
        map_annotation_t *annotation = &context->annotations[i];
        map_cell_t *cell = annotation->cell;
        uint8_t map_layer = annotation->map_layer;
        uint8_t sub_layer = annotation->sub_layer;

        if ((map_layer % NUM_LAYERS) + 1 == LAYER_LIVING &&
            map_cell_actor_record_read(cell, sub_layer)->name[0] != '\0' &&
            setting_get_int(OPT_CAT_MAP, OPT_PLAYER_NAMES)) {
            bool draw_name = false;
            const char *name = map_cell_actor_record_read(cell, sub_layer)->name;

            if (setting_get_int(OPT_CAT_MAP, OPT_PLAYER_NAMES) == 1) {
                draw_name = true;
            } else if (setting_get_int(OPT_CAT_MAP, OPT_PLAYER_NAMES) == 2) {
                draw_name = map_cell_actor_record_read(cell, sub_layer)->target_object_count != 0;
            } else if (setting_get_int(OPT_CAT_MAP, OPT_PLAYER_NAMES) == 3) {
                draw_name = map_cell_actor_record_read(cell, sub_layer)->target_object_count == 0;
            }

            if (draw_name) {
#ifdef ATRINIK_WIDGET_TESTS
                if (map_ui_test_active) {
                    map_ui_test_names++;
                }
#endif
                int x = annotation->xoff + annotation->xoff2 +
                        (annotation->xlen - annotation->xoff2 * 2) / 2 -
                        text_get_width(FONT_SANS9, name, 0) / 2 - 2;
                if (surface != NULL) {
                    text_show(surface,
                              FONT_SANS9,
                              name,
                              x,
                              annotation->yl - 24,
                              map_cell_actor_record_read(cell, sub_layer)->color,
                              TEXT_OUTLINE,
                              NULL);
                } else {
                    text_show_root(FONT_SANS9,
                                   name,
                                   x,
                                   annotation->yl - 24,
                                   map_cell_actor_record_read(cell, sub_layer)->color,
                                   TEXT_OUTLINE,
                                   NULL);
                }
            }
        }

        if (map_cell_layer_record_read(cell, map_layer)->flags & FFLAG_SLEEP) {
            surface_show_effects(surface,
                                 annotation->xl + annotation->bitmap_w / 2,
                                 annotation->yl - 5,
                                 NULL,
                                 TEXTURE_CLIENT("sleep"),
                                 &annotation->effects);
        }

        if (map_cell_layer_record_read(cell, map_layer)->flags & FFLAG_CONFUSED) {
            surface_show_effects(surface,
                                 annotation->xl + annotation->bitmap_w / 2 - 1,
                                 annotation->yl - 4,
                                 NULL,
                                 TEXTURE_CLIENT("confused"),
                                 &annotation->effects);
        }

        if (map_cell_layer_record_read(cell, map_layer)->flags & FFLAG_SCARED) {
            surface_show_effects(surface,
                                 annotation->xl + annotation->bitmap_w / 2 + 10,
                                 annotation->yl - 4,
                                 NULL,
                                 TEXTURE_CLIENT("scared"),
                                 &annotation->effects);
        }

        if (map_cell_layer_record_read(cell, map_layer)->flags & FFLAG_BLINDED) {
            surface_show_effects(surface,
                                 annotation->xl + annotation->bitmap_w / 2 + 3,
                                 annotation->yl - 6,
                                 NULL,
                                 TEXTURE_CLIENT("blind"),
                                 &annotation->effects);
        }

        if (map_cell_layer_record_read(cell, map_layer)->flags & FFLAG_PARALYZED) {
            surface_show_effects(surface,
                                 annotation->xl + annotation->bitmap_w / 2 + 3,
                                 annotation->yl + 3,
                                 NULL,
                                 TEXTURE_CLIENT("paralyzed"),
                                 &annotation->effects);
        }
    }

    context->annotations_num = 0;
}

/**
 * Calculates whether the specified coordinates are behind a wall.
 *
 * @param dx
 * Start X.
 * @param dy
 * Start Y.
 * @param sx
 * End X.
 * @param sy
 * End Y.
 * @return
 * Whether the coordinates @p dx and @p dy are behind a wall or not.
 */
static bool obj_is_behind_wall(int dx, int dy, int sx, int sy) {
    int fraction, dx2, dy2, stepx, stepy;
    int x = sx, y = sy;
    int distance_x = dx - sx;
    int distance_y = dy - sy;

    BRESENHAM_INIT(distance_x, distance_y, fraction, stepx, stepy, dx2, dy2);

    while (1) {
        if (x == dx && y == dy) {
            return false;
        }

        if (x < 0 || x >= map_width || y < 0 || y >= map_height) {
            return false;
        }

        for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
            map_cell_t *cell = MAP_CELL_GET_MIDDLE(x, y);

            if (map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_WALL, sub_layer))->face != 0) {
                return true;
            }
        }

        BRESENHAM_STEP(x, y, fraction, stepx, stepy, dx2, dy2);
    }
}

/** Return the base-map elevation that supports a linked level at one tile. */
static int map_level_support_height(int x, int y, int depth) {
    if (depth <= 0) {
        return 0;
    }

    map_cell_store_t *base_cells = level_cells[MAP2_DEPTH_INDEX(0)];
    int cache_width = map_width * MAP_FOW_SIZE;
    int cache_height = map_height * MAP_FOW_SIZE;
    if (base_cells == NULL || x < 0 || x >= cache_width || y < 0 || y >= cache_height) {
        return 0;
    }

    size_t index = map_cache_physical_index(x, y);
    return base_cells->headers[index].support_height;
}

/**
 * Determine if an object being rendered should be culled.
 *
 * @param surface
 * Surface that rendering is being done for.
 * @param data
 * Rendering data.
 * @return
 * Whether the object should be culled.
 */
static bool map_should_cull(SDL_Surface *surface, map_render_data_t *data) {
    /* Determine the distance of the object relative to the PC. */
    int distance_x = data->x - map_width * MAP_FOW_SIZE / 2;
    int distance_y = data->y - map_height * MAP_FOW_SIZE / 2;
    int distance = isqrt(distance_x * distance_x + distance_y * distance_y);
    if (distance > 3) {
        /* Too far away, no culling. */
        return false;
    }

    /* Must be in the southern or eastern quadrant to be culled. */
    if (data->x < map_width * MAP_FOW_SIZE / 2 || data->y < map_height * MAP_FOW_SIZE / 2) {
        return false;
    }

    bool cull = false;
    int range = 2;

    for (int sub_layer2 = NUM_SUB_LAYERS - 1; sub_layer2 > 0; sub_layer2--) {
        int16_t height =
            map_cell_layer_record_read(data->cell, GET_MAP_LAYER(LAYER_EFFECT, sub_layer2))->height;
        if (height - data->player_height_offset > 50) {
            range = 0;
        }
    }

    if (range == 0) {
        cull = true;
    }

    for (int nx = data->x - range; nx <= data->x && !cull; nx++) {
        for (int ny = data->y - range; ny <= data->y && !cull; ny++) {
            map_cell_t *cell2 = MAP_CELL_GET(nx, ny);

            for (int sub_layer2 = 0; sub_layer2 < NUM_SUB_LAYERS; sub_layer2++) {
                if (map_cell_sublayer_record_read(cell2, sub_layer2)->secondpass &
                        (1 << (LAYER_WALL - 1)) &&
                    !obj_is_behind_wall(nx,
                                        ny,
                                        map_width * MAP_FOW_SIZE / 2,
                                        map_height * MAP_FOW_SIZE / 2)) {
                    cull = true;
                    break;
                }
            }
        }
    }

    return cull && range != 0;
}

/**
 * Determine if a specified tile should be rendered.
 *
 * Assigns the cell to be rendered in the map render data structure on success.
 *
 * @param surface
 * Surface rendering is being done on.
 * @param data
 * Map rendering data.
 * @return
 * Whether the tile should be rendered.
 */
static bool map_should_draw(SDL_Surface *surface, map_render_data_t *data) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(data != NULL);

    data->xpos = surface->w / 2 - MAP_TILE_POS_XOFF / 2 + (data->x - data->midx) * MAP_TILE_YOFF -
                 (data->y - data->midy) * MAP_TILE_YOFF;
    data->ypos = surface->h / 2 - MAP_TILE_POS_YOFF / 2 + (data->x - data->midx) * MAP_TILE_XOFF +
                 (data->y - data->midy) * MAP_TILE_XOFF;
    data->level_support_height = map_level_support_height(data->x, data->y, data->depth);
    data->ypos -= data->depth * MAP_LEVEL_PIXEL_HEIGHT;
    data->ypos -= data->level_support_height;

    if (!data->world_surface) {
        data->ypos -= map_width * MAP_TILE_XOFF + map_height * MAP_TILE_XOFF * (MAP_FOW_SIZE / 2);
        data->ypos -= map_height * MAP_TILE_YOFF;
    }

    if (data->xpos > surface->w || data->xpos + MAP_TILE_POS_XOFF < 0 ||
        data->ypos + MAP_TILE_POS_YOFF < 0) {
        return false;
    }

    data->cell = MAP_CELL_GET(data->x, data->y);

    if (data->ypos - data->cell->render_max_height > surface->h) {
        return false;
    }

    return true;
}

/** Build the stable traversal set reused by a level's ground and object passes. */
static map_visible_tile_t *map_visible_tiles_create(SDL_Surface *surface,
                                                    const map_render_data_t *prototype,
                                                    int x,
                                                    int y,
                                                    int w,
                                                    int h,
                                                    size_t *tiles_num) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(prototype != NULL);
    HARD_ASSERT(tiles_num != NULL);

    size_t capacity = (size_t)(w - x) * (size_t)(h - y);
    if (capacity == 0) {
        *tiles_num = 0;
        return NULL;
    }

    map_visible_tile_t *tiles = xmallocarray(capacity, sizeof(*tiles));
    map_render_data_t data = *prototype;
    size_t output = 0;
    for (data.x = x; data.x < w; data.x++) {
        for (data.y = y; data.y < h; data.y++) {
            if (!map_should_draw(surface, &data)) {
                continue;
            }

            tiles[output++] = (map_visible_tile_t){
                .x = data.x,
                .y = data.y,
                .xpos = data.xpos,
                .ypos = data.ypos,
                .level_support_height = data.level_support_height,
                .cell = data.cell,
            };
        }
    }
    *tiles_num = output;
    return tiles;
}

/**
 * Setup the base information in a map render data structure and calculate
 * X/Y cell indexes and maximum dimensions.
 *
 * @param surface
 * Surface rendering is being done for.
 * @param[out] data Map rendering data.
 * @param[out] x Will contain X index of the cell to render. Can be NULL.
 * @param[out] y Will contain Y index of the cell to render. Can be NULL.
 * @param[out] w Maximum width. Can be NULL.
 * @param[out] h Maximum height. Can be NULL.
 */
static void map_setup_render_data(SDL_Surface *surface,
                                  map_render_data_t *data,
                                  int *x,
                                  int *y,
                                  int *w,
                                  int *h) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(data != NULL);

    data->x = map_width - (map_width / 2) - 1;
    data->y = map_height - (map_height / 2) - 1;
    data->cell = MAP_CELL_GET_MIDDLE(data->x, data->y);
    map_cell_store_t *base_cells = level_cells[MAP2_DEPTH_INDEX(0)];
    if (data->world_surface && base_cells != NULL) {
        map_cell_t *base_cell =
            map_cache_cell(base_cells, data->x + MAP_STARTX, data->y + MAP_STARTY);
        data->player_height_offset = get_top_floor_height(base_cell, MapData.player_sub_layer);
    } else {
        data->player_height_offset = get_top_floor_height(data->cell, MapData.player_sub_layer);
    }

    if (data->world_surface) {
        data->midx = map_width * MAP_FOW_SIZE / 2;
        data->midy = map_height * MAP_FOW_SIZE / 2;

        int maxw = surface->w / 2.0 / (MAP_TILE_POS_XOFF / 2.0);
        int maxh = surface->h / 2.0 / (MAP_TILE_POS_YOFF / 2.0);
        int maxtiles = MAX(maxh, maxw);

        if (x != NULL) {
            *x = data->midx - maxtiles;
            if (*x < 0) {
                *x = 0;
            }
        }

        if (y != NULL) {
            *y = data->midy - maxtiles;
            if (*y < 0) {
                *y = 0;
            }
        }

        if (w != NULL) {
            *w = data->midx + maxtiles;
            if (*w > map_width * MAP_FOW_SIZE) {
                *w = map_width * MAP_FOW_SIZE;
            }
        }

        if (h != NULL) {
            *h = data->midy + maxtiles;
            if (*h > map_height * MAP_FOW_SIZE) {
                *h = map_height * MAP_FOW_SIZE;
            }
        }
    } else {
        if (x != NULL) {
            *x = 0;
        }

        if (y != NULL) {
            *y = 0;
        }

        if (w != NULL) {
            *w = map_width * MAP_FOW_SIZE;
        }

        if (h != NULL) {
            *h = map_height * MAP_FOW_SIZE;
        }
    }
}

/** Choose the visible floor, or canonical no-floor sample, representing a cell. */
static uint8_t map_lighting_sub_layer(const map_cell_t *cell) {
    uint8_t selected = MIN(MapData.player_sub_layer, NUM_SUB_LAYERS - 1);
    uint8_t selected_floor_layer = GET_MAP_LAYER(LAYER_FLOOR, selected);
    bool selected_has_floor = map_cell_layer_record_read(cell, selected_floor_layer)->face != 0;
    int selected_height = map_cell_layer_record_read(cell, selected_floor_layer)->height;

    for (uint8_t sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        uint8_t floor_layer = GET_MAP_LAYER(LAYER_FLOOR, sub_layer);
        int height = map_cell_layer_record_read(cell, floor_layer)->height;

        if (map_cell_layer_record_read(cell, floor_layer)->face != 0 &&
            (!selected_has_floor || height >= selected_height)) {
            selected = sub_layer;
            selected_height = height;
            selected_has_floor = true;
        }
    }

    /* Linked roof maps commonly contain only a wall-layer roof object. Their
     * lighting belongs to the canonical object sub-layer, not necessarily the
     * player's base-map sub-layer. Falling back to the latter can select an
     * intentionally empty zero-valued sample and shade the roof fully black. */
    return selected_has_floor ? selected : 0;
}

/** Dirty the lighting projection of one FOW boundary segment and its light halo. */
static void
map_dirty_lighting_fow_segment(size_t level, int depth, int x0, int x1, int y0, int y1) {
    map_cell_store_t *level_cells_current = level_cells[level];
    if (level_cells_current == NULL) {
        return;
    }

    int cache_width = map_width * MAP_FOW_SIZE;
    int cache_height = map_height * MAP_FOW_SIZE;
    x0 = MAX(0, x0 - MAP_LIGHTING_FOW_SEARCH_RADIUS);
    x1 = MIN(cache_width, x1 + MAP_LIGHTING_FOW_SEARCH_RADIUS);
    y0 = MAX(0, y0 - MAP_LIGHTING_FOW_SEARCH_RADIUS);
    y1 = MIN(cache_height, y1 + MAP_LIGHTING_FOW_SEARCH_RADIUS);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    int mid_x = map_width * MAP_FOW_SIZE / 2;
    int mid_y = map_height * MAP_FOW_SIZE / 2;
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    bool found = false;
    for (int x = x0; x < x1; x++) {
        for (int y = y0; y < y1; y++) {
            map_cell_t *cell = map_cache_cell(level_cells_current, x, y);
            uint8_t sub_layer = map_lighting_sub_layer(cell);
            int floor_height = MAX(
                0,
                map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height);
            int support_height = depth > 0 ? map_level_support_height(x, y, depth) : 0;
            int screen_x = (x - mid_x) * MAP_TILE_YOFF - (y - mid_y) * MAP_TILE_YOFF;
            int screen_y = (x - mid_x) * MAP_TILE_XOFF + (y - mid_y) * MAP_TILE_XOFF -
                           floor_height - support_height;
            if (!found) {
                min_x = screen_x;
                min_y = screen_y;
                max_x = screen_x;
                max_y = screen_y;
                found = true;
            } else {
                min_x = MIN(min_x, screen_x);
                min_y = MIN(min_y, screen_y);
                max_x = MAX(max_x, screen_x);
                max_y = MAX(max_y, screen_y);
            }
        }
    }

    if (!found) {
        return;
    }
    lighting_dirty_screen_rect(depth,
                               min_x - MAP_LIGHTING_FOW_MARGIN,
                               min_y - MAP_LIGHTING_FOW_MARGIN,
                               max_x + MAP_LIGHTING_FOW_MARGIN + 1,
                               max_y + MAP_LIGHTING_FOW_MARGIN + 1);
}

/** Dirty a short projected strip without widening its diagonal envelope. */
static void map_dirty_lighting_fow_strip(size_t level, int depth, int x0, int x1, int y0, int y1) {
    for (int x = x0; x < x1; x += MAP_LIGHTING_FOW_SEGMENT) {
        for (int y = y0; y < y1; y += MAP_LIGHTING_FOW_SEGMENT) {
            map_dirty_lighting_fow_segment(level,
                                           depth,
                                           x,
                                           MIN(x1, x + MAP_LIGHTING_FOW_SEGMENT),
                                           y,
                                           MIN(y1, y + MAP_LIGHTING_FOW_SEGMENT));
        }
    }
}

/** Dirty every physical level whose light samples lose FOW-known neighbors. */
static void map_dirty_lighting_fow(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }

    int view_x = map_width * (MAP_FOW_SIZE / 2);
    int view_y = map_height * (MAP_FOW_SIZE / 2);
    int shifted_view_x = view_x - dx;
    int shifted_view_y = view_y - dy;
    int lighting_width = 0;
    int lighting_height = 0;
    int map_projection_width = (map_width + map_height) * MAP_TILE_YOFF;
    int map_projection_height = (map_width + map_height) * MAP_TILE_XOFF;
    bool dirty_incoming_fow = lighting_viewport_size_get(&lighting_width, &lighting_height) &&
                              lighting_width >= map_projection_width &&
                              lighting_height >= map_projection_height;
    for (size_t level = 0; level < arraysize(level_cells); level++) {
        int depth = (int)level - MAP2_MAX_DEPTH;
        /* FOW transitions alter the base lighting field. Linked physical
         * levels consume that field's support geometry and do not own an
         * independent visible FOW boundary. */
        if (depth != 0) {
            continue;
        }
        if (dirty_incoming_fow) {
            if (dx > 0) {
                int boundary_x = shifted_view_x + map_width;
                map_dirty_lighting_fow_strip(level,
                                             depth,
                                             boundary_x,
                                             boundary_x + dx,
                                             shifted_view_y,
                                             shifted_view_y + map_height);
            } else if (dx < 0) {
                int boundary_x = shifted_view_x;
                map_dirty_lighting_fow_strip(level,
                                             depth,
                                             boundary_x,
                                             boundary_x - dx,
                                             shifted_view_y,
                                             shifted_view_y + map_height);
            }
            if (dy > 0) {
                int boundary_y = shifted_view_y + map_height;
                map_dirty_lighting_fow_strip(level,
                                             depth,
                                             shifted_view_x,
                                             shifted_view_x + map_width,
                                             boundary_y,
                                             boundary_y + dy);
            } else if (dy < 0) {
                int boundary_y = shifted_view_y;
                map_dirty_lighting_fow_strip(level,
                                             depth,
                                             shifted_view_x,
                                             shifted_view_x + map_width,
                                             boundary_y,
                                             boundary_y - dy);
            }
        }

        if (dx > 0) {
            for (int y = shifted_view_y; y < shifted_view_y + map_height;
                 y += MAP_LIGHTING_FOW_SEGMENT) {
                map_dirty_lighting_fow_segment(
                    level,
                    depth,
                    shifted_view_x,
                    view_x,
                    y,
                    MIN(shifted_view_y + map_height, y + MAP_LIGHTING_FOW_SEGMENT));
            }
        } else if (dx < 0) {
            for (int y = shifted_view_y; y < shifted_view_y + map_height;
                 y += MAP_LIGHTING_FOW_SEGMENT) {
                map_dirty_lighting_fow_segment(
                    level,
                    depth,
                    view_x + map_width,
                    shifted_view_x + map_width,
                    y,
                    MIN(shifted_view_y + map_height, y + MAP_LIGHTING_FOW_SEGMENT));
            }
        }

        if (dy > 0) {
            for (int x = shifted_view_x; x < shifted_view_x + map_width;
                 x += MAP_LIGHTING_FOW_SEGMENT) {
                map_dirty_lighting_fow_segment(
                    level,
                    depth,
                    x,
                    MIN(shifted_view_x + map_width, x + MAP_LIGHTING_FOW_SEGMENT),
                    shifted_view_y,
                    view_y);
            }
        } else if (dy < 0) {
            for (int x = shifted_view_x; x < shifted_view_x + map_width;
                 x += MAP_LIGHTING_FOW_SEGMENT) {
                map_dirty_lighting_fow_segment(
                    level,
                    depth,
                    x,
                    MIN(shifted_view_x + map_width, x + MAP_LIGHTING_FOW_SEGMENT),
                    view_y + map_height,
                    shifted_view_y + map_height);
            }
        }
    }
}

static uint16_t
map_lighting_interpolate(uint16_t current, uint16_t next, uint64_t progress, uint64_t duration) {
    if (progress == 0 || current == next) {
        return current;
    }
    if (progress >= duration) {
        return next;
    }
    if (next > current) {
        uint64_t delta = (uint64_t)(next - current);
        return (uint16_t)MIN(UINT16_MAX,
                             (uint64_t)current + (delta * progress + duration / 2) / duration);
    }
    uint64_t delta = (uint64_t)(current - next);
    uint64_t reduction = (delta * progress + duration / 2) / duration;
    return (uint16_t)(reduction > current ? 0 : current - reduction);
}

/** Resolve one cell's temporal aggregate endpoint before spatial filtering. */
static bool map_lighting_temporal_sample(const map_cell_t *cell,
                                         uint8_t sub_layer,
                                         uint16_t *scalar,
                                         uint16_t rgb[3]) {
    if (!map_cell_light_keyframe_record_read(cell)->valid ||
        !map_cell_light_record_read(cell, sub_layer)->known ||
        !map_cell_light_record_read(cell, sub_layer)->next_known) {
        return false;
    }

    uint64_t now;
    if (!telemetry_game_time_seconds(&now)) {
        return false;
    }
    uint64_t duration = map_cell_light_keyframe_record_read(cell)->end_seconds -
                        map_cell_light_keyframe_record_read(cell)->start_seconds;
    uint64_t progress = now <= map_cell_light_keyframe_record_read(cell)->start_seconds
                            ? 0
                            : now - map_cell_light_keyframe_record_read(cell)->start_seconds;
    *scalar = map_lighting_interpolate(map_cell_light_record_read(cell, sub_layer)->radiance,
                                       map_cell_light_record_read(cell, sub_layer)->next_radiance,
                                       progress,
                                       duration);
    for (size_t channel = 0; channel < 3; channel++) {
        rgb[channel] = map_lighting_interpolate(
            map_cell_light_record_read(cell, sub_layer)->rgb_radiance[channel],
            map_cell_light_record_read(cell, sub_layer)->next_rgb_radiance[channel],
            progress,
            duration);
    }
    map_benchmark_statistics.temporal_light_samples++;
    return true;
}

/**
 * Resolve a light sample without treating an unseen map-cache cell as dark.
 *
 * Newly exposed cells arrive incrementally after a map scroll. Until their
 * authoritative light values arrive, use the average of the closest known
 * ring. This extends the known field naturally at map and FOW boundaries and
 * prevents temporary dark bands from influencing nearby structures.
 */
static void map_lighting_radiance(int x,
                                  int y,
                                  const map_cell_t *cell,
                                  uint8_t sub_layer,
                                  uint16_t *scalar,
                                  uint16_t rgb[3]) {
    int cache_width = map_width * MAP_FOW_SIZE;
    int cache_height = map_height * MAP_FOW_SIZE;

    if (map_cell_light_record_read(cell, sub_layer)->known) {
        if (!map_lighting_temporal_sample(cell, sub_layer, scalar, rgb)) {
            *scalar = map_cell_light_record_read(cell, sub_layer)->radiance;
            memcpy(rgb,
                   map_cell_light_record_read(cell, sub_layer)->rgb_radiance,
                   sizeof(map_cell_light_record_read(cell, 0)->rgb_radiance));
        }
        return;
    }

    /* The rasterizer includes a two-cell border around the drawable map.
     * Searching one cell beyond that is enough to extend authoritative
     * samples across the boundary without turning cache-key generation into
     * an unbounded nearest-neighbour scan while map data is still arriving. */
    const int search_radius = 3;
    for (int radius = 1; radius <= search_radius; radius++) {
        uint32_t total[4] = {0};
        unsigned int samples = 0;
        unsigned int temporal_samples = 0;

        for (int offset_x = -radius; offset_x <= radius; offset_x++) {
            for (int offset_y = -radius; offset_y <= radius; offset_y++) {
                if (abs(offset_x) != radius && abs(offset_y) != radius) {
                    continue;
                }

                int sample_x = x + offset_x;
                int sample_y = y + offset_y;
                if (sample_x < 0 || sample_x >= cache_width || sample_y < 0 ||
                    sample_y >= cache_height) {
                    continue;
                }

                map_cell_t *sample_cell = MAP_CELL_GET(sample_x, sample_y);
                uint8_t sample_sub_layer = map_lighting_sub_layer(sample_cell);
                if (!map_cell_light_record_read(sample_cell, sample_sub_layer)->known) {
                    continue;
                }

                uint16_t sample_scalar;
                uint16_t sample_rgb[3];
                if (!map_lighting_temporal_sample(sample_cell,
                                                  sample_sub_layer,
                                                  &sample_scalar,
                                                  sample_rgb)) {
                    sample_scalar =
                        map_cell_light_record_read(sample_cell, sample_sub_layer)->radiance;
                    memcpy(sample_rgb,
                           map_cell_light_record_read(sample_cell, sample_sub_layer)->rgb_radiance,
                           sizeof(sample_rgb));
                } else {
                    temporal_samples++;
                }
                total[0] += sample_scalar;
                for (size_t channel = 0; channel < 3; channel++) {
                    total[channel + 1] += sample_rgb[channel];
                }
                samples++;
            }
        }

        if (samples != 0) {
            map_benchmark_statistics.borrowed_light_samples++;
            map_benchmark_statistics.borrowed_temporal_light_samples += temporal_samples != 0;
            *scalar = (uint16_t)((total[0] + samples / 2) / samples);
            for (size_t channel = 0; channel < 3; channel++) {
                rgb[channel] = (uint16_t)((total[channel + 1] + samples / 2) / samples);
            }
            return;
        }
    }

    *scalar = 0;
    memset(rgb, 0, sizeof(uint16_t) * 3);
}

#ifdef ATRINIK_WIDGET_TESTS
bool widget_map_temporal_lighting_test(void) {
    for (size_t level = 0; level < arraysize(level_cells); level++) {
        if (level_cells[level] != NULL) {
            return false;
        }
    }

    uint64_t saved_generation = MapData.light_keyframe_generation;
    uint64_t saved_start = MapData.light_keyframe_start_seconds;
    uint64_t saved_end = MapData.light_keyframe_end_seconds;
    uint8_t saved_flags = MapData.light_keyframe_flags;
    bool saved_valid = MapData.light_keyframe_valid;
    map_width = 3;
    map_height = 3;
    map_cache_origin_x = 0;
    map_cache_origin_y = 0;
    current_level_index = MAP2_DEPTH_INDEX(0);
    map_level_mask = UINT16_C(1) << current_level_index;
    if (!map_select_level(0, true)) {
        return false;
    }

    map_cell_t *neighbor = MAP_CELL_GET_MIDDLE_MUTABLE(1, 0);
    map_cell_light_record_t *light = map_cell_light_record(neighbor, 0, true);
    light->known = 1;
    light->radiance = 100;
    light->next_known = 1;
    light->next_radiance = 300;
    for (size_t channel = 0; channel < 3; channel++) {
        light->rgb_radiance[channel] = 200;
        light->next_rgb_radiance[channel] = 400;
    }
    map_cell_light_keyframe_record_t *keyframe = map_cell_light_keyframe_record(neighbor, true);
    keyframe->generation = 9;
    keyframe->start_seconds = 100;
    keyframe->end_seconds = 200;
    keyframe->valid = 1;

    MapData.light_keyframe_generation = 9;
    MapData.light_keyframe_start_seconds = 100;
    MapData.light_keyframe_end_seconds = 200;
    MapData.light_keyframe_flags = MAP2_LIGHT_KEYFRAME_SNAP;
    MapData.light_keyframe_valid = true;
    telemetry_game_time_sync(150, 60000);
    uint16_t scalar = 0;
    uint16_t rgb[3] = {0};
    map_lighting_radiance(MAP_STARTX, MAP_STARTY, MAP_CELL_GET_MIDDLE(0, 0), 0, &scalar, rgb);
    bool success = scalar == 200 && rgb[0] == 300 && rgb[1] == 300 && rgb[2] == 300;

    uint64_t revision = level_lighting_revision[current_level_index];
    map_temporal_lighting_bucket = UINT64_MAX;
    map_redraw_consume();
    map_temporal_lighting_update();
    success = success && level_lighting_revision[current_level_index] == revision + 1 &&
              (map_redraw_pending_reasons() & MAP_REDRAW_REASON_LIGHTING) != 0;
    map_redraw_consume();

    map_cell_store_destroy(level_cells[current_level_index]);
    level_cells[current_level_index] = NULL;
    cells = NULL;
    map_width = 0;
    map_height = 0;
    map_level_mask = 0;
    map_temporal_lighting_bucket = UINT64_MAX;
    MapData.light_keyframe_generation = saved_generation;
    MapData.light_keyframe_start_seconds = saved_start;
    MapData.light_keyframe_end_seconds = saved_end;
    MapData.light_keyframe_flags = saved_flags;
    MapData.light_keyframe_valid = saved_valid;
    return success;
}
#endif

/** Project one cell and resolve its light sample for the lighting grid. */
static lighting_vertex_t
map_lighting_vertex(SDL_Surface *surface, const map_render_data_t *data, int x, int y) {
    map_cell_t *cell = MAP_CELL_GET(x, y);
    uint8_t sub_layer = map_lighting_sub_layer(cell);
    int height =
        MAX(0, map_cell_layer_record_read(cell, GET_MAP_LAYER(LAYER_FLOOR, sub_layer))->height);

    lighting_vertex_t vertex = {
        .x = surface->w / 2 + (x - data->midx) * MAP_TILE_YOFF - (y - data->midy) * MAP_TILE_YOFF,
        .y = surface->h / 2 + (x - data->midx) * MAP_TILE_XOFF + (y - data->midy) * MAP_TILE_XOFF -
             height + data->player_height_offset - data->depth * MAP_LEVEL_PIXEL_HEIGHT -
             map_level_support_height(x, y, data->depth),
    };
    uint16_t rgb[3];
    map_lighting_radiance(x, y, cell, sub_layer, &vertex.scalar, rgb);
    if (data->primary_level && data->depth == 0 && !cell->fow) {
        uint16_t weight = map_visibility_field_weight(x - data->midx, y - data->midy);
        vertex.scalar = map_visibility_add_player_radiance(vertex.scalar, weight);
        for (size_t channel = 0; channel < 3; channel++) {
            rgb[channel] = map_visibility_add_player_radiance(rgb[channel], weight);
        }
    }
    vertex.red = rgb[0];
    vertex.green = rgb[1];
    vertex.blue = rgb[2];
    return vertex;
}

/** Rasterize and composite the interpolated map light field. */
static void
map_draw_lighting(SDL_Surface *surface, map_render_data_t *data, int x, int y, int w, int h) {
    int cache_width = map_width * MAP_FOW_SIZE;
    int cache_height = map_height * MAP_FOW_SIZE;
    int start_x = MAX(0, x - 2);
    int start_y = MAX(0, y - 2);
    int end_x = MIN(cache_width - 1, w + 1);
    int end_y = MIN(cache_height - 1, h + 1);
    HARD_ASSERT(gpu_renderer_ready());
    int vertex_width = end_x - start_x + 1;
    int vertex_height = end_y - start_y + 1;
    lighting_vertex_t *vertices =
        xmalloc((size_t)vertex_width * (size_t)vertex_height * sizeof(*vertices));
    for (int vertex_x = start_x; vertex_x <= end_x; vertex_x++) {
        for (int vertex_y = start_y; vertex_y <= end_y; vertex_y++) {
            vertices[(size_t)(vertex_x - start_x) * (size_t)vertex_height +
                     (size_t)(vertex_y - start_y)] =
                map_lighting_vertex(surface, data, vertex_x, vertex_y);
        }
    }

    for (int cell_x = start_x; cell_x < end_x; cell_x++) {
        for (int cell_y = start_y; cell_y < end_y; cell_y++) {
            int left = surface->w / 2 + (cell_x - data->midx) * MAP_TILE_YOFF -
                       (cell_y + 1 - data->midy) * MAP_TILE_YOFF;
            int right = surface->w / 2 + (cell_x + 1 - data->midx) * MAP_TILE_YOFF -
                        (cell_y - data->midy) * MAP_TILE_YOFF;
            if (right < 0 || left >= surface->w) {
                continue;
            }

            size_t vertex =
                (size_t)(cell_x - start_x) * (size_t)vertex_height + (size_t)(cell_y - start_y);
            lighting_vertex_t quad[4] = {
                vertices[vertex],
                vertices[vertex + (size_t)vertex_height],
                vertices[vertex + (size_t)vertex_height + 1],
                vertices[vertex + 1],
            };
            gpu_renderer_map_light_quad((uint8_t)MAP2_DEPTH_INDEX(data->depth), quad);
        }
    }
    free(vertices);
}

/** Rebuild only the compact light grid consumed by retained painter commands. */
static void map_draw_retained_lighting(SDL_Surface *surface) {
    if (!setting_get_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING)) {
        return;
    }

    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        uint16_t bit = UINT16_C(1) << MAP2_DEPTH_INDEX(depth);
        if (!(map_level_mask & bit) || !map_select_level(depth, false) ||
            !lighting_select_level(depth)) {
            continue;
        }

        map_render_data_t data = {
            .world_surface = true,
            .primary_level = depth == 0,
            .depth = depth,
        };
        int x, y, w, h;
        map_setup_render_data(surface, &data, &x, &y, &w, &h);
        uint64_t profile_lighting_started = render_profiler_begin();
        map_draw_lighting(surface, &data, x, y, w, h);
        render_profiler_end(RENDER_PROFILE_LIGHTING, profile_lighting_started);
    }
}

static void map_render_cohort_append(map_render_context_t *context,
                                     const map_visible_tile_t *tile,
                                     int depth,
                                     size_t candidates_first,
                                     size_t commands_first,
                                     size_t annotations_first,
                                     size_t tiles_first) {
    if (!context->capture_candidates) {
        return;
    }
    if (context->cohorts_num == context->cohorts_capacity) {
        context->cohorts_capacity =
            context->cohorts_capacity == 0 ? 256 : context->cohorts_capacity * 2;
        context->cohorts =
            xreallocarray(context->cohorts, context->cohorts_capacity, sizeof(*context->cohorts));
    }
    const map_cell_header_t *header = &cells->headers[map_cache_physical_index(tile->x, tile->y)];
    context->cohorts[context->cohorts_num++] = (map_render_cohort_t){
        .candidates_first = candidates_first,
        .candidates_num = context->candidates_num - candidates_first,
        .commands_first = commands_first,
        .commands_num = context->commands_num - commands_first,
        .annotations_first = annotations_first,
        .annotations_num = context->annotations_num - annotations_first,
        .tiles_first = tiles_first,
        .tiles_num = context->tiles_num - tiles_first,
        .record_identity = tile->cell->painter_identity,
        .cell_generation = header->generation,
        .cell_revision = header->revision,
        .tile_x = tile->x,
        .tile_y = tile->y,
        .depth = (int8_t)depth,
    };
}

/**
 * Draw the map.
 *
 * @param surface
 * Surface to render on.
 */
static void map_draw_level(SDL_Surface *surface,
                           SDL_Surface *ground_surface,
                           int depth,
                           bool primary_level,
                           bool primary_surface,
                           bool objects_only,
                           map_render_context_t *render_context) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(gpu_renderer_ready());
    HARD_ASSERT(ground_surface == NULL);

    map_render_data_t data = {
        .world_surface = true,
        .primary_level = primary_level,
        .depth = depth,
        .render_context = render_context,
    };
    int x, y, w, h;
    map_setup_render_data(surface, &data, &x, &y, &w, &h);
    size_t visible_tiles_num = 0;
    map_visible_tile_t *visible_tiles =
        map_visible_tiles_create(surface, &data, x, y, w, h, &visible_tiles_num);
    data.smooth_lighting = primary_surface && setting_get_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING);
    if (data.smooth_lighting && !objects_only) {
        data.lightmap_pending = false;
    }

    if (!objects_only) {
        /* Draw floor and fmasks. */
        data.ground_pass = true;
        uint64_t profile_ground_started = render_profiler_begin();
        for (size_t tile_index = 0; tile_index < visible_tiles_num; tile_index++) {
            const map_visible_tile_t *tile = &visible_tiles[tile_index];
            size_t candidates_first = render_context->candidates_num;
            size_t commands_first = render_context->commands_num;
            size_t annotations_first = render_context->annotations_num;
            size_t tiles_first = render_context->tiles_num;
            data.x = tile->x;
            data.y = tile->y;
            data.xpos = tile->xpos;
            data.ypos = tile->ypos;
            data.level_support_height = tile->level_support_height;
            data.cell = tile->cell;
            for (data.layer = LAYER_FLOOR; data.layer <= LAYER_FMASK; data.layer++) {
                if (map_cell_sublayer_record_read(data.cell, 0)->priority &
                    (1 << (data.layer - 1))) {
                    continue;
                }

                data.defer_rendering = true;
                draw_map_object(surface, &data);
                data.defer_rendering = false;
            }
            map_render_cohort_append(render_context,
                                     tile,
                                     depth,
                                     candidates_first,
                                     commands_first,
                                     annotations_first,
                                     tiles_first);
        }
        render_profiler_end(RENDER_PROFILE_MAP_GROUND, profile_ground_started);

        /* The screen-space lightmap is correct for ground geometry. Elevated
         * sprites project over unrelated cells, so light those using the owning
         * tile's level instead of applying the ground field over them. */
        if (data.smooth_lighting) {
            uint64_t profile_lighting_started = render_profiler_begin();
            map_draw_lighting(surface, &data, x, y, w, h);
            render_profiler_end(RENDER_PROFILE_LIGHTING, profile_lighting_started);
            data.lightmap_pending = false;
        }
        data.ground_pass = false;
    }

    uint8_t floor_layer_pl = GET_MAP_LAYER(LAYER_FLOOR, MapData.player_sub_layer);

    /* Now draw everything else. */
    data.defer_rendering = true;
    uint64_t profile_objects_started = render_profiler_begin();
    for (size_t tile_index = 0; tile_index < visible_tiles_num; tile_index++) {
        const map_visible_tile_t *tile = &visible_tiles[tile_index];
        size_t candidates_first = render_context->candidates_num;
        size_t commands_first = render_context->commands_num;
        size_t annotations_first = render_context->annotations_num;
        size_t tiles_first = render_context->tiles_num;
        data.x = tile->x;
        data.y = tile->y;
        data.xpos = tile->xpos;
        data.ypos = tile->ypos;
        data.level_support_height = tile->level_support_height;
        data.cell = tile->cell;

        for (data.layer = LAYER_FLOOR; data.layer <= NUM_LAYERS; data.layer++) {
            for (data.sub_layer = 0; data.sub_layer < NUM_SUB_LAYERS; data.sub_layer++) {
                if (data.sub_layer == 0 &&
                    (data.layer == LAYER_FLOOR || data.layer == LAYER_FMASK)) {
                    continue;
                }

                /* Skip objects on the effect layer with non-zero sub-layer
                 * because they will be rendered later. */
                if (data.layer == LAYER_EFFECT && data.sub_layer != 0) {
                    uint8_t effect_layer = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer);
                    uint8_t floor_layer = GET_MAP_LAYER(LAYER_FLOOR, MapData.player_sub_layer);
                    if (map_cell_layer_record_read(data.cell, effect_layer)->height >=
                        map_cell_layer_record_read(data.cell, floor_layer)->height) {
                        continue;
                    }
                }

                if (map_cell_sublayer_record_read(data.cell, data.sub_layer)->priority &
                    (1 << (data.layer - 1))) {
                    continue;
                }

                draw_map_object(surface, &data);
            }
        }

        for (data.sub_layer = 0; data.sub_layer < NUM_SUB_LAYERS; data.sub_layer++) {
            uint8_t map_layer = GET_MAP_LAYER(LAYER_FLOOR, data.sub_layer);
            if (map_cell_layer_record_read(data.cell, map_layer)->height >
                map_cell_layer_record_read(data.cell, floor_layer_pl)->height) {
                continue;
            }

            for (data.layer = LAYER_FLOOR; data.layer <= NUM_LAYERS; data.layer++) {
                if (!(map_cell_sublayer_record_read(data.cell, data.sub_layer)->priority &
                      (1 << (data.layer - 1)))) {
                    continue;
                }

                if (data.layer == LAYER_EFFECT && data.sub_layer != 0) {
                    map_layer = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer);
                    if (map_cell_layer_record_read(data.cell, map_layer)->height >=
                        map_cell_layer_record_read(data.cell, floor_layer_pl)->height) {
                        continue;
                    }
                }

                draw_map_object(surface, &data);
            }
        }

        for (data.layer = LAYER_FLOOR; data.layer <= NUM_LAYERS; data.layer++) {
            if (!(map_cell_sublayer_record_read(data.cell, 0)->priority &
                  (1 << (data.layer - 1)))) {
                continue;
            }

            draw_map_object(surface, &data);
        }

        data.layer = LAYER_EFFECT;

        for (data.sub_layer = NUM_SUB_LAYERS - 1; data.sub_layer >= 1; data.sub_layer--) {
            if (map_cell_sublayer_record_read(data.cell, data.sub_layer)->priority &
                (1 << (LAYER_EFFECT - 1))) {
                continue;
            }

            uint8_t map_layer = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer);
            if (map_cell_layer_record_read(data.cell, map_layer)->height <
                map_cell_layer_record_read(data.cell, floor_layer_pl)->height) {
                continue;
            }

            if (data.world_surface && map_should_cull(surface, &data)) {
                continue;
            }

            draw_map_object(surface, &data);
        }

        for (data.sub_layer = 0; data.sub_layer < NUM_SUB_LAYERS; data.sub_layer++) {
            uint8_t map_layer = GET_MAP_LAYER(LAYER_FLOOR, data.sub_layer);
            if (map_cell_layer_record_read(data.cell, map_layer)->height <=
                map_cell_layer_record_read(data.cell, floor_layer_pl)->height) {
                continue;
            }

            for (data.layer = LAYER_FLOOR; data.layer <= NUM_LAYERS; data.layer++) {
                if (!(map_cell_sublayer_record_read(data.cell, data.sub_layer)->priority &
                      (1 << (data.layer - 1)))) {
                    continue;
                }

                draw_map_object(surface, &data);
            }
        }

        data.layer = LAYER_EFFECT;

        for (data.sub_layer = NUM_SUB_LAYERS - 1; data.sub_layer >= 1; data.sub_layer--) {
            uint8_t map_layer = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer);
            if (map_cell_layer_record_read(data.cell, map_layer)->height <
                map_cell_layer_record_read(data.cell, floor_layer_pl)->height) {
                continue;
            }

            uint8_t map_layer2 = GET_MAP_LAYER(LAYER_FLOOR, data.sub_layer - 1);
            if (map_cell_layer_record_read(data.cell, map_layer)->height <=
                map_cell_layer_record_read(data.cell, map_layer2)->height) {
                continue;
            }

            map_layer2 = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer - 1);
            if (map_cell_layer_record_read(data.cell, map_layer)->height <=
                map_cell_layer_record_read(data.cell, map_layer2)->height) {
                continue;
            }

            if (data.world_surface && map_should_cull(surface, &data)) {
                continue;
            }

            draw_map_object(surface, &data);
        }

        if (map_cell_sublayer_record_read(data.cell, 0)->priority & (1 << (LAYER_WALL - 1)) &&
            map_cell_layer_record_read(data.cell, GET_MAP_LAYER(LAYER_WALL, 0))->height > 0) {
            data.layer = LAYER_WALL;
            data.sub_layer = 0;
            draw_map_object(surface, &data);
        }
        map_render_cohort_append(render_context,
                                 tile,
                                 depth,
                                 candidates_first,
                                 commands_first,
                                 annotations_first,
                                 tiles_first);
    }

    /* Preserve the legacy living cue on secondary destinations such as the
     * dynamic minimap. The primary map uses the identity-specific final
     * outline instead. */
    if (primary_level && !primary_surface) {
        for (size_t tile_index = 0; tile_index < visible_tiles_num; tile_index++) {
            const map_visible_tile_t *tile = &visible_tiles[tile_index];
            data.x = tile->x;
            data.y = tile->y;
            data.xpos = tile->xpos;
            data.ypos = tile->ypos;
            data.level_support_height = tile->level_support_height;
            data.cell = tile->cell;

            for (data.sub_layer = NUM_SUB_LAYERS - 1; data.sub_layer >= 1; data.sub_layer--) {
                uint8_t map_layer = GET_MAP_LAYER(LAYER_EFFECT, data.sub_layer);
                if (map_cell_layer_record_read(data.cell, map_layer)->height != 0 &&
                    map_cell_layer_record_read(data.cell, map_layer)->face != 0) {
                    data.cell = NULL;
                    break;
                }
            }

            if (data.cell == NULL) {
                continue;
            }

            data.layer = LAYER_LIVING;
            data.alpha_forced = 100;

            for (data.sub_layer = 0; data.sub_layer < NUM_SUB_LAYERS; data.sub_layer++) {
                draw_map_object(surface, &data);
            }
        }
    }

    render_profiler_end(RENDER_PROFILE_MAP_OBJECTS, profile_objects_started);
    free(visible_tiles);

#undef CALCULATE_POSITIONS
}

/** Sort projected map sprites back-to-front across every physical level. */
static int map_render_command_compare(const void *left_ptr, const void *right_ptr) {
    const map_render_command_t *left = left_ptr;
    const map_render_command_t *right = right_ptr;

    if (left->sort_y != right->sort_y) {
        return left->sort_y < right->sort_y ? -1 : 1;
    }

    if (left->sort_x != right->sort_x) {
        return left->sort_x < right->sort_x ? -1 : 1;
    }

    if (left->sequence != right->sequence) {
        return left->sequence < right->sequence ? -1 : 1;
    }

    return 0;
}

/** Return whether two projected, non-empty sprite bounds overlap. */
static bool map_render_command_overlaps(const map_render_command_t *left,
                                        const map_render_command_t *right) {
    return left->bounds_x < right->bounds_x + right->bounds_w &&
           right->bounds_x < left->bounds_x + left->bounds_w &&
           left->bounds_y < right->bounds_y + right->bounds_h &&
           right->bounds_y < left->bounds_y + left->bounds_h;
}

/** Return whether a later sprite hides the requested share of another sprite. */
static bool map_render_command_covers(const map_render_command_t *covered_command,
                                      const map_render_command_t *occluder,
                                      bool substantial) {
    if (!map_render_command_overlaps(covered_command, occluder)) {
        return false;
    }

    /* Rotated/zoomed sprites are uncommon structural geometry. Their projected
     * bounds remain the safe fallback because mapping a transformed source
     * pixel back exactly would duplicate the rotozoom implementation. */
    if (covered_command->transformed || occluder->transformed) {
        return true;
    }

    bool covered_command_locked = false;
    bool occluder_locked = false;
    if (SDL_MUSTLOCK(covered_command->source)) {
        if (!SDL_LockSurface(covered_command->source)) {
            return false;
        }
        covered_command_locked = true;
    }
    if (occluder->source != covered_command->source && SDL_MUSTLOCK(occluder->source)) {
        if (!SDL_LockSurface(occluder->source)) {
            if (covered_command_locked) {
                SDL_UnlockSurface(covered_command->source);
            }
            return false;
        }
        occluder_locked = true;
    }

    size_t visible_pixels = 0;
    for (int y = 0; y < covered_command->source->h; y++) {
        for (int x = 0; x < covered_command->source->w; x++) {
            visible_pixels += surface_pixel_visible(covered_command->source, x, y);
        }
    }

    bool covered = false;
    int covered_copies = covered_command->draw_double ? 2 : 1;
    int occluder_copies = occluder->draw_double ? 2 : 1;
    for (int covered_copy = 0; covered_copy < covered_copies && !covered; covered_copy++) {
        int covered_y = covered_command->y - covered_copy * 22;
        for (int occluder_copy = 0; occluder_copy < occluder_copies && !covered; occluder_copy++) {
            int occluder_y = occluder->y - occluder_copy * 22;
            int x_start = MAX(covered_command->x, occluder->x);
            int x_end = MIN(covered_command->x + covered_command->source->w,
                            occluder->x + occluder->source->w);
            int y_start = MAX(covered_y, occluder_y);
            int y_end =
                MIN(covered_y + covered_command->source->h, occluder_y + occluder->source->h);
            size_t covered_pixels = 0;

            for (int y = y_start; y < y_end && !covered; y++) {
                for (int x = x_start; x < x_end; x++) {
                    if (!surface_pixel_visible(covered_command->source,
                                               x - covered_command->x,
                                               y - covered_y) ||
                        !surface_pixel_visible(occluder->source, x - occluder->x, y - occluder_y)) {
                        continue;
                    }

                    covered_pixels++;
                    if ((!substantial && covered_pixels != 0) ||
                        (substantial && covered_pixels * 2 >= visible_pixels)) {
                        covered = true;
                        break;
                    }
                }
            }
        }
    }

    if (occluder_locked) {
        SDL_UnlockSurface(occluder->source);
    }
    if (covered_command_locked) {
        SDL_UnlockSurface(covered_command->source);
    }
    return covered;
}

/** Mark nearby doors that are actually covered in the final painter order. */
static void map_render_commands_find_door_hints(map_render_context_t *context) {
    int player_x = map_width * MAP_FOW_SIZE / 2;
    int player_y = map_height * MAP_FOW_SIZE / 2;

    for (size_t door_index = 0; door_index < context->commands_num; door_index++) {
        map_render_command_t *door = &context->commands[door_index];
        if (!door->door || door->depth != 0) {
            continue;
        }

        int distance_x = door->tile_x - player_x;
        int distance_y = door->tile_y - player_y;
        if (distance_x * distance_x + distance_y * distance_y >
            DOOR_HINT_RADIUS * DOOR_HINT_RADIUS) {
            continue;
        }

        /* Only later commands can cover this door in the final painter order.
         * The queue contains every linked physical level, so upper walls and
         * roofs are handled without directional or per-level special cases. */
        for (size_t occluder_index = door_index + 1; occluder_index < context->commands_num;
             occluder_index++) {
            map_render_command_t *occluder = &context->commands[occluder_index];
            if (occluder->object_layer != LAYER_WALL || occluder->door ||
                (occluder->effects.alpha != 0 && occluder->effects.alpha < 128) ||
                !map_render_command_covers(door, occluder, true)) {
                continue;
            }

            door->door_hint = true;
            break;
        }
    }
}

/** Return the geometry-only transformed surface used by the final painter. */
static SDL_Surface *map_render_command_geometry(const map_render_command_t *command) {
    if (!command->transformed) {
        return command->source;
    }

    sprite_effects_t effects = {
        .zoom_x = command->effects.zoom_x,
        .zoom_y = command->effects.zoom_y,
        .rotate = command->effects.rotate,
    };
    return sprite_effects_create(command->source, &effects);
}

/** Add living pixels hidden by one structural sprite to its occlusion mask. */
static bool map_render_command_mask_occlusion(map_render_command_t *living,
                                              SDL_Surface *living_geometry,
                                              const map_render_command_t *occluder) {
    SDL_Surface *occluder_geometry = map_render_command_geometry(occluder);
    if (occluder_geometry == NULL) {
        return false;
    }

    bool living_locked = false;
    bool occluder_locked = false;
    bool mask_locked = false;
    bool added = false;
    if (SDL_MUSTLOCK(living_geometry)) {
        if (!SDL_LockSurface(living_geometry)) {
            goto done;
        }
        living_locked = true;
    }
    if (occluder_geometry != living_geometry && SDL_MUSTLOCK(occluder_geometry)) {
        if (!SDL_LockSurface(occluder_geometry)) {
            goto done;
        }
        occluder_locked = true;
    }
    if (SDL_MUSTLOCK(living->living_occlusion_mask)) {
        if (!SDL_LockSurface(living->living_occlusion_mask)) {
            goto done;
        }
        mask_locked = true;
    }

    Uint32 visible =
        pixel_format_map_rgba(living->living_occlusion_mask->format, 255, 255, 255, 255);
    int living_copies = living->draw_double ? 2 : 1;
    int occluder_copies = occluder->draw_double ? 2 : 1;
    int mask_y = living->y - (living_copies - 1) * 22;
    for (int living_copy = 0; living_copy < living_copies; living_copy++) {
        int living_y = living->y - living_copy * 22;
        for (int occluder_copy = 0; occluder_copy < occluder_copies; occluder_copy++) {
            int occluder_y = occluder->y - occluder_copy * 22;
            int x_start = MAX(living->x, occluder->x);
            int x_end = MIN(living->x + living_geometry->w, occluder->x + occluder_geometry->w);
            int y_start = MAX(living_y, occluder_y);
            int y_end = MIN(living_y + living_geometry->h, occluder_y + occluder_geometry->h);

            for (int y = y_start; y < y_end; y++) {
                for (int x = x_start; x < x_end; x++) {
                    int source_x = x - living->x;
                    int source_y = y - living_y;
                    if (!surface_pixel_visible(living_geometry, source_x, source_y) ||
                        !surface_pixel_visible(occluder_geometry,
                                               x - occluder->x,
                                               y - occluder_y)) {
                        continue;
                    }

                    putpixel(living->living_occlusion_mask, source_x, y - mask_y, visible);
                    added = true;
                }
            }
        }
    }

done:
    if (mask_locked) {
        SDL_UnlockSurface(living->living_occlusion_mask);
    }
    if (occluder_locked) {
        SDL_UnlockSurface(occluder_geometry);
    }
    if (living_locked) {
        SDL_UnlockSurface(living_geometry);
    }
    if (occluder_geometry != occluder->source) {
        SDL_DestroySurface(occluder_geometry);
    }
    return added;
}

/** Return whether structural geometry can hide pixels of a living actor. */
static bool map_render_command_is_living_occluder(const map_render_command_t *living,
                                                  const map_render_command_t *occluder) {
    return occluder->object_layer == LAYER_WALL &&
           !(BIT_QUERY(occluder->effects.flags, SPRITE_FLAG_DARK) &&
             occluder->effects.dark_level == DARK_LEVELS) &&
           (occluder->effects.alpha == 0 || occluder->effects.alpha >= 128) &&
           map_render_command_overlaps(living, occluder);
}

/** Record each living actor's pixels hidden by later structural geometry. */
static void map_render_commands_find_living_occlusion(map_render_context_t *context) {
    for (size_t living_index = 0; living_index < context->commands_num; living_index++) {
        map_render_command_t *living = &context->commands[living_index];
        if (living->object_layer != LAYER_LIVING || living->fogged ||
            (BIT_QUERY(living->effects.flags, SPRITE_FLAG_DARK) &&
             living->effects.dark_level == DARK_LEVELS)) {
            continue;
        }

        size_t occluder_index = living_index + 1;
        while (occluder_index < context->commands_num &&
               !map_render_command_is_living_occluder(living, &context->commands[occluder_index])) {
            occluder_index++;
        }
        if (occluder_index == context->commands_num) {
            continue;
        }

        SDL_Surface *living_geometry = map_render_command_geometry(living);
        if (living_geometry == NULL) {
            continue;
        }
        living->living_occlusion_mask =
            SDL_CreateSurface(living_geometry->w,
                              living_geometry->h + (living->draw_double ? 22 : 0),
                              FormatHolder->format);
        Uint32 transparent =
            living->living_occlusion_mask != NULL
                ? pixel_format_map_rgba(living->living_occlusion_mask->format, 0, 0, 0, 0)
                : 0;
        if (living->living_occlusion_mask == NULL ||
            !surface_fill_rect(living->living_occlusion_mask, NULL, transparent)) {
            SDL_DestroySurface(living->living_occlusion_mask);
            living->living_occlusion_mask = NULL;
            if (living_geometry != living->source) {
                SDL_DestroySurface(living_geometry);
            }
            continue;
        }

        bool living_occluded = false;
        for (; occluder_index < context->commands_num; occluder_index++) {
            const map_render_command_t *occluder = &context->commands[occluder_index];
            if (!map_render_command_is_living_occluder(living, occluder)) {
                continue;
            }

            living_occluded |= map_render_command_mask_occlusion(living, living_geometry, occluder);
        }

        if (living_geometry != living->source) {
            SDL_DestroySurface(living_geometry);
        }
        if (!living_occluded) {
            SDL_DestroySurface(living->living_occlusion_mask);
            living->living_occlusion_mask = NULL;
        }
    }
}

/** Return whether a command affects a living silhouette cache result. */
static bool map_living_outline_command(const map_render_command_t *command) {
    return command->object_layer == LAYER_LIVING || command->object_layer == LAYER_WALL;
}

/** Capture the exact command fields used by occlusion geometry and selection. */
static map_living_outline_key_t map_living_outline_key(const map_render_command_t *command) {
    return (map_living_outline_key_t){
        .source = command->source,
        .source_generation = gpu_renderer_surface_generation(command->source),
        .x = command->x,
        .y = command->y,
        .flags = command->effects.flags,
        .stretch = command->effects.stretch,
        .zoom_x = command->effects.zoom_x,
        .zoom_y = command->effects.zoom_y,
        .rotate = command->effects.rotate,
        .dark_level = command->effects.dark_level,
        .alpha = command->effects.alpha,
        .object_layer = command->object_layer,
        .draw_double = command->draw_double,
        .local_player = command->local_player,
        .fogged = command->fogged,
    };
}

/** Compare cache keys field-by-field without depending on structure padding. */
static bool map_living_outline_key_equal(const map_living_outline_key_t *left,
                                         const map_living_outline_key_t *right) {
    return left->source == right->source && left->source_generation == right->source_generation &&
           left->x == right->x && left->y == right->y && left->flags == right->flags &&
           left->stretch == right->stretch && left->zoom_x == right->zoom_x &&
           left->zoom_y == right->zoom_y && left->rotate == right->rotate &&
           left->dark_level == right->dark_level && left->alpha == right->alpha &&
           left->object_layer == right->object_layer && left->draw_double == right->draw_double &&
           left->local_player == right->local_player && left->fogged == right->fogged;
}

/** Release retained silhouettes and their complete geometry identity. */
static void map_living_outline_cache_clear(map_living_outline_cache_t *cache) {
    for (size_t i = 0; i < cache->outlines_num; i++) {
        SDL_DestroySurface(cache->outlines[i].surface);
    }
    free(cache->outlines);
    free(cache->keys);
    memset(cache, 0, sizeof(*cache));
}

/** Collect the ordered cohort whose geometry determines all living masks. */
static map_living_outline_key_t *map_living_outline_keys_create(const map_render_context_t *context,
                                                                size_t *keys_num) {
    size_t count = 0;
    for (size_t i = 0; i < context->commands_num; i++) {
        count += map_living_outline_command(&context->commands[i]);
    }
    map_living_outline_key_t *keys = count == 0 ? NULL : xmallocarray(count, sizeof(*keys));
    size_t output = 0;
    for (size_t i = 0; i < context->commands_num; i++) {
        if (map_living_outline_command(&context->commands[i])) {
            keys[output++] = map_living_outline_key(&context->commands[i]);
        }
    }
    *keys_num = count;
    return keys;
}

/** Return whether retained silhouettes describe this complete painter cohort. */
static bool map_living_outline_cache_matches(const map_living_outline_cache_t *cache,
                                             const map_render_context_t *context) {
    if (!cache->valid) {
        return false;
    }
    size_t keys_num;
    map_living_outline_key_t *keys = map_living_outline_keys_create(context, &keys_num);
    bool matches = keys_num == cache->keys_num;
    for (size_t i = 0; matches && i < keys_num; i++) {
        matches = map_living_outline_key_equal(&keys[i], &cache->keys[i]);
    }
    free(keys);
    return matches;
}

/** Consume newly computed masks into retained final outline surfaces. */
static void map_living_outline_cache_build(map_living_outline_cache_t *cache,
                                           map_render_context_t *context) {
    map_living_outline_cache_clear(cache);
    cache->keys = map_living_outline_keys_create(context, &cache->keys_num);
    for (size_t i = 0; i < context->commands_num; i++) {
        map_render_command_t *command = &context->commands[i];
        if (command->living_occlusion_mask == NULL) {
            continue;
        }
        SDL_Color color;
        const char *outline_color =
            command->local_player ? MAP_OUTLINE_COLOR : MAP_LIVING_OUTLINE_COLOR;
        SDL_Surface *outline = NULL;
        if (text_color_parse(outline_color, &color)) {
            outline = sprite_outline_create(command->living_occlusion_mask, &color);
        }
        SDL_DestroySurface(command->living_occlusion_mask);
        command->living_occlusion_mask = NULL;
        if (outline == NULL) {
            continue;
        }
        cache->outlines =
            xreallocarray(cache->outlines, cache->outlines_num + 1, sizeof(*cache->outlines));
        cache->outlines[cache->outlines_num++] = (map_living_outline_t){
            .surface = outline,
            .x = command->x - SPRITE_GLOW_SIZE,
            .y = command->y - (command->draw_double ? 22 : 0) - SPRITE_GLOW_SIZE,
        };
    }
    cache->valid = true;
}

/** Submit retained living silhouettes after the world painter pass. */
static void map_living_outline_cache_draw(SDL_Surface *surface,
                                          const map_living_outline_cache_t *cache) {
    for (size_t i = 0; i < cache->outlines_num; i++) {
        surface_show(surface,
                     cache->outlines[i].x,
                     cache->outlines[i].y,
                     NULL,
                     cache->outlines[i].surface);
    }
}

/** Return whether a command participates in the visible exit cue. */
static bool map_exit_cue_command(const map_render_command_t *command) {
    return command->exit && command->depth == 0;
}

/** Copy the geometry identity used to reuse grouped exit cues. */
static map_exit_cue_key_t map_exit_cue_key(const map_render_command_t *command) {
    return (map_exit_cue_key_t){
        .tile_x = command->tile_x,
        .tile_y = command->tile_y,
        .x = command->x,
        .y = command->y,
        .elevation = command->elevation,
        .depth = command->depth,
        .zoom_x = command->effects.zoom_x,
        .zoom_y = command->effects.zoom_y,
        .rotate = command->effects.rotate,
        .source = command->source,
        .source_generation = gpu_renderer_surface_generation(command->source),
        .draw_double = command->draw_double,
    };
}

/** Compare two exit cue keys without relying on pointer ordering. */
static int map_exit_cue_key_compare(const void *left_ptr, const void *right_ptr) {
    const map_exit_cue_key_t *left = left_ptr;
    const map_exit_cue_key_t *right = right_ptr;

#define COMPARE_KEY_FIELD(_field)                     \
    if (left->_field != right->_field) {              \
        return left->_field < right->_field ? -1 : 1; \
    }
    COMPARE_KEY_FIELD(tile_x);
    COMPARE_KEY_FIELD(tile_y);
    COMPARE_KEY_FIELD(elevation);
    COMPARE_KEY_FIELD(depth);
    COMPARE_KEY_FIELD(x);
    COMPARE_KEY_FIELD(y);
    COMPARE_KEY_FIELD(zoom_x);
    COMPARE_KEY_FIELD(zoom_y);
    COMPARE_KEY_FIELD(rotate);
    if (left->source != right->source) {
        uintptr_t left_source = (uintptr_t)left->source;
        uintptr_t right_source = (uintptr_t)right->source;
        return left_source < right_source ? -1 : 1;
    }
    COMPARE_KEY_FIELD(source_generation);
    COMPARE_KEY_FIELD(draw_double);
#undef COMPARE_KEY_FIELD
    return 0;
}

/** Compare two already canonicalized exit cue keys. */
static bool map_exit_cue_key_equal(const map_exit_cue_key_t *left,
                                   const map_exit_cue_key_t *right) {
    return map_exit_cue_key_compare(left, right) == 0;
}

/** Release cached grouped cue masks and their geometry identities. */
static void map_exit_cue_cache_clear(map_exit_cue_cache_t *cache) {
    for (size_t i = 0; i < cache->groups_num; i++) {
        SDL_DestroySurface(cache->groups[i].surface);
        free(cache->groups[i].keys);
    }
    free(cache->groups);
    free(cache->keys);
    memset(cache, 0, sizeof(*cache));
}

/** Return whether two visible exits belong to one cardinally connected group. */
static bool map_exit_cue_commands_linked(const map_render_command_t *left,
                                         const map_render_command_t *right) {
    if (!map_exit_cue_command(left) || !map_exit_cue_command(right) ||
        left->elevation != right->elevation || left->depth != right->depth) {
        return false;
    }

    int distance_x = abs(left->tile_x - right->tile_x);
    int distance_y = abs(left->tile_y - right->tile_y);
    return distance_x + distance_y <= 1;
}

/** Collect the sorted identities for all visible depth-zero exit commands. */
static map_exit_cue_key_t *map_exit_cue_keys_create(const map_render_context_t *context,
                                                    size_t *keys_num) {
    size_t count = 0;
    for (size_t i = 0; i < context->commands_num; i++) {
        count += map_exit_cue_command(&context->commands[i]);
    }

    map_exit_cue_key_t *keys = count == 0 ? NULL : xmallocarray(count, sizeof(*keys));
    size_t output = 0;
    for (size_t i = 0; i < context->commands_num; i++) {
        if (map_exit_cue_command(&context->commands[i])) {
            keys[output++] = map_exit_cue_key(&context->commands[i]);
        }
    }
    if (count > 1) {
        qsort(keys, count, sizeof(*keys), map_exit_cue_key_compare);
    }
    *keys_num = count;
    return keys;
}

/** Return whether the cached cue masks still describe this animation frame. */
static bool map_exit_cue_cache_matches(const map_exit_cue_cache_t *cache,
                                       const map_render_context_t *context) {
    if (!cache->valid) {
        return false;
    }

    size_t keys_num;
    map_exit_cue_key_t *keys = map_exit_cue_keys_create(context, &keys_num);
    bool matches = keys_num == cache->keys_num;
    for (size_t i = 0; matches && i < keys_num; i++) {
        matches = map_exit_cue_key_equal(&keys[i], &cache->keys[i]);
    }
    free(keys);
    return matches;
}

/** Copy one transformed sprite silhouette into a shared group mask. */
static bool map_exit_cue_copy_geometry(SDL_Surface *mask,
                                       int32_t mask_x,
                                       int32_t mask_y,
                                       const map_render_command_t *command,
                                       SDL_Surface *geometry) {
    bool geometry_locked = false;
    bool mask_locked = false;
    if (SDL_MUSTLOCK(geometry)) {
        if (!SDL_LockSurface(geometry)) {
            return false;
        }
        geometry_locked = true;
    }
    if (SDL_MUSTLOCK(mask)) {
        if (!SDL_LockSurface(mask)) {
            if (geometry_locked) {
                SDL_UnlockSurface(geometry);
            }
            return false;
        }
        mask_locked = true;
    }

    Uint32 visible = pixel_format_map_rgba(mask->format, 255, 255, 255, 255);
    int copies = command->draw_double ? 2 : 1;
    for (int copy = 0; copy < copies; copy++) {
        int32_t source_y = command->y - copy * 22;
        for (int y = 0; y < geometry->h; y++) {
            for (int x = 0; x < geometry->w; x++) {
                if (surface_pixel_visible(geometry, x, y)) {
                    putpixel(mask, command->x + x - mask_x, source_y + y - mask_y, visible);
                }
            }
        }
    }

    if (mask_locked) {
        SDL_UnlockSurface(mask);
    }
    if (geometry_locked) {
        SDL_UnlockSurface(geometry);
    }
    return true;
}

/** Build one shared alpha mask for a connected exit component. */
static bool map_exit_cue_group_build(map_exit_cue_t *group,
                                     const map_render_context_t *context,
                                     const size_t *indices,
                                     size_t indices_num) {
    SDL_Surface **geometries = xcalloc(indices_num, sizeof(*geometries));
    int32_t minimum_x = INT32_MAX;
    int32_t minimum_y = INT32_MAX;
    int32_t maximum_x = INT32_MIN;
    int32_t maximum_y = INT32_MIN;
    bool success = true;

    for (size_t i = 0; i < indices_num; i++) {
        const map_render_command_t *command = &context->commands[indices[i]];
        geometries[i] = map_render_command_geometry(command);
        if (geometries[i] == NULL) {
            success = false;
            break;
        }
        minimum_x = MIN(minimum_x, command->x);
        minimum_y = MIN(minimum_y, command->y - (command->draw_double ? 22 : 0));
        maximum_x = MAX(maximum_x, command->x + geometries[i]->w);
        maximum_y = MAX(maximum_y, command->y + geometries[i]->h);
    }
    if (!success || minimum_x == INT32_MAX || maximum_x <= minimum_x || maximum_y <= minimum_y) {
        for (size_t i = 0; i < indices_num; i++) {
            if (geometries[i] != NULL && geometries[i] != context->commands[indices[i]].source) {
                SDL_DestroySurface(geometries[i]);
            }
        }
        free(geometries);
        return false;
    }

    SDL_Surface *mask =
        SDL_CreateSurface(maximum_x - minimum_x, maximum_y - minimum_y, FormatHolder->format);
    if (mask == NULL || !surface_set_transparent_black_mutable(mask) ||
        !surface_clear_transparent_black(mask)) {
        SDL_DestroySurface(mask);
        for (size_t i = 0; i < indices_num; i++) {
            if (geometries[i] != NULL && geometries[i] != context->commands[indices[i]].source) {
                SDL_DestroySurface(geometries[i]);
            }
        }
        free(geometries);
        return false;
    }

    for (size_t i = 0; i < indices_num && success; i++) {
        success = map_exit_cue_copy_geometry(mask,
                                             minimum_x,
                                             minimum_y,
                                             &context->commands[indices[i]],
                                             geometries[i]);
    }
    for (size_t i = 0; i < indices_num; i++) {
        if (geometries[i] != context->commands[indices[i]].source) {
            SDL_DestroySurface(geometries[i]);
        }
    }
    free(geometries);
    if (!success) {
        SDL_DestroySurface(mask);
        return false;
    }

    SDL_Color color;
    if (!text_color_parse(MAP_OUTLINE_COLOR, &color)) {
        SDL_DestroySurface(mask);
        return false;
    }
    group->surface = sprite_outline_create(mask, &color);
    SDL_DestroySurface(mask);
    if (group->surface == NULL) {
        return false;
    }
    group->x = minimum_x;
    group->y = minimum_y;
    group->keys_num = indices_num;
    group->keys = xmallocarray(indices_num, sizeof(*group->keys));
    for (size_t i = 0; i < indices_num; i++) {
        group->keys[i] = map_exit_cue_key(&context->commands[indices[i]]);
    }
    if (indices_num > 1) {
        qsort(group->keys, indices_num, sizeof(*group->keys), map_exit_cue_key_compare);
    }
    return true;
}

/** Build grouped exit masks from the current sorted painter commands. */
static bool map_exit_cue_cache_build(map_exit_cue_cache_t *cache,
                                     const map_render_context_t *context) {
    map_exit_cue_cache_clear(cache);
    cache->keys = map_exit_cue_keys_create(context, &cache->keys_num);

    bool *assigned = xcalloc(context->commands_num, sizeof(*assigned));
    for (size_t start = 0; start < context->commands_num; start++) {
        if (assigned[start] || !map_exit_cue_command(&context->commands[start])) {
            continue;
        }

        size_t indices_num = 1;
        size_t indices_capacity = 8;
        size_t *indices = xmallocarray(indices_capacity, sizeof(*indices));
        indices[0] = start;
        assigned[start] = true;
        for (size_t member = 0; member < indices_num; member++) {
            for (size_t candidate = 0; candidate < context->commands_num; candidate++) {
                if (assigned[candidate] ||
                    !map_exit_cue_commands_linked(&context->commands[indices[member]],
                                                  &context->commands[candidate])) {
                    continue;
                }
                if (indices_num == indices_capacity) {
                    indices_capacity *= 2;
                    indices = xreallocarray(indices, indices_capacity, sizeof(*indices));
                }
                indices[indices_num++] = candidate;
                assigned[candidate] = true;
            }
        }

        map_exit_cue_t group = {0};
        bool success = map_exit_cue_group_build(&group, context, indices, indices_num);
        free(indices);
        if (!success) {
            free(assigned);
            map_exit_cue_cache_clear(cache);
            return false;
        }
        cache->groups = xreallocarray(cache->groups, cache->groups_num + 1, sizeof(*cache->groups));
        cache->groups[cache->groups_num++] = group;
    }
    free(assigned);
    cache->valid = true;
    return true;
}

/** Draw all cached grouped exit perimeters after the world painter pass. */
static void map_exit_cue_cache_draw(SDL_Surface *surface, const map_exit_cue_cache_t *cache) {
    for (size_t i = 0; i < cache->groups_num; i++) {
        surface_show(surface,
                     cache->groups[i].x - SPRITE_GLOW_SIZE,
                     cache->groups[i].y - SPRITE_GLOW_SIZE,
                     NULL,
                     cache->groups[i].surface);
    }
}

/** Preserve the old single-sprite cue as an allocation-failure fallback. */
static void map_render_command_draw_exit(SDL_Surface *surface,
                                         const map_render_command_t *command) {
    sprite_effects_t effects = {0};
    effects.zoom_x = command->effects.zoom_x;
    effects.zoom_y = command->effects.zoom_y;
    effects.rotate = command->effects.rotate;
    snprintf(VS(effects.outline), "%s", MAP_OUTLINE_COLOR);
    surface_show_effects(surface, command->x, command->y, NULL, command->source, &effects);
    if (command->draw_double) {
        surface_show_effects(surface, command->x, command->y - 22, NULL, command->source, &effects);
    }
}

/** Paint all projected sprites in one isometric order. */
static void map_render_commands(SDL_Surface *surface,
                                map_render_context_t *context,
                                bool primary_surface,
                                map_exit_cue_cache_t *exit_cues) {
    HARD_ASSERT(surface == NULL);
    uint64_t profile_paint_started = render_profiler_begin();
    uint64_t profile_sort_started = render_profiler_begin();
    map_render_context_sort(context);
    render_profiler_end(RENDER_PROFILE_MAP_COMMAND_SORT, profile_sort_started);

    if (primary_surface) {
        uint64_t profile_hint_started = render_profiler_begin();
        map_render_commands_find_door_hints(context);
        render_profiler_end(RENDER_PROFILE_MAP_HINT_REPLAY, profile_hint_started);
        if (!map_living_outline_cache_matches(&map_animation_living_outlines, context)) {
            uint64_t profile_occlusion_started = render_profiler_begin();
            map_render_commands_find_living_occlusion(context);
            map_living_outline_cache_build(&map_animation_living_outlines, context);
            render_profiler_end(RENDER_PROFILE_MAP_LIVING_OCCLUSION, profile_occlusion_started);
        }
    }

    bool grouped_exit_cues = false;
    if (primary_surface && exit_cues != NULL) {
        if (!map_exit_cue_cache_matches(exit_cues, context)) {
            grouped_exit_cues = map_exit_cue_cache_build(exit_cues, context);
        } else {
            grouped_exit_cues = true;
        }
    }

    uint64_t profile_effects_started = render_profiler_begin();
    int selected_depth = MAP2_MAX_DEPTH + 1;
    for (size_t i = 0; i < context->commands_num; i++) {
        map_render_command_t *command = &context->commands[i];
        if (selected_depth != command->depth) {
            SOFT_ASSERT(lighting_select_level(command->depth),
                        "Could not select lighting context for depth %d",
                        command->depth);
            selected_depth = command->depth;
        }

        bool scene_lit = BIT_QUERY(command->effects.flags, SPRITE_FLAG_SMOOTH_DARK) ||
                         BIT_QUERY(command->effects.flags, SPRITE_FLAG_SMOOTH_DARK_SURFACE);
        gpu_renderer_map_set_owner(scene_lit ? (uint8_t)MAP2_DEPTH_INDEX(command->depth)
                                             : GPU_RENDERER_OWNER_UNLIT,
                                   command->effects.smooth_dark_y);
        gpu_renderer_map_set_instance_identity(command->record_identity,
                                               (uint32_t)command->record_layer << 2U);
        surface_show_effects(surface,
                             command->x,
                             command->y,
                             NULL,
                             command->source,
                             &command->effects);
        if (command->draw_double) {
            gpu_renderer_map_set_instance_identity(command->record_identity,
                                                   ((uint32_t)command->record_layer << 2U) | 1U);
            surface_show_effects(surface,
                                 command->x,
                                 command->y - 22,
                                 NULL,
                                 command->source,
                                 &command->effects);
        }
    }
    render_profiler_end(RENDER_PROFILE_MAP_SPRITE_EFFECTS, profile_effects_started);

    gpu_renderer_map_set_owner(GPU_RENDERER_OWNER_UNLIT, 0);

    if (primary_surface) {
        /* Draw grouped cues after the world painter, matching the legacy
         * per-sprite cue phase so later sprites cannot erase the perimeter. */
        if (grouped_exit_cues) {
            map_exit_cue_cache_draw(surface, exit_cues);
        }
        map_living_outline_cache_draw(surface, &map_animation_living_outlines);

        for (size_t i = 0; i < context->commands_num; i++) {
            map_render_command_t *command = &context->commands[i];
            if (!command->door_hint) {
                continue;
            }

            sprite_effects_t effects = {0};
            if (command->door_hint) {
                effects.zoom_x = command->effects.zoom_x;
                effects.zoom_y = command->effects.zoom_y;
                effects.rotate = command->effects.rotate;
                snprintf(VS(effects.outline), "%s", MAP_OUTLINE_COLOR);
                gpu_renderer_map_set_instance_identity(command->record_identity,
                                                       ((uint32_t)command->record_layer << 2U) |
                                                           2U);
                surface_show_effects(surface,
                                     command->x,
                                     command->y,
                                     NULL,
                                     command->source,
                                     &effects);
                if (command->draw_double) {
                    gpu_renderer_map_set_instance_identity(command->record_identity,
                                                           ((uint32_t)command->record_layer << 2U) |
                                                               3U);
                    surface_show_effects(surface,
                                         command->x,
                                         command->y - 22,
                                         NULL,
                                         command->source,
                                         &effects);
                }
            }
        }

        if (!grouped_exit_cues) {
            for (size_t i = 0; i < context->commands_num; i++) {
                if (map_exit_cue_command(&context->commands[i])) {
                    map_render_command_draw_exit(surface, &context->commands[i]);
                }
            }
        }
    }

    context->commands_num = 0;
    render_profiler_end(RENDER_PROFILE_MAP_PAINT, profile_paint_started);
}

/** Draw map annotations and target UI after the unified world pass. */
static void map_draw_ui(SDL_Surface *surface, map_render_context_t *context) {
    uint64_t profile_ui_started = render_profiler_begin();
    map_draw_annotations(surface, context);

    for (size_t i = 0; i < context->tiles_num; i++) {
        SDL_Rect box = {
            .x = context->tiles[i].x,
            .y = context->tiles[i].y,
            .w = MAP_TILE_POS_XOFF,
            .h = MAP_TILE_POS_YOFF,
        };
        if (surface != NULL) {
            text_show_format(surface,
                             FONT("arial", 9),
                             box.x,
                             box.y,
                             COLOR_WHITE,
                             TEXT_OUTLINE | TEXT_VALIGN_CENTER | TEXT_ALIGN_CENTER,
                             &box,
                             "%d,%d",
                             context->tiles[i].w,
                             context->tiles[i].h);
        } else {
            text_show_format_root(FONT("arial", 9),
                                  box.x,
                                  box.y,
                                  COLOR_WHITE,
                                  TEXT_OUTLINE | TEXT_VALIGN_CENTER | TEXT_ALIGN_CENTER,
                                  &box,
                                  "%d,%d",
                                  context->tiles[i].w,
                                  context->tiles[i].h);
        }
    }
    context->tiles_num = 0;

    if (context->target_cell != NULL && cpl.target_code != 0) {
#ifdef ATRINIK_WIDGET_TESTS
        if (map_ui_test_active) {
            map_ui_test_targets++;
        }
#endif
        const char *hp_color;

        if (cpl.target_hp > 90) {
            hp_color = COLOR_GREEN;
        } else if (cpl.target_hp > 75) {
            hp_color = COLOR_DGOLD;
        } else if (cpl.target_hp > 50) {
            hp_color = COLOR_HGOLD;
        } else if (cpl.target_hp > 25) {
            hp_color = COLOR_YELLOW;
        } else if (cpl.target_hp > 10) {
            hp_color = COLOR_ORANGE;
        } else {
            hp_color = COLOR_RED;
        }

        if (!(setting_get_int(OPT_CAT_MAP, OPT_PLAYER_NAMES) &&
              map_cell_actor_record_read(context->target_cell, context->target_sub_layer)
                      ->name[0] != '\0')) {
            int x = context->target_rect.x + context->target_rect.w / 2 -
                    text_get_width(FONT_SANS9, cpl.target_name, 0) / 2;
            if (surface != NULL) {
                text_show(surface,
                          FONT_SANS9,
                          cpl.target_name,
                          x,
                          context->target_rect.y - 15,
                          cpl.target_color,
                          TEXT_OUTLINE,
                          NULL);
            } else {
                text_show_root(FONT_SANS9,
                               cpl.target_name,
                               x,
                               context->target_rect.y - 15,
                               cpl.target_color,
                               TEXT_OUTLINE,
                               NULL);
            }
        }

        rectangle_create(surface,
                         context->target_rect.x - 2,
                         context->target_rect.y - 2,
                         1,
                         5,
                         hp_color);
        rectangle_create(surface,
                         context->target_rect.x - 2,
                         context->target_rect.y - 2,
                         3,
                         1,
                         hp_color);
        rectangle_create(surface,
                         context->target_rect.x - 2,
                         context->target_rect.y + 2,
                         3,
                         1,
                         hp_color);
        rectangle_create(surface,
                         context->target_rect.x + context->target_rect.w + 1,
                         context->target_rect.y - 2,
                         1,
                         5,
                         hp_color);
        rectangle_create(surface,
                         context->target_rect.x + context->target_rect.w - 1,
                         context->target_rect.y - 2,
                         3,
                         1,
                         hp_color);
        rectangle_create(surface,
                         context->target_rect.x + context->target_rect.w - 1,
                         context->target_rect.y + 2,
                         3,
                         1,
                         hp_color);

        context->target_rect.w =
            context->target_rect.w / 100.0 *
            map_cell_actor_record_read(context->target_cell, context->target_sub_layer)->probe;
        context->target_rect.w = MAX(1, MIN(100, context->target_rect.w));
        rectangle_create(surface,
                         context->target_rect.x,
                         context->target_rect.y,
                         context->target_rect.w,
                         context->target_rect.h,
                         hp_color);
    }

    render_profiler_end(RENDER_PROFILE_MAP_UI, profile_ui_started);
}

/** Projection invariants for one retained primary painter compilation. */
typedef struct map_retained_projection {
    int surface_width;
    int surface_height;
    int map_width;
    int map_height;
    int origin_x;
    int origin_y;
    uint16_t level_mask;
    uint8_t player_sub_layer;
    bool smooth_lighting;
    bool valid;
} map_retained_projection_t;

static void map_render_context_copy(map_render_context_t *destination,
                                    const map_render_context_t *source) {
    if (destination->commands_capacity < source->commands_num) {
        destination->commands_capacity = source->commands_num;
        destination->commands = xreallocarray(destination->commands,
                                              destination->commands_capacity,
                                              sizeof(*destination->commands));
    }
    if (destination->annotations_capacity < source->annotations_num) {
        destination->annotations_capacity = source->annotations_num;
        destination->annotations = xreallocarray(destination->annotations,
                                                 destination->annotations_capacity,
                                                 sizeof(*destination->annotations));
    }
    if (destination->tiles_capacity < source->tiles_num) {
        destination->tiles_capacity = source->tiles_num;
        destination->tiles = xreallocarray(destination->tiles,
                                           destination->tiles_capacity,
                                           sizeof(*destination->tiles));
    }
    if (destination->candidates_capacity < source->candidates_num) {
        destination->candidates_capacity = source->candidates_num;
        destination->candidates = xreallocarray(destination->candidates,
                                                destination->candidates_capacity,
                                                sizeof(*destination->candidates));
    }
    if (destination->cohorts_capacity < source->cohorts_num) {
        destination->cohorts_capacity = source->cohorts_num;
        destination->cohorts = xreallocarray(destination->cohorts,
                                             destination->cohorts_capacity,
                                             sizeof(*destination->cohorts));
    }
    if (source->commands_num != 0) {
        memcpy(destination->commands,
               source->commands,
               source->commands_num * sizeof(*destination->commands));
    }
    if (source->annotations_num != 0) {
        memcpy(destination->annotations,
               source->annotations,
               source->annotations_num * sizeof(*destination->annotations));
    }
    if (source->tiles_num != 0) {
        memcpy(destination->tiles, source->tiles, source->tiles_num * sizeof(*destination->tiles));
    }
    if (source->candidates_num != 0) {
        memcpy(destination->candidates,
               source->candidates,
               source->candidates_num * sizeof(*destination->candidates));
    }
    if (source->cohorts_num != 0) {
        memcpy(destination->cohorts,
               source->cohorts,
               source->cohorts_num * sizeof(*destination->cohorts));
    }
    destination->commands_num = source->commands_num;
    destination->annotations_num = source->annotations_num;
    destination->tiles_num = source->tiles_num;
    destination->candidates_num = source->candidates_num;
    destination->cohorts_num = source->cohorts_num;
    destination->next_sequence = source->next_sequence;
    destination->target_cell = source->target_cell;
    destination->target_rect = source->target_rect;
    destination->target_identity = source->target_identity;
    destination->target_generation = source->target_generation;
    destination->target_revision = source->target_revision;
    destination->target_tile_x = source->target_tile_x;
    destination->target_tile_y = source->target_tile_y;
    destination->target_depth = source->target_depth;
    destination->target_sub_layer = source->target_sub_layer;
    destination->commands_sorted = source->commands_sorted;
    destination->capture_candidates = source->capture_candidates;
}

/** Resolve a retained cell only while both its sparse slot and revision match. */
static bool map_retained_cell_matches(int depth,
                                      int x,
                                      int y,
                                      uint64_t identity,
                                      uint32_t generation,
                                      uint16_t revision,
                                      map_cell_t **cell_out) {
    if (!map_select_level(depth, false)) {
        return false;
    }
    size_t physical_index = map_cache_physical_index(x, y);
    map_cell_t *cell = MAP_CELL_GET(x, y);
    const map_cell_header_t *header = &cells->headers[physical_index];
    if (cell->painter_identity != identity || header->generation != generation ||
        header->revision != revision) {
        return false;
    }
    if (cell_out != NULL) {
        *cell_out = cell;
    }
    return true;
}

static void map_render_context_ranges_reserve(map_render_context_t *context,
                                              size_t commands,
                                              size_t annotations,
                                              size_t tiles,
                                              size_t candidates,
                                              size_t cohorts) {
    if (context->commands_capacity < commands) {
        context->commands_capacity = commands;
        context->commands = xreallocarray(context->commands, commands, sizeof(*context->commands));
    }
    if (context->annotations_capacity < annotations) {
        context->annotations_capacity = annotations;
        context->annotations =
            xreallocarray(context->annotations, annotations, sizeof(*context->annotations));
    }
    if (context->tiles_capacity < tiles) {
        context->tiles_capacity = tiles;
        context->tiles = xreallocarray(context->tiles, tiles, sizeof(*context->tiles));
    }
    if (context->candidates_capacity < candidates) {
        context->candidates_capacity = candidates;
        context->candidates =
            xreallocarray(context->candidates, candidates, sizeof(*context->candidates));
    }
    if (context->cohorts_capacity < cohorts) {
        context->cohorts_capacity = cohorts;
        context->cohorts = xreallocarray(context->cohorts, cohorts, sizeof(*context->cohorts));
    }
}

static void map_retained_candidate_replay(map_render_context_t *context,
                                          const map_render_candidate_t *candidate,
                                          map_cell_t *cell) {
    map_render_data_t data = {
        .x = candidate->x,
        .y = candidate->y,
        .midx = candidate->midx,
        .midy = candidate->midy,
        .xpos = candidate->xpos,
        .ypos = candidate->ypos,
        .player_height_offset = candidate->player_height_offset,
        .level_support_height = candidate->level_support_height,
        .cell = cell,
        .render_context = context,
        .layer = candidate->layer,
        .sub_layer = candidate->sub_layer,
        .alpha_forced = candidate->alpha_forced,
        .smooth_lighting = candidate->smooth_lighting,
        .lightmap_pending = candidate->lightmap_pending,
        .defer_rendering = true,
        .ground_pass = candidate->ground_pass,
        .world_surface = true,
        .primary_level = candidate->primary_level,
        .depth = candidate->depth,
    };
    size_t before = context->commands_num;
    draw_map_object(cur_widget[MAP_ID]->surface, &data);
    if (context->commands_num != before) {
        context->commands[before].sequence = candidate->sequence;
    }
}

/** Materialize complete changed-cell cohorts, including insertions and deletions. */
static void map_retained_animation_prepare(map_render_context_t *context,
                                           const map_render_context_t *retained) {
    context->commands_num = 0;
    context->annotations_num = 0;
    context->tiles_num = 0;
    context->candidates_num = 0;
    context->cohorts_num = 0;
    context->next_sequence = retained->next_sequence;
    context->target_cell = NULL;
    context->capture_candidates = false;
    map_render_context_ranges_reserve(context,
                                      retained->commands_num,
                                      retained->annotations_num,
                                      retained->tiles_num,
                                      retained->candidates_num,
                                      retained->cohorts_num);

    map_cell_t *target_cell;
    if (retained->target_cell != NULL && map_retained_cell_matches(retained->target_depth,
                                                                   retained->target_tile_x,
                                                                   retained->target_tile_y,
                                                                   retained->target_identity,
                                                                   retained->target_generation,
                                                                   retained->target_revision,
                                                                   &target_cell)) {
        context->target_cell = target_cell;
        context->target_rect = retained->target_rect;
        context->target_identity = retained->target_identity;
        context->target_generation = retained->target_generation;
        context->target_revision = retained->target_revision;
        context->target_tile_x = retained->target_tile_x;
        context->target_tile_y = retained->target_tile_y;
        context->target_depth = retained->target_depth;
        context->target_sub_layer = retained->target_sub_layer;
    }

    for (size_t index = 0; index < retained->cohorts_num; index++) {
        const map_render_cohort_t *old = &retained->cohorts[index];
        size_t candidates_first = context->candidates_num;
        size_t commands_first = context->commands_num;
        size_t annotations_first = context->annotations_num;
        size_t tiles_first = context->tiles_num;
        if (old->candidates_num != 0) {
            memcpy(&context->candidates[context->candidates_num],
                   &retained->candidates[old->candidates_first],
                   old->candidates_num * sizeof(*context->candidates));
        }
        context->candidates_num += old->candidates_num;

        map_cell_t *cell = NULL;
        bool stable = map_retained_cell_matches(old->depth,
                                                old->tile_x,
                                                old->tile_y,
                                                old->record_identity,
                                                old->cell_generation,
                                                old->cell_revision,
                                                &cell);
        if (stable) {
            if (old->commands_num != 0) {
                memcpy(&context->commands[context->commands_num],
                       &retained->commands[old->commands_first],
                       old->commands_num * sizeof(*context->commands));
            }
            context->commands_num += old->commands_num;
            for (size_t annotation = 0; annotation < old->annotations_num; annotation++) {
                map_annotation_t copy = retained->annotations[old->annotations_first + annotation];
                copy.cell = cell;
                context->annotations[context->annotations_num++] = copy;
            }
            if (old->tiles_num != 0) {
                memcpy(&context->tiles[context->tiles_num],
                       &retained->tiles[old->tiles_first],
                       old->tiles_num * sizeof(*context->tiles));
            }
            context->tiles_num += old->tiles_num;
            map_benchmark_statistics.reused_render_commands += old->commands_num;
        } else if (map_select_level(old->depth, false)) {
            cell = MAP_CELL_GET(old->tile_x, old->tile_y);
            for (size_t candidate = 0; candidate < old->candidates_num; candidate++) {
                map_retained_candidate_replay(
                    context,
                    &retained->candidates[old->candidates_first + candidate],
                    cell);
            }
            map_benchmark_statistics.compiled_render_commands += old->candidates_num;
        }

        const map_cell_header_t *header =
            &cells->headers[map_cache_physical_index(old->tile_x, old->tile_y)];
        context->cohorts[context->cohorts_num++] = (map_render_cohort_t){
            .candidates_first = candidates_first,
            .candidates_num = context->candidates_num - candidates_first,
            .commands_first = commands_first,
            .commands_num = context->commands_num - commands_first,
            .annotations_first = annotations_first,
            .annotations_num = context->annotations_num - annotations_first,
            .tiles_first = tiles_first,
            .tiles_num = context->tiles_num - tiles_first,
            .record_identity = cell != NULL ? cell->painter_identity : 0,
            .cell_generation = header->generation,
            .cell_revision = header->revision,
            .tile_x = old->tile_x,
            .tile_y = old->tile_y,
            .depth = old->depth,
        };
    }
    context->commands_sorted = false;
}

static void map_render_context_sort(map_render_context_t *context) {
    if (!context->commands_sorted && context->commands_num > 1) {
        qsort(context->commands,
              context->commands_num,
              sizeof(*context->commands),
              map_render_command_compare);
    }
    context->commands_sorted = true;
}

/** Force one auxiliary surface through the raw GPU map compositor. */
static bool map_gpu_auxiliary;
/** Persistent painter storage for the primary and auxiliary projections. */
static map_render_context_t map_render_contexts[2];
/** Immutable pre-paint primary compilation used by animation-only frames. */
static map_render_context_t map_retained_primary_context;
static map_retained_projection_t map_retained_primary_projection;

static bool map_retained_projection_matches(SDL_Surface *surface) {
    return map_retained_primary_projection.valid &&
           map_retained_primary_projection.surface_width == surface->w &&
           map_retained_primary_projection.surface_height == surface->h &&
           map_retained_primary_projection.map_width == map_width &&
           map_retained_primary_projection.map_height == map_height &&
           map_retained_primary_projection.origin_x == map_cache_origin_x &&
           map_retained_primary_projection.origin_y == map_cache_origin_y &&
           map_retained_primary_projection.level_mask == map_level_mask &&
           map_retained_primary_projection.player_sub_layer == MapData.player_sub_layer &&
           map_retained_primary_projection.smooth_lighting ==
               (setting_get_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING) != 0);
}

static void map_retained_projection_commit(SDL_Surface *surface) {
    map_retained_primary_projection = (map_retained_projection_t){
        .surface_width = surface->w,
        .surface_height = surface->h,
        .map_width = map_width,
        .map_height = map_height,
        .origin_x = map_cache_origin_x,
        .origin_y = map_cache_origin_y,
        .level_mask = map_level_mask,
        .player_sub_layer = MapData.player_sub_layer,
        .smooth_lighting = setting_get_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING) != 0,
        .valid = true,
    };
}

/** Draw independently cached levels through one projected painter order. */
void map_draw_map(SDL_Surface *surface) {
    HARD_ASSERT(surface != NULL);

    uint64_t profile_map_started = render_profiler_begin();

    bool primary_surface = cur_widget[MAP_ID] != NULL && surface == cur_widget[MAP_ID]->surface;
    bool gpu_output = gpu_renderer_ready() && (primary_surface || map_gpu_auxiliary);
    bool animation_only = primary_surface && !map_redraw_due() && map_animation_redraw_due() &&
                          map_redraw_pending_reasons() == MAP_REDRAW_REASON_ANIMATION;
    bool reuse_retained = animation_only && map_retained_projection_matches(surface);
    if (animation_only && !reuse_retained) {
        /* A missing/stale compilation is safe to recover with one full build;
         * never reinterpret it as an animation delta. */
        animation_only = false;
    }
    map_benchmark_statistics.map_draws++;
    if (primary_surface) {
        map_benchmark_statistics.primary_map_draws++;
    } else {
        map_benchmark_statistics.auxiliary_map_draws++;
    }
    map_render_context_t *render_context = &map_render_contexts[map_gpu_auxiliary ? 1 : 0];
    render_context->commands_num = 0;
    render_context->annotations_num = 0;
    render_context->tiles_num = 0;
    render_context->candidates_num = 0;
    render_context->cohorts_num = 0;
    render_context->next_sequence = 0;
    render_context->target_cell = NULL;
    render_context->commands_sorted = false;
    render_context->capture_candidates = primary_surface && !reuse_retained;

    if (!gpu_output) {
        map_benchmark_statistics.render_failures++;
        SDL_SetError("mandatory GPU map renderer is unavailable");
        LOG(ERROR, "%s", SDL_GetError());
        render_profiler_end(RENDER_PROFILE_MAP, profile_map_started);
        return;
    }
    bool began = map_gpu_auxiliary ? gpu_renderer_map_begin_auxiliary(surface->w, surface->h)
                                   : gpu_renderer_map_begin(surface->w, surface->h);
    if (!began) {
        map_benchmark_statistics.render_failures++;
        LOG(ERROR, "Could not begin retained GPU map target: %s", SDL_GetError());
        render_profiler_end(RENDER_PROFILE_MAP, profile_map_started);
        return;
    }

    uint64_t gpu_command_build_started = gpu_renderer_timing_begin();
    uint64_t active_levels = 0;
    if (reuse_retained) {
        /* GPU begin resets the frame-local row/span lookup. Re-emit only the
         * compact light field so unchanged records retain their owner keys and
         * are rejected by the GPU dirty-range uploader. */
        map_draw_retained_lighting(surface);
        map_retained_animation_prepare(render_context, &map_retained_primary_context);
        active_levels = map_active_level_count();
        map_benchmark_statistics.animation_level_draws += active_levels;
    } else {
        for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
            uint16_t bit = UINT16_C(1) << MAP2_DEPTH_INDEX(depth);
            if (!(map_level_mask & bit) || !map_select_level(depth, false) ||
                !lighting_select_level(depth)) {
                continue;
            }

            active_levels++;
            map_benchmark_statistics.level_draws++;
            map_draw_level(surface,
                           NULL,
                           depth,
                           depth == 0,
                           primary_surface,
                           false,
                           render_context);
        }
        map_benchmark_statistics.compiled_render_commands += render_context->commands_num;
    }

    map_benchmark_statistics.animation_draws += animation_only;

    if (primary_surface) {
        /* Cohort ranges address traversal-order output, so retain the
         * compilation before sorting the working painter list. */
        render_context->commands_sorted = false;
        map_render_context_copy(&map_retained_primary_context, render_context);
        map_retained_projection_commit(surface);
    }
    map_render_context_sort(render_context);

    map_benchmark_statistics.render_commands += render_context->commands_num;
    map_benchmark_commands_accumulate(render_context->commands,
                                      render_context->commands_num,
                                      primary_surface,
                                      animation_only);
    map_benchmark_statistics.annotations += render_context->annotations_num;
    map_benchmark_statistics.ui_tiles += render_context->tiles_num;
    map_benchmark_statistics.peak_render_commands =
        MAX(map_benchmark_statistics.peak_render_commands, render_context->commands_num);
    map_benchmark_statistics.peak_active_levels =
        MAX(map_benchmark_statistics.peak_active_levels, active_levels);

    map_render_commands(NULL,
                        render_context,
                        primary_surface,
                        primary_surface ? &map_animation_exit_cues : NULL);
    map_draw_ui(NULL, render_context);
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMMAND_BUILD, gpu_command_build_started);
    if (!gpu_renderer_map_end()) {
        map_benchmark_statistics.render_failures++;
        if (primary_surface) {
            map_retained_primary_projection.valid = false;
        }
        LOG(ERROR, "Could not finish retained GPU map target: %s", SDL_GetError());
    }
    map_select_level(0, true);
    lighting_select_level(0);
    render_profiler_end(RENDER_PROFILE_MAP, profile_map_started);
}

void map_draw_map_gpu_auxiliary(SDL_Surface *surface) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(!map_gpu_auxiliary);
    map_gpu_auxiliary = true;
    map_draw_map(surface);
    map_gpu_auxiliary = false;
}

/**
 * Check whether the map pointer cue may occupy a screen point.
 * @param x
 * Screen X coordinate.
 * @param y
 * Screen Y coordinate.
 * @return
 * 1 if the point is not covered by a popup or higher-priority widget, 0
 * otherwise.
 */
bool map_pointer_overlay_visible_at(int x, int y) {
    /* The retained-frame path draws this cue after widgets and popups.  The
     * popup event path deliberately leaves the previous widget owner in
     * place, so refresh the hit-test here and explicitly reject popup-covered
     * points before drawing over the UI. A NULL widget owner is valid while
     * the widget tree is being rebuilt after renderer recovery. */
    if (popup_covers_point(x, y)) {
        return false;
    }
    widgetdata *owner = get_widget_owner(x, y, NULL, NULL);
    return owner == NULL || owner == cur_widget[MAP_ID];
}

/** Draw the tile cue after the completed screen frame has been retained. */
void map_draw_pointer_overlay(void) {
    if (!map_show_mouse || widget_mouse_event.owner != cur_widget[MAP_ID] ||
        !map_pointer_overlay_visible_at(cursor_x, cursor_y)) {
        return;
    }

    int tx, ty;
    if (!mouse_to_tile_coords(cursor_x, cursor_y, &tx, &ty)) {
        map_show_mouse = false;
        return;
    }

    map_draw_one(tx, ty, TEXTURE_CLIENT("square_highlight"));
}

/** Return the logical map surface used to derive GPU presentation geometry. */
static SDL_Surface *map_displayed_surface(widgetdata *widget) {
    return widget->surface;
}

/**
 * Draw one sprite on map.
 * @param x
 * X position.
 * @param y
 * Y position.
 * @param surface
 * What to draw.
 */
void map_draw_one(int x, int y, SDL_Surface *surface) {
    map_render_data_t data = {.world_surface = true};

    map_setup_render_data(cur_widget[MAP_ID]->surface, &data, NULL, NULL, NULL, NULL);

    data.x = x;
    data.y = y;
    SOFT_ASSERT(map_should_draw(cur_widget[MAP_ID]->surface, &data),
                "map_should_draw() returned false");

    if (surface->w > MAP_TILE_POS_XOFF) {
        data.xpos -= (surface->w - MAP_TILE_POS_XOFF) / 2;
    }

    if (map_cell_layer_record_read(data.cell, 0)->face != 0) {
        data.ypos -= get_top_floor_height(data.cell, MapData.player_sub_layer);
        data.ypos += data.player_height_offset;
    }

    SDL_Surface *displayed = map_displayed_surface(cur_widget[MAP_ID]);
    map_screen_point_t screen;
    SOFT_ASSERT(map_local_anchor_to_screen(widget_x(cur_widget[MAP_ID]),
                                           widget_y(cur_widget[MAP_ID]),
                                           cur_widget[MAP_ID]->surface->w,
                                           cur_widget[MAP_ID]->surface->h,
                                           displayed->w,
                                           displayed->h,
                                           data.xpos,
                                           data.ypos,
                                           &screen),
                "Map highlight coordinate overflow");

    sprite_effects_t effects = {0};
    effects.zoom_x = 100.0 * displayed->w / cur_widget[MAP_ID]->surface->w;
    effects.zoom_y = 100.0 * displayed->h / cur_widget[MAP_ID]->surface->h;

    /* Outside of the "visible" area; always render as fog of war
     * (grayscale). */
    if (x < map_width * (MAP_FOW_SIZE / 2) || x >= map_width * (MAP_FOW_SIZE / 2) + map_width ||
        y < map_height * (MAP_FOW_SIZE / 2) || y >= map_height * (MAP_FOW_SIZE / 2) + map_height) {
        BIT_SET(effects.flags, SPRITE_FLAG_FOW);
    }

    surface_show_effects(OfflineRenderSurface, screen.x, screen.y, NULL, surface, &effects);
}

/**
 * Send a command to move the player to the specified square.
 *
 * @param tx
 * Square X position.
 * @param ty
 * Square Y position.
 */
static void send_move_path(int tx, int ty) {
    packet_struct *packet;

    if (tx < 0 || ty < 0 || tx >= map_width || ty >= map_height) {
        return;
    }

#ifdef ATRINIK_WIDGET_TESTS
    if (map_interaction_test_active) {
        map_interaction_test_moves++;
    }
#endif

    packet = packet_new(SERVER_CMD_MOVE_PATH, 8, 0);
    packet_writer_write_uint8(packet, tx);
    packet_writer_write_uint8(packet, ty);
    socket_send_packet(packet);
}

/**
 * Send a command to target an NPC.
 * @param tx
 * NPC's X position.
 * @param ty
 * NPC's Y position.
 * @param count
 * NPC's UID.
 */
static void send_target(int x, int y, uint32_t count) {
    packet_struct *packet;

    if ((x < 0 || y < 0 || x >= map_width || y >= map_height) && !(x == -1 && y == -1)) {
        return;
    }

#ifdef ATRINIK_WIDGET_TESTS
    if (map_interaction_test_active) {
        map_interaction_test_targets++;
    }
#endif

    packet = packet_new(SERVER_CMD_TARGET, 16, 0);

    if (x == -1 && y == -1) {
        packet_writer_write_uint8(packet, CMD_TARGET_CLEAR);
    } else {
        packet_writer_write_uint8(packet, CMD_TARGET_MAPXY);
        packet_writer_write_uint8(packet, x);
        packet_writer_write_uint8(packet, y);
        packet_writer_write_uint32(packet, count);
    }

    socket_send_packet(packet);
}

/**
 * Compare distances between two targets on the map.
 * @param a
 * First target.
 * @param b
 * Second target.
 * @return
 * Comparison result.
 */
static int map_target_cmp(const void *a, const void *b) {
    double x, y, x2, y2;
    unsigned long dist1, dist2;

    x = ((const map_target_struct *)a)->x - (map_width / 2.0f);
    y = ((const map_target_struct *)a)->y - (map_height / 2.0f);

    x2 = ((const map_target_struct *)b)->x - (map_width / 2.0f);
    y2 = ((const map_target_struct *)b)->y - (map_height / 2.0f);

    dist1 = isqrt(x * x + y * y);
    dist2 = isqrt(x2 * x2 + y2 * y2);

    if (dist1 < dist2) {
        return -1;
    } else if (dist1 > dist2) {
        return 1;
    } else {
        return 0;
    }
}

/**
 * Target something on the map.
 * @param is_friend
 * 1 if targeting friendlies only.
 */
void map_target_handle(uint8_t is_friend) {
    int x, y, layer;
    map_cell_t *cell;
    UT_array *targets;
    UT_icd icd = {sizeof(map_target_struct), NULL, NULL, NULL};
    map_target_struct *p;
    uint32_t curr_target;

    if (cpl.target_is_friend != is_friend) {
        cpl.target_object_index = 0;
    }

    utarray_new(targets, &icd);
    curr_target = 0;

    for (x = 0; x < map_width; x++) {
        for (y = 0; y < map_height; y++) {
            cell = MAP_CELL_GET_MIDDLE(x, y);

            if (cell->fow) {
                continue;
            }

            for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                layer = GET_MAP_LAYER(LAYER_LIVING, sub_layer);
                if (map_cell_layer_record_read(cell, layer)->face &&
                    map_cell_actor_record_read(cell, sub_layer)->target_object_count &&
                    map_cell_actor_record_read(cell, sub_layer)->target_is_friend == is_friend) {
                    map_target_struct target;

                    target.count = map_cell_actor_record_read(cell, sub_layer)->target_object_count;
                    target.x = x;
                    target.y = y;
                    utarray_push_back(targets, &target);

                    if (map_cell_actor_record_read(cell, sub_layer)->probe != 0) {
                        curr_target = target.count;
                    }
                }
            }
        }
    }

    utarray_sort(targets, map_target_cmp);

    if (cpl.target_object_index >= utarray_len(targets)) {
        cpl.target_object_index = 0;
    }

    if (cpl.target_object_index == 0) {
        p = (map_target_struct *)utarray_front(targets);

        if (p != NULL && p->count == curr_target) {
            cpl.target_object_index++;
        }
    }

    p = (map_target_struct *)utarray_eltptr(targets, cpl.target_object_index);

    if (p != NULL) {
        send_target(p->x, p->y, p->count);
        cpl.target_object_index++;
    } else if (cpl.target_is_friend != is_friend) {
        send_target(-1, -1, 0);
    }

    cpl.target_is_friend = is_friend;

    utarray_free(targets);
}

/** Convert screen mouse coordinates to map-local coordinates. */
static void map_mouse_to_local_coords(int *mx, int *my) {
    *mx -= widget_x(cur_widget[MAP_ID]);
    *my -= widget_y(cur_widget[MAP_ID]);
}

/**
 * Transform mouse coordinates to tile coordinates on map.
 *
 * Both 'tx' and 'ty' can be NULL, which is useful if you only want to
 * check if the mouse is over a valid map tile.
 *
 * @param mx
 * Mouse X.
 * @param my
 * Mouse Y.
 * @param[out] tx Will contain tile X, unless function returns false.
 * @param[out] ty Will contain tile Y, unless function returns false.
 * @return
 * True on success, false on failure.
 */
bool mouse_to_tile_coords(int mx, int my, int *tx, int *ty) {
#ifdef ATRINIK_WIDGET_TESTS
    if (map_interaction_test_active) {
        if (tx != NULL) {
            *tx = map_width * (MAP_FOW_SIZE / 2) + 2;
        }
        if (ty != NULL) {
            *ty = map_height * (MAP_FOW_SIZE / 2) + 3;
        }
        return true;
    }
#endif

    map_render_data_t data = {.world_surface = true};
    int x, y, w, h;
    map_setup_render_data(cur_widget[MAP_ID]->surface, &data, &x, &y, &w, &h);

    double zoom = setting_get_int(OPT_CAT_MAP, OPT_MAP_ZOOM) / 100.0;

    map_mouse_to_local_coords(&mx, &my);

    for (data.x = w - 1; data.x >= x; data.x--) {
        for (data.y = h - 1; data.y >= y; data.y--) {
            if (!map_should_draw(cur_widget[MAP_ID]->surface, &data)) {
                continue;
            }

            if (MapData.height_diff &&
                abs(get_top_floor_height(data.cell, MapData.player_sub_layer) -
                    data.player_height_offset) > HEIGHT_MAX_RENDER) {
                continue;
            }

            data.xpos *= zoom;
            data.ypos *= zoom;

            if (map_cell_layer_record_read(data.cell, 0)->face != 0) {
                int height = get_top_floor_height(data.cell, MapData.player_sub_layer);
                data.ypos = (data.ypos - height * zoom) + data.player_height_offset * zoom;
            }

            uint32_t stretch = 0;
            int16_t max_height = 0;

            for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                int16_t height =
                    map_cell_layer_record_read(data.cell, sub_layer * NUM_LAYERS)->height;
                if (height > max_height) {
                    max_height = height;
                    stretch = map_cell_sublayer_record_read(data.cell, sub_layer)->stretch;
                }
            }

            int stretch_height = (stretch >> 24) & 0xff;

            /* See if this square matches our 48x24 box shape. */
            if (mx >= data.xpos && mx <= data.xpos + (MAP_TILE_POS_XOFF * zoom) &&
                my >= data.ypos && my <= data.ypos + (MAP_TILE_YOFF + stretch_height) * zoom) {
                if (tilestretcher_coords_in_tile(stretch,
                                                 (mx - data.xpos) / zoom,
                                                 (my - data.ypos) / zoom)) {
                    if (tx != NULL) {
                        *tx = data.x;
                    }

                    if (ty != NULL) {
                        *ty = data.y;
                    }

                    return true;
                }
            }
        }
    }

    return false;
}

/**
 * Handle the mouse firing gesture.
 *
 * @return
 * True if the gesture was handled, false otherwise.
 */
bool map_mouse_fire(void) {
    int x, y;
    SDL_MouseButtonFlags state = mouse_get_state(&x, &y);

    if ((state != (SDL_BUTTON_MASK(SDL_BUTTON_RIGHT) | SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) &&
         state != SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE))) {
        return false;
    }

    int tx, ty;
    if (!mouse_to_tile_coords(x, y, &tx, &ty)) {
        return false;
    }

    int rx = tx - map_width * (MAP_FOW_SIZE / 2);
    int ry = ty - map_height * (MAP_FOW_SIZE / 2);

    cpl.fire_on = 1;
    move_keys(dir_from_tile_coords(rx, ry));
    cpl.fire_on = 0;
    return true;
}

/**
 * Handle the "Walk Here" option in map widget menu.
 * @param widget
 * Map widget.
 * @param menuitem
 * Menu item.
 * @param event
 * Event.
 */
static void menu_map_walk_here(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    int tx, ty;

    if (mouse_to_tile_coords(cur_widget[MENU_ID]->x, cur_widget[MENU_ID]->y, &tx, &ty)) {
        int rx = tx - map_width * (MAP_FOW_SIZE / 2);
        int ry = ty - map_height * (MAP_FOW_SIZE / 2);
        send_move_path(rx, ry);
    }
}

/**
 * Handle the "Talk To NPC" option in map widget menu.
 * @param widget
 * Map widget.
 * @param menuitem
 * Menu item.
 * @param event
 * Event.
 */
static void menu_map_talk_to(widgetdata *widget, widgetdata *menuitem, SDL_Event *event) {
    int tx, ty;

    if (mouse_to_tile_coords(cur_widget[MENU_ID]->x, cur_widget[MENU_ID]->y, &tx, &ty)) {
        int rx = tx - map_width * (MAP_FOW_SIZE / 2);
        int ry = ty - map_height * (MAP_FOW_SIZE / 2);
        send_target(rx, ry, 0);
#ifdef ATRINIK_WIDGET_TESTS
        if (map_interaction_test_active) {
            map_interaction_test_talks++;
        }
#endif
        keybind_process_command("?HELLO");
    }
}

typedef struct map_menu_action {
    const char *name;
    void (*handler)(widgetdata *, widgetdata *, SDL_Event *);
} map_menu_action;

static const map_menu_action map_menu_actions[] = {
    {"Walk Here", menu_map_walk_here},
    {"Talk To NPC", menu_map_talk_to},
};

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    static int gfx_toggle = 0;
    int mx, my;

    if (widget->surface == NULL || widget->surface->w != widget->w ||
        widget->surface->h != widget->h) {
        if (widget->surface != NULL) {
            SDL_DestroySurface(widget->surface);
            map_redraw_request(MAP_REDRAW_REASON_RESIZE);
        }

        widget->surface = surface_create_rgb(get_video_flags(),
                                             widget->w,
                                             widget->h,
                                             video_get_bpp(),
                                             0,
                                             0,
                                             0,
                                             0);
        if (widget->surface == NULL) {
            LOG(ERROR, "Could not create map widget surface: %s", SDL_GetError());
            return;
        }
    }

    /* Make sure the map widget is always the last to handle events for. */
    widget_enforce_map_priority();

    double zoom = setting_get_int(OPT_CAT_MAP, OPT_MAP_ZOOM) / 100.0;
    if (widget_set_zoom(widget, zoom)) {
        map_redraw_request(MAP_REDRAW_REASON_RESIZE);
    }

    /* A complete GPU redraw remains the sole map compositor. Animation-only
     * invalidation is retained as evidence about why this redraw occurred. */
    if (map_redraw_due()) {
        map_draw_map(widget->surface);
        map_redraw_consume();
        effect_sprites_play();
    } else if (map_animation_redraw_due()) {
        map_draw_map(widget->surface);
        map_animation_redraw_consume();
        effect_sprites_play();
    }

    SDL_Surface *displayed = map_displayed_surface(widget);
    if (!gpu_renderer_draw_map((float)widget_x(widget),
                               (float)widget_y(widget),
                               (float)widget_w(widget),
                               (float)widget_h(widget))) {
        LOG(ERROR, "Could not submit retained GPU map target: %s", SDL_GetError());
    }

    /* The damage numbers */
    map_anims_play();

    map_render_data_t data = {0};
    map_setup_render_data(widget->surface, &data, NULL, NULL, NULL, NULL);

    /* Health and food warnings are widget-centered alerts. Their anchor,
     * texture size, and offset intentionally remain in screen pixels. */
    int xpos = widget_x(widget) + displayed->w / 2;
    int ypos = widget_y(widget) + displayed->h / 2;
    ypos -= MAP_TILE_POS_YOFF * 1.5 + 7;

    /* Draw warning icons above player */
    if ((gfx_toggle++ & 63) < 25) {
        int warn = setting_get_int(OPT_CAT_MAP, OPT_HEALTH_WARNING);
        double hp_percent = (double)cpl.stats.hp / cpl.stats.maxhp * 100.0;
        if (warn != 0 && warn >= hp_percent) {
            SDL_Surface *texture = TEXTURE_CLIENT("warn_hp");
            surface_show(OfflineRenderSurface,
                         xpos - texture->w / 2,
                         ypos - texture->h / 2,
                         NULL,
                         texture);
        }
    } else {
        int warn = setting_get_int(OPT_CAT_MAP, OPT_FOOD_WARNING);
        double food_percent = (double)cpl.stats.food / 1000.0 * 100.0;
        if (warn != 0 && warn >= food_percent) {
            SDL_Surface *texture = TEXTURE_CLIENT("warn_food");
            surface_show(OfflineRenderSurface,
                         xpos - texture->w / 2,
                         ypos - texture->h / 2,
                         NULL,
                         texture);
        }
    }

    /* MAPSTATS messages are screen-space UI. They are horizontally centered
     * on the exact displayed map surface, while their historical vertical
     * anchor follows its effective top edge at a fixed 300-pixel offset.
     * Font size and trajectory also remain fixed in screen pixels. */
    if (msg_anim.message[0] != '\0') {
        if ((LastTick - msg_anim.tick) < 3000) {
            int bmoff, y_offset;
            char *msg, *cp;

            bmoff = (int)((50.0f / 3.0f) * ((float)(LastTick - msg_anim.tick) / 1000.0f) *
                              ((float)(LastTick - msg_anim.tick) / 1000.0f) +
                          ((int)(150.0f * ((float)(LastTick - msg_anim.tick) / 3000.0f))));
            y_offset = 0;
            msg = xstrdup(msg_anim.message);

            cp = strtok(msg, "\n");

            while (cp) {
                text_show_root(FONT_SERIF16,
                               cp,
                               widget_x(widget) + displayed->w / 2 -
                                   text_get_width(FONT_SERIF16, cp, TEXT_OUTLINE) / 2,
                               widget_y(widget) + 300 - bmoff + y_offset,
                               msg_anim.color,
                               TEXT_OUTLINE | TEXT_MARKUP,
                               NULL);
                y_offset += FONT_HEIGHT(FONT_SERIF16);
                cp = strtok(NULL, "\n");
            }

            free(msg);
            widget->redraw++;
        } else {
            msg_anim.message[0] = '\0';
        }
    }

    /* Holding the right mouse button for some time, create a menu. */
    if (mouse_get_state(&mx, &my) == SDL_BUTTON_MASK(SDL_BUTTON_RIGHT) && right_click_ticks != -1 &&
        client_ui_ticks() - right_click_ticks > 500) {
        widgetdata *menu;

        menu = create_menu(mx, my, widget);
        for (size_t i = 0; i < arraysize(map_menu_actions); i++) {
            add_menuitem(menu,
                         map_menu_actions[i].name,
                         map_menu_actions[i].handler,
                         MENU_NORMAL,
                         0);
        }
        widget_menu_standard_items(widget, menu);
        menu_finalize(menu);
        right_click_ticks = -1;
    }
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    if (!EVENT_IS_MOUSE(event)) {
        return 0;
    }

    /* Check if the mouse is in play field. */
    int tx, ty;
    if (!mouse_to_tile_coords(event_mouse_x(event), event_mouse_y(event), &tx, &ty)) {
        return 0;
    }

    int rx = tx - map_width * (MAP_FOW_SIZE / 2);
    int ry = ty - map_height * (MAP_FOW_SIZE / 2);

    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        /* Send target command if we released the right button in time;
         * otherwise the widget menu will be created. */
        if (event->button.button == SDL_BUTTON_RIGHT &&
            client_ui_ticks() - right_click_ticks < 500) {
            send_target(rx, ry, 0);
        }

        right_click_ticks = -1;
        return 1;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.button == SDL_BUTTON_RIGHT) {
            right_click_ticks = client_ui_ticks();
        } else if (mouse_get_state(NULL, NULL) == SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) {
            /* Running */

            if (cpl.fire_on || cpl.run_on) {
                move_keys(dir_from_tile_coords(rx, ry));
            } else {
                send_move_path(rx, ry);
            }
        }

        return 1;
    } else if (event->type == SDL_EVENT_MOUSE_MOTION) {
        if (tx != old_map_mouse_x || ty != old_map_mouse_y) {
            old_map_mouse_x = tx;
            old_map_mouse_y = ty;
            map_show_mouse = true;

            return 1;
        }
    }

    return 0;
}

#ifdef ATRINIK_WIDGET_TESTS

void widget_map_draw_test(widgetdata *widget) {
    widget_draw(widget);
}

void widget_map_pointer_test_set(int x, int y, bool world_pointer) {
    cursor_x = x;
    cursor_y = y;
    widget_mouse_event.owner = world_pointer ? cur_widget[MAP_ID] : NULL;
    map_show_mouse = world_pointer;
}

void widget_map_ui_test_begin(void) {
    map_ui_test_names = 0;
    map_ui_test_targets = 0;
    map_ui_test_active = true;
}

bool widget_map_ui_test_end(void) {
    map_ui_test_active = false;
    if (map_ui_test_names == 0 || map_ui_test_targets == 0) {
        SDL_SetError("map UI fixture draws missing: names=%d targets=%d",
                     map_ui_test_names,
                     map_ui_test_targets);
        return false;
    }
    return true;
}

void widget_map_animation_test_begin(void) {
    map_animation_test_damage_draws = 0;
    map_animation_test_kill_draws = 0;
    map_animation_test_elevated_draws = 0;
    map_animation_test_source_floor_height = 0;
    map_animation_test_player_floor_height = 0;
    map_animation_test_layer_content_draws = 0;
    map_animation_test_active = true;
}

bool widget_map_animation_test_end(bool expect_damage,
                                   bool expect_kill,
                                   bool expect_elevated,
                                   bool expect_layer_content) {
    map_animation_test_active = false;
    bool success = (!expect_damage || map_animation_test_damage_draws > 0) &&
                   (!expect_kill || map_animation_test_kill_draws > 0) &&
                   (!expect_elevated || map_animation_test_elevated_draws > 0) &&
                   (!expect_layer_content || map_animation_test_layer_content_draws > 0);
    if (!success) {
        fprintf(stderr,
                "map animation test: damage=%d kill=%d elevated=%d source-floor=%d "
                "player-floor=%d layer-content=%d\n",
                map_animation_test_damage_draws,
                map_animation_test_kill_draws,
                map_animation_test_elevated_draws,
                map_animation_test_source_floor_height,
                map_animation_test_player_floor_height,
                map_animation_test_layer_content_draws);
    }
    return success;
}

void widget_map_animation_test_death_texture_set(SDL_Surface *texture) {
    map_animation_test_death_texture = texture;
}

bool widget_map_visibility_test(void) {
    const uint32_t initial_tick = LastTick;
    const int player_x = map_width * MAP_FOW_SIZE / 2;
    const int player_y = map_height * MAP_FOW_SIZE / 2;
    const int center_view_x = map_width - map_width / 2 - 1;
    const int center_view_y = map_height - map_height / 2 - 1;
    bool success = true;
    bool center_item = false;
    bool center_living = false;
    bool center_effect = false;

    map_animate();

    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        if (!(map_level_mask & (UINT16_C(1) << MAP2_DEPTH_INDEX(depth))) ||
            !map_select_level(depth, false)) {
            continue;
        }

        for (int x = 0; x < map_width; x++) {
            for (int y = 0; y < map_height; y++) {
                map_cell_t *cell = MAP_CELL_GET_MIDDLE(x, y);
                int cache_x = x + MAP_STARTX;
                int cache_y = y + MAP_STARTY;

                for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
                    for (int object_layer = LAYER_ITEM; object_layer <= LAYER_EFFECT;
                         object_layer++) {
                        if (!map_visibility_transient_layer(object_layer)) {
                            continue;
                        }
                        int layer = GET_MAP_LAYER(object_layer, sub_layer);
                        const map_visibility_fade_t *fade =
                            &map_cell_layer_record_read(cell, layer)->visibility;
                        if (!fade->initialized || !fade->authorized ||
                            map_cell_layer_record_read(cell, layer)->face == 0) {
                            continue;
                        }

                        uint8_t expected = map_visibility_field_alpha(
                            map_visibility_field_weight(cache_x - player_x, cache_y - player_y));
                        bool local_player =
                            depth == 0 && cache_x == player_x && cache_y == player_y &&
                            object_layer == LAYER_LIVING &&
                            sub_layer == MIN(MapData.player_sub_layer, NUM_SUB_LAYERS - 1);
                        if (local_player) {
                            expected = UINT8_MAX;
                        }
                        if (fade->target_alpha != expected) {
                            fprintf(stderr,
                                    "map visibility test: target mismatch depth=%d x=%d y=%d "
                                    "layer=%d expected=%u got=%u\n",
                                    depth,
                                    x,
                                    y,
                                    object_layer,
                                    expected,
                                    fade->target_alpha);
                            success = false;
                        }

                        if (depth == 0 && x == center_view_x && y == center_view_y) {
                            center_item |= object_layer == LAYER_ITEM;
                            center_living |= object_layer == LAYER_LIVING;
                            center_effect |= object_layer == LAYER_EFFECT;
                        }
                    }
                }
            }
        }
    }

    if (!center_item || !center_living || !center_effect) {
        fprintf(stderr,
                "map visibility test: center records item=%d living=%d effect=%d\n",
                center_item,
                center_living,
                center_effect);
        success = false;
    }

    LastTick = initial_tick + MAP_VISIBILITY_FADE_DURATION_MS;
    map_animate();
    if (!map_select_level(0, true)) {
        return false;
    }

    map_cell_t *center = MAP_CELL_GET_MIDDLE(center_view_x, center_view_y);
    for (int object_layer = LAYER_ITEM; object_layer <= LAYER_EFFECT; object_layer++) {
        if (!map_visibility_transient_layer(object_layer)) {
            continue;
        }
        int layer = GET_MAP_LAYER(object_layer, MIN(MapData.player_sub_layer, NUM_SUB_LAYERS - 1));
        const map_visibility_fade_t *fade = &map_cell_layer_record_read(center, layer)->visibility;
        if (map_cell_layer_record_read(center, layer)->face == 0) {
            continue;
        }
        if (!fade->initialized || !fade->authorized || fade->alpha != UINT8_MAX) {
            fprintf(stderr,
                    "map visibility test: center layer %d did not remain opaque "
                    "after static interval (initialized=%d authorized=%d alpha=%u)\n",
                    object_layer,
                    fade->initialized,
                    fade->authorized,
                    fade->alpha);
            success = false;
        }
    }

    /* Keep the simulated clock monotonic after advancing the fade interval. */
    return success;
}

bool widget_map_retained_visibility_test_set(bool visible) {
    if (!map_select_level(0, false)) {
        return false;
    }

    int x = map_width * MAP_FOW_SIZE / 2;
    int y = map_height * MAP_FOW_SIZE / 2;
    map_cell_t *cell = MAP_CELL_GET(x, y);
    uint8_t expected_layers = 0;
    bool changed = false;
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = LAYER_ITEM; object_layer <= LAYER_EFFECT; object_layer++) {
            if (!map_visibility_transient_layer(object_layer)) {
                continue;
            }
            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            map_cell_layer_record_t *record = map_cell_layer_record(cell, layer, false);
            if (record == NULL || record->face == 0 || !record->visibility.initialized ||
                !record->visibility.authorized) {
                continue;
            }
            expected_layers |= UINT8_C(1) << (object_layer - LAYER_ITEM);
            uint8_t alpha = visible ? UINT8_MAX : 0;
            changed |= record->visibility.alpha != alpha;
            record->visibility.alpha = alpha;
        }
    }
    uint8_t required_layers = (UINT8_C(1) << (LAYER_ITEM - LAYER_ITEM)) |
                              (UINT8_C(1) << (LAYER_LIVING - LAYER_ITEM)) |
                              (UINT8_C(1) << (LAYER_EFFECT - LAYER_ITEM));
    if ((expected_layers & required_layers) != required_layers || !changed) {
        return false;
    }

    map_cell_presentation_changed(map_cache_physical_index(x, y));
    map_animation_redraw_request(LAYER_LIVING);
    return true;
}

void widget_map_animation_test_add(int type,
                                   int x_offset,
                                   int y_offset,
                                   int sub_layer,
                                   int depth,
                                   int value,
                                   uint32_t elapsed_ms) {
    HARD_ASSERT(type == ANIM_DAMAGE || type == ANIM_KILL);
    HARD_ASSERT(elapsed_ms <= 850 && elapsed_ms <= LastTick);
    HARD_ASSERT(sub_layer >= 0 && sub_layer < NUM_SUB_LAYERS);
    HARD_ASSERT(depth >= -MAP2_MAX_DEPTH && depth <= MAP2_MAX_DEPTH);
    map_animation_test_expected_depth = depth;
    map_animation_test_expected_sub_layer = sub_layer;

    map_anim_t *anim = map_anims_add(type,
                                     map_width / 2 + x_offset,
                                     map_height / 2 + y_offset,
                                     sub_layer,
                                     depth,
                                     value);
    anim->start_tick -= elapsed_ms;
    anim->last_tick -= elapsed_ms;
}

/** Find a screen point that the production hit-test resolves to one tile. */
static bool widget_map_click_test_find_point(widgetdata *widget, int target_x, int target_y) {
    if (widget == NULL || widget->surface == NULL || cur_widget[MAP_ID] == NULL) {
        return false;
    }

    map_render_data_t data = {.world_surface = true};
    int x, y, w, h;
    map_setup_render_data(widget->surface, &data, &x, &y, &w, &h);
    if (target_x < x || target_x >= w || target_y < y || target_y >= h) {
        return false;
    }

    data.x = target_x;
    data.y = target_y;
    if (!map_should_draw(widget->surface, &data)) {
        return false;
    }

    double zoom = setting_get_int(OPT_CAT_MAP, OPT_MAP_ZOOM) / 100.0;
    data.xpos *= zoom;
    data.ypos *= zoom;

    if (map_cell_layer_record_read(data.cell, 0)->face != 0) {
        int height = get_top_floor_height(data.cell, MapData.player_sub_layer);
        data.ypos = (data.ypos - height * zoom) + data.player_height_offset * zoom;
    }

    uint32_t stretch = 0;
    int16_t max_height = 0;
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        int16_t height = map_cell_layer_record_read(data.cell, sub_layer * NUM_LAYERS)->height;
        if (height > max_height) {
            max_height = height;
            stretch = map_cell_sublayer_record_read(data.cell, sub_layer)->stretch;
        }
    }

    int stretch_height = (stretch >> 24) & 0xff;
    int displayed_width = MAX(1, (int)(widget->surface->w * zoom));
    int displayed_height = MAX(1, (int)(widget->surface->h * zoom));
    int left = MAX(0, data.xpos);
    int top = MAX(0, data.ypos);
    int right = MIN(displayed_width - 1, data.xpos + (int)(MAP_TILE_POS_XOFF * zoom));
    int bottom =
        MIN(displayed_height - 1, data.ypos + (int)((MAP_TILE_YOFF + stretch_height) * zoom));

    for (int local_x = left; local_x <= right; local_x++) {
        for (int local_y = top; local_y <= bottom; local_y++) {
            int found_x = -1;
            int found_y = -1;
            if (mouse_to_tile_coords(widget_x(widget) + local_x,
                                     widget_y(widget) + local_y,
                                     &found_x,
                                     &found_y) &&
                found_x == target_x && found_y == target_y) {
                return true;
            }
        }
    }

    return false;
}

typedef struct widget_map_click_test_case {
    const char *name;
    bool fow;
    bool structural_fow;
    bool face;
    bool black;
    int height;
    uint32_t stretch;
    bool height_diff;
    bool expected;
} widget_map_click_test_case_t;

/** Exercise click-to-move hit-testing against cached map presentation states. */
bool widget_map_fog_click_test(widgetdata *widget) {
    if (widget == NULL || widget->surface == NULL || map_width < 4 || map_height < 4) {
        return false;
    }

    size_t saved_level_index = current_level_index;
    map_cell_store_t *saved_cells_pointer = cells;
    map_cell_store_t *saved_base_cells = level_cells[MAP2_DEPTH_INDEX(0)];
    if (!map_select_level(0, false)) {
        return false;
    }

    const int target_x = map_width * (MAP_FOW_SIZE / 2) + 7;
    const int target_y = map_height * (MAP_FOW_SIZE / 2) + 7;
    size_t cell_count = (size_t)map_width * MAP_FOW_SIZE * (size_t)map_height * MAP_FOW_SIZE;
    map_cell_store_t *test_cells = map_cell_store_create(cell_count);
    level_cells[MAP2_DEPTH_INDEX(0)] = test_cells;
    cells = test_cells;
    map_cell_t *cell = MAP_CELL_GET_MUTABLE(target_x, target_y);
    bool saved_height_diff = MapData.height_diff;
    int64_t saved_zoom = setting_get_int(OPT_CAT_MAP, OPT_MAP_ZOOM);
    double saved_widget_zoom = widget->zoom;
    int saved_widget_zoom_x = widget->zoom_x;
    int saved_widget_zoom_y = widget->zoom_y;
    widgetdata *saved_map_widget = cur_widget[MAP_ID];
    cur_widget[MAP_ID] = widget;

    static const int zooms[] = {75, 100, 125};
    static const widget_map_click_test_case_t cases[] = {
        {"remembered-fog", true, true, true, false, 75, 0, false, true},
        {"blank-fog", true, false, false, false, 0, 0, false, true},
        {"black", false, false, true, true, 0, 0, false, true},
        {"stretched-fog", true, false, false, false, 1, UINT32_C(0x02030401), false, true},
        {"height-difference", true, true, true, false, 75, 0, true, false},
    };

    bool success = true;
    for (size_t case_index = 0; case_index < arraysize(cases) && success; case_index++) {
        const widget_map_click_test_case_t *test_case = &cases[case_index];
        for (size_t zoom_index = 0; zoom_index < arraysize(zooms); zoom_index++) {
            setting_set_int(OPT_CAT_MAP, OPT_MAP_ZOOM, zooms[zoom_index]);
            widget_set_zoom(widget, zooms[zoom_index] / 100.0);

            map_cell_release_contents(cell);
            memset(cell, 0, sizeof(*cell));
            cell->fow = test_case->fow;
            cell->structural_fow = test_case->structural_fow;
            map_cell_layer_record_t *floor =
                map_cell_layer_record(cell, GET_MAP_LAYER(LAYER_FLOOR, 0), true);
            floor->face = test_case->face ? 1 : 0;
            floor->height = test_case->height;
            cell->render_max_height = test_case->height;
            map_cell_sublayer_record(cell, 0, true)->stretch = test_case->stretch;
            if (test_case->black) {
                map_cell_light_record_t *light = map_cell_light_record(cell, 0, true);
                light->known = 1;
                light->radiance = 0;
            }
            MapData.height_diff = test_case->height_diff;

            bool hit = widget_map_click_test_find_point(widget, target_x, target_y);
            if (hit != test_case->expected) {
                fprintf(stderr,
                        "map click test: case=%s zoom=%d expected=%d got=%d\n",
                        test_case->name,
                        zooms[zoom_index],
                        test_case->expected,
                        hit);
                success = false;
            }
        }
    }

    if (success) {
        int outside = widget->surface->w + MAP_TILE_POS_XOFF;
        if (mouse_to_tile_coords(widget_x(widget) - outside,
                                 widget_y(widget) - outside,
                                 NULL,
                                 NULL) ||
            mouse_to_tile_coords(widget_x(widget) + outside,
                                 widget_y(widget) + outside,
                                 NULL,
                                 NULL)) {
            fprintf(stderr, "map click test: out-of-bounds point resolved to a tile\n");
            success = false;
        }
    }

    setting_set_int(OPT_CAT_MAP, OPT_MAP_ZOOM, saved_zoom);
    widget->zoom = saved_widget_zoom;
    widget->zoom_x = saved_widget_zoom_x;
    widget->zoom_y = saved_widget_zoom_y;
    MapData.height_diff = saved_height_diff;
    map_cell_store_destroy(test_cells);
    level_cells[MAP2_DEPTH_INDEX(0)] = saved_base_cells;
    cur_widget[MAP_ID] = saved_map_widget;
    current_level_index = saved_level_index;
    cells = saved_cells_pointer;

    return success;
}

bool widget_map_interaction_test(widgetdata *widget) {
    int old_map_width = map_width;
    int old_map_height = map_height;
    widgetdata *old_menu = cur_widget[MENU_ID];
    widgetdata menu = {0};
    SDL_Event event = {0};

    map_width = 10;
    map_height = 10;
    map_interaction_test_moves = 0;
    map_interaction_test_targets = 0;
    map_interaction_test_talks = 0;
    map_interaction_test_active = true;
    cur_widget[MENU_ID] = &menu;

    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    bool success = widget_event(widget, &event) == 1;
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    success = success && widget_event(widget, &event) == 1;
    success = success && arraysize(map_menu_actions) == 2;
    success = success && strcmp(map_menu_actions[0].name, "Walk Here") == 0;
    success = success && strcmp(map_menu_actions[1].name, "Talk To NPC") == 0;
    map_menu_actions[0].handler(widget, NULL, &event);
    map_menu_actions[1].handler(widget, NULL, &event);
    success = success && map_interaction_test_moves == 1;
    success = success && map_interaction_test_targets == 2;
    success = success && map_interaction_test_talks == 1;

    cur_widget[MENU_ID] = old_menu;
    map_interaction_test_active = false;
    map_width = old_map_width;
    map_height = old_map_height;
    return success;
}

bool widget_map_mouse_origin_test(int mx, int my, int expected_mx, int expected_my) {
    map_mouse_to_local_coords(&mx, &my);
    return mx == expected_mx && my == expected_my;
}

#endif

/** @copydoc widgetdata::background_func */
static void widget_background(widgetdata *widget, int draw) {
    if (!widget->redraw) {
        region_map_ready(MapData.region_map);
    }
}

/** @copydoc widgetdata::deinit_func */
void map_runtime_deinit(void) {
    map_state_transaction_abort();
    map_anims_clear();
    lighting_deinit();

    map_exit_cue_cache_clear(&map_animation_exit_cues);
    map_living_outline_cache_clear(&map_animation_living_outlines);
    for (size_t index = 0; index < arraysize(map_render_contexts); index++) {
        free(map_render_contexts[index].commands);
        free(map_render_contexts[index].annotations);
        free(map_render_contexts[index].tiles);
        free(map_render_contexts[index].candidates);
        free(map_render_contexts[index].cohorts);
        memset(&map_render_contexts[index], 0, sizeof(map_render_contexts[index]));
    }
    free(map_retained_primary_context.commands);
    free(map_retained_primary_context.annotations);
    free(map_retained_primary_context.tiles);
    free(map_retained_primary_context.candidates);
    free(map_retained_primary_context.cohorts);
    memset(&map_retained_primary_context, 0, sizeof(map_retained_primary_context));
    memset(&map_retained_primary_projection, 0, sizeof(map_retained_primary_projection));

    for (size_t i = 0; i < arraysize(level_cells); i++) {
        map_cell_store_destroy(level_cells[i]);
        level_cells[i] = NULL;
    }
    cells = NULL;
    memset(level_lighting_revision, 0, sizeof(level_lighting_revision));
    current_level_index = MAP2_DEPTH_INDEX(0);
    map_level_mask = 0;
    map_width = 0;
    map_height = 0;
    map_cache_origin_x = 0;
    map_cache_origin_y = 0;
    map_redraw_consume();
    map_benchmark_statistics_reset();

    if (MapData.region_map != NULL) {
        region_map_free(MapData.region_map);
        MapData.region_map = NULL;
    }
}

/** @copydoc widgetdata::deinit_func */
static void widget_deinit(widgetdata *widget) {
    map_runtime_deinit();
}

void map_runtime_init(void) {
    HARD_ASSERT(MapData.region_map == NULL);
    MapData.region_map = region_map_create();
    HARD_ASSERT(MapData.region_map != NULL);
}

/**
 * Initialize one map widget.
 */
void widget_map_init(widgetdata *widget) {
    map_runtime_init();

    widget->draw_func = widget_draw;
    widget->event_func = widget_event;
    widget->background_func = widget_background;
    widget->deinit_func = widget_deinit;
    widget->menu_handle_func = NULL;

    widget_enforce_map_priority();
}

/**
 * Add an animation.
 * @param type
 * Animation type, one of @ref ANIM_xxx.
 * @param mapx
 * Map X.
 * @param mapy
 * Map Y.
 * @param sub_layer
 * Sub-layer.
 * @param value
 * Value to display.
 * @return
 * Created animation.
 */
struct map_anim *map_anims_add(int type, int mapx, int mapy, int sub_layer, int depth, int value) {
    map_anim_t *anim;
    int num_ticks;

    anim = xcalloc(1, sizeof(*anim));

    DL_APPEND(first_anim, anim);

    /* Type */
    anim->type = type;

    /* Map coordinates */
    anim->mapx = mapx + MAP_STARTX;
    anim->mapy = mapy + MAP_STARTY;

    /* Sub-layer. */
    anim->sub_layer = sub_layer;
    anim->depth = depth;
    /* Amount of damage */
    anim->value = value;

    /* Current time in MilliSeconds */
    anim->start_tick = LastTick;

    switch (type) {
        case ANIM_DAMAGE:
            /* How many ticks to display */
            num_ticks = 850;
            anim->last_tick = anim->start_tick + num_ticks;
            /* 850 ticks 25 pixel move up */
            anim->yoff = -(25.0f / 850.0f);
            break;

        case ANIM_KILL:
            /* How many ticks to display */
            num_ticks = 850;
            anim->last_tick = anim->start_tick + num_ticks;
            /* 850 ticks 25 pixel move up */
            anim->yoff = -(25.0f / 850.0f);
            break;
    }

    return anim;
}

/**
 * Remove a map animation.
 * @param anim
 * The animation to remove.
 */
void maps_anims_remove(map_anim_t *anim) {
    HARD_ASSERT(anim != NULL);

    DL_DELETE(first_anim, anim);

    free(anim);
}

/**
 * Adjust the X/Y coordinates of map animations due to a map scroll.
 * @param xoff
 * X offset.
 * @param Yoff
 * Y offset.
 */
void map_anims_mapscroll(int xoff, int yoff) {
    map_anim_t *anim;
    DL_FOREACH(first_anim, anim) {
        anim->mapx -= xoff;
        anim->mapy -= yoff;
    }
}

/**
 * Clear map animations.
 */
void map_anims_clear(void) {
    map_anim_t *anim, *tmp;
    DL_FOREACH_SAFE(first_anim, anim, tmp) {
        maps_anims_remove(anim);
    }
}

/**
 * Play map animations.
 */
void map_anims_play(void) {
    map_render_data_t data = {.world_surface = true, .primary_level = true};
    map_select_level(0, true);
    map_setup_render_data(cur_widget[MAP_ID]->surface, &data, NULL, NULL, NULL, NULL);

    map_anim_t *anim, *tmp;
    DL_FOREACH_SAFE(first_anim, anim, tmp) {
        /* Have we passed the last tick */
        if (LastTick > anim->last_tick) {
            maps_anims_remove(anim);
            continue;
        }

        if (!map_select_level(anim->depth, false)) {
            continue;
        }

        data.depth = anim->depth;
        data.sub_layer = anim->sub_layer;
        data.x = anim->mapx;
        data.y = anim->mapy;
        if (!map_should_draw(cur_widget[MAP_ID]->surface, &data)) {
            continue;
        }

        int source_floor_height = get_top_floor_height(data.cell, data.sub_layer);
        data.ypos -= source_floor_height;
        data.ypos += data.player_height_offset;
#ifdef ATRINIK_WIDGET_TESTS
        if (map_animation_test_active && source_floor_height != 0 &&
            data.player_height_offset != 0) {
            map_animation_test_elevated_draws++;
        }
        if (map_animation_test_active) {
            map_animation_test_source_floor_height = source_floor_height;
            map_animation_test_player_floor_height = data.player_height_offset;
            if (anim->depth == map_animation_test_expected_depth &&
                data.sub_layer == map_animation_test_expected_sub_layer &&
                source_floor_height != 0 &&
                map_cell_layer_record_read(data.cell, GET_MAP_LAYER(LAYER_LIVING, data.sub_layer))
                        ->face != 0) {
                map_animation_test_layer_content_draws++;
            }
        }
#endif
        data.xpos += MAP_TILE_POS_XOFF / 2;
        data.ypos -= MAP_TILE_POS_YOFF;

        map_screen_point_t screen;
        SDL_Surface *displayed = map_displayed_surface(cur_widget[MAP_ID]);
        SOFT_ASSERT(map_local_anchor_to_screen(widget_x(cur_widget[MAP_ID]),
                                               widget_y(cur_widget[MAP_ID]),
                                               cur_widget[MAP_ID]->surface->w,
                                               cur_widget[MAP_ID]->surface->h,
                                               displayed->w,
                                               displayed->h,
                                               data.xpos,
                                               data.ypos,
                                               &screen),
                    "Map animation coordinate overflow");

        uint32_t num_ticks = LastTick - anim->start_tick;
        /* Font/icon dimensions and the historical 25-pixel rise over 850 ms
         * are screen-space presentation, so apply them after the anchor has
         * followed the map widget origin and zoom. */
        screen.y += map_screen_motion_offset(num_ticks, anim->yoff);
        screen.x += map_screen_motion_offset(num_ticks, anim->xoff);

        char buf[32];
        switch (anim->type) {
            case ANIM_DAMAGE: {
#ifdef ATRINIK_WIDGET_TESTS
                if (map_animation_test_active) {
                    map_animation_test_damage_draws++;
                }
#endif
                snprintf(VS(buf), "%d", abs(anim->value));
                int wd = text_get_width(FONT_MONO10, buf, TEXT_OUTLINE);
                const char *color = anim->value < 0 ? COLOR_GREEN : COLOR_ORANGE;

                text_show_root(FONT_MONO10,
                               buf,
                               screen.x - wd / 2,
                               screen.y,
                               color,
                               TEXT_OUTLINE,
                               NULL);
                break;
            }

            case ANIM_KILL: {
#ifdef ATRINIK_WIDGET_TESTS
                if (map_animation_test_active) {
                    map_animation_test_kill_draws++;
                }
#endif
                snprintf(VS(buf), "%d", anim->value);
                int wd = text_get_width(FONT_MONO10, buf, TEXT_OUTLINE);
                int ht = text_get_height(FONT_MONO10, buf, 0);
#ifdef ATRINIK_WIDGET_TESTS
                SDL_Surface *texture = map_animation_test_active ? map_animation_test_death_texture
                                                                 : TEXTURE_CLIENT("death");
#else
                SDL_Surface *texture = TEXTURE_CLIENT("death");
#endif
                HARD_ASSERT(texture != NULL);
                surface_show(OfflineRenderSurface,
                             screen.x - texture->w / 2,
                             screen.y - ht / 2 + 2,
                             NULL,
                             texture);

                text_show_root(FONT_MONO10,
                               buf,
                               screen.x - wd / 2,
                               screen.y,
                               COLOR_ORANGE,
                               TEXT_OUTLINE,
                               NULL);

                break;
            }

            default:
                LOG(ERROR, "Unknown animation type: %d", anim->type);
                break;
        }
    }

    map_select_level(0, true);
}

/**
 * Check whether the damage animations need redrawing.
 * @return
 * 1 if the damage animations need redrawing, 0 otherwise.
 */
int map_anims_need_redraw(void) {
    return first_anim != NULL;
}
