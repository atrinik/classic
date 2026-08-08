/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Metaserver updating related code.
 */

#include <global.h>
#include <server_main.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <toolkit/curl.h>
#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <toolkit/rendezvous.h>
#include <player.h>
#include <server.h>
#include <metaserver_internal.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <curl/curl.h>
#include <ctype.h>

/**
 * Used to hold metaserver statistics.
 */
static struct {
    uint64_t num; ///< Number of successful updates.

    uint64_t num_failed; ///< Number of failed updates.

    uint64_t rendezvous_reconnects; ///< Rendezvous reconnect attempts.

    time_t last; ///< Last successful update.

    time_t last_failed; ///< Last failed update.
} stats;

/**
 * Where the metaserver key file is located.
 */
#define METASERVER_KEY_FILE "metaserver_key"

/**
 * Mutex for the metaserver stats.
 */
static pthread_mutex_t stats_lock;

/**
 * cURL request structure.
 */
static curl_request_t *current_request = NULL;
/**
 * Mutex for the current request pointer.
 */
static pthread_mutex_t request_lock;
/**
 * Number of players.
 */
static uint32_t request_num_players = 0;
/**
 * Keeps track of whether the generate metaserver key is new or not.
 */
static bool key_is_new = false;
static rendezvous_invite_t metaserver_invite;
static bool metaserver_invite_active;
static unsigned char metaserver_synthetic_invite_secret[RENDEZVOUS_SECRET_SIZE];

#define METASERVER_INVITE_FILE "rendezvous-invite"

static void metaserver_invite_path(char *path, size_t path_size) {
    if (*settings.rendezvous_invite_file != '\0') {
        snprintf(path, path_size, "%s", settings.rendezvous_invite_file);
    } else {
        snprintf(path, path_size, "%s/%s", settings.datapath, METASERVER_INVITE_FILE);
    }
}

static bool metaserver_invite_read(const char *path, const char *server_id, bool *not_found) {
    HARD_ASSERT(not_found != NULL);

    *not_found = false;
    char text[RENDEZVOUS_INVITE_TEXT_SIZE];
    bool permissive_mode = false;
    path_secret_error_t error = path_read_secret(path, VS(text), &permissive_mode);
    *not_found = error == PATH_SECRET_NOT_FOUND;
    bool ok = error == PATH_SECRET_OK && !permissive_mode &&
              rendezvous_invite_parse(text, &metaserver_invite) &&
              rendezvous_invite_valid_at(&metaserver_invite, server_id, (uint64_t)time(NULL));
    OPENSSL_cleanse(text, sizeof(text));
    if (!ok) {
        rendezvous_invite_cleanse(&metaserver_invite);
        if (*not_found) {
            return false;
        }
        if (error != PATH_SECRET_OK) {
            LOG(ERROR,
                "Cannot load rendezvous invite file %s: %s",
                path,
                path_secret_error_string(error));
        } else if (permissive_mode) {
            LOG(ERROR,
                "Rendezvous invite file %s must be accessible only to the current OS user",
                path);
        } else {
            LOG(ERROR,
                "Rendezvous invite file %s is malformed, expired, too long-lived, or belongs "
                "to another server; delete it and restart to rotate",
                path);
        }
    }
    return ok;
}

static bool metaserver_invite_create(const char *path, const char *server_id) {
    uint64_t now = (uint64_t)time(NULL);
    char text[RENDEZVOUS_INVITE_TEXT_SIZE];
    if (now > UINT64_MAX - RENDEZVOUS_INVITE_LIFETIME_MAX ||
        !rendezvous_invite_generate(server_id,
                                    now + RENDEZVOUS_INVITE_LIFETIME_MAX,
                                    &metaserver_invite) ||
        !rendezvous_invite_render(&metaserver_invite, VS(text))) {
        goto out;
    }

    path_secret_create_result_t result = path_secret_create_atomic(path, text, strlen(text));
    if (result == PATH_SECRET_CREATE_OK) {
        LOG(SYSTEM,
            "Created protected rendezvous invite file %s; share this file with invited players",
            path);
        OPENSSL_cleanse(text, sizeof(text));
        return true;
    }
    if (result == PATH_SECRET_CREATE_EXISTS) {
        bool not_found;
        rendezvous_invite_cleanse(&metaserver_invite);
        OPENSSL_cleanse(text, sizeof(text));
        return metaserver_invite_read(path, server_id, &not_found);
    }
    LOG(ERROR, "Cannot securely create rendezvous invite file %s", path);

out:
    rendezvous_invite_cleanse(&metaserver_invite);
    OPENSSL_cleanse(text, sizeof(text));
    return false;
}

static bool metaserver_invite_init(void) {
    rendezvous_invite_cleanse(&metaserver_invite);
    OPENSSL_cleanse(metaserver_synthetic_invite_secret, sizeof(metaserver_synthetic_invite_secret));
    metaserver_invite_active = false;
    if (*settings.join_password == '\0') {
        return true;
    }
    if (RAND_priv_bytes(VS(metaserver_synthetic_invite_secret)) != 1) {
        return false;
    }
    char server_id[65], path[HUGE_BUF];
    if (!socket_server_quic_identity(server_id)) {
        return false;
    }
    metaserver_invite_path(VS(path));
    bool not_found;
    bool ok = metaserver_invite_read(path, server_id, &not_found);
    if (!ok && not_found) {
        ok = metaserver_invite_create(path, server_id);
    }
    metaserver_invite_active = ok;
    OPENSSL_cleanse(server_id, sizeof(server_id));
    return ok;
}

static bool metaserver_identity(char *identity, size_t identity_size) {
    HARD_ASSERT(identity_size >= 65);
    return socket_server_quic_identity(identity);
}

static void metaserver_key_path(char *path, size_t path_size) {
    snprintf(path, path_size, "%s/%s", settings.datapath, METASERVER_KEY_FILE);
}

bool metaserver_rendezvous_token_parse(const char *body, size_t body_size, char token[65]) {
    HARD_ASSERT(token != NULL);

    OPENSSL_cleanse(token, 65);
    static const char prefix[] = "\"rendezvousToken\":\"";
    const size_t required = sizeof(prefix) - 1 + 65;
    if (body == NULL || body_size < required) {
        return false;
    }

    for (size_t offset = 0; offset <= body_size - required; offset++) {
        if (memcmp(body + offset, prefix, sizeof(prefix) - 1) != 0) {
            continue;
        }
        const char *value = body + offset + sizeof(prefix) - 1;
        if (value[64] != '\"') {
            continue;
        }
        memcpy(token, value, 64);
        token[64] = '\0';
        if (string_is_hex_fixed(token, 64, true)) {
            return true;
        }
        OPENSSL_cleanse(token, 65);
    }

    return false;
}

