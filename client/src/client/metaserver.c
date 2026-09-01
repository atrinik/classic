/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Handles the QUIC server directory and its client-side server list.
 */

#include <metaserver.h>
#include <metaserver_options.h>
#include <client.h>
#include <join_credentials.h>
#include <main.h>
#include <wrapper.h>
#include <toolkit/logger.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <toolkit/curl.h>
#include <toolkit/datetime.h>
#include <toolkit/string.h>
#include <toolkit/metaserver_url.h>
#include "metaserver_directory.h"
#include "metaserver_private.h"

/** Are we connecting to the metaserver? */
static int metaserver_connecting;
/** Mutex to protect ::metaserver_connecting. */
static SDL_Mutex *metaserver_connecting_mutex;
/** The list of the servers. */
static server_struct *server_head;
/** Number of the servers. */
static size_t server_count;
/** Mutex to protect ::server_head and ::server_count. */
static SDL_Mutex *server_head_mutex;
/** Joinable directory worker, retained until completion is observed. */
static SDL_Thread *metaserver_worker;
/** Is metaserver enabled? */
static bool enabled;

static bool metaserver_etag_valid(const char *value) {
    size_t size = strlen(value);
    if (size < 2 || size > 255 || value[0] != '"' || value[size - 1] != '"') {
        return false;
    }
    for (size_t i = 1; i + 1 < size; i++) {
        unsigned char cp = (unsigned char)value[i];
        if (cp <= 0x20U || cp >= 0x7fU || cp == '"' || cp == '\\') {
            return false;
        }
    }
    return true;
}

static bool metaserver_response_headers_valid(curl_request_t *request) {
    size_t headers_size;
    const char *headers = curl_request_get_header(request, &headers_size);
    if (headers == NULL || headers_size == 0 || headers[headers_size] != '\0') {
        return false;
    }
    size_t pos = 0;
    char line[HUGE_BUF];
    size_t content_types = 0;
    size_t etags = 0;
    while (string_get_word(headers, &pos, '\n', VS(line), 0)) {
        char *cps[2];
        if (string_split(line, cps, arraysize(cps), ':') != arraysize(cps)) {
            continue;
        }
        string_whitespace_trim(cps[0]);
        string_whitespace_trim(cps[1]);
        if (strcasecmp(cps[0], "Content-Type") == 0) {
            content_types++;
            if (strcasecmp(cps[1], "application/xml; charset=utf-8") != 0) {
                return false;
            }
        } else if (strcasecmp(cps[0], "ETag") == 0) {
            etags++;
            if (!metaserver_etag_valid(cps[1])) {
                return false;
            }
        }
    }
    return content_types == 1 && etags == 1;
}

static char *metaserver_cache_path(const client_metaserver_endpoint_t *endpoint) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    char scope[65];
    bool ok =
        context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(context, endpoint->directory_url, strlen(endpoint->directory_url) + 1) ==
            1 &&
        EVP_DigestUpdate(context,
                         endpoint->rendezvous_origin,
                         strlen(endpoint->rendezvous_origin) + 1) == 1 &&
        EVP_DigestFinal_ex(context, digest, &digest_size) == 1 && digest_size == 32 &&
        string_tohex(digest, digest_size, VS(scope), false) == 64;
    EVP_MD_CTX_free(context);
    if (!ok) {
        return NULL;
    }
    char relative[HUGE_BUF];
    if (snprintf(VS(relative), DIRECTORY_CACHE "/metaserver-v4-%s.xml", scope) >=
        (int)sizeof(relative)) {
        return NULL;
    }
    return file_path(relative, "wb");
}

static bool metaserver_cached_snapshot(const char *body,
                                       size_t body_size,
                                       metaserver_directory_snapshot_t **snapshot,
                                       bool *current,
                                       uint64_t now) {
    if (!metaserver_directory_parse(body, body_size, snapshot)) {
        return false;
    }
    *current = metaserver_directory_current(*snapshot, now);
    return true;
}

