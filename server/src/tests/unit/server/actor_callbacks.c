/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <arrow.h>
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
#include <skills.h>
#include <toolkit/math.h>
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

static void assert_no_arch(mapstruct *map, archetype_t *arch) {
    for (int x = 0; x < MAP_WIDTH(map); x++) {
        for (int y = 0; y < MAP_HEIGHT(map); y++) {
            for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                ck_assert_ptr_ne(op->arch, arch);
            }
        }
    }
}

START_TEST(test_projectile_rejects_relocated_origin) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    mapstruct *destination = map;
    if (_i != 0) {
        destination = get_empty_map(24, 24);
        FREE_AND_COPY_HASH(destination->path, "/python/actor-callback-destination");
    }
    object *target = place(map, 13, 12);
    draw_client_map(pl);
    ck_assert(target_player_authorized(CONTR(pl), target, NULL));
    ck_assert(object_set_value(target, "callback_destination", destination->path, true));
    ask_show(target, "teleport_observer");
    size_t reserved = propagation_reserved_cells();

    ck_assert_int_eq(fire_arch_from_position(pl, pl, pl->x, pl->y, 3,
                                              spellarch[SP_BULLET], SP_BULLET, target), 0);

    ck_assert_ptr_eq(pl->map, destination);
    ck_assert_int_eq(pl->x, 17);
    ck_assert_int_eq(pl->y, 17);
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
    assert_no_arch(map, spellarch[SP_BULLET]);
    assert_no_arch(destination, spellarch[SP_BULLET]);
}
END_TEST

START_TEST(test_ranged_first_and_second_authorization_logout) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *target = place(map, 13, 12);
    object *arrow = arch_get("arrow");
    arrow->last_sp = 5;
    arrow->weight = MAX(1, arrow->weight);
    CLEAR_FLAG(arrow, FLAG_STARTEQUIP);
    CLEAR_FLAG(arrow, FLAG_NO_DROP);
    arrow = object_insert_into(arrow, pl, INS_NO_MERGE);
    draw_client_map(pl);
    player *controller = CONTR(pl);
    socket_struct *socket = controller->cs;
    tag_t count = pl->count;
    ck_assert(target_player_authorized(controller, target, NULL));
    controller->target_object = target;
    controller->target_object_count = target->count;
    ask_show(target, _i == 0 ? "kick_observer" : "kick_observer_second");

    int result = object_ranged_fire(arrow, pl, 3, NULL);

    ck_assert_int_eq(result, _i == 0 ? OBJECT_METHOD_UNHANDLED : OBJECT_METHOD_OK);
    ck_assert(!player_connection_is_current(pl, count, controller, socket));
    ck_assert_str_eq(object_get_value(target, "actor_callback_calls"), _i == 0 ? "1" : "2");
    assert_no_arch(map, arch_find("arrow"));
}
END_TEST

START_TEST(test_disease_aborts_after_actor_invalidation) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *target;
    player *target_controller = NULL;
    socket_struct *target_socket = NULL;
    if (_i == 4) {
        target = player_get_dummy("actor-callback-target", NULL);
        target_controller = CONTR(target);
        target_socket = target_controller->cs;
        object_remove(target, 0);
        target->x = 13;
        target->y = 12;
        target = object_insert_map(target, map, NULL, 0);
    } else {
        target = place(map, 13, 12);
    }
    object *caster = pl;
    player *caster_controller = NULL;
    socket_struct *caster_socket = NULL;
    if (_i == 1) {
        caster = place(map, 12, 13);
        FREE_AND_COPY_HASH(caster->name, "callback caster");
    } else if (_i == 3) {
        caster = player_get_dummy("actor-callback-caster", NULL);
        caster_controller = CONTR(caster);
        caster_socket = caster_controller->cs;
        object_remove(caster, 0);
        caster->x = 12;
        caster->y = 13;
        caster = object_insert_map(caster, map, NULL, 0);
        ck_assert(object_set_value(target, "callback_caster", caster->name, true));
    }
    tag_t caster_count = caster->count, target_count = target->count;
    player *controller = CONTR(pl);
    socket_struct *socket = controller->cs;
    tag_t count = pl->count;
    draw_client_map(pl);
    ck_assert(target_player_authorized(controller, target, NULL));
    controller->target_object = target;
    controller->target_object_count = target_count;
    const char *modes[] = {"kick_observer", "destroy_caster", "recycle_target", "kick_caster", "kick_target"};
    ask_show(target, modes[_i]);
    size_t reserved = propagation_reserved_cells();

    ck_assert_int_eq(cast_cause_disease(pl, caster, 3, spellarch[SP_CAUSE_COLD], SP_CAUSE_COLD), 0);

    if (_i == 0) {
        ck_assert(!player_connection_is_current(pl, count, controller, socket));
    } else if (_i == 3) {
        ck_assert(!player_connection_is_current(caster, caster_count, caster_controller, caster_socket));
    } else if (_i == 4) {
        ck_assert(!player_connection_is_current(target, target_count, target_controller, target_socket));
    } else {
        ck_assert(_i == 1 ? !OBJECT_VALID(caster, caster_count) :
                            !OBJECT_VALID(target, target_count));
    }
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
    for (int x = 0; x < MAP_WIDTH(map); x++) {
        for (int y = 0; y < MAP_HEIGHT(map); y++) {
            for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                ck_assert_ptr_null(object_find_type(op, DISEASE));
            }
        }
    }
}
END_TEST

