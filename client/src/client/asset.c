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
 * Bounded explicit-QUIC-stream asset scheduler and receiver.
 */

#include <global.h>
#include <wrapper.h>
#include <client_socket.h>
#include <network_graph.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>
#include <toolkit/string.h>
#include <openssl/evp.h>

typedef enum asset_transport_state {
    ASSET_TRANSPORT_QUEUED,
    ASSET_TRANSPORT_SEND_REQUEST,
    ASSET_TRANSPORT_READ_HEADER,
    ASSET_TRANSPORT_READ_BODY,
    ASSET_TRANSPORT_WAIT_FIN,
} asset_transport_state_t;

typedef enum asset_batch_transport_state {
    ASSET_BATCH_SEND_REQUEST,
    ASSET_BATCH_READ_HEADER,
    ASSET_BATCH_READ_BODY,
    ASSET_BATCH_WAIT_FIN,
} asset_batch_transport_state_t;

typedef struct asset_batch asset_batch_t;

struct asset_request {
    UT_hash_handle hh;
    char *key;
    char *path;
    char *cache_path;
    uint8_t *data;
    size_t size;
    size_t max_size;
    size_t received;
    size_t references;
    uint64_t sequence;
    uint8_t cached_digest[ASSET_DIGEST_SIZE];
    uint8_t expected_digest[ASSET_DIGEST_SIZE];
    bool cache_loaded;
    bool metadata_only;
    bool cancelled;
    bool cache_needs_save;
    bool face_batch_eligible;
    bool preempt_priority;
    bool preempt_isolated;
    bool preempt_requested;
    uint16_t face;
    asset_request_state_t state;
    asset_transport_state_t transport_state;
    socket_stream_t *stream;
    asset_batch_t *batch;
    packet_struct *wire_request;
    size_t wire_pos;
    uint8_t response_header[SOCKET_ASSET_RESPONSE_HEADER_SIZE];
    size_t response_header_pos;
    socket_asset_response_t response;
    EVP_MD_CTX *digest;
};

struct asset_batch {
    asset_batch_t *next;
    socket_stream_t *stream;
    packet_struct *wire_request;
    size_t wire_pos;
    asset_request_t *members[ASSET_FACE_BATCH_MAX];
    uint16_t faces[ASSET_FACE_BATCH_MAX];
    size_t count;
    size_t current;
    size_t declared_total;
    uint8_t response_header[SOCKET_FACE_BATCH_RESPONSE_HEADER_SIZE];
    size_t response_header_pos;
    socket_face_batch_response_t response;
    asset_batch_transport_state_t transport_state;
    bool final_member_valid;
    bool priority;
    bool preempt_requested;
    bool finished;
};

static asset_request_t *asset_requests;
static asset_batch_t *asset_batches;
static SDL_Mutex *asset_mutex;
static uint64_t asset_request_sequence;
static bool asset_transport_connected;
static uint8_t asset_transport_capabilities;

static bool asset_lock(void) {
    if (asset_mutex == NULL) {
        asset_mutex = SDL_CreateMutex();
    }
    if (asset_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(asset_mutex);
    return true;
}

static void asset_request_destroy(asset_request_t *request) {
    if (request->stream != NULL) {
        socket_stream_destroy(request->stream);
    }
    if (request->wire_request != NULL) {
        packet_free(request->wire_request);
    }
    EVP_MD_CTX_free(request->digest);
    free(request->key);
    free(request->path);
    free(request->cache_path);
    free(request->data);
    free(request);
}

static void asset_request_cache_load(asset_request_t *request) {
    if (request->cache_path == NULL) {
        return;
    }

    uint64_t started = SDL_GetTicksNS();
    FILE *fp = path_fopen(request->cache_path, "rb");
    struct stat sb;
    if (fp == NULL || fstat(fileno(fp), &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0 ||
        (uint64_t)sb.st_size > ASSET_MAX_SIZE) {
        if (fp != NULL) {
            fclose(fp);
        }
        return;
    }

    uint8_t *data = xmalloc((size_t)sb.st_size + 1);
    bool success = fread(data, 1, (size_t)sb.st_size, fp) == (size_t)sb.st_size;
    if (fclose(fp) != 0) {
        success = false;
    }
    if (!success) {
        free(data);
        return;
    }

    data[(size_t)sb.st_size] = '\0';
    request->data = data;
    request->size = (size_t)sb.st_size;
    unsigned int digest_size = 0;
    if (EVP_Digest(request->data,
                   request->size,
                   request->cached_digest,
                   &digest_size,
                   EVP_sha256(),
                   NULL) != 1 ||
        digest_size != ASSET_DIGEST_SIZE) {
        free(request->data);
        request->data = NULL;
        request->size = 0;
        return;
    }
    request->cache_loaded = true;
    LOG(DEBUG,
        "Loaded cached QUIC asset %s (%" PRIu64 " bytes) in %.3f ms",
        request->path,
        (uint64_t)request->size,
        (double)(SDL_GetTicksNS() - started) / 1000000.0);
}

static void asset_request_cache_save(const asset_request_t *request) {
    if (request->cache_path == NULL) {
        return;
    }

    uint64_t started = SDL_GetTicksNS();
    char *path = file_path(request->cache_path, "wb");
    bool success = path_write_atomic(path, request->data, request->size, 0600);
    free(path);
    if (success) {
        LOG(DEBUG,
            "Wrote QUIC asset cache %s in %.3f ms",
            request->cache_path,
            (double)(SDL_GetTicksNS() - started) / 1000000.0);
    } else {
        LOG(ERROR,
            "Could not write QUIC asset cache %s in %.3f ms",
            request->cache_path,
            (double)(SDL_GetTicksNS() - started) / 1000000.0);
    }
}

static size_t asset_pending_count(void) {
    size_t count = 0;
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->state == ASSET_REQUEST_PENDING && !request->cancelled) {
            count++;
        }
    }
    return count;
}

