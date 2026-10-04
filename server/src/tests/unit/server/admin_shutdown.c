/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <admin_shutdown.h>
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

void check_server_admin_shutdown(void) {
    Suite *suite = suite_create("server/admin_shutdown");
    TCase *core = tcase_create("Core");
    tcase_add_test(core, test_valid_shutdown);
    tcase_add_test(core, test_reject_invalid_requests);
    tcase_add_test(core, test_limits_and_truncation);
    suite_add_tcase(suite, core);
    check_run_suite(suite, __FILE__);
}
