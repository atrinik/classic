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

/** @file Authoritative gameplay metrics registry and storage API. */

#ifndef METRICS_H
#define METRICS_H

#include <decls.h>
#include <server_clock.h>
#include <toolkit/stringbuffer.h>

#define METRICS_SCHEMA_VERSION 1
#define METRICS_UNIQUE_ID_MAX 255
#define METRICS_UNIQUE_DEFAULT_LIMIT 512
#define METRICS_KEYED_DEFAULT_LIMIT 512
#define METRICS_CONTENT_ID_LIMIT 8192
#define METRICS_OPAQUE_LINE_LIMIT 128

typedef enum metric_scope {
    METRIC_SCOPE_CHARACTER,
    METRIC_SCOPE_ACCOUNT
} metric_scope_t;

typedef enum metric_kind {
    METRIC_KIND_COUNTER,
    METRIC_KIND_MAXIMUM,
    METRIC_KIND_MINIMUM,
    METRIC_KIND_CURRENT,
    METRIC_KIND_TIMESTAMP,
    METRIC_KIND_DURATION
} metric_kind_t;

typedef enum metric_unit {
    METRIC_UNIT_COUNT,
    METRIC_UNIT_SECONDS,
    METRIC_UNIT_HIT_POINTS,
    METRIC_UNIT_EXPERIENCE,
    METRIC_UNIT_FOOD_POINTS,
    METRIC_UNIT_LEVEL,
    METRIC_UNIT_MANA_POINTS,
    METRIC_UNIT_CURRENCY
} metric_unit_t;

typedef enum metric_visibility {
    METRIC_VISIBILITY_OPERATOR,
    METRIC_VISIBILITY_ANALYTICS,
    METRIC_VISIBILITY_INTERNAL
} metric_visibility_t;

typedef enum metric_aggregation {
    METRIC_AGGREGATION_SUM,
    METRIC_AGGREGATION_MIN,
    METRIC_AGGREGATION_MAX,
    METRIC_AGGREGATION_LATEST,
    METRIC_AGGREGATION_SET_UNION,
    METRIC_AGGREGATION_NONE
} metric_aggregation_t;

typedef enum metric_reset_policy {
    METRIC_RESET_LIFETIME
} metric_reset_policy_t;