void asset_requests_connect(socket_t *sc) {
    HARD_ASSERT(sc != NULL);

    bool connected = socket_is_quic(sc);
    if (!asset_lock()) {
        return;
    }
    asset_transport_connected = connected;
    /* Setup belongs to this connection. Never carry negotiated bits across a
     * reconnect while the replacement setup packet is still in flight. */
    asset_transport_capabilities = 0;
    SDL_UnlockMutex(asset_mutex);
}

void asset_requests_set_capabilities(uint8_t capabilities) {
    if (!asset_lock()) {
        return;
    }
    asset_transport_capabilities = capabilities & ASSET_TRANSPORT_CAP_ALL;
    SDL_UnlockMutex(asset_mutex);
}

bool asset_requests_available(void) {
    if (!asset_lock()) {
        return false;
    }
    bool available = asset_transport_connected &&
                     (asset_transport_capabilities & ASSET_TRANSPORT_CAP_GENERIC) != 0;
    SDL_UnlockMutex(asset_mutex);
    return available;
}

bool asset_face_batch_available(void) {
    if (!asset_lock()) {
        return false;
    }
    bool available = asset_transport_connected &&
                     (asset_transport_capabilities & ASSET_TRANSPORT_CAP_GENERIC) != 0 &&
                     (asset_transport_capabilities & ASSET_TRANSPORT_CAP_FACE_BATCH) != 0;
    SDL_UnlockMutex(asset_mutex);
    return available;
}

static asset_request_t *asset_request_start_internal(const char *path,
                                                     const char *cache_path,
                                                     bool metadata_only,
                                                     size_t max_size,
                                                     bool preempt_priority) {
    if (path == NULL || *path == '\0' || strlen(path) >= MAX_BUF || max_size == 0 ||
        max_size > ASSET_MAX_SIZE) {
        return NULL;
    }

    char key[MAX_BUF + 32];
    snprintf(VS(key), "%c:%" PRIu64 ":%s", metadata_only ? 'M' : 'D', (uint64_t)max_size, path);
    if (!asset_lock()) {
        return NULL;
    }
    if (!asset_transport_connected ||
        (asset_transport_capabilities & ASSET_TRANSPORT_CAP_GENERIC) == 0) {
        SDL_UnlockMutex(asset_mutex);
        return NULL;
    }
    asset_request_t *request;
    HASH_FIND_STR(asset_requests, key, request);
    if (request != NULL) {
        if (request->cancelled) {
            SDL_UnlockMutex(asset_mutex);
            return NULL;
        }
        request->references++;
        request->preempt_priority |= preempt_priority;
        SDL_UnlockMutex(asset_mutex);
        return request;
    }
    if (asset_pending_count() >= ASSET_REQUEST_PENDING_MAX) {
        LOG(DEBUG, "Deferring QUIC asset %s: pending request limit reached", path);
        SDL_UnlockMutex(asset_mutex);
        return NULL;
    }
    SDL_UnlockMutex(asset_mutex);

    request = xcalloc(1, sizeof(*request));
    request->key = xstrdup(key);
    request->path = xstrdup(path);
    request->cache_path = cache_path != NULL ? xstrdup(cache_path) : NULL;
    request->references = 1;
    request->max_size = max_size;
    request->metadata_only = metadata_only;
    request->preempt_priority = preempt_priority;
    request->face_batch_eligible = !metadata_only && cache_path == NULL &&
                                   max_size == ASSET_FACE_MAX_SIZE &&
                                   socket_asset_face_path_parse(path, &request->face);
    request->state = ASSET_REQUEST_PENDING;
    request->transport_state = ASSET_TRANSPORT_QUEUED;
    asset_request_cache_load(request);

    request->wire_request = packet_new(0, 128, 128);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    socket_asset_request_append(request->wire_request,
                                request->path,
                                request->cache_loaded ? (uint32_t)request->size : 0,
                                request->cache_loaded ? request->cached_digest : empty_digest,
                                request->metadata_only ? ASSET_REQUEST_METADATA : 0);
    if (!packet_writer_finish(request->wire_request)) {
        asset_request_destroy(request);
        return NULL;
    }

    if (!asset_lock()) {
        asset_request_destroy(request);
        return NULL;
    }
    if (!asset_transport_connected ||
        (asset_transport_capabilities & ASSET_TRANSPORT_CAP_GENERIC) == 0) {
        SDL_UnlockMutex(asset_mutex);
        asset_request_destroy(request);
        return NULL;
    }
    asset_request_t *existing;
    HASH_FIND_STR(asset_requests, key, existing);
    if (existing != NULL) {
        bool available = !existing->cancelled;
        if (available) {
            existing->references++;
            existing->preempt_priority |= preempt_priority;
        }
        SDL_UnlockMutex(asset_mutex);
        asset_request_destroy(request);
        return available ? existing : NULL;
    }
    if (asset_pending_count() >= ASSET_REQUEST_PENDING_MAX) {
        LOG(DEBUG, "Deferring QUIC asset %s: pending request limit reached", path);
        SDL_UnlockMutex(asset_mutex);
        asset_request_destroy(request);
        return NULL;
    }
    request->sequence = asset_request_sequence++;
    HASH_ADD_KEYPTR(hh, asset_requests, request->key, strlen(request->key), request);
    LOG(DEBUG, "Queued QUIC asset %s%s", request->path, metadata_only ? " (metadata)" : "");
    SDL_UnlockMutex(asset_mutex);
    return request;
}

