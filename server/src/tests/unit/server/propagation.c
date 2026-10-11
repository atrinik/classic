/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <arch.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <propagation.h>
#include <object_methods.h>

static unsigned visits[3], cells[24][24];
static void visit(object *source, mapstruct *map, int x, int y, int argument) {
    (void)source;
    (void)map;
    visits[argument]++;
    if (argument == 0) {
        ck_assert_int_ge(x, 0);
        ck_assert_int_lt(x, 24);
        ck_assert_int_ge(y, 0);
        ck_assert_int_lt(y, 24);
        ck_assert_uint_eq(cells[x][y]++, 0);
    }
}

static object *source_on(mapstruct *map, int x, int y) {
    object *source = arch_get("empty_archetype");
    source->x = x;
    source->y = y;
    source->level = 1;
    return object_insert_map(source, map, NULL, INS_NO_MERGE);
}

START_TEST(test_area_convergence_and_later_events) {
    mapstruct *map = get_empty_map(24, 24);
    object *source = source_on(map, 12, 12);
    memset(visits, 0, sizeof(visits));
    memset(cells, 0, sizeof(cells));
    ck_assert(propagation_area(source, 3, visit, 0));
    propagation_tick();
    ck_assert_uint_eq(visits[0], 49);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    memset(cells, 0, sizeof(cells));
    ck_assert(propagation_area(source, 3, visit, 0));
    propagation_tick();
    ck_assert_uint_eq(visits[0], 98);
    delete_map(map);
}
END_TEST

