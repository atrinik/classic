/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* One private local-echo widget. The host owns every native/GPU lifetime. */
#include <shared_chat_pilot.h>
#include <tilewright_ui.h>
#ifdef ATRINIK_SHARED_CHAT_PILOT_TEST
#include "../tests/shared_chat_host.h"
#else
#include <capture_privacy.h>
#include <client.h>
#include <event.h>
#include <gpu_renderer.h>
#include <keybind.h>
#include <main.h>
#include <player.h>
#include <popup.h>
#include <toolkit/memory.h>
#include <video.h>
#include <widget.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "shared_chat_fonts.h"

#define PILOT_BYTES 4096U
#define PILOT_WIDTH 480
#define PILOT_HEIGHT 240
#define PILOT_TITLE 22
#define PILOT_BATCH 256U

static struct {
    widgetdata *widget;
    twui_context *ui;
    SDL_Surface *pixels;
    twui_stamp prepared;
    uint64_t epoch, session_start, accepted;
    uint32_t error_status;
    uint32_t pressed[SDL_SCANCODE_COUNT];
    bool owned[SDL_SCANCODE_COUNT];
    bool failed, pending, text_owner, blocked, batch_ime, batch_composition, native_composing,
        attachment, bounds_valid;
    int x, y, w, h;
} pilot;

/* Retired presses belong to the host event stream, not a particular widget.
 * Keep them quarantined across /resetwidgets and periods with no pilot. */
static bool retired_keys[SDL_SCANCODE_COUNT];

static void cancel_world(void) {
    for (size_t i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        if (keys[i].pressed)
            pilot.owned[i] = true;
    }
    memset(keys, 0, sizeof(keys));
    keybind_movement_focus_lost();
    keybind_movement_flush();
    event_dragging_stop();
    widgets_cancel_pointer_gestures();
}

static void fail_closed(void) {
    pilot.failed = true;
    cancel_world();
    if (pilot.widget != NULL)
        pilot.widget->redraw = 2;
    /* No text or transcript enters diagnostics. */
}

static bool state(twui_state *out) {
    *out = (twui_state){.struct_size = sizeof(*out)};
    if (pilot.ui != NULL && twui_get_state(pilot.ui, out) == TWUI_OK)
        return true;
    fail_closed();
    return false;
}

static void native_barrier(void) {
    pilot.session_start = SDL_GetTicksNS();
    pilot.native_composing = false;
    if (!SDL_StopTextInput(ScreenWindow) || !SDL_StartTextInput(ScreenWindow))
        fail_closed();
}

static void control(void) {
    twui_control value = {.struct_size = sizeof(value)};
    if (twui_get_control(pilot.ui, &value) != TWUI_OK) {
        fail_closed();
        return;
    }
    if (value.epoch != pilot.epoch) {
        pilot.epoch = value.epoch;
        cancel_world();
        /* Retire native composition even when the final owner is unchanged. */
        native_barrier();
    }
    twui_state current;
    if (state(&current))
        pilot.text_owner = current.focused && !current.suspended && !current.modal_open;
}

static twui_status apply(twui_input input, uint32_t *consumed) {
    input.struct_size = sizeof(input);
    uint32_t ignored = 1;
    twui_status result = twui_apply(pilot.ui, &input, consumed != NULL ? consumed : &ignored);
    control();
    pilot.error_status = result;
    if (pilot.widget != NULL)
        pilot.widget->redraw = 1;
    /* Ordinary validation/backpressure errors preserve state and consume input. */
    if (result >= TWUI_WRONG_THREAD)
        fail_closed();
    return result;
}

static twui_status
text(uint32_t kind, const char *bytes, size_t size, uint64_t start, uint64_t length) {
    twui_input input = {.kind = kind,
                        .text = (const uint8_t *)bytes,
                        .text_len = size,
                        .anchor = start,
                        .caret = length};
    if (kind != TWUI_RECEIVE) {
        twui_state current;
        if (!state(&current))
            return TWUI_POISONED;
        input.widget_generation = current.widget_generation;
        input.control_epoch = current.control_epoch;
        input.text_revision = current.text_revision;
    }
    return apply(input, NULL);
}

bool shared_chat_pilot_text_owner(void) {
    return pilot.widget != NULL && pilot.text_owner;
}

