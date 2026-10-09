/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <book_edit.h>
#include <string.h>
#include <toolkit/packet.h>

/* Reject invalid, overlong, surrogate and out-of-range UTF-8 before publication. */
bool book_edit_utf8_valid(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = *p++;
        unsigned int extra;
        uint32_t minimum;
        if (cp < 0x80) {
            continue;
        } else if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 0x1f; extra = 1; minimum = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 0x0f; extra = 2; minimum = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7; extra = 3; minimum = 0x10000;
        } else {
            return false;
        }
        while (extra--) {
            if ((*p & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (*p++ & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool book_edit_parse(book_edit_snapshot_t *snapshot, const uint8_t *data, size_t len, size_t pos) {
    book_edit_snapshot_t next = {0};
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    next.result = packet_reader_read_uint8(&reader);
    next.session = packet_reader_read_uint32(&reader);
    next.selected = packet_reader_read_uint32(&reader);
    next.ink = packet_reader_read_uint32(&reader);
    next.capacity = packet_reader_read_uint32(&reader);
    next.count = packet_reader_read_uint16(&reader);
    if (next.result > BOOK_EDIT_ERROR || next.session == 0 ||
        next.ink > next.capacity || next.count > BOOK_EDIT_BOOKS_MAX) {
        return false;
    }
    bool selected_found = next.selected == 0;
    for (size_t i = 0; i < next.count; i++) {
        book_edit_entry_t *entry = &next.books[i];
        entry->tag = packet_reader_read_uint32(&reader);
        uint8_t finalized = packet_reader_read_uint8(&reader);
        entry->finalized = finalized != 0;
        if (!packet_reader_read_string(&reader, entry->title, sizeof(entry->title)) ||
            !book_edit_utf8_valid(entry->title) || finalized > 1 || entry->tag == 0) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (next.books[j].tag == entry->tag) {
                return false;
            }
        }
        selected_found |= next.selected == entry->tag;
    }
    if (!selected_found ||
        !packet_reader_read_string(&reader, next.title, sizeof(next.title)) ||
        !packet_reader_read_string(&reader, next.contents, sizeof(next.contents)) ||
        !packet_reader_read_string(&reader, next.notice, sizeof(next.notice)) ||
        !packet_reader_finish(&reader) || !book_edit_utf8_valid(next.title) ||
        !book_edit_utf8_valid(next.contents) || !book_edit_utf8_valid(next.notice) ||
        (next.selected == 0 && (next.title[0] || next.contents[0]))) {
        return false;
    }
    *snapshot = next;
    return true;
}
