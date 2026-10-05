/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CLIENT_BOOK_EDIT_H
#define CLIENT_BOOK_EDIT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <atrinik/protocol/book_edit.h>

typedef struct book_edit_entry {
    uint32_t tag;
    bool finalized;
    char title[BOOK_EDIT_TITLE_MAX + 1];
} book_edit_entry_t;
typedef struct book_edit_snapshot {
    uint8_t result;
    uint32_t session, selected, ink, capacity;
    uint16_t count;
    book_edit_entry_t books[BOOK_EDIT_BOOKS_MAX];
    char title[BOOK_EDIT_TITLE_MAX + 1];
    char contents[BOOK_EDIT_CONTENT_MAX + 1];
    char notice[BOOK_EDIT_NOTICE_MAX + 1];
} book_edit_snapshot_t;

bool book_edit_utf8_valid(const char *text);
bool book_edit_parse(book_edit_snapshot_t *snapshot, const uint8_t *data, size_t len, size_t pos);
enum book_edit_confirmation {
    BOOK_CONFIRM_NONE, BOOK_CONFIRM_COPY, BOOK_CONFIRM_SIGN, BOOK_CONFIRM_SELECT,
    BOOK_CONFIRM_OPEN_DISCARD, BOOK_CONFIRM_OPEN_REBASE
};

/* The base and draft remain bound to destination across a closed canvas. */
typedef struct book_edit_model {
    book_edit_snapshot_t current, incoming;
    char title[BOOK_EDIT_TITLE_MAX + 1], contents[BOOK_EDIT_CONTENT_MAX + 1];
    char base_title[BOOK_EDIT_TITLE_MAX + 1], base_contents[BOOK_EDIT_CONTENT_MAX + 1];
    uint32_t destination, source, selection_target, confirmed_source;
    bool pending;
    enum book_edit_confirmation confirmation;
} book_edit_model_t;

bool book_edit_model_dirty(const book_edit_model_t *model);
bool book_edit_model_receive(book_edit_model_t *model, const book_edit_snapshot_t *next);
void book_edit_model_resolve_open(book_edit_model_t *model, bool accept);
void book_edit_model_close(book_edit_model_t *model);
void book_edit_model_cancel(book_edit_model_t *model);
bool book_edit_model_confirm(book_edit_model_t *model, enum book_edit_action action,
                             uint32_t destination);
bool book_edit_model_can_submit(const book_edit_model_t *model, enum book_edit_action action,
                                uint32_t destination);
void socket_command_book_edit(uint8_t *data, size_t len, size_t pos);
/** Forget session identity on disconnect; never submit an old draft to a new connection. */
void book_edit_disconnect(void);
void book_edit_deinit(void);
#ifdef ATRINIK_WIDGET_TESTS
typedef struct book_edit_test_request {
    unsigned count;
    enum book_edit_action action;
    uint32_t session, destination, source;
    char title[BOOK_EDIT_TITLE_MAX + 1], contents[BOOK_EDIT_CONTENT_MAX + 1];
} book_edit_test_request_t;
/* Read-only production-popup and serialized-request observations. */
const book_edit_model_t *book_edit_test_model(void);
const book_edit_test_request_t *book_edit_test_request(void);
bool book_edit_test_title_focused(void);
#endif

#endif
