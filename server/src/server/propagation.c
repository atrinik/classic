/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <object.h>
#include <propagation.h>
#include <server_main.h>
#include <spatial.h>

#define NO_ENTRY UINT32_MAX

typedef struct propagation_cast propagation_cast;
struct propagation_entry {
    propagation_cast *cast;
    spatial_cell cell;
    object *effect, *drop;
    tag_t effect_count, drop_count;
    uint32_t next;
    int cost, hp;
    bool queued, expanded;
};

struct propagation_cast {
    propagation_cast *next, *ready_next;
    struct propagation_entry *entries;
    uint32_t *hash;
    size_t capacity, hash_size, used, active, reservation;
    uint64_t group;
    uint32_t first, last;
    int radius, axis[3], damage, level, argument;
    archetype_t *arch;
    object *source, *owner;
    tag_t source_count, owner_count;
    propagation_visit visit;
    bool cancelled, reserved_projectile, scheduled;
};

static propagation_cast *casts, *last_cast, *ready_first, *ready_last;
static size_t reserved, last_expansions;
static bool processing;
static uint64_t next_group;

static size_t volume(int radius) {
    if (radius < 0 || radius > 35) {
        return 0;
    }
    size_t side = 2u * (unsigned)radius + 1u;
    size_t cells = side * side * MIN(side, 13u);
    return cells <= PROPAGATION_CAST_CELLS ? cells : 0;
}

bool propagation_can_admit(int radius) {
    size_t cells = volume(radius);
    return cells != 0 && cells <= PROPAGATION_GLOBAL_CELLS - reserved;
}

size_t propagation_reserved_cells(void) { return reserved; }
size_t propagation_last_expansions(void) { return last_expansions; }

static size_t cell_hash(const spatial_cell *cell) {
    uintptr_t value = (uintptr_t)cell->map;
    value ^= (uintptr_t)(unsigned)cell->x * UINT32_C(2654435761);
    value ^= (uintptr_t)(unsigned)cell->y * UINT32_C(2246822519);
    return (size_t)(value ^ (value >> 16));
}

static struct propagation_entry *admit(propagation_cast *cast,
                                        const spatial_cell *cell, int cost) {
    size_t slot = cell_hash(cell) & (cast->hash_size - 1);
    while (cast->hash[slot] != NO_ENTRY) {
        struct propagation_entry *entry = &cast->entries[cast->hash[slot]];
        if (entry->cell.map == cell->map && entry->cell.x == cell->x &&
            entry->cell.y == cell->y) {
            return NULL;
        }
        slot = (slot + 1) & (cast->hash_size - 1);
    }
    if (cast->used == cast->capacity) {
        return NULL;
    }
    cast->hash[slot] = (uint32_t)cast->used;
    struct propagation_entry *entry = &cast->entries[cast->used++];
    entry->cast = cast;
    entry->cell = *cell;
    entry->cost = cost;
    entry->next = NO_ENTRY;
    return entry;
}

static void schedule(propagation_cast *cast) {
    if (cast->scheduled || cast->first == NO_ENTRY || cast->cancelled) {
        return;
    }
    cast->scheduled = true;
    cast->ready_next = NULL;
    if (ready_last != NULL) {
        ready_last->ready_next = cast;
    } else {
        ready_first = cast;
    }
    ready_last = cast;
}

static void enqueue(struct propagation_entry *entry) {
    propagation_cast *cast = entry->cast;
    if (entry->queued || entry->expanded || cast->cancelled) {
        return;
    }
    entry->queued = true;
    uint32_t index = (uint32_t)(entry - cast->entries);
    if (cast->last == NO_ENTRY) {
        cast->first = index;
    } else {
        cast->entries[cast->last].next = index;
    }
    cast->last = index;
    schedule(cast);
}

