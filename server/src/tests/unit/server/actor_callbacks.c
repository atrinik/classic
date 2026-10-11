/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <arrow.h>
#include <attack.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <connection.h>
#include <initialization.h>
#include <monster.h>
#include <metrics.h>
#include <monster_guard.h>
#include <faction.h>
#include <monster_data.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <plugin.h>
#include <perception.h>
#include <propagation.h>
#include <server.h>
#include <spells.h>
#include <skills.h>
#include <toolkit/math.h>
#include <toolkit/mempool.h>
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

/* Force the target into a new normal-sized object puddle. Holding the old
 * free chunks avoids changing allocator configuration or mocking logout. */
static object *reclaimable_target(mapstruct *map, mempool_struct *pool,
                                  mempool_chunk_struct **target_storage, bool npc) {
    size_t num_held = pool->nrof_free[0];
    object **held = xcalloc(MAX(num_held, 1), sizeof(*held));
    for (size_t i = 0; i < num_held; i++) {
        held[i] = object_get();
    }
    ck_assert_uint_eq(pool->nrof_free[0], 0);
    object *target = npc ? arch_get("raas") : player_get_dummy("actor-reclaim-target", NULL);
    if (npc) {
        monster_data_init(target);
        FREE_AND_COPY_HASH(target->msg, "talk fixture");
    }
    ck_assert_ptr_nonnull(pool->puddlelist[0]);
    *target_storage = pool->puddlelist[0]->first_chunk;
    uintptr_t address = (uintptr_t)MEM_POOLDATA(target);
    uintptr_t first = (uintptr_t)*target_storage;
    ck_assert(address >= first &&
              address < first + pool->expand_size * (sizeof(mempool_chunk_struct) + pool->chunksize));
    if (!QUERY_FLAG(target, FLAG_REMOVED)) {
        object_remove(target, 0);
    }
    target->x = 13;
    target->y = 12;
    target = object_insert_map(target, map, NULL, 0);
    for (size_t i = 0; i < num_held; i++) {
        object_destroy(held[i]);
    }
    efree(held);
    return target;
}

START_TEST(test_target_logout_reclaims_storage_across_authorization_callers) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    player *controller = CONTR(pl);
    controller->cmd_permissions = xrealloc(controller->cmd_permissions, 2 * sizeof(char *));
    controller->cmd_permissions[1] = xstrdup("memfree");
    controller->num_cmd_permissions = 2;
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *target_storage;
    bool npc = _i >= 9;
    int caller = _i % 9;
    object *target = reclaimable_target(map, pool, &target_storage, npc);
    player *target_controller = npc ? NULL : CONTR(target);
    socket_struct *target_socket = npc ? NULL : target_controller->cs;
    tag_t target_count = target->count;
    draw_client_map(pl);
    ck_assert(target_player_authorized(controller, target, NULL));
    controller->target_object = target;
    controller->target_object_count = target_count;
    ask_show(target, npc ? "destroy_target_reclaim" : "kick_target_reclaim");
    size_t reserved = propagation_reserved_cells();

    switch (caller) {
        case 0:
            ck_assert(!perception_identity_visible(pl, target));
            break;
        case 1:
            ck_assert(!perception_object_visible(pl, target));
            break;
        case 2:
            ck_assert(!perception_player_part_authorized(controller, target));
            break;
        case 3:
            ck_assert(!perception_player_authorized(controller, target));
            break;
        case 4:
            ck_assert(!target_player_authorized(controller, target, NULL));
            break;
        case 5:
            send_target_command(controller);
            break;
        case 6:
            esrv_update_stats(controller);
            break;
        case 7:
            ck_assert_int_eq(cast_cause_disease(pl, pl, 3, spellarch[SP_CAUSE_COLD], SP_CAUSE_COLD), 0);
            break;
        default: {
            packet_struct *select = packet_new(SERVER_CMD_TARGET, 8, 0);
            packet_writer_write_uint8(select, CMD_TARGET_MAPXY);
            packet_writer_write_uint8(select, controller->cs->mapx_2 + 1);
            packet_writer_write_uint8(select, controller->cs->mapy_2);
            packet_writer_write_int8(select, 0);
            packet_writer_write_uint32(select, target_count);
            pticks++;
            socket_command_target(controller->cs, controller, select->data, select->len, 0);
            packet_free(select);
            break;
        }
    }

    if (!npc) {
        ck_assert(!player_connection_is_current(target, target_count, target_controller, target_socket));
    }
    /* Prove the callback freed the backing allocation, rather than merely
     * invalidating an object count in still-readable pool storage. */
    for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
        ck_assert_ptr_ne(puddle->first_chunk, target_storage);
    }
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
    if (caller == 5 || caller == 6 || caller == 8) {
        ck_assert_ptr_eq(controller->target_object, pl);
        ck_assert_uint_eq(controller->target_object_count, 0);
        ck_assert_ptr_null(pl->enemy);
    } else {
        /* Direct predicates leave selection management to their caller. */
        controller->target_object = pl;
        controller->target_object_count = 0;
    }
}
END_TEST

