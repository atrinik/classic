/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <atrinik/protocol/game_commands.h>
#include <client.h>
#include <client_command_queue.h>
#include <toolkit/datetime.h>
#include <toolkit/toolkit.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

typedef struct fake_dispatch {
    uint64_t now_us;
    uint64_t command_cost_us;
    uint8_t order[8];
    size_t count;
    bool continue_dispatch;
} fake_dispatch_t;

static uint64_t fake_clock(void *user_data) {
    fake_dispatch_t *fake = user_data;
    return fake->now_us;
}

static bool fake_map_dispatch(uint8_t *data, size_t len, void *user_data) {
    fake_dispatch_t *fake = user_data;
    TEST_CHECK(len == 2);
    TEST_CHECK(data[0] == CLIENT_CMD_MAP);
    TEST_CHECK(fake->count < arraysize(fake->order));
    fake->order[fake->count++] = data[1];
    fake->now_us += fake->command_cost_us;
    return fake->continue_dispatch;
}

static void enqueue_map(uint8_t sequence, uint64_t arrival_us) {
    const uint8_t envelope[] = {CLIENT_CMD_MAP, sequence};
    TEST_CHECK(client_command_queue_enqueue_envelope_at(envelope, sizeof(envelope), arrival_us));
}

static void test_empty_tick_and_validation(void) {
    fake_dispatch_t fake = {.now_us = 100, .continue_dispatch = true};
    client_command_queue_drain_result_t result;
    client_command_queue_statistics_t statistics;

    TEST_CHECK(client_command_queue_statistics_reset());
    TEST_CHECK(!client_command_queue_enqueue_envelope_at(NULL, 1, 0));
    TEST_CHECK(!client_command_queue_enqueue_envelope_at((const uint8_t *)"", 0, 0));
    client_command_queue_drain(CLIENT_COMMAND_QUEUE_BUDGET_US,
                               fake_clock,
                               &fake,
                               fake_map_dispatch,
                               &fake,
                               &result);
    TEST_CHECK(result.commands == 0);
    TEST_CHECK(result.bytes == 0);
    TEST_CHECK(result.processing_us == 0);
    TEST_CHECK(!result.budget_due);

    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(!statistics.due);
    TEST_CHECK(statistics.depth == 0);
    TEST_CHECK(statistics.bytes == 0);
    TEST_CHECK(statistics.enqueued == 0);
    TEST_CHECK(statistics.dequeued == 0);
    TEST_CHECK(statistics.order_digests_comparable);
}

static void test_ordered_budget_yield_and_recovery(void) {
    fake_dispatch_t fake = {
        .now_us = 100,
        .command_cost_us = 2500,
        .continue_dispatch = true,
    };
    client_command_queue_drain_result_t result;
    client_command_queue_statistics_t statistics;

    TEST_CHECK(client_command_queue_statistics_reset());
    enqueue_map(1, 10);
    enqueue_map(2, 20);
    enqueue_map(3, 30);
    TEST_CHECK(!client_command_queue_statistics_reset());

    client_command_queue_drain(CLIENT_COMMAND_QUEUE_BUDGET_US,
                               fake_clock,
                               &fake,
                               fake_map_dispatch,
                               &fake,
                               &result);
    TEST_CHECK(result.commands == 2);
    TEST_CHECK(result.bytes == 4);
    TEST_CHECK(result.processing_us == 5000);
    TEST_CHECK(result.budget_due);
    TEST_CHECK(fake.count == 2);
    TEST_CHECK(fake.order[0] == 1);
    TEST_CHECK(fake.order[1] == 2);

    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.enqueued == 3);
    TEST_CHECK(statistics.dequeued == 2);
    TEST_CHECK(statistics.budget_yields == 1);
    TEST_CHECK(statistics.recoveries == 0);
    TEST_CHECK(statistics.depth == 1);
    TEST_CHECK(statistics.bytes == 2);
    TEST_CHECK(statistics.peak_depth == 3);
    TEST_CHECK(statistics.peak_bytes == 6);
    TEST_CHECK(statistics.current_oldest_age_us == 5070);
    TEST_CHECK(statistics.oldest_age_us == 5070);
    TEST_CHECK(statistics.processing_us == 5000);
    TEST_CHECK(statistics.due);
    TEST_CHECK(statistics.budget_due);

    client_command_queue_drain(CLIENT_COMMAND_QUEUE_BUDGET_US,
                               fake_clock,
                               &fake,
                               fake_map_dispatch,
                               &fake,
                               &result);
    TEST_CHECK(result.commands == 1);
    TEST_CHECK(result.bytes == 2);
    TEST_CHECK(result.processing_us == 2500);
    TEST_CHECK(!result.budget_due);
    TEST_CHECK(fake.count == 3);
    TEST_CHECK(fake.order[2] == 3);

    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.enqueued == 3);
    TEST_CHECK(statistics.dequeued == 3);
    TEST_CHECK(statistics.budget_yields == 1);
    TEST_CHECK(statistics.recoveries == 1);
    TEST_CHECK(statistics.depth == 0);
    TEST_CHECK(statistics.bytes == 0);
    TEST_CHECK(statistics.oldest_age_us == 5070);
    TEST_CHECK(statistics.current_oldest_age_us == 0);
    TEST_CHECK(statistics.processing_us == 7500);
    TEST_CHECK(statistics.enqueued_order_digest != 0);
    TEST_CHECK(statistics.enqueued_order_digest == statistics.dequeued_order_digest);
    TEST_CHECK(statistics.order_digests_comparable);
    TEST_CHECK(!statistics.due);
    TEST_CHECK(!statistics.budget_due);
    TEST_CHECK(client_command_queue_statistics_reset());
}

