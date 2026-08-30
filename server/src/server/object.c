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
 * Object related code.
 */

#include <global.h>
#include <server_main.h>
#include <server_item.h>
#include <server.h>
#include <quest.h>
#include <los.h>
#include <shop.h>
#include <light.h>
#include <connection.h>
#include <loader.h>
#include <toolkit/string.h>
#include <monster_data.h>
#include <plugin.h>
#include <arch.h>
#include <object.h>
#include <player.h>
#include <gameplay_journal.h>
#include <object_methods.h>
#include <door.h>
#include <exit.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <errno.h>

/** List of active objects that need to be processed */
object *active_objects;

/**
 * Gender nouns.
 */
const char *gender_noun[GENDER_MAX] = {"neuter", "male", "female", "hermaphrodite"};
/**
 * Subjective pronouns.
 */
const char *gender_subjective[GENDER_MAX] = {"it", "he", "she", "it"};
/**
 * Subjective pronouns, with first letter in uppercase.
 */
const char *gender_subjective_upper[GENDER_MAX] = {"It", "He", "She", "It"};
/**
 * Objective pronouns.
 */
const char *gender_objective[GENDER_MAX] = {"it", "him", "her", "it"};
/**
 * Possessive pronouns.
 */
const char *gender_possessive[GENDER_MAX] = {"its", "his", "her", "its"};
/**
 * Reflexive pronouns.
 */
const char *gender_reflexive[GENDER_MAX] = {"itself", "himself", "herself", "itself"};

/**
 * X offset when searching around a spot.
 */
int freearr_x[SIZEOFFREE] = {
    /* Same tile */
    0,
    /* One square away */
    0,
    1,
    1,
    1,
    0,
    -1,
    -1,
    -1,
    /* Two squares away */
    0,
    1,
    2,
    2,
    2,
    2,
    2,
    1,
    0,
    -1,
    -2,
    -2,
    -2,
    -2,
    -2,
    -1,
    /* Three squares away */
    0,
    1,
    2,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    2,
    1,
    0,
    -1,
    -2,
    -3,
    -3,
    -3,
    -3,
    -3,
    -3,
    -3,
    -2,
    -1,
};

/**
 * Y offset when searching around a spot.
 */
int freearr_y[SIZEOFFREE] = {
    /* Same tile */
    0,
    /* One square away */
    -1,
    -1,
    0,
    1,
    1,
    1,
    0,
    -1,
    /* Two squares away */
    -2,
    -2,
    -2,
    -1,
    0,
    1,
    2,
    2,
    2,
    2,
    2,
    1,
    0,
    -1,
    -2,
    -2,
    /* Three squares away */
    -3,
    -3,
    -3,
    -3,
    -2,
    -1,
    0,
    1,
    2,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    2,
    1,
    0,
    -1,
    -2,
    -3,
    -3,
    -3,
};

/**
 * Number of spots around a location, including that location (except for 0).
 */
int maxfree[SIZEOFFREE] = {
    /* Same tile */
    0,
    /* One square away */
    9,
    10,
    13,
    14,
    17,
    18,
    21,
    22,
    /* Two squares away */
    25,
    26,
    27,
    30,
    31,
    32,
    33,
    36,
    37,
    39,
    39,
    42,
    43,
    44,
    45,
    48,
    /* Three squares away */
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
    49,
};

/**
 * Direction we're pointing on this spot.
 */
int freedir[SIZEOFFREE] = {
    /* Same tile */
    0,
    /* One square away */
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    /* Two squares away */
    1,
    2,
    2,
    2,
    3,
    4,
    4,
    4,
    5,
    6,
    6,
    6,
    7,
    8,
    8,
    8,
    /* Three squares away */
    1,
    2,
    2,
    2,
    2,
    2,
    3,
    4,
    4,
    4,
    4,
    4,
    5,
    6,
    6,
    6,
    6,
    6,
    7,
    8,
    8,
    8,
    8,
    8,
};

/**
 * The object memory pool.
 */
static mempool_struct *pool_object;

/**
 * This is a list of pointers that correspond to the FLAG_.. values.
 * This is a simple 1:1 mapping - if FLAG_FRIENDLY is 15, then the 15'th
 * element of this array should match that name.
 *
 * If an entry is NULL, that is a flag not to be loaded/saved.
 * @see flag_defines
 */
const char *object_flag_names[NUM_FLAGS + 1] = {"sleep",
                                                "confused",
                                                NULL,
                                                "scared",
                                                "is_blind",
                                                "is_invisible",
                                                "is_ethereal",
                                                "is_good",
                                                "no_pick",
                                                "walk_on",
                                                "no_pass",
                                                "is_animated",
                                                "slow_move",
                                                "flying",
                                                "monster",
                                                "friendly",
                                                NULL,
                                                "been_applied",
                                                "auto_apply",
                                                NULL,
                                                "is_neutral",
                                                "see_invisible",
                                                "can_roll",
                                                "connect_reset",
                                                "is_turnable",
                                                "walk_off",
                                                "fly_on",
                                                "fly_off",
                                                "is_used_up",
                                                "identified",
                                                "reflecting",
                                                "changing",
                                                "splitting",
                                                "hitback",
                                                "startequip",
                                                "blocksview",
                                                "undead",
                                                "can_stack",
                                                "unaggressive",
                                                "reflect_missile",
                                                "reflect_spell",
                                                "no_magic",
                                                "no_fix_player",
                                                "is_evil",
                                                "soulbound",
                                                "run_away",
                                                "pass_thru",
                                                "can_pass_thru",
                                                "outdoor",
                                                "unique",
                                                "no_drop",
                                                "is_indestructible",
                                                "can_cast_spell",
                                                NULL,
                                                "two_handed",
                                                "can_use_bow",
                                                "can_use_armour",
                                                "can_use_weapon",
                                                "connect_no_push",
                                                "connect_no_release",
                                                "has_ready_bow",
                                                "xrays",
                                                NULL,
                                                "is_floor",
                                                "lifesave",
                                                "is_magical",
                                                NULL,
                                                "stand_still",
                                                "random_move",
                                                "only_attack",
                                                NULL,
                                                "stealth",
                                                NULL,
                                                NULL,
                                                "cursed",
                                                "damned",
                                                "is_buildable",
                                                "no_pvp",
                                                NULL,
                                                NULL,
                                                "is_thrown",
                                                NULL,
                                                NULL,
                                                "is_male",
                                                "is_female",
                                                "applied",
                                                "inv_locked",
                                                NULL,
                                                NULL,
                                                NULL,
                                                "has_ready_weapon",
                                                "no_skill_ident",
                                                NULL,
                                                "can_see_in_dark",
                                                "is_cauldron",
                                                "is_dust",
                                                NULL,
                                                "one_hit",
                                                "draw_double_always",
                                                "berserk",
                                                "no_attack",
                                                "invulnerable",
                                                "quest_item",
                                                "is_trapped",
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL,
                                                "sys_object",
                                                "use_fix_pos",
                                                "unpaid",
                                                "hidden",
                                                "make_invisible",
                                                "make_ethereal",
                                                "is_player",
                                                "is_named",
                                                NULL,
                                                "no_teleport",
                                                "corpse",
                                                "corpse_forced",
                                                "player_only",
                                                NULL,
                                                "one_drop",
                                                "cursed_perm",
                                                "damned_perm",
                                                "door_closed",
                                                "is_spell",
                                                "is_missile",
                                                "draw_direction",
                                                "draw_double",
                                                "is_assassin",
                                                NULL,
                                                "no_save",
                                                NULL};

/** @copydoc chunk_debugger */
static void object_debugger(void *ptr, char *buf, size_t size) {
    object *op = ptr;
    snprintf(buf, size, "count: %d", op->count);

    if (op->name != NULL) {
        SET_FLAG(op, FLAG_IDENTIFIED);
        char *name = object_get_name_s(op, NULL);
        snprintfcat(buf, size, " name: %s", name);
        free(name);
    }

    snprintfcat(buf, size, " coords: %d, %d", op->x, op->y);
}

/** @copydoc chunk_validator */
static bool object_validator(void *ptr) {
    object *op = ptr;
    return op->count != 0 && !QUERY_FLAG(op, FLAG_REMOVED);
}

/**
 * Initialize the object API.
 */
void object_init(void) {
    pool_object = mempool_create("objects",
                                 OBJECT_EXPAND,
                                 sizeof(object),
                                 MEMPOOL_ALLOW_FREEING,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL);
    mempool_set_debugger(pool_object, object_debugger);
    mempool_set_validator(pool_object, object_validator);
}

/**
 * Deinitialize the object API.
 */
void object_deinit(void) {}

/**
 * Compares value lists.
 *
 * @param op
 * What to search.
 * @param cmp
 * Where to search.
 * @return
 * True if every key_values in 'op' has a partner with the same value
 * in 'cmp'.
 */
static inline bool object_can_merge_key_values_one(const object *op, const object *cmp) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(cmp != NULL);

    /* For each field in wants. */
    for (key_value_t *op_field = op->key_values; op_field != NULL; op_field = op_field->next) {
        key_value_t *cmp_field = object_get_key_link(cmp, op_field->key);
        if (cmp_field == NULL) {
            return false;
        }

        /* Found the matching field. */
        if (cmp_field->value != op_field->value) {
            return false;
        }
    }

    return true;
}

/**
 * Check if two objects have the same key values and thus can be merged.
 *
 * @param ob1
 * Object to check.
 * @param ob2
 * Object to check.
 * @return
 * True if ob1 has the same key_values as ob2.
 */
static inline bool object_can_merge_key_values(const object *ob1, const object *ob2) {
    HARD_ASSERT(ob1 != NULL);
    HARD_ASSERT(ob2 != NULL);

    return (object_can_merge_key_values_one(ob1, ob2) && object_can_merge_key_values_one(ob2, ob1));
}

/* Custody provenance is deliberately compact and bounded.  Each segment is
 * encoded as lineage@quantity and segments are separated by '|'.  Lineage
 * identifiers are generated by the journal and are restricted to the same
 * bounded size as other journal identifiers. */
#define OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS 8
#define OBJECT_CUSTODY_PROVENANCE_MAX_LENGTH 4096
#define OBJECT_CUSTODY_UNKNOWN_LINEAGE "legacy:unknown"

typedef struct object_custody_segment {
    char lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
    uint32_t quantity;
} object_custody_segment_t;

static int object_custody_segment_compare(const void *left, const void *right) {
    const object_custody_segment_t *a = left;
    const object_custody_segment_t *b = right;
    return strcmp(a->lineage, b->lineage);
}

static bool object_custody_segments_normalize(object_custody_segment_t *segments,
                                              size_t *count) {
    qsort(segments, *count, sizeof(*segments), object_custody_segment_compare);

    size_t output = 0;
    for (size_t i = 0; i < *count; i++) {
        if (segments[i].quantity == 0) {
            continue;
        }
        if (output != 0 && strcmp(segments[output - 1].lineage, segments[i].lineage) == 0) {
            if (UINT32_MAX - segments[output - 1].quantity < segments[i].quantity) {
                return false;
            }
            segments[output - 1].quantity += segments[i].quantity;
            continue;
        }
        if (output >= OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS) {
            return false;
        }
        segments[output++] = segments[i];
    }
    *count = output;
    return true;
}

static bool object_custody_segments_read(const object *op,
                                         object_custody_segment_t *segments,
                                         size_t *count) {
    *count = 0;
    if (op->custody_provenance == NULL) {
        if (op->custody_lineage == NULL) {
            return true;
        }
        if (strlen(op->custody_lineage) > GAMEPLAY_JOURNAL_ID_MAX) {
            return false;
        }
        snprintf(segments[0].lineage, sizeof(segments[0].lineage), "%s", op->custody_lineage);
        segments[0].quantity = MAX(1, op->nrof);
        *count = 1;
        return true;
    }

    if (*op->custody_provenance == '\0' ||
        strlen(op->custody_provenance) >= OBJECT_CUSTODY_PROVENANCE_MAX_LENGTH) {
        return false;
    }

    char encoded[OBJECT_CUSTODY_PROVENANCE_MAX_LENGTH];
    snprintf(encoded, sizeof(encoded), "%s", op->custody_provenance);
    char *save = NULL;
    for (char *token = strtok_r(encoded, "|", &save); token != NULL;
         token = strtok_r(NULL, "|", &save)) {
        if (*count >= OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS) {
            return false;
        }

        char *separator = strrchr(token, '@');
        if (separator == NULL || separator == token || separator[1] == '\0' ||
            strchr(token, '@') != separator) {
            return false;
        }

        *separator = '\0';
        if (strlen(token) > GAMEPLAY_JOURNAL_ID_MAX) {
            return false;
        }
        errno = 0;
        char *end = NULL;
        unsigned long long quantity = strtoull(separator + 1, &end, 10);
        if (errno == ERANGE || end == separator + 1 || *end != '\0' || quantity == 0 ||
            quantity > UINT32_MAX) {
            return false;
        }

        snprintf(segments[*count].lineage, sizeof(segments[*count].lineage), "%s", token);
        segments[*count].quantity = (uint32_t)quantity;
        (*count)++;
    }

    return *count != 0 && object_custody_segments_normalize(segments, count);
}

static bool object_custody_segments_write(object *op,
                                          const object_custody_segment_t *segments,
                                          size_t count) {
    if (count == 0) {
        FREE_AND_CLEAR_HASH2(op->custody_provenance);
        return true;
    }

    char encoded[OBJECT_CUSTODY_PROVENANCE_MAX_LENGTH];
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        int written = snprintf(encoded + used,
                               sizeof(encoded) - used,
                               "%s%s@%" PRIu32,
                               i == 0 ? "" : "|",
                               segments[i].lineage,
                               segments[i].quantity);
        if (written < 0 || (size_t)written >= sizeof(encoded) - used) {
            return false;
        }
        used += (size_t)written;
    }
    FREE_AND_COPY_HASH(op->custody_provenance, encoded);
    return true;
}

static bool object_custody_has_metadata(const object *op) {
    return op->custody_lineage != NULL || op->custody_provenance != NULL ||
           op->custody_first != NULL || op->custody_last != NULL || op->custody_actor != NULL;
}

static bool object_custody_has_pending_currency(const object *op) {
    return op->custody_lineage != NULL && strncmp(op->custody_lineage, "currency:", 9) == 0;
}

static bool object_custody_segments_for_merge(const object *left,
                                              const object *right,
                                              object_custody_segment_t *segments,
                                              size_t *count) {
    object_custody_segment_t left_segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    object_custody_segment_t right_segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t left_count, right_count;
    if (!object_custody_segments_read(left, left_segments, &left_count) ||
        !object_custody_segments_read(right, right_segments, &right_count)) {
        return false;
    }
    if (left_count == 0 && right_count == 0) {
        *count = 0;
        return true;
    }
    if (left_count == 0) {
        snprintf(left_segments[0].lineage,
                 sizeof(left_segments[0].lineage),
                 "%s",
                 OBJECT_CUSTODY_UNKNOWN_LINEAGE);
        left_segments[0].quantity = MAX(1, left->nrof);
        left_count = 1;
    }
    if (right_count == 0) {
        snprintf(right_segments[0].lineage,
                 sizeof(right_segments[0].lineage),
                 "%s",
                 OBJECT_CUSTODY_UNKNOWN_LINEAGE);
        right_segments[0].quantity = MAX(1, right->nrof);
        right_count = 1;
    }
    memcpy(segments, left_segments, left_count * sizeof(*segments));
    *count = left_count + right_count;
    for (size_t i = 0; i < right_count; i++) {
        bool found = false;
        for (size_t j = 0; j < left_count; j++) {
            if (strcmp(segments[j].lineage, right_segments[i].lineage) == 0) {
                if (UINT32_MAX - segments[j].quantity < right_segments[i].quantity) {
                    return false;
                }
                segments[j].quantity += right_segments[i].quantity;
                found = true;
                break;
            }
        }
        if (!found) {
            if (left_count >= OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS) {
                return false;
            }
            segments[left_count++] = right_segments[i];
        }
    }
    *count = left_count;
    return object_custody_segments_normalize(segments, count);
}

static bool object_custody_provenance_can_merge(const object *left, const object *right) {
    object_custody_segment_t segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t count;
    return object_custody_segments_for_merge(left, right, segments, &count);
}

static bool object_custody_provenance_merge(object *target, const object *source) {
    object_custody_segment_t segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t count;
    if (!object_custody_segments_for_merge(target, source, segments, &count) ||
        !object_custody_segments_write(target, segments, count)) {
        return false;
    }
    if (target->custody_lineage == NULL && source->custody_lineage != NULL) {
        target->custody_lineage = add_refcount(source->custody_lineage);
    }
    if (target->custody_first == NULL && source->custody_first != NULL) {
        target->custody_first = add_refcount(source->custody_first);
    }
    if (target->custody_last == NULL && source->custody_last != NULL) {
        target->custody_last = add_refcount(source->custody_last);
    }
    if (target->custody_actor == NULL && source->custody_actor != NULL) {
        target->custody_actor = add_refcount(source->custody_actor);
    }
    return true;
}

