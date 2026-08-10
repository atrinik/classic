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
    uint8_t data[16];
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

static void expect_clear(size_t index, bool scoped, uint8_t command, uint32_t epoch) {
    TEST_CHECK(index < captured_num);
    TEST_CHECK(captured[index].type == SERVER_CMD_CLEAR);
    TEST_CHECK(captured[index].len == (scoped ? 5 : 0));
    if (scoped) {
        TEST_CHECK(captured[index].data[0] == command);
        TEST_CHECK(captured[index].data[1] == (uint8_t)(epoch >> 24));
        TEST_CHECK(captured[index].data[2] == (uint8_t)(epoch >> 16));
        TEST_CHECK(captured[index].data[3] == (uint8_t)(epoch >> 8));
        TEST_CHECK(captured[index].data[4] == (uint8_t)epoch);
    }
}

static void test_movement_replacement_packets(void) {
    capture_reset();
    cpl.fire_on = 0;
    cpl.run_on = 1;
    move_keys_replace(3, UINT32_C(0x01020304));
    TEST_CHECK(captured_num == 2);
    expect_clear(0, true, SERVER_CMD_MOVE, UINT32_C(0x01020304));
    TEST_CHECK(captured[1].type == SERVER_CMD_MOVE && captured[1].len == 6);
    TEST_CHECK(captured[1].data[0] == 4 && captured[1].data[1] == 1);
    TEST_CHECK(captured[1].data[2] == 1 && captured[1].data[3] == 2 && captured[1].data[4] == 3 &&
               captured[1].data[5] == 4);
    TEST_CHECK(move_keys_run_stream_active());

    capture_reset();
    cpl.fire_on = 1;
    move_keys_replace(9, UINT32_C(0x05060708));
    TEST_CHECK(captured_num == 2);
    expect_clear(0, true, SERVER_CMD_FIRE, UINT32_C(0x05060708));
    TEST_CHECK(captured[1].type == SERVER_CMD_FIRE && captured[1].len == 9);
    TEST_CHECK(captured[1].data[0] == 2);
    TEST_CHECK(captured[1].data[1] == 0 && captured[1].data[2] == 0 && captured[1].data[3] == 0 &&
               captured[1].data[4] == 0);
    TEST_CHECK(captured[1].data[5] == 5 && captured[1].data[6] == 6 && captured[1].data[7] == 7 &&
               captured[1].data[8] == 8);
}

static void test_stop_and_stay_packets(void) {
    capture_reset();
    cpl.fire_on = 1;
    move_keys_run_stop();
    TEST_CHECK(captured_num == 1);
    TEST_CHECK(captured[0].type == SERVER_CMD_MOVE && captured[0].len == 6);
    TEST_CHECK(captured[0].data[0] == 0 && captured[0].data[1] == 0);
    TEST_CHECK(!move_keys_run_stream_active());

    capture_reset();
    move_keys_stream_stop(UINT32_C(0x090a0b0c));
    TEST_CHECK(captured_num == 1);
    expect_clear(0, true, SERVER_CMD_MOVE, UINT32_C(0x090a0b0c));

    capture_reset();
    cpl.fire_on = 0;
    move_keys(5);
    TEST_CHECK(captured_num == 1);
    expect_clear(0, false, 0, 0);
}

static void test_direct_actions_use_unreplaceable_epoch(void) {
    capture_reset();
    cpl.fire_on = 0;
    cpl.run_on = 0;
    move_keys(3);
    TEST_CHECK(captured_num == 1);
    TEST_CHECK(captured[0].type == SERVER_CMD_MOVE && captured[0].len == 6);
    TEST_CHECK(captured[0].data[2] == 0 && captured[0].data[3] == 0 && captured[0].data[4] == 0 &&
               captured[0].data[5] == 0);

    capture_reset();
    client_send_fire(5, 42);
    TEST_CHECK(captured_num == 1);
    TEST_CHECK(captured[0].type == SERVER_CMD_FIRE && captured[0].len == 9);
    TEST_CHECK(captured[0].data[1] == 0 && captured[0].data[2] == 0 && captured[0].data[3] == 0 &&
               captured[0].data[4] == 42);
    TEST_CHECK(captured[0].data[5] == 0 && captured[0].data[6] == 0 && captured[0].data[7] == 0 &&
               captured[0].data[8] == 0);
}

int main(void) {
    toolkit_import(packet);
    test_movement_replacement_packets();
    test_stop_and_stay_packets();
    test_direct_actions_use_unreplaceable_epoch();
    toolkit_deinit();
    return 0;
}
