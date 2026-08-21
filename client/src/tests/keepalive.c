/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *************************************************************************/

#include <keepalive.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static void test_out_of_order_responses_and_average(void) {
    client_keepalive_state_t state;
    client_keepalive_statistics_t statistics;
    uint32_t first;
    uint32_t second;
    uint64_t rtt;

    client_keepalive_reset(&state);
    TEST_CHECK(client_keepalive_start(&state, 1000, &first));
    TEST_CHECK(client_keepalive_start(&state, 2000, &second));
    TEST_CHECK(first != second);

    TEST_CHECK(client_keepalive_receive(&state, second, 5000, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 3000);
    TEST_CHECK(client_keepalive_receive(&state, first, 7000, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 6000);

    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.tx == 2);
    TEST_CHECK(statistics.rx == 2);
    TEST_CHECK(statistics.responses == 2);
    TEST_CHECK(statistics.pending == 0);
    TEST_CHECK(statistics.total_rtt_us == 9000);
    TEST_CHECK(statistics.last_rtt_us == 6000);
    TEST_CHECK(client_keepalive_average_us(&statistics) == 4500);

    TEST_CHECK(client_keepalive_receive(&state, second, 6000, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_DUPLICATE);
    TEST_CHECK(client_keepalive_receive(&state, UINT32_MAX, 6000, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_UNKNOWN);
    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.duplicate == 1);
    TEST_CHECK(statistics.unknown == 1);
}

static void test_explicit_timeout_and_late_response(void) {
    client_keepalive_state_t state;
    client_keepalive_statistics_t statistics;
    uint32_t id;

    client_keepalive_reset(&state);
    TEST_CHECK(client_keepalive_start(&state, 100, &id));
    client_keepalive_expire(&state, 100 + CLIENT_KEEPALIVE_TIMEOUT_US);

    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.timed_out == 1);
    TEST_CHECK(statistics.pending == 0);
    TEST_CHECK(client_keepalive_receive(&state, id, 100 + CLIENT_KEEPALIVE_TIMEOUT_US + 1, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_LATE);
    TEST_CHECK(client_keepalive_receive(&state, id, 100 + CLIENT_KEEPALIVE_TIMEOUT_US + 2, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_DUPLICATE);

    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.responses == 1);
    TEST_CHECK(statistics.late == 1);
    TEST_CHECK(statistics.duplicate == 1);
    TEST_CHECK(statistics.rx == 0);
}

static void test_clock_regression_and_counter_wrap(void) {
    client_keepalive_state_t state;
    client_keepalive_statistics_t statistics;
    uint32_t wrapped_id;
    uint32_t regressed_id;
    uint64_t rtt;

    client_keepalive_reset(&state);
    TEST_CHECK(client_keepalive_start(&state, UINT64_MAX - 5, &wrapped_id));
    TEST_CHECK(client_keepalive_receive(&state, wrapped_id, 5, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 11);

    TEST_CHECK(client_keepalive_start(&state, 100, &regressed_id));
    TEST_CHECK(client_keepalive_receive(&state, regressed_id, 99, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_CLOCK_REGRESSION);
    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.pending == 1);
    TEST_CHECK(statistics.clock_regressions == 1);
    TEST_CHECK(client_keepalive_receive(&state, regressed_id, 200, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 100);
}

static void test_pending_capacity_is_never_evicted(void) {
    client_keepalive_state_t state;
    client_keepalive_statistics_t statistics;
    uint32_t ids[CLIENT_KEEPALIVE_RECORD_CAPACITY];

    client_keepalive_reset(&state);
    for (size_t i = 0; i < arraysize(ids); i++) {
        TEST_CHECK(client_keepalive_start(&state, i, &ids[i]));
    }
    uint32_t extra;
    TEST_CHECK(!client_keepalive_start(&state, arraysize(ids), &extra));

    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.tx == arraysize(ids));
    TEST_CHECK(statistics.pending == arraysize(ids));

    client_keepalive_expire(&state, CLIENT_KEEPALIVE_TIMEOUT_US + arraysize(ids));
    TEST_CHECK(client_keepalive_start(&state, CLIENT_KEEPALIVE_TIMEOUT_US + 100, &extra));
    TEST_CHECK(extra != ids[0]);
}

static void test_reset_clears_session_state(void) {
    client_keepalive_state_t state;
    client_keepalive_statistics_t statistics;
    uint32_t id;

    client_keepalive_reset(&state);
    TEST_CHECK(client_keepalive_start(&state, 1, &id));
    client_keepalive_reset(&state);
    client_keepalive_statistics(&state, &statistics);
    TEST_CHECK(statistics.tx == 0);
    TEST_CHECK(statistics.pending == 0);
    TEST_CHECK(client_keepalive_receive(&state, id, 2, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_UNKNOWN);
}

int main(void) {
    test_out_of_order_responses_and_average();
    test_explicit_timeout_and_late_response();
    test_clock_regression_and_counter_wrap();
    test_pending_capacity_is_never_evicted();
    test_reset_clears_session_state();
    return 0;
}
