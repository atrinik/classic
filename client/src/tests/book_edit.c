/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <book_edit.h>
#include <text_input.h>
#include <keybind.h>
#include <settings.h>
#include <toolkit/packet.h>
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
int main(void) { test_packet(); test_multiline(); return 0; }