typedef enum metric_id {
    METRIC_CHARACTER_CREATED_AT,
    METRIC_CHARACTER_FIRST_PLAYED_AT,
    METRIC_CHARACTER_LAST_PLAYED_AT,
    METRIC_CHARACTER_SESSIONS_STARTED,
    METRIC_CHARACTER_SESSIONS_COMPLETED,
    METRIC_CHARACTER_LAST_LOGOUT_AT,
    METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION,
    METRIC_CHARACTER_SESSIONS_WITH_PROGRESS,
    METRIC_CHARACTER_SESSION_DURATION,
    METRIC_CHARACTER_ACTIVE_PLAY_TIME,
    METRIC_CHARACTER_AFK_TIME,
    METRIC_CHARACTER_LONGEST_SESSION,
    METRIC_CHARACTER_LONGEST_ACTIVE_SESSION,
    METRIC_CHARACTER_AFK_ENTRIES,
    METRIC_CHARACTER_CURRENT_LEVEL,
    METRIC_CHARACTER_HIGHEST_LEVEL,
    METRIC_CHARACTER_LEVELS_GAINED,
    METRIC_CHARACTER_LAST_LEVEL_GAINED_AT,
    METRIC_CHARACTER_EXPERIENCE_GAINED,
    METRIC_CHARACTER_EXPERIENCE_LOST,
    METRIC_CHARACTER_DEATHS,
    METRIC_CHARACTER_PVE_DEATHS,
    METRIC_CHARACTER_PVP_DEATHS,
    METRIC_CHARACTER_ENVIRONMENTAL_DEATHS,
    METRIC_CHARACTER_MONSTERS_KILLED,
    METRIC_CHARACTER_PVP_KILLS,
    METRIC_CHARACTER_DAMAGE_DEALT,
    METRIC_CHARACTER_DAMAGE_TAKEN,
    METRIC_CHARACTER_LARGEST_HIT_DEALT,
    METRIC_CHARACTER_LARGEST_HIT_TAKEN,
    METRIC_CHARACTER_MELEE_ATTACKS,
    METRIC_CHARACTER_MELEE_HITS,
    METRIC_CHARACTER_PROJECTILE_HITS,
    METRIC_CHARACTER_PVP_DAMAGE_DEALT,
    METRIC_CHARACTER_PVP_DAMAGE_TAKEN,
    METRIC_CHARACTER_HP_REGENERATED,
    METRIC_CHARACTER_MANA_REGENERATED,
    METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH,
    METRIC_CHARACTER_LONGEST_ACTIVE_TIME_WITHOUT_DEATH,
    METRIC_CHARACTER_TIMES_POISONED,
    METRIC_CHARACTER_TIMES_DISEASED,
    METRIC_CHARACTER_RESPAWNS,
    METRIC_CHARACTER_DEATHS_AVOIDED,
    METRIC_CHARACTER_SAVEBEDS_BOUND,
    METRIC_CHARACTER_FOOD_POINTS_CONSUMED,
    METRIC_CHARACTER_FOOD_ITEMS_CONSUMED,
    METRIC_CHARACTER_HEALING_SELF,
    METRIC_CHARACTER_HEALING_OTHERS,
    METRIC_CHARACTER_HEALING_RECEIVED,
    METRIC_CHARACTER_STEPS,
    METRIC_CHARACTER_MAP_TRANSITIONS,
    METRIC_CHARACTER_SPELLS_CAST,
    METRIC_CHARACTER_SPELLS_FAILED,
    METRIC_CHARACTER_MANA_SPENT,
    METRIC_CHARACTER_SPELLS_LEARNED,
    METRIC_CHARACTER_SPELLS_FORGOTTEN,
    METRIC_CHARACTER_CURRENT_KNOWN_SPELLS,
    METRIC_CHARACTER_HIGHEST_KNOWN_SPELLS,
    METRIC_CHARACTER_ARROWS_FIRED,
    METRIC_CHARACTER_MISSILES_THROWN,
    METRIC_CHARACTER_BOOKS_READ,
    METRIC_CHARACTER_UNIQUE_BOOKS_READ,
    METRIC_CHARACTER_POTIONS_USED,
    METRIC_CHARACTER_SCROLLS_USED,
    METRIC_CHARACTER_ITEM_UNITS_DROPPED,
    METRIC_CHARACTER_ITEM_UNITS_PICKED_UP,
    METRIC_CHARACTER_CORPSES_SEARCHED,
    METRIC_CHARACTER_TRAPS_FOUND,
    METRIC_CHARACTER_TRAPS_DISARMED,
    METRIC_CHARACTER_TRAPS_SPRUNG,
    METRIC_CHARACTER_POISON_CURED,
    METRIC_CHARACTER_DISEASE_CURED,
    METRIC_CHARACTER_SHOP_PURCHASES,
    METRIC_CHARACTER_SHOP_SALES,
    METRIC_CHARACTER_SHOP_CURRENCY_SPENT,
    METRIC_CHARACTER_SHOP_CURRENCY_EARNED,
    METRIC_CHARACTER_CURRENCY_SPENT,
    METRIC_CHARACTER_HOUSING_PURCHASES,
    METRIC_CHARACTER_HOUSING_FEES_PAID,
    METRIC_CHARACTER_HOUSING_CURRENCY_SPENT,
    METRIC_CHARACTER_AUCTION_PURCHASES,
    METRIC_CHARACTER_AUCTION_LISTINGS,
    METRIC_CHARACTER_AUCTION_CURRENCY_SPENT,
    METRIC_CHARACTER_POST_ITEMS_SENT,
    METRIC_CHARACTER_POST_ITEMS_RECEIVED,
    METRIC_CHARACTER_POSTAGE_CURRENCY_SPENT,
    METRIC_CHARACTER_CONTAINERS_OPENED,
    METRIC_CHARACTER_BANK_DEPOSITS,
    METRIC_CHARACTER_BANK_WITHDRAWALS,
    METRIC_CHARACTER_BANK_CURRENCY_DEPOSITED,
    METRIC_CHARACTER_BANK_CURRENCY_WITHDRAWN,
    METRIC_CHARACTER_SKILL_USES,
    METRIC_CHARACTER_SUCCESSFUL_SKILL_USES,
    METRIC_CHARACTER_PARTIES_FORMED,
    METRIC_CHARACTER_PARTIES_JOINED,
    METRIC_CHARACTER_PARTY_ACTIVE_TIME,
    METRIC_CHARACTER_PARTY_KILLS,
    METRIC_CHARACTER_PARTY_QUESTS_COMPLETED,
    METRIC_CHARACTER_HIGHEST_PARTY_SIZE,
    METRIC_CHARACTER_GUILD_APPLICATIONS,
    METRIC_CHARACTER_GUILD_DEPARTURES,
    METRIC_CHARACTER_BOUNTIES_CLEARED,
    METRIC_CHARACTER_BOUNTY_CURRENCY_SPENT,
    METRIC_CHARACTER_JAIL_SENTENCES,
    METRIC_CHARACTER_JAIL_TIME_SENTENCED,
    METRIC_CHARACTER_ITEMS_RENAMED,
    METRIC_CHARACTER_EMOTES_USED,
    METRIC_CHARACTER_BOOKS_INSCRIBED,
    METRIC_CHARACTER_CONSTRUCTIONS_BUILT,
    METRIC_CHARACTER_CONSTRUCTIONS_REMOVED,
    METRIC_CHARACTER_QUESTS_STARTED,
    METRIC_CHARACTER_QUESTS_COMPLETED,
    METRIC_CHARACTER_QUESTS_FAILED,
    METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE,
    METRIC_CHARACTER_QUESTS_HIGHEST_ACTIVE,
    METRIC_CHARACTER_QUESTS_REPEATABLE_COMPLETED,
    METRIC_ACCOUNT_CREATED_AT,
    METRIC_ACCOUNT_FIRST_LOGIN_AT,
    METRIC_ACCOUNT_LAST_LOGIN_AT,
    METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS,
    METRIC_ACCOUNT_CHARACTER_SESSIONS_STARTED,
    METRIC_ACCOUNT_CHARACTER_SESSIONS_COMPLETED,
    METRIC_ACCOUNT_LAST_LOGOUT_AT,
    METRIC_ACCOUNT_ACTIVE_PLAY_TIME,
    METRIC_ACCOUNT_LONGEST_CHARACTER_SESSION,
    METRIC_ACCOUNT_CHARACTERS_CREATED,
    METRIC_ACCOUNT_CHARACTERS_DELETED,
    METRIC_ACCOUNT_CURRENT_ROSTER_SIZE,
    METRIC_ACCOUNT_HIGHEST_ROSTER_SIZE,
    METRIC_ACCOUNT_HIGHEST_CHARACTER_LEVEL,
    METRIC_COUNT
} metric_id_t;

