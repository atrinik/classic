/*************************************************************************
 * Atrinik server projectile impact regression tests.                    *
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <metrics.h>
#include <monster_data.h>
#include <movement.h>
#include <object_methods.h>
#include <player.h>
#include <skills.h>
#include <toolkit/packet.h>

#define TEST_BASE_DAMAGE 24

START_TEST(test_invalid_projectile_directions_stop_before_lookup) {
    static const int invalid_directions[] = {INT8_MIN, 0, NUM_DIRECTION + 1, INT8_MAX};
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    for (size_t i = 0; i < arraysize(invalid_directions); i++) {
        object *arrow = arch_get("arrow");
        arrow->x = pl->x;
        arrow->y = pl->y;
        arrow->direction = invalid_directions[i];
        arrow->last_sp = 2;
        SET_FLAG(arrow, FLAG_IS_MISSILE);
        object_owner_set(arrow, pl);
        arrow = object_insert_map(arrow, map, NULL, INS_NO_MERGE);
        tag_t count = arrow->count;

        if (i % 2 == 0) {
            common_object_projectile_process(arrow);
        } else {
            ck_assert_ptr_null(common_object_projectile_move(arrow));
        }

        if (OBJECT_VALID(arrow, count)) {
            if (arrow->map != NULL || arrow->env != NULL) {
                object_remove(arrow, 0);
            }
            object_destroy(arrow);
        }
    }
}
END_TEST

static object *projectile_test_target(mapstruct *map, object *pl) {
    if (map->path == NULL) {
        FREE_AND_COPY_HASH(map->path, "/tests/projectile");
    }

    object *target = arch_get("kobold");
    target->x = pl->x + 1;
    target->y = pl->y;
    target = object_insert_map(target, map, NULL, INS_NO_MERGE);
    monster_data_init(target);
    target = HEAD(target);
    target->stats.hp = 1000;
    target->stats.maxhp = 1000;
    target->block = 0;
    target->absorb = 0;
    memset(target->protection, 0, sizeof(target->protection));
    return target;
}

static object *projectile_test_skill(object *pl, enum skillnrs skill_id) {
    const char *arch_name;
    switch (skill_id) {
        case SK_THROWING:
            arch_name = "skill_throwing";
            break;

        case SK_WIZARDRY_SPELLS:
            arch_name = "skill_wizardry_spells";
            break;

        default:
            arch_name = "skill_bow_archery";
            break;
    }
    object *skill = arch_get(arch_name);
    skill->stats.sp = skill_id;
    pl->chosen_skill = skill;
    return skill;
}

static object *projectile_test_arrow(mapstruct *map,
                                     object *pl,
                                     object *target,
                                     int direction,
                                     enum skillnrs skill_id) {
    projectile_test_skill(pl, skill_id);

    object *arrow = arch_get("arrow");
    arrow->x = target->x;
    arrow->y = target->y;
    arrow->stats.dam = TEST_BASE_DAMAGE;
    arrow->stats.hp = arrow->arch->clone.stats.dam;
    arrow->speed = 1.0f;
    arrow->direction = direction;
    memset(arrow->attack, 0, sizeof(arrow->attack));
    arrow->attack[ATNR_PIERCE] = 100;
    SET_FLAG(arrow, FLAG_FLYING);
    SET_FLAG(arrow, FLAG_IS_MISSILE);
    object_owner_set(arrow, pl);
    return object_insert_map(arrow, map, NULL, INS_NO_MERGE);
}

static size_t projectile_test_bonus_messages(object *pl, const char *expected) {
    size_t count = 0;

    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }

        packet_reader_t reader;
        char color[64];
        char message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        ck_assert(packet_reader_read_string(&reader, VS(color)));
        ck_assert(packet_reader_read_string(&reader, VS(message)));
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);

        bool bonus_message =
            strncmp(message, "Archery damage bonus:", strlen("Archery damage bonus:")) == 0 ||
            strncmp(message, "Sneak damage bonus:", strlen("Sneak damage bonus:")) == 0;
        if (bonus_message) {
            if (expected == NULL || strcmp(message, expected) == 0) {
                count++;
            }
        }
    }

    return count;
}

static int projectile_test_hit(object *arrow, object *target) {
    target->last_damage = 0;
    target->damage_round_tag = global_round_tag;
    ck_assert_int_eq(object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    return target->last_damage;
}

START_TEST(test_archery_impact_bonuses_and_feedback) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    object *target = projectile_test_target(map, pl);

    /* Launch the arrows before either actor turns. The projectile's travel
     * direction and the target's new impact-time facing control the bonus;
     * the owner's new facing is irrelevant. */
    pl->direction = 7;
    target->direction = 5;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    object *following_arrow = projectile_test_arrow(map, pl, target, 3, SK_CROSSBOW_ARCHERY);
    pl->direction = 1;
    target->direction = 3;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 36);
    ck_assert_ptr_eq(arrow->env, target);
    ck_assert(OBJECT_VALID(target->enemy, target->enemy_count));
    ck_assert_ptr_eq(target->enemy, pl);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(
        projectile_test_bonus_messages(
            pl,
            "Archery damage bonus: +50% (+12 base damage) — rear shot, unaware target."),
        1);

    /* A second already-in-flight arrow sees the first impact's aggro
     * transition and gets rear only. */
    ck_assert_int_eq(projectile_test_hit(following_arrow, target), 30);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 2);
    ck_assert_uint_eq(
        projectile_test_bonus_messages(pl,
                                       "Archery damage bonus: +25% (+6 base damage) — rear shot."),
        1);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 2);
}
END_TEST

