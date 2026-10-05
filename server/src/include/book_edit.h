/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SERVER_BOOK_EDIT_H
#define SERVER_BOOK_EDIT_H

#include <atrinik/protocol/book_edit.h>

#define BOOK_EDIT_FINALIZED "book_finalized"
#define BOOK_EDIT_SIGNER "book_signer"
#define BOOK_EDIT_DATE "book_signed_date"
#define BOOK_EDIT_UTC "book_signed_utc"

bool book_edit_is_pen(const object *op);
bool book_edit_finalized(const object *op);
bool book_edit_text_valid(const char *text, bool title);
size_t book_edit_ink_cost(const char *before, const char *after);
bool book_edit_open(object *pen, object *writer);
void book_edit_clear(player *pl);
void socket_command_book_edit(socket_struct *cs, player *pl, uint8_t *data,
                             size_t len, size_t pos);

#endif
