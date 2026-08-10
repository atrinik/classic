/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Serves an immutable, startup-cached game asset snapshot over QUIC.
 */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <initialization.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>
#include <toolkit/datetime.h>
#include <resources.h>
#include <network_metrics.h>
#include <openssl/evp.h>

#define ASSET_CACHE_MAX_TOTAL (1024ULL * 1024ULL * 1024ULL)
#define ASSET_RATE_BYTES_PER_SECOND (8U * 1024U * 1024U)
#define ASSET_RATE_REQUESTS_PER_SECOND SOCKET_ASSET_REQUEST_RATE_MAX
#define ASSET_TOKEN_BUCKET_CAPACITY ASSET_RATE_BYTES_PER_SECOND
#define ASSET_STREAM_ACCEPT_QUANTUM 16U
/* Bound all connections together to the configured byte rate and a fixed
 * number of accept/read/write operations per game-loop iteration. */
#define ASSET_GLOBAL_WORK_PER_TICK 512U
/* A server loop normally runs every 125 ms. Two request-read rounds (payload
 * and FIN) are followed by one header and up to four 16 KiB body writes per
 * batch member. Permit an entire maximum batch to progress in one invocation
 * when QUIC, byte pacing, and the server-wide work budget allow it. Those
 * existing bounds remain the authoritative caps across connections. */
#define ASSET_FACE_BODY_SERVICE_ROUNDS \
    ((ASSET_FACE_MAX_SIZE + ASSET_STREAM_QUANTUM - 1U) / ASSET_STREAM_QUANTUM)
#define ASSET_FACE_MEMBER_SERVICE_ROUNDS (1U + ASSET_FACE_BODY_SERVICE_ROUNDS)
#define ASSET_FACE_BATCH_REQUEST_SERVICE_ROUNDS 2U
#define ASSET_FACE_BATCH_SERVICE_ROUNDS_REQUIRED \
    (ASSET_FACE_BATCH_REQUEST_SERVICE_ROUNDS +   \
     ASSET_FACE_BATCH_MAX * ASSET_FACE_MEMBER_SERVICE_ROUNDS)
#define ASSET_STREAM_SERVICE_ROUNDS ASSET_FACE_BATCH_SERVICE_ROUNDS_REQUIRED
/* One saturated connection may send a complete maximum batch in a normal
 * tick, but cannot consume the entire aggregate allowance before peers run. */
#define ASSET_CONNECTION_BYTES_PER_PASS \
    (ASSET_FACE_BATCH_MAX_SIZE + ASSET_FACE_BATCH_MAX * SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE)
#define ASSET_CONNECTION_WORK_PER_PASS \
    (1U + ASSET_STREAM_ACCEPT_QUANTUM + ASSET_STREAM_ACTIVE_MAX * ASSET_STREAM_SERVICE_ROUNDS)

CASSERT(ASSET_FACE_BATCH_MAX_SIZE == ASSET_FACE_BATCH_MAX * ASSET_FACE_MAX_SIZE);
CASSERT(ASSET_STREAM_SERVICE_ROUNDS >= ASSET_FACE_BATCH_SERVICE_ROUNDS_REQUIRED);
CASSERT(SOCKET_ASSET_REQUEST_MAX_SIZE >= SOCKET_FACE_BATCH_REQUEST_MAX_SIZE);
CASSERT(ASSET_GLOBAL_WORK_PER_TICK >= 2U * (ASSET_CONNECTION_WORK_PER_PASS + 1U));

typedef struct asset_cache_entry {
    UT_hash_handle hh;
    char *name;
    uint8_t *data;
    uint32_t size;
    uint8_t digest[ASSET_DIGEST_SIZE];
    size_t references;
} asset_cache_entry_t;

typedef enum asset_server_stream_state {
    ASSET_SERVER_READ_REQUEST,
    ASSET_SERVER_SEND_HEADER,
    ASSET_SERVER_SEND_BODY,
} asset_server_stream_state_t;

struct asset_stream_state {
    struct asset_stream_state *next;
    struct asset_stream_state *prev;
    socket_stream_t *stream;
    socket_stream_kind_t kind;
    asset_cache_entry_t *entry;
    const uint8_t *body;
    uint32_t body_size;
    asset_server_stream_state_t state;
    uint64_t started_us;
    uint8_t request[SOCKET_ASSET_REQUEST_MAX_SIZE];
    size_t request_size;
    packet_struct *header;
    size_t header_pos;
    size_t body_pos;
    uint16_t batch_faces[ASSET_FACE_BATCH_MAX];
    uint8_t batch_count;
    uint8_t batch_pos;
    bool concluded;
};

