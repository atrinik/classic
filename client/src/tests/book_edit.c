/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <book_edit.h>
#include <text_input.h>
#include <keybind.h>
#include <settings.h>
#include <toolkit/packet.h>
#include <toolkit/toolkit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "book editor check failed: %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

int keybind_command_matches_event(const char *cmd, SDL_KeyboardEvent *event) {
    return strcmp(cmd, "?PASTE") == 0 && event->key == SDLK_V &&
           (event->mod & SDL_KMOD_CTRL);
}
int64_t setting_get_int(int cat, int setting) { (void)cat; (void)setting; return 20; }
int glyph_get_width(font_struct *font, char c) { (void)font; (void)c; return 1; }
int glyph_get_utf8_width(font_struct *font, const char *text) { (void)font; (void)text; return 1; }

static packet_struct *snapshot(uint8_t result, uint16_t count, const char *body) {
    packet_struct *p = packet_new(0, 128, 128);
    packet_writer_write_uint8(p, result);
    packet_writer_write_uint32(p, 17);
    packet_writer_write_uint32(p, count ? 41 : 0);
    packet_writer_write_uint32(p, 100);
    packet_writer_write_uint32(p, 500);
    packet_writer_write_uint16(p, count);
    for (uint16_t i = 0; i < count; i++) {
        packet_writer_write_uint32(p, 41 + i);
        packet_writer_write_uint8(p, i % 2);
        packet_writer_write_cstring(p, "An inventory book");
    }
    packet_writer_write_cstring(p, count ? "Title" : "");
    packet_writer_write_cstring(p, count ? body : "");
    packet_writer_write_cstring(p, "Insufficient ink: draft retained.");
    return p;
}

static void test_packet(void) {
    book_edit_snapshot_t output = {0};
    packet_struct *p = snapshot(BOOK_EDIT_OPEN, 2, "First line\nSecond line: \xc3\xa9");
    CHECK(book_edit_parse(&output, p->data, p->len, 0));
    CHECK(output.selected == 41 && output.count == 2 && output.books[1].finalized);
    CHECK(strchr(output.contents, '\n') != NULL);
    book_edit_snapshot_t before = output;
    for (size_t cut = 0; cut < p->len; cut++) {
        CHECK(!book_edit_parse(&output, p->data, cut, 0));
        CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    }
    packet_writer_write_uint8(p, 0);
    CHECK(!book_edit_parse(&output, p->data, p->len, 0));
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    packet_free(p);
    p = snapshot(BOOK_EDIT_ERROR, 2, "server text");
    CHECK(book_edit_parse(&output, p->data, p->len, 0));
    CHECK(output.result == BOOK_EDIT_ERROR);
    packet_free(p);
    p = snapshot(BOOK_EDIT_OPEN, BOOK_EDIT_BOOKS_MAX, "");
    CHECK(book_edit_parse(&output, p->data, p->len, 0));
    packet_free(p);
    p = snapshot(BOOK_EDIT_OPEN, BOOK_EDIT_BOOKS_MAX + 1, "");
    CHECK(!book_edit_parse(&output, p->data, p->len, 0));
    packet_free(p);
    char body[BOOK_EDIT_CONTENT_MAX + 2];
    memset(body, 'a', sizeof(body)); body[BOOK_EDIT_CONTENT_MAX] = '\0';
    p = snapshot(BOOK_EDIT_UPDATED, 1, body);
    CHECK(book_edit_parse(&output, p->data, p->len, 0));
    packet_free(p);
    body[BOOK_EDIT_CONTENT_MAX] = 'a'; body[BOOK_EDIT_CONTENT_MAX + 1] = '\0';
    p = snapshot(BOOK_EDIT_UPDATED, 1, body);
    CHECK(!book_edit_parse(&output, p->data, p->len, 0));
    packet_free(p);
    const char *bad[] = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        p = snapshot(BOOK_EDIT_OPEN, 1, bad[i]);
        CHECK(!book_edit_parse(&output, p->data, p->len, 0));
        packet_free(p);
    }
}

static void key(text_input_struct *input, SDL_Keycode code) {
    SDL_Event event = {0}; event.type = SDL_EVENT_KEY_DOWN; event.key.key = code;
    CHECK(text_input_event(input, &event));
}