void metaserver_init(void) {
    server_head = NULL;
    server_count = 0;
    enabled = client_metaserver_options_enabled(&clioption_settings.metaservers);
    metaserver_connecting = enabled ? 1 : 0;
    metaserver_connecting_mutex = SDL_CreateMutex();
    server_head_mutex = SDL_CreateMutex();
    metaserver_worker = NULL;
    if (metaserver_connecting_mutex == NULL || server_head_mutex == NULL) {
        LOG(ERROR, "Could not create metaserver mutexes: %s", SDL_GetError());
        exit(EXIT_FAILURE);
    }
}

void metaserver_server_free(server_struct *server) {
    HARD_ASSERT(server != NULL);

    free(server->hostname);
    free(server->server_id);
    free(server->quic_certificate_sha256);
    free(server->rendezvous_origin);
    client_attempt_secrets_clear(&server->join_password, NULL, &server->rendezvous_invite);
    free(server->name);
    free(server->version);
    free(server->desc);
    free(server);
}

void metaserver_server_add(server_struct *server) {
    HARD_ASSERT(server != NULL);

    SDL_LockMutex(server_head_mutex);
    DL_PREPEND(server_head, server);
    server_count++;
    SDL_UnlockMutex(server_head_mutex);
}

bool metaserver_rendezvous_url(const server_struct *server, char *url, size_t url_size) {
    if (server == NULL || server->server_id == NULL || server->rendezvous_origin == NULL) {
        return false;
    }
    return metaserver_url_rendezvous(server->rendezvous_origin,
                                     server->server_id,
                                     "client",
                                     url,
                                     url_size);
}

server_struct *server_get_id(size_t num) {
    server_struct *node;
    size_t i;

    SDL_LockMutex(server_head_mutex);
    for (node = server_head, i = 0; node; node = node->next, i++) {
        if (i == num) {
            break;
        }
    }
    SDL_UnlockMutex(server_head_mutex);
    return node;
}

size_t server_get_count(void) {
    SDL_LockMutex(server_head_mutex);
    size_t count = server_count;
    SDL_UnlockMutex(server_head_mutex);
    return count;
}

int ms_connecting(int val) {
    SDL_LockMutex(metaserver_connecting_mutex);
    int connecting = metaserver_connecting;
    if (val != -1) {
        metaserver_connecting = val;
    }
    SDL_UnlockMutex(metaserver_connecting_mutex);
    return connecting;
}

void metaserver_clear_data(void) {
    if (selected_server != NULL) {
        client_attempt_secrets_clear(&selected_server->join_password,
                                     &clioption_settings.join_password,
                                     &selected_server->rendezvous_invite);
        selected_server = NULL;
    }
    SDL_LockMutex(server_head_mutex);
    server_struct *node, *tmp;
    DL_FOREACH_SAFE(server_head, node, tmp) {
        DL_DELETE(server_head, node);
        metaserver_server_free(node);
    }
    server_count = 0;
    SDL_UnlockMutex(server_head_mutex);
}

void metaserver_deinit(void) {
    if (metaserver_worker != NULL) {
        SDL_WaitThread(metaserver_worker, NULL);
        metaserver_worker = NULL;
    }

    metaserver_clear_data();
    SDL_DestroyMutex(server_head_mutex);
    SDL_DestroyMutex(metaserver_connecting_mutex);
    server_head_mutex = NULL;
    metaserver_connecting_mutex = NULL;
}

server_struct *metaserver_add(const char *hostname,
                              int port,
                              const char *name,
                              const char *version,
                              const char *desc) {
    server_struct *node = xcalloc(1, sizeof(*node));
    node->port = port;
    node->hostname = xstrdup(hostname);
    node->name = xstrdup(name);
    node->version = xstrdup(version);
    node->desc = xstrdup(desc);

    metaserver_server_add(node);
    return node;
}

