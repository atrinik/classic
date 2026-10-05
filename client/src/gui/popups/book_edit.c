/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <book_edit.h>
#include <button.h>
#include <client.h>
#include <commands.h>
#include <event.h>
#include <main.h>
#include <popup.h>
#include <sprite.h>
#include <text.h>
#include <text_input.h>
#include <texture.h>
#include <toolkit/packet.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

/* This editor owns a separate draft; ordinary BOOK/help popups cannot replace it. */
static book_edit_snapshot_t current;
static popup_struct *editor;
static text_input_struct title, contents;
static button_struct buttons[8];
static bool initialized, pending;
static uint32_t source;
static uint32_t draft_destination;
static uint32_t selection_target;
static enum { CONFIRM_NONE, CONFIRM_COPY, CONFIRM_SIGN, CONFIRM_SELECT } confirmation;
static char message[BOOK_EDIT_NOTICE_MAX + 1];

static const book_edit_entry_t *find_book(uint32_t tag) {
    for (size_t i = 0; i < current.count; i++) {
        if (current.books[i].tag == tag) {
            return &current.books[i];
        }
    }
    return NULL;
}

static bool dirty(void) {
    return strcmp(title.str, current.title) != 0 || strcmp(contents.str, current.contents) != 0;
}

static void submit(enum book_edit_action action, uint32_t destination) {
    if (pending || current.session == 0 || cpl.state != ST_PLAY) {
        return;
    }
    packet_struct *packet = packet_new(SERVER_CMD_BOOK_EDIT, 256, 256);
    packet_writer_write_uint8(packet, action);
    packet_writer_write_uint32(packet, current.session);
    packet_writer_write_uint32(packet, destination);
    packet_writer_write_uint32(packet, action == BOOK_EDIT_COPY ? source : 0);
    packet_writer_write_cstring(packet, action == BOOK_EDIT_SAVE ? title.str :
                                          action == BOOK_EDIT_SIGN ? current.title : "");
    packet_writer_write_cstring(packet, action == BOOK_EDIT_SAVE ? contents.str :
                                          action == BOOK_EDIT_SIGN ? current.contents : "");
    socket_send_packet(packet);
    pending = action != BOOK_EDIT_CANCEL;
    confirmation = CONFIRM_NONE;
    snprintf(message, sizeof(message), "Waiting for the server...");
}

static void label(popup_struct *popup, const char *text, int x, int y, int height) {
    SDL_Rect box = {.w = popup->surface->w - x - 26, .h = height};
    text_show(popup->surface, FONT_ARIAL11, text, x, y, COLOR_BLACK,
              TEXT_WORD_WRAP, &box);
}

static int draw(popup_struct *popup) {
    surface_show(popup->surface, 0, 0, NULL, texture_surface(popup->texture));
    label(popup, "Write a book", 63, 27, 22);
    const book_edit_entry_t *destination = find_book(draft_destination);
    const book_edit_entry_t *src = find_book(source);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Destination (#%u): %s%s", draft_destination,
             destination ? destination->title : "Choose a book", destination && destination->finalized ? " [signed, read only]" : "");
    label(popup, buffer, 28, 58, 23);
    snprintf(buffer, sizeof(buffer), "Copy source (#%u): %s%s", source,
             src ? src->title : "Choose a different book", src && src->finalized ? " [signed]" : "");
    label(popup, buffer, 28, 90, 23);
    label(popup, "Title", 28, 123, 18);
    label(popup, "Contents (Enter: newline; Tab: change field)", 28, 155, 18);
    text_input_set_parent(&title, popup->x, popup->y);
    text_input_set_parent(&contents, popup->x, popup->y);
    text_input_show(&title, popup->surface, 85, 120);
    text_input_show(&contents, popup->surface, 28, 176);
    snprintf(buffer, sizeof(buffer), "Ink: %u/%u. Contents: %zu/%u bytes. Insertions cost 1 ink/byte; deletions/title/signing are free.",
             current.ink, current.capacity, contents.num, BOOK_EDIT_CONTENT_MAX);
    label(popup, buffer, 28, 329, 30);
    label(popup, message, 28, 362, 45);
    static const char *names[] = {"Next", "Next", "Save", "Copy", "Sign", "Cancel", "Confirm", "Back"};
    static const int xs[] = {580, 580, 28, 138, 248, 358, 248, 358};
    for (size_t i = 0; i < arraysize(buttons); i++) {
        buttons[i].x = xs[i];
        buttons[i].y = i < 2 ? 54 + (int)i * 32 : 406;
        buttons[i].surface = popup->surface;
        button_set_parent(&buttons[i], popup->x, popup->y);
        bool confirm = confirmation != CONFIRM_NONE;
        if ((i >= 6) != confirm && i >= 2) {
            continue;
        }
        buttons[i].disabled = pending || (i < 2 && (confirm || current.count < 2));
        if (i >= 2 && i <= 4) {
            buttons[i].disabled |= destination == NULL || destination->finalized ||
                                   draft_destination != current.selected;
        }
        if (i == 3) {
            buttons[i].disabled |= src == NULL || source == draft_destination;
        }
        button_show(&buttons[i], names[i]);
    }
    return 1;
}