#if LIBCURL_VERSION_NUM >= 0x075600
#define RENDEZVOUS_PUNCH_JOBS_MAX 64
#define RENDEZVOUS_PUNCH_GRACE_MS 200
#define RENDEZVOUS_AUTH_DEADLINE_MS 15000U

static pthread_mutex_t rendezvous_lock;
static pthread_mutex_t rendezvous_disclosure_lock;
static pthread_cond_t rendezvous_condition;
static pthread_t rendezvous_thread;
typedef enum rendezvous_thread_state {
    RENDEZVOUS_THREAD_STOPPED,
    RENDEZVOUS_THREAD_RUNNING,
    RENDEZVOUS_THREAD_EXITED
} rendezvous_thread_state_t;
static rendezvous_thread_state_t rendezvous_thread_state;
static bool rendezvous_shutdown;
static uint64_t rendezvous_generation;

typedef struct rendezvous_args {
    char url[HUGE_BUF];
    char token[65];
    uint64_t generation;
    bool authorization_required;
} rendezvous_args_t;

typedef struct rendezvous_punch_job {
    socket_punch_pacer_t pacer;
    unsigned int punches_sent;
    uint16_t port;
    char host[65];
    char ticket[65];
} rendezvous_punch_job_t;

typedef enum metaserver_rendezvous_frame_result {
    METASERVER_RENDEZVOUS_FRAME_IGNORED,
    METASERVER_RENDEZVOUS_FRAME_HANDLED,
    METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR,
    METASERVER_RENDEZVOUS_FRAME_CANCELLED
} metaserver_rendezvous_frame_result_t;

static bool metaserver_rendezvous_current_locked(uint64_t generation) {
    return metaserver_rendezvous_generation_allows(rendezvous_generation,
                                                   generation,
                                                   rendezvous_shutdown);
}

static metaserver_rendezvous_frame_result_t
metaserver_rendezvous_send(CURL *curl, const char *frame, uint64_t generation) {
    pthread_mutex_lock(&rendezvous_disclosure_lock);
    pthread_mutex_lock(&rendezvous_lock);
    bool current = metaserver_rendezvous_current_locked(generation);
    pthread_mutex_unlock(&rendezvous_lock);
    if (!current) {
        pthread_mutex_unlock(&rendezvous_disclosure_lock);
        return METASERVER_RENDEZVOUS_FRAME_CANCELLED;
    }
    size_t length = strlen(frame), sent = 0;
    bool ok =
        curl_ws_send(curl, frame, length, &sent, 0, CURLWS_TEXT) == CURLE_OK && sent == length;
    pthread_mutex_unlock(&rendezvous_disclosure_lock);
    return ok ? METASERVER_RENDEZVOUS_FRAME_HANDLED : METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
}

static metaserver_rendezvous_frame_result_t
metaserver_rendezvous_auth_init(CURL *curl,
                                metaserver_rendezvous_auth_job_t *jobs,
                                const char *ticket,
                                const char *invite_id,
                                uint64_t generation) {
    metaserver_rendezvous_auth_job_t *job = NULL;
    metaserver_rendezvous_auth_claim_t claim =
        metaserver_rendezvous_auth_claim(jobs,
                                         METASERVER_RENDEZVOUS_AUTH_JOBS_MAX,
                                         ticket,
                                         datetime_monotonic_ms() + RENDEZVOUS_AUTH_DEADLINE_MS,
                                         &job);
    if (claim != METASERVER_RENDEZVOUS_AUTH_CLAIM_OK) {
        return METASERVER_RENDEZVOUS_FRAME_IGNORED;
    }
    if (RAND_priv_bytes(VS(job->challenge)) != 1) {
        metaserver_rendezvous_auth_clear(job);
        return METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
    }

    uint64_t now = (uint64_t)time(NULL);
    job->known_invite =
        metaserver_invite_active &&
        rendezvous_invite_valid_at(&metaserver_invite, metaserver_invite.server_id, now) &&
        CRYPTO_memcmp(invite_id, metaserver_invite.invite_id, RENDEZVOUS_INVITE_ID_HEX_SIZE) == 0;
    if (job->known_invite) {
        job->invite = metaserver_invite;
    } else {
        char server_id[65];
        if (!socket_server_quic_identity(server_id)) {
            metaserver_rendezvous_auth_clear(job);
            return METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
        }
        snprintf(VS(job->invite.server_id), "%s", server_id);
        snprintf(VS(job->invite.invite_id), "%s", invite_id);
        memcpy(job->invite.secret, metaserver_synthetic_invite_secret, sizeof(job->invite.secret));
        job->invite.expiry = now == UINT64_MAX ? now : now + 1U;
        OPENSSL_cleanse(server_id, sizeof(server_id));
    }
    char response[RENDEZVOUS_FRAME_MAX + 1U];
    metaserver_rendezvous_frame_result_t result = METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
    if (rendezvous_auth_challenge_render(VS(response), ticket, job->challenge)) {
        result = metaserver_rendezvous_send(curl, response, generation);
        if (result == METASERVER_RENDEZVOUS_FRAME_HANDLED &&
            !rendezvous_server_auth_challenge_sent(&job->state)) {
            result = METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
        }
    }
    OPENSSL_cleanse(response, sizeof(response));
    if (result != METASERVER_RENDEZVOUS_FRAME_HANDLED) {
        metaserver_rendezvous_auth_clear(job);
    }
    return result;
}