typedef enum metric_collection_id {
    METRIC_COLLECTION_CHARACTER_QUESTS_COMPLETED,
    METRIC_COLLECTION_CHARACTER_QUEST_PARTS_COMPLETED,
    METRIC_COLLECTION_CHARACTER_MAPS_VISITED,
    METRIC_COLLECTION_CHARACTER_REGIONS_VISITED,
    METRIC_COLLECTION_CHARACTER_REGION_MAPS_DISCOVERED,
    METRIC_COLLECTION_CHARACTER_LANDMARKS_DISCOVERED,
    METRIC_COLLECTION_CHARACTER_LORE_TOPICS_DISCOVERED,
    METRIC_COLLECTION_CHARACTER_BOOKS_READ,
    METRIC_COLLECTION_CHARACTER_FOOD_ARCHETYPES_USED,
    METRIC_COLLECTION_CHARACTER_POTION_ARCHETYPES_USED,
    METRIC_COLLECTION_CHARACTER_SCROLL_ARCHETYPES_USED,
    METRIC_COLLECTION_CHARACTER_SPELLS_LEARNED,
    METRIC_COLLECTION_CHARACTER_SKILLS_LEARNED,
    METRIC_COLLECTION_CHARACTER_BOSSES_DEFEATED,
    METRIC_COLLECTION_CHARACTER_SAVEBED_REGIONS,
    METRIC_COLLECTION_CHARACTER_ACTIVE_DAYS,
    METRIC_COLLECTION_ACCOUNT_CHARACTERS_PLAYED,
    METRIC_COLLECTION_ACCOUNT_RACES_CREATED,
    METRIC_COLLECTION_ACCOUNT_RACES_PLAYED,
    METRIC_COLLECTION_ACCOUNT_QUESTS_COMPLETED,
    METRIC_COLLECTION_ACCOUNT_QUEST_PARTS_COMPLETED,
    METRIC_COLLECTION_ACCOUNT_MAPS_VISITED,
    METRIC_COLLECTION_ACCOUNT_REGIONS_VISITED,
    METRIC_COLLECTION_ACCOUNT_REGION_MAPS_DISCOVERED,
    METRIC_COLLECTION_ACCOUNT_LANDMARKS_DISCOVERED,
    METRIC_COLLECTION_ACCOUNT_LORE_TOPICS_DISCOVERED,
    METRIC_COLLECTION_ACCOUNT_BOOKS_READ,
    METRIC_COLLECTION_ACCOUNT_FOOD_ARCHETYPES_USED,
    METRIC_COLLECTION_ACCOUNT_POTION_ARCHETYPES_USED,
    METRIC_COLLECTION_ACCOUNT_SCROLL_ARCHETYPES_USED,
    METRIC_COLLECTION_ACCOUNT_SPELLS_LEARNED,
    METRIC_COLLECTION_ACCOUNT_SKILLS_LEARNED,
    METRIC_COLLECTION_ACCOUNT_BOSSES_DEFEATED,
    METRIC_COLLECTION_ACCOUNT_SAVEBED_REGIONS,
    METRIC_COLLECTION_ACCOUNT_ACTIVE_DAYS,
    METRIC_COLLECTION_COUNT
} metric_collection_id_t;

