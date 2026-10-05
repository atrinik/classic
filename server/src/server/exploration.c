/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/
/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <global.h>
#include <account.h>
#include <exploration.h>
#include <server.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>

#define EXPLORATION_MAPS_MAX 10000U
#define EXPLORATION_DIM_MAX 256U
#define EXPLORATION_PATH_MAX 255U
#define EXPLORATION_BYTES_MAX (96U * 1024U * 1024U)
#define EXPLORATION_MAGIC "AEXP0001"

typedef struct exploration_map {
    char *path;
    unsigned width, height;
    uint8_t *bits;
    UT_hash_handle hh;
} exploration_map;

typedef struct exploration_pending {
    struct exploration_pending *next, *prev;
    char *path;
    exploration_map *map;
    uint8_t *bits;
    UT_hash_handle hh;
} exploration_pending;

typedef struct exploration_account exploration_account;

/* Owned by its socket. Other account sessions are visited only when newly
 * discovered bits must be broadcast, never to locate this session or flush it. */
typedef struct exploration_session {
    struct exploration_session *next, *prev;
    socket_struct *socket;
    exploration_account *account;
    exploration_map *last_map;
    exploration_pending *pending, *pending_index;
} exploration_session;

struct exploration_account {
    char *name, *path;
    exploration_map *maps;
    exploration_session *sessions;
    unsigned count;
    bool valid, dirty, error_reported;
    time_t last_save;
    UT_hash_handle hh;
};

static exploration_account *accounts;

#ifdef ATRINIK_TESTING
static exploration_test_stats stats;
#define TEST_COUNT(field, count) (stats.field += (count))
#else
#define TEST_COUNT(field, count) ((void)0)
#endif

static size_t bitmap_size(unsigned width, unsigned height) {
    return ((size_t)width * height + 7) / 8;
}

static bool valid_path(const char *path) {
    if (path == NULL || path[0] != '/' || strlen(path) > EXPLORATION_PATH_MAX || path[1] == '\0') {
        return false;
    }
    /* Paths are identities, never filesystem operands. Exclude physical/private
     * names and noncanonical traversal even when reading an operator-owned save. */
    const char *segment = path + 1;
    for (const unsigned char *p = (const unsigned char *)segment;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t len = (const char *)p - segment;
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*p == '\0') {
                return true;
            }
            segment = (const char *)p + 1;
        } else if (*p < 32 || *p == 127 || *p == '\\' || *p == ':') {
            return false;
        }
    }
}

static exploration_map *find_map(exploration_account *account, const char *path) {
    exploration_map *map;
    TEST_COUNT(map_lookups, 1);
    HASH_FIND_STR(account->maps, path, map);
    return map;
}

static void free_maps(exploration_account *account) {
    exploration_map *map, *next;
    HASH_ITER(hh, account->maps, map, next) {
        HASH_DEL(account->maps, map);
        free(map->path);
        free(map->bits);
        free(map);
    }
    account->count = 0;
}

static exploration_map *
add_map(exploration_account *account, const char *path, unsigned width, unsigned height) {
    if (account->count == EXPLORATION_MAPS_MAX) {
        return NULL;
    }
    exploration_map *map = xcalloc(1, sizeof(*map));
    map->path = xstrdup(path);
    map->width = width;
    map->height = height;
    map->bits = xcalloc(bitmap_size(width, height), 1);
    HASH_ADD_KEYPTR(hh, account->maps, map->path, strlen(map->path), map);
    account->count++;
    return map;
}

static bool read_u16(FILE *fp, unsigned *value) {
    uint8_t data[2];
    if (fread(data, 1, 2, fp) != 2) {
        return false;
    }
    *value = ((unsigned)data[0] << 8) | data[1];
    return true;
}