START_TEST(test_archery_unaware_state_is_impact_time_and_validated) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    object *target = projectile_test_target(map, pl);

    /* Stale enemy and pending-attacker references do not make the target alert. */
    object *stale = arch_get("goblin");
    target->enemy = stale;
    target->enemy_count = stale->count + 1;
    target->attacked_by = stale;
    target->attacked_by_count = stale->count + 1;
    target->direction = 1;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_SLING_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);
    ck_assert_uint_eq(projectile_test_bonus_messages(
                          pl,
                          "Archery damage bonus: +25% (+6 base damage) — unaware target."),
                      1);

    /* Engagement with another player makes a fresh target alert. */
    object *other_player = player_get_dummy("Other archer", NULL);
    target = projectile_test_target(map, pl);
    target->direction = 1;
    target->enemy = other_player;
    target->enemy_count = other_player->count;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    /* A valid pending attacker makes a fresh target alert, even without enemy. */
    target = projectile_test_target(map, pl);
    target->direction = 1;
    target->attacked_by = pl;
    target->attacked_by_count = pl->count;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    object_destroy(stale);
}
END_TEST

START_TEST(test_archery_invisibility_and_target_ownership_exclusions) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    SET_MULTI_FLAG(pl, FLAG_IS_INVISIBLE);
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);
    ck_assert(!OBJECT_VALID(target->enemy, target->enemy_count));
    ck_assert_uint_eq(
        projectile_test_bonus_messages(pl,
                                       "Archery damage bonus: +25% (+6 base damage) — rear shot."),
        1);
    CLEAR_MULTI_FLAG(pl, FLAG_IS_INVISIBLE);

    target = projectile_test_target(map, pl);
    object_owner_set(target, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(object_projectile_hit(arrow, target), OBJECT_METHOD_UNHANDLED);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    object *npc = arch_get("goblin");
    npc->x = pl->x;
    npc->y = pl->y + 1;
    npc = object_insert_map(npc, map, NULL, INS_NO_MERGE);
    monster_data_init(npc);
    npc->enemy = target;
    npc->enemy_count = target->count;
    object_owner_set(npc, pl);
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    object_owner_set(arrow, npc);
    ck_assert_ptr_null(arrow->chosen_skill);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->type = BULLET;
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    SET_FLAG(arrow, FLAG_IS_SPELL);
    object_owner_set(arrow, npc);
    ck_assert_ptr_null(arrow->chosen_skill);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    object *other_player = player_get_dummy("Projectile target", NULL);
    object_remove(other_player, 0);
    other_player->x = pl->x + 1;
    other_player->y = pl->y;
    other_player->stats.hp = 1000;
    other_player->stats.maxhp = 1000;
    other_player->block = 0;
    memset(other_player->protection, 0, sizeof(other_player->protection));
    other_player = object_insert_map(other_player, map, NULL, INS_NO_MERGE);
    map->map_flags |= MAP_FLAG_PVP;
    arrow = projectile_test_arrow(map, pl, other_player, 3, SK_BOW_ARCHERY);
    int pvp_damage = projectile_test_hit(arrow, other_player);
    ck_assert_int_gt(pvp_damage, 0);
    ck_assert_int_le(pvp_damage, TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);
}
END_TEST

