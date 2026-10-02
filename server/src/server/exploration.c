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
#include <toolkit/stringbuffer.h>

#define EXPLORATION_MAPS_MAX 4096U
#define EXPLORATION_DIM_MAX 256U
#define EXPLORATION_PATH_MAX 255U
#define EXPLORATION_BYTES_MAX (36U * 1024U * 1024U)
#define EXPLORATION_MAGIC "AEXP0001"

typedef struct exploration_map {
    struct exploration_map *next;
    char *path;
    unsigned width, height;
    uint8_t *bits;
    unsigned index;
    uint64_t revision;
} exploration_map;

typedef struct exploration_session {
    struct exploration_session *next;
    socket_struct *socket;
    uint64_t seen[EXPLORATION_MAPS_MAX];
} exploration_session;

typedef struct exploration_account {
    struct exploration_account *next;
    char *name, *path;
    exploration_map *maps;
    exploration_session *sessions;
    unsigned count;
    bool valid, dirty, error_reported;
    time_t last_save;
} exploration_account;

static exploration_account *accounts;

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
    for (exploration_map *map = account->maps; map != NULL; map = map->next) {
        if (strcmp(map->path, path) == 0) {
            return map;
        }
    }
    return NULL;
}

static exploration_account *find_account(socket_struct *ns) {
    for (exploration_account *account = accounts; account != NULL; account = account->next) {
        for (exploration_session *session = account->sessions; session != NULL;
             session = session->next) {
            if (session->socket == ns) {
                return account;
            }
        }
    }
    return NULL;
}