static uint32_t next_book(uint32_t tag, bool writable) {
    size_t start = 0;
    for (size_t i = 0; i < current.count; i++) {
        if (current.books[i].tag == tag) {
            start = i + 1;
            break;
        }
    }
    for (size_t i = 0; i < current.count; i++) {
        const book_edit_entry_t *entry = &current.books[(start + i) % current.count];
        if (!writable || !entry->finalized) {
            return entry->tag;
        }
    }
    return tag;
}

static void cancel(void) {
    submit(BOOK_EDIT_CANCEL, draft_destination);
    /* Cancel is deliberate discard; error packets never take this path. */
    current.session = 0;
    draft_destination = 0;
    pending = false;
    text_input_reset(&title);
    text_input_reset(&contents);
    if (editor) {
        popup_destroy(editor);
    }
}

static int handle_event(popup_struct *popup, SDL_Event *event) {
    (void)popup;
    bool confirm = confirmation != CONFIRM_NONE;
    for (size_t i = 0; i < arraysize(buttons); i++) {
        if (i >= 2 && ((i >= 6) != confirm)) {
            continue;
        }
        if (!button_event(&buttons[i], event)) {
            continue;
        }
        switch (i) {
            case 0:
                selection_target = next_book(draft_destination, true);
                if (selection_target == draft_destination) {
                    break;
                }
                if (dirty()) {
                    confirmation = CONFIRM_SELECT;
                    snprintf(message, sizeof(message), "Discard this unsaved draft and select book #%u?", selection_target);
                } else {
                    submit(BOOK_EDIT_SELECT, selection_target);
                }
                break;
            case 1:
                source = next_book(source, false);
                break;
            case 2:
                submit(BOOK_EDIT_SAVE, draft_destination);
                break;
            case 3: {
                const book_edit_entry_t *src = find_book(source);
                const book_edit_entry_t *dst = find_book(draft_destination);
                confirmation = CONFIRM_COPY;
                snprintf(message, sizeof(message), "Replace destination '%s' (#%u) with source '%s' (#%u)? Unsaved edits will be discarded. The copy remains unsigned.",
                         dst ? dst->title : "", draft_destination, src ? src->title : "", source);
                break;
            }
            case 4:
                if (dirty()) {
                    snprintf(message, sizeof(message), "Save your edits first, then sign the saved book.");
                } else {
                    confirmation = CONFIRM_SIGN;
                    snprintf(message, sizeof(message), "Permanently sign book #%u as your character? It will become read only and signing cannot be undone.", draft_destination);
                }
                break;
            case 5: cancel(); return 1;
            case 6:
                submit(confirmation == CONFIRM_COPY ? BOOK_EDIT_COPY :
                       confirmation == CONFIRM_SIGN ? BOOK_EDIT_SIGN : BOOK_EDIT_SELECT,
                       confirmation == CONFIRM_SELECT ? selection_target : draft_destination);
                break;
            case 7:
                confirmation = CONFIRM_NONE;
                snprintf(message, sizeof(message), "Draft retained.");
                break;
        }
        return 1;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) {
        if (confirm) {
            confirmation = CONFIRM_NONE;
            snprintf(message, sizeof(message), "Draft retained.");
        } else {
            cancel();
        }
        return 1;
    }
    const book_edit_entry_t *destination = find_book(draft_destination);
    if (!pending && !confirm && destination && !destination->finalized) {
        if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_TAB) {
            title.focus = !title.focus;
            contents.focus = !title.focus;
            return 1;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
            if (text_input_mouse_over(&title, event->button.x, event->button.y)) {
                title.focus = 1; contents.focus = 0;
            } else if (text_input_mouse_over(&contents, event->button.x, event->button.y)) {
                title.focus = 0; contents.focus = 1;
            }
        }
        text_input_event(title.focus ? &title : &contents, event);
    }
    /* Focused editor consumes all key/text events, including releases. */
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP ||
        event->type == SDL_EVENT_TEXT_INPUT || event->type == SDL_EVENT_TEXT_EDITING) {
        return 1;
    }
    return -1;
}

