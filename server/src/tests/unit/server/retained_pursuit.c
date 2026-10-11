/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <monster_data.h>
#include <object.h>
#include <object_methods.h>
#include <pathfinder.h>
#include <perception.h>
#include <player.h>
#include <plugin.h>
#include <server_main.h>
#include <toolkit/mempool.h>
#include <toolkit/packet.h>
#include <waypoint.h>

static bool plugins_initialized;

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

/* Exhaust existing free chunks so this actor's backing allocation can actually
 * be returned by /memfree, independently of the older surviving pursuer. */
static object *reclaimable_target(mapstruct *map, mempool_struct *pool,
                                  mempool_chunk_struct **storage, const char *arch_name) {
    size_t count = pool->nrof_free[0];
    object **held = xcalloc(MAX(count, 1), sizeof(*held));
    for (size_t i = 0; i < count; i++) {
        held[i] = object_get();
    }
    ck_assert_uint_eq(pool->nrof_free[0], 0);
    object *target = arch_get(arch_name);
    ck_assert_ptr_nonnull(target);
    target->x = 18;
    target->y = 12;
    target = object_insert_map(target, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(target);
    if (target->type == MONSTER) {
        monster_data_init(target);
    }
    *storage = pool->puddlelist[0]->first_chunk;
    uintptr_t address = (uintptr_t)MEM_POOLDATA(target);
    uintptr_t first = (uintptr_t)*storage;
    ck_assert(address >= first &&
              address < first + pool->expand_size * (sizeof(mempool_chunk_struct) + pool->chunksize));
    for (size_t i = 0; i < count; i++) {
        object_destroy(held[i]);
    }
    xfree(held);
    return target;
}

static void assert_reclaimed(mempool_struct *pool, mempool_chunk_struct *storage) {
    for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL; puddle = puddle->next) {
        ck_assert_ptr_ne(puddle->first_chunk, storage);
    }
}

