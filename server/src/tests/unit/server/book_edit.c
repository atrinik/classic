/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <server_main.h>
#include <server.h>
#include <server_item.h>
#include <book_edit.h>
#include <commands.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <plugin.h>
#include <toolkit/packet.h>

static packet_struct *last_packet(object *writer, uint8_t type) {
    packet_struct *last = NULL;
    for (packet_struct *packet = CONTR(writer)->cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            last = packet;
        }
    }
    ck_assert_ptr_nonnull(last);
    return last;
}

static packet_struct *last_reply(object *writer) {
    return last_packet(writer, CLIENT_CMD_BOOK_EDIT);
}

static uint32_t reply_id(object *writer, uint8_t expected_result) {
    packet_struct *packet = last_reply(writer);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), expected_result);
    return packet_reader_read_uint32(&reader);
}

static void mark(object *writer, object *book) {
    CONTR(writer)->mark = book;
    CONTR(writer)->mark_count = book->count;
}

static void setup_writer(object **writer, object **pen, object **book) {
    mapstruct *map;
    check_setup_env_pl(&map, writer);
    CONTR(*writer)->cs->state = ST_PLAYING;
    if (find_skill(*writer, SK_LITERACY) == NULL) {
        object_insert_into(arch_get("skill_literacy"), *writer, INS_NO_MERGE);
    }
    if (find_skill(*writer, SK_INSCRIPTION) == NULL) {
        object_insert_into(arch_get("skill_inscription"), *writer, INS_NO_MERGE);
    }
    *pen = arch_get("writing_pen");
    (*pen)->nrof = 1;
    FREE_AND_COPY_HASH((*pen)->race, "writing_ink");
    (*pen)->stats.maxhp = 1000;
    (*pen)->stats.food = 1000;
    object_insert_into(*pen, *writer, INS_NO_MERGE);
    *book = arch_get("book");
    (*book)->nrof = 1;
    FREE_AND_COPY_HASH((*book)->name, "Draft");
    FREE_AND_COPY_HASH((*book)->msg, "abc");
    object_insert_into(*book, *writer, INS_NO_MERGE);
    mark(*writer, *book);
}

static packet_struct *request(uint8_t action, uint32_t id, object *book, object *source,
                              const char *title, const char *body) {
    packet_struct *packet = packet_new(SERVER_CMD_BOOK_EDIT, 128, 128);
    packet_writer_write_uint8(packet, action);
    packet_writer_write_uint32(packet, id);
    packet_writer_write_uint32(packet, book->count);
    packet_writer_write_uint32(packet, source != NULL ? source->count : 0);
    packet_writer_write_cstring(packet, title);
    packet_writer_write_cstring(packet, body);
    return packet;
}

static void submit(object *writer, uint8_t action, uint32_t id, object *book, object *source,
                   const char *title, const char *body) {
    packet_struct *packet = request(action, id, book, source, title, body);
    socket_command_book_edit(CONTR(writer)->cs, CONTR(writer), packet->data, packet->len, 0);
    packet_free(packet);
}

START_TEST(test_cost_and_text_bounds) {
    ck_assert_uint_eq(book_edit_ink_cost("abc", "abc"), 0);
    ck_assert_uint_eq(book_edit_ink_cost("abcdef", "ace"), 0);
    ck_assert_uint_eq(book_edit_ink_cost("abc", "axc"), 1);
    ck_assert_uint_eq(book_edit_ink_cost("abc", "zabcq"), 2);
    ck_assert_uint_eq(book_edit_ink_cost("", "\xc3\xa9"), 2);
    ck_assert(book_edit_text_valid("Title", true));
    ck_assert(book_edit_text_valid("\xc3\xa9\n\t", false));
    ck_assert(!book_edit_text_valid("line\nname injected", true));
    ck_assert(!book_edit_text_valid(" title", true));
    ck_assert(!book_edit_text_valid("endmsg\narch sword", false));
    ck_assert(!book_edit_text_valid("ENDMSG", false));
    ck_assert(!book_edit_text_valid("\xc0\xaf", false));
    ck_assert(!book_edit_text_valid("\xed\xa0\x80", false));
    ck_assert(!book_edit_text_valid("\xf4\x90\x80\x80", false));
    ck_assert(!book_edit_text_valid("\xe2\x82", false));
    char boundary[BOOK_EDIT_CONTENT_MAX + 2];
    memset(boundary, 'a', sizeof(boundary));
    boundary[BOOK_EDIT_CONTENT_MAX] = '\0';
    ck_assert(book_edit_text_valid(boundary, false));
    boundary[BOOK_EDIT_CONTENT_MAX] = 'a';
    boundary[BOOK_EDIT_CONTENT_MAX + 1] = '\0';
    ck_assert(!book_edit_text_valid(boundary, false));
    boundary[BOOK_EDIT_TITLE_MAX] = '\0';
    ck_assert(book_edit_text_valid(boundary, true));
    boundary[BOOK_EDIT_TITLE_MAX] = 'a';
    boundary[BOOK_EDIT_TITLE_MAX + 1] = '\0';
    ck_assert(!book_edit_text_valid(boundary, true));
}
END_TEST