static metaserver_rendezvous_frame_result_t
metaserver_rendezvous_auth_proof(CURL *curl,
                                 metaserver_rendezvous_auth_job_t *jobs,
                                 const char *message,
                                 uint64_t generation) {
    metaserver_rendezvous_auth_job_t *job = NULL;
    unsigned char proof[RENDEZVOUS_PROOF_SIZE] = {0};
    for (size_t i = 0; i < METASERVER_RENDEZVOUS_AUTH_JOBS_MAX; i++) {
        if (jobs[i].active && jobs[i].state == RENDEZVOUS_SERVER_AUTH_WAIT_PROOF &&
            rendezvous_auth_proof_parse(message, jobs[i].ticket, proof)) {
            job = &jobs[i];
            break;
        }
    }
    if (job == NULL) {
        OPENSSL_cleanse(proof, sizeof(proof));
        return METASERVER_RENDEZVOUS_FRAME_IGNORED;
    }

    bool proof_matches =
        rendezvous_invite_proof_verify(&job->invite, job->ticket, job->challenge, proof);
    bool authorized =
        proof_matches && job->known_invite && datetime_monotonic_ms() < job->deadline_ms &&
        rendezvous_invite_valid_at(&job->invite, job->invite.server_id, (uint64_t)time(NULL));
    char response[RENDEZVOUS_FRAME_MAX + 1U];
    metaserver_rendezvous_frame_result_t result = METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
    if (rendezvous_auth_result_render(VS(response), job->ticket, authorized)) {
        result = metaserver_rendezvous_send(curl, response, generation);
    }
    OPENSSL_cleanse(proof, sizeof(proof));
    OPENSSL_cleanse(response, sizeof(response));
    OPENSSL_cleanse(&job->invite, sizeof(job->invite));
    OPENSSL_cleanse(job->challenge, sizeof(job->challenge));
    job->known_invite = false;
    if (result != METASERVER_RENDEZVOUS_FRAME_HANDLED ||
        !rendezvous_server_auth_result_sent(&job->state, authorized) || !authorized) {
        metaserver_rendezvous_auth_clear(job);
    }
    return result;
}

static metaserver_rendezvous_frame_result_t
metaserver_rendezvous_send_complete(CURL *curl, const char *ticket, uint64_t generation) {
    char complete[128];
    if (!socket_rendezvous_complete_render(VS(complete), ticket)) {
        return METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR;
    }
    return metaserver_rendezvous_send(curl, complete, generation);
}

static metaserver_rendezvous_frame_result_t
metaserver_rendezvous_punch_update(CURL *curl, rendezvous_punch_job_t *jobs, uint64_t generation) {
    uint64_t now = datetime_monotonic_ms();
    for (size_t i = 0; i < RENDEZVOUS_PUNCH_JOBS_MAX; i++) {
        rendezvous_punch_job_t *job = &jobs[i];
        socket_punch_action_t action = socket_punch_pacer_poll(&job->pacer, now);
        if (action == SOCKET_PUNCH_WAIT) {
            continue;
        }

        if (action == SOCKET_PUNCH_SEND) {
            pthread_mutex_lock(&rendezvous_disclosure_lock);
            pthread_mutex_lock(&rendezvous_lock);
            bool current = metaserver_rendezvous_current_locked(generation);
            pthread_mutex_unlock(&rendezvous_lock);
            if (!current) {
                pthread_mutex_unlock(&rendezvous_disclosure_lock);
                return METASERVER_RENDEZVOUS_FRAME_CANCELLED;
            }
            bool sent = socket_server_quic_punch(job->host, job->port);
            pthread_mutex_unlock(&rendezvous_disclosure_lock);
            if (sent) {
                job->punches_sent++;
            }
            socket_punch_pacer_advance(&job->pacer, now, action);
            continue;
        }

        metaserver_rendezvous_frame_result_t result =
            metaserver_rendezvous_send_complete(curl, job->ticket, generation);
        if (result != METASERVER_RENDEZVOUS_FRAME_HANDLED) {
            return result;
        }
        LOG(DEBUG,
            "Completed rendezvous UDP punch window (sent %d/%d probes)",
            job->punches_sent,
            job->pacer.attempts);
        socket_punch_pacer_advance(&job->pacer, now, action);
    }
    return METASERVER_RENDEZVOUS_FRAME_HANDLED;
}

static bool metaserver_rendezvous_punch_schedule(rendezvous_punch_job_t *jobs,
                                                 const char *host,
                                                 uint16_t port,
                                                 const char *ticket) {
    rendezvous_punch_job_t *available = NULL;
    for (size_t i = 0; i < RENDEZVOUS_PUNCH_JOBS_MAX; i++) {
        if (jobs[i].pacer.active && strcmp(jobs[i].ticket, ticket) == 0) {
            available = &jobs[i];
            break;
        }
        if (!jobs[i].pacer.active && available == NULL) {
            available = &jobs[i];
        }
    }
    if (available == NULL) {
        return false;
    }

    snprintf(VS(available->host), "%s", host);
    snprintf(VS(available->ticket), "%s", ticket);
    available->port = port;
    available->punches_sent = 0;
    socket_punch_pacer_start(&available->pacer, datetime_monotonic_ms(), RENDEZVOUS_PUNCH_GRACE_MS);
    return true;
}

static bool metaserver_rendezvous_current(uint64_t generation) {
    pthread_mutex_lock(&rendezvous_lock);
    bool current = metaserver_rendezvous_current_locked(generation);
    pthread_mutex_unlock(&rendezvous_lock);
    return current;
}

static bool metaserver_rendezvous_wait(uint64_t generation, unsigned int timeout_ms) {
    struct timeval now;
    GETTIMEOFDAY(&now);
    uint64_t deadline_ns = (uint64_t)now.tv_usec * 1000 + (uint64_t)timeout_ms * 1000000;
    struct timespec deadline = {.tv_sec = now.tv_sec + (time_t)(deadline_ns / 1000000000),
                                .tv_nsec = (long)(deadline_ns % 1000000000)};

    pthread_mutex_lock(&rendezvous_lock);
    int wait_error = 0;
    while (metaserver_rendezvous_current_locked(generation) && wait_error == 0) {
        wait_error = pthread_cond_timedwait(&rendezvous_condition, &rendezvous_lock, &deadline);
    }
    bool current = metaserver_rendezvous_current_locked(generation);
    pthread_mutex_unlock(&rendezvous_lock);
    return current && wait_error == ETIMEDOUT;
}

static int metaserver_rendezvous_progress(void *data,
                                          curl_off_t download_total,
                                          curl_off_t download_now,
                                          curl_off_t upload_total,
                                          curl_off_t upload_now) {
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;

    rendezvous_args_t *args = data;
    return metaserver_rendezvous_current(args->generation) ? 0 : 1;
}

static bool metaserver_rendezvous_retry(const rendezvous_args_t *args,
                                        uint32_t *failures,
                                        uint32_t retry_after_seconds) {
    uint32_t random_value;
    if (RAND_bytes((unsigned char *)&random_value, sizeof(random_value)) != 1) {
        random_value = (uint32_t)(datetime_monotonic_ms() ^ args->generation ^ *failures);
    }
    uint32_t delay =
        metaserver_rendezvous_retry_delay_ms(*failures, retry_after_seconds, random_value);
    if (*failures != UINT32_MAX) {
        (*failures)++;
    }
    LOG(INFO, "Retrying the rendezvous control in %" PRIu32 " ms", delay);
    if (!metaserver_rendezvous_wait(args->generation, delay)) {
        return false;
    }

    pthread_mutex_lock(&stats_lock);
    stats.rendezvous_reconnects++;
    pthread_mutex_unlock(&stats_lock);
    return true;
}