static bool object_custody_provenance_seed(object *op) {
    if (!object_custody_has_metadata(op)) {
        return true;
    }
    if (op->custody_provenance != NULL) {
        object_custody_segment_t existing[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
        size_t existing_count;
        return object_custody_segments_read(op, existing, &existing_count);
    }
    if (op->custody_lineage == NULL || strlen(op->custody_lineage) > GAMEPLAY_JOURNAL_ID_MAX) {
        return false;
    }
    object_custody_segment_t segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    snprintf(segments[0].lineage, sizeof(segments[0].lineage), "%s", op->custody_lineage);
    segments[0].quantity = MAX(1, op->nrof);
    return object_custody_segments_write(op, segments, 1);
}

static bool object_custody_provenance_add(object *op, const char *lineage, uint32_t quantity) {
    if (lineage == NULL || *lineage == '\0' || quantity == 0 ||
        strlen(lineage) > GAMEPLAY_JOURNAL_ID_MAX || strchr(lineage, '@') != NULL ||
        strchr(lineage, '|') != NULL) {
        return false;
    }
    object_custody_segment_t segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t count;
    if (!object_custody_segments_read(op, segments, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(segments[i].lineage, lineage) == 0) {
            if (UINT32_MAX - segments[i].quantity < quantity) {
                return false;
            }
            segments[i].quantity += quantity;
            return object_custody_segments_write(op, segments, count);
        }
    }
    if (count >= OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS) {
        return false;
    }
    snprintf(segments[count].lineage, sizeof(segments[count].lineage), "%s", lineage);
    segments[count++].quantity = quantity;
    return object_custody_segments_normalize(segments, &count) &&
           object_custody_segments_write(op, segments, count);
}

static bool object_custody_provenance_remove(object *op, uint32_t quantity) {
    object_custody_segment_t segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t count;
    if (quantity == 0 || !object_custody_segments_read(op, segments, &count) || count == 0) {
        return true;
    }
    for (size_t i = count; i > 0 && quantity != 0; i--) {
        uint32_t take = MIN(quantity, segments[i - 1].quantity);
        segments[i - 1].quantity -= take;
        quantity -= take;
    }
    if (quantity != 0) {
        return false;
    }
    return object_custody_segments_normalize(segments, &count) &&
           object_custody_segments_write(op, segments, count);
}

static bool object_custody_provenance_split(object *source, object *split, uint32_t quantity) {
    object_custody_segment_t source_segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    object_custody_segment_t split_segments[OBJECT_CUSTODY_PROVENANCE_MAX_SEGMENTS];
    size_t source_count = 0, split_count = 0;
    if (!object_custody_segments_read(source, source_segments, &source_count) ||
        source_count == 0 || quantity == 0) {
        return source_count == 0;
    }
    for (size_t i = source_count; i > 0 && quantity != 0; i--) {
        uint32_t take = MIN(quantity, source_segments[i - 1].quantity);
        source_segments[i - 1].quantity -= take;
        split_segments[split_count] = source_segments[i - 1];
        split_segments[split_count++].quantity = take;
        quantity -= take;
    }
    if (quantity != 0 || !object_custody_segments_normalize(source_segments, &source_count) ||
        !object_custody_segments_normalize(split_segments, &split_count) ||
        !object_custody_segments_write(source, source_segments, source_count) ||
        !object_custody_segments_write(split, split_segments, split_count)) {
        return false;
    }
    return true;
}

/**
 * Examines two objects, and returns true if they can be merged together.
 *
 * @param ob1
 * The first object.
 * @param ob2
 * The second object.
 * @return
 * True if the two object can merge, false otherwise.
 */
bool object_can_merge(object *ob1, object *ob2) {
    HARD_ASSERT(ob1 != NULL);
    HARD_ASSERT(ob2 != NULL);

    if (!QUERY_FLAG(ob1, FLAG_CAN_STACK) && ob1->type != EVENT_OBJECT) {
        return false;
    }

    if (ob1 == ob2) {
        return false;
    }

    /* Do not merge objects if nrof would overflow. We use INT32_MAX
     * because int32_t is often used to store nrof instead of uint32_t. */
    if ((uint64_t)ob1->nrof + ob2->nrof > INT32_MAX) {
        return false;
    }

    /* Do not merge objects with different layer/sub-layer. */
    if (ob1->layer != ob2->layer || ob1->sub_layer != ob2->sub_layer) {
        return false;
    }

    /* Do not allow merging objects if either has nrof of 0 and it's
     * not an event object (those normally have nrof of 0 but they are
     * allowed to merge). */
    if ((ob1->nrof == 0 || ob2->nrof == 0) && ob1->type != EVENT_OBJECT) {
        return false;
    }

    /* Do not ever merge objects with glow radius, since more objects with
     * the same glow_radius actually generate more light than one object. */
    if (ob1->glow_radius || ob2->glow_radius) {
        return false;
    }

    if (ob1->light_color != ob2->light_color) {
        return false;
    }

    /* Do not merge arrows with different owners. */
    if (ob1->type == ARROW && ob2->type == ARROW && ob1->attacked_by_count != 0 &&
        ob2->attacked_by_count != 0 && ob1->attacked_by_count != ob2->attacked_by_count) {
        return false;
    }

    /* Check attributes that cannot ever merge if they're different. */
    if (ob1->arch != ob2->arch || ob1->item_condition != ob2->item_condition ||
        ob1->item_level != ob2->item_level || ob1->item_power != ob2->item_power ||
        ob1->item_quality != ob2->item_quality || ob1->item_race != ob2->item_race ||
        ob1->item_skill != ob2->item_skill || ob1->last_grace != ob2->last_grace ||
        ob1->level != ob2->level || ob1->magic != ob2->magic || ob1->material != ob2->material ||
        ob1->material_real != ob2->material_real || ob1->other_arch != ob2->other_arch ||
        ob1->path_attuned != ob2->path_attuned || ob1->path_denied != ob2->path_denied ||
        ob1->path_repelled != ob2->path_repelled || ob1->randomitems != ob2->randomitems ||
        ob1->sub_type != ob2->sub_type || ob1->terrain_flag != ob2->terrain_flag ||
        ob1->terrain_type != ob2->terrain_type || ob1->type != ob2->type ||
        ob1->value != ob2->value || ob1->weight != ob2->weight) {
        return false;
    }

    if (!DBL_EQUAL(ob1->speed, ob2->speed) || !DBL_EQUAL(ob1->weapon_speed, ob2->weapon_speed)) {
        return false;
    }

    /* If the inventory consists only of event objects, and the event objects
     * are the same, allow merging. */
    if (ob1->inv != NULL || ob2->inv != NULL) {
        if (ob1->inv == NULL || ob2->inv == NULL) {
            return false;
        }

        /* Check that all inv objects are event objects */
        object *tmp1, *tmp2;
        for (tmp1 = ob1->inv, tmp2 = ob2->inv; tmp1 != NULL && tmp2 != NULL;
             tmp1 = tmp1->below, tmp2 = tmp2->below) {
            if (tmp1->type != EVENT_OBJECT || tmp2->type != EVENT_OBJECT) {
                return false;
            }
        }

        if (tmp1 != NULL || tmp2 != NULL) {
            /* Different number of event objects. */
            return false;
        }

        for (tmp1 = ob1->inv; tmp1 != NULL; tmp1 = tmp1->below) {
            for (tmp2 = ob2->inv; tmp2 != NULL; tmp2 = tmp2->below) {
                if (object_can_merge(tmp1, tmp2)) {
                    break;
                }
            }

            /* Couldn't find something to merge event from ob1 with? */
            if (tmp2 == NULL) {
                return false;
            }
        }
    }

    /* Check the shared strings of both objects. */
    if (ob1->name != ob2->name || ob1->name_pl != ob2->name_pl || ob1->title != ob2->title ||
        ob1->race != ob2->race || ob1->slaying != ob2->slaying || ob1->msg != ob2->msg ||
        ob1->artifact != ob2->artifact || ob1->custom_name != ob2->custom_name ||
        ob1->glow != ob2->glow) {
        return false;
    }

    /* Custody fields are private audit metadata, not gameplay identity. */
    /* Currency lineages are intentionally retained as merge barriers until
     * their journal transaction retires the tag.  Reconciliation locates
     * those outputs by custody_lineage. */
    if (object_custody_has_pending_currency(ob1) || object_custody_has_pending_currency(ob2) ||
        !object_custody_provenance_can_merge(ob1, ob2)) {
        return false;
    }

    /* Compare arrays and structures the object has (stats, protections, etc) */
    if (memcmp(&ob1->stats, &ob2->stats, sizeof(living)) != 0 ||
        memcmp(&ob1->attack, &ob2->attack, sizeof(ob1->attack)) != 0 ||
        memcmp(&ob1->protection, &ob2->protection, sizeof(ob1->protection)) != 0) {
        return false;
    }

    /* Ignore REMOVED and BEEN_APPLIED */
    if ((ob1->flags[0] | FLAG_BITMASK(FLAG_REMOVED) | FLAG_BITMASK(FLAG_BEEN_APPLIED)) !=
            (ob2->flags[0] | FLAG_BITMASK(FLAG_REMOVED) | FLAG_BITMASK(FLAG_BEEN_APPLIED)) ||
        (ob1->flags[1]) != (ob2->flags[1]) ||
        (ob1->flags[2] | FLAG_BITMASK(FLAG_APPLIED)) !=
            (ob2->flags[2] | FLAG_BITMASK(FLAG_APPLIED)) ||
        (ob1->flags[3]) != (ob2->flags[3])) {
        return false;
    }

    /* Compare face and animation IDs. */
    if (ob1->face != ob2->face || ob1->inv_face != ob2->inv_face ||
        ob1->animation_id != ob2->animation_id || ob1->inv_animation_id != ob2->inv_animation_id) {
        return false;
    }

    /* Avoid merging empty containers. */
    if (ob1->type == CONTAINER) {
        return false;
    }

    /* At least one of these has key_values. */
    if (ob1->key_values != NULL || ob2->key_values != NULL) {
        /* One has fields, but the other one doesn't. */
        if ((ob1->key_values == NULL) != (ob2->key_values == NULL)) {
            return false;
        }

        return object_can_merge_key_values(ob1, ob2);
    }

    return true;
}

/**
 * Tries to merge 'op' with items above and below the object.
 *
 * @param op
 * Object to merge.
 * @return
 * 'op', or the object 'op' was merged into.
 */
object *object_merge(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->nrof == 0 || !QUERY_FLAG(op, FLAG_CAN_STACK)) {
        return op;
    }

    object *tmp;
    if (op->map != NULL) {
        tmp = GET_MAP_OB_LAST(op->map, op->x, op->y);
    } else if (op->env != NULL) {
        tmp = op->env->inv;
    } else {
        return op;
    }

    for (; tmp != NULL; tmp = tmp->below) {
        if (tmp != op && object_can_merge(op, tmp)) {
            if (!object_custody_provenance_merge(tmp, op)) {
                continue;
            }
            tmp->nrof += op->nrof;
            esrv_update_item(UPD_NAME | UPD_NROF, tmp);

            object_remove(op, REMOVE_NO_WEIGHT);
            object_destroy(op);
            return tmp;
        }
    }

    return op;
}

/**
 * Recursive function to calculate the weight an object is carrying.
 *
 * It goes through in figures out how much containers are carrying, and
 * sums it up.
 *
 * @param op
 * The object to calculate the weight for
 * @return
 * The calculated weight
 */
uint32_t object_weight_sum(object *op) {
    HARD_ASSERT(op != NULL);

    if (QUERY_FLAG(op, FLAG_SYS_OBJECT)) {
        return 0;
    }

    uint32_t sum = 0;
    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        if (QUERY_FLAG(tmp, FLAG_SYS_OBJECT)) {
            continue;
        }

        if (tmp->inv != NULL) {
            object_weight_sum(tmp);
        }

        sum += WEIGHT_NROF(tmp, tmp->nrof);
    }

    if (op->type == CONTAINER && !DBL_EQUAL(op->weapon_speed, 1.0)) {
        /* We'll store the calculated value in damage_round_tag, so
         * we can use that as 'cache' for unmodified carrying weight.
         * This allows us to reliably calculate the weight again in
         * object_weight_add() and object_weight_sub() without
         * rounding errors. */
        op->damage_round_tag = sum;
        sum = sum * op->weapon_speed;
    }

    op->carrying = sum;
    return sum;
}

/**
 * Adds the specified weight to an object, and also updates how much the
 * environment(s) is/are carrying.
 *
 * @param op
 * The object.
 * @param weight
 * The weight to add.
 */
void object_weight_add(object *op, uint32_t weight) {
    HARD_ASSERT(op != NULL);

    while (op != NULL) {
        if (op->type == CONTAINER && !DBL_EQUAL(op->weapon_speed, 1.0)) {
            uint32_t old_carrying = op->carrying;
            op->damage_round_tag += weight;
            op->carrying = op->damage_round_tag * op->weapon_speed;
            weight = op->carrying - old_carrying;
        } else {
            op->carrying += weight;
        }

        if (op->env != NULL && op->env->type == PLAYER) {
            esrv_update_item(UPD_WEIGHT, op);
        }

        op = op->env;
    }
}

/**
 * Check whether adding weight can be represented through an inventory chain.
 *
 * @param op
 * First containing object.
 * @param weight
 * Unmodified weight to add.
 * @return
 * True if every updated 32-bit carrying value can represent the addition.
 */
bool object_weight_can_add(const object *op, uint64_t weight) {
    HARD_ASSERT(op != NULL);

    while (op != NULL) {
        if (weight > UINT32_MAX) {
            return false;
        }
        if (op->type == CONTAINER && !DBL_EQUAL(op->weapon_speed, 1.0)) {
            if (weight > UINT32_MAX - op->damage_round_tag) {
                return false;
            }
            uint64_t unmodified = op->damage_round_tag + weight;
            long double carrying = (long double)unmodified * op->weapon_speed;
            if (carrying < op->carrying || carrying > UINT32_MAX) {
                return false;
            }
            weight = (uint64_t)carrying - op->carrying;
        } else {
            if (weight > UINT32_MAX - op->carrying) {
                return false;
            }
        }
        op = op->env;
    }
    return true;
}

/**
 * Check whether moving weight between two inventory chains is representable.
 *
 * The object is still present in the source chain while this check runs, so a
 * plain addition check would count it twice at every shared ancestor.
 */
static bool
object_weight_can_move(const object *source, const object *destination, uint64_t weight) {
    HARD_ASSERT(destination != NULL);

    if (source == NULL) {
        return object_weight_can_add(destination, weight);
    }

    const object *common = destination;
    while (common != NULL) {
        const object *candidate = source;
        while (candidate != NULL && candidate != common) {
            candidate = candidate->env;
        }
        if (candidate == common) {
            break;
        }
        common = common->env;
    }
    if (common == NULL) {
        return object_weight_can_add(destination, weight);
    }

    uint64_t removed = weight;
    for (const object *tmp = source; tmp != common; tmp = tmp->env) {
        if (removed > UINT32_MAX) {
            return false;
        }
        if (tmp->type == CONTAINER && !DBL_EQUAL(tmp->weapon_speed, 1.0)) {
            if (removed > tmp->damage_round_tag) {
                return false;
            }
            uint64_t unmodified = tmp->damage_round_tag - removed;
            uint64_t carrying = (long double)unmodified * tmp->weapon_speed;
            if (carrying > tmp->carrying) {
                return false;
            }
            removed = tmp->carrying - carrying;
        } else if (removed > tmp->carrying) {
            return false;
        }
    }

    uint64_t added = weight;
    for (const object *tmp = destination; tmp != common; tmp = tmp->env) {
        if (added > UINT32_MAX) {
            return false;
        }
        if (tmp->type == CONTAINER && !DBL_EQUAL(tmp->weapon_speed, 1.0)) {
            if (added > UINT32_MAX - tmp->damage_round_tag) {
                return false;
            }
            uint64_t unmodified = tmp->damage_round_tag + added;
            uint64_t carrying = (long double)unmodified * tmp->weapon_speed;
            if (carrying < tmp->carrying || carrying > UINT32_MAX) {
                return false;
            }
            added = carrying - tmp->carrying;
        } else if (added > UINT32_MAX - tmp->carrying) {
            return false;
        }
    }

    for (const object *tmp = common; tmp != NULL; tmp = tmp->env) {
        if (removed > UINT32_MAX || added > UINT32_MAX) {
            return false;
        }
        if (tmp->type == CONTAINER && !DBL_EQUAL(tmp->weapon_speed, 1.0)) {
            if (removed > tmp->damage_round_tag) {
                return false;
            }
            uint64_t interim_unmodified = tmp->damage_round_tag - removed;
            if (added > UINT32_MAX - interim_unmodified) {
                return false;
            }
            uint64_t final_unmodified = interim_unmodified + added;
            uint64_t interim_carrying = (long double)interim_unmodified * tmp->weapon_speed;
            uint64_t final_carrying = (long double)final_unmodified * tmp->weapon_speed;
            if (interim_carrying > tmp->carrying || final_carrying > UINT32_MAX) {
                return false;
            }
            removed = tmp->carrying - interim_carrying;
            added = final_carrying - interim_carrying;
        } else {
            if (removed > tmp->carrying || added > UINT32_MAX - (tmp->carrying - removed)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Recursively (outwards) subtracts a number from the weight of an object
 * (and what is carried by its environment(s)).
 *
 * @param op
 * The object.
 * @param weight
 * The weight to subtract.
 */
void object_weight_sub(object *op, uint32_t weight) {
    HARD_ASSERT(op != NULL);

    while (op != NULL) {
        if (op->type == CONTAINER && !DBL_EQUAL(op->weapon_speed, 1.0)) {
            uint32_t old_carrying = op->carrying;
            op->damage_round_tag -= weight;
            op->carrying = op->damage_round_tag * op->weapon_speed;
            weight = old_carrying - op->carrying;
        } else {
            op->carrying -= weight;
        }

        if (op->env != NULL && op->env->type == PLAYER) {
            esrv_update_item(UPD_WEIGHT, op);
        }

        op = op->env;
    }
}

/**
 * Acquire the outermost environment object for a given object.
 *
 * @param op
 * Object we want the environment of.
 * @return
 * The outermost environment object for a given object. Never NULL.
 */
object *object_get_env(object *op) {
    HARD_ASSERT(op != NULL);

    while (op->env != NULL) {
        op = op->env;
    }

    return op;
}

/**
 * Check if the specified object is somewhere inside another object's
 * inventory, regardless of the inventory nesting level.
 *
 * @param op
 * The object to check.
 * @param inv
 * Inventory the object should be in.
 * @return
 * True if the checked object is somewhere inside the specified inventory,
 * false otherwise.
 */
bool object_is_in_inventory(const object *op, const object *inv) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(inv != NULL);

    do {
        if (op->env == inv) {
            return true;
        }

        op = op->env;
    } while (op != NULL);

    return false;
}

/** Whether an object is a hidden wall-layer roof/camera surface. */
bool object_is_roof_surface(const object *op) {
    return op != NULL && op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN);
}

/**
 * Dumps an object.
 *
 * @param op
 * Object to dump. Can be NULL.
 * @param sb
 * Buffer that will contain object information. Must not be NULL.
 */
void object_dump(const object *op, StringBuffer *sb) {
    HARD_ASSERT(sb != NULL);

    if (op == NULL) {
        stringbuffer_append_string(sb, "[NULL pointer]");
        return;
    }

    if (op->arch != NULL) {
        stringbuffer_append_printf(sb,
                                   "arch %s\n",
                                   op->arch->name != NULL ? op->arch->name : "(null)");
        get_ob_diff(sb, op, &arches[ARCH_EMPTY_ARCHETYPE]->clone);
        stringbuffer_append_string(sb, "end\n");
    } else {
        stringbuffer_append_string(sb, "Object ");
        stringbuffer_append_string(sb, op->name == NULL ? "(null)" : op->name);
        stringbuffer_append_string(sb, "\nend\n");
    }
}

/**
 * Dump an object, complete with its inventory.
 *
 * @param op
 * Object to dump.
 * @param sb
 * Buffer that will contain object information.
 */
void object_dump_rec(const object *op, StringBuffer *sb) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(sb != NULL);

    /* Get the difference from the object's archetype. */
    archetype_t *at = op->arch;
    if (at == NULL) {
        /* No archetype, use empty archetype. */
        at = arches[ARCH_EMPTY_ARCHETYPE];
    }

    stringbuffer_append_printf(sb, "arch %s\n", at->name);
    get_ob_diff(sb, op, &at->clone);

    /* Recursively dump the inventory. */
    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        object_dump_rec(tmp, sb);
    }

    stringbuffer_append_string(sb, "end\n");
}