asset_request_t *asset_request_start(const char *path) {
    return asset_request_start_internal(path, NULL, false, ASSET_MAX_SIZE, false);
}

asset_request_t *asset_request_start_bounded(const char *path, size_t max_size) {
    return asset_request_start_internal(path, NULL, false, max_size, false);
}

asset_request_t *asset_request_start_bounded_priority(const char *path, size_t max_size) {
    uint16_t face = 0;
    if (max_size != ASSET_FACE_MAX_SIZE || !socket_asset_face_path_parse(path, &face)) {
        return NULL;
    }
    return asset_request_start_internal(path, NULL, false, max_size, true);
}

asset_request_t *asset_request_start_cached(const char *path, const char *cache_path) {
    return asset_request_start_internal(path, cache_path, false, ASSET_MAX_SIZE, false);
}

asset_request_t *asset_request_start_metadata(const char *path) {
    return asset_request_start_internal(path, NULL, true, ASSET_MAX_SIZE, false);
}

asset_request_state_t asset_request_get_state(asset_request_t *request) {
    if (request == NULL || !asset_lock()) {
        return ASSET_REQUEST_ERROR;
    }
    bool save_cache = request->state == ASSET_REQUEST_COMPLETE && request->cache_needs_save;
    if (save_cache) {
        request->cache_needs_save = false;
        request->references++;
    }
    asset_request_state_t state = request->state;
    SDL_UnlockMutex(asset_mutex);
    if (save_cache) {
        /* The temporary reference keeps this request alive while potentially
         * slow filesystem I/O runs without stalling the transport thread. */
        asset_request_cache_save(request);
        asset_request_free(request);
    }
    return state;
}

const uint8_t *asset_request_get_data(const asset_request_t *request, size_t *size) {
    if (request == NULL || !asset_lock()) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = request->size;
    }
    const uint8_t *data = request->state == ASSET_REQUEST_COMPLETE ? request->data : NULL;
    SDL_UnlockMutex(asset_mutex);
    return data;
}

bool asset_request_get_metadata(const asset_request_t *request,
                                size_t *size,
                                uint8_t digest[ASSET_DIGEST_SIZE]) {
    if (request == NULL || !asset_lock()) {
        return false;
    }
    bool valid = request->metadata_only && request->state == ASSET_REQUEST_COMPLETE;
    if (valid && size != NULL) {
        *size = request->size;
    }
    if (valid && digest != NULL) {
        memcpy(digest, request->expected_digest, ASSET_DIGEST_SIZE);
    }
    SDL_UnlockMutex(asset_mutex);
    return valid;
}

void asset_request_free(asset_request_t *request) {
    if (request == NULL || !asset_lock()) {
        return;
    }
    HARD_ASSERT(request->references != 0);
    request->references--;
    if (request->references == 0) {
        if (request->stream != NULL || request->batch != NULL) {
            request->cancelled = true;
        } else {
            HASH_DEL(asset_requests, request);
            asset_request_destroy(request);
        }
    }
    SDL_UnlockMutex(asset_mutex);
}

static void asset_request_set_error(asset_request_t *request, const char *reason) {
    LOG(ERROR, "QUIC asset %s failed: %s", request->path, reason);
    EVP_MD_CTX_free(request->digest);
    request->digest = NULL;
    request->state = ASSET_REQUEST_ERROR;
}

static void asset_request_fail(asset_request_t *request, const char *reason) {
    HARD_ASSERT(request->batch == NULL);
    if (request->stream != NULL) {
        socket_stream_reset(request->stream, SOCKET_STREAM_ERROR_CLIENT_PROTOCOL);
        socket_stream_destroy(request->stream);
        request->stream = NULL;
    }
    asset_request_set_error(request, reason);
}

static bool asset_request_header(asset_request_t *request) {
    if (!socket_asset_response_parse(request->response_header,
                                     sizeof(request->response_header),
                                     0,
                                     &request->response)) {
        asset_request_fail(request, "malformed response header");
        return false;
    }
    memcpy(request->expected_digest, request->response.digest, ASSET_DIGEST_SIZE);

    if (request->response.total_size > request->max_size) {
        asset_request_fail(request, "declared size exceeds the request-specific limit");
        return false;
    }

    if (request->response.status == ASSET_STATUS_OK && !request->metadata_only) {
        request->size = request->response.total_size;
        free(request->data);
        request->data = xmalloc(request->size + 1);
        request->data[0] = '\0';
        request->received = 0;
        request->cache_loaded = false;
        request->digest = EVP_MD_CTX_new();
        if (request->digest == NULL ||
            EVP_DigestInit_ex(request->digest, EVP_sha256(), NULL) != 1) {
            asset_request_fail(request, "could not initialize SHA-256");
            return false;
        }
        request->transport_state =
            request->size == 0 ? ASSET_TRANSPORT_WAIT_FIN : ASSET_TRANSPORT_READ_BODY;
        return true;
    }
    if (request->response.status == ASSET_STATUS_METADATA && request->metadata_only) {
        request->size = request->response.total_size;
        request->transport_state = ASSET_TRANSPORT_WAIT_FIN;
        return true;
    }
    if (request->response.status == ASSET_STATUS_NOT_MODIFIED && !request->metadata_only &&
        request->cache_loaded && request->response.total_size == request->size &&
        memcmp(request->response.digest, request->cached_digest, ASSET_DIGEST_SIZE) == 0) {
        request->size = request->response.total_size;
        request->transport_state = ASSET_TRANSPORT_WAIT_FIN;
        return true;
    }
    asset_request_fail(request, "server rejected request or returned an invalid status");
    return false;
}