typedef struct asset_service_budget {
    size_t bytes;
    size_t work;
} asset_service_budget_t;

static asset_cache_entry_t *asset_cache;
static uint64_t asset_cache_size;
static size_t asset_cache_rss;
static socket_struct *asset_connections;
static socket_struct *asset_connections_tail;
static socket_struct *asset_service_cursor;
static size_t asset_connection_count;
static uint64_t asset_service_generation;

size_t socket_assets_tick_byte_budget(void) {
    long multiplier = MAX((long)max_time_multiplier, 1L);
    uint64_t duration_us = MIN((uint64_t)MAX(max_time / multiplier, 1L), UINT64_C(1000000));
    uint64_t bytes = (uint64_t)ASSET_RATE_BYTES_PER_SECOND * duration_us / UINT64_C(1000000);
    return (size_t)MAX(bytes, UINT64_C(1));
}

size_t socket_assets_connection_pass_byte_budget(size_t remaining) {
    return MIN(remaining, (size_t)ASSET_CONNECTION_BYTES_PER_PASS);
}

size_t socket_assets_face_batch_service_rounds(const uint32_t *body_sizes, size_t count) {
    if (body_sizes == NULL || count == 0 || count > ASSET_FACE_BATCH_MAX) {
        return 0;
    }
    size_t rounds = ASSET_FACE_BATCH_REQUEST_SERVICE_ROUNDS;
    for (size_t i = 0; i < count; i++) {
        if (body_sizes[i] > ASSET_FACE_MAX_SIZE) {
            return 0;
        }
        rounds += 1U + (body_sizes[i] + ASSET_STREAM_QUANTUM - 1U) / ASSET_STREAM_QUANTUM;
    }
    return rounds;
}

