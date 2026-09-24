/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Headless tests of the actual notification consumer. Only presentation is stubbed. */
#include "../gui/widgets/notification.c"
#include <stdio.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

widgetdata *cur_widget[TOTAL_SUBWIDGETS];
uint32_t client_ui_ticks(void) {
    return 0;
}
void SetPriorityWidget(widgetdata *widget) {
    (void)widget;
}
void resize_widget(widgetdata *widget, int side, int offset) {
    (void)widget;
    (void)side;
    (void)offset;
}
keybind_struct *keybind_find_by_command(const char *command) {
    (void)command;
    return NULL;
}
char *keybind_get_key_shortcut(SDL_Keycode key, SDL_Keymod mod, char *buf, size_t len) {
    (void)key;
    (void)mod;
    (void)len;
    buf[0] = '\0';
    return buf;
}
font_struct *font_get_weak(const char *name, uint8_t size) {
    (void)name;
    (void)size;
    return NULL;
}
void text_show(SDL_Surface *surface,
               font_struct *font,
               const char *text,
               int x,
               int y,
               const char *color,
               uint64_t flags,
               SDL_Rect *box) {
    (void)surface;
    (void)font;
    (void)text;
    (void)x;
    (void)y;
    (void)color;
    (void)flags;
    box->w = 20;
    box->h = 10;
}
int text_color_parse(const char *text, SDL_Color *color) {
    (void)text;
    *color = (SDL_Color){0};
    return 1;
}
uint32_t get_video_flags(void) {
    return 0;
}
int video_get_bpp(void) {
    return 32;
}
SDL_Surface *
surface_create_rgb(Uint32 flags, int w, int h, int depth, Uint32 r, Uint32 g, Uint32 b, Uint32 a) {
    (void)flags;
    (void)depth;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    return SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
}
bool gpu_renderer_canvas_register(SDL_Surface **surface) {
    return *surface != NULL;
}
bool surface_fill_rect(SDL_Surface *surface, const SDL_Rect *box, Uint32 color) {
    return SDL_FillSurfaceRect(surface, box, color);
}
Uint32 surface_map_rgb(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b) {
    (void)surface;
    (void)r;
    (void)g;
    (void)b;
    return 0;
}
void border_create_color(SDL_Surface *surface, SDL_Rect *box, int width, const char *color) {
    (void)surface;
    (void)box;
    (void)width;
    (void)color;
}

static packet_error_t consume(uint8_t *data, size_t length) {
    packet_reader_scope_t scope;
    packet_reader_scope_begin(&scope);
    socket_command_notification(data, length, 0);
    return packet_reader_scope_finish(&scope);
}

static void test_delay(uint32_t delay, bool following) {
    packet_struct *packet = packet_new(0, 32, 32);
    packet_writer_write_uint8(packet, CMD_NOTIFICATION_TEXT);
    packet_writer_write_cstring(packet, "A sign");
    packet_writer_write_uint8(packet, CMD_NOTIFICATION_DELAY);
    packet_writer_write_uint32(packet, delay);
    if (following) {
        packet_writer_write_uint8(packet, CMD_NOTIFICATION_ACTION);
        packet_writer_write_cstring(packet, "help");
    }
    TEST_CHECK(consume(packet->data, packet->len) == PACKET_ERROR_NONE);
    TEST_CHECK(notification->delay == (delay < 5000 ? 5000 : delay));
    TEST_CHECK(strcmp(notification->message, following ? "A sign (click)" : "A sign") == 0);
    TEST_CHECK(!following || strcmp(notification->action, "help") == 0);
    packet_free(packet);
}

int main(int argc, char **argv) {
    widgetdata widget = {0};
    cur_widget[NOTIFICATION_ID] = &widget;
    toolkit_import(packet);
    toolkit_import(string);
    toolkit_import(stringbuffer);
    uint8_t sign_packet[] = {0, 'A', ' ', 's', 'i', 'g', 'n', 0, 3, 0, 1, 0xd4, 0xc0};
    TEST_CHECK(consume(sign_packet, sizeof(sign_packet)) == PACKET_ERROR_NONE);
    TEST_CHECK(notification->delay == 120000);
    if (argc == 2) {
        uint8_t actual[sizeof(sign_packet) + 1];
        FILE *fp = fopen(argv[1], "rb");
        TEST_CHECK(fp != NULL);
        size_t length = fread(actual, 1, sizeof(actual), fp);
        TEST_CHECK(fclose(fp) == 0);
        TEST_CHECK(length == sizeof(sign_packet));
        TEST_CHECK(memcmp(actual, sign_packet, length) == 0);
        TEST_CHECK(consume(actual, length) == PACKET_ERROR_NONE);
        TEST_CHECK(notification->delay == 120000);
    }
    const uint32_t delays[] = {0, 4999, 5000, 5001, 120000, UINT32_MAX};
    for (size_t i = 0; i < arraysize(delays); i++) {
        test_delay(delays[i], false);
        test_delay(delays[i], true);
    }
    for (size_t length = 9; length < sizeof(sign_packet); length++) {
        TEST_CHECK(consume(sign_packet, length) == PACKET_ERROR_TRUNCATED);
    }
    uint8_t missing_nul[] = {CMD_NOTIFICATION_TEXT, 'x'};
    TEST_CHECK(consume(missing_nul, sizeof(missing_nul)) == PACKET_ERROR_TRUNCATED);
    uint8_t unknown[] = {255};
    TEST_CHECK(consume(unknown, sizeof(unknown)) == PACKET_ERROR_UNSUPPORTED);
    notification_destroy();
    SDL_DestroySurface(widget.surface);
    toolkit_deinit();
    return 0;
}