static bool metaserver_rendezvous_message_type(const char *message, const char *type) {
    char prefix[64];
    int length = snprintf(VS(prefix), "{\"type\":\"%s\"", type);
    return length > 0 && (size_t)length < sizeof(prefix) &&
           strncmp(message, prefix, (size_t)length) == 0;
}

static void *metaserver_rendezvous_thread(void *data) {
    rendezvous_args_t *args = data;
    uint32_t failures = 0;
    while (metaserver_rendezvous_current(args->generation)) {
        uint32_t retry_after_seconds = 0;
        uint64_t connected_ms = 0;
        CURL *curl = curl_easy_init();
        struct curl_slist *headers = NULL;
        char authorization[sizeof("Authorization: Bearer ") + 64] = {0};
        if (curl == NULL) {
            LOG(ERROR, "Cannot allocate a rendezvous connection");
        } else {
            snprintf(VS(authorization), "Authorization: Bearer %s", args->token);
            headers = curl_slist_append(NULL, authorization);
            if (headers == NULL) {
                LOG(ERROR, "Cannot allocate rendezvous request headers");
            } else if (args->authorization_required) {
                struct curl_slist *protocol_headers =
                    curl_slist_append(headers,
                                      "Sec-WebSocket-Protocol: " RENDEZVOUS_INVITE_SUBPROTOCOL);
                if (protocol_headers == NULL) {
                    LOG(ERROR, "Cannot allocate rendezvous invite request headers");
                    curl_slist_free_all(headers);
                    headers = NULL;
                } else {
                    headers = protocol_headers;
                }
            }
        }

        if (curl != NULL && headers != NULL) {
            metaserver_rendezvous_headers_t response_headers = {0};
            curl_easy_setopt(curl, CURLOPT_URL, args->url);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
            curl_easy_setopt(curl,
                             CURLOPT_CONNECTTIMEOUT_MS,
                             METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, metaserver_rendezvous_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, args);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, metaserver_rendezvous_header);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
#ifdef WIN32
            curl_easy_setopt(curl, CURLOPT_CAINFO, "ca-bundle.crt");
#endif
            CURLcode result = curl_easy_perform(curl);
            /* The persistent frame loop owns its own cancellable waits. Do not
             * carry the blocking upgrade deadline into curl_ws_recv/send. */
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            retry_after_seconds =
                response_headers.has_retry_after ? response_headers.retry_after_seconds : 0;
            bool connected = result == CURLE_OK && http_code == 101 &&
                             metaserver_rendezvous_protocol_allows(&response_headers,
                                                                   args->authorization_required);
            if (!connected) {
                if (result != CURLE_OK) {
                    LOG(ERROR,
                        "Rendezvous connection failed (HTTP %ld): %s",
                        http_code,
                        curl_easy_strerror(result));
                } else if (http_code != 101) {
                    LOG(ERROR, "Rendezvous connection failed with HTTP status %ld", http_code);
                } else if (!args->authorization_required) {
                    LOG(ERROR,
                        "Rendezvous connection failed: passwordless control selected an "
                        "unexpected subprotocol");
                } else {
                    LOG(ERROR, "Rendezvous connection failed: invite subprotocol was not selected");
                }
            } else {
                uint64_t connected_at = datetime_monotonic_ms();
                rendezvous_punch_job_t punch_jobs[RENDEZVOUS_PUNCH_JOBS_MAX] = {0};
                metaserver_rendezvous_auth_job_t auth_jobs[METASERVER_RENDEZVOUS_AUTH_JOBS_MAX] = {
                    0};
                char message[RENDEZVOUS_FRAME_MAX + 1U] = {0};
                size_t used = 0;
                bool stop_control = false;
                while (!stop_control && metaserver_rendezvous_current(args->generation)) {
                    metaserver_rendezvous_auth_expire(auth_jobs,
                                                      arraysize(auth_jobs),
                                                      datetime_monotonic_ms());
                    metaserver_rendezvous_frame_result_t frame_result =
                        metaserver_rendezvous_punch_update(curl, punch_jobs, args->generation);
                    if (frame_result != METASERVER_RENDEZVOUS_FRAME_HANDLED) {
                        break;
                    }

                    socket_websocket_receive_state_t receive_state =
                        socket_websocket_receive(curl, VS(message), &used);
                    if (receive_state == SOCKET_WEBSOCKET_EMPTY) {
                        if (!metaserver_rendezvous_wait(args->generation, 20)) {
                            break;
                        }
                        continue;
                    }
                    if (receive_state == SOCKET_WEBSOCKET_PARTIAL) {
                        continue;
                    }
                    if (receive_state != SOCKET_WEBSOCKET_MESSAGE) {
                        break;
                    }

                    char host[65], ticket[65], invite_id[33];
                    uint16_t port;
                    if (args->authorization_required &&
                        rendezvous_auth_init_parse(message, ticket, invite_id)) {
                        frame_result = metaserver_rendezvous_auth_init(curl,
                                                                       auth_jobs,
                                                                       ticket,
                                                                       invite_id,
                                                                       args->generation);
                        stop_control = frame_result == METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR ||
                                       frame_result == METASERVER_RENDEZVOUS_FRAME_CANCELLED;
                    } else if (args->authorization_required &&
                               metaserver_rendezvous_message_type(message, "auth_proof")) {
                        frame_result = metaserver_rendezvous_auth_proof(curl,
                                                                        auth_jobs,
                                                                        message,
                                                                        args->generation);
                        stop_control = frame_result == METASERVER_RENDEZVOUS_FRAME_CONTROL_ERROR ||
                                       frame_result == METASERVER_RENDEZVOUS_FRAME_CANCELLED;
                    } else {
                        bool authorization_required = args->authorization_required;
                        metaserver_rendezvous_auth_job_t *authorized = NULL;
                        bool candidate_parsed = false;
                        if (authorization_required) {
                            for (size_t i = 0; i < arraysize(auth_jobs); i++) {
                                if (auth_jobs[i].active &&
                                    socket_rendezvous_client_candidate_parse(message,
                                                                             auth_jobs[i].ticket,
                                                                             true,
                                                                             auth_jobs[i].state,
                                                                             VS(host),
                                                                             &port,
                                                                             ticket)) {
                                    authorized = &auth_jobs[i];
                                    candidate_parsed = true;
                                    break;
                                }
                            }
                        } else {
                            candidate_parsed =
                                socket_rendezvous_client_candidate_parse(message,
                                                                         NULL,
                                                                         false,
                                                                         RENDEZVOUS_SERVER_AUTH_NEW,
                                                                         VS(host),
                                                                         &port,
                                                                         ticket);
                        }
                        if (!candidate_parsed &&
                            (metaserver_rendezvous_message_type(message, "auth_init") ||
                             metaserver_rendezvous_message_type(message, "auth_proof") ||
                             metaserver_rendezvous_message_type(message, "client_candidate"))) {
                            LOG(DEBUG, "Ignoring an invalid or stale rendezvous ticket frame");
                        } else if (!candidate_parsed) {
                            stop_control = true;
                        } else {
                            socket_direct_candidate_t candidates[SOCKET_DIRECT_MAX_CANDIDATES];
                            size_t count =
                                socket_server_quic_candidates(candidates, arraysize(candidates));
                            for (size_t i = 0; i < count; i++) {
                                char response[256];
                                if (!socket_rendezvous_server_candidate_render(
                                        VS(response),
                                        &candidates[i],
                                        ticket,
                                        authorization_required,
                                        authorized != NULL ? authorized->state
                                                           : RENDEZVOUS_SERVER_AUTH_NEW)) {
                                    stop_control = true;
                                } else {
                                    frame_result = metaserver_rendezvous_send(curl,
                                                                              response,
                                                                              args->generation);
                                    stop_control =
                                        frame_result != METASERVER_RENDEZVOUS_FRAME_HANDLED;
                                }
                                OPENSSL_cleanse(response, sizeof(response));
                                if (stop_control) {
                                    break;
                                }
                            }

                            if (!stop_control) {
                                LOG(INFO,
                                    "Opening a rendezvous UDP path to an %s client candidate",
                                    authorization_required ? "authorized" : "accepted");
                                if (!metaserver_rendezvous_punch_schedule(punch_jobs,
                                                                          host,
                                                                          port,
                                                                          ticket)) {
                                    LOG(ERROR, "Rendezvous UDP punch queue is full");
                                    frame_result =
                                        metaserver_rendezvous_send_complete(curl,
                                                                            ticket,
                                                                            args->generation);
                                    stop_control =
                                        frame_result != METASERVER_RENDEZVOUS_FRAME_HANDLED;
                                }
                            }
                            if (authorized != NULL) {
                                if (!rendezvous_server_auth_candidate_consume(&authorized->state)) {
                                    LOG(ERROR, "Cannot consume an authorized rendezvous ticket");
                                }
                                metaserver_rendezvous_auth_clear(authorized);
                            }
                        }
                    }
                    OPENSSL_cleanse(message, sizeof(message));
                    used = 0;
                }

                uint64_t disconnected_at = datetime_monotonic_ms();
                if (disconnected_at >= connected_at) {
                    connected_ms = disconnected_at - connected_at;
                }
                OPENSSL_cleanse(message, sizeof(message));
                OPENSSL_cleanse(auth_jobs, sizeof(auth_jobs));
                OPENSSL_cleanse(punch_jobs, sizeof(punch_jobs));
            }
        }

        if (curl != NULL) {
            curl_easy_cleanup(curl);
        }
        if (headers != NULL) {
            curl_slist_free_all(headers);
        }
        OPENSSL_cleanse(authorization, sizeof(authorization));
        if (!metaserver_rendezvous_current(args->generation)) {
            break;
        }
        failures = metaserver_rendezvous_retry_failures(failures, connected_ms);
        if (!metaserver_rendezvous_retry(args, &failures, retry_after_seconds)) {
            break;
        }
    }

    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_thread_state = RENDEZVOUS_THREAD_EXITED;
    pthread_cond_broadcast(&rendezvous_condition);
    pthread_mutex_unlock(&rendezvous_lock);
    OPENSSL_cleanse(args->token, sizeof(args->token));
    free(args);
    return NULL;
}

