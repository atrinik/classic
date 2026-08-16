#include <global.h>
#include <interface_packet.h>
#include <item_packet.h>

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static void write_item_record(packet_struct *packet, const char *glow) {
    packet_writer_write_uint32(packet, 42);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_uint32(packet, 1000);
    packet_writer_write_uint16(packet, 1);
    packet_writer_write_uint8(packet, 2);
    packet_writer_write_uint8(packet, TYPE_FORCE);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 100);
    packet_writer_write_uint8(packet, 90);
    packet_writer_write_uint8(packet, 1);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_cstring(packet, "test item");
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint32(packet, 1);
    packet_writer_write_cstring(packet, glow);
    packet_writer_write_uint8(packet, 1);
}

static packet_struct *make_item_command(const char *glow) {
    packet_struct *packet = packet_new(0, 128, 64);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint32(packet, 1);
    packet_writer_write_uint8(packet, 0);
    write_item_record(packet, glow);
    return packet;
}

static packet_struct *make_interface_command(const char *glow) {
    packet_struct *packet = packet_new(0, 32, 32);
    packet_writer_write_uint8(packet, CMD_INTERFACE_OBJECT);
    packet_writer_write_uint16(packet, UPD_GLOW);
    packet_writer_write_uint32(packet, 7);
    packet_writer_write_cstring(packet, glow);
    packet_writer_write_uint8(packet, 1);
    return packet;
}

static packet_struct *make_spell_extra_update(const char *message) {
    packet_struct *packet = packet_new(0, 32, 32);
    packet_writer_write_uint8(packet, TYPE_SPELL);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 255);
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_cstring(packet, message);
    return packet;
}

static packet_struct *make_spell_item_command(const char *message) {
    packet_struct *packet = packet_new(0, 128, 64);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint32(packet, 1);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_uint32(packet, 1000);
    packet_writer_write_uint16(packet, 1);
    packet_writer_write_uint8(packet, 2);
    packet_writer_write_uint8(packet, TYPE_SPELL);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 255);
    packet_writer_write_cstring(packet, "test spell");
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint32(packet, 1);
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_cstring(packet, message);
    packet_writer_write_cstring(packet, "#dbce3");
    packet_writer_write_uint8(packet, 1);
    return packet;
}

static void check_interface_payload_truncations(packet_struct *packet) {
    TEST_CHECK(interface_packet_validate(packet->data, packet->len, 0));
    for (size_t len = 1; len < packet->len; len++) {
        TEST_CHECK(!interface_packet_validate(packet->data, len, 0));
    }
    packet_free(packet);
}

static void test_item_command(void) {
    packet_struct *packet = make_item_command("#dbce3");
    TEST_CHECK(item_packet_validate_command(packet->data, packet->len, 0));
    for (size_t len = 0; len < packet->len; len++) {
        if (len != 6) {
            TEST_CHECK(!item_packet_validate_command(packet->data, len, 0));
        }
    }

    packet_writer_write_uint8(packet, 0xff);
    TEST_CHECK(!item_packet_validate_command(packet->data, packet->len, 0));
    packet_free(packet);

    const uint8_t unsupported_delete[] = {2};
    TEST_CHECK(!item_packet_validate_command(unsupported_delete, sizeof(unsupported_delete), 0));

    const uint8_t unsupported_bflag[] = {0, 0, 0, 0, 1, 2};
    TEST_CHECK(!item_packet_validate_command(unsupported_bflag, sizeof(unsupported_bflag), 0));

    const uint8_t unsupported_apply[] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 4};
    TEST_CHECK(!item_packet_validate_command(unsupported_apply, sizeof(unsupported_apply), 0));

    const uint8_t delete_only[] = {1, 0, 0, 0, 42};
    TEST_CHECK(item_packet_validate_command(delete_only, sizeof(delete_only), 0));

    packet_reader_t reader;
    packet_reader_init(&reader, NULL, 0);
    object base = {0};
    item_packet_update_t update;
    TEST_CHECK(!item_packet_parse_update(&reader, 1U << 31, &base, &update));
    TEST_CHECK(packet_reader_error(&reader) == PACKET_ERROR_UNSUPPORTED);
}

static packet_struct *make_name_count_update(const char *name, uint32_t nrof) {
    packet_struct *packet = packet_new(0, 32, 32);
    packet_writer_write_cstring(packet, name);
    packet_writer_write_uint32(packet, nrof);
    return packet;
}