static void test_prepend_marks_streaming_digests_incomparable(void) {
    fake_dispatch_t fake = {
        .now_us = 1000,
        .command_cost_us = 1,
        .continue_dispatch = true,
    };
    client_command_queue_drain_result_t result;
    client_command_queue_statistics_t statistics;

    TEST_CHECK(client_command_queue_statistics_reset());
    enqueue_map(1, 900);
    enqueue_map(2, 950);
    command_buffer *parent = get_next_input_command();
    TEST_CHECK(parent != NULL);
    TEST_CHECK(parent->data[1] == 1);
    command_buffer_free(parent);

    const uint8_t expanded[] = {CLIENT_CMD_MAP, 3};
    add_input_command(command_buffer_new(sizeof(expanded), (uint8_t *)expanded));
    client_command_queue_drain(0, fake_clock, &fake, fake_map_dispatch, &fake, &result);
    TEST_CHECK(result.commands == 2);
    TEST_CHECK(fake.count == 2);
    TEST_CHECK(fake.order[0] == 3);
    TEST_CHECK(fake.order[1] == 2);

    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.enqueued == 3);
    TEST_CHECK(statistics.dequeued == 3);
    TEST_CHECK(!statistics.order_digests_comparable);
    TEST_CHECK(statistics.enqueued_order_digest != statistics.dequeued_order_digest);
    TEST_CHECK(client_command_queue_statistics_reset());
}

static void test_unbounded_drain_and_clear(void) {
    fake_dispatch_t fake = {
        .now_us = 1000,
        .command_cost_us = 9000,
        .continue_dispatch = true,
    };
    client_command_queue_drain_result_t result;
    client_command_queue_statistics_t statistics;

    enqueue_map(4, 900);
    enqueue_map(5, 950);
    client_command_queue_drain(0, fake_clock, &fake, fake_map_dispatch, &fake, &result);
    TEST_CHECK(result.commands == 2);
    TEST_CHECK(!result.budget_due);
    TEST_CHECK(fake.order[0] == 4);
    TEST_CHECK(fake.order[1] == 5);

    enqueue_map(6, fake.now_us);
    client_command_queue_clear();
    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.enqueued == 3);
    TEST_CHECK(statistics.dequeued == 2);
    TEST_CHECK(statistics.enqueued_order_digest != statistics.dequeued_order_digest);
    TEST_CHECK(statistics.depth == 0);
    TEST_CHECK(!statistics.due);
    TEST_CHECK(client_command_queue_statistics_reset());
    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.enqueued == 0);
    TEST_CHECK(statistics.dequeued == 0);
    TEST_CHECK(statistics.enqueued_order_digest == 0);
    TEST_CHECK(statistics.dequeued_order_digest == 0);
}

static void test_dispatch_pause_preserves_remaining_commands(void) {
    fake_dispatch_t fake = {
        .now_us = 1000,
        .command_cost_us = 1,
        .continue_dispatch = false,
    };
    client_command_queue_drain_result_t result;
    client_command_queue_statistics_t statistics;

    TEST_CHECK(client_command_queue_statistics_reset());
    enqueue_map(1, 900);
    enqueue_map(2, 950);
    client_command_queue_drain(0, fake_clock, &fake, fake_map_dispatch, &fake, &result);
    TEST_CHECK(result.commands == 1);
    TEST_CHECK(fake.count == 1);
    client_command_queue_statistics_get(fake.now_us, &statistics);
    TEST_CHECK(statistics.depth == 1);

    fake.continue_dispatch = true;
    client_command_queue_drain(0, fake_clock, &fake, fake_map_dispatch, &fake, &result);
    TEST_CHECK(result.commands == 1);
    TEST_CHECK(fake.count == 2);
    TEST_CHECK(fake.order[0] == 1);
    TEST_CHECK(fake.order[1] == 2);
    TEST_CHECK(client_command_queue_statistics_reset());
}

static void test_deinitialize_resets_reconnect_state(void) {
    client_command_queue_statistics_t statistics;
    enqueue_map(7, 1);
    client_command_queue_deinitialize();
    TEST_CHECK(client_command_queue_initialize());
    client_command_queue_statistics_get(1, &statistics);
    TEST_CHECK(statistics.enqueued == 0);
    TEST_CHECK(statistics.dequeued == 0);
    TEST_CHECK(statistics.depth == 0);
    TEST_CHECK(!statistics.due);
}

int main(void) {
    toolkit_import(datetime);
    TEST_CHECK(client_command_queue_initialize());
    test_empty_tick_and_validation();
    test_ordered_budget_yield_and_recovery();
    test_prepend_marks_streaming_digests_incomparable();
    test_unbounded_drain_and_clear();
    test_dispatch_pause_preserves_remaining_commands();
    test_deinitialize_resets_reconnect_state();
    client_command_queue_deinitialize();
    toolkit_deinit();
    return 0;
}
