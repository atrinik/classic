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

/** @file Authoritative gameplay metrics implementation. */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <metrics.h>
#include <player.h>
#include <object.h>
#include <region.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#include <ctype.h>

#define CHARACTER(scope_name, category_name, name, description, kind, unit, aggregation) \
    {scope_name,                                                                         \
     category_name,                                                                      \
     name,                                                                               \
     description,                                                                        \
     METRIC_SCOPE_CHARACTER,                                                             \
     kind,                                                                               \
     unit,                                                                               \
     METRIC_VISIBILITY_OPERATOR,                                                         \
     aggregation,                                                                        \
     METRIC_RESET_LIFETIME}
#define ACCOUNT(scope_name, category_name, name, description, kind, unit, aggregation) \
    {scope_name,                                                                       \
     category_name,                                                                    \
     name,                                                                             \
     description,                                                                      \
     METRIC_SCOPE_ACCOUNT,                                                             \
     kind,                                                                             \
     unit,                                                                             \
     METRIC_VISIBILITY_OPERATOR,                                                       \
     aggregation,                                                                      \
     METRIC_RESET_LIFETIME}

static const metric_metadata_t registry[METRIC_COUNT] = {
    [METRIC_CHARACTER_CREATED_AT] = CHARACTER(
        "lifecycle.created_at",
        "lifecycle",
        "Created at",
        "UTC character creation timestamp; backfilled from the player file when available.",
        METRIC_KIND_TIMESTAMP,
        METRIC_UNIT_SECONDS,
        METRIC_AGGREGATION_LATEST),
    [METRIC_CHARACTER_FIRST_PLAYED_AT] = CHARACTER(
        "lifecycle.first_played_at",
        "lifecycle",
        "First played",
        "UTC timestamp of the first successfully started character session observed by metrics.",
        METRIC_KIND_TIMESTAMP,
        METRIC_UNIT_SECONDS,
        METRIC_AGGREGATION_LATEST),
    [METRIC_CHARACTER_LAST_PLAYED_AT] =
        CHARACTER("lifecycle.last_played_at",
                  "lifecycle",
                  "Last played",
                  "UTC timestamp of the latest successfully started character session.",
                  METRIC_KIND_TIMESTAMP,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_LATEST),
    [METRIC_CHARACTER_SESSIONS_STARTED] =
        CHARACTER("sessions.started",
                  "sessions",
                  "Sessions started",
                  "Successful transitions of this character into the playing state.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SESSIONS_COMPLETED] =
        CHARACTER("sessions.completed",
                  "sessions",
                  "Sessions completed",
                  "Character sessions ended through the orderly logout path.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_LAST_LOGOUT_AT] =
        CHARACTER("sessions.last_logout_at",
                  "sessions",
                  "Last logout",
                  "UTC timestamp of the latest orderly character logout.",
                  METRIC_KIND_TIMESTAMP,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_LATEST),
    [METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION] =
        CHARACTER("sessions.shortest_nontrivial",
                  "sessions",
                  "Shortest nontrivial session",
                  "Shortest completed session lasting at least sixty seconds.",
                  METRIC_KIND_MINIMUM,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_MIN),
    [METRIC_CHARACTER_SESSIONS_WITH_PROGRESS] =
        CHARACTER("sessions.with_progress",
                  "sessions",
                  "Sessions with progression",
                  "Completed sessions with positive experience or a completed quest.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SESSION_DURATION] = CHARACTER(
        "sessions.total_duration",
        "sessions",
        "Total session duration",
        "Elapsed monotonic seconds across character sessions, including active and AFK time.",
        METRIC_KIND_DURATION,
        METRIC_UNIT_SECONDS,
        METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ACTIVE_PLAY_TIME] =
        CHARACTER("sessions.active_time",
                  "sessions",
                  "Active play time",
                  "Elapsed monotonic session seconds while the character was not marked AFK.",
                  METRIC_KIND_DURATION,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_AFK_TIME] =
        CHARACTER("sessions.afk_time",
                  "sessions",
                  "AFK time",
                  "Elapsed monotonic session seconds while the character was marked AFK.",
                  METRIC_KIND_DURATION,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_LONGEST_SESSION] =
        CHARACTER("sessions.longest",
                  "sessions",
                  "Longest session",
                  "Longest completed or checkpointed character session.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_LONGEST_ACTIVE_SESSION] =
        CHARACTER("sessions.longest_active",
                  "sessions",
                  "Longest active session",
                  "Most non-AFK time accumulated during one character session.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_AFK_ENTRIES] = CHARACTER("sessions.afk_entries",
                                               "sessions",
                                               "AFK entries",
                                               "Transitions from active play into AFK mode.",
                                               METRIC_KIND_COUNTER,
                                               METRIC_UNIT_COUNT,
                                               METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CURRENT_LEVEL] =
        CHARACTER("progression.current_level",
                  "progression",
                  "Current level",
                  "Snapshot of authoritative character level at the latest checkpoint.",
                  METRIC_KIND_CURRENT,
                  METRIC_UNIT_LEVEL,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_HIGHEST_LEVEL] = CHARACTER("progression.highest_level",
                                                 "progression",
                                                 "Highest level",
                                                 "Highest authoritative character level observed.",
                                                 METRIC_KIND_MAXIMUM,
                                                 METRIC_UNIT_LEVEL,
                                                 METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_LEVELS_GAINED] =
        CHARACTER("progression.levels_gained",
                  "progression",
                  "Levels gained",
                  "Positive character-level transitions observed after metrics began.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_LAST_LEVEL_GAINED_AT] =
        CHARACTER("progression.last_level_gained_at",
                  "progression",
                  "Last level gained",
                  "UTC timestamp of the latest positive character-level transition.",
                  METRIC_KIND_TIMESTAMP,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_LATEST),
    [METRIC_CHARACTER_EXPERIENCE_GAINED] =
        CHARACTER("progression.experience_gained",
                  "progression",
                  "Experience gained",
                  "Positive authoritative experience awards, including skill experience.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_EXPERIENCE,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_EXPERIENCE_LOST] =
        CHARACTER("progression.experience_lost",
                  "progression",
                  "Experience lost",
                  "Magnitude of authoritative negative experience changes.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_EXPERIENCE,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_DEATHS] = CHARACTER("combat.deaths",
                                          "combat",
                                          "Deaths",
                                          "Character deaths.",
                                          METRIC_KIND_COUNTER,
                                          METRIC_UNIT_COUNT,
                                          METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PVE_DEATHS] = CHARACTER("combat.deaths_pve",
                                              "combat",
                                              "PvE deaths",
                                              "Deaths caused by a monster or monster-owned effect.",
                                              METRIC_KIND_COUNTER,
                                              METRIC_UNIT_COUNT,
                                              METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PVP_DEATHS] =
        CHARACTER("combat.deaths_pvp",
                  "combat",
                  "PvP deaths",
                  "Deaths credited to another player or player-owned effect.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ENVIRONMENTAL_DEATHS] =
        CHARACTER("combat.deaths_environmental",
                  "combat",
                  "Environmental deaths",
                  "Deaths from starvation, traps, or another non-living source.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MONSTERS_KILLED] = CHARACTER(
        "combat.monsters_killed",
        "combat",
        "Monsters killed",
        "Non-player kills credited by the normal experience owner, including summon ownership.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_COUNT,
        METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PVP_KILLS] = CHARACTER("combat.pvp_kills",
                                             "combat",
                                             "PvP kills",
                                             "Player kills credited by the normal kill owner; not "
                                             "suitable for rewards without anti-farming policy.",
                                             METRIC_KIND_COUNTER,
                                             METRIC_UNIT_COUNT,
                                             METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_DAMAGE_DEALT] =
        CHARACTER("combat.damage_dealt",
                  "combat",
                  "Damage dealt",
                  "Effective damage after protections and overkill clamping.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_DAMAGE_TAKEN] =
        CHARACTER("combat.damage_taken",
                  "combat",
                  "Damage taken",
                  "Effective damage received after protections and overkill clamping.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_LARGEST_HIT_DEALT] =
        CHARACTER("combat.largest_hit_dealt",
                  "combat",
                  "Largest hit dealt",
                  "Largest effective single hit credited to the character.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_LARGEST_HIT_TAKEN] =
        CHARACTER("combat.largest_hit_taken",
                  "combat",
                  "Largest hit taken",
                  "Largest effective single hit received by the character.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_MELEE_ATTACKS] =
        CHARACTER("combat.melee_attacks",
                  "combat",
                  "Melee attacks",
                  "Eligible direct player attack rolls after event vetoes.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MELEE_HITS] =
        CHARACTER("combat.melee_hits",
                  "combat",
                  "Melee hits",
                  "Direct player attacks that dealt positive effective damage.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PROJECTILE_HITS] =
        CHARACTER("ranged.projectile_hits",
                  "ranged",
                  "Projectile hits",
                  "Player-owned arrows or thrown missiles that dealt positive effective damage.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PVP_DAMAGE_DEALT] =
        CHARACTER("pvp.damage_dealt",
                  "pvp",
                  "PvP damage dealt",
                  "Effective damage dealt to players in an allowed PvP area.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PVP_DAMAGE_TAKEN] =
        CHARACTER("pvp.damage_taken",
                  "pvp",
                  "PvP damage taken",
                  "Effective damage received from players in an allowed PvP area.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HP_REGENERATED] =
        CHARACTER("survival.hp_regenerated",
                  "survival",
                  "HP regenerated",
                  "Effective hit points restored by passive regeneration.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MANA_REGENERATED] =
        CHARACTER("survival.mana_regenerated",
                  "survival",
                  "Mana regenerated",
                  "Effective mana restored by passive regeneration.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_MANA_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH] =
        CHARACTER("survival.active_time_since_death",
                  "survival",
                  "Active time since death",
                  "Current non-AFK seconds accumulated since the latest death.",
                  METRIC_KIND_CURRENT,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_NONE),
    [METRIC_CHARACTER_LONGEST_ACTIVE_TIME_WITHOUT_DEATH] =
        CHARACTER("survival.longest_active_without_death",
                  "survival",
                  "Longest active time without death",
                  "Highest observed non-AFK seconds accumulated between deaths.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_TIMES_POISONED] =
        CHARACTER("survival.times_poisoned",
                  "survival",
                  "Times poisoned",
                  "Successful applications of a new poisoning effect.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_TIMES_DISEASED] = CHARACTER("survival.times_diseased",
                                                  "survival",
                                                  "Times diseased",
                                                  "Successful applications of a disease effect.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_RESPAWNS] =
        CHARACTER("survival.respawns",
                  "survival",
                  "Respawns",
                  "Death-path relocations to the character respawn point.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_DEATHS_AVOIDED] =
        CHARACTER("survival.deaths_avoided",
                  "survival",
                  "Deaths avoided",
                  "Fatal outcomes prevented by the authoritative save-life mechanic.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SAVEBEDS_BOUND] = CHARACTER("survival.savebeds_bound",
                                                  "survival",
                                                  "Savebeds bound",
                                                  "Successful respawn-point updates at a savebed.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_FOOD_POINTS_CONSUMED] =
        CHARACTER("items.food_points_consumed",
                  "items",
                  "Food points consumed",
                  "Food value consumed by successful food use.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_FOOD_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_FOOD_ITEMS_CONSUMED] = CHARACTER("items.food_items_consumed",
                                                       "items",
                                                       "Food items consumed",
                                                       "Individual food item units consumed.",
                                                       METRIC_KIND_COUNTER,
                                                       METRIC_UNIT_COUNT,
                                                       METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HEALING_SELF] =
        CHARACTER("support.healing_self",
                  "support",
                  "Self healing",
                  "Effective spell healing applied to self; overhealing excluded.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HEALING_OTHERS] =
        CHARACTER("support.healing_others",
                  "support",
                  "Healing others",
                  "Effective spell healing applied to friendly targets; overhealing excluded.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HEALING_RECEIVED] =
        CHARACTER("support.healing_received",
                  "support",
                  "Healing received",
                  "Effective spell healing received from friendly creatures.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_HIT_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_STEPS] =
        CHARACTER("movement.steps",
                  "movement",
                  "Steps",
                  "Successful voluntary movement actions; diagonal movement is one step and forced "
                  "movement, teleportation, and map transitions do not count.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MAP_TRANSITIONS] = CHARACTER(
        "movement.map_transitions",
        "movement",
        "Map transitions",
        "Successful transitions from one map to a different map after the session starts.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_COUNT,
        METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SPELLS_CAST] = CHARACTER("magic.spells_cast",
                                               "magic",
                                               "Spells cast",
                                               "Successful spell casts.",
                                               METRIC_KIND_COUNTER,
                                               METRIC_UNIT_COUNT,
                                               METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SPELLS_FAILED] =
        CHARACTER("magic.spells_failed",
                  "magic",
                  "Failed spell casts",
                  "Normal player spell attempts that did not complete successfully.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MANA_SPENT] =
        CHARACTER("magic.mana_spent",
                  "magic",
                  "Mana spent",
                  "Mana consumed by successful normal player spell casts.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_MANA_POINTS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SPELLS_LEARNED] =
        CHARACTER("magic.spells_learned",
                  "magic",
                  "Spells learned",
                  "Successful spell-learning events, including relearning a forgotten spell.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SPELLS_FORGOTTEN] =
        CHARACTER("magic.spells_forgotten",
                  "magic",
                  "Spells forgotten",
                  "Known spells removed by an authoritative forgetting effect.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CURRENT_KNOWN_SPELLS] =
        CHARACTER("magic.current_known_spells",
                  "magic",
                  "Current known spells",
                  "Current authoritative count of spell objects known by this character.",
                  METRIC_KIND_CURRENT,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_NONE),
    [METRIC_CHARACTER_HIGHEST_KNOWN_SPELLS] =
        CHARACTER("magic.highest_known_spells",
                  "magic",
                  "Highest known spell count",
                  "Highest number of spells concurrently known by this character.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_ARROWS_FIRED] = CHARACTER("ranged.arrows_fired",
                                                "ranged",
                                                "Arrows fired",
                                                "Arrow or bolt units fired.",
                                                METRIC_KIND_COUNTER,
                                                METRIC_UNIT_COUNT,
                                                METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_MISSILES_THROWN] = CHARACTER("ranged.missiles_thrown",
                                                   "ranged",
                                                   "Missiles thrown",
                                                   "Thrown missile units launched.",
                                                   METRIC_KIND_COUNTER,
                                                   METRIC_UNIT_COUNT,
                                                   METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BOOKS_READ] = CHARACTER("items.books_read",
                                              "items",
                                              "Books read",
                                              "Successful readable-book uses.",
                                              METRIC_KIND_COUNTER,
                                              METRIC_UNIT_COUNT,
                                              METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_UNIQUE_BOOKS_READ] = CHARACTER(
        "items.unique_books_read",
        "items",
        "Unique books read",
        "First-read book rewards observed; the collection is authoritative for stable IDs.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_COUNT,
        METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_POTIONS_USED] = CHARACTER("items.potions_used",
                                                "items",
                                                "Potions used",
                                                "Potion units successfully applied.",
                                                METRIC_KIND_COUNTER,
                                                METRIC_UNIT_COUNT,
                                                METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SCROLLS_USED] = CHARACTER("items.scrolls_used",
                                                "items",
                                                "Scrolls used",
                                                "Scroll units successfully applied.",
                                                METRIC_KIND_COUNTER,
                                                METRIC_UNIT_COUNT,
                                                METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ITEM_UNITS_DROPPED] =
        CHARACTER("items.units_dropped",
                  "items",
                  "Item units dropped",
                  "Individual units in successful drop operations, not stack actions.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ITEM_UNITS_PICKED_UP] =
        CHARACTER("items.units_picked_up",
                  "items",
                  "Item units picked up",
                  "Individual units in successful pickup operations, not stack actions.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CORPSES_SEARCHED] =
        CHARACTER("items.corpses_searched",
                  "items",
                  "Corpses searched",
                  "Corpse containers successfully searched for the first time by this character.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_TRAPS_FOUND] = CHARACTER("skills.traps_found",
                                               "skills",
                                               "Traps found",
                                               "Traps authoritatively revealed by find-traps.",
                                               METRIC_KIND_COUNTER,
                                               METRIC_UNIT_COUNT,
                                               METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_TRAPS_DISARMED] = CHARACTER("skills.traps_disarmed",
                                                  "skills",
                                                  "Traps disarmed",
                                                  "Successful trap disarms.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_TRAPS_SPRUNG] = CHARACTER("skills.traps_sprung",
                                                "skills",
                                                "Traps sprung",
                                                "Trap activations attributed to the character.",
                                                METRIC_KIND_COUNTER,
                                                METRIC_UNIT_COUNT,
                                                METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_POISON_CURED] =
        CHARACTER("support.poison_cured",
                  "support",
                  "Poison cured",
                  "Successful poison cures applied to another player.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_DISEASE_CURED] =
        CHARACTER("support.disease_cured",
                  "support",
                  "Disease cured",
                  "Successful disease cures applied to another player.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SHOP_PURCHASES] =
        CHARACTER("economy.shop_purchases",
                  "economy",
                  "Shop purchases",
                  "Individual item stacks successfully purchased from NPC shops.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SHOP_SALES] =
        CHARACTER("economy.shop_sales",
                  "economy",
                  "Shop sales",
                  "Individual item stacks successfully sold to NPC shops.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SHOP_CURRENCY_SPENT] =
        CHARACTER("economy.shop_currency_spent",
                  "economy",
                  "Shop currency spent",
                  "Currency removed by completed NPC shop purchases.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SHOP_CURRENCY_EARNED] =
        CHARACTER("economy.shop_currency_earned",
                  "economy",
                  "Shop currency earned",
                  "Currency created by completed NPC shop sales.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CURRENCY_SPENT] = CHARACTER(
        "economy.currency_spent",
        "economy",
        "Currency spent",
        "Currency removed by successful direct payments, including shops and scripted services.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_CURRENCY,
        METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HOUSING_PURCHASES] =
        CHARACTER("economy.housing_purchases",
                  "economy",
                  "Housing purchases",
                  "Successful scripted apartment purchases, upgrades, and luxury-house purchases.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HOUSING_FEES_PAID] =
        CHARACTER("economy.housing_fees_paid",
                  "economy",
                  "Housing fees paid",
                  "Successful scripted daily luxury-house fee payments.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HOUSING_CURRENCY_SPENT] =
        CHARACTER("economy.housing_currency_spent",
                  "economy",
                  "Housing currency spent",
                  "Currency spent on scripted housing purchases, upgrades, and recurring fees.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_AUCTION_PURCHASES] = CHARACTER("economy.auction_purchases",
                                                     "economy",
                                                     "Auction purchases",
                                                     "Successful scripted Auction House purchases.",
                                                     METRIC_KIND_COUNTER,
                                                     METRIC_UNIT_COUNT,
                                                     METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_AUCTION_LISTINGS] =
        CHARACTER("economy.auction_listings",
                  "economy",
                  "Auction listings",
                  "Items successfully listed in the scripted Auction House.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_AUCTION_CURRENCY_SPENT] =
        CHARACTER("economy.auction_currency_spent",
                  "economy",
                  "Auction currency spent",
                  "Currency removed by successful scripted Auction House purchases.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_POST_ITEMS_SENT] =
        CHARACTER("social.post_items_sent",
                  "social",
                  "Post items sent",
                  "Items successfully deposited into the scripted post office.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_POST_ITEMS_RECEIVED] =
        CHARACTER("social.post_items_received",
                  "social",
                  "Post items received",
                  "Items successfully withdrawn from the scripted post office.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_POSTAGE_CURRENCY_SPENT] =
        CHARACTER("economy.postage_currency_spent",
                  "economy",
                  "Postage currency spent",
                  "Currency removed by successful scripted post-office sends.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CONTAINERS_OPENED] =
        CHARACTER("items.containers_opened",
                  "items",
                  "Containers opened",
                  "Previously closed non-corpse containers successfully opened.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BANK_DEPOSITS] =
        CHARACTER("economy.bank_deposits",
                  "economy",
                  "Bank deposits",
                  "Successful positive-value bank deposit operations.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BANK_WITHDRAWALS] =
        CHARACTER("economy.bank_withdrawals",
                  "economy",
                  "Bank withdrawals",
                  "Successful positive-value bank withdrawal operations.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BANK_CURRENCY_DEPOSITED] =
        CHARACTER("economy.bank_currency_deposited",
                  "economy",
                  "Bank currency deposited",
                  "Currency transferred into the character bank; not currency creation.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BANK_CURRENCY_WITHDRAWN] =
        CHARACTER("economy.bank_currency_withdrawn",
                  "economy",
                  "Bank currency withdrawn",
                  "Currency transferred out of the character bank; not currency destruction.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SKILL_USES] =
        CHARACTER("skills.uses",
                  "skills",
                  "Skill uses",
                  "Explicit non-combat skill-use requests accepted for dispatch.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_SUCCESSFUL_SKILL_USES] =
        CHARACTER("skills.successful_uses",
                  "skills",
                  "Successful skill uses",
                  "Non-combat skill uses returning a positive authoritative result.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PARTIES_FORMED] = CHARACTER("social.parties_formed",
                                                  "social",
                                                  "Parties formed",
                                                  "Successful party creation actions.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PARTIES_JOINED] = CHARACTER("social.parties_joined",
                                                  "social",
                                                  "Parties joined",
                                                  "Successful party joins.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PARTY_ACTIVE_TIME] =
        CHARACTER("social.party_active_time",
                  "social",
                  "Party active time",
                  "Non-AFK session seconds while the character belongs to a party.",
                  METRIC_KIND_DURATION,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PARTY_KILLS] =
        CHARACTER("social.party_kills",
                  "social",
                  "Party kills",
                  "Monster kills credited while the character belongs to a party.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_PARTY_QUESTS_COMPLETED] =
        CHARACTER("social.party_quests_completed",
                  "social",
                  "Party quests completed",
                  "Quest completions while the character belongs to a party.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_HIGHEST_PARTY_SIZE] =
        CHARACTER("social.highest_party_size",
                  "social",
                  "Highest party size",
                  "Highest online member count observed on joining a party.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_GUILD_APPLICATIONS] =
        CHARACTER("social.guild_applications",
                  "social",
                  "Guild applications",
                  "Successful scripted guild membership applications submitted by the character.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_GUILD_DEPARTURES] =
        CHARACTER("social.guild_departures",
                  "social",
                  "Guild departures",
                  "Approved guild memberships voluntarily left by the character.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BOUNTIES_CLEARED] =
        CHARACTER("social.bounties_cleared",
                  "social",
                  "Bounties cleared",
                  "Successful scripted payments that cleared a faction bounty.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BOUNTY_CURRENCY_SPENT] =
        CHARACTER("economy.bounty_currency_spent",
                  "economy",
                  "Bounty currency spent",
                  "Currency spent through scripted services that clear faction bounties.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_CURRENCY,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_JAIL_SENTENCES] =
        CHARACTER("social.jail_sentences",
                  "social",
                  "Jail sentences",
                  "Successful scripted jail placements, including replacement sentences.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_JAIL_TIME_SENTENCED] =
        CHARACTER("social.jail_time_sentenced",
                  "social",
                  "Jail time sentenced",
                  "Finite scripted jail sentence durations assigned to the character.",
                  METRIC_KIND_DURATION,
                  METRIC_UNIT_SECONDS,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_ITEMS_RENAMED] = CHARACTER("items.items_renamed",
                                                 "items",
                                                 "Items renamed",
                                                 "Successful item rename actions.",
                                                 METRIC_KIND_COUNTER,
                                                 METRIC_UNIT_COUNT,
                                                 METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_EMOTES_USED] =
        CHARACTER("social.emotes_used",
                  "social",
                  "Emotes used",
                  "Emote command uses; message content is never retained.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_BOOKS_INSCRIBED] = CHARACTER("skills.books_inscribed",
                                                   "skills",
                                                   "Books inscribed",
                                                   "Successful inscription uses that write a book.",
                                                   METRIC_KIND_COUNTER,
                                                   METRIC_UNIT_COUNT,
                                                   METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CONSTRUCTIONS_BUILT] =
        CHARACTER("skills.constructions_built",
                  "skills",
                  "Constructions built",
                  "Successful construction-skill build operations.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_CONSTRUCTIONS_REMOVED] =
        CHARACTER("skills.constructions_removed",
                  "skills",
                  "Constructions removed",
                  "Successful construction-skill removal operations.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_QUESTS_STARTED] =
        CHARACTER("quests.started",
                  "quests",
                  "Quests started",
                  "Explicit quest starts observed after metrics began.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_QUESTS_COMPLETED] =
        CHARACTER("quests.completed",
                  "quests",
                  "Quests completed",
                  "Explicit successful quest completions; failure is never inferred as completion.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_QUESTS_FAILED] = CHARACTER("quests.failed",
                                                 "quests",
                                                 "Quests failed",
                                                 "Explicit failed quest outcomes.",
                                                 METRIC_KIND_COUNTER,
                                                 METRIC_UNIT_COUNT,
                                                 METRIC_AGGREGATION_SUM),
    [METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE] =
        CHARACTER("quests.current_active",
                  "quests",
                  "Current active quests",
                  "Current top-level quests observed in the started state.",
                  METRIC_KIND_CURRENT,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_NONE),
    [METRIC_CHARACTER_QUESTS_HIGHEST_ACTIVE] =
        CHARACTER("quests.highest_active",
                  "quests",
                  "Highest active quest count",
                  "Highest number of concurrently active top-level quests observed.",
                  METRIC_KIND_MAXIMUM,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_MAX),
    [METRIC_CHARACTER_QUESTS_REPEATABLE_COMPLETED] =
        CHARACTER("quests.repeatable_completed",
                  "quests",
                  "Repeatable quest completions",
                  "Successful completions explicitly identified by authored content as repeatable.",
                  METRIC_KIND_COUNTER,
                  METRIC_UNIT_COUNT,
                  METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_CREATED_AT] = ACCOUNT("lifecycle.created_at",
                                          "lifecycle",
                                          "Created at",
                                          "UTC account creation timestamp.",
                                          METRIC_KIND_TIMESTAMP,
                                          METRIC_UNIT_SECONDS,
                                          METRIC_AGGREGATION_LATEST),
    [METRIC_ACCOUNT_FIRST_LOGIN_AT] = ACCOUNT("lifecycle.first_login_at",
                                              "lifecycle",
                                              "First login",
                                              "UTC timestamp of the first successful account "
                                              "password authentication observed by metrics.",
                                              METRIC_KIND_TIMESTAMP,
                                              METRIC_UNIT_SECONDS,
                                              METRIC_AGGREGATION_LATEST),
    [METRIC_ACCOUNT_LAST_LOGIN_AT] =
        ACCOUNT("lifecycle.last_login_at",
                "lifecycle",
                "Last login",
                "UTC timestamp of the latest successful account password authentication.",
                METRIC_KIND_TIMESTAMP,
                METRIC_UNIT_SECONDS,
                METRIC_AGGREGATION_LATEST),
    [METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS] =
        ACCOUNT("authentication.successes",
                "authentication",
                "Successful authentications",
                "Successful account password authentications; failed credentials are security "
                "audit data and are excluded.",
                METRIC_KIND_COUNTER,
                METRIC_UNIT_COUNT,
                METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_CHARACTER_SESSIONS_STARTED] =
        ACCOUNT("sessions.character_started",
                "sessions",
                "Character sessions started",
                "Successful character playing-state transitions from this account.",
                METRIC_KIND_COUNTER,
                METRIC_UNIT_COUNT,
                METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_CHARACTER_SESSIONS_COMPLETED] =
        ACCOUNT("sessions.character_completed",
                "sessions",
                "Character sessions completed",
                "Orderly character-session completions on this account.",
                METRIC_KIND_COUNTER,
                METRIC_UNIT_COUNT,
                METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_LAST_LOGOUT_AT] =
        ACCOUNT("sessions.last_logout_at",
                "sessions",
                "Last logout",
                "UTC timestamp of the latest orderly character logout.",
                METRIC_KIND_TIMESTAMP,
                METRIC_UNIT_SECONDS,
                METRIC_AGGREGATION_LATEST),
    [METRIC_ACCOUNT_ACTIVE_PLAY_TIME] = ACCOUNT(
        "sessions.active_time",
        "sessions",
        "Active play time",
        "Sum of non-AFK character-session seconds; concurrent characters contribute independently.",
        METRIC_KIND_DURATION,
        METRIC_UNIT_SECONDS,
        METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_LONGEST_CHARACTER_SESSION] =
        ACCOUNT("sessions.longest_character",
                "sessions",
                "Longest character session",
                "Longest completed character session on the account.",
                METRIC_KIND_MAXIMUM,
                METRIC_UNIT_SECONDS,
                METRIC_AGGREGATION_MAX),
    [METRIC_ACCOUNT_CHARACTERS_CREATED] = ACCOUNT("roster.characters_created",
                                                  "roster",
                                                  "Characters created",
                                                  "Successful character roster creations.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_CHARACTERS_DELETED] = ACCOUNT("roster.characters_deleted",
                                                  "roster",
                                                  "Characters deleted",
                                                  "Successful character roster deletions.",
                                                  METRIC_KIND_COUNTER,
                                                  METRIC_UNIT_COUNT,
                                                  METRIC_AGGREGATION_SUM),
    [METRIC_ACCOUNT_CURRENT_ROSTER_SIZE] = ACCOUNT("roster.current_size",
                                                   "roster",
                                                   "Current roster size",
                                                   "Current authoritative account roster size.",
                                                   METRIC_KIND_CURRENT,
                                                   METRIC_UNIT_COUNT,
                                                   METRIC_AGGREGATION_MAX),
    [METRIC_ACCOUNT_HIGHEST_ROSTER_SIZE] =
        ACCOUNT("roster.highest_size",
                "roster",
                "Highest roster size",
                "Highest authoritative account roster size observed.",
                METRIC_KIND_MAXIMUM,
                METRIC_UNIT_COUNT,
                METRIC_AGGREGATION_MAX),
    [METRIC_ACCOUNT_HIGHEST_CHARACTER_LEVEL] =
        ACCOUNT("roster.highest_character_level",
                "roster",
                "Highest character level",
                "Highest authoritative level observed for any account character.",
                METRIC_KIND_MAXIMUM,
                METRIC_UNIT_LEVEL,
                METRIC_AGGREGATION_MAX),
};

#undef CHARACTER
#undef ACCOUNT

#define COLLECTION(save_name, category, name, description, scope) \
    {save_name,                                                   \
     category,                                                    \
     name,                                                        \
     description,                                                 \
     scope,                                                       \
     METRICS_UNIQUE_DEFAULT_LIMIT,                                \
     METRIC_VISIBILITY_OPERATOR,                                  \
     METRIC_AGGREGATION_SET_UNION,                                \
     METRIC_RESET_LIFETIME}
#define COLLECTION_LIMIT(save_name, category, name, description, scope, limit) \
    {save_name,                                                                \
     category,                                                                 \
     name,                                                                     \
     description,                                                              \
     scope,                                                                    \
     limit,                                                                    \
     METRIC_VISIBILITY_OPERATOR,                                               \
     METRIC_AGGREGATION_SET_UNION,                                             \
     METRIC_RESET_LIFETIME}

static const metric_collection_metadata_t collection_registry[METRIC_COLLECTION_COUNT] = {
    [METRIC_COLLECTION_CHARACTER_QUESTS_COMPLETED] =
        COLLECTION("quests.completed",
                   "quests",
                   "Completed quest IDs",
                   "Domain-qualified stable quest UIDs successfully completed by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_QUEST_PARTS_COMPLETED] =
        COLLECTION_LIMIT("quests.completed_parts",
                         "quests",
                         "Completed quest-part IDs",
                         "Domain-qualified stable quest and nested part-UID paths successfully "
                         "completed.",
                         METRIC_SCOPE_CHARACTER,
                         METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_CHARACTER_MAPS_VISITED] = COLLECTION_LIMIT(
        "exploration.maps",
        "exploration",
        "Visited map IDs",
        "Domain-qualified canonical map paths visited by this character; generated "
        "random-instance paths are excluded.",
        METRIC_SCOPE_CHARACTER,
        METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_CHARACTER_REGIONS_VISITED] =
        COLLECTION("exploration.regions",
                   "exploration",
                   "Visited region IDs",
                   "Domain-qualified stable internal region IDs visited by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_REGION_MAPS_DISCOVERED] =
        COLLECTION("exploration.region_maps",
                   "exploration",
                   "Discovered region-map IDs",
                   "Domain-qualified stable region IDs for region maps acquired by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_LANDMARKS_DISCOVERED] =
        COLLECTION("exploration.landmarks",
                   "exploration",
                   "Discovered landmark IDs",
                   "Explicit stable authored landmark IDs observed by scripts.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_LORE_TOPICS_DISCOVERED] =
        COLLECTION("lore.topics",
                   "lore",
                   "Discovered lore-topic IDs",
                   "Explicit stable authored lore-topic IDs observed by scripts.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_BOOKS_READ] = COLLECTION_LIMIT(
        "items.books",
        "items",
        "Read book IDs",
        "Domain-qualified artifact or fallback archetype IDs read by this character.",
        METRIC_SCOPE_CHARACTER,
        METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_CHARACTER_FOOD_ARCHETYPES_USED] =
        COLLECTION("items.food_archetypes_used",
                   "items",
                   "Food archetype IDs used",
                   "Domain-qualified food archetype IDs successfully consumed by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_POTION_ARCHETYPES_USED] =
        COLLECTION("items.potion_archetypes_used",
                   "items",
                   "Potion archetype IDs used",
                   "Domain-qualified potion archetype IDs successfully used by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_SCROLL_ARCHETYPES_USED] =
        COLLECTION("items.scroll_archetypes_used",
                   "items",
                   "Scroll archetype IDs used",
                   "Domain-qualified scroll archetype IDs successfully used by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_SPELLS_LEARNED] =
        COLLECTION("magic.spells_learned",
                   "magic",
                   "Learned spell IDs",
                   "Domain-qualified stable spell IDs ever learned by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_SKILLS_LEARNED] =
        COLLECTION("progression.skills",
                   "progression",
                   "Learned skill IDs",
                   "Domain-qualified stable skill IDs learned by this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_BOSSES_DEFEATED] =
        COLLECTION("combat.bosses",
                   "combat",
                   "Boss tags",
                   "Explicit stable boss tags credited to this character.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_SAVEBED_REGIONS] =
        COLLECTION("survival.savebed_regions",
                   "survival",
                   "Savebed region IDs",
                   "Domain-qualified region IDs in which this character bound a respawn point.",
                   METRIC_SCOPE_CHARACTER),
    [METRIC_COLLECTION_CHARACTER_ACTIVE_DAYS] =
        COLLECTION_LIMIT("sessions.active_days",
                         "sessions",
                         "Active UTC days",
                         "UTC calendar dates on which a character session started.",
                         METRIC_SCOPE_CHARACTER,
                         4096),
    [METRIC_COLLECTION_ACCOUNT_CHARACTERS_PLAYED] =
        COLLECTION("roster.characters_played",
                   "roster",
                   "Characters played",
                   "Stable character save identities played from this account.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_RACES_CREATED] =
        COLLECTION("roster.races_created",
                   "roster",
                   "Races created",
                   "Domain-qualified playable archetype IDs created on this account.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_RACES_PLAYED] =
        COLLECTION("roster.races_played",
                   "roster",
                   "Races played",
                   "Domain-qualified playable archetype IDs used in successful sessions.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_QUESTS_COMPLETED] =
        COLLECTION("discoveries.quests",
                   "discoveries",
                   "Account quest IDs",
                   "Set union of domain-qualified completed quest UIDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_QUEST_PARTS_COMPLETED] =
        COLLECTION_LIMIT("discoveries.quest_parts",
                         "discoveries",
                         "Account quest-part IDs",
                         "Set union of domain-qualified stable quest-part paths from account "
                         "characters.",
                         METRIC_SCOPE_ACCOUNT,
                         METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_ACCOUNT_MAPS_VISITED] =
        COLLECTION_LIMIT("discoveries.maps",
                         "discoveries",
                         "Account map IDs",
                         "Set union of domain-qualified canonical map paths visited by account "
                         "characters.",
                         METRIC_SCOPE_ACCOUNT,
                         METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_ACCOUNT_REGIONS_VISITED] =
        COLLECTION("discoveries.regions",
                   "discoveries",
                   "Account region IDs",
                   "Set union of domain-qualified region IDs visited by account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_REGION_MAPS_DISCOVERED] =
        COLLECTION("discoveries.region_maps",
                   "discoveries",
                   "Account region-map IDs",
                   "Set union of domain-qualified acquired region-map IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_LANDMARKS_DISCOVERED] =
        COLLECTION("discoveries.landmarks",
                   "discoveries",
                   "Account landmark IDs",
                   "Set union of explicit stable landmark IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_LORE_TOPICS_DISCOVERED] =
        COLLECTION("discoveries.lore_topics",
                   "discoveries",
                   "Account lore-topic IDs",
                   "Set union of explicit stable lore-topic IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_BOOKS_READ] =
        COLLECTION_LIMIT("discoveries.books",
                         "discoveries",
                         "Account book IDs",
                         "Set union of domain-qualified artifact or archetype book IDs read by "
                         "account characters.",
                         METRIC_SCOPE_ACCOUNT,
                         METRICS_CONTENT_ID_LIMIT),
    [METRIC_COLLECTION_ACCOUNT_FOOD_ARCHETYPES_USED] =
        COLLECTION("discoveries.food_archetypes",
                   "discoveries",
                   "Account food archetype IDs",
                   "Set union of stable consumed food archetype IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_POTION_ARCHETYPES_USED] =
        COLLECTION("discoveries.potion_archetypes",
                   "discoveries",
                   "Account potion archetype IDs",
                   "Set union of stable used potion archetype IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_SCROLL_ARCHETYPES_USED] =
        COLLECTION("discoveries.scroll_archetypes",
                   "discoveries",
                   "Account scroll archetype IDs",
                   "Set union of stable used scroll archetype IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_SPELLS_LEARNED] =
        COLLECTION("discoveries.spells",
                   "discoveries",
                   "Account spell IDs",
                   "Set union of stable learned spell IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_SKILLS_LEARNED] =
        COLLECTION("discoveries.skills",
                   "discoveries",
                   "Account skill IDs",
                   "Set union of stable learned skill IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_BOSSES_DEFEATED] =
        COLLECTION("discoveries.bosses",
                   "discoveries",
                   "Account boss tags",
                   "Set union of explicit stable boss tags credited to account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_SAVEBED_REGIONS] =
        COLLECTION("discoveries.savebed_regions",
                   "discoveries",
                   "Account savebed region IDs",
                   "Set union of stable savebed region IDs from account characters.",
                   METRIC_SCOPE_ACCOUNT),
    [METRIC_COLLECTION_ACCOUNT_ACTIVE_DAYS] =
        COLLECTION_LIMIT("sessions.active_days",
                         "sessions",
                         "Account active UTC days",
                         "Set union of UTC character-session start dates.",
                         METRIC_SCOPE_ACCOUNT,
                         4096),
};

#define KEYED_LIMIT(save_name, category, name, description, kind, unit, aggregation, limit) \
    {save_name,                                                                             \
     category,                                                                              \
     name,                                                                                  \
     description,                                                                           \
     METRIC_SCOPE_CHARACTER,                                                                \
     kind,                                                                                  \
     unit,                                                                                  \
     limit,                                                                                 \
     METRIC_VISIBILITY_OPERATOR,                                                            \
     aggregation,                                                                           \
     METRIC_RESET_LIFETIME}
#define KEYED(save_name, category, name, description, kind, unit, aggregation) \
    KEYED_LIMIT(save_name,                                                     \
                category,                                                      \
                name,                                                          \
                description,                                                   \
                kind,                                                          \
                unit,                                                          \
                aggregation,                                                   \
                METRICS_KEYED_DEFAULT_LIMIT)

static const metric_keyed_metadata_t keyed_registry[METRIC_KEYED_COUNT] = {
    [METRIC_KEYED_CHARACTER_QUEST_STARTS] =
        KEYED("quests.starts_by_id",
              "quests",
              "Quest starts by ID",
              "Top-level quest starts grouped by domain-qualified stable quest UID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS] = KEYED(
        "quests.completions_by_id",
        "quests",
        "Quest completions by ID",
        "Successful top-level quest completions grouped by domain-qualified stable quest UID, "
        "including repeats.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_COUNT,
        METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_QUEST_PART_COMPLETIONS] = KEYED_LIMIT(
        "quests.part_completions_by_id",
        "quests",
        "Quest-part completions by ID",
        "Successful quest-part completions grouped by domain-qualified stable quest and "
        "nested part-UID path.",
        METRIC_KIND_COUNTER,
        METRIC_UNIT_COUNT,
        METRIC_AGGREGATION_SUM,
        METRICS_CONTENT_ID_LIMIT),
    [METRIC_KEYED_CHARACTER_QUEST_FAILURES] =
        KEYED("quests.failures_by_id",
              "quests",
              "Quest failures by ID",
              "Failed top-level quest outcomes grouped by domain-qualified stable quest UID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_MONSTER_KILLS] =
        KEYED_LIMIT("combat.monster_kills_by_archetype",
                    "combat",
                    "Monster kills by archetype",
                    "Credited monster kills grouped by domain-qualified monster archetype ID.",
                    METRIC_KIND_COUNTER,
                    METRIC_UNIT_COUNT,
                    METRIC_AGGREGATION_SUM,
                    METRICS_CONTENT_ID_LIMIT),
    [METRIC_KEYED_CHARACTER_MONSTER_KILLS_BY_FAMILY] =
        KEYED("combat.monster_kills_by_family",
              "combat",
              "Monster kills by family",
              "Credited monster kills grouped by domain-qualified stable monster-family ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL] =
        KEYED("progression.skill_current_level",
              "progression",
              "Current skill levels",
              "Current authoritative skill level grouped by domain-qualified stable skill ID.",
              METRIC_KIND_CURRENT,
              METRIC_UNIT_LEVEL,
              METRIC_AGGREGATION_NONE),
    [METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL] =
        KEYED("progression.skill_highest_level",
              "progression",
              "Highest skill levels",
              "Highest observed skill level grouped by domain-qualified stable skill ID.",
              METRIC_KIND_MAXIMUM,
              METRIC_UNIT_LEVEL,
              METRIC_AGGREGATION_MAX),
    [METRIC_KEYED_CHARACTER_SKILL_EXPERIENCE_GAINED] =
        KEYED("progression.skill_experience_gained",
              "progression",
              "Skill experience gained",
              "Positive experience awards grouped by domain-qualified stable skill ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_EXPERIENCE,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SKILL_EXPERIENCE_LOST] =
        KEYED("progression.skill_experience_lost",
              "progression",
              "Skill experience lost",
              "Magnitude of negative experience changes grouped by domain-qualified stable skill "
              "ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_EXPERIENCE,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SKILL_LEVELS_GAINED] =
        KEYED("progression.skill_levels_gained",
              "progression",
              "Skill levels gained",
              "Positive skill-level transitions grouped by domain-qualified stable skill ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SKILL_USES] =
        KEYED("skills.uses_by_id",
              "skills",
              "Skill uses by ID",
              "Eligible skill-use requests grouped by domain-qualified stable skill ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SKILL_SUCCESSFUL_USES] =
        KEYED("skills.successful_uses_by_id",
              "skills",
              "Successful skill uses by ID",
              "Successful skill uses grouped by domain-qualified stable skill ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_CONSTRUCTIONS_BUILT] =
        KEYED_LIMIT("skills.constructions_built_by_archetype",
                    "skills",
                    "Constructions built by archetype",
                    "Successful construction builds grouped by domain-qualified result archetype "
                    "ID.",
                    METRIC_KIND_COUNTER,
                    METRIC_UNIT_COUNT,
                    METRIC_AGGREGATION_SUM,
                    METRICS_CONTENT_ID_LIMIT),
    [METRIC_KEYED_CHARACTER_CONSTRUCTIONS_REMOVED] =
        KEYED_LIMIT("skills.constructions_removed_by_archetype",
                    "skills",
                    "Constructions removed by archetype",
                    "Successful construction removals grouped by domain-qualified archetype ID.",
                    METRIC_KIND_COUNTER,
                    METRIC_UNIT_COUNT,
                    METRIC_AGGREGATION_SUM,
                    METRICS_CONTENT_ID_LIMIT),
    [METRIC_KEYED_CHARACTER_BOUNTIES_CLEARED] =
        KEYED("social.bounties_cleared_by_faction",
              "social",
              "Bounties cleared by faction",
              "Successful bounty clears grouped by domain-qualified stable faction ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SPELLS_CAST] =
        KEYED("magic.spells_cast_by_id",
              "magic",
              "Spell casts by ID",
              "Successful normal spell casts grouped by domain-qualified stable spell ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_SPELLS_FAILED] =
        KEYED("magic.spells_failed_by_id",
              "magic",
              "Failed spell casts by ID",
              "Failed normal spell attempts grouped by domain-qualified stable spell ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_COUNT,
              METRIC_AGGREGATION_SUM),
    [METRIC_KEYED_CHARACTER_MANA_SPENT_BY_SPELL] =
        KEYED("magic.mana_spent_by_spell",
              "magic",
              "Mana spent by spell",
              "Mana consumed by successful normal casts grouped by domain-qualified stable spell "
              "ID.",
              METRIC_KIND_COUNTER,
              METRIC_UNIT_MANA_POINTS,
              METRIC_AGGREGATION_SUM),
};

#undef KEYED
#undef KEYED_LIMIT

#undef COLLECTION
#undef COLLECTION_LIMIT

const metric_metadata_t *metrics_metadata(metric_id_t id) {
    return id >= 0 && id < METRIC_COUNT ? &registry[id] : NULL;
}

const metric_collection_metadata_t *metrics_collection_metadata(metric_collection_id_t id) {
    return id >= 0 && id < METRIC_COLLECTION_COUNT ? &collection_registry[id] : NULL;
}

const metric_keyed_metadata_t *metrics_keyed_metadata(metric_keyed_id_t id) {
    return id >= 0 && id < METRIC_KEYED_COUNT ? &keyed_registry[id] : NULL;
}

const char *metrics_scope_name(metric_scope_t scope) {
    return scope == METRIC_SCOPE_CHARACTER ? "character" : "account";
}

const char *metrics_unit_name(metric_unit_t unit) {
    static const char *const names[] = {"count",
                                        "seconds",
                                        "hit points",
                                        "experience",
                                        "food points",
                                        "level",
                                        "mana points",
                                        "currency"};
    return unit >= 0 && (size_t)unit < arraysize(names) ? names[unit] : "unknown";
}

static uint64_t saturating_add(uint64_t lhs, uint64_t rhs) {
    return UINT64_MAX - lhs < rhs ? UINT64_MAX : lhs + rhs;
}

void metrics_store_init(metric_store_t *store, metric_scope_t scope, uint64_t epoch) {
    HARD_ASSERT(store != NULL);
    memset(store, 0, sizeof(*store));
    store->scope = scope;
    store->epoch = epoch;
}

void metrics_store_free(metric_store_t *store) {
    HARD_ASSERT(store != NULL);
    for (size_t collection = 0; collection < METRIC_COLLECTION_COUNT; collection++) {
        for (size_t entry = 0; entry < store->collections[collection].count; entry++) {
            free(store->collections[collection].ids[entry]);
        }
        free(store->collections[collection].ids);
    }
    for (size_t keyed = 0; keyed < METRIC_KEYED_COUNT; keyed++) {
        for (size_t entry = 0; entry < store->keyed[keyed].count; entry++) {
            free(store->keyed[keyed].entries[entry].id);
        }
        free(store->keyed[keyed].entries);
    }
    for (size_t line = 0; line < store->opaque_lines_count; line++) {
        free(store->opaque_lines[line]);
    }
    free(store->opaque_lines);
    memset(store, 0, sizeof(*store));
}

void metrics_store_move(metric_store_t *destination, metric_store_t *source) {
    HARD_ASSERT(destination != NULL);
    HARD_ASSERT(source != NULL);
    metrics_store_free(destination);
    memcpy(destination, source, sizeof(*destination));
    memset(source, 0, sizeof(*source));
}

static bool metric_matches(const metric_store_t *store, metric_id_t id) {
    const metric_metadata_t *metadata = metrics_metadata(id);
    return metadata != NULL && metadata->scope == store->scope;
}

uint64_t metrics_get(const metric_store_t *store, metric_id_t id) {
    HARD_ASSERT(store != NULL);
    return metric_matches(store, id) ? store->values[id] : 0;
}

bool metrics_add(metric_store_t *store, metric_id_t id, uint64_t amount) {
    HARD_ASSERT(store != NULL);
    if (!metric_matches(store, id) || registry[id].kind == METRIC_KIND_MAXIMUM ||
        registry[id].kind == METRIC_KIND_MINIMUM || registry[id].kind == METRIC_KIND_CURRENT ||
        registry[id].kind == METRIC_KIND_TIMESTAMP) {
        return false;
    }
    uint64_t value = saturating_add(store->values[id], amount);
    if (value != store->values[id]) {
        store->values[id] = value;
        store->dirty = true;
    }
    return true;
}

bool metrics_set(metric_store_t *store, metric_id_t id, uint64_t value) {
    HARD_ASSERT(store != NULL);
    if (!metric_matches(store, id)) {
        return false;
    }
    if (store->values[id] != value) {
        store->values[id] = value;
        store->dirty = true;
    }
    return true;
}

bool metrics_update_max(metric_store_t *store, metric_id_t id, uint64_t candidate) {
    HARD_ASSERT(store != NULL);
    if (!metric_matches(store, id) || registry[id].kind != METRIC_KIND_MAXIMUM) {
        return false;
    }
    if (candidate > store->values[id]) {
        store->values[id] = candidate;
        store->dirty = true;
    }
    return true;
}

bool metrics_update_min(metric_store_t *store, metric_id_t id, uint64_t candidate) {
    HARD_ASSERT(store != NULL);
    if (!metric_matches(store, id) || registry[id].kind != METRIC_KIND_MINIMUM) {
        return false;
    }
    if (candidate != 0 && (store->values[id] == 0 || candidate < store->values[id])) {
        store->values[id] = candidate;
        store->dirty = true;
    }
    return true;
}

static bool unique_id_valid(const char *subject_id) {
    if (subject_id == NULL || *subject_id == '\0' ||
        strnlen(subject_id, METRICS_UNIQUE_ID_MAX + 1) > METRICS_UNIQUE_ID_MAX) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)subject_id; *cp != '\0'; cp++) {
        bool alphanumeric =
            (*cp >= 'a' && *cp <= 'z') || (*cp >= 'A' && *cp <= 'Z') || (*cp >= '0' && *cp <= '9');
        if (!alphanumeric && strchr("_./:-+", *cp) == NULL) {
            return false;
        }
    }
    return true;
}

bool metrics_format_content_id(char *buffer, size_t size, const char *domain, const char *key) {
    if (buffer == NULL || size == 0 || domain == NULL || *domain == '\0' || key == NULL ||
        *key == '\0') {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)domain; *cp != '\0'; cp++) {
        bool lowercase = *cp >= 'a' && *cp <= 'z';
        bool digit = cp != (const unsigned char *)domain && *cp >= '0' && *cp <= '9';
        bool hyphen = cp != (const unsigned char *)domain && *cp == '-';
        if (!lowercase && !digit && !hyphen) {
            return false;
        }
    }
    int length = snprintf(buffer, size, "%s:%s", domain, key);
    return length > 0 && (size_t)length < size && unique_id_valid(buffer);
}

static bool collection_matches(const metric_store_t *store, metric_collection_id_t id) {
    const metric_collection_metadata_t *metadata = metrics_collection_metadata(id);
    return metadata != NULL && metadata->scope == store->scope;
}

static bool unique_find(const metric_unique_set_t *set, const char *subject_id, size_t *position) {
    size_t left = 0;
    size_t right = set->count;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        int comparison = strcmp(set->ids[middle], subject_id);
        if (comparison < 0) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }
    *position = left;
    return left < set->count && strcmp(set->ids[left], subject_id) == 0;
}

bool metrics_has_unique(const metric_store_t *store,
                        metric_collection_id_t id,
                        const char *subject_id) {
    HARD_ASSERT(store != NULL);
    if (!collection_matches(store, id) || !unique_id_valid(subject_id)) {
        return false;
    }
    size_t position;
    return unique_find(&store->collections[id], subject_id, &position);
}

bool metrics_mark_unique(metric_store_t *store, metric_collection_id_t id, const char *subject_id) {
    HARD_ASSERT(store != NULL);
    if (!collection_matches(store, id) || !unique_id_valid(subject_id)) {
        return false;
    }
    metric_unique_set_t *set = &store->collections[id];
    size_t position;
    if (unique_find(set, subject_id, &position)) {
        return true;
    }
    if (set->count >= collection_registry[id].limit) {
        return false;
    }
    set->ids = xreallocarray(set->ids, set->count + 1, sizeof(*set->ids));
    memmove(&set->ids[position + 1],
            &set->ids[position],
            (set->count - position) * sizeof(*set->ids));
    set->ids[position] = xstrdup(subject_id);
    set->count++;
    store->dirty = true;
    return true;
}

size_t metrics_unique_count(const metric_store_t *store, metric_collection_id_t id) {
    HARD_ASSERT(store != NULL);
    return collection_matches(store, id) ? store->collections[id].count : 0;
}

static bool keyed_matches(const metric_store_t *store, metric_keyed_id_t id) {
    const metric_keyed_metadata_t *metadata = metrics_keyed_metadata(id);
    return metadata != NULL && metadata->scope == store->scope;
}

static bool keyed_find(const metric_keyed_map_t *map, const char *subject_id, size_t *position) {
    size_t left = 0;
    size_t right = map->count;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        int comparison = strcmp(map->entries[middle].id, subject_id);
        if (comparison < 0) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }
    *position = left;
    return left < map->count && strcmp(map->entries[left].id, subject_id) == 0;
}

uint64_t
metrics_keyed_get(const metric_store_t *store, metric_keyed_id_t id, const char *subject_id) {
    HARD_ASSERT(store != NULL);
    if (!keyed_matches(store, id) || !unique_id_valid(subject_id)) {
        return 0;
    }
    size_t position;
    return keyed_find(&store->keyed[id], subject_id, &position)
               ? store->keyed[id].entries[position].value
               : 0;
}

size_t metrics_keyed_count(const metric_store_t *store, metric_keyed_id_t id) {
    HARD_ASSERT(store != NULL);
    return keyed_matches(store, id) ? store->keyed[id].count : 0;
}

static bool metrics_keyed_store(metric_store_t *store,
                                metric_keyed_id_t id,
                                const char *subject_id,
                                uint64_t value) {
    if (!keyed_matches(store, id) || !unique_id_valid(subject_id)) {
        return false;
    }
    metric_keyed_map_t *map = &store->keyed[id];
    size_t position;
    bool found = keyed_find(map, subject_id, &position);
    if (value == 0) {
        if (found) {
            free(map->entries[position].id);
            memmove(&map->entries[position],
                    &map->entries[position + 1],
                    (map->count - position - 1) * sizeof(*map->entries));
            map->count--;
            store->dirty = true;
        }
        return true;
    }
    if (found) {
        if (map->entries[position].value != value) {
            map->entries[position].value = value;
            store->dirty = true;
        }
        return true;
    }
    if (map->count >= keyed_registry[id].limit) {
        return false;
    }
    map->entries = xreallocarray(map->entries, map->count + 1, sizeof(*map->entries));
    memmove(&map->entries[position + 1],
            &map->entries[position],
            (map->count - position) * sizeof(*map->entries));
    map->entries[position].id = xstrdup(subject_id);
    map->entries[position].value = value;
    map->count++;
    store->dirty = true;
    return true;
}

bool metrics_keyed_add(metric_store_t *store,
                       metric_keyed_id_t id,
                       const char *subject_id,
                       uint64_t amount) {
    HARD_ASSERT(store != NULL);
    if (!keyed_matches(store, id) || keyed_registry[id].kind != METRIC_KIND_COUNTER ||
        !unique_id_valid(subject_id)) {
        return false;
    }
    uint64_t current = metrics_keyed_get(store, id, subject_id);
    uint64_t value = saturating_add(current, amount);
    return amount == 0 || metrics_keyed_store(store, id, subject_id, value);
}

bool metrics_keyed_set(metric_store_t *store,
                       metric_keyed_id_t id,
                       const char *subject_id,
                       uint64_t value) {
    HARD_ASSERT(store != NULL);
    if (!keyed_matches(store, id) || keyed_registry[id].kind != METRIC_KIND_CURRENT) {
        return false;
    }
    return metrics_keyed_store(store, id, subject_id, value);
}

bool metrics_keyed_update_max(metric_store_t *store,
                              metric_keyed_id_t id,
                              const char *subject_id,
                              uint64_t candidate) {
    HARD_ASSERT(store != NULL);
    if (!keyed_matches(store, id) || keyed_registry[id].kind != METRIC_KIND_MAXIMUM ||
        !unique_id_valid(subject_id)) {
        return false;
    }
    return candidate <= metrics_keyed_get(store, id, subject_id) ||
           metrics_keyed_store(store, id, subject_id, candidate);
}

static metric_id_t metric_by_name(metric_scope_t scope, const char *name) {
    for (metric_id_t id = 0; id < METRIC_COUNT; id++) {
        if (registry[id].scope == scope && strcmp(registry[id].save_name, name) == 0) {
            return id;
        }
    }
    return METRIC_COUNT;
}

static metric_collection_id_t collection_by_name(metric_scope_t scope, const char *name) {
    for (metric_collection_id_t id = 0; id < METRIC_COLLECTION_COUNT; id++) {
        if (collection_registry[id].scope == scope &&
            strcmp(collection_registry[id].save_name, name) == 0) {
            return id;
        }
    }
    return METRIC_COLLECTION_COUNT;
}

static metric_keyed_id_t keyed_by_name(metric_scope_t scope, const char *name) {
    for (metric_keyed_id_t id = 0; id < METRIC_KEYED_COUNT; id++) {
        if (keyed_registry[id].scope == scope && strcmp(keyed_registry[id].save_name, name) == 0) {
            return id;
        }
    }
    return METRIC_KEYED_COUNT;
}

static bool parse_uint64(const char *text, uint64_t *value) {
    if (text == NULL || *text == '\0' || *text == '-' || isspace((unsigned char)*text)) {
        return false;
    }
    errno = 0;
    char *end;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno == ERANGE || parsed > UINT64_MAX || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool preserve_opaque_line(metric_store_t *store, const char *line) {
    if (store->opaque_lines_count >= METRICS_OPAQUE_LINE_LIMIT || strlen(line) >= 1024) {
        return false;
    }
    store->opaque_lines = xreallocarray(store->opaque_lines,
                                        store->opaque_lines_count + 1,
                                        sizeof(*store->opaque_lines));
    store->opaque_lines[store->opaque_lines_count++] = xstrdup(line);
    return true;
}

bool metrics_parse_line(metric_store_t *store, const char *line, bool *recognized) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(line != NULL);
    HARD_ASSERT(recognized != NULL);
    *recognized = true;

    if (strncmp(line, "metrics_version ", 16) == 0) {
        uint64_t version;
        return parse_uint64(line + 16, &version) && version == METRICS_SCHEMA_VERSION;
    }
    if (strncmp(line, "metrics_epoch ", 14) == 0) {
        return parse_uint64(line + 14, &store->epoch);
    }
    if (strncmp(line, "metric ", 7) == 0) {
        char name[METRICS_UNIQUE_ID_MAX + 1];
        char value_string[32];
        char extra;
        if (sscanf(line + 7, "%255s %31s %c", name, value_string, &extra) != 2) {
            return false;
        }
        metric_id_t id = metric_by_name(store->scope, name);
        if (id == METRIC_COUNT) {
            return preserve_opaque_line(store, line);
        }
        uint64_t value;
        if (!parse_uint64(value_string, &value)) {
            return false;
        }
        store->values[id] = value;
        return true;
    }
    if (strncmp(line, "unique ", 7) == 0) {
        char name[METRICS_UNIQUE_ID_MAX + 1];
        char subject[METRICS_UNIQUE_ID_MAX + 1];
        char extra;
        if (sscanf(line + 7, "%255s %255s %c", name, subject, &extra) != 2) {
            return false;
        }
        metric_collection_id_t id = collection_by_name(store->scope, name);
        if (id == METRIC_COLLECTION_COUNT) {
            return preserve_opaque_line(store, line);
        }
        return metrics_mark_unique(store, id, subject);
    }
    if (strncmp(line, "keyed ", 6) == 0) {
        char name[METRICS_UNIQUE_ID_MAX + 1];
        char subject[METRICS_UNIQUE_ID_MAX + 1];
        char value_string[32];
        char extra;
        if (sscanf(line + 6, "%255s %255s %31s %c", name, subject, value_string, &extra) != 3) {
            return false;
        }
        metric_keyed_id_t id = keyed_by_name(store->scope, name);
        if (id == METRIC_KEYED_COUNT) {
            return preserve_opaque_line(store, line);
        }
        uint64_t value;
        if (!parse_uint64(value_string, &value)) {
            return false;
        }
        return metrics_keyed_store(store, id, subject, value);
    }

    *recognized = false;
    return true;
}

void metrics_append(StringBuffer *buffer, const metric_store_t *store) {
    HARD_ASSERT(buffer != NULL);
    HARD_ASSERT(store != NULL);
    stringbuffer_append_printf(buffer, "metrics_version %d\n", METRICS_SCHEMA_VERSION);
    stringbuffer_append_printf(buffer, "metrics_epoch %" PRIu64 "\n", store->epoch);
    for (metric_id_t id = 0; id < METRIC_COUNT; id++) {
        if (registry[id].scope == store->scope && store->values[id] != 0) {
            stringbuffer_append_printf(buffer,
                                       "metric %s %" PRIu64 "\n",
                                       registry[id].save_name,
                                       store->values[id]);
        }
    }
    for (metric_collection_id_t id = 0; id < METRIC_COLLECTION_COUNT; id++) {
        if (collection_registry[id].scope != store->scope) {
            continue;
        }
        for (size_t entry = 0; entry < store->collections[id].count; entry++) {
            stringbuffer_append_printf(buffer,
                                       "unique %s %s\n",
                                       collection_registry[id].save_name,
                                       store->collections[id].ids[entry]);
        }
    }
    for (metric_keyed_id_t id = 0; id < METRIC_KEYED_COUNT; id++) {
        if (keyed_registry[id].scope != store->scope) {
            continue;
        }
        for (size_t entry = 0; entry < store->keyed[id].count; entry++) {
            stringbuffer_append_printf(buffer,
                                       "keyed %s %s %" PRIu64 "\n",
                                       keyed_registry[id].save_name,
                                       store->keyed[id].entries[entry].id,
                                       store->keyed[id].entries[entry].value);
        }
    }
    for (size_t line = 0; line < store->opaque_lines_count; line++) {
        stringbuffer_append_printf(buffer, "%s\n", store->opaque_lines[line]);
    }
}

bool metrics_load_file(metric_store_t *store, const char *path) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(path != NULL);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return errno == ENOENT;
    }
    metric_store_t parsed;
    metrics_store_init(&parsed, store->scope, store->epoch);
    bool version_seen = false;
    bool valid = true;
    char line[1024];
    while (fgets(VS(line), fp) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            valid = false;
            break;
        }
        string_strip_newline(line);
        bool recognized;
        if (!metrics_parse_line(&parsed, line, &recognized) || !recognized) {
            valid = false;
            break;
        }
        version_seen |= strncmp(line, "metrics_version ", 16) == 0;
    }
    bool stream_error = ferror(fp) != 0;
    if (fclose(fp) == EOF || stream_error) {
        valid = false;
    }
    if (!valid || !version_seen) {
        LOG(ERROR, "Invalid metrics file preserved without loading: %s", path);
        metrics_store_free(&parsed);
        return false;
    }
    parsed.dirty = false;
    metrics_store_move(store, &parsed);
    return true;
}

bool metrics_save_file(metric_store_t *store, const char *path) {
    HARD_ASSERT(store != NULL);
    HARD_ASSERT(path != NULL);
    StringBuffer *buffer = stringbuffer_new();
    metrics_append(buffer, store);
    char *contents = stringbuffer_finish(buffer);
    bool ok = path_write_atomic(path, contents, strlen(contents), 0600);
    free(contents);
    if (ok) {
        store->dirty = false;
    } else {
        LOG(ERROR, "Could not atomically replace metrics file: %s", path);
    }
    return ok;
}

static uint64_t wall_now(void) {
    server_wall_utc_t now = server_wall_utc_now();
    return now.seconds > 0 ? (uint64_t)now.seconds : 0;
}

void metrics_character_load(player *pl) {
    HARD_ASSERT(pl != NULL);
    char *path = player_make_path(pl->ob->name, "metrics.dat");
    if (!metrics_load_file(&pl->metrics, path)) {
        pl->metrics_load_failed = true;
        draw_info(COLOR_RED,
                  pl->ob,
                  "Your metrics could not be loaded; previous data was preserved.");
    }
    free(path);
}

bool metrics_character_save(player *pl) {
    HARD_ASSERT(pl != NULL);
    if (pl->metrics_load_failed) {
        return false;
    }
    metrics_character_session_checkpoint(pl);
    if (!pl->metrics.dirty) {
        return true;
    }
    char *path = player_make_path(pl->ob->name, "metrics.dat");
    path_ensure_directories(path);
    bool ok = metrics_save_file(&pl->metrics, path);
    free(path);
    return ok;
}

static uint64_t
elapsed_whole_seconds(server_monotonic_t now, server_monotonic_t since, uint64_t *remainder) {
    uint64_t elapsed = server_monotonic_difference(now, since).microseconds;
    elapsed = saturating_add(elapsed, *remainder);
    uint64_t seconds = elapsed / UINT64_C(1000000);
    *remainder = elapsed == UINT64_MAX ? 0 : elapsed % UINT64_C(1000000);
    return seconds;
}

static void checkpoint_segment(player *pl, server_monotonic_t now) {
    if (!server_monotonic_is_set(pl->metrics_segment_started)) {
        pl->metrics_segment_started = now;
        return;
    }
    if (pl->metrics_segment_afk) {
        metrics_add(
            &pl->metrics,
            METRIC_CHARACTER_AFK_TIME,
            elapsed_whole_seconds(now, pl->metrics_segment_started, &pl->metrics_afk_remainder_us));
    } else {
        uint64_t seconds = elapsed_whole_seconds(now,
                                                 pl->metrics_segment_started,
                                                 &pl->metrics_active_remainder_us);
        metrics_add(&pl->metrics, METRIC_CHARACTER_ACTIVE_PLAY_TIME, seconds);
        uint64_t survival = metrics_get(&pl->metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH);
        survival = saturating_add(survival, seconds);
        metrics_set(&pl->metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH, survival);
        metrics_update_max(&pl->metrics,
                           METRIC_CHARACTER_LONGEST_ACTIVE_TIME_WITHOUT_DEATH,
                           survival);
        if (pl->party != NULL) {
            metrics_add(&pl->metrics,
                        METRIC_CHARACTER_PARTY_ACTIVE_TIME,
                        elapsed_whole_seconds(now,
                                              pl->metrics_segment_started,
                                              &pl->metrics_party_remainder_us));
        }
        pl->metrics_session_active_seconds =
            saturating_add(pl->metrics_session_active_seconds, seconds);
    }
    pl->metrics_segment_started = now;
}

void metrics_character_session_start(player *pl) {
    HARD_ASSERT(pl != NULL);
    uint64_t now = wall_now();
    if (metrics_get(&pl->metrics, METRIC_CHARACTER_FIRST_PLAYED_AT) == 0) {
        metrics_set(&pl->metrics, METRIC_CHARACTER_FIRST_PLAYED_AT, now);
    }
    metrics_set(&pl->metrics, METRIC_CHARACTER_LAST_PLAYED_AT, now);
    metrics_add(&pl->metrics, METRIC_CHARACTER_SESSIONS_STARTED, 1);
    time_t started = (time_t)now;
    struct tm utc;
    if (gmtime_r(&started, &utc) != NULL) {
        char day[16];
        if (strftime(VS(day), "%Y-%m-%d", &utc) != 0) {
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_ACTIVE_DAYS, day);
        }
    }
    pl->metrics_session_started = server_monotonic_now();
    pl->metrics_segment_started = pl->metrics_session_started;
    pl->metrics_session_active_seconds = 0;
    pl->metrics_session_accounted_seconds = 0;
    pl->metrics_active_remainder_us = 0;
    pl->metrics_afk_remainder_us = 0;
    pl->metrics_party_remainder_us = 0;
    pl->metrics_session_progressed = false;
    pl->metrics_segment_afk = pl->afk;
}

void metrics_character_session_checkpoint(player *pl) {
    HARD_ASSERT(pl != NULL);
    if (!server_monotonic_is_set(pl->metrics_session_started)) {
        return;
    }
    server_monotonic_t now = server_monotonic_now();
    checkpoint_segment(pl, now);
    uint64_t session_seconds =
        server_monotonic_difference(now, pl->metrics_session_started).microseconds /
        UINT64_C(1000000);
    if (session_seconds > pl->metrics_session_accounted_seconds) {
        metrics_add(&pl->metrics,
                    METRIC_CHARACTER_SESSION_DURATION,
                    session_seconds - pl->metrics_session_accounted_seconds);
        pl->metrics_session_accounted_seconds = session_seconds;
    }
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_LONGEST_SESSION, session_seconds);
    metrics_update_max(&pl->metrics,
                       METRIC_CHARACTER_LONGEST_ACTIVE_SESSION,
                       pl->metrics_session_active_seconds);
    metrics_set(&pl->metrics, METRIC_CHARACTER_CURRENT_LEVEL, MAX(0, pl->ob->level));
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_HIGHEST_LEVEL, MAX(0, pl->ob->level));
}

void metrics_character_session_end(player *pl) {
    HARD_ASSERT(pl != NULL);
    if (!server_monotonic_is_set(pl->metrics_session_started)) {
        return;
    }
    metrics_character_session_checkpoint(pl);
    uint64_t session_seconds = pl->metrics_session_accounted_seconds;
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_LONGEST_SESSION, session_seconds);
    metrics_update_max(&pl->metrics,
                       METRIC_CHARACTER_LONGEST_ACTIVE_SESSION,
                       pl->metrics_session_active_seconds);
    pl->metrics_completed_session_seconds = session_seconds;
    metrics_add(&pl->metrics, METRIC_CHARACTER_SESSIONS_COMPLETED, 1);
    metrics_set(&pl->metrics, METRIC_CHARACTER_LAST_LOGOUT_AT, wall_now());
    if (session_seconds >= 60) {
        metrics_update_min(&pl->metrics,
                           METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION,
                           session_seconds);
    }
    if (pl->metrics_session_progressed) {
        metrics_add(&pl->metrics, METRIC_CHARACTER_SESSIONS_WITH_PROGRESS, 1);
    }
    pl->metrics_session_started = (server_monotonic_t){0};
    pl->metrics_segment_started = (server_monotonic_t){0};
}

void metrics_character_party_changed(player *pl) {
    HARD_ASSERT(pl != NULL);
    metrics_character_session_checkpoint(pl);
}

void metrics_character_progressed(player *pl) {
    HARD_ASSERT(pl != NULL);
    pl->metrics_session_progressed = true;
}

void metrics_character_death(player *pl, bool pvp, bool environmental) {
    HARD_ASSERT(pl != NULL);
    metrics_character_session_checkpoint(pl);
    metrics_add(&pl->metrics, METRIC_CHARACTER_DEATHS, 1);
    if (pvp) {
        metrics_add(&pl->metrics, METRIC_CHARACTER_PVP_DEATHS, 1);
    } else if (environmental) {
        metrics_add(&pl->metrics, METRIC_CHARACTER_ENVIRONMENTAL_DEATHS, 1);
    } else {
        metrics_add(&pl->metrics, METRIC_CHARACTER_PVE_DEATHS, 1);
    }
    metrics_set(&pl->metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH, 0);
}

void metrics_character_afk_changed(player *pl, bool afk) {
    HARD_ASSERT(pl != NULL);
    if (pl->metrics_segment_afk == afk) {
        return;
    }
    checkpoint_segment(pl, server_monotonic_now());
    pl->metrics_segment_afk = afk;
    if (afk) {
        metrics_add(&pl->metrics, METRIC_CHARACTER_AFK_ENTRIES, 1);
    }
}

void metrics_character_visit(player *pl, mapstruct *map, bool transition) {
    HARD_ASSERT(pl != NULL);
    if (map == NULL || map->path == NULL) {
        return;
    }
    if (transition) {
        metrics_add(&pl->metrics, METRIC_CHARACTER_MAP_TRANSITIONS, 1);
    }
    if (strncmp(map->path, "/random/", 8) != 0) {
        char id[METRICS_UNIQUE_ID_MAX + 1];
        if (metrics_format_content_id(VS(id), "map", map->path)) {
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_MAPS_VISITED, id);
        }
    }
    if (map->region != NULL && map->region->name != NULL) {
        char id[METRICS_UNIQUE_ID_MAX + 1];
        if (metrics_format_content_id(VS(id), "region", map->region->name)) {
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, id);
        }
    }
}

bool metrics_character_quest_status(player *pl, const char *quest_uid, int status) {
    HARD_ASSERT(pl != NULL);
    char id[METRICS_UNIQUE_ID_MAX + 1];
    if (!metrics_format_content_id(VS(id), "quest", quest_uid)) {
        return false;
    }
    switch (status) {
        case QUEST_STATUS_STARTED: {
            metrics_keyed_add(&pl->metrics, METRIC_KEYED_CHARACTER_QUEST_STARTS, id, 1);
            metrics_add(&pl->metrics, METRIC_CHARACTER_QUESTS_STARTED, 1);
            uint64_t active = metrics_get(&pl->metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE);
            active = saturating_add(active, 1);
            metrics_set(&pl->metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE, active);
            metrics_update_max(&pl->metrics, METRIC_CHARACTER_QUESTS_HIGHEST_ACTIVE, active);
            return true;
        }
        case QUEST_STATUS_COMPLETED:
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_QUESTS_COMPLETED, id);
            metrics_keyed_add(&pl->metrics, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, id, 1);
            metrics_character_progressed(pl);
            if (pl->party != NULL) {
                metrics_add(&pl->metrics, METRIC_CHARACTER_PARTY_QUESTS_COMPLETED, 1);
            }
            metrics_add(&pl->metrics, METRIC_CHARACTER_QUESTS_COMPLETED, 1);
            break;
        case QUEST_STATUS_FAILED:
            metrics_keyed_add(&pl->metrics, METRIC_KEYED_CHARACTER_QUEST_FAILURES, id, 1);
            metrics_add(&pl->metrics, METRIC_CHARACTER_QUESTS_FAILED, 1);
            break;
        default:
            return false;
    }

    uint64_t active = metrics_get(&pl->metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE);
    if (active != 0) {
        metrics_set(&pl->metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE, active - 1);
    }
    return true;
}

bool metrics_character_add_by_name(player *pl, const char *save_name, uint64_t amount) {
    HARD_ASSERT(pl != NULL);
    if (save_name == NULL) {
        return false;
    }
    metric_id_t id = metric_by_name(METRIC_SCOPE_CHARACTER, save_name);
    return id != METRIC_COUNT && metrics_add(&pl->metrics, id, amount);
}

bool metrics_character_keyed_add_by_name(player *pl,
                                         const char *save_name,
                                         const char *subject_id,
                                         uint64_t amount) {
    HARD_ASSERT(pl != NULL);
    if (save_name == NULL) {
        return false;
    }
    metric_keyed_id_t id = keyed_by_name(METRIC_SCOPE_CHARACTER, save_name);
    if (id < 0 || id >= METRIC_KEYED_COUNT || keyed_registry[id].kind != METRIC_KIND_COUNTER ||
        !unique_id_valid(subject_id)) {
        return false;
    }
    return metrics_keyed_add(&pl->metrics, id, subject_id, amount) ||
           pl->metrics.keyed[id].count >= keyed_registry[id].limit;
}

bool metrics_character_mark_unique_by_name(player *pl,
                                           const char *save_name,
                                           const char *subject_id) {
    HARD_ASSERT(pl != NULL);
    if (save_name == NULL) {
        return false;
    }
    metric_collection_id_t id = collection_by_name(METRIC_SCOPE_CHARACTER, save_name);
    if (id < 0 || id >= METRIC_COLLECTION_COUNT || !unique_id_valid(subject_id)) {
        return false;
    }
    return metrics_mark_unique(&pl->metrics, id, subject_id) ||
           pl->metrics.collections[id].count >= collection_registry[id].limit;
}

void metrics_character_spells_changed(player *pl) {
    HARD_ASSERT(pl != NULL);
    uint64_t count = 0;
    for (object *tmp = pl->ob->inv; tmp != NULL; tmp = tmp->below) {
        if (tmp->type != SPELL) {
            continue;
        }
        const char *spell_id = spell_id_from_index(tmp->stats.sp);
        char id[METRICS_UNIQUE_ID_MAX + 1];
        if (spell_id != NULL && metrics_format_content_id(VS(id), "spell", spell_id)) {
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_SPELLS_LEARNED, id);
            count++;
        }
    }
    metrics_set(&pl->metrics, METRIC_CHARACTER_CURRENT_KNOWN_SPELLS, count);
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_HIGHEST_KNOWN_SPELLS, count);
}

void metrics_character_backfill(player *pl) {
    HARD_ASSERT(pl != NULL);
    uint64_t level = MAX(0, pl->ob->level);
    metrics_set(&pl->metrics, METRIC_CHARACTER_CURRENT_LEVEL, level);
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_HIGHEST_LEVEL, level);
    metrics_character_spells_changed(pl);
    for (object *tmp = pl->ob->inv; tmp != NULL; tmp = tmp->below) {
        if (tmp->type == REGION_MAP && tmp->name != NULL) {
            char id[METRICS_UNIQUE_ID_MAX + 1];
            if (metrics_format_content_id(VS(id), "region", tmp->name)) {
                metrics_mark_unique(&pl->metrics,
                                    METRIC_COLLECTION_CHARACTER_REGION_MAPS_DISCOVERED,
                                    id);
            }
        }
    }
    for (int skill = 0; skill < NROFSKILLS; skill++) {
        if (pl->skill_ptr[skill] != NULL) {
            const char *skill_id = skill_id_from_index(skill);
            char id[METRICS_UNIQUE_ID_MAX + 1];
            if (skill_id == NULL || !metrics_format_content_id(VS(id), "skill", skill_id)) {
                continue;
            }
            metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_SKILLS_LEARNED, id);
            uint64_t skill_level = MAX(0, pl->skill_ptr[skill]->level);
            metrics_keyed_set(&pl->metrics,
                              METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL,
                              id,
                              skill_level);
            metrics_keyed_update_max(&pl->metrics,
                                     METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL,
                                     id,
                                     skill_level);
        }
    }
    uint64_t active = 0;
    if (pl->quest_container != NULL) {
        for (object *quest = pl->quest_container->inv; quest != NULL; quest = quest->below) {
            if (quest->magic == QUEST_STATUS_COMPLETED && quest->name != NULL) {
                char id[METRICS_UNIQUE_ID_MAX + 1];
                if (!metrics_format_content_id(VS(id), "quest", quest->name)) {
                    continue;
                }
                metrics_mark_unique(&pl->metrics, METRIC_COLLECTION_CHARACTER_QUESTS_COMPLETED, id);
                if (metrics_keyed_get(&pl->metrics, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, id) ==
                    0) {
                    metrics_keyed_add(&pl->metrics,
                                      METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS,
                                      id,
                                      1);
                }
            } else if (quest->magic == QUEST_STATUS_STARTED) {
                active++;
            }
        }
    }
    metrics_set(&pl->metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE, active);
    metrics_update_max(&pl->metrics, METRIC_CHARACTER_QUESTS_HIGHEST_ACTIVE, active);
}