static bool metaserver_rendezvous_url(char *url, size_t url_size) {
    char quic_fingerprint[65];
    if (!settings.server_public || !socket_server_quic_identity(quic_fingerprint)) {
        return false;
    }

    return socket_rendezvous_url(settings.metaserver_url,
                                 quic_fingerprint,
                                 "server",
                                 url,
                                 url_size);
}

static void metaserver_rendezvous_start(const char *token) {
    rendezvous_args_t *args = xcalloc(1, sizeof(*args));
    snprintf(VS(args->token), "%s", token);
    args->authorization_required = *settings.join_password != '\0';
    if (!metaserver_rendezvous_url(VS(args->url))) {
        OPENSSL_cleanse(args->token, sizeof(args->token));
        free(args);
        return;
    }

    pthread_mutex_lock(&rendezvous_disclosure_lock);
    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_generation++;
    pthread_cond_broadcast(&rendezvous_condition);
    args->generation = rendezvous_generation;
    bool join_old = rendezvous_thread_state != RENDEZVOUS_THREAD_STOPPED;
    pthread_t old_thread = rendezvous_thread;
    pthread_mutex_unlock(&rendezvous_lock);
    pthread_mutex_unlock(&rendezvous_disclosure_lock);

    if (join_old) {
        pthread_join(old_thread, NULL);
    }

    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_thread_state = RENDEZVOUS_THREAD_STOPPED;
    if (rendezvous_shutdown || args->generation != rendezvous_generation) {
        pthread_mutex_unlock(&rendezvous_lock);
        OPENSSL_cleanse(args->token, sizeof(args->token));
        free(args);
        return;
    }
    int error = pthread_create(&rendezvous_thread, NULL, metaserver_rendezvous_thread, args);
    if (error != 0) {
        LOG(ERROR, "Failed to start the rendezvous thread");
        rendezvous_thread_state = RENDEZVOUS_THREAD_STOPPED;
        pthread_mutex_unlock(&rendezvous_lock);
        OPENSSL_cleanse(args->token, sizeof(args->token));
        free(args);
        return;
    }
    rendezvous_thread_state = RENDEZVOUS_THREAD_RUNNING;
    pthread_mutex_unlock(&rendezvous_lock);
}