static bool asset_simple_name(const char *name) {
    return name != NULL && *name != '\0' && strchr(name, '/') == NULL &&
           strchr(name, '\\') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool asset_resolve_path(const char *asset, char *path, size_t path_size) {
    if (strcmp(asset, "data/listing.txt") == 0) {
        return snprintf(path, path_size, "%s/data/listing.txt", settings.httppath) < (int)path_size;
    }

    if (string_startswith(asset, "data/")) {
        const char *name = asset + sizeof("data/") - 1;
        size_t length = strlen(name);
        if (!asset_simple_name(name) || length <= sizeof(".zz") - 1 ||
            strcmp(name + length - (sizeof(".zz") - 1), ".zz") != 0) {
            return false;
        }
        return snprintf(path, path_size, "%s/data/%s", settings.httppath, name) < (int)path_size;
    }

    if (string_startswith(asset, "resources/")) {
        const char *name = asset + sizeof("resources/") - 1;
        if (resources_find(name) == NULL) {
            return false;
        }
        return snprintf(path, path_size, "%s/%s", settings.resourcespath, name) < (int)path_size;
    }

    if (string_startswith(asset, "client-maps/")) {
        const char *name = asset + sizeof("client-maps/") - 1;
        size_t length = strlen(name);
        bool extension = length > 4 && (strcmp(name + length - 4, ".png") == 0 ||
                                        strcmp(name + length - 4, ".def") == 0);
        if (!asset_simple_name(name) || !extension) {
            return false;
        }
        return snprintf(path, path_size, "%s/client-maps/%s", settings.httppath, name) <
               (int)path_size;
    }

    return false;
}

static bool asset_cache_add(const char *name, const char *path) {
#ifdef O_NOFOLLOW
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    FILE *fp = fd >= 0 ? fdopen(fd, "rb") : NULL;
    if (fd >= 0 && fp == NULL) {
        close(fd);
    }
#else
    FILE *fp = fopen(path, "rb");
#endif
    struct stat sb;
    if (fp == NULL || fstat(fileno(fp), &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0 ||
        (uint64_t)sb.st_size > ASSET_MAX_SIZE ||
        asset_cache_size + (uint64_t)sb.st_size > ASSET_CACHE_MAX_TOTAL) {
        if (fp != NULL) {
            fclose(fp);
        }
        LOG(ERROR, "Cannot cache game asset %s from %s", name, path);
        return false;
    }

    asset_cache_entry_t *entry = xcalloc(1, sizeof(*entry));
    entry->name = xstrdup(name);
    entry->size = (uint32_t)sb.st_size;
    entry->data = xmalloc(MAX((size_t)entry->size, (size_t)1));
    bool ok = fread(entry->data, 1, entry->size, fp) == entry->size;
    if (fclose(fp) != 0) {
        ok = false;
    }
    unsigned int digest_size = 0;
    ok = ok &&
         EVP_Digest(entry->data, entry->size, entry->digest, &digest_size, EVP_sha256(), NULL) ==
             1 &&
         digest_size == ASSET_DIGEST_SIZE;
    if (!ok) {
        LOG(ERROR, "Cannot read or hash game asset %s from %s", name, path);
        free(entry->data);
        free(entry->name);
        free(entry);
        return false;
    }

    HASH_ADD_KEYPTR(hh, asset_cache, entry->name, strlen(entry->name), entry);
    asset_cache_size += entry->size;
    asset_cache_rss +=
        sizeof(*entry) + strlen(entry->name) + 1 + MAX((size_t)entry->size, (size_t)1);
    return true;
}

static void
asset_cache_directory(const char *root, const char *relative, const char *prefix, bool recursive) {
    char directory[HUGE_BUF];
    int length = snprintf(VS(directory), "%s%s%s", root, *relative != '\0' ? "/" : "", relative);
    if (length < 0 || (size_t)length >= sizeof(directory)) {
        return;
    }
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }

    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char child_relative[HUGE_BUF];
        length = snprintf(VS(child_relative),
                          "%s%s%s",
                          relative,
                          *relative != '\0' ? "/" : "",
                          item->d_name);
        if (length < 0 || (size_t)length >= sizeof(child_relative)) {
            continue;
        }
        char path[HUGE_BUF];
        length = snprintf(VS(path), "%s/%s", root, child_relative);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            continue;
        }
        struct stat sb;
        if (stat(path, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode) && recursive) {
            asset_cache_directory(root, child_relative, prefix, true);
            continue;
        }
        if (!S_ISREG(sb.st_mode)) {
            continue;
        }

        char name[HUGE_BUF];
        length = snprintf(VS(name), "%s/%s", prefix, child_relative);
        if (length < 0 || (size_t)length >= sizeof(name)) {
            continue;
        }
        char resolved[HUGE_BUF];
        if (asset_resolve_path(name, VS(resolved))) {
            asset_cache_add(name, resolved);
        }
    }
    closedir(dir);
}

void socket_assets_init(void) {
    char path[HUGE_BUF];
    snprintf(VS(path), "%s/data", settings.httppath);
    asset_cache_directory(path, "", "data", false);
    asset_cache_directory(settings.resourcespath, "", "resources", true);
    snprintf(VS(path), "%s/client-maps", settings.httppath);
    asset_cache_directory(path, "", "client-maps", false);
    LOG(INFO, "Cached %" PRIu64 " bytes of game assets in memory", asset_cache_size);
    server_metrics_asset_cache(asset_cache_rss);
}

void socket_assets_deinit(void) {
    while (asset_connections != NULL) {
        socket_assets_connection_clear(asset_connections);
    }

    asset_cache_entry_t *entry, *next;
    HASH_ITER(hh, asset_cache, entry, next) {
        HARD_ASSERT(entry->references == 0);
        HASH_DEL(asset_cache, entry);
        free(entry->data);
        free(entry->name);
        free(entry);
    }
    asset_cache_size = 0;
    asset_cache_rss = 0;
    asset_service_generation = 0;
    server_metrics_asset_cache(0);
}

void socket_assets_connection_register(socket_struct *ns) {
    HARD_ASSERT(ns != NULL);
    HARD_ASSERT(!ns->asset_service_registered);
    HARD_ASSERT(ns->asset_service_next == NULL);
    HARD_ASSERT(ns->asset_service_prev == NULL);

    ns->asset_transport_capabilities =
        ns->sc != NULL && socket_is_quic(ns->sc) ? ASSET_TRANSPORT_CAP_ALL : 0;
    ns->asset_accept_batch_next = false;

    ns->asset_service_prev = asset_connections_tail;
    if (asset_connections_tail != NULL) {
        asset_connections_tail->asset_service_next = ns;
    } else {
        asset_connections = ns;
    }
    asset_connections_tail = ns;
    ns->asset_service_registered = true;
    asset_connection_count++;
    if (asset_service_cursor == NULL) {
        asset_service_cursor = ns;
    }
}