/* Real ASK_SHOW/SAY callbacks exercise the exact action, including its normal
 * post-conversation selection notification. No synthetic acknowledgement path. */
START_TEST(test_bound_talk_rejects_callback_identity_changes) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    object *npc = place(map, 13, 12);
    FREE_AND_COPY_HASH(npc->msg, "talk fixture");
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    tag_t observer_count = observer->count;
    tag_t npc_count = npc->count;
    draw_client_map(observer);
    const char *modes[] = {"kick_observer", "recycle_target", "kick_observer_second"};
    ask_show(npc, modes[_i]);
    packet_struct *packet = packet_new(SERVER_CMD_TALK, 32, 32);
    packet_writer_write_uint8(packet, CMD_TALK_NPC_TARGET);
    packet_writer_write_uint8(packet, socket->mapx_2 + 1);
    packet_writer_write_uint8(packet, socket->mapy_2);
    packet_writer_write_int8(packet, 0);
    packet_writer_write_uint32(packet, npc_count);
    packet_writer_write_cstring(packet, "hello");
    socket_command_talk(socket, controller, packet->data, packet->len, 0);
    packet_free(packet);
    if (_i == 1) {
        ck_assert(player_connection_is_current(observer, observer_count, controller, socket));
        ck_assert_ptr_null(controller->talking_to);
        ck_assert(!OBJECT_VALID(npc, npc_count));
    } else {
        ck_assert(!player_connection_is_current(observer, observer_count, controller, socket));
    }
}
END_TEST

START_TEST(test_npc_say_logout_aborts_dialogue_bookkeeping) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    object *npc = place(map, 13, 12);
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_SAY;
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
    FREE_AND_COPY_HASH(event->slaying, "kick_observer");
    object_insert_into(event, npc, INS_NO_MERGE);
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    tag_t count = observer->count;
    char message[] = "hello";
    ck_assert_int_eq(talk_to_npc(observer, npc, message), 0);
    ck_assert(!player_connection_is_current(observer, count, controller, socket));
}
END_TEST

START_TEST(test_bound_talk_rejects_reclaimed_npc_storage) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    player *controller = CONTR(observer);
    controller->cmd_permissions = xrealloc(controller->cmd_permissions, 2 * sizeof(char *));
    controller->cmd_permissions[1] = xstrdup("memfree");
    controller->num_cmd_permissions = 2;
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *npc_storage;
    object *npc = reclaimable_target(map, pool, &npc_storage, true);
    controller->target_object = observer;
    controller->target_object_count = 0;
    socket_struct *socket = controller->cs;
    tag_t observer_count = observer->count;
    tag_t npc_count = npc->count;
    draw_client_map(observer);
    if (_i != 1) {
        ask_show(npc, _i == 0 ? "destroy_target_reclaim" : "destroy_target_reclaim_second");
    } else {
        object *event = object_get();
        event->type = EVENT_OBJECT;
        event->sub_type = EVENT_SAY;
        FREE_AND_COPY_HASH(event->name, "Python");
        FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
        FREE_AND_COPY_HASH(event->slaying, "destroy_target_reclaim");
        object_insert_into(event, npc, INS_NO_MERGE);
    }
    packet_struct *packet = packet_new(SERVER_CMD_TALK, 32, 32);
    packet_writer_write_uint8(packet, CMD_TALK_NPC_TARGET);
    packet_writer_write_uint8(packet, socket->mapx_2 + 1);
    packet_writer_write_uint8(packet, socket->mapy_2);
    packet_writer_write_int8(packet, 0);
    packet_writer_write_uint32(packet, npc_count);
    packet_writer_write_cstring(packet, "hello");
    socket_command_talk(socket, controller, packet->data, packet->len, 0);
    packet_free(packet);
    ck_assert(player_connection_is_current(observer, observer_count, controller, socket));
    ck_assert_ptr_null(controller->talking_to);
    ck_assert_ptr_eq(controller->target_object, observer);
    ck_assert_uint_eq(controller->target_object_count, 0);
    for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
        ck_assert_ptr_ne(puddle->first_chunk, npc_storage);
    }
}
END_TEST

