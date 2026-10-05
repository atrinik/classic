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
#include <player.h>
#include <client_socket.h>
#include <sprite.h>
#include <text.h>
#include <text_input.h>
#include <texture.h>
#include <toolkit/packet.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

/* This editor owns a separate draft; ordinary BOOK/help popups cannot replace it. */
static book_edit_model_t model;
static popup_struct *editor;
static text_input_struct title, contents;
static button_struct buttons[8];
static bool initialized;
static char message[BOOK_EDIT_NOTICE_MAX + 1];

static const book_edit_entry_t *find_book(uint32_t tag) {
    for (size_t i = 0; i < model.current.count; i++) {
        if (model.current.books[i].tag == tag) {
            return &model.current.books[i];
        }
    }
    return NULL;
}

static void capture_draft(void) {
    if (initialized) {
        snprintf(model.title, sizeof(model.title), "%.*s", (int)BOOK_EDIT_TITLE_MAX, title.str);
        snprintf(model.contents, sizeof(model.contents), "%.*s", (int)BOOK_EDIT_CONTENT_MAX, contents.str);
    }
}

static void display_draft(void) {
    text_input_set(&title, model.title);
    text_input_set(&contents, model.contents);
}

static bool dirty(void) {
    capture_draft();
    return book_edit_model_dirty(&model);
}

#ifdef ATRINIK_WIDGET_TESTS
static book_edit_test_request_t last_request;

/* Observe serialized production requests without creating a transport. */
static void record_request(packet_struct *packet) {
    book_edit_test_request_t next = {.count = last_request.count + 1};
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    next.action = packet_reader_read_uint8(&reader);
    next.session = packet_reader_read_uint32(&reader);
    next.destination = packet_reader_read_uint32(&reader);
    next.source = packet_reader_read_uint32(&reader);
    if (packet_reader_read_string(&reader, next.title, sizeof(next.title)) &&
        packet_reader_read_string(&reader, next.contents, sizeof(next.contents)) &&
        packet_reader_finish(&reader)) {
        last_request = next;
    }
}

const book_edit_model_t *book_edit_test_model(void) {
    capture_draft();
    return &model;
}

const book_edit_test_request_t *book_edit_test_request(void) {
    return &last_request;
}

bool book_edit_test_title_focused(void) {
    return title.focus && !contents.focus;
}
#endif

static void submit(enum book_edit_action action, uint32_t destination) {
    if (!book_edit_model_can_submit(&model, action, destination) || cpl.state != ST_PLAY) {
        return;
    }
    packet_struct *packet = packet_new(SERVER_CMD_BOOK_EDIT, 256, 256);
    packet_writer_write_uint8(packet, action);
    packet_writer_write_uint32(packet, model.current.session);
    packet_writer_write_uint32(packet, destination);
    packet_writer_write_uint32(packet, action == BOOK_EDIT_COPY ? model.source : 0);
    packet_writer_write_cstring(packet, action == BOOK_EDIT_SAVE ? title.str :
                                          action == BOOK_EDIT_SIGN ? model.base_title : "");
    packet_writer_write_cstring(packet, action == BOOK_EDIT_SAVE ? contents.str :
                                          action == BOOK_EDIT_SIGN ? model.base_contents : "");
#ifdef ATRINIK_WIDGET_TESTS
    record_request(packet);
#endif
    socket_send_packet(packet);
    model.pending = action != BOOK_EDIT_CANCEL;
    model.confirmation = BOOK_CONFIRM_NONE;
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
    const book_edit_entry_t *destination = find_book(model.destination);
    const book_edit_entry_t *src = find_book(model.source);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "Destination (#%u): %s%s", model.destination,
             destination ? destination->title : "Choose a book", destination && destination->finalized ? " [signed, read only]" : "");
    label(popup, buffer, 28, 58, 23);
    snprintf(buffer, sizeof(buffer), "Copy source (#%u): %s%s", model.source,
             src ? src->title : "Choose a different book", src && src->finalized ? " [signed]" : "");
    label(popup, buffer, 28, 90, 23);
    label(popup, "Title", 28, 123, 18);
    label(popup, "Contents (Enter: newline; Tab: change field)", 28, 155, 18);
    text_input_set_parent(&title, popup->x, popup->y);
    text_input_set_parent(&contents, popup->x, popup->y);
    text_input_show(&title, popup->surface, 85, 120);
    text_input_show(&contents, popup->surface, 28, 176);
    snprintf(buffer, sizeof(buffer), "Ink: %u/%u. Contents: %zu/%u bytes. Insertions cost 1 ink/byte; deletions/title/signing are free.",
             model.current.ink, model.current.capacity, contents.num, BOOK_EDIT_CONTENT_MAX);
    label(popup, buffer, 28, 329, 30);
    label(popup, message, 28, 362, 45);
    static const char *names[] = {"Next", "Next", "Save", "Copy", "Sign", "Cancel", "Confirm", "Back"};
    static const int xs[] = {580, 580, 28, 138, 248, 358, 248, 358};
    for (size_t i = 0; i < arraysize(buttons); i++) {
        buttons[i].x = xs[i];
        buttons[i].y = i < 2 ? 54 + (int)i * 32 : 406;
        buttons[i].surface = popup->surface;
        button_set_parent(&buttons[i], popup->x, popup->y);
        bool confirm = model.confirmation != BOOK_CONFIRM_NONE;
        if ((i >= 6) != confirm && i >= 2) {
            continue;
        }
        buttons[i].disabled = model.pending || (i < 2 && (confirm || !model.current.session || model.current.count < 2));
        if (i >= 2 && i <= 4) {
            buttons[i].disabled |= model.current.session == 0 || destination == NULL || destination->finalized ||
                                   model.destination != model.current.selected;
        }
        if (i == 3) {
            buttons[i].disabled |= src == NULL || model.source == model.destination;
        }
        button_show(&buttons[i], names[i]);
    }
    return 1;
}