static asset_cache_entry_t *asset_cache_find(const char *name) {
    asset_cache_entry_t *entry;
    HASH_FIND_STR(asset_cache, name, entry);
    return entry;
}

bool socket_assets_request_rate_allow(socket_struct *ns, unsigned int requests) {
    HARD_ASSERT(ns != NULL);
    HARD_ASSERT(requests != 0);

    uint64_t now = datetime_monotonic_ms();
    if (ns->asset_request_window_ms == 0 || now - ns->asset_request_window_ms >= 1000) {
        ns->asset_request_window_ms = now;
        ns->asset_window_requests = 0;
    }
    if (requests > ASSET_RATE_REQUESTS_PER_SECOND ||
        ns->asset_window_requests > ASSET_RATE_REQUESTS_PER_SECOND - requests) {
        LOG(ERROR,
            "Connection %s exceeded the in-band asset request-rate limit",
            ns->sc != NULL ? socket_get_id(ns->sc) : "(unbound)");
        ns->state = ST_ZOMBIE;
        return false;
    }
    ns->asset_window_requests += requests;
    return true;
}

static size_t asset_tokens_available(socket_struct *ns) {
    uint64_t now = datetime_monotonic_ms();
    if (ns->asset_token_updated_ms == 0) {
        ns->asset_token_updated_ms = now;
        ns->asset_tokens = ASSET_TOKEN_BUCKET_CAPACITY;
        return ns->asset_tokens;
    }
    uint64_t elapsed = now - ns->asset_token_updated_ms;
    if (elapsed != 0) {
        if (elapsed >= 1000U) {
            ns->asset_tokens = ASSET_TOKEN_BUCKET_CAPACITY;
        } else {
            uint64_t refill = elapsed * ASSET_RATE_BYTES_PER_SECOND / 1000U;
            ns->asset_tokens =
                (size_t)MIN((uint64_t)ASSET_TOKEN_BUCKET_CAPACITY, ns->asset_tokens + refill);
        }
        ns->asset_token_updated_ms = now;
    }
    return ns->asset_tokens;
}

static void
asset_stream_free(socket_struct *ns, asset_stream_state_t *state, bool reset, bool rejected) {
    if (reset) {
        socket_stream_reset(state->stream, SOCKET_STREAM_ERROR_SERVER_PROTOCOL);
    }
    socket_stream_destroy(state->stream);
    if (state->entry != NULL) {
        HARD_ASSERT(state->entry->references != 0);
        state->entry->references--;
    }
    if (state->header != NULL) {
        packet_free(state->header);
    }
    DL_DELETE(ns->asset_streams, state);
    HARD_ASSERT(ns->asset_stream_count != 0);
    ns->asset_stream_count--;
    server_metrics_asset_stream(-1, 0, rejected);
    free(state);
}

static bool asset_stream_header(asset_stream_state_t *state,
                                uint16_t face,
                                uint8_t status,
                                uint32_t total_size,
                                const uint8_t digest[ASSET_DIGEST_SIZE]) {
    HARD_ASSERT(state->header == NULL);
    HARD_ASSERT(state->header_pos == 0);
    HARD_ASSERT(state->body_pos == 0);

    size_t capacity = state->kind == SOCKET_STREAM_FACE_BATCH
                          ? SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE
                          : SOCKET_ASSET_RESPONSE_HEADER_SIZE;
    state->header = packet_new(0, capacity, 0);
    bool appended =
        state->kind == SOCKET_STREAM_FACE_BATCH
            ? socket_face_batch_response_append(state->header, face, status, total_size, digest)
            : (socket_asset_response_append_status(state->header, status, total_size, digest),
               true);
    if (!appended) {
        packet_free(state->header);
        state->header = NULL;
        return false;
    }
    HARD_ASSERT(packet_writer_finish(state->header));
    state->state = ASSET_SERVER_SEND_HEADER;
    return true;
}