START_TEST(test_edit_copy_sign_and_persistence) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    object *copy = object_clone(book);
    FREE_AND_COPY_HASH(copy->name, "Destination");
    FREE_AND_COPY_HASH(copy->msg, "");
    object_set_value(copy, "quest_marker", "keep", true);
    object_insert_into(copy, writer, INS_NO_MERGE);
    /* Authored hooks and quest properties stay on the same object. */
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_APPLY;
    FREE_AND_COPY_HASH(event->race, "python");
    FREE_AND_COPY_HASH(event->slaying, "book_quest.py");
    object_insert_into(event, copy, INS_NO_MERGE);
    uint32_t event_flags = copy->event_flags;
    copy->level = 7;
    copy->value = 321;
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    ck_assert(!QUERY_FLAG(pen, FLAG_APPLIED));
    ck_assert_int_eq(pen->stats.food, 1000);
    char *description = object_get_description_s(pen, writer);
    ck_assert_ptr_nonnull(strstr(description, "ink 1000/1000"));
    free(description);
    FREE_AND_COPY_HASH(book->custom_name, "Old alias");
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "New title", "axc");
    reply_id(writer, BOOK_EDIT_UPDATED);
    ck_assert_str_eq(book->name, "New title");
    ck_assert_ptr_null(book->custom_name);
    ck_assert_str_eq(book->msg, "axc");
    ck_assert_int_eq(pen->stats.food, 999);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Renamed", "axc");
    ck_assert_int_eq(pen->stats.food, 999);
    submit(writer, BOOK_EDIT_SIGN, id, book, NULL, "Renamed", "unsaved");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(!book_edit_finalized(book));
    submit(writer, BOOK_EDIT_SIGN, id, book, NULL, "Renamed", "axc");
    reply_id(writer, BOOK_EDIT_UPDATED);
    ck_assert(book_edit_finalized(book));
    ck_assert_str_eq(object_get_value(book, BOOK_EDIT_SIGNER), writer->name);
    ck_assert_ptr_nonnull(object_get_value(book, BOOK_EDIT_DATE));
    ck_assert_ptr_nonnull(object_get_value(book, BOOK_EDIT_UTC));
    ck_assert_int_eq(pen->stats.food, 999);
    book->nrof = 2; /* Source stacks are safe: only the destination changes. */
    submit(writer, BOOK_EDIT_COPY, id, copy, book, "", "");
    reply_id(writer, BOOK_EDIT_UPDATED);
    ck_assert_str_eq(copy->name, "Renamed");
    ck_assert_str_eq(copy->msg, "axc");
    ck_assert(!book_edit_finalized(copy));
    ck_assert_ptr_null(object_get_value(copy, BOOK_EDIT_SIGNER));
    ck_assert_str_eq(object_get_value(copy, "quest_marker"), "keep");
    ck_assert_ptr_eq(copy->inv, event);
    ck_assert_ptr_eq(event->env, copy);
    ck_assert_uint_eq(copy->event_flags, event_flags);
    ck_assert_int_eq(copy->level, 7);
    ck_assert_int_eq(copy->value, 321);
    ck_assert_int_eq(pen->stats.food, 996);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Tampered", "axc");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->name, "Renamed");
    mark(writer, book);
    char rename[] = "Forged title";
    command_rename(writer, "rename", rename);
    ck_assert_ptr_null(book->custom_name);
    submit(writer, BOOK_EDIT_COPY, id, book, copy, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    StringBuffer *sb = stringbuffer_new();
    object_dump_rec(book, sb);
    char *dump = stringbuffer_finish(sb);
    object *loaded = object_load_str(dump);
    ck_assert_ptr_nonnull(loaded);
    ck_assert_str_eq(loaded->name, "Renamed");
    ck_assert_str_eq(loaded->msg, "axc");
    ck_assert(book_edit_finalized(loaded));
    ck_assert_str_eq(object_get_value(loaded, BOOK_EDIT_SIGNER), writer->name);
    ck_assert_str_eq(object_get_value(loaded, BOOK_EDIT_DATE), object_get_value(book, BOOK_EDIT_DATE));
    ck_assert_str_eq(object_get_value(loaded, BOOK_EDIT_UTC), object_get_value(book, BOOK_EDIT_UTC));
    object_destroy(loaded);
    free(dump);
    submit(writer, BOOK_EDIT_SIGN, id, copy, NULL, "Renamed", "axc");
    reply_id(writer, BOOK_EDIT_UPDATED);
    sb = stringbuffer_new();
    object_dump_rec(copy, sb);
    dump = stringbuffer_finish(sb);
    loaded = object_load_str(dump);
    ck_assert_ptr_nonnull(loaded);
    ck_assert(book_edit_finalized(loaded));
    ck_assert_str_eq(object_get_value(loaded, "quest_marker"), "keep");
    ck_assert_uint_eq(loaded->event_flags, event_flags);
    ck_assert_ptr_nonnull(loaded->inv);
    ck_assert_int_eq(loaded->inv->type, EVENT_OBJECT);
    ck_assert_int_eq(loaded->inv->sub_type, EVENT_APPLY);
    ck_assert_str_eq(loaded->inv->race, "python");
    ck_assert_str_eq(loaded->inv->slaying, "book_quest.py");
    ck_assert_int_eq(loaded->level, 7);
    ck_assert_int_eq(loaded->value, 321);
    object_destroy(loaded);
    free(dump);
    book_edit_clear(CONTR(writer));
}
END_TEST

