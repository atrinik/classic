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

#include <global.h>
#include <server_main.h>
#include <initialization.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>

static char captured_log[HUGE_BUF];

static void capture_log(const char *message) {
    snprintf(VS(captured_log), "%s", message);
}

START_TEST(test_directory_only_candidates_exclude_traversal_entries) {
    char directory[] = "/tmp/atrinik-random-map-style-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);

    char child[MAX_BUF];
    snprintf(VS(child), "%s/child", directory);
    ck_assert_int_eq(mkdir(child, 0700), 0);

    char **namelist = NULL;
    int count = load_dir(directory, &namelist, 0);
    ck_assert_int_eq(count, 1);
    ck_assert_str_eq(namelist[0], "child");

    for (uint64_t seed = 0; seed < 64; seed++) {
        rng_state_t rng;
        rng_seed(&rng, seed);
        ck_assert_str_eq(namelist[rng_range(&rng, 0, count - 1)], "child");
    }

    free(namelist[0]);
    free(namelist);
    ck_assert_int_eq(rmdir(child), 0);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_missing_style_fails_closed_with_path_diagnostic) {
    char directory[] = "/tmp/atrinik-random-map-style-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    snprintf(VS(settings.mapspath), "%s", directory);

    captured_log[0] = '\0';
    logger_set_print_func(capture_log);

    rng_state_t rng;
    rng_seed(&rng, 109);
    ck_assert_ptr_eq(find_style("/styles", "missing", -1, &rng), NULL);

    logger_set_print_func(logger_do_print);
    ck_assert_ptr_ne(strstr(captured_log, "Could not inspect style path"), NULL);
    ck_assert_ptr_ne(strstr(captured_log, "/styles/missing"), NULL);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("random_map_style");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_directory_only_candidates_exclude_traversal_entries);
    tcase_add_test(tc_core, test_missing_style_fails_closed_with_path_diagnostic);
    return s;
}

void check_server_random_map_style(void) {
    check_run_suite(suite(), __FILE__);
}
