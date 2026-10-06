/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ATRINIK_PROTOCOL_BOOK_EDIT_H
#define ATRINIK_PROTOCOL_BOOK_EDIT_H

/* BOOK_EDIT is available only in PLAYING state at protocol revision 1082.
 * Integers use the existing packet writer's network byte order. Strings are
 * NUL-terminated UTF-8, bounded in bytes excluding their terminator.
 *
 * C2S: action:u8, session:u32, destination:u32, source:u32, title:string,
 *      contents:string. Only COPY uses source (others must use zero). SAVE
 *      supplies the draft; SIGN supplies the unchanged persisted title/text;
 *      SELECT, CANCEL and COPY use empty title/text. No trailing bytes.
 *
 * S2C: result:u8, session:u32, selected:u32, ink:u32, capacity:u32, count:u16,
 *      count * (tag:u32, finalized:u8, title:string), selected_title:string,
 *      selected_contents:string, notice:string. Every packet is complete.
 * ERROR retains the current draft. OPEN binds a fresh session and preserves a
 * suspended dirty draft for the same unchanged destination. A different book
 * or changed persisted base requires explicit discard/rebase confirmation.
 * UPDATED replaces the draft after complete validation; SELECT discards a
 * draft only after explicit client confirmation.
 *
 * One session per connection. Applying another pen invalidates the old session;
 * reconnect never restores one. Server-held snapshots fence inventory custody,
 * identity, title/text and finalization. Failed submissions spend no ink.
 * CANCEL changes no item. COPY changes only destination name/msg and leaves it
 * unsigned. SIGN accepts persisted text only and permanently finalizes it.
 */
#define BOOK_EDIT_TITLE_MAX 127U
#define BOOK_EDIT_CONTENT_MAX 2037U
#define BOOK_EDIT_BOOKS_MAX 64U
#define BOOK_EDIT_NOTICE_MAX 1023U

enum book_edit_action {
    BOOK_EDIT_SAVE = 0,
    BOOK_EDIT_COPY = 1,
    BOOK_EDIT_SIGN = 2,
    BOOK_EDIT_SELECT = 3,
    BOOK_EDIT_CANCEL = 4
};

enum book_edit_result {
    BOOK_EDIT_OPEN = 0,
    BOOK_EDIT_UPDATED = 1,
    BOOK_EDIT_ERROR = 2
};

#endif