static void asset_request_finish(asset_request_t *request) {
    if (request->response.status == ASSET_STATUS_OK) {
        uint8_t digest[ASSET_DIGEST_SIZE];
        unsigned int digest_size = 0;
        if (request->received != request->size ||
            EVP_DigestFinal_ex(request->digest, digest, &digest_size) != 1 ||
            digest_size != ASSET_DIGEST_SIZE ||
            memcmp(digest, request->expected_digest, ASSET_DIGEST_SIZE) != 0) {
            asset_request_fail(request, "early EOF, declared-size violation, or SHA-256 mismatch");
            return;
        }
        request->data[request->size] = '\0';
        request->cache_needs_save = request->cache_path != NULL;
    }
    EVP_MD_CTX_free(request->digest);
    request->digest = NULL;
    socket_stream_destroy(request->stream);
    request->stream = NULL;
    request->state = ASSET_REQUEST_COMPLETE;
    LOG(DEBUG,
        "Completed QUIC asset %s (%" PRIu64 " bytes)",
        request->path,
        (uint64_t)request->size);
}

static void asset_batch_member_advance(asset_batch_t *batch) {
    asset_request_t *request = batch->members[batch->current];
    HARD_ASSERT(request != NULL);
    HARD_ASSERT(request->batch == batch);
    request->batch = NULL;
    batch->members[batch->current] = NULL;
    batch->current++;
    batch->response_header_pos = 0;
    memset(&batch->response, 0, sizeof(batch->response));
    batch->transport_state =
        batch->current == batch->count ? ASSET_BATCH_WAIT_FIN : ASSET_BATCH_READ_HEADER;
}

static void asset_batch_fail(asset_batch_t *batch, const char *reason) {
    if (batch->stream != NULL) {
        socket_stream_reset(batch->stream, SOCKET_STREAM_ERROR_CLIENT_PROTOCOL);
        socket_stream_destroy(batch->stream);
        batch->stream = NULL;
    }
    for (size_t i = batch->current; i < batch->count; i++) {
        asset_request_t *request = batch->members[i];
        if (request == NULL) {
            continue;
        }
        HARD_ASSERT(request->batch == batch);
        request->batch = NULL;
        if (request->state == ASSET_REQUEST_PENDING) {
            asset_request_set_error(request, reason);
        }
        batch->members[i] = NULL;
    }
    batch->finished = true;
}