void shared_chat_pilot_invalidate(void) {
    if (pilot.ui == NULL || pilot.failed)
        return;
    pilot.pending = false;
    if (twui_invalidate_surface(pilot.ui) != TWUI_OK)
        fail_closed();
    control();
    pilot.widget->redraw = 1;
}

static bool visible(void) {
    return pilot.widget != NULL && pilot.widget->env == NULL && pilot.widget->show &&
           !pilot.widget->hidden && cpl.state == ST_PLAY;
}

void shared_chat_pilot_sync(void) {
    if (pilot.widget == NULL)
        return;
    int width = 0, height = 0;
    bool size_ok = SDL_GetWindowSize(ScreenWindow, &width, &height);
    int x = widget_x(pilot.widget), y = widget_y(pilot.widget);
    int w = widget_w(pilot.widget), h = widget_h(pilot.widget);
    bool valid = size_ok && w == PILOT_WIDTH && h == PILOT_HEIGHT && x >= 0 && y >= 0 &&
                 x <= width - w && y <= height - h;
    pilot.bounds_valid = valid;
    if (!pilot.attachment || x != pilot.x || y != pilot.y || w != pilot.w || h != pilot.h) {
        shared_chat_pilot_invalidate();
        pilot.x = x;
        pilot.y = y;
        pilot.w = w;
        pilot.h = h;
        pilot.attachment = true;
    }
    bool blocked = !visible() || !valid || popup_get_head() != NULL ||
                   cur_widget[MENU_ID] != NULL || widgets_text_input_active() ||
                   !(SDL_GetWindowFlags(ScreenWindow) & SDL_WINDOW_INPUT_FOCUS);
    if (blocked != pilot.blocked) {
        pilot.blocked = blocked;
        if (pilot.failed) {
            /* Failed contexts still yield to native modal/text ownership. */
            cancel_world();
            native_barrier();
            if (blocked)
                pilot.text_owner = false;
        } else {
            (void)apply((twui_input){.kind = TWUI_SET_SUSPENDED, .value = blocked}, NULL);
            shared_chat_pilot_invalidate();
        }
    }
}

static void accept_actions(void) {
    for (unsigned int count = 0; count < 8 && !pilot.failed; ++count) {
        twui_action action = {.struct_size = sizeof(action)};
        twui_status status = twui_peek_action(pilot.ui, &action);
        if (status == TWUI_EMPTY)
            return;
        if (status != TWUI_OK || action.kind != TWUI_ACTION_SUBMIT_CHAT) {
            fail_closed();
            return;
        }
        if (action.batch_id != pilot.accepted) {
            uint8_t bytes[PILOT_BYTES];
            uint64_t size = 0;
            if (twui_copy_action(pilot.ui, action.batch_id, bytes, sizeof(bytes), &size) !=
                    TWUI_OK ||
                text(TWUI_RECEIVE, (const char *)bytes, (size_t)size, 0, 0) != TWUI_OK) {
                fail_closed();
                return;
            }
            pilot.accepted = action.batch_id;
        }
        if (twui_ack_action(pilot.ui, action.batch_id) != TWUI_OK) {
            fail_closed();
            return;
        }
    }
}

