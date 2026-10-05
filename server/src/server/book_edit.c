/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <global.h>
#include <server_main.h>
#include <server.h>
#include <server_item.h>
#include <object.h>
#include <player.h>
#include <book_edit.h>
#include <tod.h>
#include <toolkit/packet.h>

#define BOOK_EDIT_DEPTH_MAX 32U
#define BOOK_EDIT_SCAN_MAX 4096U

typedef struct book_snapshot {
    tag_t tag;
    tag_t ancestors[BOOK_EDIT_DEPTH_MAX];
    uint64_t generations[BOOK_EDIT_DEPTH_MAX];
    size_t depth;
    bool finalized;
    char title[BOOK_EDIT_TITLE_MAX + 1];
    char contents[BOOK_EDIT_CONTENT_MAX + 1];
} book_snapshot;

struct book_edit_session {
    uint32_t id;
    tag_t selected;
    book_snapshot pen;
    size_t count;
    book_snapshot books[BOOK_EDIT_BOOKS_MAX];
};

bool book_edit_is_pen(const object *op) {
    return op != NULL && op->type == SKILL_ITEM && op->stats.sp == SK_INSCRIPTION;
}

bool book_edit_finalized(const object *op) {
    /* Fail closed even for an unexpected marker value. */
    return object_get_value(op, BOOK_EDIT_FINALIZED) != NULL;
}

/* UTF-8 scalar validation and loader-safe text. Title names are single-line.
 * Reject the existing loader sentinel even in mixed case for legacy safety. */