static void free_maps(exploration_account *account) {
    while (account->maps != NULL) {
        exploration_map *map = account->maps;
        account->maps = map->next;
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
    map->next = account->maps;
    account->maps = map;
    map->index = account->count++;
    map->revision = 1;
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

static void append_u16(StringBuffer *buffer, unsigned value) {
    uint8_t data[2] = {value >> 8, value & 255};
    stringbuffer_append_string_len(buffer, (const char *)data, 2);
}

static bool save_account(exploration_account *account) {
    if (!account->valid || !account->dirty) {
        return !account->dirty;
    }
    StringBuffer *buffer = stringbuffer_new();
    stringbuffer_append_string_len(buffer, EXPLORATION_MAGIC, 8);
    size_t size = 8;
    for (exploration_map *map = account->maps; map != NULL; map = map->next) {
        size_t len = strlen(map->path), bytes = bitmap_size(map->width, map->height);
        append_u16(buffer, len);
        stringbuffer_append_string_len(buffer, map->path, len);
        append_u16(buffer, map->width);
        append_u16(buffer, map->height);
        stringbuffer_append_string_len(buffer, (const char *)map->bits, bytes);
        size += 6 + len + bytes;
    }
    char *data = stringbuffer_finish(buffer);
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

static void send_map(socket_struct *ns, const exploration_map *map) {
    packet_struct *packet = packet_new(CLIENT_CMD_REGION_EXPLORATION, 0, 256);
    packet_writer_write_uint8(packet, 1);
    packet_writer_write_cstring(packet, map->path);
    packet_writer_write_uint16(packet, map->width);
    packet_writer_write_uint16(packet, map->height);
    packet_writer_write_bytes(packet, map->bits, bitmap_size(map->width, map->height));
    socket_send_packet(ns, packet);
}

void exploration_begin(socket_struct *ns) {
    if (ns == NULL || ns->account == NULL) {
        return;
    }
    exploration_end(ns);
    exploration_account *account;
    for (account = accounts; account != NULL; account = account->next) {
        if (strcmp(account->name, ns->account) == 0) {
            break;
        }
    }
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
        account->next = accounts;
        accounts = account;
    }
    exploration_session *session = xcalloc(1, sizeof(*session));
    session->socket = ns;
    session->next = account->sessions;
    account->sessions = session;
    packet_struct *packet = packet_new(CLIENT_CMD_REGION_EXPLORATION, 1, 0);
    packet_writer_write_uint8(packet, 0);
    socket_send_packet(ns, packet);
    exploration_flush(ns, false);
}

bool exploration_mark(socket_struct *ns,
                      const char *path,
                      unsigned width,
                      unsigned height,
                      unsigned x,
                      unsigned y) {
    exploration_account *account = find_account(ns);
    if (account == NULL || !account->valid || !valid_path(path) || width == 0 || height == 0 ||
        width > EXPLORATION_DIM_MAX || height > EXPLORATION_DIM_MAX || x >= width || y >= height) {
        return false;
    }
    exploration_map *map = find_map(account, path);
    if (map == NULL) {
        map = add_map(account, path, width, height);
        if (map == NULL) {
            return false;
        }
    } else if (map->width != width || map->height != height) {
        /* Authored geometry changed. Old coordinates have no safe meaning. */
        free(map->bits);
        map->bits = xcalloc(bitmap_size(width, height), 1);
        map->width = width;
        map->height = height;
    }
    size_t bit = (size_t)y * width + x;
    uint8_t mask = 1U << (bit % 8);
    if (map->bits[bit / 8] & mask) {
        return false;
    }
    map->bits[bit / 8] |= mask;
    map->revision++;
    account->dirty = true;
    return true;
}

void exploration_flush(socket_struct *ns, bool force) {
    exploration_account *account = find_account(ns);
    if (account == NULL) {
        return;
    }
    /* A full account can exceed the transport's 4 MiB queue. Stream bounded
     * batches with gameplay headroom and retry unsent revisions next draw. */
    for (exploration_session *session = account->sessions; session != NULL;
         session = session->next) {
        unsigned sent = 0;
        for (exploration_map *map = account->maps; map != NULL; map = map->next) {
            if (session->seen[map->index] == map->revision) {
                continue;
            }
            if (sent == 32 || session->socket->packet_queue_bytes > 1024U * 1024U ||
                session->socket->packet_queue_count > 1024U) {
                break;
            }
            send_map(session->socket, map);
            session->seen[map->index] = map->revision;
            sent++;
        }
    }
    time_t now = time(NULL);
    if (account->dirty && (force || now < account->last_save || now - account->last_save >= 5)) {
        save_account(account);
    }
}

void exploration_end(socket_struct *ns) {
    exploration_account **link = &accounts;
    while (*link != NULL) {
        exploration_account *account = *link;
        exploration_session **session_link = &account->sessions;
        while (*session_link != NULL && (*session_link)->socket != ns) {
            session_link = &(*session_link)->next;
        }
        if (*session_link == NULL) {
            link = &account->next;
            continue;
        }
        exploration_session *session = *session_link;
        *session_link = session->next;
        free(session);
        save_account(account);
        if (account->sessions == NULL && !account->dirty) {
            *link = account->next;
            free_maps(account);
            free(account->name);
            free(account->path);
            free(account);
        }
        return;
    }
}

void exploration_shutdown(void) {
    while (accounts != NULL) {
        exploration_account *account = accounts;
        accounts = account->next;
        save_account(account);
        while (account->sessions != NULL) {
            exploration_session *session = account->sessions;
            account->sessions = session->next;
            free(session);
        }
        free_maps(account);
        free(account->name);
        free(account->path);
        free(account);
    }
}

#ifdef ATRINIK_TESTING
bool exploration_visited(socket_struct *ns, const char *path, unsigned x, unsigned y) {
    exploration_account *account = find_account(ns);
    exploration_map *map = account != NULL ? find_map(account, path) : NULL;
    if (map == NULL || x >= map->width || y >= map->height) {
        return false;
    }
    size_t bit = (size_t)y * map->width + x;
    return (map->bits[bit / 8] & (1U << (bit % 8))) != 0;
}
#endif
