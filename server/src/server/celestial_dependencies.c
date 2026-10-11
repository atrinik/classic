/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>

#include <celestial_dependencies.h>
#include <map.h>
#include <region.h>
#include <openssl/sha.h>

#define DEPENDENCY_LIMIT 4096
#define FIELD_METADATA_LIMIT ((size_t)8192)

typedef struct dependency_entry {
    const mapstruct *map;
    const mapstruct *edges[TILED_NUM];
    int transform[3];
    unsigned char signature[SHA256_DIGEST_LENGTH];
} dependency_entry;

struct celestial_dependencies {
    struct celestial_dependencies *next;
    dependency_entry *entries;
    size_t count, capacity, maximum, references, last;
    size_t *reserved;
    size_t limit, bytes, share;
    bool invalid, completed;
};

typedef struct field_binding {
    struct field_binding *next;
    const mapstruct *map;
    celestial_dependencies *fields[2];
} field_binding;

static celestial_dependencies *contexts;
static field_binding *bindings;
static size_t retained_bytes, binding_count;

static bool retained_fits(size_t added_bytes, size_t added_bindings) {
    if (binding_count > SIZE_MAX / FIELD_METADATA_LIMIT - added_bindings ||
        added_bytes > SIZE_MAX - retained_bytes) {
        return false;
    }
    return retained_bytes + added_bytes <=
           (binding_count + added_bindings) * FIELD_METADATA_LIMIT;
}

/* Raw input is zero-initialized, including padding. Target metadata is read
 * only while every captured map handle is live: forget invalidates contexts
 * before teardown, and fresh checks invalid before accessing any map. */
static void signature(const mapstruct *map, unsigned char output[SHA256_DIGEST_LENGTH]) {
    struct {
        uintptr_t spaces, path, region;
        uint64_t count, revision;
        int width, height;
        uint32_t residency;
        uint8_t schema, sky;
        uint8_t profile[32];
        struct {
            uintptr_t target, path;
            uint64_t count;
            uint32_t residency;
            uint8_t boundary, seen;
        } edges[TILED_NUM];
    } raw;
    memset(&raw, 0, sizeof(raw));
    raw.spaces = (uintptr_t)map->spaces;
    raw.path = (uintptr_t)map->path;
    raw.region = (uintptr_t)map->region;
    raw.count = map->count;
    raw.revision = map->celestial_structure_revision;
    raw.width = map->width;
    raw.height = map->height;
    raw.residency = map->in_memory;
    raw.schema = map->celestial_schema;
    raw.sky = map->celestial_sky_above;
    const region_celestial_profile_t *profile = region_celestial_for_map(map);
    if (profile != NULL) {
        memcpy(raw.profile, profile->digest, sizeof(raw.profile));
    }
    for (size_t i = 0; i < TILED_NUM; i++) {
        const mapstruct *target = map->tile_map[i];
        raw.edges[i].target = (uintptr_t)target;
        raw.edges[i].path = (uintptr_t)map->tile_path[i];
        raw.edges[i].boundary = map->celestial_boundary[i];
        raw.edges[i].seen = map->celestial_tile_path_seen[i];
        if (target != NULL) {
            raw.edges[i].count = target->count;
            raw.edges[i].residency = target->in_memory;
        }
    }
    SHA256((const unsigned char *)&raw, sizeof(raw), output);
}

static bool reserve(size_t *used, size_t limit, size_t bytes) {
    if (*used > limit || bytes > limit - *used) {
        return false;
    }
    *used += bytes;
    return true;
}

celestial_dependencies *celestial_dependencies_begin(size_t *reserved,
                                                      size_t limit,
                                                      size_t max_entries) {
    if (reserved == NULL || max_entries == 0 ||
        !reserve(reserved, limit, sizeof(celestial_dependencies))) {
        return NULL;
    }
    celestial_dependencies *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        *reserved -= sizeof(*ctx);
        return NULL;
    }
    ctx->reserved = reserved;
    ctx->limit = limit;
    ctx->maximum = MIN(max_entries, (size_t)DEPENDENCY_LIMIT);
    ctx->bytes = sizeof(*ctx);
    ctx->references = 1;
    ctx->next = contexts;
    contexts = ctx;
    return ctx;
}