/**
 * Clear pointer to owner of an object, including ownercount.
 *
 * @param op
 * The object to clear the owner for.
 */
void object_owner_clear(object *op) {
    HARD_ASSERT(op != NULL);
    op->owner = NULL;
    op->ownercount = 0;
    op->player_attack_source = false;
}

/**
 * Sets the owner of the object 'op' to the 'owner' object.
 *
 * @param op
 * The object to set the owner for.
 * @param owner
 * The owner.
 * @see object_owner()
 */
static void object_owner_set_internal(object *op, object *owner) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(owner != NULL);

    while (owner->owner != NULL && owner != owner->owner &&
           owner->ownercount == owner->owner->count) {
        owner = owner->owner;
    }

    /* If the owner still has an owner, we did not resolve to a final owner,
     * so lets not add to that. */
    if (owner->owner != NULL) {
        return;
    }

    op->owner = owner;
    op->ownercount = owner->count;
}

/**
 * Sets the owner and snapshots a player owner's current skill. Non-player
 * ownership clears any stale skill provenance.
 *
 * @param op
 * The object.
 * @param owner
 * The owner.
 */
void object_owner_set(object *op, object *owner) {
    HARD_ASSERT(op != NULL);

    if (unlikely(owner == NULL)) {
        log_error("Called with NULL owner, object: %s", object_get_str(op));
        return;
    }

    /* Ensure we have a head. */
    owner = HEAD(owner);
    object_owner_set_internal(op, owner);

    op->chosen_skill = owner->type == PLAYER ? owner->chosen_skill : NULL;
    op->player_attack_source = owner->type == PLAYER;
}

/**
 * Copies owner from a source object to the specified object.
 *
 * Chosen skill object is set to that of the source object (typically the
 * skill that was currently chosen at the time when the source object's
 * owner was set and not the owner's current skill object).
 *
 * Use this function if a player-created effect (e.g. fire bullet, swarm
 * spell) creates further effects whose kills should be accounted for the
 * player's original skill, even if the player has changed skills in the
 * meanwhile. Direct-player attack provenance propagates only through
 * nonliving effect chains, not through player-owned monsters.
 *
 * @param op
 * The object.
 * @param src
 * The source object to copy the owner from.
 */
void object_owner_copy(object *op, object *src) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(src != NULL);

    object *owner = object_owner(src);
    if (owner == NULL) {
        /* Players don't have owners - they own themselves. Update
         * as appropriate. */
        if (src->type == PLAYER) {
            owner = src;
        } else {
            return;
        }
    }

    object_owner_set_internal(op, owner);
    op->chosen_skill = src->chosen_skill;
    op->player_attack_source = src->type == PLAYER || (!IS_LIVE(src) && src->player_attack_source);
}

/**
 * Returns the object which this object marks as being the owner.
 *
 * An ID scheme is used to avoid pointing to objects which have been
 * freed and are now reused. If this is detected, the owner is set to
 * NULL, and NULL is returned.
 *
 * @param op
 * The object to get owner for.
 * @return
 * Owner of the object if any, NULL if no owner.
 */
object *object_owner(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->owner == NULL) {
        return NULL;
    }

    if (OBJECT_FREE(op) || op->owner->count != op->ownercount) {
        op->owner = NULL;
        return NULL;
    }

    return op->owner;
}

/** Free combat participation state that must not survive object reuse/copy. */
static void object_combat_contributions_free(object *op) {
    combat_contribution_t *contribution = op->combat_contributions;
    while (contribution != NULL) {
        combat_contribution_t *next = contribution->next;
        free(contribution);
        contribution = next;
    }

    op->combat_contributions = NULL;
}

/**
 * Copy object first frees everything allocated by the second object,
 * and then copies the contents of the first object into the second
 * object, allocating what needs to be allocated.
 *
 * @param op
 * Object to copy to.
 * @param src
 * Object to copy from.
 * @param no_speed
 * If set, do not touch the active list.
 */
void object_copy(object *op, const object *src, bool no_speed) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(src != NULL);

    bool is_removed = QUERY_FLAG(op, FLAG_REMOVED);

    object_combat_contributions_free(op);

    FREE_ONLY_HASH(op->name);
    FREE_ONLY_HASH(op->name_pl);
    FREE_ONLY_HASH(op->title);
    FREE_ONLY_HASH(op->race);
    FREE_ONLY_HASH(op->slaying);
    FREE_ONLY_HASH(op->msg);
    FREE_ONLY_HASH(op->artifact);
    FREE_ONLY_HASH(op->custom_name);
    FREE_ONLY_HASH(op->glow);
    FREE_ONLY_HASH(op->custody_lineage);
    FREE_ONLY_HASH(op->custody_provenance);
    FREE_ONLY_HASH(op->custody_first);
    FREE_ONLY_HASH(op->custody_last);
    FREE_ONLY_HASH(op->custody_actor);

    object_free_key_values(op);
    op->exit_cache_entry = NULL;

    memcpy((char *)op + offsetof(object, name),
           (const char *)src + offsetof(object, name),
           sizeof(object) - offsetof(object, name));

    if (is_removed) {
        SET_FLAG(op, FLAG_REMOVED);
    }

    ADD_REF_NOT_NULL_HASH(op->name);
    ADD_REF_NOT_NULL_HASH(op->name_pl);
    ADD_REF_NOT_NULL_HASH(op->title);
    ADD_REF_NOT_NULL_HASH(op->race);
    ADD_REF_NOT_NULL_HASH(op->slaying);
    ADD_REF_NOT_NULL_HASH(op->msg);
    ADD_REF_NOT_NULL_HASH(op->artifact);
    ADD_REF_NOT_NULL_HASH(op->custom_name);
    ADD_REF_NOT_NULL_HASH(op->glow);
    ADD_REF_NOT_NULL_HASH(op->custody_lineage);
    ADD_REF_NOT_NULL_HASH(op->custody_provenance);
    ADD_REF_NOT_NULL_HASH(op->custody_first);
    ADD_REF_NOT_NULL_HASH(op->custody_last);
    ADD_REF_NOT_NULL_HASH(op->custody_actor);

    /* Only alter speed_left when we are sure that we have not done it before */
    if (!no_speed && op->speed < 0.0 && DBL_EQUAL(op->speed_left, op->arch->clone.speed_left)) {
        op->speed_left += rndm(0, 90) / 100.0f;
    }

    /* Copy over key_values, if any. */
    if (src->key_values != NULL) {
        op->key_values = NULL;

        for (key_value_t *link = src->key_values, *tail = NULL; link != NULL; link = link->next) {
            key_value_t *new_link = xmalloc(sizeof(*new_link));

            new_link->next = NULL;
            new_link->key = add_refcount(link->key);

            if (link->value != NULL) {
                new_link->value = add_refcount(link->value);
            } else {
                new_link->value = NULL;
            }

            /* Link it up. */
            if (op->key_values == NULL) {
                op->key_values = new_link;
                tail = new_link;
            } else {
                tail->next = new_link;
                tail = new_link;
            }
        }
    }

    if (!no_speed) {
        object_update_speed(op);
    }
}

/**
 * Completely copy an object, duplicating the inventory too.
 *
 * @param op
 * Where to copy.
 * @param src
 * Object to copy.
 */
void object_copy_full(object *op, const object *src) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(src != NULL);

    object_copy(op, src, false);

    for (object *tmp = src->inv; tmp != NULL; tmp = tmp->below) {
        object *clone = object_get();
        object_copy_full(clone, tmp);
        object_insert_into(clone, op, 0);
    }
}

/**
 * Grabs an object from the list of unused objects, makes sure it is
 * initialized, and returns it.
 *
 * If there are no free objects, expand_objects() is called to get more.
 * @return
 * The new object.
 */
object *object_get(void) {

    object *new_obj = mempool_get(pool_object);
    SET_FLAG(new_obj, FLAG_REMOVED);
    new_obj->light_color = LIGHT_COLOR_WHITE;

    static New_Face *blank_face = NULL;
    if (blank_face == NULL) {
        blank_face = &new_faces[find_face(BLANK_FACE_NAME, 0)];
    }
    new_obj->face = blank_face;

    static tag_t count = 0;
    /* Give the object a new (unique) count tag. */
    new_obj->count = ++count;

    return new_obj;
}

/**
 * If an object with the FLAG_IS_TURNABLE flag needs to be updated due to
 * a direction change, this function can be called to update the face
 * variable.
 *
 * @param op
 * The object to update.
 */
void object_update_turnable(object *op) {
    HARD_ASSERT(op != NULL);

    if (!QUERY_FLAG(op, FLAG_IS_TURNABLE)) {
        return;
    }

    SET_ANIMATION(op, (NUM_ANIMATIONS(op) / NUM_FACINGS(op)) * op->direction);
    object_update(op, UP_OBJ_FACE);
}

/**
 * Updates the speed of an object. If the speed changes from 0 to another
 * value, or vice versa, then add/remove the object from the active list.
 *
 * This function needs to be called whenever the speed of an object changes.
 *
 * @param op
 * The object.
 */
void object_update_speed(object *op) {
    HARD_ASSERT(op != NULL);

    if (OBJECT_FREE(op) && DBL_EQUAL(op->speed, 0.0)) {
        LOG(ERROR, "Object %s is freed but has speed.", object_get_str(op));
        op->speed = 0.0;
    }

    /* No reason putting the archetypes objects on the speed list,
     * since they never really need to be updated. */
    if (arch_in_init) {
        return;
    }

    /* Spawn point templates can have speed, but must never be added to the
     * active list. Still let the removal path below unlink a template that
     * was already active before its type changed. */
    if (op->type != SPAWN_POINT_MOB && FABS(op->speed) > MIN_ACTIVE_SPEED) {
        /* If already on active list, don't do anything */
        if (op->active_next || op->active_prev || op == active_objects) {
            return;
        }

        /* process_events() expects us to insert the object at the beginning
         * of the list. */
        op->active_next = active_objects;

        if (op->active_next != NULL) {
            op->active_next->active_prev = op;
        }

        active_objects = op;
        op->active_prev = NULL;
    } else {
        /* If not on the active list, nothing needs to be done. */
        if (op->active_next == NULL && op->active_prev == NULL && op != active_objects) {
            return;
        }

        if (op->active_prev == NULL) {
            active_objects = op->active_next;

            if (op->active_next != NULL) {
                op->active_next->active_prev = NULL;
            }
        } else {
            op->active_prev->active_next = op->active_next;

            if (op->active_next != NULL) {
                op->active_next->active_prev = op->active_prev;
            }
        }

        op->active_next = NULL;
        op->active_prev = NULL;
    }
}

/**
 * Updates the various map square flags and values depending on 'action'.
 *
 * @param op
 * Object to update.
 * @param action
 * Hint of what the caller believes need to be done. One of
 * @ref UP_OBJ_xxx values.
 */
void object_update(object *op, int action) {
    HARD_ASSERT(op != NULL);

    if (op->env != NULL || op->map == NULL || op->map->in_memory == MAP_SAVING) {
        return;
    }

    /* No need to change anything except the map update counter. */
    if (action == UP_OBJ_FACE) {
        INC_MAP_UPDATE_COUNTER(op->map, op->x, op->y);
        return;
    }

    MapSpace *msp = GET_MAP_SPACE_PTR(op->map, op->x, op->y);
    int newflags = msp->flags;
    int flags = newflags;

    if (action == UP_OBJ_INSERT) {
        msp->update_tile++;

        if (op->glow_radius != 0) {
            adjust_light_source_color(op->map, op->x, op->y, op->glow_radius, op->light_color, 1);
        }

        if (QUERY_FLAG(op, FLAG_NO_PASS) || QUERY_FLAG(op, FLAG_PASS_THRU) ||
            QUERY_FLAG(op, FLAG_DOOR_CLOSED)) {
            newflags |= P_NEED_UPDATE;
        } else if (QUERY_FLAG(op, FLAG_IS_FLOOR)) {
            /* Floors define our node - force an update. */
            newflags |= P_NEED_UPDATE;
            msp->light_value += op->last_sp;
        } else {
            if (op->type == CHECK_INV) {
                newflags |= P_CHECK_INV;
            }

            if (QUERY_FLAG(op, FLAG_MONSTER)) {
                newflags |= P_IS_MONSTER;
            }

            if (QUERY_FLAG(op, FLAG_IS_PLAYER)) {
                newflags |= P_IS_PLAYER;
            }

            if (QUERY_FLAG(op, FLAG_PLAYER_ONLY)) {
                newflags |= P_PLAYER_ONLY;
            }

            if (QUERY_FLAG(op, FLAG_BLOCKSVIEW)) {
                newflags |= P_BLOCKSVIEW;
            }

            if (QUERY_FLAG(op, FLAG_NO_MAGIC)) {
                newflags |= P_NO_MAGIC;
            }

            if (QUERY_FLAG(op, FLAG_WALK_ON)) {
                newflags |= P_WALK_ON;
            }

            if (QUERY_FLAG(op, FLAG_FLY_ON)) {
                newflags |= P_FLY_ON;
            }

            if (QUERY_FLAG(op, FLAG_WALK_OFF)) {
                newflags |= P_WALK_OFF;
            }

            if (QUERY_FLAG(op, FLAG_FLY_OFF)) {
                newflags |= P_FLY_OFF;
            }

            if (QUERY_FLAG(op, FLAG_DOOR_CLOSED)) {
                newflags |= P_DOOR_CLOSED;
            }

            if (QUERY_FLAG(op, FLAG_NO_PVP)) {
                newflags |= P_NO_PVP;
            }

            if (op->type == MAGIC_MIRROR) {
                newflags |= P_MAGIC_MIRROR;
            }

            if (op->type == EXIT) {
                newflags |= P_IS_EXIT;
            }

            if (QUERY_FLAG(op, FLAG_OUTDOOR)) {
                newflags |= P_OUTDOOR;
            }
        }
    } else if (action == UP_OBJ_REMOVE) {
        msp->update_tile++;

        if (op->glow_radius != 0) {
            adjust_light_source_color(op->map, op->x, op->y, op->glow_radius, op->light_color, -1);
        }

        /* We must rebuild the flags when one of these flags is touched from our
         * object */
        if (QUERY_FLAG(op, FLAG_MONSTER) || QUERY_FLAG(op, FLAG_IS_PLAYER) ||
            QUERY_FLAG(op, FLAG_BLOCKSVIEW) || QUERY_FLAG(op, FLAG_DOOR_CLOSED) ||
            QUERY_FLAG(op, FLAG_PASS_THRU) || QUERY_FLAG(op, FLAG_NO_PASS) ||
            QUERY_FLAG(op, FLAG_PLAYER_ONLY) || QUERY_FLAG(op, FLAG_NO_MAGIC) ||
            QUERY_FLAG(op, FLAG_WALK_ON) || QUERY_FLAG(op, FLAG_FLY_ON) ||
            QUERY_FLAG(op, FLAG_WALK_OFF) || QUERY_FLAG(op, FLAG_FLY_OFF) ||
            QUERY_FLAG(op, FLAG_IS_FLOOR) || QUERY_FLAG(op, FLAG_OUTDOOR) ||
            QUERY_FLAG(op, FLAG_NO_PVP) || op->type == CHECK_INV || op->type == MAGIC_MIRROR ||
            op->type == EXIT) {
            newflags |= P_NEED_UPDATE;
        }
    } else if (action == UP_OBJ_FLAGS) {
        /* Force flags rebuild but no tile counter. */
        newflags |= P_NEED_UPDATE;
    } else if (action == UP_OBJ_FLAGFACE) {
        /* Force flags rebuild */
        newflags |= P_NEED_UPDATE;
        msp->update_tile++;
    } else if (action == UP_OBJ_ALL) {
        /* Force full tile update */
        newflags |= P_NEED_UPDATE;
        msp->update_tile++;
    } else {
        return;
    }

    if (flags != newflags) {
        /* Rebuild flags */
        if (newflags & P_NEED_UPDATE) {
            msp->flags |= newflags;
            update_position(op->map, op->x, op->y);
        } else {
            msp->flags |= newflags;
        }
    }

    if (op->map->in_memory == MAP_IN_MEMORY &&
        (QUERY_FLAG(op, FLAG_BLOCKSVIEW) || QUERY_FLAG(op, FLAG_IS_FLOOR))) {
        recalculate_light_sources(op->map);
        celestial_light_invalidate(op->map);
    }

    exit_destination_cache_object_changed(op, action);

    if (op->more != NULL && action != UP_OBJ_INSERT) {
        object_update(op->more, action);
    }
}