static uint32_t next_book(uint32_t tag, bool writable) {
    size_t start = 0;
    for (size_t i = 0; i < model.current.count; i++) {
        if (model.current.books[i].tag == tag) {
            start = i + 1;
            break;
        }
    }
    for (size_t i = 0; i < model.current.count; i++) {
        const book_edit_entry_t *entry = &model.current.books[(start + i) % model.current.count];
        if (!writable || !entry->finalized) {
            return entry->tag;
        }
    }
    return tag;
}

static void close_server_session(void) {
    uint32_t session = model.incoming.session ? model.incoming.session : model.current.session;
    uint32_t destination = model.incoming.session ? model.incoming.selected : model.destination;
    if (!session || cpl.state != ST_PLAY) {
        return;
    }
    packet_struct *packet = packet_new(SERVER_CMD_BOOK_EDIT, 32, 32);
    packet_writer_write_uint8(packet, BOOK_EDIT_CANCEL);
    packet_writer_write_uint32(packet, session);
    packet_writer_write_uint32(packet, destination);
    packet_writer_write_uint32(packet, 0);
    packet_writer_write_cstring(packet, "");
    packet_writer_write_cstring(packet, "");
#ifdef ATRINIK_WIDGET_TESTS
    record_request(packet);
#endif
    socket_send_packet(packet);
}

static void back(void) {
    if (model.confirmation == BOOK_CONFIRM_OPEN_DISCARD ||
        model.confirmation == BOOK_CONFIRM_OPEN_REBASE) {
        close_server_session();
        book_edit_model_resolve_open(&model, false);
        snprintf(message, sizeof(message), "Draft retained. Close, mark its original book, and apply the pen to resume.");
    } else {
        model.confirmation = BOOK_CONFIRM_NONE;
        snprintf(message, sizeof(message), "Draft retained.");
    }
}

static void cancel(void) {
    close_server_session();
    /* Cancel is deliberate discard; error packets never take this path. */
    book_edit_model_cancel(&model);
    text_input_reset(&title);
    text_input_reset(&contents);
    if (editor) {
        popup_destroy(editor);
    }
}