int metaserver_thread(void *dummy) {
    (void)dummy;

    for (size_t i = clioption_settings.metaservers.count; i > 0; i--) {
        const client_metaserver_endpoint_t *endpoint =
            &clioption_settings.metaservers.endpoints[i - 1];
        time_t current_time = time(NULL);
        if (current_time < 0) {
            continue;
        }
        uint64_t now = (uint64_t)current_time;
        char *cache_path = metaserver_cache_path(endpoint);
        char *cached_body = NULL;
        size_t cached_body_size = 0;
        metaserver_directory_snapshot_t *cached_snapshot = NULL;
        bool cache_current = false;
        bool cache_valid = cache_path != NULL &&
                           curl_cache_read(cache_path,
                                           METASERVER_DIRECTORY_BODY_MAX,
                                           &cached_body,
                                           &cached_body_size) &&
                           metaserver_cached_snapshot(cached_body,
                                                      cached_body_size,
                                                      &cached_snapshot,
                                                      &cache_current,
                                                      now);

        curl_request_t *request =
            curl_request_create(endpoint->directory_url, CURL_PKEY_TRUST_SYSTEM);
        curl_request_set_follow_redirects(request, false);
        curl_request_set_max_body(request, METASERVER_DIRECTORY_BODY_MAX);
        curl_request_set_max_header(request, 16384);
        curl_request_set_timeout(request, 15000);
        if (cache_valid) {
            curl_request_set_path(request, cache_path);
        }
        curl_request_do_get(request);
        size_t body_size;
        char *body = curl_request_get_body(request, &body_size);
        int http_code = curl_request_get_http_code(request);
        bool parsed = false;
        if (curl_request_get_state(request) == CURL_STATE_OK && body != NULL &&
            (http_code == 200 || http_code == 304)) {
            metaserver_directory_snapshot_t *received_snapshot = NULL;
            bool received_current = false;
            bool received_valid = metaserver_cached_snapshot(body,
                                                             body_size,
                                                             &received_snapshot,
                                                             &received_current,
                                                             now);
            bool response_valid =
                http_code == 304 || (metaserver_response_headers_valid(request) &&
                                     metaserver_directory_replacement_valid(received_snapshot,
                                                                            body,
                                                                            body_size,
                                                                            cached_snapshot,
                                                                            cached_body,
                                                                            cached_body_size));
            if (received_valid && received_current && response_valid) {
                uint64_t accepted_generation;
                parsed = metaserver_direct_parse(body,
                                                 body_size,
                                                 endpoint->rendezvous_origin,
                                                 now,
                                                 cache_valid ? cached_snapshot->generation : 0,
                                                 &accepted_generation);
                if (parsed && http_code == 200 && !curl_request_cache_commit(request)) {
                    LOG(ERROR, "Could not persist the validated metaserver directory cache");
                }
            }
            metaserver_directory_free(received_snapshot);
        }
        curl_request_free(request);
        if (!parsed && cache_valid && cache_current) {
            uint64_t accepted_generation;
            parsed = metaserver_direct_parse(cached_body,
                                             cached_body_size,
                                             endpoint->rendezvous_origin,
                                             now,
                                             cached_snapshot->generation,
                                             &accepted_generation);
        }
        metaserver_directory_free(cached_snapshot);
        free(cached_body);
        free(cache_path);
        if (parsed) {
            break;
        }
    }

    SDL_LockMutex(metaserver_connecting_mutex);
    metaserver_connecting = 0;
    SDL_UnlockMutex(metaserver_connecting_mutex);
    return 0;
}

void metaserver_get_servers(void) {
    if (!enabled) {
        return;
    }

    if (metaserver_worker != NULL) {
        SDL_WaitThread(metaserver_worker, NULL);
        metaserver_worker = NULL;
    }

    SDL_LockMutex(metaserver_connecting_mutex);
    metaserver_connecting = 1;
    SDL_UnlockMutex(metaserver_connecting_mutex);

    metaserver_worker = SDL_CreateThread(metaserver_thread, "metaserver", NULL);
    if (metaserver_worker == NULL) {
        LOG(ERROR, "Metaserver thread creation failed: %s", SDL_GetError());
        exit(1);
    }
}
