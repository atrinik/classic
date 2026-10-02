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
#include <region_exploration.h>
#include <toolkit/path.h>
#include <toolkit/uthash.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPLORATION_MAP_LIMIT 10000U
#define EXPLORATION_PACKET_MAX (1U + 256U + 4U + 8192U)

typedef struct exploration_record {
    char path[256];
    unsigned width, height;
    uint8_t *bits;
    UT_hash_handle hh;
} exploration_record_t;
typedef struct exploration_request {
    char path[256];
    struct exploration_request *next;
    UT_hash_handle hh;
} exploration_request_t;
static exploration_record_t *records;
static exploration_request_t *requests, *pending, *pending_tail;
static char connection[65], cache_key[65];
static char *cache_directory, *cache_path;
static bool active, dirty;

static void requests_clear(void) {
    exploration_request_t *entry, *next;
    HASH_ITER(hh, requests, entry, next) {
        HASH_DEL(requests, entry);
        free(entry);
    }
    pending = pending_tail = NULL;
}
static void records_clear(void) {
    exploration_record_t *entry, *next;
    HASH_ITER(hh, records, entry, next) {
        HASH_DEL(records, entry);
        free(entry->bits);
        free(entry);
    }
}

/* Paths are opaque logical identifiers, never filesystem paths. */
static bool logical_path(const char *path) {
    if (path == NULL || path[0] != '/' || strlen(path) > 255) {
        return false;
    }
    const char *segment = path + 1;
    for (const char *p = segment;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 32 || c == 127 || c == '\\' || c == ':')) {
            return false;
        }
        if (!c || c == '/') {
            size_t n = (size_t)(p - segment);
            if (!n || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            segment = p + 1;
        }
        if (!c) {
            return true;
        }
    }
}
static unsigned read_u16(const uint8_t *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static void write_u16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}
static size_t record_encode(const exploration_record_t *entry, uint8_t *data) {
    data[0] = 1;
    size_t pos = strlen(entry->path) + 2;
    memcpy(data + 1, entry->path, pos - 1);
    write_u16(data + pos, entry->width);
    write_u16(data + pos + 2, entry->height);
    size_t bytes = ((size_t)entry->width * entry->height + 7) / 8;
    memcpy(data + pos + 4, entry->bits, bytes);
    return pos + 4 + bytes;
}
static void cache_save(void) {
    if (!dirty || cache_path == NULL) {
        return;
    }
    size_t size = 68;
    exploration_record_t *entry, *next;
    HASH_ITER(hh, records, entry, next) {
        size += 2 + strlen(entry->path) + 6 + ((size_t)entry->width * entry->height + 7) / 8;
    }
    uint8_t *data = malloc(size);
    if (data == NULL) {
        return;
    }
    memcpy(data, "RXC2", 4);
    memcpy(data + 4, cache_key, 64);
    size_t pos = 68;
    HASH_ITER(hh, records, entry, next) {
        size_t len = record_encode(entry, data + pos + 2);
        write_u16(data + pos, (unsigned)len);
        pos += 2 + len;
    }
    if (path_write_atomic_existing(cache_path, data, size, 0600)) {
        dirty = false;
    }
    free(data);
}
static void cache_load(void) {
    if (cache_path == NULL) {
        return;
    }
    FILE *file = fopen(cache_path, "rb");
    if (file == NULL) {
        return;
    }
    uint8_t header[68], packet[EXPLORATION_PACKET_MAX];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
              memcmp(header, "RXC2", 4) == 0 && memcmp(header + 4, cache_key, 64) == 0;
    size_t count = 0;
    while (ok) {
        uint8_t length[2] = {0};
        size_t n = fread(length, 1, 2, file);
        if (n == 0 && feof(file)) {
            break;
        }
        unsigned len = read_u16(length);
        if (n != 2 || ++count > EXPLORATION_MAP_LIMIT || len == 0 || len > sizeof(packet) ||
            fread(packet, 1, len, file) != len || packet[0] != 1 ||
            !region_exploration_receive(packet, len, NULL)) {
            ok = false;
        }
    }
    if (ferror(file)) {
        ok = false;
    }
    fclose(file);
    if (!ok) {
        records_clear(); /* A corrupt optional cache is a complete miss. */
    }
    dirty = false;
}