static bool asset_stream_prepare_batch_member(asset_stream_state_t *state) {
    HARD_ASSERT(state->kind == SOCKET_STREAM_FACE_BATCH);
    HARD_ASSERT(state->batch_pos < state->batch_count);
    HARD_ASSERT(state->header == NULL);
    HARD_ASSERT(state->body == NULL);

    uint16_t face = state->batch_faces[state->batch_pos];
    const uint8_t *body = NULL;
    const uint8_t *digest = NULL;
    uint32_t body_size = 0;
    if (!face_get_asset(face, &body, &body_size, &digest) || body == NULL || body_size == 0 ||
        digest == NULL) {
        return asset_stream_header(state, face, ASSET_STATUS_NOT_FOUND, 0, NULL);
    }

    HARD_ASSERT(body_size <= ASSET_FACE_MAX_SIZE);
    state->body = body;
    state->body_size = body_size;
    if (!asset_stream_header(state, face, ASSET_STATUS_OK, body_size, digest)) {
        state->body = NULL;
        state->body_size = 0;
        return false;
    }
    return true;
}

static bool asset_stream_prepare_batch(socket_struct *ns, asset_stream_state_t *state) {
    socket_face_batch_request_t request;
    if (!socket_face_batch_request_parse(state->request, state->request_size, 0, &request)) {
        LOG(ERROR, "Connection %s sent a malformed QUIC face-batch request", socket_get_id(ns->sc));
        return false;
    }
    if (!socket_assets_request_rate_allow(ns, request.count)) {
        return false;
    }
    if (!socket_connection_admitted(ns)) {
        LOG(ERROR,
            "Connection %s opened a face-batch stream before admission",
            socket_get_id(ns->sc));
        return false;
    }

    state->batch_count = request.count;
    memcpy(state->batch_faces, request.faces, request.count * sizeof(*request.faces));
    uint32_t body_sizes[ASSET_FACE_BATCH_MAX] = {0};
    for (size_t i = 0; i < request.count; i++) {
        face_get_asset(request.faces[i], NULL, &body_sizes[i], NULL);
    }
    size_t rounds = socket_assets_face_batch_service_rounds(body_sizes, request.count);
    if (rounds == 0 || rounds > ASSET_STREAM_SERVICE_ROUNDS) {
        LOG(ERROR,
            "Connection %s requested a face batch outside the service bound",
            socket_get_id(ns->sc));
        return false;
    }
    LOG(DEBUG,
        "Connection %s opened QUIC face-batch stream with %u faces",
        socket_get_id(ns->sc),
        request.count);
    return asset_stream_prepare_batch_member(state);
}

static bool asset_stream_prepare_generic(socket_struct *ns, asset_stream_state_t *state) {
    socket_asset_request_t request;
    if (!socket_asset_request_parse(state->request, state->request_size, 0, &request)) {
        LOG(ERROR, "Connection %s sent a malformed QUIC asset request", socket_get_id(ns->sc));
        return false;
    }
    if (!socket_assets_request_rate_allow(ns, 1)) {
        return false;
    }
    if (!socket_connection_admitted(ns)) {
        LOG(ERROR, "Connection %s opened an asset stream before admission", socket_get_id(ns->sc));
        return false;
    }

    char resolved[HUGE_BUF];
    asset_cache_entry_t *entry = NULL;
    const uint8_t *body = NULL;
    const uint8_t *digest = NULL;
    uint32_t body_size = 0;
    uint16_t face = 0;
    if (socket_asset_face_path_parse(request.path, &face)) {
        face_get_asset(face, &body, &body_size, &digest);
    } else if (asset_resolve_path(request.path, VS(resolved))) {
        entry = asset_cache_find(request.path);
        if (entry != NULL) {
            body = entry->data;
            body_size = entry->size;
            digest = entry->digest;
        }
    }
    if (body == NULL || body_size == 0 || digest == NULL) {
        if (!asset_stream_header(state,
                                 0,
                                 request.flags & ASSET_REQUEST_METADATA
                                     ? ASSET_STATUS_METADATA_NOT_FOUND
                                     : ASSET_STATUS_NOT_FOUND,
                                 0,
                                 NULL)) {
            return false;
        }
    } else if (request.flags & ASSET_REQUEST_METADATA) {
        if (!asset_stream_header(state, 0, ASSET_STATUS_METADATA, body_size, digest)) {
            return false;
        }
    } else if (request.cached_size == body_size &&
               memcmp(request.cached_digest, digest, ASSET_DIGEST_SIZE) == 0) {
        if (!asset_stream_header(state, 0, ASSET_STATUS_NOT_MODIFIED, body_size, digest)) {
            return false;
        }
    } else {
        state->entry = entry;
        if (entry != NULL) {
            entry->references++;
        }
        state->body = body;
        state->body_size = body_size;
        if (!asset_stream_header(state, 0, ASSET_STATUS_OK, body_size, digest)) {
            state->body = NULL;
            state->body_size = 0;
            if (entry != NULL) {
                HARD_ASSERT(entry->references != 0);
                entry->references--;
                state->entry = NULL;
            }
            return false;
        }
    }
    LOG(DEBUG,
        "Connection %s opened QUIC asset stream for %s",
        socket_get_id(ns->sc),
        request.path);
    return true;
}