bool celestial_dependencies_observe_at(celestial_dependencies *ctx,
                                       const mapstruct *map, int tx, int ty, int z) {
    if (ctx == NULL || ctx->invalid || ctx->completed || map == NULL) {
        return false;
    }
    if (ctx->count != 0) {
        const dependency_entry *entry = &ctx->entries[ctx->last];
        if (entry->map == map && entry->transform[0] == tx &&
            entry->transform[1] == ty && entry->transform[2] == z) {
            return true;
        }
    }
    for (size_t i = 0; i < ctx->count; i++) {
        const dependency_entry *entry = &ctx->entries[i];
        if (entry->map == map && entry->transform[0] == tx &&
            entry->transform[1] == ty && entry->transform[2] == z) {
            ctx->last = i;
            return true;
        }
    }
    if (ctx->count == ctx->maximum) {
        ctx->invalid = true;
        return false;
    }
    if (ctx->count == ctx->capacity) {
        size_t capacity = MIN(ctx->maximum, ctx->capacity == 0 ? 4 : ctx->capacity * 2);
        size_t growth = (capacity - ctx->capacity) * sizeof(dependency_entry);
        if (!reserve(ctx->reserved, ctx->limit, growth)) {
            ctx->invalid = true;
            return false;
        }
        dependency_entry *entries = realloc(ctx->entries, capacity * sizeof(*entries));
        if (entries == NULL) {
            *ctx->reserved -= growth;
            ctx->invalid = true;
            return false;
        }
        ctx->entries = entries;
        ctx->capacity = capacity;
        ctx->bytes += growth;
    }
    ctx->last = ctx->count;
    dependency_entry *entry = &ctx->entries[ctx->count++];
    entry->map = map;
    entry->transform[0] = tx;
    entry->transform[1] = ty;
    entry->transform[2] = z;
    for (size_t i = 0; i < TILED_NUM; i++) {
        entry->edges[i] = map->tile_map[i];
    }
    signature(map, entry->signature);
    return true;
}

bool celestial_dependencies_observe(celestial_dependencies *ctx, const mapstruct *map) {
    return celestial_dependencies_observe_at(ctx, map, 0, 0, 0);
}

bool celestial_dependencies_fresh(const celestial_dependencies *ctx) {
    if (ctx == NULL || ctx->invalid || ctx->count == 0) {
        return false;
    }
    for (size_t i = 0; i < ctx->count; i++) {
        /* A newly linked target is not one of our retained lifetime handles.
         * Detect that change before signature() can inspect its residency. */
        for (size_t j = 0; j < TILED_NUM; j++) {
            if (ctx->entries[i].map->tile_map[j] != ctx->entries[i].edges[j]) {
                return false;
            }
        }
        unsigned char current[SHA256_DIGEST_LENGTH];
        signature(ctx->entries[i].map, current);
        if (memcmp(current, ctx->entries[i].signature, sizeof(current)) != 0) {
            return false;
        }
    }
    return true;
}

bool celestial_dependencies_references_map(const celestial_dependencies *ctx,
                                           const mapstruct *map) {
    if (ctx == NULL || map == NULL) {
        return false;
    }
    for (size_t i = 0; i < ctx->count; i++) {
        if (ctx->entries[i].map == map) {
            return true;
        }
        for (size_t j = 0; j < TILED_NUM; j++) {
            if (ctx->entries[i].edges[j] == map) {
                return true;
            }
        }
    }
    return false;
}

size_t celestial_dependencies_bytes(const celestial_dependencies *ctx) {
    return ctx != NULL ? ctx->bytes : 0;
}

size_t celestial_dependencies_count(const celestial_dependencies *ctx) {
    return ctx != NULL ? ctx->count : 0;
}

void celestial_dependencies_release(celestial_dependencies *ctx) {
    if (ctx == NULL || --ctx->references != 0) {
        return;
    }
    celestial_dependencies **link = &contexts;
    while (*link != ctx) {
        link = &(*link)->next;
    }
    *link = ctx->next;
    if (ctx->reserved != NULL) {
        HARD_ASSERT(*ctx->reserved >= ctx->bytes);
        *ctx->reserved -= ctx->bytes;
    } else if (ctx->completed) {
        HARD_ASSERT(retained_bytes >= ctx->bytes);
        retained_bytes -= ctx->bytes;
    }
    free(ctx->entries);
    free(ctx);
}

static field_binding *find_binding(const mapstruct *map) {
    for (field_binding *binding = bindings; binding != NULL; binding = binding->next) {
        if (binding->map == map) {
            return binding;
        }
    }
    return NULL;
}

static bool fits_binding(const field_binding *binding, size_t share, bool next) {
    size_t other = binding != NULL && binding->fields[!next] != NULL ?
                       binding->fields[!next]->share : 0;
    return share <= FIELD_METADATA_LIMIT - sizeof(field_binding) &&
           other <= FIELD_METADATA_LIMIT - sizeof(field_binding) - share;
}