static void metaserver_rendezvous_response(curl_request_t *request) {
    size_t body_size = 0;
    char *body = curl_request_get_body(request, &body_size);
    char value[65];
    if (!metaserver_rendezvous_token_parse(body, body_size, value)) {
        return;
    }
    metaserver_rendezvous_start(value);
    OPENSSL_cleanse(value, sizeof(value));
}
#endif

/**
 * Figure out whether the meta-server is enabled or not.
 *
 * @return
 * True if the meta-server is enabled, false otherwise.
 */
static bool metaserver_enabled(void) {
    if (settings.provision_scenario) {
        return false;
    }

    char identity[MAX_BUF];
    if (!metaserver_identity(VS(identity))) {
        return false;
    }

    if (settings.unit_tests) {
        return false;
    }

    return true;
}

/**
 * Initialize the metaserver.
 */
void metaserver_init(void) {
    if (!metaserver_enabled()) {
        return;
    }

    pthread_mutex_init(&stats_lock, NULL);
    pthread_mutex_init(&request_lock, NULL);
#if LIBCURL_VERSION_NUM >= 0x075600
    pthread_mutex_init(&rendezvous_lock, NULL);
    pthread_mutex_init(&rendezvous_disclosure_lock, NULL);
    pthread_cond_init(&rendezvous_condition, NULL);
    rendezvous_thread_state = RENDEZVOUS_THREAD_STOPPED;
    rendezvous_shutdown = false;
    rendezvous_generation = 0;
#endif
    if (!metaserver_invite_init()) {
        LOG(ERROR, "Protected rendezvous is disabled until the invite capability problem is fixed");
    }
    metaserver_info_update();
}

/**
 * Deinitialize the metaserver.
 */
void metaserver_deinit(void) {
    if (!metaserver_enabled()) {
        return;
    }

    pthread_mutex_lock(&request_lock);
    if (current_request != NULL) {
        pthread_mutex_unlock(&request_lock);
        curl_state_t state;
        do {
            pthread_mutex_lock(&request_lock);
            if (current_request == NULL) {
                pthread_mutex_unlock(&request_lock);
                break;
            }
            state = curl_request_get_state(current_request);
            pthread_mutex_unlock(&request_lock);
            sleep(1);
        } while (state == CURL_STATE_INPROGRESS);

        /* No other thread is working with the current request at this
         * point. */
        if (current_request != NULL) {
            curl_request_free(current_request);
            current_request = NULL;
        }
    } else {
        pthread_mutex_unlock(&request_lock);
    }

#if LIBCURL_VERSION_NUM >= 0x075600
    pthread_mutex_lock(&rendezvous_disclosure_lock);
    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_shutdown = true;
    rendezvous_generation++;
    pthread_cond_broadcast(&rendezvous_condition);
    bool join_rendezvous = rendezvous_thread_state != RENDEZVOUS_THREAD_STOPPED;
    pthread_t thread = rendezvous_thread;
    pthread_mutex_unlock(&rendezvous_lock);
    pthread_mutex_unlock(&rendezvous_disclosure_lock);
    if (join_rendezvous) {
        pthread_join(thread, NULL);
    }
    pthread_mutex_lock(&rendezvous_lock);
    rendezvous_thread_state = RENDEZVOUS_THREAD_STOPPED;
    pthread_mutex_unlock(&rendezvous_lock);
    pthread_cond_destroy(&rendezvous_condition);
    pthread_mutex_destroy(&rendezvous_lock);
    pthread_mutex_destroy(&rendezvous_disclosure_lock);
#endif

    rendezvous_invite_cleanse(&metaserver_invite);
    metaserver_invite_active = false;
    OPENSSL_cleanse(metaserver_synthetic_invite_secret, sizeof(metaserver_synthetic_invite_secret));

    pthread_mutex_destroy(&stats_lock);
    pthread_mutex_destroy(&request_lock);
}

/**
 * Check if the specified cURL request resulted in an error.
 *
 * @param request
 * Request to check.
 * @return
 * True if an error was processed, false otherwise.
 */
static bool metaserver_request_process_error(curl_request_t *request) {
    HARD_ASSERT(request != NULL);

    curl_state_t state = curl_request_get_state(request);
    int http_code = curl_request_get_http_code(request);
    if (state == CURL_STATE_OK && http_code == 200) {
        return false;
    }

    char *body = curl_request_get_body(request, NULL);
    LOG(SYSTEM,
        "Failed to update metaserver information "
        "(HTTP code: %d), response: %s",
        http_code,
        body != NULL ? body : "<empty>");

    pthread_mutex_lock(&stats_lock);
    stats.last_failed = time(NULL);
    stats.num_failed++;
    pthread_mutex_unlock(&stats_lock);
    return true;
}

/**
 * Callback received for publishing a metaserver update.
 *
 * @param request
 * cURL request.
 * @param user_data
 * NULL.
 */
static void metaserver_update_request(curl_request_t *request, void *user_data) {
    pthread_mutex_lock(&request_lock);
    current_request = NULL;

    if (metaserver_request_process_error(request)) {
        curl_state_t state = curl_request_get_state(request);
        int http_code = curl_request_get_http_code(request);
        metaserver_registration_key_action_t key_action =
            metaserver_registration_key_action(state, http_code, key_is_new);
        if (key_action == METASERVER_REGISTRATION_KEY_RETRY_ESTABLISHED) {
            /* An ambiguous failure may have happened after ownership committed.
             * Probe with the derived established-owner key before attempting
             * the same retained registration key again. */
            key_is_new = false;
        } else if (key_action == METASERVER_REGISTRATION_KEY_RETRY_REGISTRATION) {
            /* A 409 for registration=0 definitively means this identity has no
             * owner. Retry the exact retained key as a first claim. */
            key_is_new = true;
        } else if (key_action == METASERVER_REGISTRATION_KEY_DELETE) {
            char path[HUGE_BUF];
            metaserver_key_path(VS(path));

            if (unlink(path) == 0 || errno == ENOENT) {
                key_is_new = false;
            } else {
                LOG(ERROR, "Failed to unlink %s: %s (%d)", path, strerror(errno), errno);
            }
        }

        goto out;
    }

    key_is_new = false;
#if LIBCURL_VERSION_NUM >= 0x075600
    metaserver_rendezvous_response(request);
#endif

    pthread_mutex_lock(&stats_lock);
    stats.last = time(NULL);
    stats.num++;
    pthread_mutex_unlock(&stats_lock);

out:
    curl_request_free(request);
    pthread_mutex_unlock(&request_lock);
}

/**
 * Computes SHA-512 over up to three data segments.
 */
