/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Narrow fake host for exercising the production adapter and real shared ABI. */
#include <capture_privacy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define arraysize(x) (sizeof(x) / sizeof((x)[0]))
#define ST_PLAY 1
#define LABEL_ID 0
#define MENU_ID 1
#define INPUT_ID 2
#define WIDGET_TEXTURE_TYPE_NONE 0
#define FONT_ARIAL11 NULL
#define COLOR_WHITE "white"
typedef struct widgetdata {
    struct widgetdata *env;
    int x, y, w, h, type, save, required, moveable, resizeable, show, redraw, hidden, texture_type;
    char *name, *id;
    void (*draw_func)(struct widgetdata *);
    void (*deinit_func)(struct widgetdata *);
    void *menu_handle_func;
} widgetdata;
static widgetdata *cur_widget[3];
static struct {
    int pressed;
} keys[SDL_SCANCODE_COUNT];
static struct {
    int state;
} cpl = {ST_PLAY};
static SDL_Window *ScreenWindow;
static unsigned int cancelled, drawn;
static bool drawing_ok = true, popup_open, classic_input;
static SDL_WindowFlags test_window_flags = SDL_WINDOW_INPUT_FOCUS;
static SDL_WindowFlags test_get_window_flags(SDL_Window *window) {
    (void)window;
    return test_window_flags;
}
#define SDL_GetWindowFlags test_get_window_flags
static void keybind_movement_focus_lost(void) {
    ++cancelled;
}
static void keybind_movement_flush(void) {}
static void event_dragging_stop(void) {}
static void widgets_cancel_pointer_gestures(void) {}
static bool widgets_text_input_active(void) {
    return classic_input;
}
static void *popup_get_head(void) {
    return popup_open ? &popup_open : NULL;
}
static int widget_x(widgetdata *w) {
    return w->x;
}
static int widget_y(widgetdata *w) {
    return w->y;
}
static int widget_w(widgetdata *w) {
    return w->w;
}
static int widget_h(widgetdata *w) {
    return w->h;
}
static widgetdata *create_widget(int type) {
    widgetdata *w = calloc(1, sizeof(*w));
    cur_widget[type] = w;
    return w;
}
static widgetdata *get_widget_owner(int x, int y, void *start, void *end) {
    (void)start;
    (void)end;
    widgetdata *w = cur_widget[LABEL_ID];
    return w && w->show && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h ? w : NULL;
}
static char *xstrdup(const char *text) {
    char *copy = malloc(strlen(text) + 1);
    return strcpy(copy, text);
}
static void text_show_format_root(void *font,
                                  int x,
                                  int y,
                                  const char *color,
                                  int flags,
                                  void *box,
                                  const char *format,
                                  ...) {
    (void)font;
    (void)x;
    (void)y;
    (void)color;
    (void)flags;
    (void)box;
    (void)format;
}
static void gpu_renderer_surface_changed(SDL_Surface *surface) {
    (void)surface;
}
static void gpu_renderer_invalidate_surface(SDL_Surface *surface) {
    (void)surface;
}
static bool
gpu_renderer_draw_surface(SDL_Surface *surface, const SDL_Rect *source, const SDL_FRect *dest) {
    (void)source;
    (void)dest;
    SDL_BlendMode mode = SDL_BLENDMODE_NONE;
    if (!SDL_GetSurfaceBlendMode(surface, &mode) || mode != SDL_BLENDMODE_BLEND_PREMULTIPLIED)
        abort();
    ++drawn;
    return drawing_ok;
}
#define EVENT_IS_MOUSE(e) \
    ((e)->type >= SDL_EVENT_MOUSE_MOTION && (e)->type <= SDL_EVENT_MOUSE_WHEEL)
static int event_mouse_x(SDL_Event *e) {
    return (int)(e->type == SDL_EVENT_MOUSE_WHEEL    ? e->wheel.mouse_x
                 : e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x
                                                     : e->button.x);
}
static int event_mouse_y(SDL_Event *e) {
    return (int)(e->type == SDL_EVENT_MOUSE_WHEEL    ? e->wheel.mouse_y
                 : e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y
                                                     : e->button.y);
}
