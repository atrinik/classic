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
void socket_command_book_edit(uint8_t *data, size_t len, size_t pos);
/** Forget session identity on disconnect; never submit an old draft to a new connection. */
void book_edit_disconnect(void);
void book_edit_deinit(void);
#endif
