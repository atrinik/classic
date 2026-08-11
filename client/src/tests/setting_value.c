/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <stdint.h>
#include <stdlib.h>

#include <setting_value.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void check_value(setting_struct *setting, const char *text, int64_t expected) {
    TEST_CHECK(setting_value_parse(setting, text));
    TEST_CHECK(setting->val.i == expected);
}

static void check_rejected(setting_struct *setting, const char *text) {
    setting->val.i = 42;
    TEST_CHECK(!setting_value_parse(setting, text));
    TEST_CHECK(setting->val.i == 42);
}

static void test_boolean(void) {
    setting_struct setting = {.type = OPT_TYPE_BOOL};

    check_value(&setting, "yes", 1);
    check_value(&setting, "ON", 1);
    check_value(&setting, "True", 1);
    check_value(&setting, "1", 1);
    check_value(&setting, "no", 0);
    check_value(&setting, "OFF", 0);
    check_value(&setting, "False", 0);
    check_value(&setting, "0", 0);
    check_rejected(&setting, "-1");
    check_rejected(&setting, "2");
    check_rejected(&setting, "true ");
}

static void test_select(void) {
    setting_select select = {.options_len = 4};
    setting_struct setting = {.type = OPT_TYPE_SELECT, .custom_attrset = &select};

    check_value(&setting, "0", 0);
    check_value(&setting, "3", 3);
    check_rejected(&setting, "-1");
    check_rejected(&setting, "4");
    select.options_len = 0;
    check_rejected(&setting, "0");
}

static void test_range(void) {
    setting_range range = {.min = 9, .max = 17, .advance = 1};
    setting_struct setting = {.type = OPT_TYPE_RANGE, .custom_attrset = &range};

    check_value(&setting, "9", 9);
    check_value(&setting, "17", 17);
    check_rejected(&setting, "8");
    check_rejected(&setting, "18");
}

static void test_integer_syntax_and_bounds(void) {
    setting_struct setting = {.type = OPT_TYPE_INT};

    check_value(&setting, "-9223372036854775808", INT64_MIN);
    check_value(&setting, "9223372036854775807", INT64_MAX);
    check_rejected(&setting, "");
    check_rejected(&setting, "invalid");
    check_rejected(&setting, "12junk");
    check_rejected(&setting, "9223372036854775808");
    check_rejected(&setting, "-9223372036854775809");

    setting.type = OPT_TYPE_INPUT_NUM;
    check_value(&setting, "+12", 12);
}

int main(void) {
    test_boolean();
    test_select();
    test_range();
    test_integer_syntax_and_bounds();
    return 0;
}