static void asset_batch_disconnect(asset_batch_t *batch) {
    if (batch->stream != NULL) {
        socket_stream_reset(batch->stream, SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(batch->stream);
        batch->stream = NULL;
    }
    for (size_t i = batch->current; i < batch->count; i++) {
        asset_request_t *request = batch->members[i];
        if (request == NULL) {
            continue;
        }
        request->batch = NULL;
        EVP_MD_CTX_free(request->digest);
        request->digest = NULL;
        if (request->state == ASSET_REQUEST_PENDING) {
            request->state = ASSET_REQUEST_ERROR;
        }
        batch->members[i] = NULL;
    }
    batch->finished = true;
}

static void asset_request_mark_preempted(asset_request_t *request) {
    EVP_MD_CTX_free(request->digest);
    request->digest = NULL;
    free(request->data);
    request->data = NULL;
    request->size = 0;
    request->received = 0;
    request->cache_needs_save = false;
    request->state = ASSET_REQUEST_PREEMPTED;
}

static void asset_batch_preempt(asset_batch_t *batch) {
    if (batch->stream != NULL) {
        socket_stream_reset(batch->stream, SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(batch->stream);
        batch->stream = NULL;
    }
    for (size_t i = batch->current; i < batch->count; i++) {
        asset_request_t *request = batch->members[i];
        if (request == NULL) {
            continue;
        }
        request->batch = NULL;
        if (!request->cancelled && request->state == ASSET_REQUEST_PENDING) {
            asset_request_mark_preempted(request);
        }
        batch->members[i] = NULL;
    }
    batch->finished = true;
}

bool asset_request_preempt(asset_request_t *victim,
                           asset_request_t *replacement,
                           bool *displace_victim) {
    if (displace_victim != NULL) {
        *displace_victim = false;
    }
    if (victim == NULL || replacement == NULL || displace_victim == NULL || victim == replacement ||
        !asset_lock()) {
        return false;
    }

    bool replacement_valid = victim->face_batch_eligible && replacement->face_batch_eligible &&
                             !replacement->cancelled && replacement->references != 0 &&
                             (replacement->state == ASSET_REQUEST_PENDING ||
                              replacement->state == ASSET_REQUEST_COMPLETE);
    if (!replacement_valid || victim->cancelled || victim->references == 0 ||
        (victim->state != ASSET_REQUEST_PENDING && victim->state != ASSET_REQUEST_COMPLETE)) {
        SDL_UnlockMutex(asset_mutex);
        return false;
    }

    /* The I/O thread may consume the prestarted replacement before this main-
     * thread handoff. Its physical capacity is then already reserved (or even
     * released after completion), so only discard the speculative logical
     * slot; never reset an unrelated victim stream in that race. */
    if (replacement->state == ASSET_REQUEST_COMPLETE || replacement->stream != NULL ||
        replacement->batch != NULL) {
        SDL_UnlockMutex(asset_mutex);
        return true;
    }

    if (replacement->transport_state != ASSET_TRANSPORT_QUEUED) {
        SDL_UnlockMutex(asset_mutex);
        return false;
    }
    bool old_priority = replacement->preempt_priority;
    replacement->preempt_priority = true;

    if (victim->state == ASSET_REQUEST_COMPLETE) {
        *displace_victim = true;
        SDL_UnlockMutex(asset_mutex);
        return true;
    }

    asset_batch_t *batch = victim->batch;
    if (replacement->preempt_isolated ||
        (batch != NULL && (batch->priority || batch->preempt_requested)) ||
        victim->preempt_requested) {
        /* Several visible admissions can race ahead of one I/O pass. Each
         * successful handoff must reclaim a distinct physical stream. */
        replacement->preempt_priority = old_priority;
        SDL_UnlockMutex(asset_mutex);
        return false;
    }

    if (batch != NULL) {
        /* QUIC streams belong to the I/O thread. Publish the handoff intent;
         * its next service pass resets this batch before counting/opening
         * physical streams, then opens the already-prioritized replacement. */
        batch->preempt_requested = true;
    } else {
        if (victim->stream != NULL) {
            victim->preempt_requested = true;
        } else {
            /* A queued logical victim reclaims no physical capacity. Let the
             * image layer inspect another admitted background instead. */
            replacement->preempt_priority = old_priority;
            SDL_UnlockMutex(asset_mutex);
            return false;
        }
    }

    replacement->preempt_isolated = true;
    *displace_victim = true;
    SDL_UnlockMutex(asset_mutex);
    return true;
}

static void asset_batch_destroy(asset_batch_t *batch) {
    if (batch->stream != NULL) {
        socket_stream_destroy(batch->stream);
    }
    if (batch->wire_request != NULL) {
        packet_free(batch->wire_request);
    }
    free(batch);
}

static bool asset_batch_cancel_if_unobserved(asset_batch_t *batch) {
    if (batch->current == batch->count) {
        return false;
    }
    for (size_t i = batch->current; i < batch->count; i++) {
        if (batch->members[i] != NULL && !batch->members[i]->cancelled) {
            return false;
        }
    }
    socket_stream_reset(batch->stream, SOCKET_STREAM_ERROR_CANCELLED);
    socket_stream_destroy(batch->stream);
    batch->stream = NULL;
    for (size_t i = batch->current; i < batch->count; i++) {
        if (batch->members[i] != NULL) {
            batch->members[i]->batch = NULL;
            batch->members[i] = NULL;
        }
    }
    batch->finished = true;
    return true;
}

static bool asset_batch_header(asset_batch_t *batch) {
    asset_request_t *request = batch->members[batch->current];
    HARD_ASSERT(request != NULL);
    if (!socket_face_batch_response_parse(batch->response_header,
                                          sizeof(batch->response_header),
                                          0,
                                          &batch->response) ||
        batch->response.face != batch->faces[batch->current] ||
        batch->response.body_size > request->max_size ||
        batch->declared_total > ASSET_FACE_BATCH_MAX_SIZE - batch->response.body_size) {
        asset_batch_fail(batch, "malformed or out-of-order face batch response");
        return false;
    }
    batch->declared_total += batch->response.body_size;
    request->size = batch->response.body_size;
    request->received = 0;
    memcpy(request->expected_digest, batch->response.digest, ASSET_DIGEST_SIZE);

    if (batch->response.status == ASSET_STATUS_NOT_FOUND) {
        if (!request->cancelled) {
            asset_request_set_error(request, "server did not find the batched face");
        }
        asset_batch_member_advance(batch);
        return true;
    }
    if (batch->response.status != ASSET_STATUS_OK || request->size == 0) {
        asset_batch_fail(batch, "invalid face batch response status");
        return false;
    }

    free(request->data);
    request->data = request->cancelled ? NULL : xmalloc(request->size + 1U);
    request->digest = EVP_MD_CTX_new();
    if (request->digest == NULL || EVP_DigestInit_ex(request->digest, EVP_sha256(), NULL) != 1) {
        asset_batch_fail(batch, "could not initialize batched face SHA-256");
        return false;
    }
    batch->transport_state = ASSET_BATCH_READ_BODY;
    return true;
}

static void asset_batch_member_finish(asset_batch_t *batch) {
    asset_request_t *request = batch->members[batch->current];
    HARD_ASSERT(request != NULL);
    uint8_t digest[ASSET_DIGEST_SIZE];
    unsigned int digest_size = 0;
    bool valid = request->received == request->size && request->digest != NULL &&
                 EVP_DigestFinal_ex(request->digest, digest, &digest_size) == 1 &&
                 digest_size == ASSET_DIGEST_SIZE &&
                 memcmp(digest, request->expected_digest, ASSET_DIGEST_SIZE) == 0;
    EVP_MD_CTX_free(request->digest);
    request->digest = NULL;
    bool final_member = batch->current + 1U == batch->count;
    if (!request->cancelled) {
        if (valid) {
            request->data[request->size] = '\0';
            if (final_member) {
                /* Only FIN proves that the final declared record has exact
                 * framing. Keep this request attached and unobservable until
                 * that proof arrives. */
                batch->final_member_valid = true;
            } else {
                request->state = ASSET_REQUEST_COMPLETE;
                LOG(DEBUG,
                    "Completed batched QUIC face %u (%" PRIu64 " bytes)",
                    request->face,
                    (uint64_t)request->size);
            }
        } else {
            free(request->data);
            request->data = NULL;
            request->size = 0;
            asset_request_set_error(request, "batched face SHA-256 mismatch");
        }
    }
    if (final_member) {
        batch->transport_state = ASSET_BATCH_WAIT_FIN;
    } else {
        asset_batch_member_advance(batch);
    }
}

static bool asset_batch_service(asset_batch_t *batch) {
    if (asset_batch_cancel_if_unobserved(batch)) {
        return true;
    }
    if (batch->transport_state == ASSET_BATCH_SEND_REQUEST) {
        size_t amount = 0;
        socket_stream_result_t result =
            socket_stream_write(batch->stream,
                                batch->wire_request->data + batch->wire_pos,
                                batch->wire_request->len - batch->wire_pos,
                                &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
            asset_batch_fail(batch, "face batch stream closed while writing");
            return true;
        }
        batch->wire_pos += amount;
        network_graph_update(NETWORK_GRAPH_TYPE_ASSET, NETWORK_GRAPH_TRAFFIC_TX, amount);
        if (batch->wire_pos == batch->wire_request->len) {
            if (!socket_stream_conclude(batch->stream)) {
                asset_batch_fail(batch, "could not conclude face batch request");
                return true;
            }
            packet_free(batch->wire_request);
            batch->wire_request = NULL;
            batch->transport_state = ASSET_BATCH_READ_HEADER;
        }
        return amount != 0;
    }

    uint8_t discard[ASSET_STREAM_QUANTUM];
    void *buffer = discard;
    size_t capacity = 1;
    asset_request_t *request = NULL;
    if (batch->transport_state == ASSET_BATCH_READ_HEADER) {
        buffer = batch->response_header + batch->response_header_pos;
        capacity = sizeof(batch->response_header) - batch->response_header_pos;
    } else if (batch->transport_state == ASSET_BATCH_READ_BODY) {
        request = batch->members[batch->current];
        HARD_ASSERT(request != NULL);
        capacity = MIN((size_t)ASSET_STREAM_QUANTUM, request->size - request->received);
        if (!request->cancelled) {
            buffer = request->data + request->received;
        }
    }

    size_t amount = 0;
    socket_stream_result_t result = socket_stream_read(batch->stream, buffer, capacity, &amount);
    if (result == SOCKET_STREAM_RESULT_ERROR) {
        asset_batch_fail(batch, "face batch stream reset or connection error");
        return true;
    }
    if (result == SOCKET_STREAM_RESULT_FINISHED) {
        if (batch->transport_state == ASSET_BATCH_WAIT_FIN) {
            if (batch->current < batch->count) {
                asset_request_t *request = batch->members[batch->current];
                HARD_ASSERT(request != NULL);
                if (!request->cancelled && batch->final_member_valid) {
                    request->state = ASSET_REQUEST_COMPLETE;
                    LOG(DEBUG,
                        "Completed batched QUIC face %u (%" PRIu64 " bytes)",
                        request->face,
                        (uint64_t)request->size);
                }
                asset_batch_member_advance(batch);
            }
            socket_stream_destroy(batch->stream);
            batch->stream = NULL;
            batch->finished = true;
        } else {
            asset_batch_fail(batch, "early EOF in face batch response");
        }
        return true;
    }
    if (amount == 0) {
        return false;
    }
    network_graph_update(NETWORK_GRAPH_TYPE_ASSET, NETWORK_GRAPH_TRAFFIC_RX, amount);

    if (batch->transport_state == ASSET_BATCH_READ_HEADER) {
        batch->response_header_pos += amount;
        if (batch->response_header_pos == sizeof(batch->response_header)) {
            asset_batch_header(batch);
        }
    } else if (batch->transport_state == ASSET_BATCH_READ_BODY) {
        if (EVP_DigestUpdate(request->digest, buffer, amount) != 1) {
            asset_batch_fail(batch, "batched face SHA-256 update failed");
            return true;
        }
        request->received += amount;
        if (request->received == request->size) {
            asset_batch_member_finish(batch);
        }
    } else {
        asset_batch_fail(batch, "surplus bytes after face batch records");
    }
    return true;
}

static bool asset_request_service(asset_request_t *request) {
    if (request->cancelled) {
        socket_stream_reset(request->stream, SOCKET_STREAM_ERROR_CANCELLED);
        socket_stream_destroy(request->stream);
        request->stream = NULL;
        return true;
    }

    if (request->transport_state == ASSET_TRANSPORT_SEND_REQUEST) {
        size_t amount = 0;
        socket_stream_result_t result =
            socket_stream_write(request->stream,
                                request->wire_request->data + request->wire_pos,
                                request->wire_request->len - request->wire_pos,
                                &amount);
        if (result == SOCKET_STREAM_RESULT_ERROR || result == SOCKET_STREAM_RESULT_FINISHED) {
            asset_request_fail(request, "request stream closed while writing");
            return true;
        }
        request->wire_pos += amount;
        network_graph_update(NETWORK_GRAPH_TYPE_ASSET, NETWORK_GRAPH_TRAFFIC_TX, amount);
        if (request->wire_pos == request->wire_request->len) {
            if (!socket_stream_conclude(request->stream)) {
                asset_request_fail(request, "could not conclude request");
                return true;
            }
            packet_free(request->wire_request);
            request->wire_request = NULL;
            request->transport_state = ASSET_TRANSPORT_READ_HEADER;
        }
        return amount != 0;
    }

    uint8_t discard;
    void *buffer = &discard;
    size_t capacity = 1;
    if (request->transport_state == ASSET_TRANSPORT_READ_HEADER) {
        buffer = request->response_header + request->response_header_pos;
        capacity = sizeof(request->response_header) - request->response_header_pos;
    } else if (request->transport_state == ASSET_TRANSPORT_READ_BODY) {
        capacity = MIN((size_t)ASSET_STREAM_QUANTUM, request->size - request->received);
        buffer = request->data + request->received;
    }

    size_t amount = 0;
    socket_stream_result_t result = socket_stream_read(request->stream, buffer, capacity, &amount);
    if (result == SOCKET_STREAM_RESULT_ERROR) {
        asset_request_fail(request, "stream reset or connection error");
        return true;
    }
    if (result == SOCKET_STREAM_RESULT_FINISHED) {
        if (request->transport_state == ASSET_TRANSPORT_WAIT_FIN) {
            asset_request_finish(request);
        } else {
            asset_request_fail(request, "early EOF");
        }
        return true;
    }
    if (amount == 0) {
        return false;
    }
    network_graph_update(NETWORK_GRAPH_TYPE_ASSET, NETWORK_GRAPH_TRAFFIC_RX, amount);

    if (request->transport_state == ASSET_TRANSPORT_READ_HEADER) {
        request->response_header_pos += amount;
        if (request->response_header_pos == sizeof(request->response_header)) {
            asset_request_header(request);
        }
    } else if (request->transport_state == ASSET_TRANSPORT_READ_BODY) {
        if (EVP_DigestUpdate(request->digest, buffer, amount) != 1) {
            asset_request_fail(request, "SHA-256 update failed");
            return true;
        }
        request->received += amount;
        if (request->received == request->size) {
            request->transport_state = ASSET_TRANSPORT_WAIT_FIN;
        }
    } else {
        asset_request_fail(request, "received surplus body bytes");
    }
    return true;
}

static bool asset_request_is_queued(const asset_request_t *request) {
    return request->state == ASSET_REQUEST_PENDING && !request->cancelled &&
           request->transport_state == ASSET_TRANSPORT_QUEUED && request->stream == NULL &&
           request->batch == NULL;
}

static asset_request_t *asset_request_oldest_queued(void) {
    asset_request_t *oldest = NULL;
    asset_request_t *oldest_generic = NULL;
    asset_request_t *oldest_priority = NULL;
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (!asset_request_is_queued(request)) {
            continue;
        }
        if (oldest == NULL || request->sequence < oldest->sequence) {
            oldest = request;
        }
        if (!request->face_batch_eligible &&
            (oldest_generic == NULL || request->sequence < oldest_generic->sequence)) {
            oldest_generic = request;
        }
        if (request->preempt_priority &&
            (oldest_priority == NULL || request->sequence < oldest_priority->sequence)) {
            oldest_priority = request;
        }
    }
    if (oldest_priority != NULL) {
        if (oldest_generic != NULL && oldest_generic->sequence < oldest_priority->sequence) {
            /* Priority may jump speculative faces, but an older generic asset
             * remains a strict application-level sequence barrier. */
            return oldest_generic;
        }
        return oldest_priority;
    }
    return oldest;
}

static size_t asset_active_stream_count(void) {
    size_t active = 0;
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->stream != NULL) {
            active++;
        }
    }
    for (asset_batch_t *batch = asset_batches; batch != NULL; batch = batch->next) {
        if (!batch->finished) {
            active++;
        }
    }
    return active;
}