START_TEST(test_projectile_metric_and_non_archery_openers) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_THROWING);
    SET_FLAG(arrow, FLAG_IS_THROWN);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(projectile_test_bonus_messages(
                          pl,
                          "Sneak damage bonus: +25% (+6 base damage) — unaware target."),
                      1);

    /* A stopped arrow is neither bonus-eligible nor metric-eligible. */
    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    CLEAR_FLAG(arrow, FLAG_FLYING);
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);
}
END_TEST

START_TEST(test_archery_slaying_stacks_after_bounded_bonus_without_mutation) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    object *target = projectile_test_target(map, pl);
    target->direction = 3;

    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    FREE_AND_COPY_HASH(arrow->slaying, target->race);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 63);
    object *stopped = object_projectile_stop(arrow, OBJECT_PROJECTILE_STOP_HIT);
    ck_assert_ptr_nonnull(stopped);
    ck_assert_int_eq(stopped->stats.dam, stopped->arch->clone.stats.dam);
    ck_assert_ptr_null(stopped->chosen_skill);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    FREE_AND_COPY_HASH(arrow->slaying, target->race);
    SET_FLAG(arrow, FLAG_IS_ASSASSINATION);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 81);
    stopped = object_projectile_stop(arrow, OBJECT_PROJECTILE_STOP_HIT);
    ck_assert_ptr_nonnull(stopped);
    ck_assert_int_eq(stopped->stats.dam, stopped->arch->clone.stats.dam);
    ck_assert_ptr_null(stopped->chosen_skill);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 2);
}
END_TEST

START_TEST(test_archery_rounding_and_overflow_boundaries) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    target->enemy = pl;
    target->enemy_count = pl->count;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->stats.dam = 25;
    arrow->weight = 1001;
    FREE_AND_COPY_HASH(arrow->slaying, target->race);
    object *combat_target = HEAD(target);
    int hp_before = combat_target->stats.hp;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 54);
    ck_assert_int_eq(combat_target->stats.hp, hp_before - 55);
    ck_assert_uint_eq(
        projectile_test_bonus_messages(pl,
                                       "Archery damage bonus: +25% (+6 base damage) — rear shot."),
        1);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    target->enemy = pl;
    target->enemy_count = pl->count;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->stats.dam = 25;
    arrow->weight = 1001;
    FREE_AND_COPY_HASH(arrow->slaying, target->race);
    SET_FLAG(arrow, FLAG_IS_ASSASSINATION);
    combat_target = HEAD(target);
    hp_before = combat_target->stats.hp;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 69);
    ck_assert_int_eq(combat_target->stats.hp, hp_before - 70);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    target->stats.hp = 100000;
    target->stats.maxhp = 100000;
    target->enemy = pl;
    target->enemy_count = pl->count;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->stats.dam = INT16_MAX;
    arrow->weight = 1001;
    FREE_AND_COPY_HASH(arrow->slaying, target->race);
    ck_assert_int_eq(object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->stats.hp, 28323);
    ck_assert_int_eq(target->last_damage, INT16_MAX);
    ck_assert_int_eq(arrow->stats.dam, INT16_MIN);
    ck_assert_uint_eq(projectile_test_bonus_messages(
                          pl,
                          "Archery damage bonus: +25% (+8191 base damage) — rear shot."),
                      1);

    target = projectile_test_target(map, pl);
    target->stats.hp = 100000;
    target->stats.maxhp = 100000;
    target->enemy = pl;
    target->enemy_count = pl->count;
    projectile_test_skill(pl, SK_WIZARDRY_SPELLS);
    object *spell = arch_get("lightning");
    ck_assert_ptr_null(spell->other_arch);
    spell->x = target->x;
    spell->y = target->y;
    spell->stats.dam = INT16_MAX;
    memset(spell->attack, 0, sizeof(spell->attack));
    spell->attack[ATNR_ELECTRICITY] = 100;
    FREE_AND_COPY_HASH(spell->slaying, target->race);
    SET_FLAG(spell, FLAG_IS_ASSASSINATION);
    object_owner_set(spell, pl);
    spell = object_insert_map(spell, map, NULL, INS_NO_MERGE);
    tag_t spell_count = spell->count;
    GET_MAP_SPACE_PTR(map, spell->x, spell->y)->flags |= P_NO_PASS;
    check_fired_arch(spell);
    ck_assert_int_eq(target->stats.hp, 26274);
    ck_assert(!OBJECT_VALID(spell, spell_count));
}
END_TEST