static bool load_account(exploration_account *account) {
    FILE *fp = fopen(account->path, "rb");
    if (fp == NULL) {
        return errno == ENOENT;
    }
    struct stat st;
    char magic[8];
    bool ok = fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 8 &&
              (uint64_t)st.st_size <= EXPLORATION_BYTES_MAX &&
              fread(magic, 1, sizeof(magic), fp) == sizeof(magic) &&
              memcmp(magic, EXPLORATION_MAGIC, sizeof(magic)) == 0;
    while (ok) {
        int first = fgetc(fp);
        if (first == EOF) {
            ok = !ferror(fp);
            break;
        }
        int second = fgetc(fp);
        unsigned length = ((unsigned)first << 8) | (unsigned)second;
        char path[EXPLORATION_PATH_MAX + 1];
        unsigned width, height;
        if (second == EOF || length == 0 || length > EXPLORATION_PATH_MAX ||
            fread(path, 1, length, fp) != length) {
            ok = false;
            break;
        }
        path[length] = '\0';
        if (strlen(path) != length || !valid_path(path) || find_map(account, path) != NULL ||
            !read_u16(fp, &width) || !read_u16(fp, &height) || width == 0 || height == 0 ||
            width > EXPLORATION_DIM_MAX || height > EXPLORATION_DIM_MAX) {
            ok = false;
            break;
        }
        exploration_map *map = add_map(account, path, width, height);
        TEST_COUNT(load_records, 1);
        size_t size = bitmap_size(width, height);
        if (map == NULL || fread(map->bits, 1, size, fp) != size ||
            ((width * height) % 8 != 0 && (map->bits[size - 1] >> ((width * height) % 8)) != 0)) {
            ok = false;
            break;
        }
    }
    fclose(fp);
    if (!ok) {
        free_maps(account);
    }
    return ok;
}