bool celestial_dependencies_finish(celestial_dependencies *ctx,
                                    mapstruct *const *maps, size_t count, bool next) {
    if (ctx == NULL || ctx->completed || maps == NULL || count == 0 ||
        count > MAP2_LEVELS || !celestial_dependencies_fresh(ctx)) {
        return false;
    }
    size_t share = (ctx->bytes + count - 1) / count;
    field_binding *selected[MAP2_LEVELS] = {0};
    bool created[MAP2_LEVELS] = {0};
    size_t reserved = 0, added_bindings = 0;
    for (size_t i = 0; i < count; i++) {
        if (maps[i] == NULL || !celestial_dependencies_references_map(ctx, maps[i])) {
            goto fail;
        }
        for (size_t j = 0; j < i; j++) {
            if (maps[i] == maps[j]) {
                goto fail;
            }
        }
        selected[i] = find_binding(maps[i]);
        if (!fits_binding(selected[i], share, next)) {
            goto fail;
        }
        if (selected[i] == NULL) {
            if (!reserve(ctx->reserved, ctx->limit, sizeof(field_binding))) {
                goto fail;
            }
            reserved += sizeof(field_binding);
            selected[i] = calloc(1, sizeof(field_binding));
            if (selected[i] == NULL) {
                goto fail;
            }
            created[i] = true;
            added_bindings++;
            selected[i]->map = maps[i];
        }
    }
    if (!retained_fits(ctx->bytes + reserved, added_bindings)) {
        goto fail;
    }
    retained_bytes += ctx->bytes + reserved;
    binding_count += added_bindings;
    ctx->share = share;
    ctx->completed = true;
    HARD_ASSERT(*ctx->reserved >= ctx->bytes + reserved);
    *ctx->reserved -= ctx->bytes + reserved;
    ctx->reserved = NULL;
    for (size_t i = 0; i < count; i++) {
        if (created[i]) {
            selected[i]->next = bindings;
            bindings = selected[i];
        }
        celestial_dependencies *old = selected[i]->fields[next];
        selected[i]->fields[next] = ctx;
        ctx->references++;
        celestial_dependencies_release(old);
    }
    return true;

fail:
    for (size_t i = 0; i < count; i++) {
        if (created[i]) {
            free(selected[i]);
        }
    }
    *ctx->reserved -= reserved;
    ctx->invalid = true;
    return false;
}

bool celestial_dependencies_field_fresh(const mapstruct *map, bool next) {
    const field_binding *binding = find_binding(map);
    return binding != NULL && celestial_dependencies_fresh(binding->fields[next]);
}

celestial_dependencies *celestial_dependencies_field_get(const mapstruct *map, bool next) {
    field_binding *binding = find_binding(map);
    celestial_dependencies *ctx = binding != NULL ? binding->fields[next] : NULL;
    if (ctx != NULL) {
        ctx->references++;
    }
    return ctx;
}

bool celestial_dependencies_field_bind(celestial_dependencies *ctx,
                                        mapstruct *map, bool next) {
    field_binding *binding = find_binding(map);
    if (ctx == NULL || !ctx->completed || map == NULL ||
        !celestial_dependencies_references_map(ctx, map) ||
        !fits_binding(binding, ctx->share, next)) {
        return false;
    }
    if (binding == NULL) {
        if (!retained_fits(sizeof(*binding), 1)) {
            return false;
        }
        binding = calloc(1, sizeof(*binding));
        if (binding == NULL) {
            return false;
        }
        binding->map = map;
        binding->next = bindings;
        bindings = binding;
        retained_bytes += sizeof(*binding);
        binding_count++;
    }
    celestial_dependencies *old = binding->fields[next];
    ctx->references++;
    binding->fields[next] = ctx;
    celestial_dependencies_release(old);
    return true;
}

void celestial_dependencies_field_release(const mapstruct *map, bool next) {
    field_binding **link = &bindings;
    while (*link != NULL) {
        field_binding *binding = *link;
        if (binding->map != map) {
            link = &binding->next;
            continue;
        }
        celestial_dependencies_release(binding->fields[next]);
        binding->fields[next] = NULL;
        /* Keep the map's allowance until actual teardown. Snapshot references
         * may retain a former field after its registry slot is replaced. */
        return;
    }
}

void celestial_dependencies_forget(const mapstruct *map) {
    for (celestial_dependencies *ctx = contexts; ctx != NULL; ctx = ctx->next) {
        if (celestial_dependencies_references_map(ctx, map)) {
            ctx->invalid = true;
        }
    }
}

void celestial_dependencies_field_forget(const mapstruct *map) {
    celestial_dependencies_forget(map);
    /* Drop all cache references to now-unusable contexts before withdrawing
     * this map's retained-memory allowance. Other map bindings stay resident. */
    field_binding **link = &bindings;
    while (*link != NULL) {
        field_binding *binding = *link;
        for (size_t i = 0; i < 2; i++) {
            if (binding->fields[i] != NULL &&
                (binding->map == map || binding->fields[i]->invalid)) {
                celestial_dependencies_release(binding->fields[i]);
                binding->fields[i] = NULL;
            }
        }
        if (binding->map == map) {
            *link = binding->next;
            retained_bytes -= sizeof(*binding);
            binding_count--;
            free(binding);
        } else {
            link = &binding->next;
        }
    }
}