START_TEST(test_archery_immune_hit_alerts_without_feedback_or_metric) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    target->protection[ATNR_PIERCE] = 99;

    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->weight = 1001;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 0);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_DAMAGE_DEALT), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_LARGEST_HIT_DEALT), 1);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    target->protection[ATNR_PIERCE] = 100;

    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->weight = 1001;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 0);
    ck_assert(OBJECT_VALID(target->enemy, target->enemy_count));
    ck_assert_ptr_eq(target->enemy, pl);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_DAMAGE_DEALT), 1);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);
}
END_TEST

START_TEST(test_archery_alert_sources_stealth_and_direction_boundaries) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    /* Passive stealth does not gate an otherwise unaware opening. Direction
     * zero can never add the rear reason. */
    SET_FLAG(pl, FLAG_STEALTH);
    object *target = projectile_test_target(map, pl);
    target->direction = 0;
    object *arrow = projectile_test_arrow(map, pl, target, 0, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);
    ck_assert_uint_eq(projectile_test_bonus_messages(
                          pl,
                          "Archery damage bonus: +25% (+6 base damage) — unaware target."),
                      1);

    /* Front and side impacts against an engaged target receive no bonus. */
    target = projectile_test_target(map, pl);
    target->enemy = pl;
    target->enemy_count = pl->count;
    target->direction = 7;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);

    target = projectile_test_target(map, pl);
    target->enemy = pl;
    target->enemy_count = pl->count;
    target->direction = 4;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 1);
}
END_TEST

START_TEST(test_nonarchery_openers_and_invalid_sources) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->ownercount++;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 0);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_WIZARDRY_SPELLS);
    arrow->type = LIGHTNING;
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    SET_FLAG(arrow, FLAG_IS_SPELL);
    ck_assert_int_eq(common_object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->last_damage, 30);

    /* Cascaded nonliving spell effects retain both the original wizardry
     * skill and direct-player attack provenance. */
    target = projectile_test_target(map, pl);
    object *parent_effect = projectile_test_arrow(map, pl, target, 3, SK_WIZARDRY_SPELLS);
    parent_effect->type = BULLET;
    CLEAR_FLAG(parent_effect, FLAG_IS_MISSILE);
    SET_FLAG(parent_effect, FLAG_IS_SPELL);
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->type = LIGHTNING;
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    SET_FLAG(arrow, FLAG_IS_SPELL);
    object_owner_copy(arrow, parent_effect);
    ck_assert_ptr_eq(arrow->chosen_skill, parent_effect->chosen_skill);
    ck_assert(arrow->player_attack_source);
    ck_assert_int_eq(common_object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->last_damage, 30);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    SET_FLAG(arrow, FLAG_IS_SPELL);
    ck_assert_int_eq(common_object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->last_damage, 30);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    SET_FLAG(arrow, FLAG_IS_THROWN);
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->type = BULLET;
    CLEAR_FLAG(arrow, FLAG_IS_MISSILE);
    SET_FLAG(arrow, FLAG_IS_SPELL);
    ck_assert_int_eq(common_object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->last_damage, 30);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    object *npc = arch_get("goblin");
    npc->x = pl->x;
    npc->y = pl->y + 1;
    npc = object_insert_map(npc, map, NULL, INS_NO_MERGE);
    monster_data_init(npc);
    npc->enemy = target;
    npc->enemy_count = target->count;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    object_owner_set(arrow, npc);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);

    /* Player ownership may be flattened through a pet, but the live
     * non-player source cannot propagate the player's skill provenance. */
    target = projectile_test_target(map, pl);
    target->direction = 3;
    object_owner_set(npc, pl);
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    object_owner_copy(arrow, npc);
    ck_assert_ptr_eq(arrow->chosen_skill, npc->chosen_skill);
    ck_assert_ptr_nonnull(arrow->chosen_skill);
    ck_assert(!arrow->player_attack_source);
    ck_assert_int_eq(projectile_test_hit(arrow, target), TEST_BASE_DAMAGE);

    /* Further nonliving effects preserve the pet origin rather than
     * laundering the flattened player owner into direct-player provenance. */
    target = projectile_test_target(map, pl);
    object *pet_descendant = projectile_test_arrow(map, pl, target, 3, SK_WIZARDRY_SPELLS);
    pet_descendant->type = LIGHTNING;
    CLEAR_FLAG(pet_descendant, FLAG_IS_MISSILE);
    SET_FLAG(pet_descendant, FLAG_IS_SPELL);
    object_owner_copy(pet_descendant, arrow);
    ck_assert_ptr_eq(pet_descendant->chosen_skill, arrow->chosen_skill);
    ck_assert(!pet_descendant->player_attack_source);
    ck_assert_int_eq(common_object_projectile_hit(pet_descendant, target), OBJECT_METHOD_OK);
    ck_assert_int_eq(target->last_damage, TEST_BASE_DAMAGE);

    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 2);
    ck_assert_uint_eq(projectile_test_bonus_messages(
                          pl,
                          "Sneak damage bonus: +25% (+6 base damage) — unaware target."),
                      5);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 5);
}
END_TEST