static void draw(widgetdata *widget) {
    if (!visible())
        return;
    text_show_format_root(FONT_ARIAL11,
                          widget_x(widget),
                          widget_y(widget),
                          COLOR_WHITE,
                          0,
                          NULL,
                          "%s",
                          pilot.failed         ? "Local echo unavailable"
                          : pilot.error_status ? "Local echo - input rejected"
                                               : "Local echo only - Escape releases focus");
    if (pilot.failed || !pilot.attachment || !pilot.bounds_valid)
        return;
    accept_actions();
    if (pilot.failed)
        return;
    if (pilot.pending) {
        if (twui_discard_prepared(pilot.ui, &pilot.prepared) != TWUI_OK) {
            fail_closed();
            return;
        }
        pilot.pending = false;
    }
    twui_layout layout = {.struct_size = sizeof(layout),
                          .viewport_width = PILOT_WIDTH,
                          .viewport_height = PILOT_HEIGHT - PILOT_TITLE,
                          .viewport_scale = 1,
                          .bounds_width = PILOT_WIDTH,
                          .bounds_height = PILOT_HEIGHT - PILOT_TITLE,
                          .clip_width = PILOT_WIDTH,
                          .clip_height = PILOT_HEIGHT - PILOT_TITLE};
    twui_image image = {.struct_size = sizeof(image)};
    if (twui_set_layout(pilot.ui, &layout) != TWUI_OK ||
        twui_render_prepare_current(pilot.ui, &image) != TWUI_OK) {
        fail_closed();
        return;
    }
    control();
    pilot.prepared = image.stamp;
    pilot.pending = true;
    if (image.width != PILOT_WIDTH || image.height != PILOT_HEIGHT - PILOT_TITLE ||
        image.stride != PILOT_WIDTH * 4 ||
        image.byte_length != PILOT_WIDTH * (PILOT_HEIGHT - PILOT_TITLE) * 4) {
        fail_closed();
        return;
    }
    if (pilot.pixels == NULL) {
        pilot.pixels =
            SDL_CreateSurface(PILOT_WIDTH, PILOT_HEIGHT - PILOT_TITLE, SDL_PIXELFORMAT_RGBA32);
        if (pilot.pixels == NULL ||
            !SDL_SetSurfaceBlendMode(pilot.pixels, SDL_BLENDMODE_BLEND_PREMULTIPLIED)) {
            fail_closed();
            return;
        }
    }
    uint64_t required = 0;
    if (pilot.pixels->pitch != (int)image.stride ||
        twui_copy_rgba(pilot.ui,
                       &image.stamp,
                       pilot.pixels->pixels,
                       image.byte_length,
                       &required) != TWUI_OK ||
        required != image.byte_length) {
        fail_closed();
        return;
    }
    gpu_renderer_surface_changed(pilot.pixels);
    SDL_FRect destination = {(float)pilot.x,
                             (float)(pilot.y + PILOT_TITLE),
                             PILOT_WIDTH,
                             PILOT_HEIGHT - PILOT_TITLE};
    if (!gpu_renderer_draw_surface(pilot.pixels, NULL, &destination))
        shared_chat_pilot_invalidate();
}

void shared_chat_pilot_presented(bool presented) {
    if (pilot.ui == NULL || pilot.failed)
        return;
    if (!presented) {
        shared_chat_pilot_invalidate();
        return;
    }
    if (!pilot.pending)
        return;
    pilot.pending = false;
    if (twui_ack_presented(pilot.ui, &pilot.prepared) != TWUI_OK) {
        fail_closed();
        return;
    }
    twui_rect rect = {.struct_size = sizeof(rect)};
    if (pilot.text_owner && twui_get_ime_rect(pilot.ui, &pilot.prepared, &rect) == TWUI_OK) {
        SDL_Rect area = {pilot.x + (int)rect.x,
                         pilot.y + PILOT_TITLE + (int)rect.y,
                         (int)SDL_ceilf(rect.width),
                         (int)SDL_ceilf(rect.height)};
        if (area.w < 1)
            area.w = 1;
        if (area.h < 1)
            area.h = 1;
        if (!SDL_SetTextInputArea(ScreenWindow, &area, 0))
            fail_closed();
    }
}

static void destroy(widgetdata *widget) {
    (void)widget;
    cancel_world();
    if (pilot.ui != NULL)
        (void)twui_destroy(pilot.ui);
    if (pilot.pixels != NULL) {
        gpu_renderer_invalidate_surface(pilot.pixels);
        SDL_DestroySurface(pilot.pixels);
    }
    bool was_owner = pilot.text_owner;
    for (size_t i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        retired_keys[i] = retired_keys[i] || pilot.owned[i];
    }
    memset(&pilot, 0, sizeof(pilot));
    if (was_owner) {
        (void)SDL_StopTextInput(ScreenWindow);
        (void)SDL_StartTextInput(ScreenWindow);
    }
    capture_privacy_shared_chat_retained(false);
}