static bool asset_stream_prepare(socket_struct *ns, asset_stream_state_t *state) {
    return state->kind == SOCKET_STREAM_FACE_BATCH ? asset_stream_prepare_batch(ns, state)
                                                   : asset_stream_prepare_generic(ns, state);
}

static bool
asset_stream_read_request(socket_struct *ns, asset_stream_state_t *state, bool *progressed) {
    HARD_ASSERT(progressed != NULL);

    uint8_t surplus;
    void *buffer = &surplus;
    size_t capacity = 1;
    if (state->request_size < sizeof(state->request)) {
        buffer = state->request + state->request_size;
        capacity = sizeof(state->request) - state->request_size;
    }
    size_t amount = 0;
    socket_stream_result_t result = socket_stream_read(state->stream, buffer, capacity, &amount);
    if (result == SOCKET_STREAM_RESULT_ERROR) {
        return false;
    }
    if (result == SOCKET_STREAM_RESULT_FINISHED) {
        *progressed = true;
        return asset_stream_prepare(ns, state);
    }
    if (state->request_size == sizeof(state->request) && amount != 0) {
        return false;
    }
    state->request_size += amount;
    *progressed = amount != 0;
    return true;
}

static bool asset_stream_advance(asset_stream_state_t *state) {
    HARD_ASSERT(state->header == NULL);
    HARD_ASSERT(state->body == NULL);
    HARD_ASSERT(state->body_size == 0);
    HARD_ASSERT(state->header_pos == 0);
    HARD_ASSERT(state->body_pos == 0);

    if (state->kind == SOCKET_STREAM_FACE_BATCH && ++state->batch_pos < state->batch_count) {
        return asset_stream_prepare_batch_member(state);
    }
    state->concluded = socket_stream_conclude(state->stream);
    return false;
}

static bool asset_stream_write(socket_struct *ns,
                               asset_stream_state_t *state,
                               asset_service_budget_t *budget,
                               bool *progressed) {
    HARD_ASSERT(progressed != NULL);
    HARD_ASSERT(budget != NULL);

    const uint8_t *data;
    size_t remaining;
    if (state->state == ASSET_SERVER_SEND_HEADER) {
        data = state->header->data + state->header_pos;
        remaining = state->header->len - state->header_pos;
    } else {
        HARD_ASSERT(state->body != NULL);
        data = state->body + state->body_pos;
        remaining = state->body_size - state->body_pos;
        size_t tokens = asset_tokens_available(ns);
        if (tokens == 0) {
            server_metrics_asset_paced();
            return true;
        }
        remaining = MIN(remaining, MIN((size_t)ASSET_STREAM_QUANTUM, tokens));
    }
    remaining = MIN(remaining, budget->bytes);
    if (remaining == 0) {
        return true;
    }

    size_t amount = 0;
    socket_stream_result_t result = socket_stream_write(state->stream, data, remaining, &amount);
    if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
        return false;
    }
    *progressed = amount != 0;
    HARD_ASSERT(budget->bytes >= amount);
    budget->bytes -= amount;
    if (state->state == ASSET_SERVER_SEND_HEADER) {
        state->header_pos += amount;
        if (state->header_pos == state->header->len) {
            server_metrics_asset_response(datetime_monotonic_us() - state->started_us);
            packet_free(state->header);
            state->header = NULL;
            state->header_pos = 0;
            if (state->body == NULL || state->body_size == 0) {
                return asset_stream_advance(state);
            }
            state->state = ASSET_SERVER_SEND_BODY;
        }
    } else {
        HARD_ASSERT(ns->asset_tokens >= amount);
        ns->asset_tokens -= amount;
        state->body_pos += amount;
        server_metrics_asset_stream(0, amount, false);
        if (state->body_pos == state->body_size) {
            state->body = NULL;
            state->body_size = 0;
            state->body_pos = 0;
            return asset_stream_advance(state);
        }
    }
    return true;
}