typedef enum metric_keyed_id {
    METRIC_KEYED_CHARACTER_QUEST_STARTS,
    METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS,
    METRIC_KEYED_CHARACTER_QUEST_PART_COMPLETIONS,
    METRIC_KEYED_CHARACTER_QUEST_FAILURES,
    METRIC_KEYED_CHARACTER_MONSTER_KILLS,
    METRIC_KEYED_CHARACTER_MONSTER_KILLS_BY_FAMILY,
    METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL,
    METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL,
    METRIC_KEYED_CHARACTER_SKILL_EXPERIENCE_GAINED,
    METRIC_KEYED_CHARACTER_SKILL_EXPERIENCE_LOST,
    METRIC_KEYED_CHARACTER_SKILL_LEVELS_GAINED,
    METRIC_KEYED_CHARACTER_SKILL_USES,
    METRIC_KEYED_CHARACTER_SKILL_SUCCESSFUL_USES,
    METRIC_KEYED_CHARACTER_CONSTRUCTIONS_BUILT,
    METRIC_KEYED_CHARACTER_CONSTRUCTIONS_REMOVED,
    METRIC_KEYED_CHARACTER_BOUNTIES_CLEARED,
    METRIC_KEYED_CHARACTER_SPELLS_CAST,
    METRIC_KEYED_CHARACTER_SPELLS_FAILED,
    METRIC_KEYED_CHARACTER_MANA_SPENT_BY_SPELL,
    METRIC_KEYED_COUNT
} metric_keyed_id_t;

typedef struct metric_metadata {
    const char *save_name;
    const char *category;
    const char *name;
    const char *description;
    metric_scope_t scope;
    metric_kind_t kind;
    metric_unit_t unit;
    metric_visibility_t visibility;
    metric_aggregation_t aggregation;
    metric_reset_policy_t reset_policy;
} metric_metadata_t;

typedef struct metric_collection_metadata {
    const char *save_name;
    const char *category;
    const char *name;
    const char *description;
    metric_scope_t scope;
    size_t limit;
    metric_visibility_t visibility;
    metric_aggregation_t aggregation;
    metric_reset_policy_t reset_policy;
} metric_collection_metadata_t;

typedef struct metric_keyed_metadata {
    const char *save_name;
    const char *category;
    const char *name;
    const char *description;
    metric_scope_t scope;
    metric_kind_t kind;
    metric_unit_t unit;
    size_t limit;
    metric_visibility_t visibility;
    metric_aggregation_t aggregation;
    metric_reset_policy_t reset_policy;
} metric_keyed_metadata_t;

