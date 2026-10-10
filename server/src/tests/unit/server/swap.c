/*************************************************************************
 *   Copyright 2026 The Atrinik Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <initialization.h>
#include <swap.h>
#include <arch.h>
#include <object.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>

static mapstruct *expiring_map(const char *directory, const char *name) {
    mapstruct *map = get_empty_map(2, 2);
    char path[HUGE_BUF];
    int written = snprintf(VS(path), "%s/%s", directory, name);
    ck_assert_int_ge(written, 0);
    ck_assert_uint_lt((size_t)written, sizeof(path));
    FREE_AND_COPY_HASH(map->path, path);
    map->map_flags |= MAP_FLAG_UNIQUE | MAP_FLAG_FIXED_RTIME;
    map->reset_time = UINT32_MAX;
    map->timeout = 1;
    return map;
}

static void discard_map(mapstruct *map) {
    char path[HUGE_BUF];
    snprintf(VS(path), "%s", map->path);
    delete_map(map);
    if (access(path, F_OK) == 0) {
        ck_assert_int_eq(unlink(path), 0);
    }
}

START_TEST(test_expiry_is_bounded_and_fifo) {
    char directory[] = "/tmp/atrinik-swap-fifo-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *maps[4];
    for (size_t i = 0; i < arraysize(maps); i++) {
        char name[32];
        snprintf(VS(name), "map-%zu", i);
        maps[i] = expiring_map(directory, name);
    }
    /* A negative explicit timeout is already expired, without underflow. */
    maps[1]->timeout = INT32_MIN;

    for (size_t tick = 0; tick < arraysize(maps); tick++) {
        check_active_maps();
        for (size_t i = 0; i < arraysize(maps); i++) {
            ck_assert_uint_eq(maps[i]->in_memory, i <= tick ? MAP_SWAPPED : MAP_IN_MEMORY);
            if (i > tick) {
                ck_assert_uint_ne(maps[i]->swap_pending_order, 0);
                ck_assert_uint_eq(maps[i]->swap_pending_count, maps[i]->count);
                ck_assert_int_eq(maps[i]->timeout, 0);
            } else {
                ck_assert_uint_eq(maps[i]->swap_pending_order, 0);
                ck_assert_int_eq(access(maps[i]->path, F_OK), 0);
            }
        }
    }

    for (size_t i = 0; i < arraysize(maps); i++) {
        discard_map(maps[i]);
    }
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_old_map_flush_preserves_waiting_expiry) {
    char directory[] = "/tmp/atrinik-swap-old-flush-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *discarded = expiring_map(directory, "discarded");
    discarded->reset_time = 0;
    mapstruct *waiting = expiring_map(directory, "waiting");
    check_active_maps();
    /* The immediately resetting first map has been deleted. */
    ck_assert_uint_ne(waiting->swap_pending_order, 0);
    uint64_t order = waiting->swap_pending_order;
    flush_old_maps();
    ck_assert_uint_eq(waiting->swap_pending_order, order);
    ck_assert_int_eq(waiting->timeout, 0);
    check_active_maps();
    ck_assert_uint_eq(waiting->in_memory, MAP_SWAPPED);
    discard_map(waiting);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_expiry_cancels_on_reentry_and_restarts_full_timeout) {
    char directory[] = "/tmp/atrinik-swap-reentry-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *first = expiring_map(directory, "first");
    mapstruct *waiting = expiring_map(directory, "waiting");
    check_active_maps();
    ck_assert_uint_ne(waiting->swap_pending_order, 0);

    object *visitor = arch_get("letter");
    ck_assert_ptr_nonnull(visitor);
    ck_assert(object_enter_map(visitor, NULL, waiting, 0, 0, true));
    ck_assert_uint_eq(waiting->swap_pending_order, 0);
    ck_assert_int_eq(waiting->timeout, 0);
    object_remove(visitor, 0);
    object_destroy(visitor);
    check_active_maps();
    ck_assert_uint_eq(waiting->in_memory, MAP_IN_MEMORY);
    ck_assert_int_gt(waiting->timeout, 1);
    ck_assert_int_ne(access(waiting->path, F_OK), 0);
    waiting->timeout = 1;
    check_active_maps();
    ck_assert_uint_eq(waiting->in_memory, MAP_SWAPPED);

    discard_map(first);
    discard_map(waiting);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_expiry_revalidates_linked_occupancy_and_allocation) {
    char directory[] = "/tmp/atrinik-swap-identity-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *first = expiring_map(directory, "first");
    mapstruct *waiting = expiring_map(directory, "waiting");
    mapstruct *later = expiring_map(directory, "later");
    mapstruct *neighbor = get_empty_map(2, 2);
    check_active_maps();
    ck_assert_uint_ne(waiting->swap_pending_order, 0);
    ck_assert_uint_ne(later->swap_pending_order, 0);

    object blocker = {0};
    neighbor->player_first = &blocker;
    waiting->tile_map[TILED_EAST] = neighbor;
    neighbor->tile_map[TILED_WEST] = waiting;
    later->swap_pending_count--;
    check_active_maps();
    ck_assert_uint_eq(waiting->swap_pending_order, 0);
    ck_assert_uint_eq(later->swap_pending_order, 0);
    ck_assert_uint_eq(waiting->in_memory, MAP_IN_MEMORY);
    ck_assert_uint_eq(later->in_memory, MAP_IN_MEMORY);
    ck_assert_int_gt(later->timeout, 1);
    neighbor->player_first = NULL;
    check_active_maps();
    ck_assert_int_gt(waiting->timeout, 1);

    discard_map(first);
    discard_map(waiting);
    discard_map(later);
    delete_map(neighbor);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_expiry_failure_backs_off_without_starving_waiting_maps) {
    char directory[] = "/tmp/atrinik-swap-retry-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *failed = expiring_map(directory, "failed");
    mapstruct *waiting = expiring_map(directory, "waiting");
    ck_assert_int_eq(mkdir(failed->path, 0700), 0);
    object *marker = arch_get("letter");
    ck_assert_ptr_nonnull(marker);
    ck_assert_ptr_eq(object_insert_map(marker, failed, NULL, 0), marker);

    check_active_maps();
    ck_assert_uint_eq(failed->in_memory, MAP_IN_MEMORY);
    ck_assert_ptr_eq(marker->map, failed);
    ck_assert_ptr_nonnull(failed->spaces);
    ck_assert_uint_eq(failed->swap_failures, 1);
    ck_assert_uint_eq(failed->swap_retry_ticks, 2);
    check_active_maps();
    ck_assert_uint_eq(waiting->in_memory, MAP_SWAPPED);
    ck_assert_uint_eq(failed->swap_retry_ticks, 1);

    for (int attempt = 2; attempt <= 7; attempt++) {
        uint8_t failures = failed->swap_failures;
        while (failed->swap_retry_ticks != 0) {
            check_active_maps();
            ck_assert_uint_eq(failed->swap_failures, failures);
        }
        check_active_maps();
        ck_assert_uint_eq(failed->swap_failures, MIN(attempt, 5));
        ck_assert_uint_eq(failed->swap_retry_ticks, 1U << MIN(attempt, 5));
        ck_assert_ptr_eq(marker->map, failed);
        ck_assert_uint_eq(failed->in_memory, MAP_IN_MEMORY);
    }

    ck_assert_int_eq(rmdir(failed->path), 0);
    while (failed->swap_retry_ticks != 0) {
        check_active_maps();
    }
    check_active_maps();
    ck_assert_uint_eq(failed->in_memory, MAP_SWAPPED);
    ck_assert_uint_eq(failed->swap_pending_order, 0);
    ck_assert_int_eq(access(failed->path, F_OK), 0);
    discard_map(failed);
    discard_map(waiting);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_expiry_pending_delete_unload_and_shutdown_flush) {
    char directory[] = "/tmp/atrinik-swap-cleanup-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    mapstruct *first = expiring_map(directory, "first");
    mapstruct *deleted = expiring_map(directory, "deleted");
    mapstruct *unloaded = expiring_map(directory, "unloaded");
    mapstruct *waiting = expiring_map(directory, "waiting");
    mapstruct *waiting_second = expiring_map(directory, "waiting-second");
    ck_assert_int_eq(mkdir(first->path, 0700), 0);
    check_active_maps();
    ck_assert_uint_eq(first->swap_retry_ticks, 2);
    ck_assert_uint_ne(deleted->swap_pending_order, 0);
    ck_assert_uint_ne(unloaded->swap_pending_order, 0);
    ck_assert_uint_ne(waiting->swap_pending_order, 0);
    delete_map(deleted);
    free_map(unloaded, 1);
    ck_assert_uint_eq(unloaded->swap_pending_order, 0);
    mapstruct *replacement = get_empty_map(2, 2);
    ck_assert_uint_eq(replacement->swap_pending_order, 0);
    delete_map(replacement);
    ck_assert_int_eq(rmdir(first->path), 0);

    char previous_datapath[MAX_BUF];
    snprintf(VS(previous_datapath), "%s", settings.datapath);
    bool previous_recycle = settings.recycle_tmp_maps;
    snprintf(VS(settings.datapath), "%s", directory);
    settings.recycle_tmp_maps = true;
    clean_tmp_files();
    ck_assert_uint_eq(first->in_memory, MAP_SWAPPED);
    ck_assert_uint_eq(first->swap_retry_ticks, 0);
    ck_assert_uint_eq(waiting->in_memory, MAP_SWAPPED);
    ck_assert_uint_eq(waiting_second->in_memory, MAP_SWAPPED);
    ck_assert_int_eq(access(waiting->path, F_OK), 0);
    ck_assert_int_eq(access(waiting_second->path, F_OK), 0);
    snprintf(VS(settings.datapath), "%s", previous_datapath);
    settings.recycle_tmp_maps = previous_recycle;
    char clock_path[HUGE_BUF], log_path[HUGE_BUF];
    snprintf(VS(clock_path), "%s/clockdata", directory);
    snprintf(VS(log_path), "%s/temp.maps", directory);
    ck_assert_int_eq(unlink(clock_path), 0);
    ck_assert_int_eq(unlink(log_path), 0);

    discard_map(first);
    discard_map(unloaded);
    discard_map(waiting);
    discard_map(waiting_second);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("swap");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_expiry_is_bounded_and_fifo);
    tcase_add_test(tc_core, test_old_map_flush_preserves_waiting_expiry);
    tcase_add_test(tc_core, test_expiry_cancels_on_reentry_and_restarts_full_timeout);
    tcase_add_test(tc_core, test_expiry_revalidates_linked_occupancy_and_allocation);
    tcase_add_test(tc_core, test_expiry_failure_backs_off_without_starving_waiting_maps);
    tcase_add_test(tc_core, test_expiry_pending_delete_unload_and_shutdown_flush);
    return s;
}

void check_server_swap(void) {
    check_run_suite(suite(), __FILE__);
}
