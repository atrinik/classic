/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <attack.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <monster.h>
#include <monster_data.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <plugin.h>
#include <propagation.h>
#include <server.h>
#include <spells.h>
#include <toolkit/packet.h>

static bool plugins_initialized;

static void fixture(mapstruct **map, object **pl) {
    toolkit_import(packet);
    check_setup_env_pl(map, pl);
    FREE_AND_COPY_HASH((*map)->path, "/python/actor-callback-fixture");
    object_remove(*pl, 0);
    (*pl)->x = 12;
    (*pl)->y = 12;
    *pl = object_insert_map(*pl, *map, NULL, 0);
    ck_assert_ptr_nonnull(*pl);
    FREE_AND_COPY_HASH((*pl)->name, "actor-callback-player");
    CONTR(*pl)->tls = 0;
    CONTR(*pl)->cmd_permissions = xcalloc(1, sizeof(char *));
    CONTR(*pl)->cmd_permissions[0] = xstrdup("kick");
    CONTR(*pl)->num_cmd_permissions = 1;
    CLEAR_FLAG(*pl, FLAG_BLIND);
    CLEAR_FLAG(*pl, FLAG_XRAYS);
    for (int x = 0; x < MAP_WIDTH(*map); x++) {
        for (int y = 0; y < MAP_HEIGHT(*map); y++) {
            GET_MAP_SPACE_PTR(*map, x, y)->light_value = 1280;
        }
    }
    if (!plugins_initialized) {
        init_plugins();
        plugins_initialized = true;
    }
    init_spells();
}

static object *place(mapstruct *map, int x, int y) {
    object *op = arch_get("raas");
    ck_assert_ptr_nonnull(op);
    op->x = x;
    op->y = y;
    op = object_insert_map(op, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(op);
    monster_data_init(op);
    return op;
}

static void ask_show(object *actor, const char *mode) {
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_ASK_SHOW;
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
    FREE_AND_COPY_HASH(event->slaying, mode);
    object_insert_into(event, actor, INS_NO_MERGE);
}

START_TEST(test_monster_process_rejects_recycled_observer_or_target) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *monster = place(map, 13, 12);
    object *target = place(map, 14, 12);
    SET_FLAG(target, FLAG_FRIENDLY);
    monster->enemy = target;
    monster->enemy_count = target->count;
    SET_FLAG(monster, FLAG_SLEEP);
    monster->stats.Con = 10;
    monster->stats.hp = 17;
    monster->stats.maxhp = 99;
    monster->last_heal = 5;
    tag_t original_count = _i == 0 ? monster->count : target->count;
    object *original = _i == 0 ? monster : target;
    ask_show(target, _i == 0 ? "recycle_observer" : "recycle_target");

    object_process(monster);

    ck_assert(!OBJECT_VALID(original, original_count));
    object *replacement = GET_MAP_OB(map, _i == 0 ? 13 : 14, 12);
    while (replacement != NULL && strcmp(replacement->name, "callback replacement") != 0) {
        replacement = replacement->above;
    }
    ck_assert_ptr_nonnull(replacement);
    ck_assert_ptr_eq(replacement, original);
    ck_assert(QUERY_FLAG(replacement, FLAG_SLEEP));
    ck_assert_int_eq(replacement->stats.hp, 17);
    ck_assert_int_eq(replacement->last_heal, 5);
    if (_i != 0) {
        ck_assert(QUERY_FLAG(monster, FLAG_SLEEP));
        ck_assert_int_eq(monster->stats.hp, 17);
    }
}
END_TEST

START_TEST(test_monster_cast_rechecks_borrowed_spell_and_enemy) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *monster = place(map, 13, 12);
    object *target = place(map, 15, 12);
    target->stats.hp = target->stats.maxhp = 100;
    SET_FLAG(target, FLAG_FRIENDLY);
    monster->enemy = target;
    monster->enemy_count = target->count;
    monster->stats.Dex = 1;
    monster->stats.Pow = monster->stats.Con = 0;
    monster->stats.sp = monster->stats.maxsp = 100;
    monster->last_grace = 0;
    monster->behavior = 0;
    SET_FLAG(monster, FLAG_CAST_SPELL);
    CLEAR_FLAG(monster, FLAG_SCARED);
    CLEAR_FLAG(monster, FLAG_CONFUSED);
    object *ability = object_get();
    ability->type = ABILITY;
    ability->stats.sp = SP_BULLET;
    ability->stats.hp = -1;
    FREE_AND_COPY_HASH(ability->name, "callback ability");
    object_insert_into(ability, monster, 0);
    tag_t ability_count = ability->count, target_count = target->count;
    ask_show(target, _i == 0 ? "destroy_spell_second" : "destroy_target_second");
    /* Installing the event is an inventory mutation: living_update_monster
     * recalculates and clamps HP. Capture the baseline after that setup. */
    int target_hp = target->stats.hp;
    ck_assert_int_gt(target_hp, 0);
    size_t reserved = propagation_reserved_cells();

    object_process(monster);

    ck_assert(_i == 0 ? !OBJECT_VALID(ability, ability_count) :
                        !OBJECT_VALID(target, target_count));
    ck_assert_int_eq(monster->stats.sp, 100);
    ck_assert_int_eq(monster->last_grace, 0);
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
    for (int x = 0; x < MAP_WIDTH(map); x++) {
        for (int y = 0; y < MAP_HEIGHT(map); y++) {
            for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                ck_assert_ptr_ne(op->arch, spellarch[SP_BULLET]);
            }
        }
    }
    if (_i == 0) {
        /* Losing an inventory ability cancels this cast. The retained enemy
         * and range vector are still valid, so ordinary pursuit may continue. */
        ck_assert(OBJECT_VALID(target, target_count));
        ck_assert_int_eq(target->stats.hp, target_hp);
    } else {
        /* Losing the mapped enemy invalidates the borrowed range vector and
         * must stop the entire action before movement or melee. */
        ck_assert_int_eq(monster->x, 13);
        ck_assert_int_eq(monster->y, 12);
    }
}
END_TEST

