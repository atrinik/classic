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
#include <celestial_lunar.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <commands.h>
#include <initialization.h>
#include <limits.h>
#include <object.h>
#include <player.h>
#include <server.h>
#include <toolkit/path.h>
#include <toolkit/packet.h>
#include <tod.h>

static bool queued_drawinfo_has(socket_struct *cs, const char *expected) {
    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }

        packet_reader_t reader;
        char color[64];
        char message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        ck_assert(packet_reader_read_string(&reader, VS(color)));
        ck_assert(packet_reader_read_string(&reader, VS(message)));
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);
        if (strcmp(message, expected) == 0) {
            return true;
        }
    }
    return false;
}

static void assert_time_lunar_output(object *pl,
                                     unsigned long tick,
                                     const char *phase,
                                     const char *visibility,
                                     const char *contribution) {
    socket_buffer_clear(CONTR(pl)->cs);
    todtick = tick;
    command_time(pl, "time", NULL);

    char expected[HUGE_BUF];
    snprintf(VS(expected), "The current moon phase is %s.", phase);
    ck_assert(queued_drawinfo_has(CONTR(pl)->cs, expected));
    snprintf(VS(expected),
             "Moon visibility: %s; moonlight contribution: %s.",
             visibility,
             contribution);
    ck_assert(queued_drawinfo_has(CONTR(pl)->cs, expected));
}

static void assert_calendar_identity(unsigned long tick) {
    todtick = tick;
    timeofday_t tod;
    get_tod(&tod);

    ck_assert_int_eq(tod.year, tick / HOURS_PER_YEAR);
    ck_assert_int_eq(tod.month, (tick / HOURS_PER_MONTH) % MONTHS_PER_YEAR);
    ck_assert_int_eq(tod.season, tod.month / MONTHS_PER_SEASON);
    ck_assert_int_eq(tod.day, (tick % HOURS_PER_MONTH) / HOURS_PER_DAY);
    ck_assert_int_eq(tod.dayofweek, (tick / HOURS_PER_DAY) % DAYS_PER_WEEK);
    ck_assert_int_eq(tod.weekofmonth, tod.day / DAYS_PER_WEEK);
    ck_assert_int_eq(tod.hour, tick % HOURS_PER_DAY);
    ck_assert_int_eq(tod.periodofday, periodsofday_hours[tod.hour]);
}

typedef struct calendar_vector {
    unsigned long tick;
    int year;
    int month;
    int season;
    int day;
    int dayofweek;
    int weekofmonth;
    int hour;
} calendar_vector;

static void assert_calendar_vector(const calendar_vector *expected) {
    todtick = expected->tick;
    timeofday_t tod;
    get_tod(&tod);

    ck_assert_int_eq(tod.year, expected->year);
    ck_assert_int_eq(tod.month, expected->month);
    ck_assert_int_eq(tod.season, expected->season);
    ck_assert_int_eq(tod.day, expected->day);
    ck_assert_int_eq(tod.dayofweek, expected->dayofweek);
    ck_assert_int_eq(tod.weekofmonth, expected->weekofmonth);
    ck_assert_int_eq(tod.hour, expected->hour);
}

