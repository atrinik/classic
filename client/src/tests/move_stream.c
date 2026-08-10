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

#include <global.h>
#include <toolkit/packet.h>

#define TEST_CHECK(condition)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                        \
        }                                                                   \
    } while (0)

Client_Player cpl;

typedef struct captured_packet {
    uint8_t type;
    uint8_t data[8];
    size_t len;
} captured_packet;

static captured_packet captured[4];
static size_t captured_num;

void socket_send_packet(packet_struct *packet) {
    TEST_CHECK(captured_num < arraysize(captured));
    TEST_CHECK(packet_writer_finish(packet));
    TEST_CHECK(packet->len <= sizeof(captured[captured_num].data));
    captured[captured_num].type = packet->type;
    captured[captured_num].len = packet->len;
    memcpy(captured[captured_num].data, packet->data, packet->len);
    captured_num++;
    packet_free(packet);
}

int64_t setting_get_int(int cat, int setting) {
    (void)cat;
    (void)setting;
    return 0;
}

static void capture_reset(void) {
    memset(captured, 0, sizeof(captured));
    captured_num = 0;
}

static void expect_clear(size_t index, bool scoped, uint8_t command) {
    TEST_CHECK(index < captured_num);
    TEST_CHECK(captured[index].type == SERVER_CMD_CLEAR);
    TEST_CHECK(captured[index].len == (scoped ? 1 : 0));
    if (scoped) {
        TEST_CHECK(captured[index].data[0] == command);
    }
}

static void test_movement_replacement_packets(void) {
    capture_reset();
    cpl.fire_on = 0;
    cpl.run_on = 1;
    move_keys_replace(3);
    TEST_CHECK(captured_num == 2);
    expect_clear(0, true, SERVER_CMD_MOVE);
    TEST_CHECK(captured[1].type == SERVER_CMD_MOVE && captured[1].len == 2);
    TEST_CHECK(captured[1].data[0] == 4 && captured[1].data[1] == 1);
    TEST_CHECK(move_keys_run_stream_active());

    capture_reset();
    cpl.fire_on = 1;
    move_keys_replace(9);
    TEST_CHECK(captured_num == 2);
    expect_clear(0, true, SERVER_CMD_FIRE);
    TEST_CHECK(captured[1].type == SERVER_CMD_FIRE && captured[1].len == 1);
    TEST_CHECK(captured[1].data[0] == 2);
}

static void test_stop_and_stay_packets(void) {
    capture_reset();
    cpl.fire_on = 1;
    move_keys_run_stop();
    TEST_CHECK(captured_num == 1);
    expect_clear(0, true, SERVER_CMD_MOVE);
    TEST_CHECK(!move_keys_run_stream_active());

    capture_reset();
    cpl.fire_on = 0;
    move_keys(5);
    TEST_CHECK(captured_num == 1);
    expect_clear(0, false, 0);
}

int main(void) {
    toolkit_import(packet);
    test_movement_replacement_packets();
    test_stop_and_stay_packets();
    toolkit_deinit();
    return 0;
}
