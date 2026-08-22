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
#include <network_graph_data.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static void test_latency_uses_only_matched_responses(void) {
    client_keepalive_state_t keepalive;
    network_graph_series_t latency;
    uint32_t first;
    uint32_t second;
    uint32_t timed_out;
    uint64_t rtt;

    client_keepalive_reset(&keepalive);
    network_graph_series_init(&latency, 1);
    TEST_CHECK(network_graph_series_resize(&latency, 5));

    TEST_CHECK(client_keepalive_start(&keepalive, 1000, &first));
    TEST_CHECK(client_keepalive_start(&keepalive, 2000, &second));

    TEST_CHECK(client_keepalive_receive(&keepalive, second, 5000, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 3000);
    network_graph_series_add(&latency, 0, rtt, false);

    network_graph_series_advance(&latency);
    TEST_CHECK(client_keepalive_receive(&keepalive, first, 7000, &rtt) ==
               CLIENT_KEEPALIVE_RESPONSE_MATCHED);
    TEST_CHECK(rtt == 6000);
    network_graph_series_add(&latency, 0, rtt, false);
    TEST_CHECK(network_graph_series_value(&latency, 1, 0) == 6000);

    network_graph_series_advance(&latency);
    TEST_CHECK(network_graph_series_is_valid(&latency, 0));
    TEST_CHECK(network_graph_series_value(&latency, 0, 0) == 3000);
    TEST_CHECK(network_graph_series_is_valid(&latency, 1));
    TEST_CHECK(network_graph_series_value(&latency, 1, 0) == 6000);
    TEST_CHECK(!network_graph_series_is_valid(&latency, 2));

    TEST_CHECK(client_keepalive_receive(&keepalive, second, 8000, NULL) ==
               CLIENT_KEEPALIVE_RESPONSE_DUPLICATE);
    network_graph_series_advance(&latency);
    TEST_CHECK(!network_graph_series_is_valid(&latency, 2));

    TEST_CHECK(client_keepalive_start(&keepalive, 10000, &timed_out));
    client_keepalive_expire(&keepalive, 10000 + CLIENT_KEEPALIVE_TIMEOUT_US);
    TEST_CHECK(client_keepalive_receive(&keepalive,
                                        timed_out,
                                        10000 + CLIENT_KEEPALIVE_TIMEOUT_US + 1,
                                        NULL) == CLIENT_KEEPALIVE_RESPONSE_LATE);
    network_graph_series_advance(&latency);
    TEST_CHECK(!network_graph_series_is_valid(&latency, 3));
    TEST_CHECK(!network_graph_series_is_valid(&latency, 4));

    network_graph_series_free(&latency);
}

static void test_bandwidth_samples_accumulate_per_bucket(void) {
    network_graph_series_t bandwidth;

    network_graph_series_init(&bandwidth, 2);
    TEST_CHECK(network_graph_series_resize(&bandwidth, 2));
    network_graph_series_add(&bandwidth, 0, 100, true);
    network_graph_series_add(&bandwidth, 0, 50, true);
    network_graph_series_add(&bandwidth, 1, 25, true);
    TEST_CHECK(network_graph_series_value(&bandwidth, 0, 0) == 150);
    TEST_CHECK(network_graph_series_value(&bandwidth, 0, 1) == 25);

    network_graph_series_advance(&bandwidth);
    TEST_CHECK(network_graph_series_is_valid(&bandwidth, 0));
    TEST_CHECK(!network_graph_series_is_valid(&bandwidth, 1));
    network_graph_series_free(&bandwidth);
}

static void test_resize_preserves_recent_history(void) {
    network_graph_series_t latency;

    network_graph_series_init(&latency, 1);
    TEST_CHECK(network_graph_series_resize(&latency, 4));
    network_graph_series_add(&latency, 0, 10, false);
    network_graph_series_advance(&latency);
    network_graph_series_add(&latency, 0, 20, false);
    network_graph_series_advance(&latency);
    network_graph_series_add(&latency, 0, 30, false);

    TEST_CHECK(network_graph_series_resize(&latency, 2));
    TEST_CHECK(network_graph_series_value(&latency, 0, 0) == 20);
    TEST_CHECK(network_graph_series_value(&latency, 1, 0) == 30);
    TEST_CHECK(latency.max == 30);

    network_graph_series_free(&latency);
}

static void test_history_edge_cases(void) {
    network_graph_series_t series;

    network_graph_series_init(&series, 1);
    TEST_CHECK(network_graph_series_resize(&series, 0));
    network_graph_series_advance(&series);
    network_graph_series_free(&series);

    network_graph_series_init(&series, SIZE_MAX);
    TEST_CHECK(!network_graph_series_resize(&series, 2));
    network_graph_series_free(&series);

    network_graph_series_init(&series, SIZE_MAX / sizeof(uint64_t) + 1);
    TEST_CHECK(!network_graph_series_resize(&series, 1));
    network_graph_series_free(&series);

    network_graph_series_init(&series, 1);
    TEST_CHECK(network_graph_series_resize(&series, 2));
    TEST_CHECK(network_graph_series_resize(&series, 3));
    network_graph_series_add(&series, 0, 50, false);
    network_graph_series_advance(&series);
    network_graph_series_add(&series, 0, 100, false);
    network_graph_series_add(&series, 0, 25, false);
    TEST_CHECK(series.max == 50);
    network_graph_series_advance(&series);
    TEST_CHECK(network_graph_series_value(&series, 0, 0) == 50);
    TEST_CHECK(network_graph_series_value(&series, 3, 0) == 0);
    network_graph_series_advance(&series);
    TEST_CHECK(network_graph_series_value(&series, 0, 0) == 25);
    network_graph_series_free(&series);

    network_graph_series_init(&series, 1);
    TEST_CHECK(network_graph_series_resize(&series, 1));
    network_graph_series_add(&series, 0, UINT64_MAX, true);
    network_graph_series_add(&series, 0, 1, true);
    TEST_CHECK(network_graph_series_value(&series, 0, 0) == UINT64_MAX);
    network_graph_series_advance(&series);
    TEST_CHECK(!network_graph_series_is_valid(&series, 0));
    network_graph_series_free(&series);
}

int main(void) {
    test_latency_uses_only_matched_responses();
    test_bandwidth_samples_accumulate_per_bucket();
    test_resize_preserves_recent_history();
    test_history_edge_cases();
    return 0;
}
