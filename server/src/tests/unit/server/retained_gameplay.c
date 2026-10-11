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
#include <player.h>
#include <propagation.h>
#include <server.h>
#include <spells.h>
#include <toolkit/mempool.h>
#include <toolkit/packet.h>

static object *place(mapstruct *map, const char *arch, int x, int y) {
    object *op = arch_get(arch);
    ck_assert_ptr_nonnull(op);
    op->x = x;
    op->y = y;
    op = object_insert_map(op, map, NULL, INS_NO_MERGE | INS_NO_WALK_ON);
    ck_assert_ptr_nonnull(op);
    if (op->type == MONSTER) {
        monster_data_init(op);
    }
    return op;
}

/* Exhaust old free slots before allocating the sole live object in a fresh
 * puddle. Every retained reference below is established before reclamation. */
static object *isolated_target(mapstruct *map, mempool_chunk_struct **storage) {
    mempool_struct *pool = mempool_find("objects");
    ck_assert_ptr_nonnull(pool);
    size_t num = pool->nrof_free[0];
    object **held = xcalloc(MAX(num, 1), sizeof(*held));
    for (size_t i = 0; i < num; i++) {
        held[i] = object_get();
    }
    ck_assert_uint_eq(pool->nrof_free[0], 0);
    object *target = place(map, "empty_archetype", 12, 8);
    *storage = pool->puddlelist[0]->first_chunk;
    uintptr_t address = (uintptr_t)MEM_POOLDATA(target);
    uintptr_t first = (uintptr_t)*storage;
    ck_assert(address >= first && address < first +
        pool->expand_size * (sizeof(mempool_chunk_struct) + pool->chunksize));
    for (size_t i = 0; i < num; i++) {
        object_destroy(held[i]);
    }
    free(held);
    return target;
}

static void destroy_and_reclaim(object *target, mempool_chunk_struct *storage) {
    /* Use the actual non-currency Python Destroy hook and /memfree allocator
     * operation, rather than changing counts or mocking unreadable memory. */
    ck_assert_int_eq(object_remove_reason(target, "script.item-destroy", true),
                     OBJECT_SEMANTIC_COMMITTED);
    mempool_struct *pool = mempool_find("objects");
    ck_assert_uint_gt(mempool_reclaim(pool), 0);
    for (mempool_puddle_struct *puddle = pool->puddlelist[0]; puddle != NULL;
         puddle = puddle->next) {
        ck_assert_ptr_ne(puddle->first_chunk, storage);
    }
}

static void fixture(mapstruct **map, object **pl) {
    toolkit_import(packet);
    check_setup_env_pl(map, pl);
    object_remove(*pl, 0);
    (*pl)->x = (*pl)->y = 12;
    *pl = object_insert_map(*pl, *map, NULL, INS_NO_MERGE);
    for (int x = 0; x < MAP_WIDTH(*map); x++) {
        for (int y = 0; y < MAP_HEIGHT(*map); y++) {
            GET_MAP_SPACE_PTR(*map, x, y)->light_value = 1280;
        }
    }
    init_spells();
}

START_TEST(test_dialogs_reject_reclaimed_activator) {
    mapstruct *map = get_empty_map(24, 24);
    object *monster = place(map, "raas", 12, 12);
    object *live = place(map, "empty_archetype", 13, 12);
    mempool_chunk_struct *storage;
    object *target = isolated_target(map, &storage);
    monster_data_dialogs_add(monster, target, 300);
    monster_data_dialogs_add(monster, live, 300);
    ck_assert_uint_eq(monster_data_dialogs_num(monster), 2);
    destroy_and_reclaim(target, storage);

    if (_i == 0) {
        ck_assert_uint_eq(monster_data_dialogs_num(monster), 1);
    } else if (_i == 1) {
        monster_data_dialogs_purge(monster);
        ck_assert_uint_eq(monster_data_dialogs_num(monster), 0);
    } else {
        MONSTER_DATA(monster)->last_cleanup = pticks -
            MONSTER_DATA_INTERFACE_CLEANUP * MAX_TICKS;
        monster_data_dialogs_cleanup(monster);
        ck_assert_uint_eq(monster_data_dialogs_num(monster), 1);
        ck_assert(monster_data_dialogs_check(monster, live));
    }
    delete_map(map);
}
END_TEST

START_TEST(test_arrow_rejects_reclaimed_selected_target) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *shooter = _i == 0 ? pl : place(map, "raas", 10, 12);
    object *arrow = arch_get("arrow");
    arrow->weight = MAX(1, arrow->weight);
    arrow->last_sp = 8;
    CLEAR_FLAG(arrow, FLAG_STARTEQUIP);
    CLEAR_FLAG(arrow, FLAG_NO_DROP);
    arrow = object_insert_into(arrow, shooter, INS_NO_MERGE);
    mempool_chunk_struct *storage;
    object *target = isolated_target(map, &storage);
    if (_i == 0) {
        CONTR(shooter)->target_object = target;
        CONTR(shooter)->target_object_count = target->count;
    } else {
        shooter->enemy = target;
        shooter->enemy_count = target->count;
    }
    destroy_and_reclaim(target, storage);

    /* Call the registered arrow method directly: the generic ranged wrapper
     * has its own earlier retained-target gate, covered in actor tests. */
    ck_assert_int_eq(OBJECT_METHODS(ARROW)->ranged_fire_func(arrow, shooter, 0, NULL),
                     OBJECT_METHOD_UNHANDLED);
    ck_assert_ptr_eq(arrow->env, shooter);
    ck_assert_uint_eq(arrow->projectile.version, 0);
    /* A directional throw remains possible after losing the target. */
    ck_assert_int_eq(OBJECT_METHODS(ARROW)->ranged_fire_func(arrow, shooter, 3, NULL),
                     OBJECT_METHOD_OK);
    ck_assert_ptr_eq(arrow->map, map);
    ck_assert_uint_ne(arrow->projectile.version, 0);
}
END_TEST