START_TEST(test_calendar_exact_boundaries) {
    static const calendar_vector vectors[] = {
        {0, 0, 0, 0, 0, 0, 0, 0},
        {23, 0, 0, 0, 0, 0, 0, 23},
        {24, 0, 0, 0, 1, 1, 0, 0},
        {167, 0, 0, 0, 6, 6, 0, 23},
        {168, 0, 0, 0, 7, 0, 1, 0},
        {335, 0, 0, 0, 13, 6, 1, 23},
        {336, 0, 0, 0, 14, 0, 2, 0},
        {503, 0, 0, 0, 20, 6, 2, 23},
        {504, 0, 0, 0, 21, 0, 3, 0},
        {671, 0, 0, 0, 27, 6, 3, 23},
        {672, 0, 1, 0, 0, 0, 0, 0},
        {2015, 0, 2, 0, 27, 6, 3, 23},
        {2016, 0, 3, 1, 0, 0, 0, 0},
        {4031, 0, 5, 1, 27, 6, 3, 23},
        {4032, 0, 6, 2, 0, 0, 0, 0},
        {6047, 0, 8, 2, 27, 6, 3, 23},
        {6048, 0, 9, 3, 0, 0, 0, 0},
        {8063, 0, 11, 3, 27, 6, 3, 23},
        {8064, 1, 0, 0, 0, 0, 0, 0},
        {123456, 15, 3, 1, 20, 6, 2, 0},
        {(12345UL * HOURS_PER_YEAR) + (7 * HOURS_PER_MONTH) + (19 * HOURS_PER_DAY) + 13,
         12345,
         7,
         2,
         19,
         5,
         2,
         13},
    };

    for (size_t i = 0; i < arraysize(vectors); i++) {
        assert_calendar_vector(&vectors[i]);
    }

    for (unsigned long month = 1; month <= MONTHS_PER_YEAR; month++) {
        assert_calendar_identity((month * HOURS_PER_MONTH) - 1);
        assert_calendar_identity(month * HOURS_PER_MONTH);
    }
}
END_TEST

START_TEST(test_calendar_days_and_weeks_cover_month) {
    for (unsigned long day = 0; day < DAYS_PER_MONTH; day++) {
        todtick = day * HOURS_PER_DAY;
        timeofday_t tod;
        get_tod(&tod);

        ck_assert_int_eq(tod.day, day);
        ck_assert_int_eq(tod.dayofweek, day % DAYS_PER_WEEK);
        ck_assert_int_eq(tod.weekofmonth, day / DAYS_PER_WEEK);

        todtick += HOURS_PER_DAY - 1;
        get_tod(&tod);
        ck_assert_int_eq(tod.day, day);
        ck_assert_int_eq(tod.dayofweek, day % DAYS_PER_WEEK);
        ck_assert_int_eq(tod.weekofmonth, day / DAYS_PER_WEEK);
    }
}
END_TEST

START_TEST(test_calendar_minute_and_period_remain_unchanged) {
    for (unsigned long hour = 0; hour < HOURS_PER_DAY; hour++) {
        todtick = hour;
        pticks = 0;
        timeofday_t tod;
        get_tod(&tod);

        ck_assert_int_eq(tod.hour, hour);
        ck_assert_int_eq(tod.periodofday, periodsofday_hours[hour]);
    }

    todtick = HOURS_PER_DAY - 1;
    const long minute_tick = PTICKS_PER_CLOCK / 58;
    static const long ticks[] = {
        0,
        PTICKS_PER_CLOCK / 58,
        PTICKS_PER_CLOCK - 1,
    };

    for (size_t i = 0; i < arraysize(ticks); i++) {
        pticks = ticks[i];
        timeofday_t tod;
        get_tod(&tod);

        int expected_minute = (ticks[i] % PTICKS_PER_CLOCK) / minute_tick;
        if (expected_minute > 59) {
            expected_minute = 59;
        }
        ck_assert_int_eq(tod.minute, expected_minute);
        ck_assert_int_eq(tod.periodofday, periodsofday_hours[HOURS_PER_DAY - 1]);
    }
}
END_TEST

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

START_TEST(test_persisted_tick_reconstructs_without_rewrite) {
    char filename[HUGE_BUF];
    snprintf(VS(filename), "%s/clockdata", settings.datapath);
    const unsigned long original =
        (12345UL * HOURS_PER_YEAR) + (7 * HOURS_PER_MONTH) + (19 * HOURS_PER_DAY) + 13;

    ck_assert(todclock_set(original));
    timeofday_t before;
    get_tod(&before);

    char *contents = path_file_contents(filename);
    ck_assert_ptr_nonnull(contents);
    unsigned long reloaded = 0;
    ck_assert(todclock_parse(contents, &reloaded));
    ck_assert_uint_eq(reloaded, original);

    todtick = reloaded;
    timeofday_t after;
    get_tod(&after);
    ck_assert_int_eq(after.year, before.year);
    ck_assert_int_eq(after.month, before.month);
    ck_assert_int_eq(after.day, before.day);
    ck_assert_int_eq(after.dayofweek, before.dayofweek);
    ck_assert_int_eq(after.hour, before.hour);
    ck_assert_int_eq(after.minute, before.minute);
    ck_assert_int_eq(after.weekofmonth, before.weekofmonth);
    ck_assert_int_eq(after.season, before.season);
    ck_assert_int_eq(after.periodofday, before.periodofday);

    char *unchanged = path_file_contents(filename);
    ck_assert_ptr_nonnull(unchanged);
    ck_assert_str_eq(unchanged, contents);
    free(unchanged);
    free(contents);
}
END_TEST