static propagation_cast *create_reserved(object *source, int radius, size_t capacity,
                                         size_t reservation, propagation_cast *credit) {
    object *location = source;
    while (location->env != NULL) {
        location = location->env;
    }
    if (location->map == NULL || capacity == 0 || reservation > PROPAGATION_CAST_CELLS ||
        (credit != NULL ? credit->reservation <= reservation :
         reservation > PROPAGATION_GLOBAL_CELLS - reserved)) {
        return NULL;
    }
    propagation_cast *cast = calloc(1, sizeof(*cast));
    if (cast == NULL) {
        return NULL;
    }
    cast->capacity = capacity;
    cast->reservation = reservation;
    cast->group = credit != NULL ? credit->group : ++next_group;
    cast->hash_size = 1;
    while (cast->hash_size < cast->capacity * 2) {
        cast->hash_size *= 2;
    }
    cast->entries = calloc(cast->capacity, sizeof(*cast->entries));
    cast->hash = malloc(cast->hash_size * sizeof(*cast->hash));
    if (cast->entries == NULL || cast->hash == NULL) {
        free(cast->entries);
        free(cast->hash);
        free(cast);
        return NULL;
    }
    memset(cast->hash, 0xff, cast->hash_size * sizeof(*cast->hash));
    cast->radius = radius;
    cast->first = cast->last = NO_ENTRY;
    cast->source = source;
    cast->source_count = source->count;
    cast->owner = object_owner(source);
    if (cast->owner == NULL) {
        cast->owner = source;
    }
    cast->owner_count = cast->owner->count;
    spatial_cell origin = {location->map, location->x, location->y, 0};
    (void)admit(cast, &origin, 0);
    if (last_cast != NULL) {
        last_cast->next = cast;
    } else {
        casts = cast;
    }
    last_cast = cast;
    if (credit != NULL) {
        credit->reservation -= reservation;
    } else {
        reserved += reservation;
    }
    return cast;
}

static propagation_cast *create(object *source, int radius) {
    return create_reserved(source, radius, volume(radius), volume(radius), NULL);
}

bool propagation_reserve_swarm(object *effect, int strength, unsigned count) {
    size_t cells = volume(strength + 1);
    if (cells == 0 || count == 0 || count > (PROPAGATION_CAST_CELLS - 1) / cells) {
        return false;
    }
    propagation_cast *cast = create_reserved(effect, 0, 1, 1 + count * cells, NULL);
    if (cast == NULL) {
        return false;
    }
    cast->first = cast->last = NO_ENTRY;
    cast->entries[0].queued = false;
    cast->entries[0].effect = effect;
    cast->entries[0].effect_count = effect->count;
    effect->propagation = &cast->entries[0];
    cast->active = 1;
    return true;
}

bool propagation_area(object *source, int radius, propagation_visit visit, int argument) {
    propagation_cast *cast = create(source, radius);
    if (cast == NULL) {
        return false;
    }
    cast->visit = visit;
    cast->argument = argument;
    enqueue(&cast->entries[0]);
    return true;
}

bool propagation_reserve_explosion_from(object *effect, object *parent, int strength) {
    if (strength < 0 || strength >= 35) {
        return false;
    }
    propagation_cast *credit = parent != NULL && parent->type == SWARM_SPELL &&
        parent->propagation != NULL ? parent->propagation->cast : NULL;
    size_t cells = volume(strength + 1);
    propagation_cast *cast = create_reserved(effect, strength + 1, cells, cells, credit);
    if (cast == NULL) {
        return false;
    }
    cast->reserved_projectile = true;
    cast->first = cast->last = NO_ENTRY;
    cast->entries[0].queued = false;
    cast->entries[0].effect = effect;
    cast->entries[0].effect_count = effect->count;
    effect->propagation = &cast->entries[0];
    cast->active = 1;
    return true;
}

bool propagation_reserve_explosion(object *effect, int strength) {
    return propagation_reserve_explosion_from(effect, NULL, strength);
}

