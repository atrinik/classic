/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <auth_worker.h>
#include <openssl/crypto.h>

static void setup(void) {
    ck_assert(auth_worker_start());
}

static void teardown(void) {
    auth_worker_stop();
}

static auth_work_t work_new(uint64_t request) {
    auth_work_t work = {0};
    work.generation = 1;
    work.request = request;
    return work;
}

START_TEST(test_two_running_eight_queued_and_cancellation) {
    auth_worker_pause_for_test(true);
    auth_work_t work = work_new(1);
    ck_assert(auth_worker_submit(&work));
    work.request++;
    ck_assert(auth_worker_submit(&work));
    ck_assert(auth_worker_wait_running_for_test(2));
    for (size_t i = 0; i < AUTH_WORKER_QUEUE; i++) {
        work.request++;
        ck_assert(auth_worker_submit(&work));
    }
    ck_assert_uint_eq(auth_worker_pending_for_test(), AUTH_WORKER_CAPACITY);
    work.request++;
    ck_assert(!auth_worker_submit(&work));
    /* Queued cancellation immediately frees a queue position. */
    auth_worker_cancel(1, 3);
    ck_assert(auth_worker_submit(&work));
    /* Running cancellation cannot free its slot until the worker clears its copy. */
    auth_worker_cancel(1, 1);
    auth_worker_pause_for_test(false);
    ck_assert(auth_worker_wait_idle_for_test());
    size_t count = 0;
    while (auth_worker_take(&work)) {
        ck_assert_uint_ne(work.request, 1);
        ck_assert_uint_ne(work.request, 3);
        count++;
    }
    ck_assert_uint_eq(count, AUTH_WORKER_CAPACITY - 1);
    ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
}
END_TEST

START_TEST(test_owned_secret_snapshot_and_verification) {
    auth_work_t work = work_new(1);
    work.create = true;
    snprintf(work.replacement, sizeof(work.replacement), "async-password-7!");
    auth_worker_pause_for_test(true);
    ck_assert(auth_worker_submit(&work));
    OPENSSL_cleanse(&work, sizeof(work));
    auth_worker_pause_for_test(false);
    ck_assert(auth_worker_wait_idle_for_test());
    ck_assert(auth_worker_take(&work));
    ck_assert_int_eq(work.result, PASSWORD_VERIFY_MATCH);
    ck_assert(password_record_is_valid(work.record));
    ck_assert_ptr_nonnull(strstr(work.record, "m=65536,t=3,p=1"));
    char record[PASSWORD_RECORD_SIZE];
    memcpy(record, work.record, sizeof(record));
    for (size_t i = 0; i < sizeof(work.replacement); i++) {
        ck_assert_int_eq(work.replacement[i], 0);
        ck_assert_int_eq(work.password[i], 0);
    }
    for (size_t i = 0; i < 2; i++) {
        work = work_new(i + 2);
        work.verify = true;
        memcpy(work.credential.record, record, sizeof(record));
        snprintf(work.password, sizeof(work.password), "%s",
                 i == 0 ? "async-password-7!" : "wrong-password-8!");
        ck_assert(auth_worker_submit(&work));
        ck_assert(auth_worker_wait_idle_for_test());
        ck_assert(auth_worker_take(&work));
        ck_assert_int_eq(work.result, i == 0 ? PASSWORD_VERIFY_MATCH : PASSWORD_VERIFY_MISMATCH);
        for (size_t j = 0; j < sizeof(work.password); j++) {
            ck_assert_int_eq(work.password[j], 0);
        }
        const unsigned char *cleared = (const unsigned char *)&work.credential;
        for (size_t j = 0; j < sizeof(work.credential); j++) {
            ck_assert_int_eq(cleared[j], 0);
        }
    }
    OPENSSL_cleanse(record, sizeof(record));
    OPENSSL_cleanse(&work, sizeof(work));
}
END_TEST

START_TEST(test_shutdown_clears_running_queued_and_done) {
    auth_work_t work = work_new(1);
    ck_assert(auth_worker_submit(&work));
    ck_assert(auth_worker_wait_idle_for_test());
    auth_worker_pause_for_test(true);
    for (size_t i = 0; i < 2; i++) {
        work.request++;
        snprintf(work.password, sizeof(work.password), "queued-secret");
        ck_assert(auth_worker_submit(&work));
    }
    ck_assert(auth_worker_wait_running_for_test(2));
    work.request++;
    ck_assert(auth_worker_submit(&work));
    auth_worker_stop();
    ck_assert_uint_eq(auth_worker_pending_for_test(), 0);
    ck_assert(!auth_worker_take(&work));
    ck_assert(!auth_worker_submit(&work));
    ck_assert(auth_worker_start());
    work = work_new(5);
    ck_assert(auth_worker_submit(&work));
    ck_assert(auth_worker_wait_idle_for_test());
    ck_assert(auth_worker_take(&work));
    ck_assert_uint_eq(work.request, 5);
}
END_TEST

void check_server_auth_worker(void) {
    Suite *s = suite_create("auth_worker");
    TCase *tc = tcase_create("Core");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_set_timeout(tc, 30);
    tcase_add_test(tc, test_two_running_eight_queued_and_cancellation);
    tcase_add_test(tc, test_owned_secret_snapshot_and_verification);
    tcase_add_test(tc, test_shutdown_clears_running_queued_and_done);
    suite_add_tcase(s, tc);
    check_run_suite(s, __FILE__);
}