bool book_edit_text_valid(const char *text, bool title) {
    if (text == NULL) {
        return false;
    }
    size_t len = strlen(text);
    if (len > (title ? BOOK_EDIT_TITLE_MAX : BOOK_EDIT_CONTENT_MAX) ||
        (title && (len == 0 || isspace((unsigned char)text[0]) ||
                   isspace((unsigned char)text[len - 1]))) ||
        (!title && strcasestr(text, "endmsg") != NULL)) {
        return false;
    }
    for (size_t i = 0; i < len;) {
        unsigned char ch = (unsigned char)text[i++];
        if (ch < 0x80) {
            if ((ch < 0x20 && (title || (ch != '\n' && ch != '\t'))) || ch == 0x7f) {
                return false;
            }
            continue;
        }
        unsigned int extra, code, minimum;
        if (ch >= 0xc2 && ch <= 0xdf) {
            extra = 1; code = ch & 0x1f; minimum = 0x80;
        } else if (ch >= 0xe0 && ch <= 0xef) {
            extra = 2; code = ch & 0x0f; minimum = 0x800;
        } else if (ch >= 0xf0 && ch <= 0xf4) {
            extra = 3; code = ch & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (len - i < extra) {
            return false;
        }
        for (unsigned int j = 0; j < extra; j++) {
            ch = (unsigned char)text[i++];
            if ((ch & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (ch & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

size_t book_edit_ink_cost(const char *before, const char *after) {
    size_t old_len = strlen(before), new_len = strlen(after);
    if (old_len > BOOK_EDIT_CONTENT_MAX || new_len > BOOK_EDIT_CONTENT_MAX) {
        return SIZE_MAX;
    }
    if (new_len == 0 || strcmp(before, after) == 0) {
        return 0;
    }
    /* One DP row: O(maximum text size) storage, bounded quadratic work. */
    uint16_t row[BOOK_EDIT_CONTENT_MAX + 1] = {0};
    for (size_t i = 0; i < old_len; i++) {
        uint16_t diagonal = 0;
        for (size_t j = 1; j <= new_len; j++) {
            uint16_t above = row[j];
            row[j] = before[i] == after[j - 1] ? diagonal + 1 : MAX(row[j], row[j - 1]);
            diagonal = above;
        }
    }
    return new_len - row[new_len];
}

static const char *contents(const object *op) {
    return op->msg != NULL ? op->msg : "";
}

static bool snapshot_location(book_snapshot *snapshot, const object *op, const object *owner) {
    snapshot->tag = op->count;
    snapshot->depth = 0;
    while (op != owner) {
        if (op == NULL || QUERY_FLAG(op, FLAG_REMOVED) ||
            snapshot->depth == BOOK_EDIT_DEPTH_MAX) {
            return false;
        }
        size_t i = snapshot->depth++;
        snapshot->ancestors[i] = op->count;
        snapshot->generations[i] = op->inventory_generation;
        op = op->env;
    }
    return snapshot->depth != 0;
}

static bool snapshot_book(book_snapshot *snapshot, const object *op, const object *owner) {
    if (op->type != BOOK || QUERY_FLAG(op, FLAG_UNPAID) ||
        !book_edit_text_valid(op->name, true) ||
        !book_edit_text_valid(contents(op), false) || !snapshot_location(snapshot, op, owner)) {
        return false;
    }
    snapshot->finalized = book_edit_finalized(op);
    snprintf(snapshot->title, sizeof(snapshot->title), "%s", op->name);
    snprintf(snapshot->contents, sizeof(snapshot->contents), "%s", contents(op));
    return true;
}

static object *inventory_find(object *owner, object *list, tag_t tag, size_t depth,
                              size_t *budget) {
    if (depth == BOOK_EDIT_DEPTH_MAX) {
        return NULL;
    }
    for (object *op = list; op != NULL; op = op->below) {
        if (*budget == 0) {
            return NULL;
        }
        (*budget)--;
        if (IS_INVISIBLE(op, owner) || QUERY_FLAG(op, FLAG_REMOVED)) {
            continue;
        }
        if (op->count == tag) {
            return op;
        }
        object *found = inventory_find(owner, op->inv, tag, depth + 1, budget);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static object *resolve(object *owner, tag_t tag) {
    size_t budget = BOOK_EDIT_SCAN_MAX;
    return inventory_find(owner, owner->inv, tag, 0, &budget);
}

object *book_edit_marked_inventory(object *writer) {
    player *pl = CONTR(writer);
    /* Compare the stored pointer only after resolving its tag. A recycled or
     * dropped marked pointer is never dereferenced. */
    object *item = pl->mark != NULL ? resolve(writer, pl->mark_count) : NULL;
    return item == pl->mark ? item : NULL;
}

bool book_edit_inventory_contains(object *writer, const object *item) {
    return item != NULL && resolve(writer, item->count) == item;
}

static bool location_matches(const book_snapshot *snapshot, const object *op, const object *owner) {
    book_snapshot now;
    if (!snapshot_location(&now, op, owner) || now.depth != snapshot->depth) {
        return false;
    }
    for (size_t i = 0; i < now.depth; i++) {
        if (now.ancestors[i] != snapshot->ancestors[i] ||
            now.generations[i] != snapshot->generations[i]) {
            return false;
        }
    }
    return true;
}

static bool book_matches(const book_snapshot *snapshot, const object *op, const object *owner) {
    return op != NULL && op->type == BOOK && !QUERY_FLAG(op, FLAG_UNPAID) &&
           op->name != NULL && location_matches(snapshot, op, owner) &&
           snapshot->finalized == book_edit_finalized(op) &&
           strcmp(snapshot->title, op->name) == 0 && strcmp(snapshot->contents, contents(op)) == 0;
}

static book_snapshot *find_snapshot(struct book_edit_session *session, tag_t tag) {
    for (size_t i = 0; i < session->count; i++) {
        if (session->books[i].tag == tag) {
            return &session->books[i];
        }
    }
    return NULL;
}

void book_edit_clear(player *pl) {
    free(pl->book_editor);
    pl->book_editor = NULL;
}

static void reply(player *pl, enum book_edit_result result, const char *notice) {
    struct book_edit_session *session = pl->book_editor;
    object *pen = session != NULL ? resolve(pl->ob, session->pen.tag) : NULL;
    book_snapshot *selected = session != NULL ? find_snapshot(session, session->selected) : NULL;
    packet_struct *packet = packet_new(CLIENT_CMD_BOOK_EDIT, 512, 512);
    packet_writer_write_uint8(packet, result);
    packet_writer_write_uint32(packet, session != NULL ? session->id : 0);
    packet_writer_write_uint32(packet, selected != NULL ? selected->tag : 0);
    packet_writer_write_uint32(packet, pen != NULL ? MAX(0, MIN(pen->stats.food, pen->stats.maxhp)) : 0);
    packet_writer_write_uint32(packet, pen != NULL ? MAX(0, pen->stats.maxhp) : 0);
    packet_writer_write_uint16(packet, session != NULL ? session->count : 0);
    if (session != NULL) {
        for (size_t i = 0; i < session->count; i++) {
            packet_writer_write_uint32(packet, session->books[i].tag);
            packet_writer_write_uint8(packet, session->books[i].finalized);
            packet_writer_write_cstring(packet, session->books[i].title);
        }
    }
    packet_writer_write_cstring(packet, selected != NULL ? selected->title : "");
    packet_writer_write_cstring(packet, selected != NULL ? selected->contents : "");
    packet_writer_write_cstring(packet, notice);
    socket_send_packet(pl->cs, packet);
}

static const char *prerequisite(object *writer, object *pen) {
    if (!book_edit_is_pen(pen) || pen->env != writer || IS_INVISIBLE(pen, writer) ||
        QUERY_FLAG(pen, FLAG_REMOVED)) {
        return "Carry the writing pen in your main inventory, then apply it again.";
    }
    if (QUERY_FLAG(pen, FLAG_UNPAID)) {
        return "You should pay for the writing pen first.";
    }
    if (pen->nrof > 1) {
        return "Split off one writing pen from the stack before writing or refilling.";
    }
    if (pen->stats.maxhp <= 0 || pen->stats.food < 0 || pen->stats.food > pen->stats.maxhp ||
        pen->race == NULL || strcmp(pen->race, "writing_ink") != 0) {
        return "This pen cannot hold writing ink. Obtain a refillable writing pen.";
    }
    if (find_skill(writer, SK_LITERACY) == NULL) {
        return "You must learn Literacy before you can write.";
    }
    if (find_skill(writer, SK_INSCRIPTION) == NULL) {
        return "You must learn Inscription before you can write.";
    }
    if (QUERY_FLAG(writer, FLAG_BLIND)) {
        return "You are unable to write while blind.";
    }
    return NULL;
}

static bool collect(struct book_edit_session *session, object *owner, object *list,
                    size_t depth, size_t *budget) {
    if (depth == BOOK_EDIT_DEPTH_MAX) {
        return list == NULL;
    }
    for (object *op = list; op != NULL; op = op->below) {
        if (*budget == 0) {
            return false;
        }
        (*budget)--;
        /* Hidden containers must not reveal their books through the editor. */
        if (IS_INVISIBLE(op, owner) || QUERY_FLAG(op, FLAG_REMOVED)) {
            continue;
        }
        if (op->type == BOOK) {
            book_snapshot snapshot;
            if (snapshot_book(&snapshot, op, owner)) {
                if (session->count == BOOK_EDIT_BOOKS_MAX) {
                    return false;
                }
                session->books[session->count++] = snapshot;
            }
        }
        if (!collect(session, owner, op->inv, depth + 1, budget)) {
            return false;
        }
    }
    return true;
}

bool book_edit_open(object *pen, object *writer) {
    player *pl = CONTR(writer);
    book_edit_clear(pl);
    const char *error = prerequisite(writer, pen);
    if (error != NULL) {
        draw_info(COLOR_WHITE, writer, error);
        return false;
    }
    /* Resolve the stored identity within the same bounded visible-inventory
     * walk used at commit time; never dereference a possibly stale mark. */
    object *book = book_edit_marked_inventory(writer);
    if (book == NULL || book->type != BOOK) {
        draw_info(COLOR_WHITE, writer,
                  "Mark a book or letter in your inventory, then apply the writing pen.");
        return false;
    }
    if (QUERY_FLAG(book, FLAG_UNPAID)) {
        draw_info(COLOR_WHITE, writer, "You should pay for the book first.");
        return false;
    }
    if (book->nrof > 1 || book_edit_finalized(book)) {
        draw_info(COLOR_WHITE, writer, book->nrof > 1 ?
                  "Split off one book from the stack before editing or signing it." :
                  "This book is signed and permanently finalized. It may still be read or copied.");
        return false;
    }
    struct book_edit_session *session = xcalloc(1, sizeof(*session));
    size_t budget = BOOK_EDIT_SCAN_MAX;
    if (!snapshot_location(&session->pen, pen, writer) ||
        !collect(session, writer, writer->inv, 0, &budget) ||
        find_snapshot(session, book->count) == NULL) {
        free(session);
        draw_info(COLOR_WHITE, writer,
                  "The editor supports at most 64 books, 127 title bytes and 2037 text bytes. "
                  "This inventory or book exceeds its supported limits or contains unsafe text.");
        return false;
    }
    static uint32_t next_session;
    if (++next_session == 0) {
        ++next_session;
    }
    session->id = next_session;
    session->selected = book->count;
    pl->book_editor = session;
    reply(pl, BOOK_EDIT_OPEN,
          "Ink: 1 unit per new or replaced UTF-8 byte; deletions, renaming and signing are free. "
          "Mark the pen and apply an ink bottle to refill. Signing permanently locks title and text.");
    return true;
}

void socket_command_book_edit(socket_struct *cs, player *pl, uint8_t *data,
                             size_t len, size_t pos) {
    (void) cs;
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    uint8_t action = packet_reader_read_uint8(&reader);
    uint32_t id = packet_reader_read_uint32(&reader);
    tag_t destination = packet_reader_read_uint32(&reader);
    tag_t source = packet_reader_read_uint32(&reader);
    char title[BOOK_EDIT_TITLE_MAX + 1], body[BOOK_EDIT_CONTENT_MAX + 1];
    bool valid = packet_reader_read_string(&reader, title, sizeof(title));
    valid = packet_reader_read_string(&reader, body, sizeof(body)) && valid;
    if (!valid || !packet_reader_finish(&reader) || action > BOOK_EDIT_CANCEL ||
        (action != BOOK_EDIT_COPY && source != 0) ||
        ((action == BOOK_EDIT_COPY || action == BOOK_EDIT_CANCEL || action == BOOK_EDIT_SELECT) &&
         (*title != '\0' || *body != '\0'))) {
        reply(pl, BOOK_EDIT_ERROR, "Malformed book editor request; your draft has not been saved.");
        return;
    }
    struct book_edit_session *session = pl->book_editor;
    if (session == NULL || session->id != id) {
        reply(pl, BOOK_EDIT_ERROR, "This editor session has expired. Apply the pen again.");
        return;
    }
    if (action == BOOK_EDIT_CANCEL) {
        book_edit_clear(pl);
        return;
    }
    object *pen = resolve(pl->ob, session->pen.tag);
    const char *error = prerequisite(pl->ob, pen);
    if (error != NULL) {
        reply(pl, BOOK_EDIT_ERROR, error);
        return;
    }
    if (!location_matches(&session->pen, pen, pl->ob)) {
        reply(pl, BOOK_EDIT_ERROR, "The pen moved while the editor was open. Apply it again.");
        return;
    }
    book_snapshot *snapshot = find_snapshot(session, destination);
    object *book = resolve(pl->ob, destination);
    if (snapshot == NULL || !book_matches(snapshot, book, pl->ob)) {
        reply(pl, BOOK_EDIT_ERROR,
              "The book moved or changed while the editor was open. Keep your draft and reopen it.");
        return;
    }
    if (action == BOOK_EDIT_SELECT) {
        session->selected = destination;
        reply(pl, BOOK_EDIT_UPDATED, snapshot->finalized ?
              "Signed book: permanently finalized; available as a copy source only." : "Book selected.");
        return;
    }
    if (book_edit_finalized(book) || book->nrof > 1) {
        reply(pl, BOOK_EDIT_ERROR, book->nrof > 1 ?
              "Split off one book from the stack before editing or signing it." :
              "This signed book is permanently finalized and cannot be changed.");
        return;
    }
    if (action == BOOK_EDIT_COPY) {
        book_snapshot *original = find_snapshot(session, source);
        object *from = resolve(pl->ob, source);
        if (source == destination || original == NULL || !book_matches(original, from, pl->ob)) {
            reply(pl, BOOK_EDIT_ERROR, "The copy source moved or changed, or matches the destination.");
            return;
        }
        snprintf(title, sizeof(title), "%s", original->title);
        snprintf(body, sizeof(body), "%s", original->contents);
    }
    if (!book_edit_text_valid(title, true) || !book_edit_text_valid(body, false)) {
        reply(pl, BOOK_EDIT_ERROR,
              "Use valid UTF-8: title 1-127 bytes, text up to 2037 bytes. "
              "Titles cannot have edge whitespace or line breaks; text cannot contain endmsg.");
        return;
    }
    if (action == BOOK_EDIT_SIGN &&
        (strcmp(title, snapshot->title) != 0 || strcmp(body, snapshot->contents) != 0)) {
        reply(pl, BOOK_EDIT_ERROR, "Save your title and text before signing the book.");
        return;
    }
    size_t cost = book_edit_ink_cost(contents(book), body);
    if (cost > (size_t)pen->stats.food) {
        reply(pl, BOOK_EDIT_ERROR, "Not enough ink. Your draft is kept; refill the pen and retry.");
        return;
    }
    bool changed = strcmp(title, book->name) != 0 || strcmp(body, contents(book)) != 0;
    /* All validation has completed. These mutations run on the simulation thread. */
    bool alias_changed = book->custom_name != NULL &&
                         (strcmp(title, book->name) != 0 || action == BOOK_EDIT_COPY);
    if (alias_changed) {
        /* A pre-existing /rename alias must not hide the chosen book title. */
        FREE_AND_CLEAR_HASH(book->custom_name);
    }
    if (changed) {
        FREE_AND_COPY_HASH(book->name, title);
        FREE_AND_COPY_HASH(book->msg, body);
        pen->stats.food -= (int)cost;
        metrics_add(&pl->metrics, METRIC_CHARACTER_BOOKS_INSCRIBED, 1);
    }
    if (action == BOOK_EDIT_SIGN) {
        timeofday_t now;
        get_tod(&now);
        char date[128], utc[32];
        snprintf(date, sizeof(date), "%d %s, Year %d", now.day + 1, month_name[now.month], now.year + 1);
        snprintf(utc, sizeof(utc), "%" PRId64, server_wall_utc_now().seconds);
        object_set_value(book, BOOK_EDIT_SIGNER, pl->ob->name, true);
        object_set_value(book, BOOK_EDIT_DATE, date, true);
        object_set_value(book, BOOK_EDIT_UTC, utc, true);
        object_set_value(book, BOOK_EDIT_FINALIZED, "1", true);
    }
    if (changed || alias_changed || action == BOOK_EDIT_SIGN) {
        book->inventory_generation++;
    }
    snapshot_book(snapshot, book, pl->ob);
    session->selected = destination;
    esrv_update_item(UPD_NAME, book);
    esrv_update_item(UPD_NAME, pen);
    char notice[256];
    if (action == BOOK_EDIT_SIGN) {
        snprintf(notice, sizeof(notice),
                 "Signed and dated. Title and text are now permanently locked.");
    } else {
        snprintf(notice, sizeof(notice), "Saved. Used %zu ink; %d remains.", cost, pen->stats.food);
    }
    reply(pl, BOOK_EDIT_UPDATED, notice);
}