START_TEST(test_archery_blocked_and_lethal_impact_ordering) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;

    object *target = projectile_test_target(map, pl);
    target->direction = 3;
    target->block = 100;
    object *arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->weight = 1001;
    attack_block_test_override = 1;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 0);
    attack_block_test_override = -1;
    ck_assert(OBJECT_VALID(target->enemy, target->enemy_count));
    ck_assert_ptr_eq(target->enemy, pl);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 0);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), 0);

    /* The block consumed the unaware opening through the normal alert
     * transition, so a subsequent rear hit receives rear only. */
    target->block = 0;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    arrow->weight = 1001;
    ck_assert_int_eq(projectile_test_hit(arrow, target), 30);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS), 1);
    ck_assert_uint_eq(
        projectile_test_bonus_messages(pl,
                                       "Archery damage bonus: +25% (+6 base damage) — rear shot."),
        1);

    target = projectile_test_target(map, pl);
    target->direction = 3;
    target->level = 1;
    target->stats.hp = 10;
    target->stats.maxhp = 10;
    target->stats.exp = 1000;
    arrow = projectile_test_arrow(map, pl, target, 3, SK_BOW_ARCHERY);
    tag_t arrow_count = arrow->count;
    object *skill = arrow->chosen_skill;
    object *saved_skill = CONTR(pl)->skill_ptr[SK_BOW_ARCHERY];
    CONTR(pl)->skill_ptr[SK_BOW_ARCHERY] = skill;
    int64_t experience_before = skill->stats.exp;
    uint64_t metric_before = metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS);
    size_t messages_before = projectile_test_bonus_messages(pl, NULL);
    ck_assert_int_eq(object_projectile_hit(arrow, target), OBJECT_METHOD_OK);
    ck_assert(OBJECT_VALID(arrow, arrow_count));
    ck_assert_ptr_nonnull(arrow->env);
    ck_assert_int_gt(skill->stats.exp, experience_before);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_PROJECTILE_HITS),
                      metric_before + 1);
    ck_assert_uint_eq(projectile_test_bonus_messages(pl, NULL), messages_before + 1);
    CONTR(pl)->skill_ptr[SK_BOW_ARCHERY] = saved_skill;
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("projectile");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_invalid_projectile_directions_stop_before_lookup);
    tcase_add_test(tc_core, test_archery_impact_bonuses_and_feedback);
    tcase_add_test(tc_core, test_archery_unaware_state_is_impact_time_and_validated);
    tcase_add_test(tc_core, test_archery_invisibility_and_target_ownership_exclusions);
    tcase_add_test(tc_core, test_projectile_metric_and_non_archery_openers);
    tcase_add_test(tc_core, test_archery_slaying_stacks_after_bounded_bonus_without_mutation);
    tcase_add_test(tc_core, test_archery_rounding_and_overflow_boundaries);
    tcase_add_test(tc_core, test_archery_immune_hit_alerts_without_feedback_or_metric);
    tcase_add_test(tc_core, test_archery_alert_sources_stealth_and_direction_boundaries);
    tcase_add_test(tc_core, test_nonarchery_openers_and_invalid_sources);
    tcase_add_test(tc_core, test_archery_blocked_and_lethal_impact_ordering);
    return s;
}

void check_types_projectile(void) {
    check_run_suite(suite(), __FILE__);
}