bool propagation_cone(object *source, object *caster, archetype_t *arch,
                      int strength, int damage, int dx, int dy, int dz) {
    if (arch == NULL || strength < 0 || strength >= 35) {
        return false;
    }
    propagation_cast *cast = source->propagation != NULL ? source->propagation->cast : NULL;
    if (cast != NULL && cast->reserved_projectile && volume(strength + 1) <= cast->capacity) {
        propagation_object_destroyed(source);
        memset(cast->entries, 0, cast->capacity * sizeof(*cast->entries));
        memset(cast->hash, 0xff, cast->hash_size * sizeof(*cast->hash));
        cast->used = 0;
        cast->first = cast->last = NO_ENTRY;
        cast->reserved_projectile = false;
        cast->radius = strength + 1;
        spatial_cell origin = {source->map, source->x, source->y, 0};
        enqueue(admit(cast, &origin, 0));
    } else {
        cast = create(source, strength + 1);
    }
    if (cast == NULL) {
        return false;
    }
    cast->arch = arch;
    cast->damage = damage;
    cast->level = SK_level(caster);
    cast->axis[0] = dx;
    cast->axis[1] = dy;
    cast->axis[2] = dz;
    cast->entries[0].hp = strength;
    enqueue(&cast->entries[0]);
    return true;
}

void propagation_expand(object *effect) {
    struct propagation_entry *entry = effect->propagation;
    if (entry == NULL) {
        /* Authored cone objects and magic storms enter through the same policy. */
        int radius = effect->stats.hp + 2;
        propagation_cast *cast = create(effect, radius);
        if (cast == NULL) {
            return;
        }
        cast->arch = effect->arch;
        cast->damage = effect->stats.dam;
        cast->level = effect->level;
        int direction = absdir(effect->stats.sp);
        cast->axis[0] = freearr_x[direction];
        cast->axis[1] = freearr_y[direction];
        entry = &cast->entries[0];
        entry->effect = effect;
        entry->effect_count = effect->count;
        effect->propagation = entry;
        cast->active = 1;
        SET_FLAG(effect, FLAG_NO_SAVE);
    }
    entry->hp = effect->stats.hp + 1;
    enqueue(entry);
}

void propagation_track_drop(object *effect, object *drop) {
    struct propagation_entry *entry = effect->propagation;
    if (entry == NULL || drop == NULL) {
        return;
    }
    entry->drop = drop;
    entry->drop_count = drop->count;
    drop->propagation = entry;
    SET_FLAG(drop, FLAG_NO_SAVE);
    entry->cast->active++;
}

void propagation_object_destroyed(object *effect) {
    struct propagation_entry *entry = effect->propagation;
    if (entry == NULL) {
        return;
    }
    effect->propagation = NULL;
    if (entry->effect == effect) {
        entry->effect = NULL;
        entry->cast->active--;
    } else if (entry->drop == effect) {
        entry->drop = NULL;
        entry->cast->active--;
    }
}

static void cancel(propagation_cast *cast) {
    if (cast->cancelled) {
        return;
    }
    cast->cancelled = true;
    cast->first = cast->last = NO_ENTRY;
    for (size_t i = 0; i < cast->used; i++) {
        struct propagation_entry *entry = &cast->entries[i];
        object *effect = entry->effect;
        object *drop = entry->drop;
        if (effect != NULL && effect->count == entry->effect_count && !OBJECT_FREE(effect)) {
            if (!QUERY_FLAG(effect, FLAG_REMOVED)) {
                object_remove(effect, 0);
            }
            object_destroy(effect);
        }
        if (drop != NULL && drop->count == entry->drop_count && !OBJECT_FREE(drop)) {
            if (!QUERY_FLAG(drop, FLAG_REMOVED)) {
                object_remove(drop, 0);
            }
            object_destroy(drop);
        }
    }
}

static void collect(void) {
    /* Dormant projectile reservations never participate in scheduling. */
    propagation_cast **ready = &ready_first;
    ready_last = NULL;
    while (*ready != NULL) {
        propagation_cast *cast = *ready;
        if (cast->first == NO_ENTRY) {
            *ready = cast->ready_next;
            cast->ready_next = NULL;
            cast->scheduled = false;
        } else {
            ready_last = cast;
            ready = &cast->ready_next;
        }
    }
    propagation_cast **link = &casts;
    last_cast = NULL;
    while (*link != NULL) {
        propagation_cast *cast = *link;
        if (cast->first == NO_ENTRY && cast->active == 0) {
            *link = cast->next;
            reserved -= cast->reservation;
            free(cast->entries);
            free(cast->hash);
            free(cast);
        } else {
            last_cast = cast;
            link = &cast->next;
        }
    }
}