static bool metaserver_sha512(unsigned char digest[SHA512_DIGEST_LENGTH],
                              const void *data1,
                              size_t size1,
                              const void *data2,
                              size_t size2,
                              const void *data3,
                              size_t size3) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return false;
    }

    bool success = EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1 &&
                   EVP_DigestUpdate(ctx, data1, size1) == 1 &&
                   EVP_DigestUpdate(ctx, data2, size2) == 1 &&
                   (data3 == NULL || EVP_DigestUpdate(ctx, data3, size3) == 1);
    unsigned int digest_len = 0;
    success = success && EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 &&
              digest_len == SHA512_DIGEST_LENGTH;
    EVP_MD_CTX_free(ctx);
    return success;
}

/**
 * Acquires the key to use for metaserver authentication.
 *
 * @param[out] key
 * Will contain the key on success.
 * @param key_size
 * Size of the 'key' buffer.
 * @param otp
 * OTP from the metaserver.
 * @param cotp
 * Generated COTP.
 * @return
 * True on success, false on failure.
 */
static bool metaserver_get_key(char *key, size_t key_size, const char *otp, const char *cotp) {
    HARD_ASSERT(key != NULL);
    HARD_ASSERT(key_size == SHA512_DIGEST_LENGTH * 2 + 1);

    unsigned char tmp_key[SHA512_DIGEST_LENGTH];
    char path[HUGE_BUF];
    metaserver_key_path(VS(path));
    FILE *fp = fopen(path, "rb");
    if (fp == NULL && errno == ENOENT) {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            LOG(ERROR, "Failed to create %s: %s (%d)", path, strerror(errno), errno);
            return false;
        }

        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            int saved_errno = errno;
            close(fd);
            if (unlink(path) != 0) {
                LOG(ERROR, "Failed to unlink %s: %s (%d)", path, strerror(errno), errno);
            }
            LOG(ERROR,
                "Failed to open %s for writing: %s (%d)",
                path,
                strerror(saved_errno),
                saved_errno);
            return false;
        }

        unsigned char bytes[64];

        if (RAND_bytes(VS(bytes)) != 1) {
            LOG(ERROR, "RAND_bytes() failed: %s", ERR_error_string(ERR_get_error(), NULL));
            goto error_creating;
        }

        if (SHA512(VS(bytes), tmp_key) == NULL) {
            LOG(ERROR, "SHA512() failed: %s", ERR_error_string(ERR_get_error(), NULL));
            goto error_creating;
        }

        OPENSSL_cleanse(bytes, sizeof(bytes));
        key[SHA512_DIGEST_LENGTH] = '\0';

        if (fwrite(VS(tmp_key), 1, fp) != 1) {
            LOG(ERROR, "Failed to write to %s: %s (%d)", path, strerror(errno), errno);
            goto error_creating;
        }

        int close_result = fclose(fp);
        fp = NULL;
        if (close_result != 0) {
            LOG(ERROR, "Failed to close %s: %s (%d)", path, strerror(errno), errno);
            goto error_creating;
        }

        SOFT_ASSERT_LABEL(string_tohex(VS(tmp_key), key, key_size, false) == key_size - 1,
                          error_creating,
                          "string_tohex failed");
        string_tolower(key);
        key_is_new = true;

        OPENSSL_cleanse(tmp_key, sizeof(tmp_key));
        return true;

    error_creating:
        if (fp != NULL) {
            fclose(fp);
            fp = NULL;
        }
        if (unlink(path) != 0 && errno != ENOENT) {
            LOG(ERROR, "Failed to unlink %s: %s (%d)", path, strerror(errno), errno);
        }

        OPENSSL_cleanse(bytes, sizeof(bytes));
        OPENSSL_cleanse(tmp_key, sizeof(tmp_key));
        OPENSSL_cleanse(key, key_size);
        return false;
    } else if (fp == NULL) {
        LOG(ERROR, "Failed to open %s for reading: %s (%d)", path, strerror(errno), errno);
        return false;
    }

    if (fread(VS(tmp_key), 1, fp) != 1) {
        LOG(ERROR, "Failed to read from %s: %s (%d)", path, strerror(errno), errno);
        goto error_reading;
    }

    SOFT_ASSERT_LABEL(string_tohex(VS(tmp_key), key, key_size, false) == key_size - 1,
                      error_reading,
                      "string_tohex failed");
    string_tolower(key);

    if (key_is_new) {
        int close_result = fclose(fp);
        fp = NULL;
        if (close_result != 0) {
            LOG(ERROR, "Failed to close %s: %s (%d)", path, strerror(errno), errno);
            goto error_reading;
        }
        OPENSSL_cleanse(tmp_key, sizeof(tmp_key));
        return true;
    }

    char identity[65];
    if (!metaserver_identity(VS(identity)) ||
        !metaserver_sha512(tmp_key, key, key_size - 1, identity, strlen(identity), NULL, 0)) {
        LOG(ERROR, "SHA-512 digest failed: %s", ERR_error_string(ERR_get_error(), NULL));
        OPENSSL_cleanse(identity, sizeof(identity));
        goto error_reading;
    }
    OPENSSL_cleanse(identity, sizeof(identity));

    SOFT_ASSERT_LABEL(string_tohex(VS(tmp_key), key, key_size, false) == key_size - 1,
                      error_reading,
                      "string_tohex failed");
    string_tolower(key);

    int close_result = fclose(fp);
    fp = NULL;
    if (close_result != 0) {
        LOG(ERROR, "Failed to close %s: %s (%d)", path, strerror(errno), errno);
        goto error_reading;
    }

    if (!metaserver_sha512(tmp_key, otp, strlen(otp), key, key_size - 1, cotp, strlen(cotp))) {
        LOG(ERROR, "SHA-512 digest failed: %s", ERR_error_string(ERR_get_error(), NULL));
        goto error_reading;
    }

    SOFT_ASSERT_LABEL(string_tohex(VS(tmp_key), key, key_size, false) == key_size - 1,
                      error_reading,
                      "string_tohex failed");
    string_tolower(key);

    OPENSSL_cleanse(tmp_key, sizeof(tmp_key));
    return true;

error_reading:
    if (fp != NULL) {
        fclose(fp);
    }
    OPENSSL_cleanse(key, key_size);
    OPENSSL_cleanse(tmp_key, sizeof(tmp_key));
    return false;
}

/**
 * Process the OTP GET request reply.
 *
 * @param request
 * cURL request.
 * @param user_data
 * NULL.
 */