START_TEST(test_time_reports_phase_separately_from_visibility_and_moonlight) {
    static const struct {
        unsigned long tick;
        const char *phase;
        const char *visibility;
        const char *contribution;
    } vectors[] = {
        {0, "new moon", "below the horizon", "none"},
        {84, "waxing crescent", "above the horizon", "present"},
        {168, "first quarter", "below the horizon", "none"},
        {252, "waxing gibbous", "below the horizon", "none"},
        {336, "full moon", "above the horizon", "present"},
        {420, "waning gibbous", "below the horizon", "none"},
        {504, "last quarter", "below the horizon", "none"},
        {588, "waning crescent", "above the horizon", "present"},
    };

    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    CONTR(pl)->cs->state = ST_PLAYING;
    pticks = 0;

    for (size_t i = 0; i < arraysize(vectors); i++) {
        assert_time_lunar_output(pl,
                                 vectors[i].tick,
                                 vectors[i].phase,
                                 vectors[i].visibility,
                                 vectors[i].contribution);
        if (i == 0) {
            ck_assert(queued_drawinfo_has(
                CONTR(pl)->cs,
                "It is midnight, 0 minutes past 12 o'clock am, on the Day of the Moon."));
            ck_assert(queued_drawinfo_has(
                CONTR(pl)->cs,
                "The 1st Day of the Month of the Winter, Year 1, in the Season of the Blizzard."));
        }
    }

    assert_time_lunar_output(pl,
                             HOURS_PER_YEAR + HOURS_PER_MONTH / 2,
                             "full moon",
                             "above the horizon",
                             "present");

    celestial_lunar_input input;
    celestial_lunar_sample sample;
    celestial_lunar_root_input((uint64_t)ULONG_MAX, &input);
    ck_assert(celestial_lunar_evaluate(&input, &sample));
    assert_time_lunar_output(pl,
                             ULONG_MAX,
                             celestial_lunar_phase_name(sample.phase),
                             sample.visible ? "above the horizon" : "below the horizon",
                             sample.moon_strength != 0 ? "present" : "none");

    object_destroy(pl);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("todclock");
    TCase *tc_calendar = tcase_create("Calendar");
    tcase_add_test(tc_calendar, test_calendar_exact_boundaries);
    tcase_add_test(tc_calendar, test_calendar_days_and_weeks_cover_month);
    tcase_add_test(tc_calendar, test_calendar_minute_and_period_remain_unchanged);
    suite_add_tcase(s, tc_calendar);

    TCase *tc_parse = tcase_create("Parse");
    tcase_add_test(tc_parse, test_parse_valid_values);
    tcase_add_test(tc_parse, test_parse_rejects_malformed_values);
    tcase_add_test(tc_parse, test_parse_rejects_overflow);
    suite_add_tcase(s, tc_parse);

    TCase *tc_output = tcase_create("Output");
    tcase_add_unchecked_fixture(tc_output, check_setup, check_teardown);
    tcase_add_test(tc_output, test_time_reports_phase_separately_from_visibility_and_moonlight);
    suite_add_tcase(s, tc_output);

    TCase *tc_persistence = tcase_create("Persistence");
    tcase_add_unchecked_fixture(tc_persistence, check_setup, check_teardown);
    tcase_add_test(tc_persistence, test_write_replaces_clockdata_with_complete_value);
    tcase_add_test(tc_persistence, test_set_persists_clockdata);
    tcase_add_test(tc_persistence, test_persisted_tick_reconstructs_without_rewrite);
    suite_add_tcase(s, tc_persistence);
    return s;
}

void check_server_todclock(void) {
    check_run_suite(suite(), __FILE__);
}