START_TEST(test_visibility_payment_and_bounded_mark) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    object *hidden = object_insert_into(object_clone(book), writer, INS_NO_MERGE);
    SET_FLAG(hidden, FLAG_IS_INVISIBLE);
    object *bag = object_insert_into(arch_get("sack"), writer, INS_NO_MERGE);
    SET_FLAG(bag, FLAG_IS_INVISIBLE);
    object_insert_into(object_clone(book), bag, INS_NO_MERGE);
    object *unpaid = object_insert_into(object_clone(book), writer, INS_NO_MERGE);
    SET_FLAG(unpaid, FLAG_UNPAID);
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    packet_struct *packet = last_reply(writer);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    packet_reader_read_uint8(&reader);
    packet_reader_read_uint32(&reader);
    packet_reader_read_uint32(&reader);
    packet_reader_read_uint32(&reader);
    packet_reader_read_uint32(&reader);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), book->count);
    SET_FLAG(book, FLAG_IS_INVISIBLE);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(!book_edit_open(pen, writer));
    CLEAR_FLAG(book, FLAG_IS_INVISIBLE);
    ck_assert(book_edit_open(pen, writer));
    id = reply_id(writer, BOOK_EDIT_OPEN);
    SET_FLAG(pen, FLAG_UNPAID);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(!book_edit_open(pen, writer));
    CLEAR_FLAG(pen, FLAG_UNPAID);
    ck_assert(book_edit_open(pen, writer));
    id = reply_id(writer, BOOK_EDIT_OPEN);
    SET_FLAG(book, FLAG_UNPAID);
    submit(writer, BOOK_EDIT_SIGN, id, book, NULL, "Draft", "abc");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(!book_edit_open(pen, writer));
    CLEAR_FLAG(book, FLAG_UNPAID);
    ck_assert(!book_edit_finalized(book));
    ck_assert_str_eq(book->msg, "abc");
    ck_assert_int_eq(pen->stats.food, 1000);
    CONTR(writer)->mark_count++;
    ck_assert(!book_edit_open(pen, writer));
    mark(writer, book);
    object *container = writer;
    for (size_t i = 0; i < 33; i++) {
        container = object_insert_into(arch_get("sack"), container, INS_NO_MERGE);
    }
    object_remove(book, 0);
    object_insert_into(book, container, INS_NO_MERGE);
    ck_assert(!book_edit_open(pen, writer));
    ck_assert_ptr_null(CONTR(writer)->book_editor);
}
END_TEST