bool asset_request_preemption_needed(const asset_request_t *replacement) {
    if (replacement == NULL || !asset_lock()) {
        return false;
    }
    bool needed = replacement->face_batch_eligible && replacement->preempt_priority &&
                  asset_request_is_queued(replacement) &&
                  asset_active_stream_count() >= ASSET_STREAM_ACTIVE_MAX;
    SDL_UnlockMutex(asset_mutex);
    return needed;
}

static bool asset_batch_open(socket_t *sc, asset_request_t *first) {
    HARD_ASSERT(first != NULL);
    HARD_ASSERT(first->face_batch_eligible);

    uint64_t generic_barrier = UINT64_MAX;
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (asset_request_is_queued(request) && !request->face_batch_eligible &&
            request->sequence < generic_barrier) {
            generic_barrier = request->sequence;
        }
    }

    bool priority = first->preempt_priority;
    bool isolated = first->preempt_isolated;
    asset_request_t *members[ASSET_FACE_BATCH_MAX] = {0};
    uint16_t faces[ASSET_FACE_BATCH_MAX] = {0};
    size_t count = 0;
    if (isolated) {
        members[count] = first;
        faces[count] = first->face;
        count++;
    }
    /* Visible bursts retain batching throughput, but an intent-backed
     * replacement reserves one physical stream while ordinary priority faces
     * may only batch with each other. Speculative siblings never join either. */
    while (!isolated && count < ASSET_FACE_BATCH_MAX) {
        asset_request_t *oldest = NULL;
        HASH_ITER(hh, asset_requests, request, next) {
            if (!asset_request_is_queued(request) || !request->face_batch_eligible ||
                request->preempt_priority != priority || request->preempt_isolated ||
                request->sequence >= generic_barrier) {
                continue;
            }
            bool selected = false;
            for (size_t i = 0; i < count; i++) {
                selected |= members[i] == request;
            }
            if (!selected && (oldest == NULL || request->sequence < oldest->sequence)) {
                oldest = request;
            }
        }
        if (oldest == NULL) {
            break;
        }
        members[count] = oldest;
        faces[count] = oldest->face;
        count++;
    }
    HARD_ASSERT(count != 0);
    HARD_ASSERT(members[0] == first);

    packet_struct *wire_request = packet_new(0, SOCKET_FACE_BATCH_REQUEST_MAX_SIZE, 0);
    if (!socket_face_batch_request_append(wire_request, faces, count) ||
        !packet_writer_finish(wire_request)) {
        packet_free(wire_request);
        return false;
    }
    socket_stream_t *stream = socket_stream_open(sc, SOCKET_STREAM_FACE_BATCH);
    if (stream == NULL) {
        packet_free(wire_request);
        return false;
    }

    asset_batch_t *batch = xcalloc(1, sizeof(*batch));
    batch->stream = stream;
    batch->wire_request = wire_request;
    batch->count = count;
    batch->priority = priority;
    batch->transport_state = ASSET_BATCH_SEND_REQUEST;
    memcpy(batch->members, members, count * sizeof(*members));
    memcpy(batch->faces, faces, count * sizeof(*faces));
    for (size_t i = 0; i < count; i++) {
        HARD_ASSERT(members[i]->batch == NULL);
        members[i]->batch = batch;
        members[i]->preempt_priority = false;
        members[i]->preempt_isolated = false;
        if (members[i]->wire_request != NULL) {
            packet_free(members[i]->wire_request);
            members[i]->wire_request = NULL;
        }
    }
    batch->next = asset_batches;
    asset_batches = batch;
    return true;
}

