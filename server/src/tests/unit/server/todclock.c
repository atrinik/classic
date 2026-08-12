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
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <toolkit/path.h>

START_TEST(test_parse_valid_values) {
    unsigned long value = ULONG_MAX;

    ck_assert(todclock_parse("0", &value));
    ck_assert_uint_eq(value, 0);
    ck_assert(todclock_parse("123456", &value));
    ck_assert_uint_eq(value, 123456);
    ck_assert(todclock_parse(" \t42\r\n", &value));
    ck_assert_uint_eq(value, 42);
}
END_TEST

START_TEST(test_parse_rejects_malformed_values) {
    static const char *invalid[] = {
        "",
        " \t\r\n",
        "value",
        "+1",
        "-1",
        "1x",
        "1 2",
    };
    unsigned long value = 17;

    for (size_t i = 0; i < arraysize(invalid); i++) {
        ck_assert(!todclock_parse(invalid[i], &value));
        ck_assert_uint_eq(value, 17);
    }
}
END_TEST

START_TEST(test_parse_rejects_overflow) {
    char overflow[64];
    unsigned long value = 17;
    snprintf(VS(overflow), "%lu0", ULONG_MAX);

    ck_assert(!todclock_parse(overflow, &value));
    ck_assert_uint_eq(value, 17);
}
END_TEST

START_TEST(test_write_replaces_clockdata_with_complete_value) {
    char filename[HUGE_BUF];
    snprintf(VS(filename), "%s/clockdata", settings.datapath);
    const char previous[] = "18446744073709551615 trailing evidence";
    ck_assert(path_write_atomic(filename, previous, sizeof(previous) - 1, SAVE_MODE));

    todtick = 42;
    write_todclock();

    char *contents = path_file_contents(filename);
    ck_assert_ptr_nonnull(contents);
    ck_assert_str_eq(contents, "42");
    free(contents);
}
END_TEST

START_TEST(test_set_persists_clockdata) {
    char filename[HUGE_BUF];
    snprintf(VS(filename), "%s/clockdata", settings.datapath);

    todtick = 17;
    ck_assert(todclock_set(23));
    ck_assert_uint_eq(todtick, 23);

    char *contents = path_file_contents(filename);
    ck_assert_ptr_nonnull(contents);
    ck_assert_str_eq(contents, "23");
    free(contents);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("todclock");
    TCase *tc_parse = tcase_create("Parse");
    tcase_add_test(tc_parse, test_parse_valid_values);
    tcase_add_test(tc_parse, test_parse_rejects_malformed_values);
    tcase_add_test(tc_parse, test_parse_rejects_overflow);
    suite_add_tcase(s, tc_parse);

    TCase *tc_persistence = tcase_create("Persistence");
    tcase_add_unchecked_fixture(tc_persistence, check_setup, check_teardown);
    tcase_add_test(tc_persistence, test_write_replaces_clockdata_with_complete_value);
    tcase_add_test(tc_persistence, test_set_persists_clockdata);
    suite_add_tcase(s, tc_persistence);
    return s;
}

void check_server_todclock(void) {
    check_run_suite(suite(), __FILE__);
}