static void asset_stream_accept(socket_struct *ns, asset_service_budget_t *budget) {
    HARD_ASSERT(budget != NULL);

    for (size_t accepted = 0; accepted < ASSET_STREAM_ACCEPT_QUANTUM &&
                              ns->asset_stream_count < ASSET_STREAM_ACTIVE_MAX && budget->work != 0;
         accepted++) {
        budget->work--;
        socket_stream_kind_t first =
            ns->asset_accept_batch_next ? SOCKET_STREAM_FACE_BATCH : SOCKET_STREAM_ASSET;
        socket_stream_kind_t second =
            first == SOCKET_STREAM_FACE_BATCH ? SOCKET_STREAM_ASSET : SOCKET_STREAM_FACE_BATCH;
        socket_stream_t *stream = socket_stream_accept(ns->sc, first);
        socket_stream_kind_t kind = first;
        if (stream == NULL) {
            stream = socket_stream_accept(ns->sc, second);
            kind = second;
        }
        if (stream == NULL) {
            break;
        }
        ns->asset_accept_batch_next = kind != SOCKET_STREAM_FACE_BATCH;
        asset_stream_state_t *state = xcalloc(1, sizeof(*state));
        state->stream = stream;
        state->kind = kind;
        state->started_us = datetime_monotonic_us();
        DL_APPEND(ns->asset_streams, state);
        ns->asset_stream_count++;
        server_metrics_asset_stream(1, 0, false);
    }
}

static bool
asset_stream_service_round(socket_struct *ns, asset_service_budget_t *budget, bool *progressed) {
    HARD_ASSERT(ns != NULL);
    HARD_ASSERT(budget != NULL);
    HARD_ASSERT(progressed != NULL);

    *progressed = false;
    asset_stream_state_t *state, *next;
    DL_FOREACH_SAFE(ns->asset_streams, state, next) {
        if (budget->work == 0 ||
            (budget->bytes == 0 && state->state != ASSET_SERVER_READ_REQUEST)) {
            return false;
        }
        budget->work--;
        bool stream_progressed = false;
        bool keep = state->state == ASSET_SERVER_READ_REQUEST
                        ? asset_stream_read_request(ns, state, &stream_progressed)
                        : asset_stream_write(ns, state, budget, &stream_progressed);
        *progressed |= stream_progressed;
        if (!keep) {
            asset_stream_free(ns, state, !state->concluded, !state->concluded);
        }
    }
    return true;
}

static void asset_stream_rotate(socket_struct *ns) {
    if (ns->asset_streams != NULL && ns->asset_streams->next != NULL) {
        asset_stream_state_t *first = ns->asset_streams;
        DL_DELETE(ns->asset_streams, first);
        DL_APPEND(ns->asset_streams, first);
    }
}

static bool asset_connection_service(socket_struct *ns, asset_service_budget_t *budget) {
    HARD_ASSERT(ns != NULL);
    HARD_ASSERT(budget != NULL);
    if (!socket_is_quic(ns->sc)) {
        return true;
    }
    if (budget->work == 0) {
        return true;
    }
    budget->work--;
    socket_stream_poll_pending(ns->sc);

    size_t round = 0;
    if (ns->asset_streams != NULL) {
        /* A client may reset a speculative stream and immediately open its
         * priority replacement. Observe and reap every previously active
         * stream before applying the shared active-stream limit to newly
         * typed streams; otherwise the replacement can be rejected against a
         * stale count of three. This is the first normal service round, so it
         * consumes the same byte/work budgets and preserves list rotation. */
        bool progressed = false;
        bool complete = asset_stream_service_round(ns, budget, &progressed);
        if (progressed) {
            asset_stream_rotate(ns);
        }
        round++;
        if (!complete || ns->state == ST_DEAD || ns->state == ST_ZOMBIE) {
            return ns->state != ST_DEAD && ns->state != ST_ZOMBIE;
        }
    }

    asset_stream_accept(ns, budget);

    for (; round < ASSET_STREAM_SERVICE_ROUNDS && budget->bytes != 0 && budget->work != 0;
         round++) {
        bool progressed = false;
        asset_stream_service_round(ns, budget, &progressed);
        if (!progressed || ns->asset_streams == NULL) {
            break;
        }
        asset_stream_rotate(ns);
    }
    return ns->state != ST_DEAD && ns->state != ST_ZOMBIE;
}