START_TEST(test_attack_retarget_aborts_when_visibility_logs_out_victim) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *attacker = place(map, 13, 12);
    object *retained = place(map, 12, 13);
    /* An active fight needs positive HP and a published visible identity for
     * both actors before the destructive callback is installed. */
    attacker->stats.hp = attacker->stats.maxhp = 100;
    retained->stats.hp = retained->stats.maxhp = 100;
    draw_client_map(pl);
    player *controller = CONTR(pl);
    ck_assert(target_player_authorized(controller, attacker, NULL));
    ck_assert(target_player_authorized(controller, retained, NULL));
    socket_struct *socket = controller->cs;
    tag_t count = pl->count;
    controller->combat = 1;
    controller->target_object = retained;
    controller->target_object_count = retained->count;
    retained->enemy = pl;
    retained->enemy_count = pl->count;
    ask_show(_i == 0 ? retained : attacker, "kick_observer");

    ck_assert_int_eq(attack_hit(pl, attacker, 1), 0);
    ck_assert(!player_connection_is_current(pl, count, controller, socket));
}
END_TEST

START_TEST(test_projectile_helpers_abort_after_callback_logout) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *target = place(map, 13, 12);
    draw_client_map(pl);
    player *controller = CONTR(pl);
    socket_struct *socket = controller->cs;
    tag_t count = pl->count;
    controller->target_object = target;
    controller->target_object_count = target->count;
    ask_show(target, "kick_observer");
    size_t reserved = propagation_reserved_cells();
    int result;
    switch (_i) {
        case 0:
            result = fire_arch_from_position(pl, pl, pl->x, pl->y, 3,
                                              spellarch[SP_BULLET], SP_BULLET, target);
            break;
        case 1:
            result = fire_arch_from_position(pl, pl, pl->x, pl->y, 3,
                                              spellarch[SP_BULLET], SP_BULLET, NULL);
            break;
        case 2:
            result = fire_bolt(pl, pl, 3, SP_FIREBOLT);
            break;
        case 3:
            result = cast_cone(pl, pl, 3, 3, SP_FIRESTORM, spellarch[SP_FIRESTORM]);
            break;
        default:
            result = fire_swarm(pl, pl, 3, spellarch[SP_BULLET], SP_BULLET, 3, 0);
            break;
    }
    ck_assert_int_eq(result, 0);
    ck_assert(!player_connection_is_current(pl, count, controller, socket));
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
}
END_TEST

START_TEST(test_projectile_rejects_destroyed_independent_caster) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *caster = place(map, 12, 13);
    FREE_AND_COPY_HASH(caster->name, "callback caster");
    object *target = place(map, 13, 12);
    draw_client_map(pl);
    ask_show(target, "destroy_caster");
    tag_t count = caster->count;
    size_t reserved = propagation_reserved_cells();

    ck_assert_int_eq(fire_arch_from_position(pl, caster, pl->x, pl->y, 3,
                                              spellarch[SP_BULLET], SP_BULLET, target), 0);
    ck_assert(!OBJECT_VALID(caster, count));
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("server.actor_callbacks");
    TCase *tc = tcase_create("callbacks");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    tcase_add_loop_test(tc, test_monster_process_rejects_recycled_observer_or_target, 0, 2);
    tcase_add_loop_test(tc, test_monster_cast_rechecks_borrowed_spell_and_enemy, 0, 2);
    tcase_add_loop_test(tc, test_attack_retarget_aborts_when_visibility_logs_out_victim, 0, 2);
    tcase_add_loop_test(tc, test_projectile_helpers_abort_after_callback_logout, 0, 5);
    tcase_add_test(tc, test_projectile_rejects_destroyed_independent_caster);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_actor_callbacks(void) {
    check_run_suite(suite(), __FILE__);
}