/**
 * Drops the inventory of the specified object into its current environment.
 *
 * Makes some decisions whether to actually drop or not, and/or to
 * create a corpse for the stuff.
 *
 * @param op
 * The object to drop the inventory for.
 */
void object_drop_inventory(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->type == PLAYER) {
        return;
    }

    if (op->env == NULL && (op->map == NULL || op->map->in_memory != MAP_IN_MEMORY)) {
        return;
    }

    object *enemy;
    if (op->enemy != NULL && op->enemy->type == PLAYER) {
        enemy = op->enemy;
    } else {
        enemy = object_owner(op->enemy);
    }

    object *corpse = NULL;
    /* Create race corpse and/or drop stuff to floor. */
    if ((QUERY_FLAG(op, FLAG_CORPSE) && !QUERY_FLAG(op, FLAG_STARTEQUIP)) ||
        QUERY_FLAG(op, FLAG_CORPSE_FORCED)) {
        ob_race *race_corpse = race_find(op->race);
        if (race_corpse != NULL) {
            corpse = arch_to_object(race_corpse->corpse);
            corpse->x = op->x;
            corpse->y = op->y;
            corpse->map = op->map;
            corpse->weight = op->weight;
        }
    }

    FOR_INV_PREPARE(op, tmp) {
        object_remove(tmp, 0);

        if (tmp->type == QUEST_CONTAINER) {
            if (enemy != NULL && enemy->type == PLAYER && enemy->count == op->enemy_count) {
                quest_handle(enemy, tmp);
            }

            object_destroy(tmp);
            continue;
        }

        if ((QUERY_FLAG(op, FLAG_STARTEQUIP) &&
             !(tmp->type == ARROW && tmp->attacked_by_count != 0)) ||
            (tmp->type != RUNE &&
             (QUERY_FLAG(tmp, FLAG_SYS_OBJECT) || QUERY_FLAG(tmp, FLAG_STARTEQUIP) ||
              QUERY_FLAG(tmp, FLAG_NO_DROP)))) {
            object_destroy(tmp);
            continue;
        }

        tmp->x = op->x;
        tmp->y = op->y;

        /* Always clear these in case the monster used the item */
        CLEAR_FLAG(tmp, FLAG_APPLIED);
        CLEAR_FLAG(tmp, FLAG_BEEN_APPLIED);

        /* If we have a corpse put the item in it. */
        if (corpse != NULL &&
            !(tmp->type == ARROW && tmp->attacked_by_count != 0 && enemy != NULL &&
              OBJECT_VALID(tmp->attacked_by, tmp->attacked_by_count) &&
              tmp->attacked_by_count != enemy->count &&
              !(tmp->attacked_by->type == PLAYER && enemy->type == PLAYER &&
                CONTR(tmp->attacked_by)->party != NULL &&
                CONTR(tmp->attacked_by)->party == CONTR(enemy)->party))) {
            object_insert_into(tmp, corpse, 0);
        } else if (tmp->type != RUNE) {
            if (op->env != NULL) {
                object_insert_into(tmp, op->env, 0);
            } else {
                object_insert_map(tmp, op->map, NULL, 0);
            }
        } else {
            object_destroy(tmp);
        }
    }
    FOR_INV_FINISH();

    /* Drop the corpse. */
    if (corpse != NULL) {
        if (enemy != NULL && enemy->type == PLAYER) {
            if (enemy->count == op->enemy_count) {
                FREE_AND_ADD_REF_HASH(corpse->slaying, enemy->name);
            }
        } else if (QUERY_FLAG(op, FLAG_CORPSE_FORCED)) {
            corpse->stats.food = 5;
        }

        /* Change sub_type to mark this corpse. */
        if (corpse->slaying != NULL) {
            if (CONTR(enemy)->party != NULL && CONTR(enemy)->party->loot != PARTY_LOOT_OWNER) {
                FREE_AND_ADD_REF_HASH(corpse->slaying, CONTR(enemy)->party->name);
                corpse->sub_type = ST1_CONTAINER_CORPSE_party;
            } else {
                corpse->sub_type = ST1_CONTAINER_CORPSE_player;
            }
        }

        /* Store the original food value. */
        corpse->last_eat = corpse->stats.food;
        corpse->sub_layer = op->sub_layer;

        if (op->env != NULL) {
            corpse = object_insert_into(corpse, op->env, 0);
        } else {
            corpse = object_insert_map(corpse, op->map, NULL, 0);
        }

        SOFT_ASSERT(corpse != NULL, "Failed to insert corpse for %s", object_get_str(op));
        object_reverse_inventory(corpse);
    }
}

/**
 * Destroy (free) inventory of an object. Used internally by object_destroy()
 * to recursively free the object's inventory.
 *
 * @param op
 * Object to free the inventory of.
 */
void object_destroy_inv(object *op) {
    HARD_ASSERT(op != NULL);

    SET_FLAG(op, FLAG_NO_FIX_PLAYER);

    FOR_INV_PREPARE(op, tmp) {
        if (tmp->inv != NULL) {
            object_destroy_inv(tmp);
        }

        object_remove(tmp, 0);
        object_destroy(tmp);
    }
    FOR_INV_FINISH();

    CLEAR_FLAG(op, FLAG_NO_FIX_PLAYER);
}

/**
 * Cleanups and frees everything allocated by an object and gives the
 * memory back to the object mempool.
 *
 * @note The object must have been removed by object_remove() first.
 * @param op
 * The object to destroy (free).
 */
void object_destroy(object *op) {
    HARD_ASSERT(op != NULL);

    bool was_spawn_point_mob = op->type == SPAWN_POINT_MOB;

    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        char buf[HUGE_BUF];

        object_debugger(op, VS(buf));
        LOG(ERROR, "Freeing an object that was not removed: %s", buf);
        return;
    }

    if (op->more != NULL) {
        object_destroy(op->more);
    }

    object_free_key_values(op);
    object_combat_contributions_free(op);

    if (QUERY_FLAG(op, FLAG_IS_LINKED)) {
        connection_object_remove(op);
    }

    /* Remove and free the inventory. */
    object_destroy_inv(op);

    /* Remove object from the active list. */
    op->speed = 0.0;
    object_update_speed(op);

    /* A spawn-point template linked before its type changed must not reach
     * the pool with any active-list linkage of its own still intact. */
    SOFT_ASSERT(!was_spawn_point_mob ||
                    (op != active_objects && op->active_next == NULL && op->active_prev == NULL),
                "Destroyed spawn-point template remains linked to the active object list: %s",
                object_get_str(op));

    if (op->head == NULL) {
        object_cb_deinit(op);
    }

    /* Free attached attrsets */
    if (op->custom_attrset) {
        switch (op->type) {
            case PLAYER:
                /* Players are changed into DEAD_OBJECTs when they logout */
            case DEAD_OBJECT:
                mempool_return(pool_player, op->custom_attrset);
                break;

            case MONSTER:
                monster_data_deinit(op);
                break;

            default:
                LOG(ERROR,
                    "Custom attrset found in unsupported object %s "
                    "(type %d)",
                    object_get_str(op),
                    op->type);
        }

        op->custom_attrset = NULL;
    }

    FREE_AND_CLEAR_HASH2(op->name);
    FREE_AND_CLEAR_HASH2(op->name_pl);
    FREE_AND_CLEAR_HASH2(op->title);
    FREE_AND_CLEAR_HASH2(op->race);
    FREE_AND_CLEAR_HASH2(op->slaying);
    FREE_AND_CLEAR_HASH2(op->msg);
    FREE_AND_CLEAR_HASH2(op->artifact);
    FREE_AND_CLEAR_HASH2(op->custom_name);
    FREE_AND_CLEAR_HASH2(op->glow);
    FREE_AND_CLEAR_HASH2(op->custody_lineage);
    FREE_AND_CLEAR_HASH2(op->custody_provenance);
    FREE_AND_CLEAR_HASH2(op->custody_first);
    FREE_AND_CLEAR_HASH2(op->custody_last);
    FREE_AND_CLEAR_HASH2(op->custody_actor);

    /* Mark object as freed and invalidate all references to it. */
    op->count = 0;

    /* Return the memory to the mempool. */
    mempool_return(pool_object, op);
}

/**
 * Drop op's inventory on the floor and remove op from the map.
 *
 * Used mainly for physical destruction of normal objects and monsters.
 *
 * @param op
 * Object to destruct.
 */
void object_destruct(object *op) {
    SET_FLAG(op, FLAG_NO_FIX_PLAYER);

    if (op->inv != NULL) {
        object_drop_inventory(op);
    }

    object_remove(op, 0);
    object_destroy(op);
}

/**
 * Checks if any objects has a movement type that matches objects that affect
 * this object on this space. Calls object_move_on() to process these events.
 *
 * @param op
 * Object that may trigger something.
 * @param originator
 * Player, monster or other object that caused 'op' to trigger the event.
 * @param state
 * 1 for move on events, 0 for move off events.
 * @return
 * True if 'op' was destroyed, false otherwise.
 */
static int object_check_move_on(object *op, object *originator, int state) {
    HARD_ASSERT(op != NULL);

    if (QUERY_FLAG(op, FLAG_NO_APPLY)) {
        return false;
    }

    MapSpace *msp = GET_MAP_SPACE_PTR(op->map, op->x, op->y);
    /* No event on this tile. */
    if (!(msp->flags & (state == 1 ? (P_WALK_ON | P_FLY_ON) : (P_WALK_OFF | P_FLY_OFF)))) {
        return false;
    }

    mapstruct *m = op->map;
    int x = op->x;
    int y = op->y;

    OBJECTS_DESTROYED_BEGIN(op) {
        FOR_MAP_PREPARE(op->map, op->x, op->y, tmp) {
            if (tmp == op) {
                continue;
            }

            if (state == 1 && IS_LIVE(op) && (op->type != PLAYER || !CONTR(op)->tcl) &&
                QUERY_FLAG(tmp, FLAG_SLOW_MOVE) &&
                (tmp->terrain_flag == 0 || tmp->terrain_flag & op->terrain_flag)) {
                op->speed_left -= SLOW_PENALTY(tmp) * FABS(op->speed);
            }

            int flag;
            if (QUERY_FLAG(op, FLAG_FLYING)) {
                flag = state == 1 ? FLAG_FLY_ON : FLAG_FLY_OFF;
            } else {
                flag = state == 1 ? FLAG_WALK_ON : FLAG_WALK_OFF;
            }

            if (!QUERY_FLAG(tmp, flag)) {
                continue;
            }

            object_move_on(tmp, op, originator, state);

            if (OBJECTS_DESTROYED(op)) {
                return true;
            }

            if (op->map != m || op->x != x || op->y != y) {
                return false;
            }
        }
        FOR_MAP_FINISH();
    }
    OBJECTS_DESTROYED_END();

    return false;
}

/**
 * This function removes the object op from the linked list of objects
 * which it is currently tied to. When this function is done, the
 * object will have no environment. If the object previously had an
 * environment, the map pointer and x/y coordinates will be updated to
 * the previous environment.
 *
 * @note If you want to remove a lot of items in player's inventory,
 * set FLAG_NO_FIX_PLAYER on the player first and then explicitly call
 * living_update() on the player.
 *
 * @param op
 * Object to remove.
 * @param flags
 * Combination of @ref REMOVAL_xxx.
 */
void object_remove(object *op, int flags) {
    HARD_ASSERT(op != NULL);

    if (QUERY_FLAG(op, FLAG_REMOVED)) {
        log_error("Tried to remove an already removed object %s.", object_get_str(op));
        return;
    }

    if (op->more != NULL) {
        object_remove(op->more, flags);
    }

    SET_FLAG(op, FLAG_REMOVED);
    SET_FLAG(op, FLAG_OBJECT_WAS_MOVED);
    op->quickslot = 0;

    /* In this case, the object to be removed is in someone's inventory. */
    if (op->env != NULL) {
        object *container = op->env;
        if (!QUERY_FLAG(op, FLAG_SYS_OBJECT) && !(flags & REMOVE_NO_WEIGHT)) {
            object_weight_sub(op->env, WEIGHT_NROF(op, op->nrof));
        }

        object *env = object_get_env(op);

        if (op->above != NULL) {
            op->above->below = op->below;
        } else {
            op->env->inv = op->below;
        }

        if (op->below != NULL) {
            op->below->above = op->above;
        }

        /* We set up values so that it could be inserted into the map,
         * but we don't actually do that - it is up to the caller to
         * decide what we want to do. */
        op->x = op->env->x;
        op->y = op->env->y;
        op->map = op->env->map;

        esrv_del_item(op);
        object_cb_remove_inv(op);

        if (container->type == EXIT) {
            exit_destination_cache_refresh(container);
            if (container->map != NULL) {
                exit_destination_cache_map_changed(container->map);
            }
        }

        op->above = NULL;
        op->below = NULL;
        op->env = NULL;

        if (env != op && IS_LIVE(env) && env->map != NULL) {
            living_update(env);
        }
    } else if (op->map != NULL) {
        /* If this is the base layer object, we assign the next object
         * to be it if it is from same layer and sub-layer. */
        MapSpace *msp = GET_MAP_SPACE_PTR(op->map, op->x, op->y);

        if (op->layer != 0 && GET_MAP_SPACE_LAYER(msp, op->layer, op->sub_layer) == op) {
            if (op->above != NULL && op->above->layer == op->layer &&
                op->above->sub_layer == op->sub_layer) {
                SET_MAP_SPACE_LAYER(msp, op->layer, op->sub_layer, op->above);
            } else {
                SET_MAP_SPACE_LAYER(msp, op->layer, op->sub_layer, NULL);
            }
        }

        /* Link the object above us. */
        if (op->above != NULL) {
            op->above->below = op->below;
        } else {
            /* Assign below as last one. */
            SET_MAP_SPACE_LAST(msp, op->below);
        }

        /* Relink the object below us, if there is one. */
        if (op->below != NULL) {
            op->below->above = op->above;
        } else {
            /* First object goes on above it. */
            SET_MAP_SPACE_FIRST(msp, op->above);
        }

        op->above = NULL;
        op->below = NULL;
        op->env = NULL;

        if (op->map->in_memory != MAP_SAVING) {
            msp->update_tile++;
            object_update(op, UP_OBJ_REMOVE);
        }

        object_cb_remove_map(op);

        if (op->type == EXIT) {
            exit_destination_cache_object_changed(op, UP_OBJ_REMOVE);
        }

        if (!(flags & REMOVE_NO_WALK_OFF)) {
            object_check_move_on(op, NULL, 0);
        }
    }
}

/**
 * This function inserts the object in the two-way linked list which
 * represents what is on a map.
 *
 * @param op
 * Object to insert.
 * @param m
 * Map to insert into.
 * @param originator
 * What caused op to be inserted.
 * @param flag
 * Combination of @ref INS_xxx "INS_xxx" values.
 * @return
 * NULL if 'op' was destroyed, 'op' otherwise.
 */