static void test_name_count_update(void) {
    object base = {.nrof = 1};
    object next = {0};
    object prev = {0};
    object env = {0};
    object inv = {0};
    base.next = &next;
    base.prev = &prev;
    base.env = &env;
    base.inv = &inv;
    snprintf(VS(base.s_name), "torch");

    packet_struct *packet = make_name_count_update("torches", 2);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    item_packet_update_t update;
    TEST_CHECK(item_packet_parse_update(&reader, UPD_NAME | UPD_NROF, &base, &update));
    TEST_CHECK(packet_reader_finish(&reader));
    TEST_CHECK(strcmp(update.item.s_name, "torches") == 0);
    TEST_CHECK(update.item.nrof == 2);
    TEST_CHECK(strcmp(base.s_name, "torch") == 0);
    TEST_CHECK(base.nrof == 1);
    item_packet_apply_update(&update, &base);
    TEST_CHECK(strcmp(base.s_name, "torches") == 0);
    TEST_CHECK(base.nrof == 2);
    TEST_CHECK(base.next == &next);
    TEST_CHECK(base.prev == &prev);
    TEST_CHECK(base.env == &env);
    TEST_CHECK(base.inv == &inv);

    for (size_t len = 0; len < packet->len; len++) {
        packet_reader_init(&reader, packet->data, len);
        object before = base;
        TEST_CHECK(!item_packet_parse_update(&reader, UPD_NAME | UPD_NROF, &base, &update));
        TEST_CHECK(memcmp(&base, &before, sizeof(base)) == 0);
    }
    packet_free(packet);

    packet = make_name_count_update("torch", 1);
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(item_packet_parse_update(&reader, UPD_NAME | UPD_NROF, &base, &update));
    TEST_CHECK(packet_reader_finish(&reader));
    item_packet_apply_update(&update, &base);
    TEST_CHECK(strcmp(base.s_name, "torch") == 0);
    TEST_CHECK(base.nrof == 1);
    packet_free(packet);

    char oversized[NAME_LEN + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    packet = make_name_count_update(oversized, 2);
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(!item_packet_parse_update(&reader, UPD_NAME | UPD_NROF, &base, &update));
    TEST_CHECK(packet_reader_error(&reader) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    packet = packet_new(0, 16, 16);
    packet_writer_write_uint32(packet, 2);
    packet_writer_write_cstring(packet, "torches");
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(!item_packet_parse_update(&reader, UPD_NAME | UPD_NROF, &base, &update) ||
               !packet_reader_finish(&reader));
    packet_free(packet);
}

static void test_extra_message_limit(void) {
    object base = {0};
    item_packet_update_t update;
    packet_reader_t reader;

    char exact[ATRINIK_PROTOCOL_ITEM_EXTRA_MESSAGE_SIZE];
    memset(exact, 'x', sizeof(exact) - 1);
    exact[sizeof(exact) - 1] = '\0';
    packet_struct *packet = make_spell_extra_update(exact);
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(item_packet_parse_update(&reader, UPD_TYPE | UPD_EXTRA, &base, &update));
    TEST_CHECK(packet_reader_finish(&reader));
    TEST_CHECK(strlen(update.extra_message) == sizeof(exact) - 1);
    packet_free(packet);

    char oversized[ATRINIK_PROTOCOL_ITEM_EXTRA_MESSAGE_SIZE + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    packet = make_spell_extra_update(oversized);
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(!item_packet_parse_update(&reader, UPD_TYPE | UPD_EXTRA, &base, &update));
    TEST_CHECK(packet_reader_error(&reader) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    char utf8_boundary[ATRINIK_PROTOCOL_ITEM_EXTRA_MESSAGE_SIZE + 1];
    memset(utf8_boundary, 'x', 253);
    memcpy(utf8_boundary + 253, "\xE2\x82\xAC", 3);
    utf8_boundary[256] = '\0';
    packet = make_spell_extra_update(utf8_boundary);
    packet_reader_init(&reader, packet->data, packet->len);
    TEST_CHECK(!item_packet_parse_update(&reader, UPD_TYPE | UPD_EXTRA, &base, &update));
    TEST_CHECK(packet_reader_error(&reader) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    packet = make_spell_item_command(oversized);
    TEST_CHECK(!item_packet_validate_command(packet->data, packet->len, 0));
    packet_free(packet);
}

static void test_glow_limit_and_error_scope(void) {
    packet_struct *packet = make_item_command("#dbce3b");
    packet_reader_scope_t scope;
    packet_reader_scope_begin(&scope);
    TEST_CHECK(!item_packet_validate_command(packet->data, packet->len, 0));
    TEST_CHECK(packet_reader_scope_finish(&scope) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);

    packet = make_interface_command("#dbce3b");
    packet_reader_scope_begin(&scope);
    TEST_CHECK(!interface_packet_validate(packet->data, packet->len, 0));
    TEST_CHECK(packet_reader_scope_finish(&scope) == PACKET_ERROR_LIMIT_EXCEEDED);
    packet_free(packet);
}

static void test_interface_command(void) {
    packet_struct *packet = make_interface_command("#dbce3");
    check_interface_payload_truncations(packet);

    packet = make_interface_command("#dbce3");
    packet_writer_write_uint8(packet, 0xff);
    TEST_CHECK(!interface_packet_validate(packet->data, packet->len, 0));
    packet_free(packet);

    const uint8_t unknown_type[] = {0xff};
    packet_reader_scope_t scope;
    packet_reader_scope_begin(&scope);
    TEST_CHECK(!interface_packet_validate(unknown_type, sizeof(unknown_type), 0));
    TEST_CHECK(packet_reader_scope_finish(&scope) == PACKET_ERROR_UNSUPPORTED);

    packet = packet_new(0, 8, 8);
    packet_writer_write_uint8(packet, CMD_INTERFACE_OBJECT);
    packet_writer_write_uint16(packet, 1U << 15);
    packet_writer_write_uint32(packet, 1);
    packet_reader_scope_begin(&scope);
    TEST_CHECK(!interface_packet_validate(packet->data, packet->len, 0));
    TEST_CHECK(packet_reader_scope_finish(&scope) == PACKET_ERROR_UNSUPPORTED);
    packet_free(packet);
}

static void test_interface_fields(void) {
    static const uint8_t string_types[] = {
        CMD_INTERFACE_TEXT,
        CMD_INTERFACE_LINK,
        CMD_INTERFACE_ICON,
        CMD_INTERFACE_TITLE,
        CMD_INTERFACE_INPUT,
        CMD_INTERFACE_INPUT_PREPEND,
        CMD_INTERFACE_AUTOCOMPLETE,
        CMD_INTERFACE_APPEND_TEXT,
    };
    for (size_t i = 0; i < arraysize(string_types); i++) {
        packet_struct *packet = packet_new(0, 16, 16);
        packet_writer_write_uint8(packet, string_types[i]);
        packet_writer_write_cstring(packet, "value");
        check_interface_payload_truncations(packet);
    }

    packet_struct *packet = packet_new(0, 8, 8);
    packet_writer_write_uint8(packet, CMD_INTERFACE_ANIM);
    packet_writer_write_uint16(packet, 1);
    packet_writer_write_uint8(packet, 2);
    packet_writer_write_uint8(packet, 3);
    check_interface_payload_truncations(packet);

    static const uint8_t flag_types[] = {
        CMD_INTERFACE_ALLOW_TAB,
        CMD_INTERFACE_INPUT_CLEANUP_DISABLE,
        CMD_INTERFACE_INPUT_ALLOW_EMPTY,
        CMD_INTERFACE_SCROLL_BOTTOM,
        CMD_INTERFACE_RESTORE,
    };
    for (size_t i = 0; i < arraysize(flag_types); i++) {
        TEST_CHECK(interface_packet_validate(&flag_types[i], 1, 0));
    }

    const uint8_t duplicate_restore[] = {CMD_INTERFACE_RESTORE, CMD_INTERFACE_RESTORE};
    TEST_CHECK(!interface_packet_validate(duplicate_restore, sizeof(duplicate_restore), 0));

    packet = packet_new(0, 32, 16);
    for (size_t i = 0; i < 2; i++) {
        packet_writer_write_uint8(packet, CMD_INTERFACE_ICON);
        packet_writer_write_cstring(packet, "icon");
        packet_writer_write_uint8(packet, CMD_INTERFACE_ANIM);
        packet_writer_write_uint16(packet, 1);
        packet_writer_write_uint8(packet, 2);
        packet_writer_write_uint8(packet, 3);
    }
    TEST_CHECK(interface_packet_validate(packet->data, packet->len, 0));
    packet_free(packet);
}

static void test_bounded_fuzz_regression(void) {
    uint32_t state = 0x48a7c315U;
    uint8_t data[64];
    for (size_t iteration = 0; iteration < 10000; iteration++) {
        state = state * 1664525U + 1013904223U;
        size_t len = state % (sizeof(data) + 1);
        for (size_t i = 0; i < len; i++) {
            state = state * 1664525U + 1013904223U;
            data[i] = (uint8_t)(state >> 24);
        }
        (void)item_packet_validate_command(data, len, 0);
        (void)interface_packet_validate(data, len, 0);
    }
}

int main(void) {
    toolkit_import(packet);
    test_item_command();
    test_name_count_update();
    test_extra_message_limit();
    test_glow_limit_and_error_scope();
    test_interface_command();
    test_interface_fields();
    test_bounded_fuzz_regression();
    toolkit_deinit();
    return 0;
}