START_TEST(test_area_global_admission_and_map_expiry) {
    mapstruct *map = get_empty_map(24, 24);
    object *source = source_on(map, 12, 12);
    ck_assert(!propagation_can_admit(-1));
    ck_assert(!propagation_can_admit(36));
    unsigned admitted = 0;
    while (propagation_area(source, 35, visit, 1)) {
        admitted++;
        ck_assert_uint_le(admitted, 16);
    }
    ck_assert_uint_eq(admitted, 16);
    ck_assert_uint_le(propagation_reserved_cells(), PROPAGATION_GLOBAL_CELLS);
    propagation_expire_map(map);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

START_TEST(test_global_budget_retains_fair_pending_work) {
    mapstruct *map = get_empty_map(24, 24);
    object *source = source_on(map, 12, 12);
    memset(visits, 0, sizeof(visits));
    for (unsigned i = 0; i < PROPAGATION_TICK_EXPANSIONS + 2; i++) {
        ck_assert(propagation_area(source, 0, visit, 1 + i % 2));
    }
    propagation_tick();
    ck_assert_uint_eq(propagation_last_expansions(), PROPAGATION_TICK_EXPANSIONS);
    ck_assert_uint_eq(visits[1], PROPAGATION_TICK_EXPANSIONS / 2);
    ck_assert_uint_eq(visits[2], PROPAGATION_TICK_EXPANSIONS / 2);
    ck_assert_uint_eq(propagation_reserved_cells(), 2);
    propagation_tick();
    ck_assert_uint_eq(propagation_last_expansions(), 2);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

START_TEST(test_pending_source_destruction_and_reservation_cleanup) {
    mapstruct *map = get_empty_map(24, 24);
    object *source = source_on(map, 12, 12);
    memset(visits, 0, sizeof(visits));
    ck_assert(propagation_area(source, 3, visit, 1));
    object_destruct(source);
    propagation_tick();
    ck_assert_uint_eq(visits[1], 0);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    source = source_on(map, 12, 12);
    ck_assert(propagation_reserve_explosion(source, 3));
    object *copy = object_clone(source);
    ck_assert_ptr_null(copy->propagation);
    object_destroy(copy);
    propagation_expire_map(map);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

static mapstruct *observed_map;
static unsigned observed_visits;
static void visit_map(object *source, mapstruct *map, int x, int y, int argument) {
    (void)source; (void)x; (void)y; (void)argument;
    if (map == observed_map) {
        observed_visits++;
    }
}

START_TEST(test_spread_around_cover_but_not_closed_story_slabs) {
    mapstruct *lower = get_empty_map(5, 5), *upper = get_empty_map(5, 5);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    object *source = source_on(lower, 2, 2);
    object *opening = NULL;
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            object *floor = arch_get("empty_archetype");
            SET_FLAG(floor, FLAG_IS_FLOOR);
            floor->layer = LAYER_FLOOR;
            floor->x = x; floor->y = y;
            floor = object_insert_map(floor, upper, NULL, INS_NO_MERGE);
            if (x == 3 && y == 2) {
                opening = floor;
            }
        }
    }
    observed_map = upper;
    observed_visits = 0;
    ck_assert(propagation_area(source, 2, visit_map, 0));
    propagation_tick();
    ck_assert_uint_eq(observed_visits, 0);
    object_destruct(opening);
    ck_assert(propagation_area(source, 2, visit_map, 0));
    propagation_tick();
    ck_assert_uint_gt(observed_visits, 0);
    delete_map(lower);
    delete_map(upper);
}
END_TEST

START_TEST(test_cone_children_expire_together_across_stories) {
    mapstruct *lower = get_empty_map(5, 5), *upper = get_empty_map(5, 5);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    object *source = source_on(lower, 2, 2);
    ck_assert(propagation_cone(source, source, arch_find("firebreath"), 3, 0, 0, 0, 0));
    propagation_tick();
    ck_assert_uint_gt(propagation_reserved_cells(), 0);
    object *effect = NULL;
    FOR_MAP_PREPARE(lower, 3, 2, candidate) {
        if (candidate->type == CONE) {
            effect = candidate;
            break;
        }
    }
    FOR_MAP_FINISH();
    ck_assert_ptr_nonnull(effect);
    tag_t count = effect->count;
    ck_assert_ptr_nonnull(effect->propagation);
    object_process(effect);
    ck_assert(OBJECT_VALID(effect, count));
    propagation_expire_map(upper);
    ck_assert(!OBJECT_VALID(effect, count));
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    propagation_tick();
    ck_assert_uint_eq(propagation_last_expansions(), 0);
    delete_map(lower);
    delete_map(upper);
}
END_TEST

START_TEST(test_swarm_reservation_transfer_and_group_expiry) {
    mapstruct *lower = get_empty_map(5, 5), *upper = get_empty_map(5, 5);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    object *parent = source_on(lower, 2, 2);
    parent->type = SWARM_SPELL;
    ck_assert(!propagation_reserve_swarm(parent, 35, 3));
    ck_assert(propagation_reserve_swarm(parent, 3, 3));
    size_t reserved_before = propagation_reserved_cells();
    object *child = source_on(upper, 2, 2);
    ck_assert(propagation_reserve_explosion_from(child, parent, 3));
    ck_assert_uint_eq(propagation_reserved_cells(), reserved_before);
    tag_t parent_count = parent->count, child_count = child->count;
    propagation_expire_map(upper);
    ck_assert(!OBJECT_VALID(parent, parent_count));
    ck_assert(!OBJECT_VALID(child, child_count));
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(lower);
    delete_map(upper);
}
END_TEST

START_TEST(test_rain_heal_no_target_fails_without_reservation) {
    mapstruct *map = get_empty_map(5, 5);
    object *source = source_on(map, 2, 2);
    ck_assert_int_eq(cast_heal_around(source, 1, SP_RAIN_HEAL), 0);
    ck_assert_uint_eq(propagation_reserved_cells(), 0);
    delete_map(map);
}
END_TEST

void check_server_propagation(void) {
    Suite *suite = suite_create("propagation");
    TCase *core = tcase_create("Core");
    tcase_add_unchecked_fixture(core, check_setup, check_teardown);
    tcase_add_checked_fixture(core, check_test_setup, check_test_teardown);
    tcase_add_test(core, test_area_convergence_and_later_events);
    tcase_add_test(core, test_area_global_admission_and_map_expiry);
    tcase_add_test(core, test_global_budget_retains_fair_pending_work);
    tcase_add_test(core, test_pending_source_destruction_and_reservation_cleanup);
    tcase_add_test(core, test_spread_around_cover_but_not_closed_story_slabs);
    tcase_add_test(core, test_cone_children_expire_together_across_stories);
    tcase_add_test(core, test_swarm_reservation_transfer_and_group_expiry);
    tcase_add_test(core, test_rain_heal_no_target_fails_without_reservation);
    suite_add_tcase(suite, core);
    check_run_suite(suite, __FILE__);
}