static int destroyed(popup_struct *popup) {
    (void)popup;
    editor = NULL;
    /* Closing the canvas keeps the draft/session; deliberate Cancel discards. */
    return 1;
}

static bool open_editor(void) {
    if (editor) {
        return true;
    }
    editor = popup_create(texture_get(TEXTURE_TYPE_CLIENT, "book"));
    if (!editor) {
        return false;
    }
    editor->draw_func = draw;
    editor->event_func = handle_event;
    editor->destroy_callback_func = destroyed;
    editor->disable_texture_drawing = 1;
    return true;
}

void socket_command_book_edit(uint8_t *data, size_t len, size_t pos) {
    book_edit_snapshot_t next;
    if (!book_edit_parse(&next, data, len, pos)) {
        LOG(ERROR, "Rejected malformed BOOK_EDIT snapshot");
        return;
    }
    if (next.result != BOOK_EDIT_OPEN && next.session != current.session) {
        return;
    }
    if (!initialized) {
        text_input_create(&title);
        title.max = BOOK_EDIT_TITLE_MAX;
        title.coords.w = 555;
        text_input_create(&contents);
        contents.max = BOOK_EDIT_CONTENT_MAX;
        contents.multiline = 1;
        contents.coords.w = 612;
        contents.coords.h = 148;
        for (size_t i = 0; i < arraysize(buttons); i++) {
            button_create(&buttons[i]);
        }
        initialized = true;
    }
    bool retain = next.result == BOOK_EDIT_ERROR && current.session == next.session;
    current = next;
    pending = false;
    confirmation = CONFIRM_NONE;
    if (!retain) {
        text_input_set(&title, current.title);
        text_input_set(&contents, current.contents);
        draft_destination = current.selected;
        title.focus = 1;
        contents.focus = 0;
    }
    if (!find_book(source)) {
        source = current.count ? current.books[0].tag : 0;
    }
    snprintf(message, sizeof(message), "%s", current.notice);
    if (!open_editor() && !client_command_retry_current()) {
        LOG(ERROR, "Could not retain book editor popup for GPU recovery");
    }
}

void book_edit_disconnect(void) {
    if (editor) {
        popup_destroy(editor);
    }
    current.session = 0;
    pending = false;
    confirmation = CONFIRM_NONE;
    /* The disconnected draft remains local, but cannot be submitted. */
}

void book_edit_deinit(void) {
    book_edit_disconnect();
    if (initialized) {
        text_input_destroy(&title);
        text_input_destroy(&contents);
        for (size_t i = 0; i < arraysize(buttons); i++) {
            button_destroy(&buttons[i]);
        }
        initialized = false;
    }
}
