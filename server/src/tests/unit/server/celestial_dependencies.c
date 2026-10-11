/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>

#include <celestial_dependencies.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <map.h>
#include <region.h>

#define RESERVATION_LIMIT ((size_t)1923584)

START_TEST(test_sampled_two_seam_change_and_unrelated_edit) {
    mapstruct maps[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        maps[i].count = (tag_t)(i + 1);
        maps[i].in_memory = MAP_IN_MEMORY;
    }
    maps[0].tile_map[TILED_EAST] = &maps[1];
    maps[1].tile_map[TILED_EAST] = &maps[2];
    maps[0].celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    maps[1].celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 8);
    ck_assert_ptr_nonnull(ctx);
    for (size_t i = 0; i < 3; i++) {
        ck_assert(celestial_dependencies_observe(ctx, &maps[i]));
    }
    ck_assert(celestial_dependencies_observe(ctx, &maps[2]));
    ck_assert_uint_eq(celestial_dependencies_count(ctx), 3);
    ck_assert(celestial_dependencies_fresh(ctx));
    maps[3].celestial_structure_revision++;
    ck_assert(celestial_dependencies_fresh(ctx));
    maps[2].celestial_structure_revision++;
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_unresolved_edge_becomes_ready) {
    mapstruct source = {0}, target = {0};
    source.in_memory = MAP_IN_MEMORY;
    target.in_memory = MAP_LOADING;
    source.tile_map[TILED_EAST] = &target;
    source.celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    /* Failed traversal still observes its source, including unready edges. */
    ck_assert(celestial_dependencies_observe(ctx, &source));
    ck_assert(celestial_dependencies_references_map(ctx, &target));
    ck_assert(celestial_dependencies_fresh(ctx));
    target.in_memory = MAP_IN_MEMORY;
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);

    source.tile_map[TILED_EAST] = NULL;
    ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe(ctx, &source));
    source.tile_map[TILED_EAST] = &target;
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_forget_guards_unready_target_and_sample_lifetime) {
    mapstruct source = {0};
    mapstruct *target = calloc(1, sizeof(*target));
    ck_assert_ptr_nonnull(target);
    target->in_memory = MAP_LOADING;
    source.tile_map[TILED_EAST] = target;
    source.celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    size_t reserved = 0;
    celestial_dependencies *pending = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    celestial_dependencies *current = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe(pending, &source));
    ck_assert(celestial_dependencies_observe(current, target));
    celestial_dependencies_forget(target);
    free(target);
    /* The dangling source edge must never be dereferenced by freshness. */
    ck_assert(!celestial_dependencies_fresh(pending));
    ck_assert(!celestial_dependencies_fresh(current));
    source.tile_map[TILED_EAST] = NULL;
    celestial_dependencies_release(pending);
    celestial_dependencies_release(current);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_incarnation_profile_transform_and_reload) {
    mapstruct map = {0};
    region_struct region = {0};
    map.region = &region;
    map.count = 7;
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe_at(ctx, &map, 24, 48, 2));
    ck_assert(celestial_dependencies_observe_at(ctx, &map, 24, 48, 2));
    ck_assert_uint_eq(celestial_dependencies_count(ctx), 1);
    ck_assert(celestial_dependencies_observe_at(ctx, &map, 24, 48, 3));
    ck_assert_uint_eq(celestial_dependencies_count(ctx), 2);
    region.celestial.digest[31] = 1;
    ck_assert(!celestial_dependencies_fresh(ctx));
    region.celestial.digest[31] = 0;
    map.count++;
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe(ctx, &map));
    ck_assert(celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
}
END_TEST