START_TEST(test_guard_dialogue_fences_ai_and_target_notification_callbacks) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    player *controller = CONTR(observer);
    controller->cmd_permissions = xrealloc(controller->cmd_permissions, 2 * sizeof(char *));
    controller->cmd_permissions[1] = xstrdup("memfree");
    controller->num_cmd_permissions = 2;
    controller->target_object = observer;
    controller->target_object_count = 0;
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *npc_storage;
    object *npc = reclaimable_target(map, pool, &npc_storage, true);
    npc->behavior |= BEHAVIOR_GUARD;
    ck_assert(object_set_value(npc, "faction", "world", true));
    shstr *faction_name = add_string("world");
    ck_assert_ptr_nonnull(faction_find(faction_name));
    player_faction_update(controller, faction_name, -10.0);
    free_string_shared(faction_name);
    socket_struct *socket = controller->cs;
    tag_t observer_count = observer->count;
    draw_client_map(observer);
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_AI;
    event->path_attuned |= EVENT_FLAG(EVENT_AI_GUARD_STOP);
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
    const char *mode = _i == 1 ? "destroy_target_reclaim" :
                       _i == 3 ? "kick_observer" : "visible";
    FREE_AND_COPY_HASH(event->slaying, mode);
    object_insert_into(event, npc, INS_NO_MERGE);
    if (_i == 2) {
        ask_show(npc, "destroy_target_reclaim");
    }
    ck_assert(monster_guard_check(npc, observer, "hello", 0));
    if (_i == 3) {
        ck_assert(!player_connection_is_current(observer, observer_count, controller, socket));
    } else if (_i == 0) {
        ck_assert_ptr_eq(controller->talking_to, npc);
        ck_assert(monster_data_dialogs_check(npc, observer));
    } else {
        ck_assert(player_connection_is_current(observer, observer_count, controller, socket));
        ck_assert_ptr_null(controller->talking_to);
        ck_assert_ptr_eq(controller->target_object, observer);
        for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
            ck_assert_ptr_ne(puddle->first_chunk, npc_storage);
        }
    }
}
END_TEST

/* A real gate trigger can invalidate either side of CLOSE after the dialogue
 * node has been detached. Keep the handle and its connection alive so this
 * specifically exercises the guard/player ownership fences in that chain. */