void propagation_expire_map(mapstruct *map) {
    for (propagation_cast *cast = casts; cast != NULL; cast = cast->next) {
        for (size_t i = 0; i < cast->used; i++) {
            if (cast->entries[i].cell.map == map ||
                (cast->entries[i].effect != NULL && cast->entries[i].effect->map == map) ||
                (cast->entries[i].drop != NULL && cast->entries[i].drop->map == map)) {
                uint64_t group = cast->group;
                for (propagation_cast *member = casts; member != NULL; member = member->next) {
                    if (member->group == group) {
                        cancel(member);
                    }
                }
                break;
            }
        }
    }
    if (!processing) {
        collect();
    }
}

static bool in_cone(const propagation_cast *cast, int dx, int dy, int dz) {
    int64_t ax = cast->axis[0], ay = cast->axis[1], az = cast->axis[2];
    int64_t norm = ax * ax + ay * ay + az * az;
    int64_t dot = ax * dx + ay * dy + az * dz;
    return norm == 0 || (dot > 0 && 2 * dot * dot >= norm * (dx * dx + dy * dy + dz * dz));
}

static void expand(propagation_cast *cast, struct propagation_entry *entry) {
    if (cast->visit != NULL) {
        if (!OBJECT_VALID(cast->source, cast->source_count)) {
            cancel(cast);
            return;
        }
        cast->visit(cast->source, entry->cell.map, entry->cell.x, entry->cell.y,
                    cast->argument);
    }
    if (cast->cancelled || entry->cost >= cast->radius) {
        return;
    }
    for (int dz = -1; dz <= 1; dz++) {
        if (abs(entry->cell.z + dz) > 6) {
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if ((dx == 0 && dy == 0 && dz == 0) || !in_cone(cast, dx, dy, dz)) {
                    continue;
                }
                spatial_cell cell;
                if (!spatial_ray_clear(entry->cell.map, entry->cell.x, entry->cell.y,
                                       dx, dy, dz, SPATIAL_COMBAT, false) ||
                    !spatial_resolve(entry->cell.map, entry->cell.x + dx,
                                     entry->cell.y + dy, dz, &cell)) {
                    continue;
                }
                cell.z = entry->cell.z + dz;
                struct propagation_entry *child = admit(cast, &cell, entry->cost + 1);
                if (child == NULL) {
                    continue;
                }
                if (cast->visit != NULL) {
                    enqueue(child);
                    continue;
                }
                object *effect = arch_to_object(cast->arch);
                effect->x = cell.x;
                effect->y = cell.y;
                effect->level = cast->level;
                effect->stats.hp = entry->hp;
                effect->stats.dam = cast->damage;
                effect->stats.maxhp = effect->count;
                effect->stats.food = 0;
                effect->propagation = child;
                child->effect = effect;
                child->effect_count = effect->count;
                cast->active++;
                SET_FLAG(effect, FLAG_NO_SAVE);
                if (OBJECT_VALID(cast->owner, cast->owner_count)) {
                    object_owner_set(effect, cast->owner);
                }
                effect = object_insert_map(effect, cell.map, NULL, INS_NO_MERGE);
                if (entry->cost == 0 && effect != NULL && effect->other_arch != NULL) {
                    cone_drop(effect);
                }
                if (cast->cancelled) {
                    return;
                }
            }
        }
    }
}

void propagation_tick(void) {
    last_expansions = 0;
    processing = true;
    while (last_expansions < PROPAGATION_TICK_EXPANSIONS && ready_first != NULL) {
        propagation_cast *cast = ready_first;
        ready_first = cast->ready_next;
        if (ready_first == NULL) {
            ready_last = NULL;
        }
        cast->scheduled = false;
        cast->ready_next = NULL;
        if (cast->first == NO_ENTRY) {
            continue;
        }
        struct propagation_entry *entry = &cast->entries[cast->first];
        cast->first = entry->next;
        if (cast->first == NO_ENTRY) {
            cast->last = NO_ENTRY;
        }
        entry->queued = false;
        entry->expanded = true;
        entry->next = NO_ENTRY;
        last_expansions++;
        expand(cast, entry);
        schedule(cast);
    }
    processing = false;
    collect();
}