START_TEST(test_capacity_and_reservation_fail_closed_and_release) {
    mapstruct maps[2] = {0};
    size_t reserved = 0;
    ck_assert_ptr_null(celestial_dependencies_begin(&reserved, 1, 4));
    ck_assert_uint_eq(reserved, 0);
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 1);
    ck_assert_ptr_nonnull(ctx);
    ck_assert(celestial_dependencies_observe(ctx, &maps[0]));
    size_t exact = reserved;
    ck_assert_uint_eq(celestial_dependencies_bytes(ctx), exact);
    ck_assert(!celestial_dependencies_observe(ctx, &maps[1]));
    ck_assert(!celestial_dependencies_fresh(ctx));
    ck_assert_uint_eq(reserved, exact);
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
    ctx = celestial_dependencies_begin(&reserved, exact - 1, 1);
    ck_assert_ptr_nonnull(ctx);
    ck_assert(!celestial_dependencies_observe(ctx, &maps[0]));
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
    ctx = celestial_dependencies_begin(&reserved, exact, 1);
    ck_assert(celestial_dependencies_observe(ctx, &maps[0]));
    ck_assert(celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_current_next_publication_snapshot_and_unload) {
    mapstruct map = {0};
    mapstruct *maps[] = {&map};
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe(ctx, &map));
    ck_assert(celestial_dependencies_finish(ctx, maps, 1, false));
    ck_assert_uint_eq(reserved, 0);
    celestial_dependencies_release(ctx);
    ck_assert(celestial_dependencies_field_fresh(&map, false));
    ck_assert(!celestial_dependencies_field_fresh(&map, true));
    celestial_dependencies *snapshot = celestial_dependencies_field_get(&map, false);
    ck_assert_ptr_nonnull(snapshot);
    ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert(celestial_dependencies_observe(ctx, &map));
    ck_assert(celestial_dependencies_finish(ctx, maps, 1, true));
    celestial_dependencies_release(ctx);
    ck_assert(celestial_dependencies_field_fresh(&map, true));
    ck_assert(celestial_dependencies_field_bind(snapshot, &map, false));
    celestial_dependencies_release(snapshot);
    celestial_dependencies_forget(&map);
    ck_assert(!celestial_dependencies_field_fresh(&map, false));
    ck_assert(!celestial_dependencies_field_fresh(&map, true));
    celestial_dependencies_field_forget(&map);
    ck_assert_ptr_null(celestial_dependencies_field_get(&map, false));
    ck_assert_ptr_null(celestial_dependencies_field_get(&map, true));
}
END_TEST

