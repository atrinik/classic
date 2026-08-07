/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>

#include <check.h>
#include <check_utils.h>
#include <checkstd.h>
#include <content_benchmark.h>

START_TEST(test_map_list_accepts_canonical_unique_logical_ids) {
    ck_assert(
        content_benchmark_maps_valid("/start,/shattered_islands/world_1_1,/plane/map-name.2"));
}
END_TEST

START_TEST(test_map_list_rejects_ambiguous_or_unsafe_ids) {
    static const char *invalid[] = {
        NULL,
        "",
        "relative",
        "/",
        "/trailing/",
        "/double//separator",
        "/dot/./component",
        "/parent/../component",
        "/windows\\separator",
        "/windows:drive",
        "/space in/name",
        "/duplicate,/duplicate",
        ",/leading",
        "/trailing,",
        "/empty,,/component",
    };

    for (size_t i = 0; i < arraysize(invalid); i++) {
        ck_assert(!content_benchmark_maps_valid(invalid[i]));
    }
}
END_TEST

START_TEST(test_map_list_enforces_count_and_component_bounds) {
    ck_assert(!content_benchmark_maps_valid("/a,/b,/c,/d,/e,/f,/g,/h,/i,/j,/k,/l,/m,/n,/o,/p,/q"));

    char oversized[MAX_BUF + 2];
    oversized[0] = '/';
    memset(oversized + 1, 'a', MAX_BUF);
    oversized[MAX_BUF + 1] = '\0';
    ck_assert(!content_benchmark_maps_valid(oversized));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("content_benchmark");
    TCase *tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_map_list_accepts_canonical_unique_logical_ids);
    tcase_add_test(tc_core, test_map_list_rejects_ambiguous_or_unsafe_ids);
    tcase_add_test(tc_core, test_map_list_enforces_count_and_component_bounds);
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_content_benchmark(void) {
    check_run_suite(suite(), __FILE__);
}