START_TEST(test_fire_command_aborts_after_ranged_logout) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *target = place(map, 13, 12);
    object *bow = object_insert_into(arch_get("bow_short"), pl, INS_NO_MERGE);
    object *skill = object_insert_into(arch_get("skill_bow_archery"), pl, INS_NO_MERGE);
    player *controller = CONTR(pl);
    controller->skill_ptr[SK_BOW_ARCHERY] = skill;
    controller->equipment[PLAYER_EQUIP_WEAPON_RANGED] = bow;
    controller->action_attack = global_round_tag;
    draw_client_map(pl);
    ck_assert(target_player_authorized(controller, target, NULL));
    controller->target_object = target;
    controller->target_object_count = target->count;
    ask_show(target, "kick_observer");
    socket_struct *socket = controller->cs;
    tag_t count = pl->count;
    uint8_t command[] = {3, 0, 0, 0, 0, 0, 0, 0, 0};

    socket_command_fire(socket, controller, command, sizeof(command), 0);

    ck_assert(!player_connection_is_current(pl, count, controller, socket));
    ck_assert_str_eq(object_get_value(target, "actor_callback_done"), "1");
}
END_TEST

START_TEST(test_monster_bow_aborts_after_later_ranged_recycling) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *monster = place(map, 13, 12);
    object *target = place(map, 15, 12);
    object *bow = object_insert_into(arch_get("bow_short"), monster, INS_NO_MERGE);
    SET_FLAG(bow, FLAG_APPLIED);
    monster->enemy = target;
    monster->enemy_count = target->count;
    monster->stats.Dex = 1;
    monster->stats.Pow = monster->stats.Con = 0;
    monster->behavior = 0;
    monster->stats.Int = 0;
    CLEAR_FLAG(monster, FLAG_SCARED);
    CLEAR_FLAG(monster, FLAG_CONFUSED);
    CLEAR_FLAG(monster, FLAG_CAST_SPELL);
    SET_FLAG(monster, FLAG_READY_BOW);
    tag_t count = monster->count;
    ask_show(target, "recycle_observer_third");
    /* Select the ordinary bow branch and its continue-after-fire outcome.
     * The callback must abort before the latter can touch the replacement. */
    uint64_t seed;
    for (seed = 0; seed < 10000; seed++) {
        rng_state_t rng;
        rng_seed(&rng, seed);
        if (rng_chance(&rng, 1) && rng_chance(&rng, 4) && !rng_chance(&rng, 2)) {
            break;
        }
    }
    ck_assert_uint_lt(seed, 10000);
    rndm_seed(seed);

    object_process(monster);

    ck_assert(!OBJECT_VALID(monster, count));
    ck_assert_str_eq(object_get_value(target, "actor_callback_calls"), "3");
    object *replacement = GET_MAP_OB(map, 13, 12);
    while (replacement != NULL && strcmp(replacement->name, "callback replacement") != 0) {
        replacement = replacement->above;
    }
    ck_assert_ptr_eq(replacement, monster);
    ck_assert(QUERY_FLAG(replacement, FLAG_SLEEP));
    ck_assert_int_eq(replacement->stats.hp, 17);
}
END_TEST

START_TEST(test_monster_bow_preserves_successful_fire_and_pursuit) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *monster = place(map, 13, 12);
    object *target = place(map, 16, 12);
    object *bow = object_insert_into(arch_get("bow_short"), monster, INS_NO_MERGE);
    object *arrow = object_insert_into(arch_get("arrow"), monster, INS_NO_MERGE);
    arrow->last_sp = 5;
    SET_FLAG(bow, FLAG_APPLIED);
    monster->enemy = target;
    monster->enemy_count = target->count;
    monster->stats.Dex = 1;
    monster->stats.Pow = monster->stats.Con = monster->stats.Int = 0;
    monster->behavior = monster->attack_move_type = 0;
    CLEAR_FLAG(monster, FLAG_SCARED);
    CLEAR_FLAG(monster, FLAG_CONFUSED);
    CLEAR_FLAG(monster, FLAG_CAST_SPELL);
    CLEAR_FLAG(monster, FLAG_STAND_STILL);
    SET_FLAG(monster, FLAG_READY_BOW);
    tag_t count = monster->count;
    int damage = arrow_get_damage(monster, bow, arrow);
    ck_assert_int_gt(damage, 0);
    /* The shot consumes a damage roll between the bow-choice and the
     * continue-after-fire roll. No target is adjacent to its first step. */
    uint64_t seed;
    for (seed = 0; seed < 10000; seed++) {
        rng_state_t rng;
        rng_seed(&rng, seed);
        if (!rng_chance(&rng, 1) || !rng_chance(&rng, 4)) {
            continue;
        }
        rng_range(&rng, damage * 0.8 + 1, damage);
        if (!rng_chance(&rng, 2)) {
            break;
        }
    }
    ck_assert_uint_lt(seed, 10000);
    rndm_seed(seed);

    object_process(monster);

    ck_assert(OBJECT_VALID(monster, count));
    ck_assert_int_eq(monster->x, 14);
    ck_assert_int_eq(monster->y, 12);
    bool fired = false;
    for (object *op = GET_MAP_OB(map, 14, 12); op != NULL; op = op->above) {
        if (op->arch == arrow->arch && OBJECT_IS_PROJECTILE(op)) {
            fired = true;
        }
    }
    ck_assert(fired);
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
    tcase_add_loop_test(tc, test_projectile_rejects_relocated_origin, 0, 2);
    tcase_add_loop_test(tc, test_ranged_first_and_second_authorization_logout, 0, 2);
    tcase_add_loop_test(tc, test_disease_aborts_after_actor_invalidation, 0, 5);
    tcase_add_test(tc, test_fire_command_aborts_after_ranged_logout);
    tcase_add_test(tc, test_monster_bow_aborts_after_later_ranged_recycling);
    tcase_add_test(tc, test_monster_bow_preserves_successful_fire_and_pursuit);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_actor_callbacks(void) {
    check_run_suite(suite(), __FILE__);
}
