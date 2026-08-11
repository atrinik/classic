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

static void write_style_map(const char *path) {
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_int_ne(fputs("arch map\n"
                           "name child style\n"
                           "width 1\n"
                           "height 1\n"
                           "difficulty 1\n"
                           "end\n"
                           "arch door1_locked\n"
                           "end\n",
                           fp),
                     EOF);
    ck_assert_int_eq(fclose(fp), 0);
}

START_TEST(test_directory_only_selection_stays_within_child) {
    char directory[] = "/tmp/atrinik-random-map-style-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);

    char styles_dir[sizeof(directory) + sizeof("/styles")];
    char child[sizeof(styles_dir) + sizeof("/child")];
    char style_map_path[sizeof(child) + sizeof("/style_1")];
    snprintf(VS(styles_dir), "%s/styles", directory);
    snprintf(VS(child), "%s/child", styles_dir);
    snprintf(VS(style_map_path), "%s/style_1", child);
    ck_assert_int_eq(mkdir(styles_dir, 0700), 0);
    ck_assert_int_eq(mkdir(child, 0700), 0);
    write_style_map(style_map_path);

    char **namelist = NULL;
    int count = load_dir(styles_dir, &namelist, 0);
    ck_assert_int_eq(count, 1);
    ck_assert_str_eq(namelist[0], "child");
    free(namelist[0]);
    free(namelist);

    snprintf(VS(settings.mapspath), "%s", directory);

    for (uint64_t seed = 0; seed < 64; seed++) {
        rng_state_t rng;
        rng_seed(&rng, seed);
        mapstruct *style = find_style("/styles", NULL, 1, &rng);
        ck_assert_ptr_ne(style, NULL);
        ck_assert_str_eq(style->path, "/styles/child/style_1");
    }

    free_style_maps();
    ck_assert_int_eq(unlink(style_map_path), 0);
    ck_assert_int_eq(rmdir(child), 0);
    ck_assert_int_eq(rmdir(styles_dir), 0);
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
    tcase_add_test(tc_core, test_directory_only_selection_stays_within_child);
    tcase_add_test(tc_core, test_missing_style_fails_closed_with_path_diagnostic);
    return s;
}

void check_server_random_map_style(void) {
    check_run_suite(suite(), __FILE__);
}