object *object_insert_map(object *op, mapstruct *m, object *originator, int flag) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(m != NULL);

    if (OBJECT_FREE(op)) {
        log_error("Attempted to insert freed object: %s", object_get_str(op));
        return NULL;
    }

    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        log_error("Attempted to insert non-removed object: %s", object_get_str(op));
        return op;
    }

    if (op->head == NULL && op->arch->more != NULL && op->more == NULL) {
        object *prev = op;
        for (archetype_t *at = op->arch->more; at != NULL; at = at->more) {
            object *tail = arch_to_object(at);

            tail->type = op->type;
            tail->layer = op->layer;
            tail->sub_layer = op->sub_layer;

            tail->head = op;
            prev->more = tail;

            prev = tail;
        }

        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_SYS_OBJECT);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_NO_APPLY);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_IS_INVISIBLE);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_IS_ETHEREAL);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_CAN_PASS_THRU);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_FLYING);
        SET_OR_CLEAR_MULTI_FLAG(op, FLAG_BLOCKSVIEW);

        SET_OR_CLEAR_MULTI_FLAG_IF_CLONE(op, FLAG_NO_PASS);
        SET_OR_CLEAR_MULTI_FLAG_IF_CLONE(op, FLAG_BLOCKSVIEW);
    }

    /* Attempt to fall down through empty map squares onto maps below, but
     * only if the object is not a player or the player doesn't have collision
     * disabled. */
    int fall_floors = 0;
    if (flag & INS_FALL_THROUGH && (op->type != PLAYER || !CONTR(op)->tcl)) {
        mapstruct *tiled;
        bool found_floor;
        int sub_layer;
        for (tiled = m; tiled != NULL; tiled = get_map_from_tiled(tiled, TILED_DOWN)) {
            object *floor = GET_MAP_OB_LAYER(tiled, op->x, op->y, LAYER_FLOOR, op->sub_layer);
            int z = floor != NULL ? floor->z : 0;
            int z_highest = 0;
            sub_layer = -1;
            found_floor = false;

            if (tiled != m) {
                fall_floors++;
            }

            object *floor_tmp;
            FOR_MAP_LAYER_BEGIN(tiled, op->x, op->y, LAYER_FLOOR, -1, floor_tmp) {
                found_floor = true;

                if (tiled == m) {
                    continue;
                }

                if (floor_tmp->z - z > MOVE_MAX_HEIGHT_DIFF) {
                    continue;
                }

                if (floor_tmp->z > z_highest) {
                    z_highest = floor_tmp->z;
                    sub_layer = floor_tmp->sub_layer;
                }
            }
            FOR_MAP_LAYER_END

            if (found_floor || QUERY_FLAG(op, FLAG_FLYING)) {
                break;
            }
        }

        if (found_floor) {
            if (fall_floors != 0 && object_blocked(op, tiled, op->x, op->y)) {
                int i = map_free_spot_first(tiled, op->x, op->y, op->arch, op);
                if (i != -1) {
                    op->x += freearr_x[i];
                    op->y += freearr_y[i];
                }
            }

            if (sub_layer != -1) {
                op->sub_layer = sub_layer;
            }

            m = tiled;
        }

        flag &= ~INS_FALL_THROUGH;
    }

    if (op->more != NULL) {
        op->more->x = HEAD(op)->x + op->more->arch->clone.x;
        op->more->y = HEAD(op)->y + op->more->arch->clone.y;
        op->more->sub_layer = HEAD(op)->sub_layer;

        if (object_insert_map(op->more, m, originator, flag) == NULL) {
            return NULL;
        }
    }

    CLEAR_FLAG(op, FLAG_REMOVED);

    int x = op->x;
    int y = op->y;
    m = get_map_from_coord(m, &x, &y);
    if (m == NULL) {
        return NULL;
    }

    op->x = x;
    op->y = y;
    op->map = m;

    /* Merge objects if possible. */
    if (op->nrof != 0 && !(flag & INS_NO_MERGE)) {
        for (object *tmp = GET_MAP_OB(m, x, y); tmp != NULL; tmp = tmp->above) {
            if (object_can_merge(op, tmp)) {
                if (!object_custody_provenance_merge(op, tmp)) {
                    continue;
                }
                op->nrof += tmp->nrof;
                object_remove(tmp, 0);
                object_destroy(tmp);
                break;
            }
        }
    }

    SET_FLAG(op, FLAG_OBJECT_WAS_MOVED);
    CLEAR_FLAG(op, FLAG_APPLIED);
    CLEAR_FLAG(op, FLAG_INV_LOCKED);

    MapSpace *msp = GET_MAP_SPACE_PTR(op->map, op->x, op->y);

    if (op->layer != 0) {
        object *top = GET_MAP_SPACE_LAYER(msp, op->layer, op->sub_layer);
        if (top == NULL) {
            for (int layer = op->layer; layer <= NUM_LAYERS && top == NULL; layer++) {
                for (int sub_layer = op->sub_layer; sub_layer < NUM_SUB_LAYERS && top == NULL;
                     sub_layer++) {
                    top = GET_MAP_SPACE_LAYER(msp, layer, sub_layer);
                }
            }
        }

        SET_MAP_SPACE_LAYER(msp, op->layer, op->sub_layer, op);

        if (top != NULL) {
            if (top->below != NULL) {
                top->below->above = op;
            } else {
                SET_MAP_SPACE_FIRST(msp, op);
            }

            op->below = top->below;
            top->below = op;
            op->above = top;
        } else {
            top = GET_MAP_SPACE_LAST(msp);
            if (top != NULL) {
                top->above = op;
                op->below = top;
            } else {
                SET_MAP_SPACE_FIRST(msp, op);
            }

            SET_MAP_SPACE_LAST(msp, op);
        }
    } else {
        /* Easy chaining */
        object *top = GET_MAP_SPACE_FIRST(msp);
        if (top != NULL) {
            top->below = op;
            op->above = top;
        } else {
            SET_MAP_SPACE_LAST(msp, op);
        }

        SET_MAP_SPACE_FIRST(msp, op);
    }

    /* Some object-type-specific adjustments/initialization. */
    if (op->type == PLAYER) {
        CONTR(op)->cs->update_tile = 0;
        CONTR(op)->update_los = 1;

        if (op->map->player_first != NULL) {
            CONTR(op->map->player_first)->map_below = op;
            CONTR(op)->map_above = op->map->player_first;
        }

        op->map->player_first = op;
    } else if (op->type == MAP_EVENT_OBJ) {
        map_event_obj_init(op);
    } else {
        object_cb_insert_map(op);
    }

    /* Mark this tile as changed. */
    msp->update_tile++;
    /* Update flags for this tile. */
    object_update(op, UP_OBJ_INSERT);

    /* Attempt to open doors. */
    door_try_open(op, op->map, op->x, op->y, false);

    if (!(flag & INS_NO_WALK_ON) && (msp->flags & (P_WALK_ON | P_FLY_ON) || op->more != NULL) &&
        op->head == NULL) {
        for (object *tmp = op; tmp != NULL; tmp = tmp->more) {
            if (object_check_move_on(tmp, originator, 1)) {
                return NULL;
            }
        }
    }

    if (fall_floors != 0 && IS_LIVE(op)) {
        OBJECTS_DESTROYED_BEGIN(op) {
            attack_perform_fall(op, fall_floors);

            if (OBJECTS_DESTROYED(op)) {
                return NULL;
            }
        }
        OBJECTS_DESTROYED_END();
    }

    return op;
}

/**
 * Split a stack of objects into another object with the specified quantity.
 *
 * If 'nrof' is more or equal to the nrof of the specified object, the
 * original object is returned instead and no extra work is done.
 *
 * @param op
 * Object to split.
 * @param nrof
 * Number of items to split from the stack.
 * @return
 * Split part of the stack. Can be the original object; never NULL.
 */
object *object_stack_get(object *op, uint32_t nrof) {
    HARD_ASSERT(op != NULL);

    nrof = MAX(1, nrof);

    if (MAX(1, op->nrof) <= nrof) {
        return op;
    }

    object *split = object_get();
    object_copy_full(split, op);
    if (!object_custody_provenance_split(op, split, nrof)) {
        object_destroy(split);
        return op;
    }

    op->nrof -= nrof;
    esrv_update_item(UPD_NAME | UPD_NROF, op);

    if (op->env != NULL && !QUERY_FLAG(op, FLAG_SYS_OBJECT)) {
        object_weight_sub(op->env, WEIGHT_NROF(op, nrof));
    }

    split->nrof = nrof;
    return split;
}

/**
 * Like object_stack_get(), but if a new object is created due to the split,
 * it is inserted in the same environment as the original object.
 *
 * @param op
 * Object to split.
 * @param nrof
 * Number of items to split from the stack.
 * @return
 * Split part of the stack. Can be the original object; never NULL.
 */
object *object_stack_get_reinsert(object *op, uint32_t nrof) {
    HARD_ASSERT(op != NULL);

    object *split = object_stack_get(op, nrof);
    if (split != op) {
        if (op->map != NULL) {
            split = object_insert_map(split, op->map, NULL, INS_NO_MERGE);
        } else if (op->env != NULL) {
            split = object_insert_into(split, op->env, INS_NO_MERGE);
        }

        if (split == NULL) {
            return op;
        }
    }

    return split;
}

/**
 * Like object_stack_get(), but if the original object is returned (no new
 * stack is created), it is also removed from its environment.
 *
 * @param op
 * Object to split.
 * @param nrof
 * Number of items to split from the stack.
 * @return
 * Split part of the stack. Can be the original object; never NULL.
 */
object *object_stack_get_removed(object *op, uint32_t nrof) {
    HARD_ASSERT(op != NULL);

    object *split = object_stack_get(op, nrof);
    if (split == op) {
        object_remove(split, 0);
    }

    return split;
}

/**
 * Decreases a specified number from the amount of an object. If the amount
 * reaches 0, the object is subsequently removed and freed.
 *
 * This function will send an update to client if op is in a player
 * inventory.
 *
 * @param op
 * Object to decrease.
 * @param nrof
 * Number to remove.
 * @return
 * 'op' if something is left, NULL if the amount reached 0.
 */
object *object_decrease(object *op, uint32_t nrof) {
    HARD_ASSERT(op != NULL);

    if (nrof == 0) {
        return op;
    }

    if (nrof > op->nrof) {
        nrof = op->nrof;
    }

    if (nrof < op->nrof && !object_custody_provenance_remove(op, nrof)) {
        return op;
    }

    if (QUERY_FLAG(op, FLAG_REMOVED)) {
        op->nrof -= nrof;
    } else {
        if (nrof < op->nrof) {
            op->nrof -= nrof;

            if (op->env != NULL && !QUERY_FLAG(op, FLAG_SYS_OBJECT)) {
                object_weight_sub(op->env, op->weight * nrof);
            }
        } else {
            object_remove(op, 0);
            op->nrof = 0;
        }
    }

    if (op->nrof != 0) {
        esrv_update_item(UPD_NAME | UPD_NROF, op);
        return op;
    }

    object_destroy(op);
    return NULL;
}

/**
 * This function inserts the object op in the linked list inside the
 * object environment.
 *
 * @param op
 * Object to insert. Must be removed. May become invalid after return,
 * so use return value of the function.
 * @param where
 * Object to insert into.
 * @param flag
 * Combination of @ref INS_xxx "INS_xxx" values.
 * @return
 * Pointer to inserted item, which will be different than op if object
 * was merged.
 */
object *object_insert_into(object *op, object *where, int flag) {
    HARD_ASSERT(op != NULL);
    SOFT_ASSERT_RC(where != NULL, op, "Attempting to insert %s into nothing.", object_get_str(op));
    SOFT_ASSERT_RC(QUERY_FLAG(op, FLAG_REMOVED),
                   op,
                   "Attempting to insert non-removed object %s into %s",
                   object_get_str(op),
                   object_get_str(where));

    where = HEAD(where);
    op = HEAD(op);

    /* If the object has tail parts, it means the object is a multi-part
     * object that was on a map prior to this insert call. Thus, we will
     * want to destroy the tail parts of this object, so if the object
     * is at some later point inserted on the map again, the tails will
     * be re-created. */
    if (op->more != NULL) {
        for (object *tmp = op->more, *next; tmp != NULL; tmp = next) {
            next = tmp->more;
            object_destroy(tmp);
        }

        op->more = NULL;
    }

    CLEAR_FLAG(op, FLAG_REMOVED);

    if (!QUERY_FLAG(op, FLAG_SYS_OBJECT)) {
        if (!(flag & INS_NO_MERGE)) {
            for (object *tmp = where->inv; tmp != NULL; tmp = tmp->below) {
                if (!QUERY_FLAG(tmp, FLAG_SYS_OBJECT) && object_can_merge(tmp, op)) {
                    if (!object_custody_provenance_merge(tmp, op)) {
                        continue;
                    }
                    tmp->nrof += op->nrof;
                    esrv_update_item(UPD_NAME | UPD_NROF, tmp);
                    object_weight_add(where, op->weight * MAX(1, op->nrof));

                    SET_FLAG(op, FLAG_REMOVED);
                    object_destroy(op);

                    return tmp;
                }
            }
        }

        object_weight_add(where, WEIGHT_NROF(op, op->nrof));
    }

    SET_FLAG(op, FLAG_OBJECT_WAS_MOVED);
    op->map = NULL;
    op->env = where;
    op->above = NULL;
    op->below = NULL;
    op->x = 0;
    op->y = 0;

    if (where->inv == NULL) {
        where->inv = op;
    } else {
        op->below = where->inv;
        op->below->above = op;
        where->inv = op;
    }

    /* Check for event object and set the environment's object event flags. */
    if (op->type == EVENT_OBJECT && op->sub_type != 0) {
        where->event_flags |= (1U << (op->sub_type - 1));
    } else if (op->type == QUEST_CONTAINER && where->type == CONTAINER) {
        where->event_flags |= EVENT_FLAG(EVENT_QUEST);
    }

    if (where->type == EXIT) {
        exit_destination_cache_refresh(where);
        if (where->map != NULL) {
            exit_destination_cache_map_changed(where->map);
        }
    }

    /* Update living objects if inside living object. */
    object *env = object_get_env(op);
    if (env != op && IS_LIVE(env) && env->map != NULL) {
        living_update(env);
    }

    if (where->type == PLAYER || where->type == CONTAINER) {
        esrv_send_item(op);
    }

    return op;
}

static bool object_custody_auditable(const object *op) {
    return !QUERY_FLAG(op, FLAG_SYS_OBJECT) && op->type != PLAYER && op->type != FORCE &&
           op->type != POTION_EFFECT && op->type != EVENT_OBJECT && op->type != QUEST_CONTAINER &&
           (op->arch == NULL ||
            (strcmp(op->arch->name, "player_info") != 0 && strcmp(op->arch->name, "force") != 0));
}

static bool object_is_hidden_bank_info(const object *op) {
    return op->arch != NULL && strcmp(op->arch->name, "player_info") == 0 && op->name != NULL &&
           strcmp(op->name, "BANK_GENERAL") == 0;
}

static bool object_contains_hidden_bank_info(const object *op) {
    if (object_is_hidden_bank_info(op)) {
        return true;
    }
    FOR_INV_PREPARE(op, item) {
        if (object_contains_hidden_bank_info(item)) {
            return true;
        }
    }
    FOR_INV_FINISH();
    return false;
}

/**
 * Check whether an object's inventory contains currency at any depth.
 *
 * @param op
 * Object whose descendants to inspect.
 * @return
 * True if a descendant is a money object.
 */
bool object_contains_money_descendant(const object *op) {
    HARD_ASSERT(op != NULL);

    FOR_INV_PREPARE(op, item) {
        if (item->type == MONEY || object_contains_money_descendant(item)) {
            return true;
        }
    }
    FOR_INV_FINISH();
    return false;
}

static bool object_is_persistent_money(const object *op, const object *root) {
    return op->type == MONEY &&
           (root->type == PLAYER || root->map != NULL || !QUERY_FLAG(root, FLAG_REMOVED));
}

static bool object_root_is_persistent(const object *root) {
    return root->type == PLAYER || root->map != NULL || !QUERY_FLAG(root, FLAG_REMOVED);
}

static const char *object_custody_location(const object *op, const object *root) {
    if (root->type == PLAYER) {
        return "player";
    }
    if (op->env != NULL) {
        return "external-container";
    }
    if (op->map != NULL || root->map != NULL) {
        return "ground";
    }
    return "service";
}

static const char *object_custody_destination(const object *where, const object *root) {
    if (root->type == PLAYER) {
        return "player";
    }
    if (where->type == CONTAINER) {
        return "external-container";
    }
    return object_custody_location(where, root);
}