START_TEST(test_retained_capacity_failure_is_atomic) {
    mapstruct maps[70] = {0};
    mapstruct *output[] = {&maps[0]};
    size_t reserved = 0;
    celestial_dependencies *original = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 1);
    ck_assert(celestial_dependencies_observe(original, &maps[0]));
    ck_assert(celestial_dependencies_finish(original, output, 1, false));
    celestial_dependencies_release(original);
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 70);
    for (size_t i = 0; i < 70; i++) {
        ck_assert(celestial_dependencies_observe(ctx, &maps[i]));
    }
    ck_assert_uint_gt(celestial_dependencies_bytes(ctx), 8192);
    ck_assert(!celestial_dependencies_finish(ctx, output, 1, false));
    ck_assert(celestial_dependencies_field_fresh(&maps[0], false));
    celestial_dependencies_release(ctx);
    celestial_dependencies_field_forget(&maps[0]);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_large_endpoint_promotion_charges_shared_context_once) {
    mapstruct maps[40] = {0};
    mapstruct *output[] = {&maps[0]};
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 40);
    for (size_t i = 0; i < 40; i++) {
        ck_assert(celestial_dependencies_observe(ctx, &maps[i]));
    }
    ck_assert_uint_gt(celestial_dependencies_bytes(ctx), 4096);
    ck_assert_uint_lt(celestial_dependencies_bytes(ctx), 8192);
    ck_assert(celestial_dependencies_finish(ctx, output, 1, true));
    ck_assert(celestial_dependencies_field_bind(ctx, &maps[0], false));
    celestial_dependencies_field_release(&maps[0], true);
    celestial_dependencies_release(ctx);
    ck_assert(celestial_dependencies_field_fresh(&maps[0], false));
    celestial_dependencies_field_forget(&maps[0]);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_atomic_replacement_credits_only_released_contexts) {
    mapstruct maps[40] = {0};
    mapstruct *output[] = {&maps[0]};
    size_t reserved = 0;
    for (size_t replacement = 0; replacement < 2; replacement++) {
        celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 40);
        for (size_t i = 0; i < 40; i++) {
            ck_assert(celestial_dependencies_observe(ctx, &maps[i]));
        }
        ck_assert(celestial_dependencies_finish(ctx, output, 1, false));
        celestial_dependencies_release(ctx);
    }
    celestial_dependencies *snapshot = celestial_dependencies_field_get(&maps[0], false);
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 40);
    for (size_t i = 0; i < 40; i++) {
        ck_assert(celestial_dependencies_observe(ctx, &maps[i]));
    }
    ck_assert(!celestial_dependencies_finish(ctx, output, 1, false));
    ck_assert(celestial_dependencies_field_fresh(&maps[0], false));
    celestial_dependencies_release(ctx);
    celestial_dependencies_release(snapshot);
    celestial_dependencies_field_forget(&maps[0]);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_discontinuous_and_travel_targets_are_independent) {
    mapstruct source = {0}, target = {0};
    source.tile_map[TILED_EAST] = &target;
    source.celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    size_t reserved = 0;
    for (size_t kind = 0; kind < 2; kind++) {
        celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
        ck_assert(celestial_dependencies_observe(ctx, &source));
        ck_assert(!celestial_dependencies_references_map(ctx, &target));
        celestial_dependencies_forget(&target);
        target.in_memory = MAP_IN_MEMORY;
        ck_assert(celestial_dependencies_fresh(ctx));
        source.tile_map[TILED_EAST] = NULL;
        ck_assert(celestial_dependencies_fresh(ctx));
        celestial_dependencies_release(ctx);
        source.tile_map[TILED_EAST] = &target;
        source.celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_UNSET;
    }
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

START_TEST(test_implicit_world_region_normalization_preserves_freshness) {
    mapstruct map = {0};
    size_t reserved = 0;
    celestial_dependencies *ctx = celestial_dependencies_begin(&reserved, RESERVATION_LIMIT, 4);
    ck_assert_ptr_nonnull(region_world());
    ck_assert(celestial_dependencies_observe(ctx, &map));
    map.region = region_world();
    ck_assert(celestial_dependencies_fresh(ctx));
    region_struct changed = {0};
    changed.celestial = map.region->celestial;
    changed.celestial.digest[0] ^= 1;
    map.region = &changed;
    ck_assert(!celestial_dependencies_fresh(ctx));
    celestial_dependencies_release(ctx);
    ck_assert_uint_eq(reserved, 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_dependencies");
    TCase *tc = tcase_create("Core");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc);
    tcase_add_test(tc, test_sampled_two_seam_change_and_unrelated_edit);
    tcase_add_test(tc, test_unresolved_edge_becomes_ready);
    tcase_add_test(tc, test_forget_guards_unready_target_and_sample_lifetime);
    tcase_add_test(tc, test_incarnation_profile_transform_and_reload);
    tcase_add_test(tc, test_capacity_and_reservation_fail_closed_and_release);
    tcase_add_test(tc, test_current_next_publication_snapshot_and_unload);
    tcase_add_test(tc, test_retained_capacity_failure_is_atomic);
    tcase_add_test(tc, test_large_endpoint_promotion_charges_shared_context_once);
    tcase_add_test(tc, test_atomic_replacement_credits_only_released_contexts);
    tcase_add_test(tc, test_discontinuous_and_travel_targets_are_independent);
    tcase_add_test(tc, test_implicit_world_region_normalization_preserves_freshness);
    return s;
}

void check_server_celestial_dependencies(void) {
    check_run_suite(suite(), __FILE__);
}