static void test_multiline(void) {
    font_struct font = {.height = 12};
    text_input_struct input = {.focus = 1, .multiline = 1, .max = BOOK_EDIT_CONTENT_MAX,
                               .font = &font, .coords = {.w = 80, .h = 48}};
    text_input_set(&input, "ab\n\xc3\xa9z");
    key(&input, SDLK_HOME); CHECK(input.pos == 3);
    key(&input, SDLK_UP); CHECK(input.pos == 0);
    key(&input, SDLK_END); CHECK(input.pos == 2);
    key(&input, SDLK_DOWN); CHECK(input.pos == input.num);
    key(&input, SDLK_BACKSPACE); CHECK(strcmp(input.str, "ab\n\xc3\xa9") == 0);
    key(&input, SDLK_BACKSPACE); CHECK(strcmp(input.str, "ab\n") == 0);
    key(&input, SDLK_RETURN); CHECK(strcmp(input.str, "ab\n\n") == 0);
    SDL_Event text = {.type = SDL_EVENT_TEXT_INPUT}; text.text.text = "\xc3\xa9";
    CHECK(text_input_event(&input, &text));
    CHECK(input.num == 6);
    key(&input, SDLK_LEFT); CHECK(input.pos == 4);
    key(&input, SDLK_DELETE); CHECK(input.num == 4);
    input.max = 5;
    CHECK(!text_input_event(&input, &text)); CHECK(input.num == 4);
    /* Soft wrapped navigation preserves visual column on UTF-8 boundaries. */
    input.max = BOOK_EDIT_CONTENT_MAX; input.coords.w = 8;
    text_input_set(&input, "abcdefgh");
    key(&input, SDLK_UP); CHECK(input.pos == 5);
    key(&input, SDLK_UP); CHECK(input.pos == 2);
    key(&input, SDLK_DOWN); CHECK(input.pos == 5);
    /* Default fields continue to treat Return as submission/history. */
    input.multiline = 0; text_input_set(&input, "title");
    key(&input, SDLK_RETURN); CHECK(strcmp(input.str, "title") == 0);
}
static book_edit_snapshot_t opened(uint32_t session, uint32_t selected, const char *body) {
    book_edit_snapshot_t next = {.result = BOOK_EDIT_OPEN, .session = session,
        .selected = selected, .ink = 100, .capacity = 500, .count = 2};
    next.books[0] = (book_edit_entry_t){.tag = 41};
    next.books[1] = (book_edit_entry_t){.tag = 42, .finalized = true};
    strcpy(next.title, "Saved title");
    strcpy(next.contents, body);
    return next;
}