START_TEST(test_copy_source_and_destination_revalidation) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    object *source = object_insert_into(object_clone(book), writer, INS_NO_MERGE);
    FREE_AND_COPY_HASH(source->msg, "source text");
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    FREE_AND_COPY_HASH(source->msg, "script changed source");
    submit(writer, BOOK_EDIT_COPY, id, book, source, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(book_edit_open(pen, writer));
    id = reply_id(writer, BOOK_EDIT_OPEN);
    SET_FLAG(source, FLAG_UNPAID);
    submit(writer, BOOK_EDIT_COPY, id, book, source, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    CLEAR_FLAG(source, FLAG_UNPAID);
    SET_FLAG(source, FLAG_IS_INVISIBLE);
    submit(writer, BOOK_EDIT_COPY, id, book, source, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    CLEAR_FLAG(source, FLAG_IS_INVISIBLE);
    object_remove(source, 0);
    submit(writer, BOOK_EDIT_COPY, id, book, source, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    object_remove(book, 0);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->name, "Draft");
    ck_assert_str_eq(book->msg, "abc");
    ck_assert_int_eq(pen->stats.food, 1000);
    book_edit_clear(CONTR(writer));
    object_destroy(source);
    object_destroy(book);
}
END_TEST

START_TEST(test_all_book_producers_are_terminated) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    player *pl = CONTR(writer);
    object *quest_container = pl->quest_container;
    ck_assert_ptr_nonnull(quest_container);
    /* Cover the no-container response without changing the player's inventory.
     * Restore the normal root before insertion can recalculate player state. */
    pl->quest_container = NULL;
    socket_command_quest_list(pl->cs, pl, NULL, 0, 0);
    pl->quest_container = quest_container;
    packet_reader_t reader;
    packet_struct *packet = last_packet(writer, CLIENT_CMD_BOOK);
    char text[4096];
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert_str_eq(text, "[title]No quests to speak of.[/title]");
    ck_assert(packet_reader_finish(&reader));
    /* Use the same nested quest archetype/status tree as authored quests. */
    object *quest = arch_get(QUEST_CONTAINER_ARCHETYPE);
    quest->magic = QUEST_STATUS_STARTED;
    FREE_AND_COPY_HASH(quest->name, "book-producer-quest");
    FREE_AND_COPY_HASH(quest->race, "Keeper's request");
    object_insert_into(quest, quest_container, INS_NO_MERGE);
    object *part = arch_get(QUEST_CONTAINER_ARCHETYPE);
    part->magic = QUEST_STATUS_STARTED;
    part->sub_type = QUEST_TYPE_KILL;
    part->last_sp = 1;
    part->last_grace = 3;
    FREE_AND_COPY_HASH(part->name, "book-producer-objective");
    FREE_AND_COPY_HASH(part->race, "Find the book");
    FREE_AND_COPY_HASH(part->msg, "Return to the keeper.");
    object_insert_into(part, quest, INS_NO_MERGE);
    socket_command_quest_list(pl->cs, pl, NULL, 0, 0);
    packet = last_packet(writer, CLIENT_CMD_BOOK);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert_ptr_nonnull(strstr(text, "Keeper's request"));
    ck_assert_ptr_nonnull(strstr(text, "Find the book"));
    ck_assert_ptr_nonnull(strstr(text, "Return to the keeper."));
    ck_assert_ptr_nonnull(strstr(text, "Status: 1/3"));
    ck_assert(packet_reader_finish(&reader));
    player_apply(writer, book, 0, 0);
    packet = last_packet(writer, CLIENT_CMD_BOOK);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert_ptr_nonnull(strstr(text, "[/book]abc"));
    ck_assert(packet_reader_finish(&reader));
    object_set_value(book, BOOK_EDIT_SIGNER, "Keeper", true);
    object_set_value(book, BOOK_EDIT_DATE, "1 Day, Year 1", true);
    object_set_value(book, BOOK_EDIT_FINALIZED, "1", true);
    player_apply(writer, book, 0, 0);
    packet = last_packet(writer, CLIENT_CMD_BOOK);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert_str_eq(text, "Keeper");
    ck_assert(packet_reader_read_string(&reader, text, sizeof(text)));
    ck_assert_str_eq(text, "1 Day, Year 1");
    ck_assert(packet_reader_finish(&reader));
}
END_TEST

START_TEST(test_stale_custody_and_content) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    object *bag = object_insert_into(arch_get("sack"), writer, INS_NO_MERGE);
    object_remove(book, 0);
    object_insert_into(book, bag, INS_NO_MERGE);
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    object_remove(bag, 0);
    object_insert_into(bag, writer, INS_NO_MERGE);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->msg, "abc");
    ck_assert_int_eq(pen->stats.food, 1000);
    ck_assert(book_edit_open(pen, writer));
    id = reply_id(writer, BOOK_EDIT_OPEN);
    FREE_AND_COPY_HASH(book->msg, "script changed this");
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->msg, "script changed this");
    ck_assert(book_edit_open(pen, writer));
    id = reply_id(writer, BOOK_EDIT_OPEN);
    object_remove(pen, 0);
    object_insert_into(pen, writer, INS_NO_MERGE);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Changed", "def");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_int_eq(pen->stats.food, 1000);
    book_edit_clear(CONTR(writer));
}
END_TEST