typedef struct metric_unique_set {
    char **ids;
    size_t count;
} metric_unique_set_t;

typedef struct metric_keyed_entry {
    char *id;
    uint64_t value;
} metric_keyed_entry_t;

typedef struct metric_keyed_map {
    metric_keyed_entry_t *entries;
    size_t count;
} metric_keyed_map_t;

typedef struct metric_store {
    metric_scope_t scope;
    uint64_t epoch;
    uint64_t values[METRIC_COUNT];
    metric_unique_set_t collections[METRIC_COLLECTION_COUNT];
    metric_keyed_map_t keyed[METRIC_KEYED_COUNT];
    char **opaque_lines;
    size_t opaque_lines_count;
    bool dirty;
} metric_store_t;

const metric_metadata_t *metrics_metadata(metric_id_t id);
const metric_collection_metadata_t *metrics_collection_metadata(metric_collection_id_t id);
const metric_keyed_metadata_t *metrics_keyed_metadata(metric_keyed_id_t id);
const char *metrics_scope_name(metric_scope_t scope);
const char *metrics_unit_name(metric_unit_t unit);
bool metrics_format_content_id(char *buffer, size_t size, const char *domain, const char *key);

void metrics_store_init(metric_store_t *store, metric_scope_t scope, uint64_t epoch);
void metrics_store_free(metric_store_t *store);
void metrics_store_move(metric_store_t *destination, metric_store_t *source);
uint64_t metrics_get(const metric_store_t *store, metric_id_t id);
bool metrics_add(metric_store_t *store, metric_id_t id, uint64_t amount);
bool metrics_set(metric_store_t *store, metric_id_t id, uint64_t value);
bool metrics_update_max(metric_store_t *store, metric_id_t id, uint64_t candidate);
bool metrics_update_min(metric_store_t *store, metric_id_t id, uint64_t candidate);
bool metrics_mark_unique(metric_store_t *store, metric_collection_id_t id, const char *subject_id);
bool metrics_has_unique(const metric_store_t *store,
                        metric_collection_id_t id,
                        const char *subject_id);
size_t metrics_unique_count(const metric_store_t *store, metric_collection_id_t id);
uint64_t
metrics_keyed_get(const metric_store_t *store, metric_keyed_id_t id, const char *subject_id);
size_t metrics_keyed_count(const metric_store_t *store, metric_keyed_id_t id);
bool metrics_keyed_add(metric_store_t *store,
                       metric_keyed_id_t id,
                       const char *subject_id,
                       uint64_t amount);
bool metrics_keyed_set(metric_store_t *store,
                       metric_keyed_id_t id,
                       const char *subject_id,
                       uint64_t value);
bool metrics_keyed_update_max(metric_store_t *store,
                              metric_keyed_id_t id,
                              const char *subject_id,
                              uint64_t candidate);

bool metrics_parse_line(metric_store_t *store, const char *line, bool *recognized);
void metrics_append(StringBuffer *buffer, const metric_store_t *store);
bool metrics_load_file(metric_store_t *store, const char *path);
bool metrics_save_file(metric_store_t *store, const char *path);

void metrics_character_load(player *pl);
bool metrics_character_save(player *pl);
void metrics_character_session_start(player *pl);
void metrics_character_session_checkpoint(player *pl);
void metrics_character_session_end(player *pl);
void metrics_character_afk_changed(player *pl, bool afk);
void metrics_character_party_changed(player *pl);
void metrics_character_progressed(player *pl);
void metrics_character_death(player *pl, bool pvp, bool environmental);
void metrics_character_visit(player *pl, mapstruct *map, bool transition);
bool metrics_character_quest_status(player *pl, const char *quest_uid, int status);
bool metrics_character_add_by_name(player *pl, const char *save_name, uint64_t amount);
bool metrics_character_keyed_add_by_name(player *pl,
                                         const char *save_name,
                                         const char *subject_id,
                                         uint64_t amount);
bool metrics_character_mark_unique_by_name(player *pl,
                                           const char *save_name,
                                           const char *subject_id);
void metrics_character_spells_changed(player *pl);
void metrics_character_backfill(player *pl);

#endif