START_TEST(test_guard_close_fences_gate_trigger_callbacks) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    tag_t observer_count = observer->count;
    controller->cmd_permissions = xrealloc(controller->cmd_permissions, 2 * sizeof(char *));
    controller->cmd_permissions[1] = xstrdup("memfree");
    controller->num_cmd_permissions = 2;
    object *handle = arch_get("handle_lever");
    ck_assert_ptr_nonnull(handle);
    FREE_AND_COPY_HASH(handle->slaying, "callback gate key");
    handle->x = 13;
    handle->y = 12;
    handle = object_insert_map(handle, map, NULL, INS_NO_MERGE);
    connection_object_add(handle, map, 41001);
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_TRIGGER;
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
    const char *mode = _i == 1 ? "gate_destroy_guard_reclaim" :
                       _i == 2 ? "gate_kick_observer" : "gate_visible";
    FREE_AND_COPY_HASH(event->slaying, mode);
    object_insert_into(event, handle, INS_NO_MERGE);
    object *key = object_get();
    key->type = KEY;
    FREE_AND_ADD_REF_HASH(key->slaying, handle->slaying);
    object *waypoint = arch_to_object(arches[ARCH_WAYPOINT]);
    SET_FLAG(waypoint, FLAG_DAMNED);
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *npc_storage;
    object *npc = reclaimable_target(map, pool, &npc_storage, true);
    FREE_AND_COPY_HASH(npc->name, "callback gate guard");
    npc->behavior |= BEHAVIOR_GUARD;
    object_insert_into(key, npc, INS_NO_MERGE);
    object_insert_into(waypoint, npc, INS_NO_MERGE);
    ck_assert(object_set_value(npc, "faction", "world", true));
    shstr *faction_name = add_string("world");
    ck_assert_ptr_nonnull(faction_find(faction_name));
    player_faction_update(controller, faction_name, -10.0);
    free_string_shared(faction_name);
    monster_data_dialogs_add(npc, observer, 300);
    controller->talking_to = controller->target_object = npc;
    controller->talking_to_count = controller->target_object_count = npc->count;
    uint8_t close[] = {CMD_TALK_CLOSE};
    socket_command_talk(socket, controller, close, sizeof(close), 0);
    ck_assert_str_eq(object_get_value(handle, "actor_callback_done"), "1");
    ck_assert_int_eq(handle->state, 1);
    if (_i == 2) {
        ck_assert(!player_connection_is_current(observer, observer_count, controller, socket));
    } else {
        ck_assert(player_connection_is_current(observer, observer_count, controller, socket));
        ck_assert_ptr_null(controller->talking_to);
        if (_i == 1) {
            ck_assert_ptr_eq(controller->target_object, observer);
            for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
                ck_assert_ptr_ne(puddle->first_chunk, npc_storage);
            }
        } else {
            ck_assert_ptr_eq(npc->enemy, observer);
            ck_assert(!monster_data_dialogs_check(npc, observer));
        }
    }
}
END_TEST

/* A fresh action must not trust a retained count after an earlier callback or
 * administrative operation reclaimed its storage; its epoch starts too late. */
START_TEST(test_dialogue_rejects_retained_reclaimed_references_on_later_action) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    object *npc = place(map, 14, 12);
    FREE_AND_COPY_HASH(npc->msg, "talk fixture");
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *old_storage;
    object *old = reclaimable_target(map, pool, &old_storage, true);
    controller->target_object = controller->talking_to = old;
    controller->target_object_count = controller->talking_to_count = old->count;
    npc->enemy = old;
    npc->enemy_count = old->count;
    object_remove(old, 0);
    object_destroy(old);
    ck_assert_uint_gt(mempool_reclaim(pool), 0);
    for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
        ck_assert_ptr_ne(puddle->first_chunk, old_storage);
    }
    uint8_t close[] = {CMD_TALK_CLOSE};
    socket_command_talk(socket, controller, close, sizeof(close), 0);
    draw_client_map(observer);
    packet_struct *packet = packet_new(SERVER_CMD_TALK, 32, 32);
    packet_writer_write_uint8(packet, CMD_TALK_NPC_TARGET);
    packet_writer_write_uint8(packet, socket->mapx_2 + 2);
    packet_writer_write_uint8(packet, socket->mapy_2);
    packet_writer_write_int8(packet, 0);
    packet_writer_write_uint32(packet, npc->count);
    packet_writer_write_cstring(packet, "hello");
    socket_command_talk(socket, controller, packet->data, packet->len, 0);
    packet_free(packet);
    ck_assert_ptr_eq(controller->talking_to, npc);
    ck_assert_ptr_eq(controller->target_object, npc);
}
END_TEST