START_TEST(test_refill_failed_commit_and_stacks) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    pen->stats.food = 1;
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Draft", "xyz");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_int_eq(pen->stats.food, 1);
    ck_assert_str_eq(book->msg, "abc");
    object *bottle = arch_get("sack");
    bottle->type = LIGHT_REFILL;
    bottle->stats.food = 1000;
    bottle->nrof = 2;
    SET_FLAG(bottle, FLAG_CAN_STACK);
    FREE_AND_COPY_HASH(bottle->race, "writing_ink");
    object_insert_into(bottle, writer, INS_NO_MERGE);
    mark(writer, pen);
    SET_FLAG(pen, FLAG_UNPAID);
    player_apply(writer, bottle, 0, 0);
    ck_assert_int_eq(pen->stats.food, 1);
    ck_assert_uint_eq(bottle->nrof, 2);
    ck_assert_int_eq(bottle->stats.food, 1000);
    CLEAR_FLAG(pen, FLAG_UNPAID);
    SET_FLAG(bottle, FLAG_UNPAID);
    OBJECT_METHODS(LIGHT_REFILL)->apply_func(bottle, writer, 0);
    ck_assert_int_eq(pen->stats.food, 1);
    ck_assert_uint_eq(bottle->nrof, 2);
    CLEAR_FLAG(bottle, FLAG_UNPAID);
    SET_FLAG(pen, FLAG_IS_INVISIBLE);
    player_apply(writer, bottle, 0, 0);
    ck_assert_int_eq(pen->stats.food, 1);
    ck_assert_uint_eq(bottle->nrof, 2);
    CLEAR_FLAG(pen, FLAG_IS_INVISIBLE);
    player_apply(writer, bottle, 0, 0);
    ck_assert_int_eq(pen->stats.food, 1000);
    ck_assert_uint_eq(bottle->nrof, 1);
    ck_assert_int_eq(bottle->stats.food, 1000);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Draft", "xyz");
    reply_id(writer, BOOK_EDIT_UPDATED);
    ck_assert_int_eq(pen->stats.food, 997);
    book->nrof = 2;
    submit(writer, BOOK_EDIT_SIGN, id, book, NULL, "Draft", "xyz");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(!book_edit_finalized(book));
    book->nrof = 1;
    pen->nrof = 2;
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Draft", "xyzz");
    reply_id(writer, BOOK_EDIT_ERROR);
    player_apply(writer, bottle, 0, 0);
    ck_assert_int_eq(pen->stats.food, 997);
    ck_assert_int_eq(bottle->stats.food, 1000);
    pen->nrof = 1;
    /* Restore a second bottle through normal insertion so weight stays valid. */
    object_insert_into(object_clone(bottle), writer, 0);
    ck_assert_uint_eq(bottle->nrof, 2);
    player_apply(writer, bottle, 0, 0);
    ck_assert_int_eq(pen->stats.food, 1000);
    ck_assert_uint_eq(bottle->nrof, 1);
    ck_assert_int_eq(bottle->stats.food, 1000);
    object *partial = NULL;
    for (object *item = writer->inv; item != NULL; item = item->below) {
        if (item != bottle && item->type == LIGHT_REFILL && item->stats.food == 997) {
            partial = item;
        }
    }
    ck_assert_ptr_nonnull(partial);
    ck_assert_uint_eq(partial->nrof, 1);
    submit(writer, BOOK_EDIT_CANCEL, id, book, NULL, "", "");
    ck_assert_ptr_null(CONTR(writer)->book_editor);
    ck_assert_str_eq(book->msg, "xyz");
    ck_assert_int_eq(pen->stats.food, 1000);
}
END_TEST