void shared_chat_pilot_create(void) {
    const char *enabled = getenv("ATRINIK_SHARED_CHAT_PILOT");
    if (pilot.widget != NULL || enabled == NULL || strcmp(enabled, "1") != 0)
        return;
    widgetdata *widget = create_widget(LABEL_ID);
    widget->type = LABEL_ID;
    widget->name = xstrdup("Local echo");
    widget->id = NULL;
    widget->save = widget->required = widget->moveable = widget->resizeable = 0;
    widget->show = widget->redraw = 1;
    widget->hidden = 0;
    widget->w = PILOT_WIDTH;
    widget->h = PILOT_HEIGHT;
    widget->x = 16;
    widget->y = 48;
    widget->texture_type = WIDGET_TEXTURE_TYPE_NONE;
    widget->draw_func = draw;
    widget->deinit_func = destroy;
    widget->menu_handle_func = NULL;
    pilot.widget = widget;
    capture_privacy_shared_chat_retained(true);
    twui_config config =
        {sizeof(config), TWUI_ABI_VERSION, PILOT_BYTES, 32, 32768, PILOT_BYTES, 8, 3};
    if (twui_abi_version() != TWUI_ABI_VERSION ||
        twui_create_with_composition(&config, TWUI_COMPOSITION_CHAT_PANEL, &pilot.ui) != TWUI_OK) {
        fail_closed();
        return;
    }
    twui_font fonts[32] = {{0}};
    uint8_t *allocated[32] = {0};
    bool good = true;
    for (size_t i = 0; i < arraysize(pilot_fonts); ++i) {
        const struct pilot_font_file *file = &pilot_fonts[i];
        FILE *stream = fopen(file->path, "rb");
        allocated[i] = malloc(file->size);
        if (stream == NULL || allocated[i] == NULL)
            good = false;
        else if (fread(allocated[i], 1, file->size, stream) != file->size || fgetc(stream) != EOF)
            good = false;
        if (stream != NULL && fclose(stream) != 0)
            good = false;
        fonts[i] = (twui_font){.struct_size = sizeof(fonts[i]),
                               .family = (const uint8_t *)file->family,
                               .family_bytes = strlen(file->family),
                               .bytes = allocated[i],
                               .byte_length = file->size};
        memcpy(fonts[i].sha256, file->sha256, 32);
        if (!good)
            break;
    }
    if (good)
        good = twui_set_fonts(pilot.ui, fonts, arraysize(pilot_fonts)) == TWUI_OK;
    for (size_t i = 0; i < arraysize(allocated); ++i)
        free(allocated[i]);
    if (!good) {
        fail_closed();
        return;
    }
    const char *notice = "Local echo: messages and /commands stay on this device.";
    (void)text(TWUI_RECEIVE, notice, strlen(notice), 0, 0);
    shared_chat_pilot_sync();
}

static uint32_t key_value(SDL_Keycode key, SDL_Keymod modifiers) {
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
        return TWUI_KEY_ENTER;
    if (key == SDLK_BACKSPACE)
        return TWUI_KEY_BACKSPACE;
    if (key == SDLK_DELETE)
        return TWUI_KEY_DELETE;
    if (key == SDLK_ESCAPE)
        return TWUI_KEY_ESCAPE;
    if (key == SDLK_A && (modifiers & SDL_KMOD_CTRL))
        return TWUI_KEY_SELECT_ALL;
    return TWUI_KEY_OTHER;
}

static void key_effect(const SDL_KeyboardEvent *event) {
    uint32_t allowed = 0;
    if (twui_key_effect_allowed(pilot.ui, event->scancode, &allowed) != TWUI_OK) {
        fail_closed();
        return;
    }
    if (!allowed)
        return;
    if (event->key == SDLK_ESCAPE) {
        (void)apply((twui_input){.kind = TWUI_SET_FOCUS}, NULL);
        return;
    }
    uint32_t motion = event->key == SDLK_LEFT    ? 1
                      : event->key == SDLK_RIGHT ? 2
                      : event->key == SDLK_HOME  ? 3
                      : event->key == SDLK_END   ? 4
                                                 : 0;
    twui_stamp displayed = {.struct_size = sizeof(displayed)};
    if (motion && twui_get_displayed(pilot.ui, &displayed) == TWUI_OK)
        (void)twui_move_visual(pilot.ui,
                               &displayed,
                               event->scancode,
                               motion,
                               !!(event->mod & SDL_KMOD_SHIFT));
    if (event->repeat || !(event->mod & SDL_KMOD_CTRL))
        return;
    twui_state current;
    if (!state(&current) || current.preedit_bytes != 0)
        return;
    if (event->key == SDLK_V) {
        char *bytes = SDL_GetClipboardText();
        if (bytes != NULL) {
            size_t size = strnlen(bytes, PILOT_BYTES + 1);
            if (size <= PILOT_BYTES)
                (void)text(TWUI_COMMIT, bytes, size, 0, 0);
            SDL_free(bytes);
        }
    } else if (event->key == SDLK_C || event->key == SDLK_X) {
        uint8_t bytes[PILOT_BYTES + 1];
        uint64_t size = 0;
        if (twui_copy_text(pilot.ui, TWUI_TEXT_COMPOSER, 0, bytes, PILOT_BYTES, &size) != TWUI_OK)
            return;
        uint64_t first = current.anchor < current.caret ? current.anchor : current.caret;
        uint64_t last = current.anchor > current.caret ? current.anchor : current.caret;
        if (first < last && last <= size) {
            bytes[last] = 0;
            if (SDL_SetClipboardText((const char *)bytes + first) && event->key == SDLK_X)
                (void)text(TWUI_COMMIT, "", 0, 0, 0);
        }
    }
}