void socket_assets_service(void) {
    if (asset_connection_count == 0) {
        return;
    }

    asset_service_budget_t budget = {
        .bytes = socket_assets_tick_byte_budget(),
        .work = ASSET_GLOBAL_WORK_PER_TICK,
    };
    if (++asset_service_generation == 0) {
        asset_service_generation++;
    }
    socket_struct *pass_start =
        asset_service_cursor != NULL ? asset_service_cursor : asset_connections;
    socket_struct *next_cursor = pass_start;
    while (budget.bytes != 0 && budget.work != 0) {
        size_t pass_initial_bytes = budget.bytes;
        socket_struct *ns = pass_start;
        size_t visited = 0;

        while (visited < asset_connection_count && budget.bytes != 0 && budget.work != 0) {
            socket_struct *next =
                ns->asset_service_next != NULL ? ns->asset_service_next : asset_connections;
            budget.work--;
            if (ns->asset_service_generation != asset_service_generation) {
                ns->asset_service_productive_passes = 0;
            }
            ns->asset_service_generation = asset_service_generation;
            if (ns->state != ST_DEAD && ns->state != ST_ZOMBIE) {
                asset_service_budget_t connection_budget = {
                    .bytes = socket_assets_connection_pass_byte_budget(budget.bytes),
                    .work = MIN(budget.work, (size_t)ASSET_CONNECTION_WORK_PER_PASS),
                };
                size_t initial_bytes = connection_budget.bytes;
                size_t initial_work = connection_budget.work;
                if (!asset_connection_service(ns, &connection_budget)) {
                    ns->state = ST_ZOMBIE;
                }
                HARD_ASSERT(connection_budget.bytes <= initial_bytes);
                HARD_ASSERT(connection_budget.work <= initial_work);
                if (connection_budget.bytes != initial_bytes) {
                    ns->asset_service_productive_passes++;
                }
                budget.bytes -= initial_bytes - connection_budget.bytes;
                budget.work -= initial_work - connection_budget.work;
            }
            visited++;
            ns = next;
        }

        if (visited < asset_connection_count) {
            next_cursor = ns;
            break;
        }
        next_cursor = pass_start->asset_service_next != NULL ? pass_start->asset_service_next
                                                             : asset_connections;
        if (budget.bytes == 0 || budget.work == 0 || budget.bytes == pass_initial_bytes) {
            break;
        }
        pass_start = next_cursor;
    }

    asset_service_cursor = next_cursor;
}

bool socket_assets_pending(const socket_struct *ns) {
    HARD_ASSERT(ns != NULL);
    asset_stream_state_t *state;
    DL_FOREACH(ns->asset_streams, state) {
        if (state->state != ASSET_SERVER_READ_REQUEST) {
            return true;
        }
    }
    return false;
}

void socket_assets_connection_clear(socket_struct *ns) {
    HARD_ASSERT(ns != NULL);

    asset_stream_state_t *state, *next;
    DL_FOREACH_SAFE(ns->asset_streams, state, next) {
        asset_stream_free(ns, state, true, false);
    }

    if (!ns->asset_service_registered) {
        return;
    }

    socket_struct *cursor_next =
        ns->asset_service_next != NULL ? ns->asset_service_next : asset_connections;
    if (ns->asset_service_prev != NULL) {
        ns->asset_service_prev->asset_service_next = ns->asset_service_next;
    } else {
        asset_connections = ns->asset_service_next;
    }
    if (ns->asset_service_next != NULL) {
        ns->asset_service_next->asset_service_prev = ns->asset_service_prev;
    } else {
        asset_connections_tail = ns->asset_service_prev;
    }
    HARD_ASSERT(asset_connection_count != 0);
    asset_connection_count--;
    if (asset_connection_count == 0) {
        asset_service_cursor = NULL;
    } else if (asset_service_cursor == ns) {
        asset_service_cursor = cursor_next;
    }
    ns->asset_service_next = NULL;
    ns->asset_service_prev = NULL;
    ns->asset_transport_capabilities = 0;
    ns->asset_accept_batch_next = false;
    ns->asset_service_productive_passes = 0;
    ns->asset_service_registered = false;
}