static int handle_event(popup_struct *popup, SDL_Event *event) {
    (void)popup;
    capture_draft();
    bool confirm = model.confirmation != BOOK_CONFIRM_NONE;
    for (size_t i = 0; i < arraysize(buttons); i++) {
        /* Input can drain several queued events before draw refreshes disabled
         * buttons; gate the live state at the point of each action. */
        if (model.pending || (i < 2 && (confirm || !model.current.session || model.current.count < 2))) {
            continue;
        }
        if (i >= 2 && ((i >= 6) != confirm)) {
            continue;
        }
        if (!button_event(&buttons[i], event)) {
            continue;
        }
        switch (i) {
            case 0:
                model.selection_target = next_book(model.destination, true);
                if (model.selection_target == model.destination) {
                    break;
                }
                if (dirty()) {
                    book_edit_model_confirm(&model, BOOK_EDIT_SELECT, model.selection_target);
                    snprintf(message, sizeof(message), "Discard this unsaved draft and select book #%u?", model.selection_target);
                } else {
                    submit(BOOK_EDIT_SELECT, model.selection_target);
                }
                break;
            case 1:
                model.source = next_book(model.source, false);
                break;
            case 2:
                submit(BOOK_EDIT_SAVE, model.destination);
                break;
            case 3: {
                const book_edit_entry_t *src = find_book(model.source);
                const book_edit_entry_t *dst = find_book(model.destination);
                if (!book_edit_model_confirm(&model, BOOK_EDIT_COPY, model.destination)) { break; }
                snprintf(message, sizeof(message), "Replace destination '%s' (#%u) with source '%s' (#%u)? Unsaved edits will be discarded. The copy remains unsigned.",
                         dst ? dst->title : "", model.destination, src ? src->title : "", model.source);
                break;
            }
            case 4:
                if (dirty()) {
                    snprintf(message, sizeof(message), "Save your edits first, then sign the saved book.");
                } else {
                    if (!book_edit_model_confirm(&model, BOOK_EDIT_SIGN, model.destination)) { break; }
                    snprintf(message, sizeof(message), "Permanently sign book #%u as your character? It will become read only and signing cannot be undone.", model.destination);
                }
                break;
            case 5: cancel(); return 1;
            case 6:
                if (model.confirmation == BOOK_CONFIRM_OPEN_DISCARD ||
                    model.confirmation == BOOK_CONFIRM_OPEN_REBASE) {
                    book_edit_model_resolve_open(&model, true);
                    display_draft();
                    snprintf(message, sizeof(message), "Draft ready. Review it before saving.");
                    break;
                }
                submit(model.confirmation == BOOK_CONFIRM_COPY ? BOOK_EDIT_COPY :
                       model.confirmation == BOOK_CONFIRM_SIGN ? BOOK_EDIT_SIGN : BOOK_EDIT_SELECT,
                       model.confirmation == BOOK_CONFIRM_SELECT ? model.selection_target : model.destination);
                break;
            case 7:
                back();
                break;
        }
        return 1;
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) {
        if (confirm) {
            back();
        } else {
            cancel();
        }
        return 1;
    }
    const book_edit_entry_t *destination = find_book(model.destination);
    if (!model.pending && !confirm && destination && !destination->finalized) {
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
    capture_draft();
    editor = NULL;
    /* The modal must close for inventory/refill. End the server session while
     * retaining draft and base; applying the pen supplies a fresh OPEN. */
    close_server_session();
    book_edit_model_close(&model);
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
    capture_draft();
    if (!book_edit_model_receive(&model, &next)) {
        return;
    }
    display_draft();
    title.focus = 1;
    contents.focus = 0;
    if (model.confirmation == BOOK_CONFIRM_OPEN_DISCARD) {
        snprintf(message, sizeof(message), "Discard unsaved draft for book #%u and open book #%u? Back keeps the draft.",
                 model.destination, model.incoming.selected);
    } else if (model.confirmation == BOOK_CONFIRM_OPEN_REBASE) {
        snprintf(message, sizeof(message), "Book #%u changed while closed. Keep your draft over its newer saved title/text? Review before saving; Back keeps it suspended.", model.destination);
    } else if (next.result == BOOK_EDIT_ERROR) {
        snprintf(message, sizeof(message), "%.900s Close with X to refill; reapply the pen on the same book to resume.", model.current.notice);
    } else {
        snprintf(message, sizeof(message), "%s", model.current.notice);
    }
    if (!open_editor() && !client_command_retry_current()) {
        LOG(ERROR, "Could not retain book editor popup for GPU recovery");
    }
}

void book_edit_disconnect(void) {
    /* Inventory tags are connection-local; a later character must never inherit
     * this draft under a coincidentally reused destination tag. Clear before
     * destroying the popup so its callback cannot send on a closed connection. */
    book_edit_model_cancel(&model);
    if (initialized) {
        text_input_reset(&title);
        text_input_reset(&contents);
    }
    if (editor) {
        popup_destroy(editor);
    }
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