START_TEST(test_homing_bullet_next_tick_after_target_reclamation) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *bullet = arch_to_object(spellarch[SP_MAGIC_MISSILE]);
    ck_assert_ptr_nonnull(bullet);
    bullet->x = bullet->y = 12;
    bullet->direction = 3;
    bullet->last_sp = 8;
    bullet->stats.sp = SP_MAGIC_MISSILE;
    bullet = object_insert_map(bullet, map, NULL, INS_NO_MERGE | INS_NO_WALK_ON);
    object_owner_set(bullet, pl);
    mempool_chunk_struct *storage;
    object *target = isolated_target(map, &storage);
    bullet->enemy = target;
    bullet->enemy_count = target->count;
    if (_i != 0) {
        destroy_and_reclaim(target, storage);
    }

    OBJECT_METHODS(BULLET)->process_func(bullet);

    ck_assert_uint_ne(bullet->projectile.version, 0);
    ck_assert_int_eq(bullet->projectile.heading[0], _i == 0 ? 0 : 65536);
    ck_assert_int_eq(bullet->projectile.heading[1], _i == 0 ? -65536 : 0);
    ck_assert_int_eq(bullet->last_sp, 7);
}
END_TEST

START_TEST(test_swarm_next_tick_after_target_reclamation) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *swarm = place(map, "empty_archetype", 12, 12);
    swarm->type = SWARM_SPELL;
    swarm->direction = 3;
    swarm->stats.hp = 1;
    swarm->stats.sp = SP_BULLET;
    swarm->other_arch = spellarch[SP_BULLET];
    object_owner_set(swarm, pl);
    mempool_chunk_struct *storage;
    object *target = isolated_target(map, &storage);
    swarm->enemy = target;
    swarm->enemy_count = target->count;
    destroy_and_reclaim(target, storage);

    OBJECT_METHODS(SWARM_SPELL)->process_func(swarm);

    ck_assert_int_eq(swarm->stats.hp, 0);
    object *child = NULL;
    FOR_MAP_PREPARE(map, 13, 12, op) {
        if (op->arch == spellarch[SP_BULLET]) {
            child = op;
            break;
        }
    }
    FOR_MAP_FINISH();
    ck_assert_ptr_nonnull(child);
    ck_assert_ptr_null(child->enemy);
    ck_assert_int_eq(child->projectile.heading[0], 65536);
}
END_TEST

static unsigned visits;
static void visit(object *source, mapstruct *map, int x, int y, int argument) {
    (void)source; (void)map; (void)x; (void)y; (void)argument;
    visits++;
}

START_TEST(test_propagation_pending_source_after_reclamation) {
    mapstruct *map = get_empty_map(24, 24);
    mempool_chunk_struct *storage;
    object *source = isolated_target(map, &storage);
    visits = 0;
    ck_assert(propagation_area(source, 1, visit, 0));
    if (_i != 0) {
        destroy_and_reclaim(source, storage);
    }
    propagation_tick();
    ck_assert_uint_eq(visits, _i == 0 ? 9 : 0);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

START_TEST(test_propagation_pending_owner_after_reclamation) {
    mapstruct *map = get_empty_map(24, 24);
    object *source = place(map, "empty_archetype", 12, 12);
    mempool_chunk_struct *storage;
    object *owner = isolated_target(map, &storage);
    object_owner_set(source, owner);
    ck_assert(propagation_cone(source, source, arch_find("firebreath"), 1, 0, 0, 0, 0));
    if (_i != 0) {
        destroy_and_reclaim(owner, storage);
    }
    propagation_tick();
    object *child = NULL;
    FOR_MAP_PREPARE(map, 13, 12, op) {
        if (op->type == CONE) {
            child = op;
            break;
        }
    }
    FOR_MAP_FINISH();
    ck_assert_ptr_nonnull(child);
    ck_assert_ptr_eq(object_owner(child), _i == 0 ? owner : NULL);
    /* Cancellation must still destroy an allocated, removed effect. */
    tag_t count = child->count;
    object_remove(child, 0);
    propagation_expire_map(map);
    ck_assert(!object_reference_matches(child, count));
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

void check_server_retained_gameplay(void) {
    Suite *suite = suite_create("retained_gameplay");
    TCase *core = tcase_create("Core");
    tcase_add_unchecked_fixture(core, check_setup, check_teardown);
    tcase_add_checked_fixture(core, check_test_setup, check_test_teardown);
    tcase_add_loop_test(core, test_dialogs_reject_reclaimed_activator, 0, 3);
    tcase_add_loop_test(core, test_arrow_rejects_reclaimed_selected_target, 0, 2);
    tcase_add_loop_test(core, test_homing_bullet_next_tick_after_target_reclamation, 0, 2);
    tcase_add_test(core, test_swarm_next_tick_after_target_reclamation);
    tcase_add_loop_test(core, test_propagation_pending_source_after_reclamation, 0, 2);
    tcase_add_loop_test(core, test_propagation_pending_owner_after_reclamation, 0, 2);
    suite_add_tcase(suite, core);
    check_run_suite(suite, __FILE__);
}