START_TEST(test_retained_pursuit_after_destroy_and_reclaim) {
    bool reclaim = _i % 2 == 0;
    int operation = _i / 2;
    mapstruct *map;
    object *pl;
    toolkit_import(packet);
    check_setup_env_pl(&map, &pl);
    FREE_AND_COPY_HASH(map->path, "/python/retained-pursuit");
    object_remove(pl, 0);
    pl->x = 12;
    pl->y = 12;
    pl = object_insert_map(pl, map, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    CONTR(pl)->cmd_permissions = xcalloc(1, sizeof(char *));
    CONTR(pl)->cmd_permissions[0] = xstrdup("memfree");
    CONTR(pl)->num_cmd_permissions = 1;
    CLEAR_FLAG(pl, FLAG_BLIND);
    CLEAR_FLAG(pl, FLAG_XRAYS);
    if (!plugins_initialized) {
        init_plugins();
        plugins_initialized = true;
    }

    object *npc = place(map, 17, 12);
    object *base = living_get_base_info(npc);
    base->speed_left = 0.25;
    CLEAR_FLAG(npc, FLAG_SLOW_MOVE);
    npc->behavior = 0;
    object *waypoint = object_insert_into(arch_get("waypoint"), npc, 0);
    ck_assert_ptr_nonnull(waypoint);
    SET_FLAG(waypoint, FLAG_DAMNED);
    waypoint->stats.hp = 7;
    waypoint->stats.sp = 8;
    object *spawn = object_get();
    spawn->type = SPAWN_POINT;
    spawn->x = 10;
    spawn->y = 10;
    spawn = object_insert_map(spawn, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(spawn);
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *storage;
    object *target = reclaimable_target(map, pool, &storage, "raas");
    tag_t target_count = target->count;
    npc->enemy = waypoint->enemy = spawn->enemy = target;
    npc->enemy_count = waypoint->enemy_count = spawn->enemy_count = target_count;
    if (operation == 0) {
        path_request(waypoint);
    }
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_ASK_SHOW;
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "events/python_unit_actor_callbacks.py");
    FREE_AND_COPY_HASH(event->slaying, reclaim ? "destroy_target_reclaim" : "visible");
    object_insert_into(event, target, INS_NO_MERGE);
    uint64_t epoch = object_reclamation_epoch();

    ck_assert_int_eq(perception_identity_visible(pl, target), !reclaim);
    if (reclaim) {
        ck_assert(!object_reclamation_is_current(epoch));
        assert_reclaimed(pool, storage);
    } else {
        ck_assert(object_reclamation_is_current(epoch));
        ck_assert_str_eq(object_get_value(target, "actor_callback_done"), "1");
    }

    /* These are independent operations after callback return: a callback-local
     * epoch fence cannot make any of the retained pointer/count pairs safe. */
    switch (operation) {
        case 0:
            ck_assert_ptr_eq(path_get_next_request(), reclaim ? NULL : waypoint);
            break;
        case 1:
            set_mobile_speed(npc, 0);
            ck_assert_double_eq(npc->speed, reclaim ? 0.5 : 1.0);
            break;
        case 2:
            SET_MAP_FLAGS(map, 5, 5, P_BLOCKSVIEW);
            ck_assert_int_eq(path_tile_blocked(npc, map, 5, 5), reclaim ? P_BLOCKSVIEW : 0);
            break;
        case 3:
            waypoint_move(waypoint, npc);
            ck_assert_int_eq(waypoint->stats.hp, reclaim ? 7 : 18);
            ck_assert_int_eq(waypoint->stats.sp, reclaim ? 8 : 12);
            break;
        case 4:
            waypoint_compute_path(waypoint);
            ck_assert_int_eq(waypoint->stats.hp, reclaim ? 7 : 18);
            ck_assert_int_eq(waypoint->stats.sp, reclaim ? 8 : 12);
            break;
        case 5:
            object_process(spawn);
            ck_assert_ptr_eq(spawn->enemy, target);
            ck_assert_uint_eq(spawn->enemy_count, target_count);
            break;
        default:
            /* Leave processing speed credit negative so this tick exercises
             * animation cleanup independently of monster AI clearing enemy. */
            npc->speed = 0.25;
            npc->speed_left = -100.0;
            object_update_speed(npc);
            CLEAR_FLAG(npc, FLAG_ANIMATE);
            npc->anim_flags = ANIM_FLAG_ATTACKING | ANIM_FLAG_STOP_ATTACKING;
            if (!reclaim) {
                target->speed_left = -100.0;
            }
            process_events();
            ck_assert_int_eq((npc->anim_flags & ANIM_FLAG_ATTACKING) != 0, !reclaim);
            ck_assert_int_eq(npc->anim_flags & ANIM_FLAG_STOP_ATTACKING, 0);
            break;
    }
}
END_TEST

START_TEST(test_queue_skips_reclaimed_waypoint_or_owner) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *npc = place(map, 12, 12);
    object *waypoint = object_insert_into(arch_get("waypoint"), npc, 0);
    object *good = object_insert_into(arch_get("waypoint"), npc, 0);
    ck_assert_ptr_nonnull(waypoint);
    ck_assert_ptr_nonnull(good);
    SET_FLAG(good, FLAG_CURSED);
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    mempool_chunk_struct *storage;
    object *victim = reclaimable_target(map, pool, &storage, _i == 0 ? "waypoint" : "raas");
    if (_i == 0) {
        /* A queued waypoint can be destroyed while its NPC survives. */
        object_remove(victim, 0);
        victim = object_insert_into(victim, npc, 0);
        SET_FLAG(victim, FLAG_CURSED);
        path_request(victim);
    } else {
        /* A waypoint can survive removal from its original owner, but the
         * queued saved owner identity must still be checked before use. */
        object_remove(waypoint, 0);
        waypoint = object_insert_into(waypoint, victim, 0);
        SET_FLAG(waypoint, FLAG_CURSED);
        path_request(waypoint);
        object_remove(waypoint, 0);
        waypoint = object_insert_into(waypoint, npc, 0);
    }
    path_request(good);
    object_remove(victim, 0);
    object_destroy(victim);
    ck_assert_uint_gt(mempool_reclaim(pool), 0);
    assert_reclaimed(pool, storage);

    ck_assert_ptr_eq(path_get_next_request(), good);
    ck_assert_ptr_null(path_get_next_request());
}
END_TEST

START_TEST(test_retained_enemy_rejects_removed_or_wrong_identity) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *npc = place(map, 12, 12);
    object *target = place(map, 14, 12);
    object *base = living_get_base_info(npc);
    base->speed_left = 0.25;
    CLEAR_FLAG(npc, FLAG_SLOW_MOVE);
    object *waypoint = object_insert_into(arch_get("waypoint"), npc, 0);
    SET_FLAG(waypoint, FLAG_DAMNED);
    npc->enemy = waypoint->enemy = target;
    npc->enemy_count = waypoint->enemy_count = target->count;
    waypoint->stats.hp = 7;
    waypoint->stats.sp = 8;
    path_request(waypoint);
    if (_i == 0) {
        object_remove(target, 0);
    } else {
        npc->enemy_count++;
        waypoint->enemy_count++;
    }

    ck_assert_ptr_null(path_get_next_request());
    set_mobile_speed(npc, 0);
    ck_assert_double_eq(npc->speed, 0.5);
    waypoint_move(waypoint, npc);
    ck_assert_int_eq(waypoint->stats.hp, 7);
    ck_assert_int_eq(waypoint->stats.sp, 8);
}
END_TEST

void check_server_retained_pursuit(void) {
    Suite *s = suite_create("server.retained_pursuit");
    TCase *tc = tcase_create("retained references");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    tcase_add_loop_test(tc, test_retained_pursuit_after_destroy_and_reclaim, 0, 14);
    tcase_add_loop_test(tc, test_queue_skips_reclaimed_waypoint_or_owner, 0, 2);
    tcase_add_loop_test(tc, test_retained_enemy_rejects_removed_or_wrong_identity, 0, 2);
    suite_add_tcase(s, tc);
    check_run_suite(s, __FILE__);
}