static void write_u16(uint8_t *data, unsigned value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static bool save_account(exploration_account *account) {
    if (!account->valid || !account->dirty) {
        return !account->dirty;
    }
    /* Allocate once: incrementally growing a large string buffer can copy the
     * entire account for each map, depending on the allocator. */
    size_t size = 8;
    for (exploration_map *map = account->maps; map != NULL; map = map->hh.next) {
        size += 6 + strlen(map->path) + bitmap_size(map->width, map->height);
    }
    uint8_t *data = xmalloc(size);
    memcpy(data, EXPLORATION_MAGIC, 8);
    size_t pos = 8;
    for (exploration_map *map = account->maps; map != NULL; map = map->hh.next) {
        size_t len = strlen(map->path), bytes = bitmap_size(map->width, map->height);
        write_u16(data + pos, len);
        pos += 2;
        memcpy(data + pos, map->path, len);
        pos += len;
        write_u16(data + pos, map->width);
        write_u16(data + pos + 2, map->height);
        pos += 4;
        memcpy(data + pos, map->bits, bytes);
        pos += bytes;
    }
    TEST_COUNT(save_records, account->count);
    TEST_COUNT(save_bytes, size);
    bool ok = path_write_atomic(account->path, data, size, 0600);
    free(data);
    if (ok) {
        account->dirty = false;
        account->error_reported = false;
    } else if (!account->error_reported) {
        LOG(ERROR, "Could not save account exploration: %s", account->path);
        account->error_reported = true;
    }
    account->last_save = time(NULL);
    return ok;
}

static exploration_pending *
pending_get(exploration_session *session, const char *path, exploration_map *map) {
    exploration_pending *pending;
    HASH_FIND_STR(session->pending_index, path, pending);
    if (pending == NULL) {
        if (HASH_COUNT(session->pending_index) >= EXPLORATION_MAPS_MAX) {
            return NULL;
        }
        pending = xcalloc(1, sizeof(*pending));
        pending->path = xstrdup(path);
        HASH_ADD_KEYPTR(hh, session->pending_index, pending->path, strlen(path), pending);
        DL_APPEND(session->pending, pending);
    }
    if (map != NULL && pending->map == NULL) {
        pending->map = map;
        pending->bits = xcalloc(bitmap_size(map->width, map->height), 1);
    }
    return pending;
}

static void pending_free(exploration_session *session, exploration_pending *pending) {
    HASH_DEL(session->pending_index, pending);
    DL_DELETE(session->pending, pending);
    free(pending->path);
    free(pending->bits);
    free(pending);
}

static void send_pending(socket_struct *ns, const exploration_pending *pending) {
    packet_struct *packet = packet_new(CLIENT_CMD_REGION_EXPLORATION, 0, 256);
    const exploration_map *map = pending->map;
    if (map == NULL) {
        packet_writer_write_uint8(packet, 3);
        packet_writer_write_cstring(packet, pending->path);
    } else {
        size_t bytes = bitmap_size(map->width, map->height);
        unsigned changed = 0;
        for (size_t i = 0; i < bytes; i++) {
            changed += pending->bits[i] != 0;
        }
        bool patch = 2 + (size_t)changed * 3 < bytes;
        packet_writer_write_uint8(packet, patch ? 2 : 1);
        packet_writer_write_cstring(packet, map->path);
        packet_writer_write_uint16(packet, map->width);
        packet_writer_write_uint16(packet, map->height);
        if (patch) {
            packet_writer_write_uint16(packet, changed);
            for (size_t i = 0; i < bytes; i++) {
                if (pending->bits[i] != 0) {
                    packet_writer_write_uint16(packet, i);
                    packet_writer_write_uint8(packet, pending->bits[i]);
                }
            }
        } else {
            packet_writer_write_bytes(packet, map->bits, bytes);
        }
    }
    TEST_COUNT(sent_records, 1);
    socket_send_packet(ns, packet);
}

void exploration_begin(socket_struct *ns) {
    if (ns == NULL || ns->account == NULL) {
        return;
    }
    exploration_end(ns);
    exploration_account *account;
    TEST_COUNT(account_lookups, 1);
    HASH_FIND_STR(accounts, ns->account, account);
    if (account == NULL) {
        account = xcalloc(1, sizeof(*account));
        account->name = xstrdup(ns->account);
        char *base = account_make_path(ns->account);
        account->path = xmalloc(strlen(base) + sizeof(".exploration"));
        sprintf(account->path, "%s.exploration", base);
        free(base);
        account->valid = load_account(account);
        if (!account->valid) {
            LOG(ERROR,
                "Invalid account exploration preserved without modification: %s",
                account->path);
        }
        HASH_ADD_KEYPTR(hh, accounts, account->name, strlen(account->name), account);
    }
    exploration_session *session = xcalloc(1, sizeof(*session));
    session->socket = ns;
    session->account = account;
    DL_APPEND(account->sessions, session);
    ns->exploration = session;
    packet_struct *packet = packet_new(CLIENT_CMD_REGION_EXPLORATION, 0, 64);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_cstring(packet, ns->account);
    socket_send_packet(ns, packet);
    /* The client requests only maps in its region, with any cached bits. */
}

bool exploration_mark(socket_struct *ns,
                      const char *path,
                      unsigned width,
                      unsigned height,
                      unsigned x,
                      unsigned y) {
    exploration_session *session = ns != NULL ? ns->exploration : NULL;
    if (session == NULL || !session->account->valid || path == NULL || width == 0 || height == 0 ||
        width > EXPLORATION_DIM_MAX || height > EXPLORATION_DIM_MAX) {
        return false;
    }
    exploration_account *account = session->account;
    exploration_map *map = session->last_map;
    if (map == NULL || strcmp(map->path, path) != 0) {
        if (!valid_path(path)) {
            return false;
        }
        map = find_map(account, path);
    }
    if (map == NULL) {
        if (x >= width || y >= height) {
            return false;
        }
        map = add_map(account, path, width, height);
        if (map == NULL) {
            return false;
        }
    }
    session->last_map = map;
    /* The initial dimensions define the bitfield layout. Do not compare map
     * revisions or replace stored exploration when authored geometry changes. */
    if (x >= map->width || y >= map->height) {
        return false;
    }
    size_t bit = (size_t)y * map->width + x;
    uint8_t mask = 1U << (bit % 8);
    if (map->bits[bit / 8] & mask) {
        return false;
    }
    map->bits[bit / 8] |= mask;
    account->dirty = true;
    for (session = account->sessions; session != NULL; session = session->next) {
        TEST_COUNT(broadcast_sessions, 1);
        exploration_pending *pending = pending_get(session, map->path, map);
        if (pending != NULL) {
            pending->bits[bit / 8] |= mask;
        } else {
            /* A client that exhausts the bounded reconciliation queue must
             * reconnect rather than silently miss authoritative live bits. */
            session->socket->state = ST_ZOMBIE;
        }
    }
    return true;
}

void socket_command_region_exploration(socket_struct *ns,
                                       player *pl,
                                       uint8_t *data,
                                       size_t len,
                                       size_t pos) {
    (void)pl;
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    uint8_t operation = packet_reader_read_uint8(&reader);
    char path[EXPLORATION_PATH_MAX + 1];
    packet_reader_read_string(&reader, path, sizeof(path));
    uint16_t count = packet_reader_read_uint16(&reader);
    packet_view_t cached = packet_reader_read_view(&reader, count);
    if (!packet_reader_finish(&reader)) {
        return;
    }
    if (operation != 0 || !valid_path(path) || count > 8192) {
        packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        return;
    }
    exploration_session *session = ns != NULL ? ns->exploration : NULL;
    if (session == NULL || ns->state != ST_PLAYING || ns->account == NULL) {
        packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        return;
    }
    exploration_map *map = find_map(session->account, path);
    if (map == NULL) {
        if (pending_get(session, path, NULL) == NULL) {
            packet_reader_set_error(&reader, PACKET_ERROR_LIMIT_EXCEEDED);
        }
        return;
    }
    size_t bytes = bitmap_size(map->width, map->height);
    unsigned tail = (map->width * map->height) % 8;
    if ((count != 0 && count != bytes) ||
        (count != 0 && tail != 0 && (cached.data[count - 1] >> tail) != 0)) {
        packet_reader_set_error(&reader, PACKET_ERROR_INVALID_ENCODING);
        return;
    }
    exploration_pending *pending = NULL;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t missing = map->bits[i] & (count != 0 ? (uint8_t)~cached.data[i] : UINT8_MAX);
        if (missing == 0) {
            continue;
        }
        if (pending == NULL) {
            pending = pending_get(session, path, map);
            if (pending == NULL) {
                packet_reader_set_error(&reader, PACKET_ERROR_LIMIT_EXCEEDED);
                return;
            }
        }
        pending->bits[i] |= missing;
    }
}