bool shared_chat_pilot_event(SDL_Event *event) {
    bool keyboard = event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP;
    if (keyboard && event->key.scancode > SDL_SCANCODE_UNKNOWN &&
        event->key.scancode < SDL_SCANCODE_COUNT && retired_keys[event->key.scancode]) {
        keys[event->key.scancode].pressed = 0;
        if (event->type == SDL_EVENT_KEY_UP) {
            retired_keys[event->key.scancode] = false;
        }
        return true;
    }
    if (pilot.widget == NULL)
        return false;
    shared_chat_pilot_sync();
    switch (event->type) {
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
            shared_chat_pilot_invalidate();
            return false;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (!pilot.failed)
                (void)apply((twui_input){.kind = TWUI_SET_WINDOW_FOCUS,
                                         .value = event->type == SDL_EVENT_WINDOW_FOCUS_GAINED},
                            NULL);
            shared_chat_pilot_invalidate();
            return false;
        default:
            break;
    }
    bool text_event = event->type == SDL_EVENT_TEXT_INPUT || event->type == SDL_EVENT_TEXT_EDITING;
    if (text_event && event->common.timestamp < pilot.session_start)
        return true;
    if (pilot.failed) {
        if (keyboard && event->key.scancode > SDL_SCANCODE_UNKNOWN &&
            event->key.scancode < SDL_SCANCODE_COUNT &&
            (pilot.text_owner || pilot.owned[event->key.scancode])) {
            keys[event->key.scancode].pressed = 0;
            if (event->type == SDL_EVENT_KEY_UP)
                pilot.owned[event->key.scancode] = false;
            return true;
        }
        if (text_event)
            return pilot.text_owner;
        if (EVENT_IS_MOUSE(event))
            return get_widget_owner(event_mouse_x(event), event_mouse_y(event), NULL, NULL) ==
                   pilot.widget;
        return false;
    }
    if (keyboard) {
        SDL_Scancode physical = event->key.scancode;
        if (physical <= SDL_SCANCODE_UNKNOWN || physical >= SDL_SCANCODE_COUNT)
            return pilot.text_owner;
        bool down = event->type == SDL_EVENT_KEY_DOWN;
        bool was_owned = pilot.owned[physical];
        /* New modal/input presses belong to that owner. Retired presses remain quarantined. */
        if (pilot.blocked && !was_owned)
            return false;
        if (down && pilot.pressed[physical] == 0)
            pilot.pressed[physical] = key_value(event->key.key, event->key.mod);
        uint32_t value = pilot.pressed[physical] ? pilot.pressed[physical]
                                                 : key_value(event->key.key, event->key.mod);
        uint32_t consumed = 1;
        twui_status status =
            apply((twui_input){.kind = down ? TWUI_KEY_DOWN : TWUI_KEY_UP,
                               .value = value,
                               .physical_key = physical,
                               .key_repeat = down && event->key.repeat,
                               /* Ordinary text fences Enter only; other keys are
                                * IME-owned only during an actual preedit session. */
                               .platform_composing =
                                   down && (pilot.native_composing || pilot.batch_composition ||
                                            (value == TWUI_KEY_ENTER && pilot.batch_ime))},
                  &consumed);
        bool owned = was_owned || consumed != 0 || status != TWUI_OK;
        pilot.owned[physical] = down && owned;
        if (down && status == TWUI_OK && !pilot.blocked)
            key_effect(&event->key);
        if (!down)
            pilot.pressed[physical] = 0;
        /* Classic records pressed before routing: owned keys never remain held. */
        if (owned)
            keys[physical].pressed = 0;
        return owned;
    }
    if (text_event) {
        if (!pilot.text_owner)
            return false;
        const char *bytes =
            event->type == SDL_EVENT_TEXT_INPUT ? event->text.text : event->edit.text;
        size_t size = strnlen(bytes, PILOT_BYTES + 1);
        pilot.native_composing = event->type == SDL_EVENT_TEXT_EDITING && size != 0;
        if (size <= PILOT_BYTES) {
            if (event->type == SDL_EVENT_TEXT_INPUT)
                (void)text(TWUI_COMMIT, bytes, size, 0, 0);
            else
                (void)text(TWUI_PREEDIT,
                           bytes,
                           size,
                           event->edit.start < 0 ? UINT64_MAX : (uint64_t)event->edit.start,
                           event->edit.length < 0 ? UINT64_MAX : (uint64_t)event->edit.length);
        }
        return true;
    }
    if (pilot.blocked)
        return false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_WHEEL) {
        int x = event_mouse_x(event), y = event_mouse_y(event);
        bool inside = get_widget_owner(x, y, NULL, NULL) == pilot.widget;
        if (!inside) {
            /* Explicit outside click retires focus; the same click cannot fire a world action. */
            if (pilot.text_owner && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                (void)apply((twui_input){.kind = TWUI_SET_FOCUS}, NULL);
                return true;
            }
            return false;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
            twui_stamp stamp = {.struct_size = sizeof(stamp)};
            uint32_t consumed = 1;
            if (twui_get_displayed(pilot.ui, &stamp) == TWUI_OK)
                (void)twui_pointer(pilot.ui,
                                   &stamp,
                                   x - pilot.x,
                                   y - pilot.y - PILOT_TITLE,
                                   &consumed);
            control();
            pilot.widget->redraw = 1;
        } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
            twui_stamp stamp = {.struct_size = sizeof(stamp)};
            uint32_t consumed = 1;
            if (twui_get_displayed(pilot.ui, &stamp) == TWUI_OK)
                (void)twui_scroll_at(pilot.ui,
                                     &stamp,
                                     x - pilot.x,
                                     y - pilot.y - PILOT_TITLE,
                                     event->wheel.y > 0   ? 1
                                     : event->wheel.y < 0 ? -1
                                                          : 0,
                                     &consumed);
            control();
            pilot.widget->redraw = 1;
        }
        return true;
    }
    return false;
}