static void metaserver_otp_request(curl_request_t *request, void *user_data) {
    unsigned char cotp[32] = {0};
    unsigned char cotp_digest[SHA512_DIGEST_LENGTH] = {0};
    char cotp_hash[SHA512_DIGEST_LENGTH * 2 + 1] = {0};
    char key[SHA512_DIGEST_LENGTH * 2 + 1] = {0};
    char *body = NULL, *otp = NULL;
    size_t body_size = 0, otp_length = 0;

    pthread_mutex_lock(&request_lock);
    current_request = NULL;

    if (metaserver_request_process_error(request)) {
        goto out;
    }

    body = curl_request_get_body(request, &body_size);
    if (body == NULL) {
        LOG(ERROR, "Failed to receive an OTP from metaserver");
        goto out;
    }

    const char *otp_identifier = "\"otp\": \"";
    const char *otp_pos = strstr(body, otp_identifier);
    if (otp_pos == NULL) {
        LOG(ERROR, "Malformed OTP response");
        goto out;
    }

    /* Jump over the OTP identifier */
    otp_pos += strlen(otp_identifier);

    const char *otp_end_pos = strstr(otp_pos, "\"");
    if (otp_end_pos == NULL) {
        LOG(ERROR, "Malformed OTP response");
        goto out;
    }

    otp_length = (size_t)(otp_end_pos - otp_pos);
    if (otp_length == 0) {
        LOG(ERROR, "Malformed OTP response");
        goto out;
    }

    if (RAND_bytes(VS(cotp)) != 1) {
        LOG(ERROR, "RAND_bytes() failed: %s", ERR_error_string(ERR_get_error(), NULL));
        goto out;
    }

    if (SHA512(VS(cotp), cotp_digest) == NULL) {
        LOG(ERROR, "SHA512() failed: %s", ERR_error_string(ERR_get_error(), NULL));
        goto out;
    }

    SOFT_ASSERT_LABEL(string_tohex(VS(cotp_digest), VS(cotp_hash), false) == sizeof(cotp_hash) - 1,
                      out,
                      "string_tohex failed");
    string_tolower(cotp_hash);

    otp = xstrndup(body + (otp_pos - body), otp_length);

    if (!metaserver_get_key(VS(key), otp, cotp_hash)) {
        goto out;
    }

    char url[HUGE_BUF];
    snprintf(VS(url), "%s/update", settings.metaserver_url);
    current_request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_cb(current_request, metaserver_update_request, NULL);

    curl_request_form_add(current_request, "version", PACKAGE_VERSION);
    curl_request_form_add(current_request, "text_comment", settings.server_desc);
    curl_request_form_add(current_request, "name", settings.server_name);
    curl_request_form_add(current_request, "otp", otp);
    curl_request_form_add(current_request, "cotp", cotp_hash);
    curl_request_form_add(current_request, "key", key);
    curl_request_form_add(current_request, "registration", key_is_new ? "1" : "0");
    char buf[32];
    snprintf(VS(buf), "%" PRIu32, request_num_players);
    curl_request_form_add(current_request, "num_players", buf);
    curl_request_form_add(current_request, "public", settings.server_public ? "1" : "0");
    curl_request_form_add(current_request,
                          "password_required",
                          *settings.join_password != '\0' ? "1" : "0");

    char quic_fingerprint[65], quic_host[MAX_BUF];
    uint16_t quic_port;
    if (socket_server_quic_info(VS(quic_host), &quic_port, quic_fingerprint)) {
        curl_request_form_add(current_request, "server_id", quic_fingerprint);
        curl_request_form_add(current_request, "quic_cert_sha256", quic_fingerprint);
        if (*quic_host != '\0') {
            curl_request_form_add(current_request, "quic_host", quic_host);
        }
        snprintf(VS(buf), "%" PRIu16, quic_port);
        curl_request_form_add(current_request, "quic_port", buf);
    }

    /* Send off the POST request */
    curl_request_start_post(current_request);

out:
    if (otp != NULL) {
        OPENSSL_cleanse(otp, otp_length);
        free(otp);
    }
    if (body != NULL) {
        OPENSSL_cleanse(body, body_size);
    }
    OPENSSL_cleanse(cotp, sizeof(cotp));
    OPENSSL_cleanse(cotp_digest, sizeof(cotp_digest));
    OPENSSL_cleanse(cotp_hash, sizeof(cotp_hash));
    OPENSSL_cleanse(key, sizeof(key));
    curl_request_free(request);
    pthread_mutex_unlock(&request_lock);
}

/**
 * Updates the metaserver information.
 */
void metaserver_info_update(void) {
    if (!metaserver_enabled()) {
        return;
    }

    pthread_mutex_lock(&request_lock);

    if (current_request != NULL) {
        curl_state_t state = curl_request_get_state(current_request);
        if (state == CURL_STATE_INPROGRESS) {
            pthread_mutex_unlock(&request_lock);
            return;
        }

        curl_request_free(current_request);
    }

    pthread_mutex_unlock(&request_lock);

    request_num_players = 0;
    for (player *pl = first_player; pl != NULL; pl = pl->next) {
        request_num_players++;
    }

    char url[HUGE_BUF];
    snprintf(VS(url), "%s/otp", settings.metaserver_url);
    /* If we're at this point, no other thread is currently working with
     * the current request and thus a lock is not necessary. */
    /* coverity[missing_lock] */
    current_request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_cb(current_request, metaserver_otp_request, NULL);
    curl_request_start_get(current_request);
}

/**
 * Construct metaserver statistics.
 *
 * @param[out] buf
 * Buffer to use for writing. Must end with a NUL.
 * @param size
 * Size of 'buf'.
 */
void metaserver_stats(char *buf, size_t size) {
    pthread_mutex_lock(&stats_lock);
    snprintfcat(buf, size, "\n=== METASERVER ===\n");
    snprintfcat(buf, size, "\nUpdates: %" PRIu64, stats.num);
    snprintfcat(buf, size, "\nFailed: %" PRIu64, stats.num_failed);
    snprintfcat(buf, size, "\nRendezvous reconnects: %" PRIu64, stats.rendezvous_reconnects);

    if (stats.last != 0) {
        snprintfcat(buf, size, "\nLast update: %.19s", ctime(&stats.last));
    }

    if (stats.last_failed != 0) {
        snprintfcat(buf, size, "\nLast failure: %.19s", ctime(&stats.last_failed));
    }

    snprintfcat(buf, size, "\n");
    pthread_mutex_unlock(&stats_lock);
}