object_semantic_result_t
object_insert_into_reason(object *op, object *where, const char *reason, object **inserted_out) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(where != NULL);
    HARD_ASSERT(reason != NULL);
    HARD_ASSERT(inserted_out != NULL);
    *inserted_out = NULL;

    if (op == where || object_is_in_inventory(where, op)) {
        return OBJECT_SEMANTIC_FAILED;
    }

    object *source_root = object_get_env(op);
    object *destination_root = object_get_env(where);
    object *source_player = source_root->type == PLAYER ? source_root : NULL;
    object *destination_player = destination_root->type == PLAYER ? destination_root : NULL;
    if (op->type != PLAYER && object_contains_hidden_bank_info(op)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (op->type == MONEY) {
        if (source_root == op && destination_player != NULL) {
            return shop_insert_coin_object_reason(op, where, reason, inserted_out);
        }
        if (source_root == op && op->map == NULL && QUERY_FLAG(op, FLAG_REMOVED) &&
            destination_root->type != PLAYER && destination_root->map == NULL &&
            QUERY_FLAG(destination_root, FLAG_REMOVED)) {
            *inserted_out = object_insert_into(op, where, 0);
            return OBJECT_SEMANTIC_COMMITTED;
        }
        return OBJECT_SEMANTIC_FAILED;
    }
    if (source_root != destination_root && object_contains_money_descendant(op) &&
        (object_root_is_persistent(source_root) || object_root_is_persistent(destination_root))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (op->nrof > INT32_MAX) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (op->env != where) {
        if (source_root == op) {
            object_weight_sum(op);
        }
        uint64_t own_weight = (uint64_t)op->weight * MAX(1, op->nrof);
        if (own_weight > UINT32_MAX - op->carrying ||
            !object_weight_can_move(op->env, where, own_weight + op->carrying)) {
            return OBJECT_SEMANTIC_FAILED;
        }
    }
    if (source_player == destination_player || !object_custody_auditable(op)) {
        if (!QUERY_FLAG(op, FLAG_REMOVED)) {
            object_remove(op, 0);
        }
        *inserted_out = object_insert_into(op, where, 0);
        return OBJECT_SEMANTIC_COMMITTED;
    }

    object *actor = source_player != NULL ? source_player : destination_player;
    object_custody_transaction_t transaction = {0};
    const char *counterparty = "";
    if (source_player != NULL && destination_player != NULL) {
        object *other = actor == source_player ? destination_player : source_player;
        counterparty = object_custody_actor_id(other);
        if (counterparty == NULL) {
            counterparty = "";
        }
    }
    const char *acquirer =
        destination_player != NULL ? object_custody_actor_id(destination_player) : "";
    const char *relinquisher = source_player != NULL ? object_custody_actor_id(source_player) : "";
    int64_t quantity = MAX(1, op->nrof);
    int64_t delta = actor == source_player ? -quantity : quantity;
    if (actor != NULL &&
        (acquirer == NULL || relinquisher == NULL ||
         !object_custody_begin_parties(op,
                                       actor,
                                       reason,
                                       object_custody_location(op, source_root),
                                       object_custody_destination(where, destination_root),
                                       counterparty,
                                       (uint32_t)quantity,
                                       acquirer,
                                       relinquisher,
                                       delta < 0 ? quantity : 0,
                                       delta,
                                       delta < 0 ? 0 : quantity,
                                       0,
                                       "",
                                       "",
                                       &transaction))) {
        *inserted_out = NULL;
        return OBJECT_SEMANTIC_FAILED;
    }
    if (actor != NULL &&
        ((source_player != NULL && !object_custody_track_player(&transaction, source_player)) ||
         (destination_player != NULL &&
          !object_custody_track_player(&transaction, destination_player)) ||
         (op->map != NULL &&
          !object_custody_track_map_object(&transaction, op->map, op->x, op->y, op)) ||
         (source_root->type != PLAYER && source_root->map != NULL &&
          !object_custody_track_map_object(&transaction,
                                           source_root->map,
                                           source_root->x,
                                           source_root->y,
                                           source_root)) ||
         (destination_root->type != PLAYER && destination_root->map != NULL &&
          !object_custody_track_map_object(&transaction,
                                           destination_root->map,
                                           destination_root->x,
                                           destination_root->y,
                                           destination_root)))) {
        object_custody_abort(&transaction, "domain-registration-failed");
        return OBJECT_SEMANTIC_FAILED;
    }
    if (actor != NULL) {
        object_custody_apply(op, &transaction);
    }
    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        object_remove(op, 0);
    }
    object *inserted = object_insert_into(op, where, 0);
    *inserted_out = inserted;
    if (actor != NULL) {
        return object_custody_finish(&transaction) ? OBJECT_SEMANTIC_COMMITTED
                                                   : OBJECT_SEMANTIC_AMBIGUOUS;
    }
    return OBJECT_SEMANTIC_COMMITTED;
}

object_semantic_result_t object_insert_map_reason(object *op,
                                                  mapstruct *m,
                                                  int x,
                                                  int y,
                                                  const char *reason,
                                                  object **inserted_out) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(m != NULL);
    HARD_ASSERT(reason != NULL);
    HARD_ASSERT(inserted_out != NULL);
    *inserted_out = NULL;

    object *root = object_get_env(op);
    if (op->type == MONEY || (op->type != PLAYER && object_contains_money_descendant(op)) ||
        op->nrof > INT32_MAX || (op->type != PLAYER && object_contains_hidden_bank_info(op))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (root == op) {
        object_weight_sum(op);
    }
    object_custody_transaction_t transaction = {0};
    bool journal = root->type == PLAYER && object_custody_auditable(op);
    if (journal && !object_custody_begin(op,
                                         root,
                                         reason,
                                         "player",
                                         "ground",
                                         "",
                                         MAX(1, op->nrof),
                                         false,
                                         true,
                                         &transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (journal && !object_custody_track_map_object(&transaction, m, x, y, op)) {
        object_custody_abort(&transaction, "domain-registration-failed");
        return OBJECT_SEMANTIC_FAILED;
    }
    if (journal) {
        object_custody_apply(op, &transaction);
    }
    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        object_remove(op, 0);
    }
    op->x = x;
    op->y = y;
    *inserted_out = object_insert_map(op, m, NULL, 0);
    if (*inserted_out == NULL) {
        /* Map insertion may return NULL after walk-on effects destroyed the
         * object. Preserve the intent for restart reconciliation. */
        if (transaction.active) {
            bool attempted = gameplay_journal_attempt(transaction.transaction_id);
            HARD_ASSERT(attempted);
            (void)attempted;
        }
        return journal ? OBJECT_SEMANTIC_AMBIGUOUS : OBJECT_SEMANTIC_FAILED;
    }
    if (journal && !object_custody_finish(&transaction)) {
        return OBJECT_SEMANTIC_AMBIGUOUS;
    }
    return OBJECT_SEMANTIC_COMMITTED;
}

object_semantic_result_t object_remove_reason(object *op, const char *reason, bool destroy) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(reason != NULL);

    object *root = object_get_env(op);
    if (op->type != MONEY && object_contains_money_descendant(op) &&
        (root->type == PLAYER || root->map != NULL || !QUERY_FLAG(root, FLAG_REMOVED))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (!object_is_hidden_bank_info(op) && object_contains_hidden_bank_info(op)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (object_is_hidden_bank_info(op) && root->type == PLAYER) {
        return destroy ? bank_destroy_balance_reason(op, reason) : OBJECT_SEMANTIC_FAILED;
    }
    if (object_is_persistent_money(op, root)) {
        return root->type == PLAYER && destroy ? shop_destroy_coin_reason(op, reason)
                                                : OBJECT_SEMANTIC_FAILED;
    }
    bool journal = root->type == PLAYER && object_custody_auditable(op);
    object_custody_transaction_t transaction = {0};
    if (journal && !object_custody_begin(op,
                                         root,
                                         reason,
                                         "player",
                                         destroy ? "destroyed" : "service",
                                         "",
                                         MAX(1, op->nrof),
                                         false,
                                         true,
                                         &transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (journal && !destroy) {
        object_custody_apply(op, &transaction);
    }
    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        object_remove(op, 0);
    }
    if (destroy) {
        object_destroy(op);
    }
    if (journal) {
        return object_custody_finish(&transaction) ? OBJECT_SEMANTIC_COMMITTED
                                                   : OBJECT_SEMANTIC_AMBIGUOUS;
    }
    return OBJECT_SEMANTIC_COMMITTED;
}

object_semantic_result_t
object_decrease_reason(object *op, uint32_t nrof, const char *reason, object **survivor_out) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(reason != NULL);
    HARD_ASSERT(survivor_out != NULL);
    *survivor_out = NULL;

    object *root = object_get_env(op);
    if (object_contains_hidden_bank_info(op)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (nrof == 0) {
        *survivor_out = op;
        return OBJECT_SEMANTIC_COMMITTED;
    }
    if (op->type != MONEY && object_contains_money_descendant(op) &&
        (root->type == PLAYER || root->map != NULL || !QUERY_FLAG(root, FLAG_REMOVED))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (object_is_persistent_money(op, root)) {
        if (root->type != PLAYER) {
            return OBJECT_SEMANTIC_FAILED;
        }
        uint32_t before = op->nrof;
        if (nrof >= before) {
            return shop_destroy_coin_reason(op, reason);
        }
        object_semantic_result_t result = shop_set_coin_nrof_reason(op, before - nrof, reason);
        if (result != OBJECT_SEMANTIC_FAILED) {
            *survivor_out = op;
        }
        return result;
    }
    bool journal = root->type == PLAYER && object_custody_auditable(op) && nrof != 0;
    if (!journal) {
        *survivor_out = object_decrease(op, nrof);
        return OBJECT_SEMANTIC_COMMITTED;
    }

    uint32_t before = MAX(1, op->nrof);
    uint32_t quantity = MIN(nrof, before);
    uint32_t after = before - quantity;
    object_custody_transaction_t transaction = {0};
    const char *actor = object_custody_actor_id(root);
    if (actor == NULL || !object_custody_begin_parties(op,
                                                       root,
                                                       reason,
                                                       "player",
                                                       after == 0 ? "destroyed" : "service",
                                                       "",
                                                       quantity,
                                                       "",
                                                       "",
                                                       before,
                                                       -(int64_t)quantity,
                                                       after,
                                                       0,
                                                       "",
                                                       "",
                                                       &transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (!object_custody_track_player(&transaction, root)) {
        object_custody_abort(&transaction, "domain-registration-failed");
        return OBJECT_SEMANTIC_FAILED;
    }

    object_custody_apply(op, &transaction);
    *survivor_out = object_decrease(op, quantity);
    return object_custody_finish(&transaction) ? OBJECT_SEMANTIC_COMMITTED
                                               : OBJECT_SEMANTIC_AMBIGUOUS;
}

object_semantic_result_t
object_set_nrof_reason(object *op, uint32_t nrof, const char *reason, object **survivor_out) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(reason != NULL);
    HARD_ASSERT(survivor_out != NULL);
    *survivor_out = NULL;

    uint32_t before = MAX(1, op->nrof);
    uint32_t after = MAX(1, nrof);
    if (nrof > INT32_MAX ||
        (!QUERY_FLAG(op, FLAG_REMOVED) && op->env != NULL && after > before &&
         !object_weight_can_add(op->env, (uint64_t)op->weight * (after - before)))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    object *root = object_get_env(op);
    if (object_contains_hidden_bank_info(op)) {
        *survivor_out = NULL;
        return OBJECT_SEMANTIC_FAILED;
    }
    if (object_is_persistent_money(op, root)) {
        if (root->type != PLAYER) {
            *survivor_out = NULL;
            return OBJECT_SEMANTIC_FAILED;
        }
        object_semantic_result_t result = shop_set_coin_nrof_reason(op, nrof, reason);
        if (result != OBJECT_SEMANTIC_FAILED) {
            *survivor_out = op;
        }
        return result;
    }
    bool journal = root->type == PLAYER && object_custody_auditable(op) && before != after;
    object_custody_transaction_t transaction = {0};
    if (journal) {
        int64_t delta = (int64_t)after - before;
        if (!object_custody_begin_economy(op,
                                          root,
                                          reason,
                                          "player",
                                          "player",
                                          "",
                                          delta < 0 ? (uint32_t)-delta : (uint32_t)delta,
                                          false,
                                          false,
                                          before,
                                          delta,
                                          after,
                                          0,
                                          "",
                                          "",
                                          &transaction)) {
            return OBJECT_SEMANTIC_FAILED;
        }
        if (!object_custody_track_player(&transaction, root)) {
            object_custody_abort(&transaction, "domain-registration-failed");
            return OBJECT_SEMANTIC_FAILED;
        }
        object_custody_apply(op, &transaction);
    }

    *survivor_out = op;
    if (!QUERY_FLAG(op, FLAG_REMOVED) && op->env != NULL) {
        if (after > before) {
            object_weight_add(op->env, op->weight * (after - before));
        } else if (after < before) {
            object_weight_sub(op->env, op->weight * (before - after));
        }
    }
    op->nrof = nrof;
    if (journal) {
        bool provenance_ok = after > before
                                 ? object_custody_provenance_add(op, transaction.lineage, after - before)
                                 : object_custody_provenance_remove(op, before - after);
        if (!provenance_ok) {
            LOG(ERROR, "Could not update custody provenance for %s.", object_get_str(op));
        }
    }
    esrv_update_item(UPD_NAME | UPD_NROF, op);
    if (!journal) {
        return OBJECT_SEMANTIC_COMMITTED;
    }
    return object_custody_finish(&transaction) ? OBJECT_SEMANTIC_COMMITTED
                                               : OBJECT_SEMANTIC_AMBIGUOUS;
}

object_semantic_result_t object_set_value_reason(object *op, int64_t value, const char *reason) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(reason != NULL);

    object *root = object_get_env(op);
    if (object_is_persistent_money(op, root)) {
        if (root->type != PLAYER) {
            return value == op->value ? OBJECT_SEMANTIC_COMMITTED : OBJECT_SEMANTIC_FAILED;
        }
        return value == op->value ? OBJECT_SEMANTIC_COMMITTED : OBJECT_SEMANTIC_FAILED;
    }
    if (object_is_hidden_bank_info(op) && root->type == PLAYER) {
        return bank_set_balance_reason(op, value, reason);
    }
    if (root->type != PLAYER || !object_custody_auditable(op) || value == op->value) {
        op->value = value;
        return OBJECT_SEMANTIC_COMMITTED;
    }

    int64_t delta;
    if (value >= op->value) {
        if (op->value < 0 && value > INT64_MAX + op->value) {
            return OBJECT_SEMANTIC_FAILED;
        }
        delta = value - op->value;
    } else {
        if (value < 0 && op->value > INT64_MAX + value) {
            return OBJECT_SEMANTIC_FAILED;
        }
        delta = -(op->value - value);
    }
    object_custody_transaction_t transaction = {0};
    if (!object_custody_begin_economy(op,
                                      root,
                                      reason,
                                      "player",
                                      "player",
                                      "",
                                      MAX(1, op->nrof),
                                      false,
                                      false,
                                      op->value,
                                      delta,
                                      value,
                                      0,
                                      "",
                                      "",
                                      &transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (!object_custody_track_player(&transaction, root)) {
        object_custody_abort(&transaction, "domain-registration-failed");
        return OBJECT_SEMANTIC_FAILED;
    }
    object_custody_apply(op, &transaction);
    op->value = value;
    esrv_update_item(UPD_NAME, op);
    return object_custody_finish(&transaction) ? OBJECT_SEMANTIC_COMMITTED
                                               : OBJECT_SEMANTIC_AMBIGUOUS;
}

/**
 * Searches for any object with a matching archetype in the inventory
 * of the given object.
 *
 * @param op
 * Where to search.
 * @param at
 * Archetype to search for.
 * @return
 * First matching object, or NULL if none matches.
 */
object *object_find_arch(object *op, archetype_t *at) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(at != NULL);

    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        if (tmp->arch == at) {
            return tmp;
        }
    }

    return NULL;
}

/**
 * Searches for any object with a matching type variable in the
 * inventory of the given object.
 *
 * @param op
 * Object to search in.
 * @param type
 * Type to search for.
 * @return
 * First matching object, or NULL if none matches.
 */
object *object_find_type(object *op, uint8_t type) {
    HARD_ASSERT(op != NULL);

    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        if (tmp->type == type) {
            return tmp;
        }
    }

    return NULL;
}

/**
 * Get direction from one object to another.
 *
 * @param op
 * The first object.
 * @param target
 * The target object.
 * @param range_vector
 * Range vector pointer to use.
 * @return
 * The direction; zero if no direction can be computed.
 */
int object_dir_to_target(object *op, object *target) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(target != NULL);

    rv_vector rv;
    if (!get_rangevector(op, target, &rv, 0)) {
        return 0;
    }

    return rv.direction;
}

/**
 * Finds out if an object can be picked up.
 *
 * @note This introduces a weight limitation for monsters.
 * @param who
 * Who is trying to pick up. Can be a monster or a player.
 * @param item
 * Item we're trying to pick up.
 * @return
 * True if it can be picked up, false otherwise.
 */
bool object_can_pick(const object *op, const object *item) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(item != NULL);
    HARD_ASSERT(op->type == PLAYER || op->type == MONSTER);

    if (item->weight <= 0) {
        return false;
    }

    if (QUERY_FLAG(item, FLAG_NO_PICK) && !QUERY_FLAG(item, FLAG_UNPAID)) {
        return false;
    }

    if (IS_INVISIBLE(item, op) && !QUERY_FLAG(op, FLAG_SEE_INVISIBLE)) {
        return false;
    }

    if (QUERY_FLAG(item, FLAG_SOULBOUND)) {
        if (op->type != PLAYER) {
            return false;
        }

        shstr *name = object_get_value(item, "soulbound_name");
        if (name == NULL) {
            return false;
        }

        if (name != op->name) {
            return false;
        }
    }

    /* Weight limit for monsters */
    if (op->type != PLAYER && item->weight > op->weight / 3) {
        return false;
    }

    return true;
}

/**
 * Create clone from one object to another.
 *
 * @param op
 * Object to clone.
 * @return
 * Clone of op, including inventory and 'more' body parts.
 */
object *object_clone(const object *op) {
    HARD_ASSERT(op != NULL);

    op = HEAD(op);

    object *ret = NULL;

    object *prev = NULL;
    for (const object *part = op; part != NULL; part = part->more) {
        object *tmp = object_get();
        object_copy(tmp, part, false);
        tmp->x -= op->x;
        tmp->y -= op->y;

        if (part->head == NULL) {
            ret = tmp;
            tmp->head = NULL;
        } else {
            tmp->head = ret;
        }

        tmp->more = NULL;

        if (prev != NULL) {
            prev->more = tmp;
        }

        prev = tmp;
    }

    /* Copy inventory */
    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        object_insert_into(object_clone(tmp), ret, 0);
    }

    return ret;
}

/**
 * Creates an object using a string representing its content.
 *
 * @param str
 * String to load the object from.
 * @return
 * The newly created object, NULL on failure.
 */
object *object_load_str(const char *str) {
    HARD_ASSERT(str != NULL);

    object *obj = object_get();
    if (load_object(str, obj, 0) != LL_NORMAL) {
        LOG(ERROR, "load_object() failed.");
        object_destroy(obj);
        return NULL;
    }

    object_weight_sum(obj);
    return obj;
}

/**
 * Zero the key_values on op, decrementing the shared-string refcounts
 * and freeing the links.
 *
 * @param op
 * Object to clear.
 */
void object_free_key_values(object *op) {
    HARD_ASSERT(op != NULL);

    key_value_t *field, *tmp;
    LL_FOREACH_SAFE(op->key_values, field, tmp) {
        if (field->key != NULL) {
            free_string_shared(field->key);
        }

        if (field->value != NULL) {
            free_string_shared(field->value);
        }

        free(field);
    }

    op->key_values = NULL;
}

/**
 * Search for a field by key.
 *
 * @param op
 * Object to search in.
 * @param key
 * Key to search. Must be a shared string.
 * @return
 * The link from the list if pb has a field named key, NULL otherwise.
 */
key_value_t *object_get_key_link(const object *op, shstr *key) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(key != NULL);

    key_value_t *field;
    LL_FOREACH(op->key_values, field) {
        if (field->key == key) {
            return field;
        }
    }

    return NULL;
}

