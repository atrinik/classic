#include <global.h>
#include <player_status.h>
#include <toolkit/packet.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static void write_entry(packet_struct *packet,
                        const char *key,
                        uint16_t face,
                        const char *name,
                        const char *tooltip,
                        int32_t seconds) {
    packet_writer_write_cstring(packet, key);
    packet_writer_write_uint16(packet, face);
    packet_writer_write_cstring(packet, name);
    packet_writer_write_cstring(packet, tooltip);
    packet_writer_write_int32(packet, seconds);
}

static packet_struct *make_upsert(const char *key, int32_t seconds) {
    packet_struct *packet = packet_new(0, 128, 64);
    packet_writer_write_uint8(packet, PLAYER_STATUS_UPSERT);
    write_entry(packet, key, 42, "status", "tooltip", seconds);
    return packet;
}

static void test_snapshot_add_update_remove(void) {
    player_status_model_t model = {0};
    packet_struct *packet = packet_new(0, 256, 64);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, 2);
    write_entry(packet, "disease:flu", 1, "flu", "aches", -1);
    write_entry(packet, "effect:7", 2, "strength", "stronger", 30);
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 2);
    TEST_CHECK(player_status_model_find(&model, "disease:flu")->seconds == -1);
    TEST_CHECK(player_status_model_find(&model, "effect:7")->seconds == 30);
    packet_free(packet);

    packet = make_upsert("effect:7", 60);
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 2);
    TEST_CHECK(player_status_model_find(&model, "effect:7")->seconds == 60);
    packet_free(packet);

    packet = make_upsert("effect:8", 1);
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 3);
    TEST_CHECK(player_status_model_tick(&model, 2));
    TEST_CHECK(player_status_model_find(&model, "effect:8")->seconds == 0);
    TEST_CHECK(player_status_model_find(&model, "disease:flu")->seconds == -1);
    packet_free(packet);

    packet = packet_new(0, 32, 16);
    packet_writer_write_uint8(packet, PLAYER_STATUS_REMOVE);
    packet_writer_write_cstring(packet, "effect:7");
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 2);
    TEST_CHECK(player_status_model_find(&model, "effect:7") == NULL);
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 2);
    packet_free(packet);

    packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, 0);
    TEST_CHECK(player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 0);
    packet_free(packet);
    player_status_model_clear(&model);
}

static void test_malformed_packets_are_transactional(void) {
    player_status_model_t model = {0};
    packet_struct *valid = make_upsert("disease:warts", -1);
    TEST_CHECK(player_status_parse_command(&model, valid->data, valid->len, 0));

    for (size_t len = 0; len < valid->len; len++) {
        TEST_CHECK(!player_status_parse_command(&model, valid->data, len, 0));
        TEST_CHECK(model.count == 1);
        TEST_CHECK(player_status_model_find(&model, "disease:warts") != NULL);
    }

    packet_struct *packet = make_upsert("effect:bad", -2);
    TEST_CHECK(!player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(player_status_model_find(&model, "effect:bad") == NULL);
    packet_free(packet);

    packet = packet_new(0, 128, 64);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, 1);
    write_entry(packet, "disease:flu", 1, "flu", "aches", -1);
    for (size_t len = 0; len < packet->len; len++) {
        TEST_CHECK(!player_status_parse_command(&model, packet->data, len, 0));
        TEST_CHECK(model.count == 1);
        TEST_CHECK(player_status_model_find(&model, "disease:warts") != NULL);
    }
    packet_free(packet);

    packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, ATRINIK_PLAYER_STATUS_MAX_STATUSES + 1U);
    TEST_CHECK(!player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 1);
    packet_free(packet);

    char oversized[ATRINIK_PLAYER_STATUS_KEY_SIZE + 2U];
    memset(oversized, 'x', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    packet = make_upsert(oversized, 1);
    TEST_CHECK(!player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 1);
    packet_free(packet);

    packet = packet_new(0, 256, 64);
    packet_writer_write_uint8(packet, PLAYER_STATUS_SNAPSHOT);
    packet_writer_write_uint16(packet, 2);
    write_entry(packet, "duplicate", 1, "first", "", 1);
    write_entry(packet, "duplicate", 2, "second", "", 2);
    TEST_CHECK(!player_status_parse_command(&model, packet->data, packet->len, 0));
    TEST_CHECK(model.count == 1);
    TEST_CHECK(player_status_model_find(&model, "disease:warts") != NULL);
    packet_free(packet);

    const uint8_t unknown[] = {PLAYER_STATUS_OPERATION_NROF};
    TEST_CHECK(!player_status_parse_command(&model, unknown, sizeof(unknown), 0));
    TEST_CHECK(model.count == 1);

    packet_free(valid);
    player_status_model_clear(&model);
}

int main(void) {
    toolkit_import(packet);
    test_snapshot_add_update_remove();
    test_malformed_packets_are_transactional();
    toolkit_deinit();
    return 0;
}
