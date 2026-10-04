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
#include <toolkit/metaserver_publisher.h>
#include <toolkit/metaserver_url.h>
#include <toolkit/path.h>
#include <toolkit/rendezvous.h>
#include <player.h>
#include <server.h>
#include <metaserver_internal.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include <ctype.h>

/**
 * Used to hold metaserver statistics.
 */
static struct {
    uint64_t num; ///< Number of successful updates.

    uint64_t num_failed; ///< Number of failed updates.

    uint64_t rendezvous_reconnects; ///< Rendezvous reconnect attempts.

    uint64_t rendezvous_attempts; ///< Server-role rendezvous upgrade attempts.

    uint64_t rendezvous_rejections; ///< Permanent rendezvous upgrade rejections.

    uint64_t publish_attempts; ///< Publication attempts, including local construction failures.

    uint64_t publish_retries; ///< Transient publisher failures scheduled for retry.

    uint64_t publish_rejections; ///< Permanent publisher rejections.

    time_t last; ///< Last successful update.

    time_t last_failed; ///< Last failed update.
} stats;

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
static bool metaserver_initialized;
static bool current_request_handled;
static metaserver_publish_cadence_t publish_cadence;

typedef struct metaserver_public_snapshot {
    char name[MAX_BUF];
    char description[MAX_BUF];
    char hostname[MAX_BUF];
    uint32_t players_count;
    uint16_t port;
    bool is_public;
    bool access_required;
} metaserver_public_snapshot_t;

static metaserver_public_snapshot_t published_snapshot;
static metaserver_public_snapshot_t attempted_snapshot;
static metaserver_public_snapshot_t blocked_snapshot;
static bool published_snapshot_valid;
static bool blocked_snapshot_valid;
static bool metaserver_identity(char *identity, size_t identity_size) {
    HARD_ASSERT(identity_size >= 65);
    return socket_server_quic_identity(identity);
}