/* A bounded drain copies SDL-owned text before the next pump. Any text activity
 * fences Enter throughout the batch, including commit-before-key ordering. */
bool shared_chat_pilot_poll(SDL_Event *event) {
    static SDL_Event batch[PILOT_BATCH];
    static char strings[PILOT_BATCH][PILOT_BYTES + 2];
    static unsigned int count, next;
    /* Finish an already copied drain even if its widget was destroyed while
     * dispatching the previous event. In particular, do not lose key releases. */
    if (next < count) {
        *event = batch[next++];
        return true;
    }
    if (pilot.widget == NULL)
        return SDL_PollEvent(event);
    if (next == count) {
        next = count = 0;
        twui_state current;
        pilot.batch_ime = !pilot.failed && state(&current) && current.preedit_bytes != 0;
        pilot.batch_composition = pilot.batch_ime || pilot.native_composing;
        while (count < PILOT_BATCH && SDL_PollEvent(&batch[count])) {
            SDL_Event *item = &batch[count];
            const char *bytes = item->type == SDL_EVENT_TEXT_INPUT     ? item->text.text
                                : item->type == SDL_EVENT_TEXT_EDITING ? item->edit.text
                                                                       : NULL;
            if (bytes != NULL) {
                size_t size = strnlen(bytes, PILOT_BYTES + 1);
                memcpy(strings[count], bytes, size);
                strings[count][size] = 0;
                if (item->type == SDL_EVENT_TEXT_INPUT)
                    item->text.text = strings[count];
                else
                    item->edit.text = strings[count];
                pilot.batch_ime = true;
                if (item->type == SDL_EVENT_TEXT_EDITING)
                    pilot.batch_composition = true;
            }
            ++count;
        }
        if (count == PILOT_BATCH)
            pilot.batch_ime = true;
    }
    if (next == count)
        return false;
    *event = batch[next++];
    return true;
}