/* Exercise the same state and request gates used by the production popup. */
static void test_draft_lifecycle(void) {
    book_edit_model_t model = {0};
    book_edit_snapshot_t next = opened(17, 41, "Saved text");
    CHECK(book_edit_model_receive(&model, &next));
    strcpy(model.title, "My draft title");
    strcpy(model.contents, "Saved text plus unsaved text");
    CHECK(book_edit_model_dirty(&model));
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 41));
    model.pending = true;
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 41));
    next.result = BOOK_EDIT_ERROR; next.ink = 0;
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(!model.pending && model.current.ink == 0);
    CHECK(strcmp(model.title, "My draft title") == 0);
    CHECK(strcmp(model.contents, "Saved text plus unsaved text") == 0);
    /* X closes the modal for inventory/refill while invalidating old replies. */
    book_edit_model_close(&model);
    CHECK(model.current.session == 0 && book_edit_model_dirty(&model));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 41));
    next.result = BOOK_EDIT_UPDATED;
    CHECK(!book_edit_model_receive(&model, &next));
    /* Apply the pen after refill: fresh session/ink, same retained draft. */
    next = opened(18, 41, "Saved text"); next.ink = 500;
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(model.current.session == 18 && model.current.ink == 500);
    CHECK(model.confirmation == BOOK_CONFIRM_NONE && book_edit_model_dirty(&model));
    CHECK(strcmp(model.title, "My draft title") == 0);
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 41));
    /* A new destination cannot replace a retained draft without approval. */
    book_edit_model_close(&model);
    next = opened(19, 42, "Other book"); next.books[1].finalized = false;
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(model.confirmation == BOOK_CONFIRM_OPEN_DISCARD && model.destination == 41);
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 42));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 41));
    book_edit_model_resolve_open(&model, false);
    CHECK(model.destination == 41 && book_edit_model_dirty(&model));
    CHECK(model.current.session == 0);
    CHECK(book_edit_model_receive(&model, &next));
    book_edit_model_resolve_open(&model, true);
    CHECK(model.destination == 42 && model.current.session == 19);
    CHECK(strcmp(model.contents, "Other book") == 0 && !book_edit_model_dirty(&model));
    /* Same book changed externally: no silent lost-update rebase. */
    strcpy(model.contents, "My unsaved replacement");
    book_edit_model_close(&model);
    next = opened(20, 42, "New external text"); next.books[1].finalized = false;
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(model.confirmation == BOOK_CONFIRM_OPEN_REBASE);
    CHECK(strcmp(model.base_contents, "Other book") == 0);
    book_edit_model_resolve_open(&model, false);
    CHECK(strcmp(model.contents, "My unsaved replacement") == 0);
    CHECK(strcmp(model.base_contents, "Other book") == 0);
    CHECK(book_edit_model_receive(&model, &next));
    book_edit_model_resolve_open(&model, true);
    CHECK(model.current.session == 20 && book_edit_model_dirty(&model));
    CHECK(strcmp(model.base_contents, "New external text") == 0);
    CHECK(strcmp(model.contents, "My unsaved replacement") == 0);
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_SAVE, 42));
    /* Cancellation explicitly discards, and reused tags on a new connection
     * cannot resurrect a draft under a different character's inventory. */
    book_edit_model_cancel(&model);
    CHECK(!model.destination && !model.current.session && !model.contents[0]);
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(strcmp(model.contents, "New external text") == 0);
}

static void test_confirmation_gates(void) {
    book_edit_model_t model = {0};
    book_edit_snapshot_t next = opened(17, 41, "Saved text");
    CHECK(book_edit_model_receive(&model, &next));
    model.source = 42; /* A signed source is legal; its destination is not. */
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    strcpy(model.contents, "Retain my draft until copy confirmation");
    CHECK(book_edit_model_confirm(&model, BOOK_EDIT_COPY, 41));
    CHECK(model.confirmation == BOOK_CONFIRM_COPY);
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    model.current.books[2] = (book_edit_entry_t){.tag = 43};
    model.current.count = 3;
    model.source = 43; /* Queued source Next must not change the approved copy. */
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    model.source = 42;
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SIGN, 41));
    model.confirmation = BOOK_CONFIRM_NONE; /* Back/Escape preserve draft. */
    CHECK(book_edit_model_dirty(&model));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    CHECK(!book_edit_model_confirm(&model, BOOK_EDIT_SIGN, 41));
    next.result = BOOK_EDIT_UPDATED;
    strcpy(next.contents, model.contents);
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(!book_edit_model_dirty(&model));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SIGN, 41));
    CHECK(book_edit_model_confirm(&model, BOOK_EDIT_SIGN, 41));
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_SIGN, 41));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_COPY, 41));
    model.confirmation = BOOK_CONFIRM_NONE;
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SIGN, 41));
    CHECK(!book_edit_model_confirm(&model, BOOK_EDIT_COPY, 42));
    model.current.books[1].finalized = false;
    strcpy(model.title, "Unsaved title");
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SELECT, 42));
    CHECK(book_edit_model_confirm(&model, BOOK_EDIT_SELECT, 42));
    CHECK(book_edit_model_can_submit(&model, BOOK_EDIT_SELECT, 42));
    CHECK(!book_edit_model_can_submit(&model, BOOK_EDIT_SELECT, 41));
    next.result = BOOK_EDIT_ERROR;
    CHECK(book_edit_model_receive(&model, &next));
    CHECK(book_edit_model_dirty(&model) && strcmp(model.title, "Unsaved title") == 0);
}
int main(void) {
    toolkit_import(packet);
    test_packet(); test_multiline(); test_draft_lifecycle(); test_confirmation_gates();
    toolkit_deinit();
    return 0;
}