START_TEST(test_device_cast_aborts_after_target_storage_reclamation) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    player *controller = CONTR(pl);
    controller->cmd_permissions = xrealloc(controller->cmd_permissions, 2 * sizeof(char *));
    controller->cmd_permissions[1] = xstrdup("memfree");
    controller->num_cmd_permissions = 2;
    object *wizardry = object_insert_into(arch_get("skill_wizardry_spells"), pl, INS_NO_MERGE);
    object *devices = object_insert_into(arch_get("skill_magic_devices"), pl, INS_NO_MERGE);
    controller->skill_ptr[SK_WIZARDRY_SPELLS] = wizardry;
    controller->skill_ptr[SK_MAGIC_DEVICES] = devices;
    int kind = _i % 4;
    bool reclaim = _i < 4;
    object *item = object_get();
    const int types[] = {SPELL, WAND, ROD, POTION};
    item->type = types[kind];
    FREE_AND_COPY_HASH(item->name, "callback device");
    item->stats.sp = SP_BULLET;
    item->stats.food = 3;
    item->stats.hp = item->stats.maxhp = 3;
    item->level = 1;
    item->last_grace = 9;
    item->nrof = 2;
    SET_FLAG(item, FLAG_IDENTIFIED);
    item = object_insert_into(item, pl, INS_NO_MERGE);
    pl->chosen_skill = kind == 0 ? wizardry : devices;
    pl->stats.sp = 1000;
    pl->path_denied = 0;
    pl->direction = 3;
    mempool_struct *pool = mempool_find("objects");
    mempool_chunk_struct *target_storage;
    object *target = reclaimable_target(map, pool, &target_storage, true);
    draw_client_map(pl);
    ck_assert(target_player_authorized(controller, target, NULL));
    controller->target_object = target;
    controller->target_object_count = target->count;
    if (reclaim) {
        ask_show(target, "destroy_target_reclaim");
    }
    double delay = -123.0;
    size_t reserved = propagation_reserved_cells();
    uint64_t reclamation = object_reclamation_epoch();
    uint64_t spells_failed = metrics_get(&controller->metrics, METRIC_CHARACTER_SPELLS_FAILED);
    uint64_t spells_cast = metrics_get(&controller->metrics, METRIC_CHARACTER_SPELLS_CAST);

    int result = kind == 3
        ? OBJECT_METHODS(POTION)->apply_func(item, pl, 0)
        : OBJECT_METHODS(types[kind])->ranged_fire_func(item, pl, 3, &delay);

    if (reclaim) {
        ck_assert_int_eq(result, OBJECT_METHOD_UNHANDLED);
        ck_assert(!object_reclamation_is_current(reclamation));
        for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
            ck_assert_ptr_ne(puddle->first_chunk, target_storage);
        }
        ck_assert(delay == -123.0);
        ck_assert_int_eq(item->nrof, 2);
        ck_assert_int_eq(item->stats.food, 3);
        ck_assert_int_eq(item->stats.hp, 3);
        ck_assert_int_eq(pl->stats.sp, 1000);
        if (kind == 0) {
            ck_assert_uint_eq(metrics_get(&controller->metrics, METRIC_CHARACTER_SPELLS_FAILED),
                              spells_failed);
        }
        assert_no_arch(map, spellarch[SP_BULLET]);
    } else {
        ck_assert_int_eq(result, OBJECT_METHOD_OK);
        ck_assert(object_reclamation_is_current(reclamation));
        if (kind == 0) {
            ck_assert_int_lt(pl->stats.sp, 1000);
            ck_assert_uint_eq(metrics_get(&controller->metrics, METRIC_CHARACTER_SPELLS_CAST),
                              spells_cast + 1);
        } else if (kind == 1) {
            ck_assert_int_eq(item->stats.food, 2);
            ck_assert(delay == 9.0);
        } else if (kind == 2) {
            ck_assert_int_eq(item->stats.hp, 2);
            ck_assert(delay == 9.0);
        } else {
            ck_assert_int_eq(item->nrof, 1);
        }
    }
    ck_assert_uint_eq(propagation_reserved_cells(), reserved);
    controller->target_object = pl;
    controller->target_object_count = 0;
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
    tcase_add_loop_test(tc, test_target_logout_reclaims_storage_across_authorization_callers, 0, 18);
    tcase_add_loop_test(tc, test_bound_talk_rejects_callback_identity_changes, 0, 3);
    tcase_add_test(tc, test_npc_say_logout_aborts_dialogue_bookkeeping);
    tcase_add_loop_test(tc, test_bound_talk_rejects_reclaimed_npc_storage, 0, 3);
    tcase_add_loop_test(tc, test_guard_dialogue_fences_ai_and_target_notification_callbacks, 0, 4);
    tcase_add_loop_test(tc, test_guard_close_fences_gate_trigger_callbacks, 0, 3);
    tcase_add_test(tc, test_dialogue_rejects_retained_reclaimed_references_on_later_action);
    tcase_add_loop_test(tc, test_device_cast_aborts_after_target_storage_reclamation, 0, 8);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_actor_callbacks(void) {
    check_run_suite(suite(), __FILE__);
}
