/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <admin_shutdown.h>
#include <object.h>
#include <player.h>
#include <server.h>
#include <server_main.h>
#include <toolkit/packet.h>
#include <check.h>
#include <check_utils.h>
#include <string.h>
#include <stdio.h>

START_TEST(test_valid_shutdown) {
    const char *message = "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 Maintenance bientôt\n";
    admin_shutdown_request request;
    ck_assert(admin_shutdown_parse(message, strlen(message), &request));
    ck_assert_str_eq(request.id, "0123456789abcdef0123456789abcdef");
    ck_assert_uint_eq(request.seconds, 60);
    ck_assert_str_eq(request.reason, "Maintenance bientôt");
}
END_TEST

START_TEST(test_reject_invalid_requests) {
    const char *messages[] = {
        "ATRINIK-ADMIN/2 SHUTDOWN 0123456789abcdef0123456789abcdef 60 update\n",
        "ATRINIK-ADMIN/1 EXEC 0123456789abcdef0123456789abcdef 60 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdeF 60 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 29 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 601 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 060 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef +60 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 999999999 update\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 update",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 update\nnext\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 up\x1b" "date\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xc0\xaf\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xed\xa0\x80\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xf4\x90\x80\x80\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xe2\x80\xae\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xc2\x85\n",
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 60 \xe2\x80\n",
    };
    admin_shutdown_request request;
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        ck_assert_msg(!admin_shutdown_parse(messages[i], strlen(messages[i]), &request),
                      "Accepted invalid request %zu", i);
    }
}
END_TEST

START_TEST(test_limits_and_truncation) {
    char message[1100];
    admin_shutdown_request request;
    int prefix = snprintf(message, sizeof(message),
        "ATRINIK-ADMIN/1 SHUTDOWN 0123456789abcdef0123456789abcdef 600 ");
    memset(message + prefix, 'a', ADMIN_SHUTDOWN_REASON_MAX);
    message[prefix + ADMIN_SHUTDOWN_REASON_MAX] = '\n';
    size_t length = (size_t)prefix + ADMIN_SHUTDOWN_REASON_MAX + 1;
    ck_assert(admin_shutdown_parse(message, length, &request));
    for (size_t i = 0; i < length; i++) {
        ck_assert(!admin_shutdown_parse(message, i, &request));
    }
    message[prefix + ADMIN_SHUTDOWN_REASON_MAX] = 'a';
    message[length++] = '\n';
    ck_assert(!admin_shutdown_parse(message, length, &request));
    message[prefix] = '\0';
    ck_assert(!admin_shutdown_parse(message, length, &request));
    ck_assert(!admin_shutdown_parse(message, sizeof(message), &request));
    ck_assert(admin_shutdown_init("", NULL));
    ck_assert(!admin_shutdown_init("relative.sock", NULL));
}
END_TEST

static unsigned queued_warning_count(socket_struct *cs, const char *expected) {
    unsigned count = 0;
    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }
        packet_reader_t reader;
        char color[64], message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        ck_assert(packet_reader_read_string(&reader, VS(color)));
        ck_assert(packet_reader_read_string(&reader, VS(message)));
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);
        if (strcmp(message, expected) == 0) {
            count++;
        }
    }
    return count;
}

static void poll_countdown_without_advancing_time(void) {
    for (unsigned i = 0; i < 200; i++) {
        ck_assert_int_eq(shutdown_timer_check_for_test(), 0);
    }
}

START_TEST(test_countdown_reminders_are_once_per_threshold) {
    const char *minute = "[Server]: Server will shut down in 01:00 minutes.";
    const char *five_seconds = "[Server]: Server will shut down in 00:05 minutes.";
    long previous_ticks = pticks;
    long previous_max_time = max_time;
    max_time = 125000;
    pticks = 1000;
    object *pl = player_get_dummy(NULL, NULL);
    socket_struct *cs = CONTR(pl)->cs;
    socket_buffer_clear(cs);

    shutdown_timer_start(61);
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 0);
    pticks += 8;
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 1);
    pticks++;
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 1);

    pticks = 1000 + 56 * 8;
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, five_seconds), 1);
    ck_assert_uint_eq(queued_warning_count(cs, minute), 1);
    pticks = 1000 + 61 * 8;
    ck_assert_int_eq(shutdown_timer_check_for_test(), 1);
    ck_assert_int_eq(shutdown_timer_check_for_test(), 1);
    ck_assert_uint_eq(queued_warning_count(cs, five_seconds), 1);

    shutdown_timer_stop();
    poll_countdown_without_advancing_time();
    pticks = previous_ticks;
    max_time = previous_max_time;
}
END_TEST

START_TEST(test_countdown_cancel_and_restart_reset_warning_guard) {
    const char *minute = "[Server]: Server will shut down in 01:00 minutes.";
    long previous_ticks = pticks;
    long previous_max_time = max_time;
    max_time = 125000;
    pticks = 1000;
    object *pl = player_get_dummy(NULL, NULL);
    socket_struct *cs = CONTR(pl)->cs;
    socket_buffer_clear(cs);

    shutdown_timer_start(60);
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 1);
    shutdown_timer_stop();
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 1);

    /* A new timer at the same tick has its own reminder entitlement. */
    shutdown_timer_start(60);
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 2);
    shutdown_timer_start(60);
    poll_countdown_without_advancing_time();
    ck_assert_uint_eq(queued_warning_count(cs, minute), 3);

    shutdown_timer_stop();
    pticks = previous_ticks;
    max_time = previous_max_time;
}
END_TEST

void check_server_admin_shutdown(void) {
    Suite *suite = suite_create("server/admin_shutdown");
    TCase *core = tcase_create("Core");
    tcase_add_test(core, test_valid_shutdown);
    tcase_add_test(core, test_reject_invalid_requests);
    tcase_add_test(core, test_limits_and_truncation);
    suite_add_tcase(suite, core);
    TCase *countdown = tcase_create("Countdown warnings");
    tcase_add_unchecked_fixture(countdown, check_setup, check_teardown);
    tcase_add_test(countdown, test_countdown_reminders_are_once_per_threshold);
    tcase_add_test(countdown, test_countdown_cancel_and_restart_reset_warning_guard);
    suite_add_tcase(suite, countdown);
    check_run_suite(suite, __FILE__);
}