static bool asset_request_open(socket_t *sc, asset_request_t *request) {
    HARD_ASSERT(request != NULL);
    if ((asset_transport_capabilities & ASSET_TRANSPORT_CAP_FACE_BATCH) != 0 &&
        request->face_batch_eligible) {
        return asset_batch_open(sc, request);
    }
    request->stream = socket_stream_open(sc, SOCKET_STREAM_ASSET);
    if (request->stream == NULL) {
        return false;
    }
    request->transport_state = ASSET_TRANSPORT_SEND_REQUEST;
    request->preempt_priority = false;
    request->preempt_isolated = false;
    return true;
}

bool asset_requests_service(socket_t *sc, bool *write_pending) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(write_pending != NULL);
    *write_pending = false;
    if (asset_mutex == NULL) {
        return false;
    }
    SDL_LockMutex(asset_mutex);

    asset_batch_t **batch_link = &asset_batches;
    while (*batch_link != NULL) {
        asset_batch_t *batch = *batch_link;
        if (!batch->preempt_requested) {
            batch_link = &batch->next;
            continue;
        }
        *batch_link = batch->next;
        asset_batch_preempt(batch);
        asset_batch_destroy(batch);
    }

    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (!request->preempt_requested) {
            continue;
        }
        request->preempt_requested = false;
        if (request->stream != NULL) {
            socket_stream_reset(request->stream, SOCKET_STREAM_ERROR_CANCELLED);
            socket_stream_destroy(request->stream);
            request->stream = NULL;
        }
        if (!request->cancelled && request->state == ASSET_REQUEST_PENDING) {
            asset_request_mark_preempted(request);
        }
    }

    size_t active = asset_active_stream_count();
    while (active < ASSET_STREAM_ACTIVE_MAX) {
        asset_request_t *oldest = asset_request_oldest_queued();
        if (oldest == NULL) {
            break;
        }
        if (!asset_request_open(sc, oldest)) {
            break;
        }
        active++;
    }

    bool progressed = false;
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->stream != NULL) {
            progressed |= asset_request_service(request);
        }
    }
    batch_link = &asset_batches;
    while (*batch_link != NULL) {
        asset_batch_t *batch = *batch_link;
        if (!batch->finished) {
            progressed |= asset_batch_service(batch);
        }
        if (batch->finished) {
            *batch_link = batch->next;
            asset_batch_destroy(batch);
        } else {
            batch_link = &batch->next;
        }
    }
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->cancelled && request->stream == NULL && request->batch == NULL) {
            HASH_DEL(asset_requests, request);
            asset_request_destroy(request);
        }
    }
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->state == ASSET_REQUEST_PENDING &&
            request->transport_state == ASSET_TRANSPORT_SEND_REQUEST) {
            *write_pending = true;
            break;
        }
    }
    if (!*write_pending) {
        for (asset_batch_t *batch = asset_batches; batch != NULL; batch = batch->next) {
            if (batch->transport_state == ASSET_BATCH_SEND_REQUEST) {
                *write_pending = true;
                break;
            }
        }
    }
    SDL_UnlockMutex(asset_mutex);
    return progressed;
}