/**
 * Get an extra value by key.
 *
 * @param op
 * Object to search in.
 * @param key
 * Key of which to retrieve the value. Doesn't need to be a shared string.
 * @return
 * The value if found, NULL otherwise.
 * @note
 * The returned string is shared.
 */
shstr *object_get_value(const object *op, const char *const key) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(key != NULL);

    shstr *shared_key = find_string(key);
    if (shared_key == NULL) {
        return NULL;
    }

    key_value_t *field;
    LL_FOREACH(op->key_values, field) {
        if (field->key == shared_key) {
            return field->value;
        }
    }

    return NULL;
}

/**
 * Updates or sets a key value.
 *
 * @param op
 * Object to update.
 * @param key
 * Key to set or update. Must be a shared string.
 * @param value
 * Value to set. Doesn't need to be a shared string. Can be NULL.
 * @param add_key
 * If false, will not add the key if it doesn't exist in op.
 * @return
 * True if key was updated or added, false otherwise.
 */
static bool object_set_value_s(object *op, shstr *key, const char *value, bool add_key) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(key != NULL);

    key_value_t *field;
    key_value_t *last = NULL;
    LL_FOREACH(op->key_values, field) {
        if (field->key != key) {
            last = field;
            continue;
        }

        if (field->value != NULL) {
            free_string_shared(field->value);
        }

        if (value != NULL) {
            field->value = add_string(value);
        } else {
            /* Basically, if the archetype has this key set, we need to
             * store the NULL value so when we save it, we save the empty
             * value so that when we load, we get this value back
             * again. */
            if (object_get_key_link(&op->arch->clone, key)) {
                field->value = NULL;
            } else {
                /* Delete this link */
                if (field->key != NULL) {
                    free_string_shared(field->key);
                }

                if (last != NULL) {
                    last->next = field->next;
                } else {
                    op->key_values = field->next;
                }

                free(field);
            }
        }

        return true;
    }

    if (!add_key) {
        return false;
    }

    /* There isn't any good reason to store a NULL value in the key/value
     * list. If the archetype has this key, then we should also have it,
     * so shouldn't be here. If user wants to store empty strings, should
     * pass in "". */
    if (value == NULL) {
        return true;
    }

    field = xmalloc(sizeof(*field));
    field->key = add_refcount(key);
    field->value = add_string(value);
    /* Usual prepend-addition. */
    field->next = op->key_values;
    op->key_values = field;

    return true;
}

/**
 * Updates the key in op to value.
 *
 * @param op
 * Object to update.
 * @param key
 * Key to set or update. Doesn't need to be a shared string.
 * @param value
 * Value to set. Doesn't need to be a shared string. Can be NULL.
 * @param add_key
 * If false, will not add the key if it doesn't exist in op.
 * @return
 * True if key was updated or added, false otherwise.
 * @note
 * This function is merely a wrapper to object_set_value_s() to ensure
 * the key is a shared string.
 */
bool object_set_value(object *op, const char *key, const char *value, bool add_key) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(key != NULL);

    bool temp_ref = false;
    shstr *shstr_key = find_string(key);
    if (shstr_key == NULL) {
        shstr_key = add_string(key);
        temp_ref = true;
    }

    bool ret = object_set_value_s(op, shstr_key, value, add_key);

    if (temp_ref) {
        free_string_shared(shstr_key);
    }

    return ret;
}

static bool object_custody_random_id(char output[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    unsigned char random[16];
    bool ok =
        RAND_bytes(random, sizeof(random)) == 1 &&
        string_tohex(random, sizeof(random), output, GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE, false) ==
            sizeof(random) * 2U;
    OPENSSL_cleanse(random, sizeof(random));
    return ok;
}

static bool object_custody_actor(object *actor_ob) {
    if (actor_ob->type != PLAYER || CONTR(actor_ob) == NULL || CONTR(actor_ob)->cs == NULL ||
        CONTR(actor_ob)->cs->account == NULL || actor_ob->name == NULL) {
        return false;
    }
    if (actor_ob->custody_actor != NULL) {
        return true;
    }
    char id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE], actor[MAX_BUF];
    if (!object_custody_random_id(id)) {
        LOG(ERROR, "Could not create durable custody identity for player %s.", actor_ob->name);
        return false;
    }
    snprintf(actor, sizeof(actor), "%s:%s", CONTR(actor_ob)->cs->account, id);
    actor_ob->custody_actor = add_string(actor);
    return true;
}

const char *object_custody_actor_id(object *player_ob) {
    return object_custody_actor(player_ob) ? player_ob->custody_actor : NULL;
}

bool object_custody_begin_parties(const object *op,
                                  object *actor_ob,
                                  const char *reason,
                                  const char *source,
                                  const char *destination,
                                  const char *counterparty,
                                  uint32_t quantity,
                                  const char *acquirer,
                                  const char *relinquisher,
                                  int64_t before,
                                  int64_t delta,
                                  int64_t after,
                                  int64_t price,
                                  const char *currency,
                                  const char *funding,
                                  object_custody_transaction_t *transaction) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(actor_ob != NULL);
    HARD_ASSERT(reason != NULL);
    HARD_ASSERT(source != NULL);
    HARD_ASSERT(destination != NULL);
    HARD_ASSERT(acquirer != NULL);
    HARD_ASSERT(relinquisher != NULL);
    HARD_ASSERT(transaction != NULL);

    memset(transaction, 0, sizeof(*transaction));
    if (op->arch == NULL || !object_custody_actor(actor_ob)) {
        return false;
    }
    snprintf(VS(transaction->actor), "%s", actor_ob->custody_actor);
    if (op->custody_lineage != NULL) {
        snprintf(VS(transaction->lineage), "%s", op->custody_lineage);
    } else {
        char id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
        if (!object_custody_random_id(id)) {
            return false;
        }
        snprintf(VS(transaction->lineage), "item:%s", id);
    }
    snprintf(VS(transaction->first_after),
             "%s",
             op->custody_first != NULL ? op->custody_first : acquirer);
    snprintf(VS(transaction->last_after),
             "%s",
             relinquisher[0] != '\0' ? relinquisher
                                     : (op->custody_last != NULL ? op->custody_last : ""));
    transaction->acquire = acquirer[0] != '\0';
    transaction->relinquish = relinquisher[0] != '\0';

    char snapshot[GAMEPLAY_JOURNAL_ID_MAX + 1];
    snprintf(VS(snapshot),
             "arch=%s;type=%u;nrof=%" PRIu32 ";value=%" PRId64 ";weight=%" PRIu32,
             op->arch != NULL ? op->arch->name : "unknown",
             op->type,
             op->nrof != 0 ? op->nrof : 1,
             op->value,
             op->weight);
    char provenance_before[GAMEPLAY_JOURNAL_ID_MAX + 1];
    char provenance_after[GAMEPLAY_JOURNAL_ID_MAX + 1];
    snprintf(VS(provenance_before),
             "first=%.112s;last=%.112s",
             op->custody_first != NULL ? op->custody_first : "",
             op->custody_last != NULL ? op->custody_last : "");
    snprintf(VS(provenance_after),
             "first=%.112s;last=%.112s",
             transaction->first_after,
             transaction->last_after);
    gameplay_journal_change_t change = {
        .subject_id = transaction->lineage,
        .lineage_id = transaction->lineage,
        .before = before,
        .delta = delta,
        .after = after,
        .archetype = op->arch != NULL ? op->arch->name : "unknown",
        .object_type = op->type,
        .snapshot = snapshot,
        .quantity = quantity,
        .source = source,
        .destination = destination,
        .actor = transaction->actor,
        .counterparty = counterparty,
        .provenance_before = provenance_before,
        .provenance_after = provenance_after,
        .price = price,
        .currency = currency,
        .funding = funding,
    };
    if (!gameplay_journal_required()) {
        return true;
    }
    transaction->active = gameplay_journal_player_begin_change(CONTR(actor_ob),
                                                               GAMEPLAY_JOURNAL_ITEM,
                                                               reason,
                                                               &change,
                                                               transaction->transaction_id);
    return transaction->active;
}

bool object_custody_begin_economy(const object *op,
                                  object *actor_ob,
                                  const char *reason,
                                  const char *source,
                                  const char *destination,
                                  const char *counterparty,
                                  uint32_t quantity,
                                  bool acquire,
                                  bool relinquish,
                                  int64_t before,
                                  int64_t delta,
                                  int64_t after,
                                  int64_t price,
                                  const char *currency,
                                  const char *funding,
                                  object_custody_transaction_t *transaction) {
    const char *actor = object_custody_actor_id(actor_ob);
    if (actor == NULL) {
        return false;
    }
    return object_custody_begin_parties(op,
                                        actor_ob,
                                        reason,
                                        source,
                                        destination,
                                        counterparty,
                                        quantity,
                                        acquire ? actor : "",
                                        relinquish ? actor : "",
                                        before,
                                        delta,
                                        after,
                                        price,
                                        currency,
                                        funding,
                                        transaction);
}

bool object_custody_begin(const object *op,
                          object *actor_ob,
                          const char *reason,
                          const char *source,
                          const char *destination,
                          const char *counterparty,
                          uint32_t quantity,
                          bool acquire,
                          bool relinquish,
                          object_custody_transaction_t *transaction) {
    return object_custody_begin_economy(op,
                                        actor_ob,
                                        reason,
                                        source,
                                        destination,
                                        counterparty,
                                        quantity,
                                        acquire,
                                        relinquish,
                                        relinquish ? quantity : 0,
                                        relinquish ? -(int64_t)quantity : (int64_t)quantity,
                                        relinquish ? 0 : quantity,
                                        0,
                                        "",
                                        "",
                                        transaction);
}

void object_custody_apply(object *op, const object_custody_transaction_t *transaction) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(transaction != NULL);

    if (op->custody_lineage == NULL) {
        op->custody_lineage = add_string(transaction->lineage);
    }
    if (!object_custody_provenance_seed(op)) {
        LOG(ERROR, "Could not seed custody provenance for %s.", object_get_str(op));
    }
    if (transaction->acquire && op->custody_first == NULL) {
        op->custody_first = add_string(transaction->first_after);
    }
    if (transaction->relinquish) {
        FREE_AND_CLEAR_HASH2(op->custody_last);
        op->custody_last = add_string(transaction->last_after);
    }
}

bool object_custody_track_player(object_custody_transaction_t *transaction, object *player_ob) {
    HARD_ASSERT(transaction != NULL);
    return !transaction->active ||
           gameplay_journal_track_player(transaction->transaction_id, player_ob);
}

bool object_custody_track_map_object(object_custody_transaction_t *transaction,
                                     mapstruct *map,
                                     int x,
                                     int y,
                                     const object *op) {
    HARD_ASSERT(transaction != NULL);
    return !transaction->active ||
           gameplay_journal_track_map_object(transaction->transaction_id, map, x, y, op);
}

bool object_custody_commit(object *op, object_custody_transaction_t *transaction) {
    object_custody_apply(op, transaction);
    return object_custody_finish(transaction);
}

bool object_custody_finish(object_custody_transaction_t *transaction) {
    HARD_ASSERT(transaction != NULL);
    if (transaction->active) {
        bool committed = gameplay_journal_commit(transaction->transaction_id);
        transaction->active = false;
        return committed;
    }
    return true;
}

void object_custody_abort(object_custody_transaction_t *transaction, const char *reason) {
    HARD_ASSERT(transaction != NULL);
    HARD_ASSERT(reason != NULL);
    if (transaction->active) {
        (void)gameplay_journal_abort(transaction->transaction_id, reason);
        transaction->active = false;
    }
}

void object_custody_record(const object *op, object *actor_ob, const char *reason) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(actor_ob != NULL);
    HARD_ASSERT(reason != NULL);

    if (op->custody_lineage == NULL || !gameplay_journal_available()) {
        return;
    }

    object_custody_transaction_t transaction;
    if (object_custody_begin(op,
                             actor_ob,
                             reason,
                             "service",
                             "player",
                             "",
                             MAX(1, op->nrof),
                             true,
                             false,
                             &transaction)) {
        (void)object_custody_finish(&transaction);
    }
}

/** Record a successful acquisition by a player without exposing custody data to clients. */
void object_custody_acquire(object *op, const object *player_ob) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(player_ob != NULL);

    if (player_ob->type != PLAYER || CONTR(player_ob) == NULL || CONTR(player_ob)->cs == NULL ||
        CONTR(player_ob)->cs->account == NULL || player_ob->name == NULL) {
        return;
    }

    object *actor_ob = (object *)player_ob;
    if (!object_custody_actor(actor_ob)) {
        return;
    }

    if (op->custody_first == NULL) {
        op->custody_first = add_refcount(actor_ob->custody_actor);
    }

    if (op->custody_lineage == NULL) {
        char id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE], lineage[MAX_BUF];
        if (!object_custody_random_id(id)) {
            LOG(ERROR, "Could not create durable custody lineage for %s.", object_get_str(op));
            return;
        }
        snprintf(lineage, sizeof(lineage), "item:%s", id);
        op->custody_lineage = add_string(lineage);
    }
    if (!object_custody_provenance_seed(op)) {
        LOG(ERROR, "Could not seed custody provenance for %s.", object_get_str(op));
    }
}

/** Record the player that successfully relinquished custody of an item. */
void object_custody_relinquish(object *op, const object *player_ob) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(player_ob != NULL);

    if (player_ob->type != PLAYER || CONTR(player_ob) == NULL || CONTR(player_ob)->cs == NULL ||
        CONTR(player_ob)->cs->account == NULL || player_ob->name == NULL) {
        return;
    }

    object *actor_ob = (object *)player_ob;
    if (!object_custody_actor(actor_ob)) {
        return;
    }
    FREE_AND_CLEAR_HASH2(op->custody_last);
    op->custody_last = add_refcount(actor_ob->custody_actor);
}

/**
 * Checks if the specified object matches one of the keywords in the specified
 * string. This is used for example by the /drop and /take commands, but also
 * by the /apply command.
 *
 * Calling function takes care of what action might need to be done and
 * if it is valid (pickup, drop, etc).
 *
 * @param op
 * The item we're trying to match.
 * @param caller
 * Who is trying to match the objects, for the purposes of functions like
 * object_get_name_s().
 * @param str
 * String we're searching.
 * @return
 * Non-zero if we have a match. A higher value means a better match. Zero
 * means no match.
 */
int object_matches_string(object *op, object *caller, const char *str) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(caller != NULL);
    HARD_ASSERT(str != NULL);

    if (caller->type == PLAYER) {
        CONTR(caller)->count = op->nrof;
    }

    char word[MAX_BUF];
    size_t pos = 0;
    while (string_get_word(str, &pos, ',', VS(word), 0)) {
        char *cp = word;

        /* All is a very generic match - low match value */
        if (strcasecmp(cp, "all") == 0) {
            return 1;
        }

        /* Unpaid is a little more specific */
        if (QUERY_FLAG(op, FLAG_UNPAID) && strcasecmp(cp, "unpaid") == 0) {
            return 2;
        }

        bool identified = QUERY_FLAG(op, FLAG_IDENTIFIED);
        if (identified) {
            if ((QUERY_FLAG(op, FLAG_CURSED) || QUERY_FLAG(op, FLAG_DAMNED)) &&
                strcasecmp(cp, "cursed") == 0) {
                return 2;
            }

            if (QUERY_FLAG(op, FLAG_IS_MAGICAL) && strcasecmp(cp, "magical") == 0) {
                return 2;
            }

            if (op->artifact != NULL && strcasecmp(cp, "artifact") == 0) {
                return 2;
            }
        }

        if (!QUERY_FLAG(op, FLAG_INV_LOCKED) && strcasecmp(cp, "unlocked") == 0) {
            return 2;
        }

        if (identified && strcasecmp(cp, "identified") == 0) {
            return 2;
        }

        if (!identified && strcasecmp(cp, "unidentified") == 0) {
            return 2;
        }

        if ((op->type == FOOD || op->type == DRINK) && strcasecmp(cp, "food") == 0) {
            return 2;
        }

        if ((op->type == GEM || op->type == JEWEL || op->type == NUGGET || op->type == PEARL) &&
            strcasecmp(cp, "valuables") == 0) {
            return 2;
        }

        if (op->type == WEAPON) {
            if (op->item_skill - 1 == SK_IMPACT_WEAPONS) {
                if (strcasecmp(cp, "impact weapons") == 0) {
                    return 2;
                }
            } else if (op->item_skill - 1 == SK_SLASH_WEAPONS) {
                if (strcasecmp(cp, "slash weapons") == 0) {
                    return 2;
                }
            } else if (op->item_skill - 1 == SK_CLEAVE_WEAPONS) {
                if (strcasecmp(cp, "cleave weapons") == 0) {
                    return 2;
                }
            } else if (op->item_skill - 1 == SK_PIERCE_WEAPONS) {
                if (strcasecmp(cp, "pierce weapons") == 0) {
                    return 2;
                }
            }
        } else if (op->type == BOOK) {
            if (strcasecmp(cp, "books") == 0) {
                return 2;
            }

            if (op->msg == NULL && strcasecmp(cp, "empty books") == 0) {
                return 2;
            }

            int book_level[2];
            if (!QUERY_FLAG(op, FLAG_NO_SKILL_IDENT)) {
                if (strcasecmp(cp, "unread books") == 0) {
                    return 2;
                }

                if (sscanf(cp, "unread level %d books", &book_level[0]) == 1 &&
                    op->level == book_level[0]) {
                    return 2;
                }

                if (sscanf(cp, "unread level %d-%d books", &book_level[0], &book_level[1]) == 2 &&
                    op->level >= book_level[0] && op->level <= book_level[1]) {
                    return 2;
                }
            } else {
                if (strcasecmp(cp, "read books") == 0) {
                    return 2;
                }

                if (sscanf(cp, "read level %d books", &book_level[0]) == 1 &&
                    op->level == book_level[0]) {
                    return 2;
                }

                if (sscanf(cp, "read level %d-%d books", &book_level[0], &book_level[1]) == 2 &&
                    op->level >= book_level[0] && op->level <= book_level[1]) {
                    return 2;
                }
            }
        }

        int count = 0;

        /* Allow for things like '100 arrows', but don't accept
         * strings like '+2', '-1' as numbers. */
        if (isdigit(cp[0]) && (count = atoi(cp)) != 0) {
            cp = strchr(cp, ' ');

            /* Get rid of spaces */
            while (cp != NULL && cp[0] == ' ') {
                cp++;
            }
        }

        if (cp == NULL || cp[0] == '\0' || count < 0) {
            return 0;
        }

        char *obj_name = object_get_name_s(op, caller);
        char *base_name = object_get_base_name_s(op, caller);
        char *short_name = object_get_short_name_s(op, caller);

        int retval;
        if (strcasecmp(cp, obj_name) == 0) {
            retval = 20;
        } else if (strcasecmp(cp, short_name) == 0) {
            retval = 18;
        } else if (strcasecmp(cp, base_name) == 0) {
            retval = 16;
        } else if (op->custom_name != NULL && strcasecmp(cp, op->custom_name) == 0) {
            retval = 15;
        } else if (strncasecmp(cp, base_name, strlen(cp)) == 0) {
            retval = 14;
        } else if (strstr(base_name, cp) != NULL) {
            /* Do substring checks, so things like 'Str+1' will match.
             * retval of these should perhaps be lower - they are lower
             * than the specific strcasecmps above, but still higher than
             * some other match criteria. */
            retval = 12;
        } else if (strstr(short_name, cp) != NULL) {
            retval = 12;
        } else if (op->custom_name != NULL && strstr(op->custom_name, cp) != NULL) {
            /* Check for partial custom name, but give a really low priority. */
            retval = 3;
        } else {
            retval = 0;
        }

        free(obj_name);
        free(base_name);
        free(short_name);

        if (retval != 0) {
            if (caller->type == PLAYER && count != 0) {
                CONTR(caller)->count = count;
            }

            return retval;
        }
    }

    return 0;
}