void region_exploration_disconnect(void) {
    cache_save();
    active = false;
    requests_clear();
}
void region_exploration_clear(void) {
    region_exploration_disconnect();
    records_clear();
    free(cache_directory);
    free(cache_path);
    cache_directory = cache_path = NULL;
    connection[0] = cache_key[0] = '\0';
    dirty = false;
}

void region_exploration_connect(const char *certificate, const char *directory) {
    region_exploration_disconnect();
    connection[0] = '\0';
    free(cache_directory);
    cache_directory = NULL;
    if (certificate == NULL || strlen(certificate) != 64) {
        return;
    }
    for (size_t i = 0; i < 64; i++) {
        char c = certificate[i];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c + ('a' - 'A'));
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            connection[0] = '\0';
            return;
        }
        connection[i] = c;
    }
    connection[64] = '\0';
    if (directory != NULL) {
        cache_directory = malloc(strlen(directory) + 1);
        if (cache_directory != NULL) {
            strcpy(cache_directory, directory);
        }
    }
}
static bool account_select(const uint8_t *data, size_t len) {
    /* RESET is trusted only inside the authenticated, pinned connection. */
    if (connection[0] == '\0' || len < 3 || len > 257 || data[len - 1] != 0) {
        return false;
    }
    if (memchr(data + 1, 0, len - 2) != NULL) {
        return false;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned digest_len;
    uint8_t identity[321];
    memcpy(identity, connection, 65);
    memcpy(identity + 65, data + 1, len - 1);
    if (EVP_Digest(identity, 65 + len - 1, digest, &digest_len, EVP_sha256(), NULL) != 1 ||
        digest_len != 32) {
        return false;
    }
    char key[65];
    for (size_t i = 0; i < 32; i++) {
        static const char hex[] = "0123456789abcdef";
        key[i * 2] = hex[digest[i] >> 4];
        key[i * 2 + 1] = hex[digest[i] & 15];
    }
    key[64] = 0;
    requests_clear();
    if (strcmp(cache_key, key) == 0) {
        active = true;
        return true;
    }
    cache_save();
    records_clear();
    free(cache_path);
    cache_path = NULL;
    dirty = false;
    memcpy(cache_key, key, sizeof(key));
    if (cache_directory != NULL) {
        char name[80];
        snprintf(name, sizeof(name), "exploration-%s.bin", key);
        cache_path = path_join(cache_directory, name);
    }
    active = true;
    cache_load();
    return true;
}

const uint8_t *region_exploration_find(const char *path, unsigned *width, unsigned *height) {
    exploration_record_t *entry;
    if (!active) {
        return NULL;
    }
    HASH_FIND_STR(records, path, entry);
    if (entry == NULL) {
        return NULL;
    }
    *width = entry->width;
    *height = entry->height;
    return entry->bits;
}

bool region_exploration_receive(const uint8_t *data, size_t len, bool *changed) {
    if (changed != NULL) {
        *changed = false;
    }
    if (data == NULL || len == 0) {
        return false;
    }
    if (data[0] == 0) {
        bool ok = account_select(data, len);
        if (changed != NULL) {
            *changed = ok;
        }
        return ok;
    }
    if (!active || data[0] > 3 || len < 4) {
        return false;
    }
    const uint8_t *end = memchr(data + 1, 0, len - 1 < 256 ? len - 1 : 256);
    if (end == NULL || !logical_path((const char *)data + 1)) {
        return false;
    }
    size_t offset = (size_t)(end - data) + 1;
    if (data[0] == 3) {
        return len == offset; /* EMPTY does not erase previously received bits. */
    }
    if (len - offset < 4) {
        return false;
    }
    unsigned width = read_u16(data + offset), height = read_u16(data + offset + 2);
    if (width == 0 || height == 0 || width > 256 || height > 256) {
        return false;
    }
    offset += 4;
    size_t bytes = ((size_t)width * height + 7) / 8;
    unsigned tail = (width * height) % 8;
    if (data[0] == 1) {
        if (len - offset != bytes || (tail && (data[len - 1] & (uint8_t)(0xffU << tail)))) {
            return false;
        }
    } else {
        if (len - offset < 2) {
            return false;
        }
        unsigned count = read_u16(data + offset);
        offset += 2;
        if (count == 0 || count > bytes || len - offset != (size_t)count * 3) {
            return false;
        }
        unsigned previous = 0;
        for (unsigned i = 0; i < count; i++) {
            unsigned index = read_u16(data + offset + i * 3);
            uint8_t mask = data[offset + i * 3 + 2];
            if (index >= bytes || (i && index <= previous) || mask == 0 ||
                (tail && index + 1 == bytes && (mask & (uint8_t)(0xffU << tail)))) {
                return false;
            }
            previous = index;
        }
    }
    exploration_record_t *entry;
    HASH_FIND_STR(records, (const char *)data + 1, entry);
    if (entry == NULL) {
        if (HASH_COUNT(records) >= EXPLORATION_MAP_LIMIT) {
            return false;
        }
        entry = calloc(1, sizeof(*entry));
        if (entry == NULL) {
            return false;
        }
        entry->bits = calloc(bytes, 1);
        if (entry->bits == NULL) {
            free(entry);
            return false;
        }
        memcpy(entry->path, data + 1, (size_t)(end - data));
        entry->width = width;
        entry->height = height;
        HASH_ADD_STR(records, path, entry);
        dirty = true;
    }
    /* Dimensions are metadata from the first observation, not an authored-map
     * consistency contract. Retain them and merge the bounded byte prefix. */
    size_t stored = ((size_t)entry->width * entry->height + 7) / 8;
    bool any = false;
    size_t count = data[0] == 1 ? bytes : (len - offset) / 3;
    unsigned stored_tail = (entry->width * entry->height) % 8;
    for (size_t i = 0; i < count; i++) {
        size_t index = data[0] == 1 ? i : read_u16(data + offset + i * 3);
        uint8_t mask = data[0] == 1 ? data[offset + i] : data[offset + i * 3 + 2];
        if (index >= stored) {
            continue;
        }
        if (stored_tail && index + 1 == stored) {
            mask &= (uint8_t)((1U << stored_tail) - 1);
        }
        any |= (entry->bits[index] | mask) != entry->bits[index];
        entry->bits[index] |= mask;
    }
    dirty |= any;
    if (changed != NULL) {
        *changed = any;
    }
    return true;
}

bool region_exploration_request(const char *path) {
    if (!active || !logical_path(path)) {
        return false;
    }
    exploration_request_t *entry;
    HASH_FIND_STR(requests, path, entry);
    if (entry != NULL) {
        return true;
    }
    if (HASH_COUNT(requests) >= EXPLORATION_MAP_LIMIT) {
        return false;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return false;
    }
    strcpy(entry->path, path);
    HASH_ADD_STR(requests, path, entry);
    if (pending_tail != NULL) {
        pending_tail->next = entry;
    } else {
        pending = entry;
    }
    pending_tail = entry;
    return true;
}
size_t region_exploration_service(region_exploration_send_fn send, void *user) {
    size_t count = 0;
    while (active && pending != NULL && count < 32) {
        uint8_t data[EXPLORATION_PACKET_MAX];
        data[0] = 0;
        size_t pos = strlen(pending->path) + 2;
        memcpy(data + 1, pending->path, pos - 1);
        unsigned width, height;
        const uint8_t *bits = region_exploration_find(pending->path, &width, &height);
        size_t bytes = bits != NULL ? ((size_t)width * height + 7) / 8 : 0;
        write_u16(data + pos, (unsigned)bytes);
        pos += 2;
        if (bytes) {
            memcpy(data + pos, bits, bytes);
        }
        if (!send(data, pos + bytes, user)) {
            break;
        }
        pending = pending->next;
        if (pending == NULL) {
            pending_tail = NULL;
        }
        count++;
    }
    return count;
}