START_TEST(test_packets_reject_partial_and_replayed_edits) {
    object *writer, *pen, *book;
    setup_writer(&writer, &pen, &book);
    ck_assert(book_edit_open(pen, writer));
    uint32_t id = reply_id(writer, BOOK_EDIT_OPEN);
    packet_struct *packet = request(BOOK_EDIT_SAVE, id, book, NULL, "New title", "new body");
    for (size_t len = 0; len < packet->len; len++) {
        socket_command_book_edit(CONTR(writer)->cs, CONTR(writer), packet->data, len, 0);
        reply_id(writer, BOOK_EDIT_ERROR);
        ck_assert_str_eq(book->msg, "abc");
        ck_assert_int_eq(pen->stats.food, 1000);
    }
    packet_writer_write_uint8(packet, 0);
    socket_command_book_edit(CONTR(writer)->cs, CONTR(writer), packet->data, packet->len, 0);
    reply_id(writer, BOOK_EDIT_ERROR);
    packet_free(packet);
    submit(writer, 255, id, book, NULL, "", "");
    reply_id(writer, BOOK_EDIT_ERROR);
    submit(writer, BOOK_EDIT_SAVE, id, book, book, "Bad", "new");
    reply_id(writer, BOOK_EDIT_ERROR);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Bad", "\xc0\xaf");
    reply_id(writer, BOOK_EDIT_ERROR);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Bad", "endmsg\nname bad");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert(book_edit_open(pen, writer));
    uint32_t new_id = reply_id(writer, BOOK_EDIT_OPEN);
    ck_assert_uint_ne(new_id, id);
    submit(writer, BOOK_EDIT_SAVE, id, book, NULL, "Bad", "new");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->msg, "abc");
    ck_assert_int_eq(pen->stats.food, 1000);
    SET_FLAG(writer, FLAG_BLIND);
    submit(writer, BOOK_EDIT_SAVE, new_id, book, NULL, "Bad", "new");
    reply_id(writer, BOOK_EDIT_ERROR);
    ck_assert_str_eq(book->msg, "abc");
    book_edit_clear(CONTR(writer));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("book_edit");
    TCase *tc = tcase_create("Core");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    tcase_add_test(tc, test_cost_and_text_bounds);
    tcase_add_test(tc, test_edit_copy_sign_and_persistence);
    tcase_add_test(tc, test_visibility_payment_and_bounded_mark);
    tcase_add_test(tc, test_copy_source_and_destination_revalidation);
    tcase_add_test(tc, test_all_book_producers_are_terminated);
    tcase_add_test(tc, test_stale_custody_and_content);
    tcase_add_test(tc, test_refill_failed_commit_and_stacks);
    tcase_add_test(tc, test_packets_reject_partial_and_replayed_edits);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_book_edit(void) {
    check_run_suite(suite(), __FILE__);
}