/**
 * Get object's gender ID, as defined in #GENDER_xxx.
 *
 * @param op
 * Object to get gender ID of.
 * @return
 * The gender ID.
 */
int object_get_gender(const object *op) {
    HARD_ASSERT(op != NULL);

    if (QUERY_FLAG(op, FLAG_IS_MALE)) {
        if (QUERY_FLAG(op, FLAG_IS_FEMALE)) {
            return GENDER_HERMAPHRODITE;
        } else {
            return GENDER_MALE;
        }
    } else if (QUERY_FLAG(op, FLAG_IS_FEMALE)) {
        return GENDER_FEMALE;
    }

    return GENDER_NEUTER;
}

/**
 * Reverses order of all the objects in the specified object's inventory.
 *
 * @param op
 * Object to reverse the inventory of.
 */
void object_reverse_inventory(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->inv == NULL) {
        return;
    }

    if (op->inv->inv != NULL) {
        object_reverse_inventory(op->inv);
    }

    object *next = op->inv->below;
    op->inv->above = NULL;
    op->inv->below = NULL;

    while (next != NULL) {
        object *tmp = next;
        next = next->below;

        tmp->above = NULL;
        tmp->below = op->inv;
        tmp->below->above = tmp;
        op->inv = tmp;

        if (tmp->inv != NULL) {
            object_reverse_inventory(tmp);
        }
    }
}

/**
 * Make the specified object enter a map, using either an absolute position
 * with a map pointer and coordinates, or using an exit object.
 *
 * If neither 'm' nor 'exit' is specified, the object enters the emergency
 * map.
 *
 * @param op
 * Object entering a map.
 * @param exit
 * Exit object to use in order to ender the map.
 * @param m
 * Map to enter.
 * @param x
 * X coordinate.
 * @param y
 * Y coordinate.
 * @param fixed_pos
 * If true, will not attempt to find an adjacency square if the original
 * destination is blocked.
 * @return
 * True on success, false on failure.
 */
bool object_enter_map(object *op, object *exit, mapstruct *m, int x, int y, bool fixed_pos) {
    HARD_ASSERT(op != NULL);

    op = HEAD(op);
    mapstruct *oldmap = op->map;

    if (m == NULL && exit != NULL) {
        if (EXIT_PATH(exit) == NULL) {
            return false;
        }

        x = EXIT_X(exit);
        y = EXIT_Y(exit);
        fixed_pos = QUERY_FLAG(exit, FLAG_USE_FIX_POS);

        if (strcmp(EXIT_PATH(exit), "/random/") == 0) {
            RMParms rp;
            memset(&rp, 0, sizeof(RMParms));

            rp.Xsize = -1;
            rp.Ysize = -1;

            if (exit->msg != NULL) {
                set_random_map_variable(&rp, exit->msg);
            }

            rp.origin_x = exit->x;
            rp.origin_y = exit->y;
            snprintf(VS(rp.origin_map), "%s", op->map->path);
            rp.origin_map_ptr = op->map;

            /* Pick a new pathname for the new map. Currently, we just use a
             * static variable and increment the counter by one each time. */
            static uint64_t reference_number = 0;
            char newmap_name[HUGE_BUF];
            snprintf(VS(newmap_name), "/random/%" PRIu64, reference_number++);

            /* Now to generate the actual map. */
            m = generate_random_map(newmap_name, &rp);

            /* Update the exit_ob so it now points directly at the newly
             * created random map. */
            if (m != NULL) {
                x = EXIT_X(exit) = MAP_ENTER_X(m);
                y = EXIT_Y(exit) = MAP_ENTER_Y(m);
                FREE_AND_COPY_HASH(EXIT_PATH(exit), newmap_name);
                FREE_AND_COPY_HASH(m->path, newmap_name);
            }
        } else if (exit->map != NULL) {
            bool unique = (op->type == PLAYER &&
                           (exit->last_eat == MAP_PLAYER_MAP ||
                            (MAP_UNIQUE(exit->map) && !map_path_isabs(EXIT_PATH(exit)))));
            char *path = map_get_path(exit->map, EXIT_PATH(exit), unique, op->name);
            m = ready_map_name(path, NULL, 0);
            free(path);

            /* Failed to load a random map? */
            if (m == NULL && op->type == PLAYER && strncmp(EXIT_PATH(exit), "/random/", 8) == 0) {
                m = ready_map_name(CONTR(op)->savebed_map, NULL, 0);
                return object_enter_map(op, NULL, m, CONTR(op)->bed_x, CONTR(op)->bed_y, true);
            }
        } else {
            m = ready_map_name(EXIT_PATH(exit), NULL, MAP_NAME_SHARED);
        }

        if (m == NULL) {
            return false;
        }

        /* If exit is damned, update player's savebed position. */
        if (QUERY_FLAG(exit, FLAG_DAMNED) && op->type == PLAYER) {
            snprintf(VS(CONTR(op)->savebed_map), "%s", m->path);
            CONTR(op)->bed_x = x;
            CONTR(op)->bed_y = y;
            player_save(op);
        }
    }

    if (m == NULL) {
        m = ready_map_name(EMERGENCY_MAPPATH, NULL, 0);
        x = EMERGENCY_X;
        y = EMERGENCY_Y;
        fixed_pos = true;
    }

    if (exit == NULL && MAP_FIXEDLOGIN(m)) {
        x = MAP_ENTER_X(m);
        y = MAP_ENTER_Y(m);
    }

    mapstruct *m2 = get_map_from_coord(m, &x, &y);
    if (m2 == NULL) {
        LOG(ERROR, "Invalid exit coordinates (%d,%d): %s", x, y, object_get_str(exit));
        if (exit != NULL) {
            return false;
        }

        x = MAP_ENTER_X(m);
        y = MAP_ENTER_Y(m);
    } else {
        m = m2;
    }

    if (exit != NULL) {
        exit_landing_t landing;
        if (!exit_find_landing(op, m, x, y, true, fixed_pos, true, &landing)) {
            return false;
        }

        m = landing.map;
        x = landing.x;
        y = landing.y;
    } else if (!fixed_pos && blocked(op, m, x, y, TERRAIN_ALL) != 0) {
        int i = map_free_spot(m, x, y, 1, SIZEOFFREE1, op->arch, NULL);
        if (i != -1) {
            x += freearr_x[i];
            y += freearr_y[i];
        }
    }

    if (!QUERY_FLAG(op, FLAG_REMOVED)) {
        object_remove(op, 0);
    }

    if (exit != NULL) {
        int sub_direction = exit->last_heal - 1 == TILED_UP ? 1 : -1;
        for (int sub_layer = op->sub_layer; sub_layer >= 0 && sub_layer < NUM_SUB_LAYERS;
             sub_layer += sub_direction) {
            object *floor = GET_MAP_OB_LAYER(m, x, y, LAYER_FLOOR, sub_layer);
            if (floor != NULL) {
                op->sub_layer = sub_layer;
                break;
            }
        }
    }

    if (op->map != NULL && op->type == PLAYER) {
        trigger_map_event(MEVENT_LEAVE, op->map, op, NULL, NULL, NULL, 0);
    }

    op->x = x;
    op->y = y;

    /* Player state can be changed by login and map-leave plugins while the
     * player is off-map. Rebuild it at this lifecycle boundary, after those
     * events and while the removed guard still protects the old map. Routine
     * one-tile movement uses object_insert_map() directly and avoids this
     * full reconciliation. */
    if (op->type == PLAYER) {
        living_update_player(op);
    }

    op = object_insert_map(op, m, NULL, 0);
    if (op == NULL) {
        return false;
    }

    trigger_map_event(MEVENT_ENTER, m, op, NULL, NULL, NULL, 0);
    m->timeout = 0;

    /* Do some action special for players after we have inserted them. */
    if (op->type == PLAYER) {
        if (CONTR(op) != NULL) {
            snprintf(VS(CONTR(op)->maplevel), "%s", m->path);
            CONTR(op)->count = 0;
            metrics_character_visit(CONTR(op), m, oldmap != NULL && oldmap != m);
        }

        /* If the player is changing maps, we need to do some special things
         * Do this after the player is on the new map - otherwise the force
         * swap of the old map does not work. */
        if (oldmap != NULL && oldmap != m && oldmap->player_first == NULL) {
            set_map_timeout(oldmap);
        }
    }

    if (exit != NULL && exit->stats.dam != 0 && op->type == PLAYER) {
        attack_hit_situational(op, exit, exit->stats.dam);
    }

    return true;
}

object_semantic_result_t
object_enter_map_reason(object *op, mapstruct *m, int x, int y, const char *reason) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(m != NULL);
    HARD_ASSERT(reason != NULL);

    object *root = object_get_env(op);
    if (op->type == MONEY || (op->type != PLAYER && object_contains_money_descendant(op)) ||
        op->nrof > INT32_MAX || (op->type != PLAYER && object_contains_hidden_bank_info(op))) {
        return OBJECT_SEMANTIC_FAILED;
    }
    object_custody_transaction_t transaction = {0};
    bool journal = root->type == PLAYER && object_custody_auditable(op);
    if (journal && !object_custody_begin(op,
                                         root,
                                         reason,
                                         "player",
                                         "ground",
                                         "",
                                         MAX(1, op->nrof),
                                         false,
                                         true,
                                         &transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (journal && !object_custody_track_map_object(&transaction, m, x, y, op)) {
        object_custody_abort(&transaction, "domain-registration-failed");
        return OBJECT_SEMANTIC_FAILED;
    }
    if (journal) {
        object_custody_apply(op, &transaction);
    }
    if (!object_enter_map(op, NULL, m, x, y, true)) {
        /* Entry can fail after removal or a map effect mutated the object. */
        if (transaction.active) {
            bool attempted = gameplay_journal_attempt(transaction.transaction_id);
            HARD_ASSERT(attempted);
            (void)attempted;
        }
        return journal ? OBJECT_SEMANTIC_AMBIGUOUS : OBJECT_SEMANTIC_FAILED;
    }
    if (journal && !object_custody_finish(&transaction)) {
        return OBJECT_SEMANTIC_AMBIGUOUS;
    }
    return OBJECT_SEMANTIC_COMMITTED;
}

/**
 * Acquires a string representation of the object that is suitable for
 * debugging purposes, as it includes the object's name, archname, map,
 * environment, etc.
 *
 * This function cycles through internal buffers to use as return values,
 * and is safe to call up to ten times. After that, previously returned
 * pointers will start getting overwritten.
 *
 * @param op
 * Object. Can be NULL.
 * @return
 * String representation of the object.
 */
const char *object_get_str(const object *op) {
    static char buf[10][HUGE_BUF * 16];
    static int buf_idx = 0;

    buf_idx++;
    buf_idx %= 10;

    return object_get_str_r(op, VS(buf[buf_idx]));
}

/**
 * Re-entrant version of object_get_str().
 *
 * @param op
 * Object. Can be NULL.
 * @param buf
 * Buffer to use.
 * @param bufsize
 * Size of 'buf'.
 * @return
 * 'buf'.
 */
char *object_get_str_r(const object *op, char *buf, size_t bufsize) {
    HARD_ASSERT(buf != NULL);

    if (op == NULL) {
        snprintf(buf, bufsize, "<no object>");
        return buf;
    }

    snprintf(buf, bufsize, "%s UID: %u", op->name != NULL ? op->name : "<no name>", op->count);

    if (arch_table != NULL && op->arch != NULL && op->arch->name != NULL) {
        snprintfcat(buf, bufsize, " arch: %s", op->arch->name);
    }

    if (first_map != NULL && op->map != NULL) {
        snprintfcat(buf,
                    bufsize,
                    " map: %s [%s] @ %d,%d",
                    op->map->name != NULL ? op->map->name : "<no name>",
                    op->map->path != NULL ? op->map->path : "<no path>",
                    op->x,
                    op->y);
    } else if (op->env != NULL) {
        char buf2[HUGE_BUF];
        snprintfcat(buf, bufsize, " env: [%s]", object_get_str_r(op->env, VS(buf2)));
    }

    return buf;
}

/**
 * Checks if the specified coordinates are blocked for the specified object.
 *
 * Takes multi-part objects into account.
 *
 * @param op
 * Object to check.
 * @param m
 * Map.
 * @param x
 * X coordinate.
 * @param y
 * Y coordinate.
 * @return
 * 0 if the tile is not blocked, a combination of @ref map_look_flags
 * otherwise.
 */
int object_blocked(object *op, mapstruct *m, int x, int y) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(m != NULL);

    SOFT_ASSERT_RC(!OUT_OF_MAP(m, x, y), P_OUT_OF_MAP, "Out of map: %s %d,%d", m->path, x, y);

    op = HEAD(op);

    if (op->more == NULL) {
        return blocked(op, m, x, y, op->terrain_flag);
    }

    for (object *tmp = op; tmp != NULL; tmp = tmp->more) {
        int xt = x + tmp->arch->clone.x;
        int yt = y + tmp->arch->clone.y;
        mapstruct *map = get_map_from_coord(m, &xt, &yt);
        if (map == NULL) {
            return P_OUT_OF_MAP;
        }

        /* If this part is a different part of the head, then skip checking
         * this tile. */
        object *tmp2;
        for (tmp2 = op; tmp2 != NULL; tmp2 = tmp2->more) {
            if (tmp2->map == map && tmp2->x == xt && tmp2->y == yt) {
                break;
            }
        }

        if (tmp2 != NULL) {
            continue;
        }

        int ret = blocked(op, map, xt, yt, op->terrain_flag);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

/**
 * Creates a dummy object.
 *
 * @param name
 * Name to give the dummy object. Can be NULL.
 * @return
 * Object of specified name. It fill have the #FLAG_NO_PICK flag set.
 */
object *object_create_singularity(const char *name) {
    char buf[MAX_BUF];
    snprintf(VS(buf), "singularity");
    if (name != NULL) {
        snprintfcat(VS(buf), " (%s)", name);
    }

    object *op = object_get();
    FREE_AND_COPY_HASH(op->name, buf);
    SET_FLAG(op, FLAG_NO_PICK);
    return op;
}

/**
 * Dumps all variables in an object to a file.
 *
 * @param op
 * Object to save.
 * @param fp
 * Where to save the object's text representation. Can be NULL, in which
 * case this is a no-op.
 */
void object_save(const object *op, FILE *fp) {
    HARD_ASSERT(op != NULL);

    if (fp == NULL) {
        return;
    }

    archetype_t *at = op->arch;
    if (at == NULL) {
        at = arches[ARCH_EMPTY_ARCHETYPE];
    }

    fprintf(fp, "arch %s\n", at->name);

    StringBuffer *sb = stringbuffer_new();
    get_ob_diff(sb, op, &at->clone);

    char *cp = stringbuffer_finish(sb);
    fputs(cp, fp);
    free(cp);

    for (object *tmp = op->inv; tmp != NULL; tmp = tmp->below) {
        object_save(tmp, fp);
    }

    fprintf(fp, "end\n");
}