void asset_requests_disconnect(void) {
    if (asset_mutex == NULL) {
        return;
    }
    SDL_LockMutex(asset_mutex);
    asset_transport_connected = false;
    asset_transport_capabilities = 0;
    while (asset_batches != NULL) {
        asset_batch_t *batch = asset_batches;
        asset_batches = batch->next;
        asset_batch_disconnect(batch);
        asset_batch_destroy(batch);
    }
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        if (request->stream != NULL) {
            socket_stream_reset(request->stream, SOCKET_STREAM_ERROR_CANCELLED);
            socket_stream_destroy(request->stream);
            request->stream = NULL;
        }
        if (request->state == ASSET_REQUEST_PENDING) {
            request->state = ASSET_REQUEST_ERROR;
        }
        if (request->references == 0) {
            HASH_DEL(asset_requests, request);
            asset_request_destroy(request);
        }
    }
    SDL_UnlockMutex(asset_mutex);
}

void asset_requests_deinit(void) {
    if (asset_mutex == NULL) {
        return;
    }
    SDL_LockMutex(asset_mutex);
    asset_transport_connected = false;
    asset_transport_capabilities = 0;
    while (asset_batches != NULL) {
        asset_batch_t *batch = asset_batches;
        asset_batches = batch->next;
        for (size_t i = batch->current; i < batch->count; i++) {
            if (batch->members[i] != NULL) {
                batch->members[i]->batch = NULL;
                batch->members[i] = NULL;
            }
        }
        asset_batch_destroy(batch);
    }
    asset_request_t *request, *next;
    HASH_ITER(hh, asset_requests, request, next) {
        HASH_DEL(asset_requests, request);
        asset_request_destroy(request);
    }
    SDL_UnlockMutex(asset_mutex);
    SDL_DestroyMutex(asset_mutex);
    asset_mutex = NULL;
    asset_request_sequence = 0;
}