void exploration_flush(socket_struct *ns, bool force) {
    exploration_session *session = ns != NULL ? ns->exploration : NULL;
    if (session == NULL) {
        return;
    }
    unsigned sent = 0;
    while (session->pending != NULL && sent < 32 && ns->packet_queue_bytes <= 1024U * 1024U &&
           ns->packet_queue_count <= 1024U && ns->state != ST_DEAD && ns->state != ST_ZOMBIE) {
        exploration_pending *pending = session->pending;
        TEST_COUNT(pending_visits, 1);
        send_pending(ns, pending);
        pending_free(session, pending);
        sent++;
    }
    exploration_account *account = session->account;
    time_t now = time(NULL);
    if (account->dirty && (force || now < account->last_save || now - account->last_save >= 5)) {
        save_account(account);
    }
}

static void free_account(exploration_account *account) {
    HASH_DEL(accounts, account);
    free_maps(account);
    free(account->name);
    free(account->path);
    free(account);
}

static void free_session(exploration_session *session) {
    while (session->pending != NULL) {
        pending_free(session, session->pending);
    }
    session->socket->exploration = NULL;
    DL_DELETE(session->account->sessions, session);
    free(session);
}

bool exploration_end_checked(socket_struct *ns) {
    exploration_session *session = ns != NULL ? ns->exploration : NULL;
    if (session == NULL) {
        return true;
    }
    exploration_account *account = session->account;
    free_session(session);
    bool saved = save_account(account);
    if (account->sessions == NULL && !account->dirty) {
        free_account(account);
    }
    return saved;
}

void exploration_end(socket_struct *ns) {
    (void)exploration_end_checked(ns);
}

bool exploration_shutdown_checked(void) {
    bool saved = true;
    while (accounts != NULL) {
        exploration_account *account = accounts;
        if (!save_account(account)) {
            saved = false;
        }
        while (account->sessions != NULL) {
            free_session(account->sessions);
        }
        free_account(account);
    }
    return saved;
}

void exploration_shutdown(void) {
    (void)exploration_shutdown_checked();
}

#ifdef ATRINIK_TESTING
bool exploration_visited(socket_struct *ns, const char *path, unsigned x, unsigned y) {
    exploration_session *session = ns != NULL ? ns->exploration : NULL;
    exploration_map *map = session != NULL ? find_map(session->account, path) : NULL;
    if (map == NULL || x >= map->width || y >= map->height) {
        return false;
    }
    size_t bit = (size_t)y * map->width + x;
    return (map->bits[bit / 8] & (1U << (bit % 8))) != 0;
}

void exploration_stats_reset(void) {
    memset(&stats, 0, sizeof(stats));
}

exploration_test_stats exploration_stats_get(void) {
    return stats;
}
#endif
