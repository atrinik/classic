/*************************************************************************
 * Atrinik server duplicator quantity-update regression tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <server.h>
#include <server_item.h>
#include <toolkit/packet.h>

static packet_struct *queued_command_find(socket_struct *cs, uint8_t type) {
    packet_struct *found = NULL;

    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            found = packet;
        }
    }

    return found;
}

static bool look_packet_has_item(packet_struct *packet,
                                 tag_t tag,
                                 const char *expected_name,
                                 uint32_t expected_nrof) {
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 0);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 0);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);

    while (reader.pos < reader.len && packet_reader_error(&reader) == PACKET_ERROR_NONE) {
        tag_t current_tag = packet_reader_read_uint32(&reader);
        if (current_tag == 0) {
            (void)packet_reader_read_uint8(&reader);
        }
        (void)packet_reader_read_uint32(&reader);
        (void)packet_reader_read_uint32(&reader);
        (void)packet_reader_read_uint16(&reader);
        (void)packet_reader_read_uint8(&reader);
        char name[ITEM_NAME_SIZE];
        packet_reader_read_string(&reader, VS(name));
        (void)packet_reader_read_uint16(&reader);
        (void)packet_reader_read_uint8(&reader);
        uint32_t nrof = packet_reader_read_uint32(&reader);
        char glow[8];
        packet_reader_read_string(&reader, VS(glow));
        (void)packet_reader_read_uint8(&reader);

        if (current_tag == tag) {
            return packet_reader_error(&reader) == PACKET_ERROR_NONE &&
                   strcmp(name, expected_name) == 0 && nrof == expected_nrof;
        }
    }

    return false;
}

START_TEST(test_trigger_refreshes_visible_stack) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *duplicator = arch_get("duplicator");
    duplicator->x = pl->x;
    duplicator->y = pl->y;
    duplicator->level = 2;
    FREE_AND_COPY_HASH(duplicator->slaying, "bolt");
    duplicator = object_insert_map(duplicator, map, NULL, INS_NO_MERGE);

    object *stack = arch_get("bolt");
    stack->x = pl->x;
    stack->y = pl->y;
    stack->nrof = 1;
    FREE_AND_COPY_HASH(stack->name, "torch");
    FREE_AND_COPY_HASH(stack->name_pl, "torches");
    stack = object_insert_map(stack, map, NULL, INS_NO_MERGE);

    uint8_t old_update = GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y);
    ck_assert_int_eq(object_trigger(duplicator, pl, 1), OBJECT_METHOD_OK);
    ck_assert_uint_eq(stack->nrof, 2);
    ck_assert_uint_ne(GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y), old_update);

    esrv_draw_look(pl);
    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_ITEM);
    ck_assert_ptr_nonnull(packet);
    ck_assert(look_packet_has_item(packet, stack->count, "torches", 2));

    object_destroy(pl);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("duplicator");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_trigger_refreshes_visible_stack);

    return s;
}

void check_types_duplicator(void) {
    check_run_suite(suite(), __FILE__);
}