bool metaserver_rendezvous_token_parse(const char *body, size_t body_size, char token[65]) {
    HARD_ASSERT(token != NULL);

    OPENSSL_cleanse(token, 65);
    static const char prefix[] = "{\"status\":\"ok\",\"rendezvousToken\":\"";
    static const char suffix[] = "\"}";
    const size_t required = sizeof(prefix) - 1U + 64U + sizeof(suffix) - 1U;
    if (body == NULL || body_size != required || memcmp(body, prefix, sizeof(prefix) - 1U) != 0 ||
        memcmp(body + required - (sizeof(suffix) - 1U), suffix, sizeof(suffix) - 1U) != 0) {
        return false;
    }
    const char *value = body + sizeof(prefix) - 1U;
    memcpy(token, value, 64);
    token[64] = '\0';
    if (!string_is_hex_fixed(token, 64, true)) {
        OPENSSL_cleanse(token, 65);
        return false;
    }
    return true;
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
static metaserver_attempt_budget_t rendezvous_attempt_budget;
static void metaserver_rendezvous_stop(void);

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

static CURLcode metaserver_rendezvous_ping(CURL *curl, uint64_t generation) {
    pthread_mutex_lock(&rendezvous_disclosure_lock);
    pthread_mutex_lock(&rendezvous_lock);
    bool current = metaserver_rendezvous_current_locked(generation);
    pthread_mutex_unlock(&rendezvous_lock);
    if (!current) {
        pthread_mutex_unlock(&rendezvous_disclosure_lock);
        return CURLE_ABORTED_BY_CALLBACK;
    }

    static const char payload[] = "";
    size_t sent = 0;
    CURLcode result = curl_ws_send(curl, payload, 0, &sent, 0, CURLWS_PING);
    pthread_mutex_unlock(&rendezvous_disclosure_lock);
    return result == CURLE_OK && sent == 0 ? CURLE_OK
                                          : result == CURLE_OK ? CURLE_WRITE_ERROR : result;
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
    bool attempted = false;
    while (metaserver_rendezvous_current(args->generation)) {
        pthread_mutex_lock(&rendezvous_lock);
        uint32_t budget_wait_ms;
        bool budget_available = metaserver_attempt_budget_consume(&rendezvous_attempt_budget,
                                                                  server_monotonic_now(),
                                                                  &budget_wait_ms);
        pthread_mutex_unlock(&rendezvous_lock);
        if (!budget_available) {
            LOG(INFO,
                "Rendezvous control attempt budget refills in %" PRIu32 " ms",
                budget_wait_ms);
            if (!metaserver_rendezvous_wait(args->generation, budget_wait_ms)) {
                break;
            }
            continue;
        }
        pthread_mutex_lock(&stats_lock);
        stats.rendezvous_attempts++;
        if (attempted) {
            stats.rendezvous_reconnects++;
        }
        pthread_mutex_unlock(&stats_lock);
        attempted = true;

        uint32_t retry_after_seconds = 0;
        uint64_t connected_ms = 0;
        bool retryable = true;
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
            } else {
                struct curl_slist *protocol_headers =
                    curl_slist_append(headers,
                                      "Sec-WebSocket-Protocol: " RENDEZVOUS_ACCESS_SUBPROTOCOL);
                if (protocol_headers == NULL) {
                    LOG(ERROR, "Cannot allocate rendezvous access request headers");
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
            bool protocol_valid =
                metaserver_rendezvous_protocol_allows(&response_headers, true);
            bool connected = result == CURLE_OK && http_code == 101 && protocol_valid;
            if (!connected) {
                retryable =
                    metaserver_rendezvous_upgrade_retryable(result, http_code, protocol_valid);
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
                    LOG(ERROR, "Rendezvous connection failed: access subprotocol was not selected");
                }
            } else {
                uint64_t connected_at = datetime_monotonic_ms();
                uint64_t next_heartbeat =
                    UINT64_MAX - connected_at < METASERVER_RENDEZVOUS_HEARTBEAT_MS
                        ? UINT64_MAX
                        : connected_at + METASERVER_RENDEZVOUS_HEARTBEAT_MS;
                rendezvous_punch_job_t punch_jobs[RENDEZVOUS_PUNCH_JOBS_MAX] = {0};
                metaserver_rendezvous_auth_job_t auth_jobs[METASERVER_RENDEZVOUS_AUTH_JOBS_MAX] = {
                    0};
                char message[RENDEZVOUS_FRAME_MAX + 1U] = {0};
                size_t used = 0;
                bool stop_control = false;
                while (!stop_control && metaserver_rendezvous_current(args->generation)) {
                    uint64_t now = datetime_monotonic_ms();
                    if (now >= next_heartbeat) {
                        CURLcode heartbeat_result =
                            metaserver_rendezvous_ping(curl, args->generation);
                        if (heartbeat_result == CURLE_OK) {
                            next_heartbeat =
                                UINT64_MAX - now < METASERVER_RENDEZVOUS_HEARTBEAT_MS
                                    ? UINT64_MAX
                                    : now + METASERVER_RENDEZVOUS_HEARTBEAT_MS;
                            LOG(DEBUG, "Sent rendezvous control heartbeat");
                        } else if (heartbeat_result == CURLE_AGAIN) {
                            next_heartbeat =
                                UINT64_MAX - now < METASERVER_RENDEZVOUS_HEARTBEAT_RETRY_MS
                                    ? UINT64_MAX
                                    : now + METASERVER_RENDEZVOUS_HEARTBEAT_RETRY_MS;
                        } else {
                            if (metaserver_rendezvous_current(args->generation)) {
                                LOG(ERROR,
                                    "Rendezvous control heartbeat failed: %s",
                                    curl_easy_strerror(heartbeat_result));
                            }
                            break;
                        }
                    }
                    metaserver_rendezvous_auth_expire(auth_jobs,
                                                      arraysize(auth_jobs),
                                                      datetime_monotonic_ms());
                    metaserver_rendezvous_frame_result_t frame_result =
                        metaserver_rendezvous_punch_update(curl, punch_jobs, args->generation);
                    if (frame_result != METASERVER_RENDEZVOUS_FRAME_HANDLED) {
                        break;
                    }

                    socket_websocket_receive_info_t receive_info;
                    socket_websocket_receive_state_t receive_state =
                        socket_websocket_receive_ex(curl, VS(message), &used, &receive_info);
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
                        const char *receive_reason =
                            receive_state == SOCKET_WEBSOCKET_CLOSED ? "closed" : "protocol error";
                        const char *curl_error = receive_info.curl_result >= 0
                                                     ? curl_easy_strerror(
                                                           (CURLcode)receive_info.curl_result)
                                                     : "not available";
                        if (receive_info.has_close_code) {
                            LOG(ERROR,
                                "Rendezvous control receive %s (libcurl result %d: %s, "
                                "frame flags 0x%x, payload %" PRIu64 ", bytes left %" PRIu64
                                ", close code %u)",
                                receive_reason,
                                receive_info.curl_result,
                                curl_error,
                                receive_info.frame_flags,
                                (uint64_t)receive_info.bytes_received,
                                receive_info.bytes_left,
                                (unsigned int)receive_info.close_code);
                        } else {
                            LOG(ERROR,
                                "Rendezvous control receive %s (libcurl result %d: %s, "
                                "frame flags 0x%x, payload %" PRIu64 ", bytes left %" PRIu64 ")",
                                receive_reason,
                                receive_info.curl_result,
                                curl_error,
                                receive_info.frame_flags,
                                (uint64_t)receive_info.bytes_received,
                                receive_info.bytes_left);
                        }
                        break;
                    }

                    char host[65], ticket[65];
                    uint16_t port;
                    {
                        bool authorization_required = args->authorization_required;
                        metaserver_rendezvous_auth_job_t *authorized = NULL;
                        /* The authenticated control receives candidates only after
                         * the metaserver atomically redeems the bound grant. Keep
                         * a bounded consumed-ticket set on this control generation. */
                        bool candidate_parsed = socket_rendezvous_client_candidate_parse(
                            message, NULL, false, RENDEZVOUS_SERVER_AUTH_NEW,
                            VS(host), &port, ticket);
                        if (candidate_parsed) {
                            candidate_parsed = metaserver_rendezvous_auth_claim(
                                auth_jobs, arraysize(auth_jobs), ticket,
                                datetime_monotonic_ms() + 15000U, &authorized) ==
                                METASERVER_RENDEZVOUS_AUTH_CLAIM_OK;
                            if (candidate_parsed) authorized->state = RENDEZVOUS_SERVER_AUTH_AUTHORIZED;
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
                                /* Retain consumed ticket until its bounded expiry. */
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
        if (!retryable) {
            LOG(ERROR,
                "Rendezvous control was permanently rejected; waiting for a successful "
                "metaserver publish before reconnecting");
            pthread_mutex_lock(&stats_lock);
            stats.rendezvous_rejections++;
            pthread_mutex_unlock(&stats_lock);
            break;
        }
        if (connected_ms >= METASERVER_RENDEZVOUS_STABLE_MS) {
            pthread_mutex_lock(&rendezvous_lock);
            if (metaserver_rendezvous_current_locked(args->generation)) {
                metaserver_attempt_budget_reset(&rendezvous_attempt_budget,
                                                server_monotonic_now());
            }
            pthread_mutex_unlock(&rendezvous_lock);
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
    if ((!settings.server_public && !settings.access_required) || !socket_server_quic_identity(quic_fingerprint)) {
        return false;
    }

    return metaserver_url_rendezvous(settings.metaserver_rendezvous_origin,
                                     quic_fingerprint,
                                     "server",
                                     url,
                                     url_size);
}

static void metaserver_rendezvous_start(const char *token) {
    rendezvous_args_t *args = xcalloc(1, sizeof(*args));
    snprintf(VS(args->token), "%s", token);
    args->authorization_required = settings.access_required;
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

static bool metaserver_rendezvous_response(curl_request_t *request) {
    size_t body_size = 0;
    char *body = curl_request_get_body(request, &body_size);
    char value[65];
    if (!metaserver_rendezvous_token_parse(body, body_size, value)) {
        return false;
    }
    if (settings.server_public || settings.access_required) {
        metaserver_rendezvous_start(value);
    } else {
        metaserver_rendezvous_stop();
    }
    OPENSSL_cleanse(value, sizeof(value));
    return true;
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

static uint32_t metaserver_publish_random(void) {
    uint32_t value;
    if (RAND_bytes((unsigned char *)&value, sizeof(value)) != 1) {
        server_monotonic_t now = server_monotonic_now();
        value = (uint32_t)(now.microseconds ^ (now.microseconds >> 32U));
    }
    return value;
}

static void metaserver_public_snapshot(metaserver_public_snapshot_t *snapshot) {
    HARD_ASSERT(snapshot != NULL);

    memset(snapshot, 0, sizeof(*snapshot));
    snprintf(VS(snapshot->name), "%s", settings.server_name);
    snprintf(VS(snapshot->description), "%s", settings.server_desc);
    if (*settings.metaserver_hostname != '\0') {
        snprintf(VS(snapshot->hostname), "%s", settings.metaserver_hostname);
        snapshot->port = settings.port_quic;
    }
    snapshot->is_public = settings.server_public;
    snapshot->access_required = settings.access_required;
    for (player *pl = first_player; pl != NULL; pl = pl->next) {
        snapshot->players_count++;
    }
}

static bool metaserver_public_snapshot_equal(const metaserver_public_snapshot_t *lhs,
                                             const metaserver_public_snapshot_t *rhs) {
    HARD_ASSERT(lhs != NULL);
    HARD_ASSERT(rhs != NULL);

    return lhs->players_count == rhs->players_count && lhs->port == rhs->port &&
           lhs->is_public == rhs->is_public && lhs->access_required == rhs->access_required &&
           strcmp(lhs->name, rhs->name) == 0 && strcmp(lhs->description, rhs->description) == 0 &&
           strcmp(lhs->hostname, rhs->hostname) == 0;
}

#if LIBCURL_VERSION_NUM >= 0x075600
static void metaserver_rendezvous_stop(void) {
    pthread_mutex_lock(&rendezvous_disclosure_lock);
    pthread_mutex_lock(&rendezvous_lock);
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
}
#endif

/**
 * Initialize the metaserver.
 */
void metaserver_init(void) {
    if (!metaserver_enabled()) {
        return;
    }

    pthread_mutex_init(&stats_lock, NULL);
    pthread_mutex_init(&request_lock, NULL);
    memset(&stats, 0, sizeof(stats));
    memset(&published_snapshot, 0, sizeof(published_snapshot));
    memset(&attempted_snapshot, 0, sizeof(attempted_snapshot));
    memset(&blocked_snapshot, 0, sizeof(blocked_snapshot));
    published_snapshot_valid = false;
    blocked_snapshot_valid = false;
    current_request = NULL;
    current_request_handled = false;
    if (settings.metaserver_heartbeat == 0) {
        settings.metaserver_heartbeat = METASERVER_PUBLISH_HEARTBEAT_DEFAULT_SECONDS;
    }
    metaserver_publish_cadence_init(&publish_cadence, server_monotonic_now());
    metaserver_initialized = true;
#if LIBCURL_VERSION_NUM >= 0x075600
    pthread_mutex_init(&rendezvous_lock, NULL);
    pthread_mutex_init(&rendezvous_disclosure_lock, NULL);
    pthread_cond_init(&rendezvous_condition, NULL);
    rendezvous_thread_state = RENDEZVOUS_THREAD_STOPPED;
    rendezvous_shutdown = false;
    rendezvous_generation = 0;
    metaserver_attempt_budget_init(&rendezvous_attempt_budget, server_monotonic_now());
#endif
    metaserver_service();
}

/**
 * Deinitialize the metaserver.
 */
void metaserver_deinit(void) {
    if (!metaserver_enabled()) {
        return;
    }

    pthread_mutex_lock(&request_lock);
    curl_request_t *request = current_request;
    pthread_mutex_unlock(&request_lock);
    if (request != NULL) {
        curl_request_free(request);
        pthread_mutex_lock(&request_lock);
        if (current_request == request) {
            current_request = NULL;
        }
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


    pthread_mutex_lock(&request_lock);
    metaserver_initialized = false;
    pthread_mutex_unlock(&request_lock);
    pthread_mutex_destroy(&stats_lock);
    pthread_mutex_destroy(&request_lock);
}

static void metaserver_publish_failed_stat(void) {
    pthread_mutex_lock(&stats_lock);
    stats.last_failed = time(NULL);
    stats.num_failed++;
    pthread_mutex_unlock(&stats_lock);
}

static void metaserver_publish_retry_locked(uint32_t retry_after_seconds) {
    metaserver_publish_cadence_failed(&publish_cadence,
                                      server_monotonic_now(),
                                      retry_after_seconds,
                                      metaserver_publish_random());
    pthread_mutex_lock(&stats_lock);
    stats.publish_retries++;
    pthread_mutex_unlock(&stats_lock);
}

static void metaserver_publish_suspend_locked(int http_code) {
    blocked_snapshot = attempted_snapshot;
    blocked_snapshot_valid = true;
    metaserver_publish_cadence_suspend(&publish_cadence);
    pthread_mutex_lock(&stats_lock);
    stats.publish_rejections++;
    pthread_mutex_unlock(&stats_lock);
    LOG(ERROR,
        "Metaserver publication was permanently rejected (HTTP %d); publishing and "
        "rendezvous reconnects are suspended until public state changes or the server restarts",
        http_code);
#if LIBCURL_VERSION_NUM >= 0x075600
    metaserver_rendezvous_stop();
#endif
}

static bool metaserver_publish_replay_recover_locked(curl_request_t *request) {
    size_t body_size = 0;
    char *body = curl_request_get_body(request, &body_size);
    uint64_t minimum;
    if (!metaserver_publish_replay_parse(body, body_size, &minimum) ||
        !metaserver_publish_cadence_recover_replay(&publish_cadence)) {
        return false;
    }

    char server_id[65] = {0};
    metaserver_publish_sequence_result_t recovered =
        metaserver_identity(VS(server_id))
            ? metaserver_publish_sequence_recover(settings.datapath, server_id, minimum)
            : METASERVER_PUBLISH_SEQUENCE_ERROR;
    OPENSSL_cleanse(server_id, sizeof(server_id));
    if (recovered == METASERVER_PUBLISH_SEQUENCE_EXHAUSTED) {
        LOG(ERROR,
            "Metaserver publish sequence space is exhausted; rotate the QUIC identity to "
            "create a new server ID");
    } else if (recovered != METASERVER_PUBLISH_SEQUENCE_OK) {
        LOG(ERROR, "Failed to persist metaserver replay-recovery state");
    }
    return recovered == METASERVER_PUBLISH_SEQUENCE_OK;
}

static void metaserver_update_request_locked(curl_request_t *request) {
    HARD_ASSERT(request != NULL);
    HARD_ASSERT(current_request == request);

    if (current_request_handled) {
        return;
    }
    current_request_handled = true;

    curl_state_t state = curl_request_get_state(request);
    int http_code = curl_request_get_http_code(request);
    size_t headers_size = 0;
    char *headers = curl_request_get_header(request, &headers_size);
    uint32_t retry_after_seconds = 0;
    bool has_retry_after =
        metaserver_publish_retry_after(headers, headers_size, &retry_after_seconds);

    bool success = state == CURL_STATE_OK && http_code == 200;
#if LIBCURL_VERSION_NUM >= 0x075600
    if (success && !metaserver_rendezvous_response(request)) {
        LOG(ERROR, "Metaserver returned a malformed successful publish response");
        success = false;
        http_code = 200;
    }
#endif
    if (success) {
        published_snapshot = attempted_snapshot;
        published_snapshot_valid = true;
        blocked_snapshot_valid = false;
        metaserver_publish_cadence_succeeded(&publish_cadence,
                                             server_monotonic_now(),
                                             true,
                                             settings.metaserver_heartbeat,
                                             metaserver_publish_random());
        pthread_mutex_lock(&stats_lock);
        stats.last = time(NULL);
        stats.num++;
        pthread_mutex_unlock(&stats_lock);
    } else {
        metaserver_publish_failed_stat();
        size_t body_size = 0;
        char *body = curl_request_get_body(request, &body_size);
        char error_code[METASERVER_PUBLISH_ERROR_CODE_MAX + 1U] = {0};
        metaserver_publish_error_code(body, body_size, VS(error_code));
        if (has_retry_after) {
            LOG(SYSTEM,
                "Failed to update metaserver information (HTTP code: %d, error: %s, "
                "retry-after: %u)",
                http_code,
                error_code,
                retry_after_seconds);
        } else {
            LOG(SYSTEM,
                "Failed to update metaserver information (HTTP code: %d, error: %s)",
                http_code,
                error_code);
        }
        metaserver_publish_failure_action_t action =
            metaserver_publish_failure_action(state, http_code);
        bool replay_recovered = action == METASERVER_PUBLISH_FAILURE_REPLAY &&
                                metaserver_publish_replay_recover_locked(request);
        if (replay_recovered || action == METASERVER_PUBLISH_FAILURE_RETRY) {
            metaserver_publish_retry_locked(retry_after_seconds);
        } else {
            metaserver_publish_suspend_locked(http_code);
        }
    }
    curl_request_clear_response(request);
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
    (void)user_data;
    pthread_mutex_lock(&request_lock);
    HARD_ASSERT(current_request == request);
    metaserver_update_request_locked(request);
    pthread_mutex_unlock(&request_lock);
}

static curl_request_t *metaserver_publish_request_create(uint32_t players_count) {
    char server_id[65] = {0};
    char body[METASERVER_PUBLISH_BODY_MAX + 1U] = {0};
    char sequence_header[21] = {0};
    char signature_header[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX] = {0};
    char url[HUGE_BUF] = {0};
    char authority[MAX_BUF] = {0};
    unsigned char nonce[METASERVER_PUBLISH_NONCE_SIZE] = {0};
    metaserver_publisher_components_t components = {0};
    metaserver_publisher_identity_t *identity = NULL;
    curl_request_t *request = NULL;
    uint64_t sequence = 0;
    size_t body_size = 0;

    if (!metaserver_identity(VS(server_id))) {
        LOG(ERROR, "Cannot access the active QUIC identity for metaserver publication");
        goto out;
    }
    identity = socket_server_quic_publisher_identity();
    if (identity == NULL) {
        LOG(ERROR, "The active QUIC listener is not a valid P-256 publisher identity");
        goto out;
    }
    metaserver_publisher_classic_payload_t payload = {
        .server_id = server_id,
        .certificate = metaserver_publisher_identity_certificate(identity),
        .name = settings.server_name,
        .players_count = players_count,
        .version = PACKAGE_VERSION,
        .text_comment = settings.server_desc,
        .is_public = settings.server_public,
        .access_required = settings.access_required,
        .hostname = *settings.metaserver_hostname != '\0' ? settings.metaserver_hostname : NULL,
        .port = *settings.metaserver_hostname != '\0' ? settings.port_quic : 0,
    };
    if (!metaserver_publisher_classic_body(&payload, body, &body_size)) {
        LOG(ERROR, "Server metadata cannot be represented by the signed publisher contract");
        goto out;
    }

    metaserver_publish_sequence_result_t reserved =
        metaserver_publish_sequence_reserve(settings.datapath, server_id, 1, &sequence);
    if (reserved == METASERVER_PUBLISH_SEQUENCE_EXHAUSTED) {
        LOG(ERROR,
            "Metaserver publish sequence space is exhausted; rotate the QUIC identity to "
            "create a new server ID");
        goto out;
    }
    if (reserved != METASERVER_PUBLISH_SEQUENCE_OK) {
        LOG(ERROR, "Cannot securely persist the next metaserver publish sequence");
        goto out;
    }
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        LOG(ERROR, "Cannot generate a metaserver publish nonce");
        goto out;
    }
    time_t now = time(NULL);
    if (now < 0 ||
        !metaserver_url_publish(settings.metaserver_publish_origin, "/", VS(url), VS(authority)) ||
        !metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V3,
                                    authority,
                                    server_id,
                                    sequence,
                                    nonce,
                                    (uint64_t)now,
                                    body,
                                    body_size,
                                    &components) ||
        !metaserver_publisher_identity_sign(identity,
                                            components.signature_base,
                                            signature_header) ||
        snprintf(VS(sequence_header), "%" PRIu64, sequence) >= (int)sizeof(sequence_header) ||
        !metaserver_url_publish(settings.metaserver_publish_origin,
                                components.path,
                                VS(url),
                                VS(authority))) {
        LOG(ERROR, "Cannot construct a signed metaserver publication");
        goto out;
    }

    request = curl_request_create_with_origin(url,
                                                 CURL_PKEY_TRUST_SYSTEM,
                                                 "server.metaserver");
    if (!curl_request_set_post_body(request, body, body_size) ||
        !curl_request_header_add(request, "Content-Type", METASERVER_PUBLISH_CONTENT_TYPE) ||
        !curl_request_header_add(request, "Content-Digest", components.content_digest) ||
        !curl_request_header_add(request, "Atrinik-Server-ID", server_id) ||
        !curl_request_header_add(request, "Atrinik-Publish-Sequence", sequence_header) ||
        !curl_request_header_add(request, "Signature-Input", components.signature_input) ||
        !curl_request_header_add(request, "Signature", signature_header)) {
        LOG(ERROR, "Cannot configure the signed metaserver publication request");
        curl_request_free(request);
        request = NULL;
        goto out;
    }
    curl_request_set_follow_redirects(request, false);
    curl_request_set_max_body(request, METASERVER_PUBLISH_RESPONSE_BODY_MAX);
    curl_request_set_max_header(request, 16384);
    curl_request_set_timeout(request, METASERVER_PUBLISH_TIMEOUT_MS);
    curl_request_set_cb(request, metaserver_update_request, NULL);

out:
    metaserver_publisher_identity_free(identity);
    OPENSSL_cleanse(server_id, sizeof(server_id));
    OPENSSL_cleanse(body, sizeof(body));
    OPENSSL_cleanse(sequence_header, sizeof(sequence_header));
    OPENSSL_cleanse(signature_header, sizeof(signature_header));
    OPENSSL_cleanse(url, sizeof(url));
    OPENSSL_cleanse(authority, sizeof(authority));
    OPENSSL_cleanse(nonce, sizeof(nonce));
    OPENSSL_cleanse(&components, sizeof(components));
    return request;
}
/** Mark public metaserver state dirty and debounce a new observation. */
void metaserver_info_update(void) {
    if (!metaserver_initialized) {
        return;
    }

    pthread_mutex_lock(&request_lock);
    metaserver_publish_cadence_changed(&publish_cadence, server_monotonic_now(), false);
    pthread_mutex_unlock(&request_lock);
}

/** Service completed requests and start a due, rate-bounded publication. */
void metaserver_service(void) {
    if (!metaserver_initialized) {
        return;
    }

    server_monotonic_t now = server_monotonic_now();

    pthread_mutex_lock(&request_lock);

    if (current_request != NULL) {
        curl_state_t state = curl_request_get_state(current_request);
        if (state == CURL_STATE_INPROGRESS) {
            pthread_mutex_unlock(&request_lock);
            return;
        }

        metaserver_update_request_locked(current_request);

        curl_request_t *completed = current_request;
        pthread_mutex_unlock(&request_lock);
        curl_request_free(completed);
        pthread_mutex_lock(&request_lock);
        if (current_request == completed) {
            current_request = NULL;
        }
    }

    if (!metaserver_publish_cadence_needs_snapshot(&publish_cadence, now)) {
        pthread_mutex_unlock(&request_lock);
        return;
    }

    metaserver_public_snapshot_t snapshot;
    metaserver_public_snapshot(&snapshot);

    bool snapshot_changed = !published_snapshot_valid ||
                            !metaserver_public_snapshot_equal(&snapshot, &published_snapshot);
    if (publish_cadence.suspended && blocked_snapshot_valid) {
        bool changed_from_rejection =
            !metaserver_public_snapshot_equal(&snapshot, &blocked_snapshot);
        if (changed_from_rejection) {
            metaserver_publish_cadence_changed(&publish_cadence, now, true);
        } else {
            publish_cadence.dirty = false;
        }
    }
    if (!metaserver_publish_cadence_due(&publish_cadence, now, snapshot_changed)) {
        pthread_mutex_unlock(&request_lock);
        return;
    }

    if (!metaserver_publish_cadence_attempted(&publish_cadence, now)) {
        pthread_mutex_unlock(&request_lock);
        return;
    }
    attempted_snapshot = snapshot;
    pthread_mutex_lock(&stats_lock);
    stats.publish_attempts++;
    pthread_mutex_unlock(&stats_lock);
    current_request = metaserver_publish_request_create(snapshot.players_count);
    if (current_request == NULL) {
        metaserver_publish_failed_stat();
        metaserver_publish_retry_locked(0);
        pthread_mutex_unlock(&request_lock);
        return;
    }
    current_request_handled = false;
    curl_request_start_post(current_request);
    pthread_mutex_unlock(&request_lock);
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
    snprintfcat(buf, size, "\nPublish attempts: %" PRIu64, stats.publish_attempts);
    snprintfcat(buf, size, "\nPublish retries: %" PRIu64, stats.publish_retries);
    snprintfcat(buf, size, "\nPublish rejections: %" PRIu64, stats.publish_rejections);
    snprintfcat(buf, size, "\nRendezvous attempts: %" PRIu64, stats.rendezvous_attempts);
    snprintfcat(buf, size, "\nRendezvous reconnects: %" PRIu64, stats.rendezvous_reconnects);
    snprintfcat(buf, size, "\nRendezvous rejections: %" PRIu64, stats.rendezvous_rejections);

    if (stats.last != 0) {
        snprintfcat(buf, size, "\nLast update: %.19s", ctime(&stats.last));
    }

    if (stats.last_failed != 0) {
        snprintfcat(buf, size, "\nLast failure: %.19s", ctime(&stats.last_failed));
    }

    snprintfcat(buf, size, "\n");
    pthread_mutex_unlock(&stats_lock);
}

/* Private route bodies never enter the public publication or its diagnostics. */
typedef struct access_route_response {
    char body[1025];
    size_t size;
} access_route_response_t;

static size_t metaserver_access_route_body(char *data, size_t size, size_t count, void *context) {
    access_route_response_t *response = context;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > sizeof(response->body) - 1 - response->size) return 0;
    memcpy(response->body + response->size, data, n);
    response->size += n;
    response->body[response->size] = 0;
    return n;
}

static bool metaserver_access_request_id(const char *request, const char *operation, char out[33]) {
    /* Independent business IDs per phase, stable across network retries. */
    static const char domain[] = "atrinik-access-native-route-request-v1";
    unsigned char digest[32];
    unsigned int n = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx != NULL && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(ctx, domain, sizeof(domain)) == 1 &&
              EVP_DigestUpdate(ctx, request, 32) == 1 &&
              EVP_DigestUpdate(ctx, operation, strlen(operation)) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &n) == 1 && n == 32 &&
              string_tohex(digest, 16, out, 33, false) == 32;
    EVP_MD_CTX_free(ctx);
    OPENSSL_cleanse(digest, sizeof(digest));
    if (ok) string_tolower(out);
    return ok;
}

static access_outcome_t metaserver_access_operation(const access_route_t *route,
                                                     const char *operation,
                                                     char reservation[33],
                                                     uint64_t deadline_ms) {
    access_outcome_t outcome = ACCESS_PENDING;
    char server_id[65], request_id[33], index[65], expiry[24] = "null", handle[36] = "null";
    char body[4097], signature[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX], url[MAX_BUF], authority[MAX_BUF];
    unsigned char nonce[16];
    metaserver_publisher_components_t components;
    metaserver_publisher_identity_t *identity = NULL;
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    access_route_response_t response = {0};
    uint64_t sequence = 0;
    uint64_t now_ms = datetime_monotonic_ms();
    time_t now = time(NULL);
    if (now < 0 || now_ms >= deadline_ms || !metaserver_identity(VS(server_id)) ||
        !string_is_hex_fixed(route->request_id, 32, true) ||
        !string_is_hex_fixed(route->token.token_id, 32, true) || route->token.revision == 0 ||
        !metaserver_access_request_id(route->request_id, operation, request_id) ||
        string_tohex(route->index, 32, VS(index), false) != 64 || RAND_bytes(nonce, 16) != 1 ||
        !metaserver_url_access(settings.metaserver_publish_origin, NULL, false, VS(url))) goto out;
    string_tolower(index);
    if (route->has_expiry) {
        if (route->expires_at <= 0) goto out;
        snprintf(VS(expiry), "\"%" PRId64 "\"", route->expires_at);
    }
    if (reservation[0] != 0) {
        if (!string_is_hex_fixed(reservation, 32, true)) goto out;
        snprintf(VS(handle), "\"%s\"", reservation);
    }
    identity = socket_server_quic_publisher_identity();
    if (identity == NULL) goto out;
    int n = snprintf(VS(body),
        "{\"schema\":\"atrinik-access-route-v1\",\"profile\":\"classic\",\"serverId\":\"%s\","
        "\"certificate\":\"%s\",\"operation\":\"%s\",\"requestId\":\"%s\",\"tokenId\":\"%s\","
        "\"tokenRevision\":\"%" PRIu64 "\",\"index\":\"%s\",\"reservationId\":%s,\"expiresAt\":%s}",
        server_id, metaserver_publisher_identity_certificate(identity), operation, request_id,
        route->token.token_id, route->token.revision, index, handle, expiry);
    if (n <= 0 || (size_t)n >= sizeof(body)) goto out;
    /* Publication/recovery use request_lock around the same durable lineage. */
    pthread_mutex_lock(&request_lock);
    metaserver_publish_sequence_result_t seq = metaserver_publish_sequence_reserve(
        settings.datapath, server_id, 1, &sequence);
    pthread_mutex_unlock(&request_lock);
    if (seq != METASERVER_PUBLISH_SEQUENCE_OK ||
        !metaserver_url_publish(settings.metaserver_publish_origin, "/", VS(url), VS(authority)) ||
        !metaserver_publisher_build(METASERVER_PUBLISHER_ACCESS_CLASSIC_V1, authority,
            server_id, sequence, nonce, (uint64_t)now, body, (size_t)n, &components) ||
        !metaserver_publisher_identity_sign(identity, components.signature_base, signature) ||
        !metaserver_url_publish(settings.metaserver_publish_origin, components.path, VS(url), VS(authority))) goto out;
    const char *names[] = {"Content-Type", "Content-Digest", "Signature-Input", "Signature", "Atrinik-Server-ID", "Atrinik-Publish-Sequence"};
    char sequence_text[21];
    snprintf(VS(sequence_text), "%" PRIu64, sequence);
    const char *values[] = {"application/json", components.content_digest, components.signature_input, signature, server_id, sequence_text};
    for (size_t i = 0; i < arraysize(names); i++) {
        char line[1200];
        int length = snprintf(VS(line), "%s: %s", names[i], values[i]);
        if (length <= 0 || (size_t)length >= sizeof(line)) goto out;
        struct curl_slist *next = curl_slist_append(headers, line);
        if (next == NULL) goto out;
        headers = next;
    }
    now_ms = datetime_monotonic_ms();
    if (now_ms >= deadline_ms) goto out;
    curl = curl_easy_init();
    if (curl == NULL) goto out;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)n);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)MIN(deadline_ms - now_ms, 14000U));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, metaserver_access_route_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
#ifdef WIN32
    curl_easy_setopt(curl, CURLOPT_CAINFO, "ca-bundle.crt");
#endif
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK || status != 200 || memchr(response.body, 0, response.size) != NULL) goto out;
    char reply_id[33], reply_outcome[16], reply_handle[36], reply_expiry[24], revision[21];
    int consumed = 0;
    if (sscanf(response.body,
        "{\"schema\":\"atrinik-access-route-result-v1\",\"requestId\":\"%32[0-9a-f]\","
        "\"outcome\":\"%15[a-z_]\",\"reservationId\":%35[^,],\"reservationExpiresAt\":%23[^,],"
        "\"tokenRevision\":\"%20[0-9]\"}%n",
        reply_id, reply_outcome, reply_handle, reply_expiry, revision, &consumed) != 5 ||
        consumed < 0 || (size_t)consumed != response.size || strcmp(reply_id, request_id) != 0) goto out;
    char expected_revision[21];
    snprintf(VS(expected_revision), "%" PRIu64, route->token.revision);
    if (strcmp(revision, expected_revision) != 0) goto out;
    bool null_handle = strcmp(reply_handle, "null") == 0;
    if (!null_handle && (strlen(reply_handle) != 34 || reply_handle[0] != '"' || reply_handle[33] != '"')) goto out;
    if (!null_handle) { reply_handle[33] = 0; if (!string_is_hex_fixed(reply_handle + 1, 32, true)) goto out; }
    if (strcmp(reply_expiry, "null") != 0) {
        size_t length = strlen(reply_expiry);
        uint64_t expires;
        if (length < 3 || reply_expiry[0] != '"' || reply_expiry[length - 1] != '"') goto out;
        reply_expiry[length - 1] = 0;
        if (reply_expiry[1] < '1' || reply_expiry[1] > '9' ||
            !string_parse_uint64(reply_expiry + 1, 10, 1, INT64_MAX, &expires)) goto out;
        if (strcmp(operation, "reserve") == 0 && (expires <= (uint64_t)now || expires - (uint64_t)now > 60)) goto out;
    } else if (strcmp(operation, "reserve") == 0) goto out;
    if (strcmp(reply_outcome, "conflict") == 0) { outcome = ACCESS_CONFLICT; goto out; }
    const char *wanted = strcmp(operation, "reserve") == 0 ? "reserved" : strcmp(operation, "activate") == 0 ? "active" : "revoked";
    if (strcmp(reply_outcome, wanted) != 0) goto out;
    if (strcmp(operation, "reserve") == 0) {
        if (null_handle) goto out;
        memcpy(reservation, reply_handle + 1, 33);
    } else if (strcmp(operation, "activate") == 0 && (null_handle || strcmp(reservation, reply_handle + 1) != 0)) goto out;
    outcome = ACCESS_COMMITTED;
out:
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    metaserver_publisher_identity_free(identity);
    OPENSSL_cleanse(body, sizeof(body));
    OPENSSL_cleanse(index, sizeof(index));
    OPENSSL_cleanse(&response, sizeof(response));
    return outcome;
}

access_outcome_t metaserver_access_route(void *context, const access_route_t *route) {
    (void)context;
    if (route == NULL || !metaserver_initialized || !metaserver_enabled()) return ACCESS_UNAVAILABLE;
    char reservation[33] = {0};
    uint64_t now = datetime_monotonic_ms();
    if (now > UINT64_MAX - 29000U) return ACCESS_UNAVAILABLE;
    uint64_t deadline = now + 29000U;
    access_outcome_t outcome = metaserver_access_operation(route, route->revoke ? "revoke" : "reserve", reservation, deadline);
    if (outcome == ACCESS_COMMITTED && !route->revoke) outcome = metaserver_access_operation(route, "activate", reservation, deadline);
    OPENSSL_cleanse(reservation, sizeof(reservation));
    return outcome;
}
