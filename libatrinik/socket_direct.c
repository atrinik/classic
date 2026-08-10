/**
 * @file
 *
 * STUN candidate discovery, UDP hole punching, and rendezvous signaling.
 */

#include "socket_private.h"
#include "string.h"
#include "datetime.h"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#ifdef WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif
#define SOCKET_STUN_MAGIC 0x2112a442U
#define SOCKET_PUNCH_PROBE "ATRINIK-PUNCH-1"
#define SOCKET_RENDEZVOUS_CLIENT_FRAMES_MAX 3U
#define SOCKET_RENDEZVOUS_SERVER_FRAMES_MAX 15U
#define SOCKET_RENDEZVOUS_SERVER_CANDIDATES_MAX 12U
#define SOCKET_RENDEZVOUS_AUTH_FRAMES_MAX 4U
#define SOCKET_RENDEZVOUS_AUTH_BYTES_MAX 2048U
#define SOCKET_RENDEZVOUS_SIGNAL_BYTES_MAX 9216U
#define SOCKET_RENDEZVOUS_RETRY_AFTER_MAX (24U * 60U * 60U)
#define SOCKET_STUN_ATTEMPT_BUDGET_MS 3000U
#define SOCKET_RENDEZVOUS_RESERVED_BUDGET_MS 5000U
#define SOCKET_STUN_RESOLVER_WORKERS_MAX 4U
#define SOCKET_STUN_LATE_DRAIN_MAX 4U

typedef enum socket_rendezvous_attempt_state {
    SOCKET_RENDEZVOUS_ATTEMPT_READY,
    SOCKET_RENDEZVOUS_ATTEMPT_NEW,
    SOCKET_RENDEZVOUS_ATTEMPT_WAIT_CHALLENGE,
    SOCKET_RENDEZVOUS_ATTEMPT_WAIT_RESULT,
    SOCKET_RENDEZVOUS_ATTEMPT_AUTHORIZED,
    SOCKET_RENDEZVOUS_ATTEMPT_WAIT_SERVER,
    SOCKET_RENDEZVOUS_ATTEMPT_COMPLETE,
    SOCKET_RENDEZVOUS_ATTEMPT_TERMINAL
} socket_rendezvous_attempt_state_t;

typedef struct socket_stun_resolver_context {
    pthread_mutex_t mutex;
    socket_stun_resolver_t resolver;
    struct addrinfo hints;
    struct addrinfo *addresses;
    char host[MAX_BUF];
    char service[6];
    int result;
    bool complete;
    bool abandoned;
} socket_stun_resolver_context_t;

static socket_stun_resolver_t socket_stun_resolver = getaddrinfo;
static socket_stun_clock_t socket_stun_clock = datetime_monotonic_ms;
static socket_stun_after_send_t socket_stun_after_send;
static socket_rendezvous_fallback_t socket_rendezvous_fallback;
static pthread_mutex_t socket_stun_resolver_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t socket_stun_resolver_condition = PTHREAD_COND_INITIALIZER;
static unsigned int socket_stun_resolver_workers;

void socket_stun_resolver_set_for_test(socket_stun_resolver_t resolver) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_stun_resolver = resolver != NULL ? resolver : getaddrinfo;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

void socket_stun_clock_set_for_test(socket_stun_clock_t clock) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_stun_clock = clock != NULL ? clock : datetime_monotonic_ms;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

void socket_stun_after_send_set_for_test(socket_stun_after_send_t after_send) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_stun_after_send = after_send;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

void socket_rendezvous_fallback_set_for_test(socket_rendezvous_fallback_t fallback) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_rendezvous_fallback = fallback;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

void socket_stun_resolver_wait_for_test(void) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    while (socket_stun_resolver_workers != 0) {
        pthread_cond_wait(&socket_stun_resolver_condition, &socket_stun_resolver_lock);
    }
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

static bool socket_stun_resolver_claim(socket_stun_resolver_t *resolver) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    bool claimed = socket_stun_resolver_workers < SOCKET_STUN_RESOLVER_WORKERS_MAX;
    if (claimed) {
        socket_stun_resolver_workers++;
        *resolver = socket_stun_resolver;
    }
    pthread_mutex_unlock(&socket_stun_resolver_lock);
    return claimed;
}

static void socket_stun_resolver_release(void) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    HARD_ASSERT(socket_stun_resolver_workers > 0);
    socket_stun_resolver_workers--;
    pthread_cond_broadcast(&socket_stun_resolver_condition);
    pthread_mutex_unlock(&socket_stun_resolver_lock);
}

static uint64_t socket_stun_clock_get(void) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_stun_clock_t clock = socket_stun_clock;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
    return clock();
}

static void socket_stun_resolver_context_destroy(socket_stun_resolver_context_t *context) {
    pthread_mutex_destroy(&context->mutex);
    free(context);
}

static void *socket_stun_resolver_run(void *data) {
    socket_stun_resolver_context_t *context = data;
    struct addrinfo *addresses = NULL;
    int result = context->resolver(context->host, context->service, &context->hints, &addresses);
    socket_stun_resolver_release();

    pthread_mutex_lock(&context->mutex);
    if (context->abandoned) {
        pthread_mutex_unlock(&context->mutex);
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        socket_stun_resolver_context_destroy(context);
        return NULL;
    }
    context->addresses = addresses;
    context->result = result;
    context->complete = true;
    pthread_mutex_unlock(&context->mutex);
    return NULL;
}

static struct addrinfo *socket_stun_resolve_until(const char *host,
                                                  const char *service,
                                                  const struct addrinfo *hints,
                                                  uint64_t deadline_ms,
                                                  int *result,
                                                  bool *timed_out) {
    *result = EAI_AGAIN;
    *timed_out = false;
    uint64_t now_ms = socket_stun_clock_get();
    if (now_ms >= deadline_ms) {
        *timed_out = true;
        return NULL;
    }

    socket_stun_resolver_context_t *context = xcalloc(1, sizeof(*context));
    if (!socket_stun_resolver_claim(&context->resolver)) {
        free(context);
        return NULL;
    }
    context->hints = *hints;
    snprintf(VS(context->host), "%s", host);
    snprintf(VS(context->service), "%s", service);
    if (pthread_mutex_init(&context->mutex, NULL) != 0) {
        socket_stun_resolver_release();
        free(context);
        return NULL;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, socket_stun_resolver_run, context) != 0) {
        socket_stun_resolver_release();
        socket_stun_resolver_context_destroy(context);
        return NULL;
    }

    pthread_mutex_lock(&context->mutex);
    while (!context->complete) {
        now_ms = socket_stun_clock_get();
        if (now_ms >= deadline_ms) {
            break;
        }
        unsigned int wait_us = (unsigned int)(MIN(deadline_ms - now_ms, 10U) * 1000U);
        pthread_mutex_unlock(&context->mutex);
        usleep(wait_us);
        pthread_mutex_lock(&context->mutex);
    }
    if (!context->complete) {
        context->abandoned = true;
        pthread_mutex_unlock(&context->mutex);
        (void)pthread_detach(thread);
        *timed_out = true;
        return NULL;
    }

    struct addrinfo *addresses = context->addresses;
    *result = context->result;
    bool complete_in_time = socket_stun_clock_get() < deadline_ms;
    pthread_mutex_unlock(&context->mutex);
    (void)pthread_join(thread, NULL);
    socket_stun_resolver_context_destroy(context);
    if (!complete_in_time) {
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        *timed_out = true;
        return NULL;
    }
    return addresses;
}

uint64_t socket_rendezvous_stun_deadline(uint64_t now_ms, uint64_t attempt_deadline_ms) {
    if (attempt_deadline_ms <= now_ms ||
        attempt_deadline_ms - now_ms <= SOCKET_RENDEZVOUS_RESERVED_BUDGET_MS) {
        return now_ms;
    }
    uint64_t available_deadline = attempt_deadline_ms - SOCKET_RENDEZVOUS_RESERVED_BUDGET_MS;
    uint64_t budget_deadline = now_ms > UINT64_MAX - SOCKET_STUN_ATTEMPT_BUDGET_MS
                                   ? UINT64_MAX
                                   : now_ms + SOCKET_STUN_ATTEMPT_BUDGET_MS;
    return MIN(available_deadline, budget_deadline);
}

struct socket_rendezvous_attempt {
    char server_id[RENDEZVOUS_SERVER_ID_HEX_SIZE + 1U];
    char ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    rendezvous_invite_t invite;
    rendezvous_websocket_protocol_t protocol;
    uint64_t deadline_ms;
    socket_rendezvous_attempt_state_t state;
    socket_rendezvous_stats_t stats;
    size_t authorization_frames;
    size_t authorization_bytes;
    uint32_t retry_after_seconds;
    bool authorization_required;
    bool has_invite;
};

typedef struct socket_punch_job {
    socket_direct_candidate_t candidate;
    socket_punch_pacer_t pacer;
} socket_punch_job_t;

typedef struct socket_candidate_kind_info {
    const char *name;
    socket_connection_mode_t mode;
    double timeout;
} socket_candidate_kind_info_t;

static const socket_candidate_kind_info_t socket_candidate_kinds[SOCKET_CANDIDATE_NUM] = {
    [SOCKET_CANDIDATE_LAN] = {"lan", SOCKET_CONNECTION_MODE_QUIC_LAN, 1.0},
    [SOCKET_CANDIDATE_IPV6] = {"ipv6", SOCKET_CONNECTION_MODE_QUIC_IPV6, 2.0},
    [SOCKET_CANDIDATE_PRFLX] = {"prflx", SOCKET_CONNECTION_MODE_QUIC_SRFLX, 3.0},
    [SOCKET_CANDIDATE_MAPPED] = {"mapped", SOCKET_CONNECTION_MODE_QUIC_MAPPED, 5.0},
    [SOCKET_CANDIDATE_SRFLX] = {"srflx", SOCKET_CONNECTION_MODE_QUIC_SRFLX, 5.0},
    [SOCKET_CANDIDATE_DIRECTORY] = {"directory", SOCKET_CONNECTION_MODE_QUIC_DIRECTORY, 5.0},
};

const char *socket_candidate_kind_name(socket_candidate_kind_t kind) {
    if ((unsigned int)kind >= SOCKET_CANDIDATE_NUM) {
        return "unknown";
    }
    return socket_candidate_kinds[kind].name;
}

bool socket_candidate_kind_parse(const char *name, socket_candidate_kind_t *kind) {
    HARD_ASSERT(name != NULL);
    HARD_ASSERT(kind != NULL);

    for (socket_candidate_kind_t i = 0; i < SOCKET_CANDIDATE_NUM; i++) {
        if (strcmp(name, socket_candidate_kinds[i].name) == 0) {
            *kind = i;
            return true;
        }
    }
    return false;
}

socket_connection_mode_t socket_candidate_kind_mode(socket_candidate_kind_t kind) {
    return (unsigned int)kind < SOCKET_CANDIDATE_NUM ? socket_candidate_kinds[kind].mode
                                                     : SOCKET_CONNECTION_MODE_QUIC;
}

double socket_candidate_kind_timeout(socket_candidate_kind_t kind) {
    return (unsigned int)kind < SOCKET_CANDIDATE_NUM ? socket_candidate_kinds[kind].timeout : 5.0;
}

static bool socket_rendezvous_ticket_valid(const char *ticket) {
    return string_is_hex_fixed(ticket, 64, true);
}

static bool socket_rendezvous_host_valid(const char *host) {
    struct in_addr address4;
    if (host != NULL && inet_pton(AF_INET, host, &address4) == 1) {
        return address4.s_addr != htonl(INADDR_ANY);
    }
#ifdef HAVE_IPV6
    struct in6_addr address6;
    return host != NULL && inet_pton(AF_INET6, host, &address6) == 1 &&
           !IN6_IS_ADDR_UNSPECIFIED(&address6);
#else
    return false;
#endif
}

static bool socket_rendezvous_client_candidate_parse_raw(const char *message,
                                                         char *host,
                                                         size_t host_size,
                                                         uint16_t *port,
                                                         char ticket[65]) {
    char parsed_host[65], parsed_ticket[65], canonical[256];
    char port_text[6];
    uint64_t parsed_port;
    int consumed = 0;
    if (message == NULL || host == NULL || host_size == 0 || port == NULL || ticket == NULL ||
        strlen(message) > RENDEZVOUS_FRAME_MAX ||
        sscanf(message,
               "{\"type\":\"client_candidate\",\"host\":\"%64[0-9a-fA-F:.]\","
               "\"port\":%5[0-9],\"ticket\":\"%64[0-9a-f]\"}%n",
               parsed_host,
               port_text,
               parsed_ticket,
               &consumed) != 3 ||
        message[consumed] != '\0' ||
        !string_parse_uint64(port_text, 10, 1, UINT16_MAX, &parsed_port) ||
        !socket_rendezvous_host_valid(parsed_host) ||
        !socket_rendezvous_ticket_valid(parsed_ticket) || strlen(parsed_host) >= host_size ||
        snprintf(VS(canonical),
                 "{\"type\":\"client_candidate\",\"host\":\"%s\","
                 "\"port\":%u,\"ticket\":\"%s\"}",
                 parsed_host,
                 (unsigned int)parsed_port,
                 parsed_ticket) >= (int)sizeof(canonical) ||
        strcmp(message, canonical) != 0) {
        return false;
    }
    snprintf(host, host_size, "%s", parsed_host);
    snprintf(ticket, 65, "%s", parsed_ticket);
    *port = (uint16_t)parsed_port;
    return true;
}

static bool socket_rendezvous_server_candidate_parse_raw(const char *message,
                                                         const char *expected_ticket,
                                                         socket_direct_candidate_t *candidate) {
    char host[65], kind[16], ticket[65], canonical[256];
    char port_text[6];
    uint64_t port;
    int consumed = 0;
    socket_candidate_kind_t parsed_kind;
    if (message == NULL || expected_ticket == NULL || candidate == NULL ||
        !socket_rendezvous_ticket_valid(expected_ticket) ||
        strlen(message) > RENDEZVOUS_FRAME_MAX ||
        sscanf(message,
               "{\"type\":\"server_candidate\",\"host\":\"%64[0-9a-fA-F:.]\","
               "\"port\":%5[0-9],\"kind\":\"%15[a-z0-9]\","
               "\"ticket\":\"%64[0-9a-f]\"}%n",
               host,
               port_text,
               kind,
               ticket,
               &consumed) != 4 ||
        message[consumed] != '\0' || !string_parse_uint64(port_text, 10, 1, UINT16_MAX, &port) ||
        strcmp(ticket, expected_ticket) != 0 || !socket_rendezvous_host_valid(host) ||
        !socket_candidate_kind_parse(kind, &parsed_kind) ||
        snprintf(VS(canonical),
                 "{\"type\":\"server_candidate\",\"host\":\"%s\","
                 "\"port\":%u,\"kind\":\"%s\",\"ticket\":\"%s\"}",
                 host,
                 (unsigned int)port,
                 socket_candidate_kind_name(parsed_kind),
                 ticket) >= (int)sizeof(canonical) ||
        strcmp(message, canonical) != 0) {
        return false;
    }
    snprintf(VS(candidate->host), "%s", host);
    candidate->port = (uint16_t)port;
    candidate->kind = parsed_kind;
    return true;
}

static bool socket_rendezvous_message_render(char *buffer,
                                             size_t size,
                                             const char *type,
                                             const char *host,
                                             uint16_t port,
                                             socket_candidate_kind_t kind,
                                             const char *ticket) {
    if (buffer != NULL && size != 0) {
        buffer[0] = '\0';
    }
    if (buffer == NULL || size == 0 || type == NULL || !socket_rendezvous_ticket_valid(ticket)) {
        return false;
    }
    int length;
    if (strcmp(type, "complete") == 0) {
        length = snprintf(buffer, size, "{\"type\":\"complete\",\"ticket\":\"%s\"}", ticket);
    } else if (strcmp(type, "client_candidate") == 0 && port != 0 &&
               socket_rendezvous_host_valid(host)) {
        length = snprintf(buffer,
                          size,
                          "{\"type\":\"client_candidate\",\"host\":\"%s\","
                          "\"port\":%" PRIu16 ",\"ticket\":\"%s\"}",
                          host,
                          port,
                          ticket);
    } else if (strcmp(type, "server_candidate") == 0 && port != 0 &&
               socket_rendezvous_host_valid(host) && (unsigned int)kind < SOCKET_CANDIDATE_NUM) {
        length = snprintf(buffer,
                          size,
                          "{\"type\":\"server_candidate\",\"host\":\"%s\","
                          "\"port\":%" PRIu16 ",\"kind\":\"%s\","
                          "\"ticket\":\"%s\"}",
                          host,
                          port,
                          socket_candidate_kind_name(kind),
                          ticket);
    } else {
        return false;
    }
    return length >= 0 && (size_t)length < size;
}

bool socket_rendezvous_client_candidate_parse(const char *message,
                                              const char *expected_ticket,
                                              bool authorization_required,
                                              rendezvous_server_auth_state_t authorization,
                                              char *host,
                                              size_t host_size,
                                              uint16_t *port,
                                              char ticket[65]) {
    char parsed_host[65], parsed_ticket[65];
    uint16_t parsed_port;
    if (host != NULL && host_size != 0) {
        host[0] = '\0';
    }
    if (port != NULL) {
        *port = 0;
    }
    if (ticket != NULL) {
        ticket[0] = '\0';
    }
    if (host == NULL || host_size == 0 || port == NULL || ticket == NULL ||
        (authorization_required && authorization != RENDEZVOUS_SERVER_AUTH_AUTHORIZED) ||
        !socket_rendezvous_client_candidate_parse_raw(message,
                                                      VS(parsed_host),
                                                      &parsed_port,
                                                      parsed_ticket) ||
        (expected_ticket != NULL && strcmp(parsed_ticket, expected_ticket) != 0) ||
        strlen(parsed_host) >= host_size) {
        return false;
    }
    snprintf(host, host_size, "%s", parsed_host);
    *port = parsed_port;
    snprintf(ticket, 65, "%s", parsed_ticket);
    return true;
}

bool socket_rendezvous_server_candidate_render(char *buffer,
                                               size_t size,
                                               const socket_direct_candidate_t *candidate,
                                               const char *ticket,
                                               bool authorization_required,
                                               rendezvous_server_auth_state_t authorization) {
    if (buffer != NULL && size != 0) {
        buffer[0] = '\0';
    }
    return candidate != NULL &&
           (!authorization_required || authorization == RENDEZVOUS_SERVER_AUTH_AUTHORIZED) &&
           socket_rendezvous_message_render(buffer,
                                            size,
                                            "server_candidate",
                                            candidate->host,
                                            candidate->port,
                                            candidate->kind,
                                            ticket);
}

bool socket_rendezvous_complete_render(char *buffer, size_t size, const char *ticket) {
    return socket_rendezvous_message_render(buffer,
                                            size,
                                            "complete",
                                            NULL,
                                            0,
                                            SOCKET_CANDIDATE_NUM,
                                            ticket);
}

bool socket_rendezvous_complete_parse(const char *message, const char *expected_ticket) {
    char expected[128];
    return message != NULL && socket_rendezvous_complete_render(VS(expected), expected_ticket) &&
           strcmp(message, expected) == 0;
}

static void socket_rendezvous_attempt_fail(socket_rendezvous_attempt_t *attempt) {
    if (attempt == NULL) {
        return;
    }
    attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_TERMINAL;
    if (attempt->has_invite) {
        rendezvous_invite_cleanse(&attempt->invite);
        attempt->has_invite = false;
    }
}

static bool socket_rendezvous_attempt_record(socket_rendezvous_attempt_t *attempt,
                                             bool server_frame,
                                             bool authorization_frame,
                                             size_t bytes) {
    if (attempt == NULL || attempt->state == SOCKET_RENDEZVOUS_ATTEMPT_TERMINAL ||
        attempt->state == SOCKET_RENDEZVOUS_ATTEMPT_COMPLETE || bytes == 0 ||
        bytes > RENDEZVOUS_FRAME_MAX ||
        attempt->stats.signal_bytes > SOCKET_RENDEZVOUS_SIGNAL_BYTES_MAX - bytes ||
        (server_frame && attempt->stats.server_frames >= SOCKET_RENDEZVOUS_SERVER_FRAMES_MAX) ||
        (!server_frame && attempt->stats.client_frames >= SOCKET_RENDEZVOUS_CLIENT_FRAMES_MAX) ||
        (authorization_frame &&
         (attempt->authorization_frames >= SOCKET_RENDEZVOUS_AUTH_FRAMES_MAX ||
          attempt->authorization_bytes > SOCKET_RENDEZVOUS_AUTH_BYTES_MAX - bytes))) {
        socket_rendezvous_attempt_fail(attempt);
        return false;
    }
    if (server_frame) {
        attempt->stats.server_frames++;
    } else {
        attempt->stats.client_frames++;
    }
    attempt->stats.signal_bytes += bytes;
    if (authorization_frame) {
        attempt->authorization_frames++;
        attempt->authorization_bytes += bytes;
    }
    return true;
}

static bool socket_rendezvous_attempt_input(socket_rendezvous_attempt_t *attempt,
                                            const char *frame,
                                            size_t frame_size,
                                            bool authorization_frame,
                                            char canonical[RENDEZVOUS_FRAME_MAX + 1U]) {
    if (!socket_rendezvous_attempt_record(attempt, true, authorization_frame, frame_size) ||
        frame == NULL || memchr(frame, '\0', frame_size) != NULL) {
        socket_rendezvous_attempt_fail(attempt);
        return false;
    }
    memcpy(canonical, frame, frame_size);
    canonical[frame_size] = '\0';
    return true;
}

socket_rendezvous_attempt_t *socket_rendezvous_attempt_create(const char *server_id,
                                                              const char *ticket,
                                                              const rendezvous_invite_t *invite,
                                                              uint64_t deadline_ms) {
    if (!string_is_hex_fixed(server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) ||
        !socket_rendezvous_ticket_valid(ticket) || deadline_ms == 0 ||
        (invite != NULL && !rendezvous_invite_matches_server(invite, server_id))) {
        return NULL;
    }
    socket_rendezvous_attempt_t *attempt = calloc(1, sizeof(*attempt));
    if (attempt == NULL) {
        return NULL;
    }
    memcpy(attempt->server_id, server_id, sizeof(attempt->server_id));
    memcpy(attempt->ticket, ticket, sizeof(attempt->ticket));
    attempt->deadline_ms = deadline_ms;
    attempt->authorization_required = invite != NULL;
    attempt->state =
        invite != NULL ? SOCKET_RENDEZVOUS_ATTEMPT_NEW : SOCKET_RENDEZVOUS_ATTEMPT_READY;
    if (invite != NULL) {
        attempt->invite = *invite;
        attempt->has_invite = true;
    }
    return attempt;
}

void socket_rendezvous_attempt_destroy(socket_rendezvous_attempt_t *attempt) {
    if (attempt != NULL) {
        OPENSSL_cleanse(attempt, sizeof(*attempt));
        free(attempt);
    }
}

bool socket_rendezvous_attempt_expired(const socket_rendezvous_attempt_t *attempt,
                                       uint64_t now_ms) {
    return attempt == NULL || now_ms >= attempt->deadline_ms;
}

size_t socket_rendezvous_attempt_header(char *data, size_t size, size_t count, void *user_data) {
    socket_rendezvous_attempt_t *attempt = user_data;
    if (attempt == NULL || (size != 0 && count > SIZE_MAX / size)) {
        return 0;
    }
    size_t bytes = size * count;
    if (bytes != 0 && data == NULL) {
        return 0;
    }
    static const char status_prefix[] = "HTTP/";
    static const char retry_name[] = "Retry-After:";
    if (bytes >= sizeof(status_prefix) - 1U &&
        memcmp(data, status_prefix, sizeof(status_prefix) - 1U) == 0) {
        attempt->retry_after_seconds = 0;
    } else if (bytes >= sizeof(retry_name) - 1U &&
               strncasecmp(data, retry_name, sizeof(retry_name) - 1U) == 0) {
        attempt->retry_after_seconds = 0;
        const char *cursor = data + sizeof(retry_name) - 1U;
        const char *end = data + bytes;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
            cursor++;
        }
        while (end > cursor &&
               (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        char value[11];
        size_t length = (size_t)(end - cursor);
        uint64_t parsed;
        if (length > 0 && length < sizeof(value)) {
            memcpy(value, cursor, length);
            value[length] = '\0';
            if (string_parse_uint64(value, 10, 1, SOCKET_RENDEZVOUS_RETRY_AFTER_MAX, &parsed)) {
                attempt->retry_after_seconds = (uint32_t)parsed;
            }
        }
    }
    return rendezvous_websocket_protocol_header(data, size, count, &attempt->protocol);
}

bool socket_rendezvous_attempt_protocol_valid(const socket_rendezvous_attempt_t *attempt) {
    return attempt != NULL && (attempt->authorization_required
                                   ? rendezvous_websocket_protocol_valid(&attempt->protocol)
                                   : attempt->protocol.echoes == 0 && !attempt->protocol.invalid);
}

uint32_t socket_rendezvous_attempt_retry_after(const socket_rendezvous_attempt_t *attempt) {
    return attempt != NULL ? attempt->retry_after_seconds : 0;
}

bool socket_rendezvous_attempt_directory_probe_allowed(const socket_rendezvous_attempt_t *attempt) {
    return attempt != NULL && !attempt->authorization_required;
}

bool socket_rendezvous_attempt_peer_traffic_allowed(const socket_rendezvous_attempt_t *attempt) {
    return attempt != NULL && attempt->state == SOCKET_RENDEZVOUS_ATTEMPT_WAIT_SERVER;
}

bool socket_rendezvous_attempt_auth_init(socket_rendezvous_attempt_t *attempt,
                                         char *frame,
                                         size_t frame_size) {
    if (frame == NULL || frame_size == 0) {
        socket_rendezvous_attempt_fail(attempt);
        return false;
    }
    frame[0] = '\0';
    if (attempt == NULL || attempt->state != SOCKET_RENDEZVOUS_ATTEMPT_NEW ||
        socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms()) ||
        !rendezvous_auth_init_render(frame,
                                     frame_size,
                                     attempt->ticket,
                                     attempt->invite.invite_id) ||
        !socket_rendezvous_attempt_record(attempt, false, true, strlen(frame))) {
        socket_rendezvous_attempt_fail(attempt);
        frame[0] = '\0';
        return false;
    }
    attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_WAIT_CHALLENGE;
    return true;
}

socket_rendezvous_frame_result_t
socket_rendezvous_attempt_challenge(socket_rendezvous_attempt_t *attempt,
                                    const char *frame,
                                    size_t frame_size,
                                    char *proof_frame,
                                    size_t proof_frame_size) {
    unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE] = {0};
    unsigned char proof[RENDEZVOUS_PROOF_SIZE] = {0};
    char canonical[RENDEZVOUS_FRAME_MAX + 1U];
    if (proof_frame != NULL && proof_frame_size != 0) {
        proof_frame[0] = '\0';
    }
    bool ok = attempt != NULL && proof_frame != NULL && proof_frame_size != 0 &&
              attempt->state == SOCKET_RENDEZVOUS_ATTEMPT_WAIT_CHALLENGE &&
              !socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms()) &&
              socket_rendezvous_attempt_input(attempt, frame, frame_size, true, canonical) &&
              rendezvous_auth_challenge_parse(canonical, attempt->ticket, challenge) &&
              attempt->has_invite &&
              rendezvous_invite_proof(&attempt->invite, attempt->ticket, challenge, proof) &&
              rendezvous_auth_proof_render(proof_frame, proof_frame_size, attempt->ticket, proof) &&
              socket_rendezvous_attempt_record(attempt, false, true, strlen(proof_frame));
    OPENSSL_cleanse(challenge, sizeof(challenge));
    OPENSSL_cleanse(proof, sizeof(proof));
    OPENSSL_cleanse(canonical, sizeof(canonical));
    if (!ok) {
        socket_rendezvous_attempt_fail(attempt);
        if (proof_frame != NULL && proof_frame_size != 0) {
            proof_frame[0] = '\0';
        }
        return SOCKET_RENDEZVOUS_FRAME_INVALID;
    }
    attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_WAIT_RESULT;
    return SOCKET_RENDEZVOUS_FRAME_CHALLENGE;
}

socket_rendezvous_frame_result_t
socket_rendezvous_attempt_auth_result(socket_rendezvous_attempt_t *attempt,
                                      const char *frame,
                                      size_t frame_size) {
    char canonical[RENDEZVOUS_FRAME_MAX + 1U];
    bool authorized = false;
    bool ok = attempt != NULL && attempt->state == SOCKET_RENDEZVOUS_ATTEMPT_WAIT_RESULT &&
              !socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms()) &&
              socket_rendezvous_attempt_input(attempt, frame, frame_size, true, canonical) &&
              rendezvous_auth_result_parse(canonical, attempt->ticket, &authorized);
    OPENSSL_cleanse(canonical, sizeof(canonical));
    if (!ok) {
        socket_rendezvous_attempt_fail(attempt);
        return SOCKET_RENDEZVOUS_FRAME_INVALID;
    }
    if (!authorized) {
        socket_rendezvous_attempt_fail(attempt);
        return SOCKET_RENDEZVOUS_FRAME_DENIED;
    }
    rendezvous_invite_cleanse(&attempt->invite);
    attempt->has_invite = false;
    attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_AUTHORIZED;
    return SOCKET_RENDEZVOUS_FRAME_AUTHORIZED;
}

bool socket_rendezvous_attempt_client_candidate(socket_rendezvous_attempt_t *attempt,
                                                const char *host,
                                                uint16_t port,
                                                char *frame,
                                                size_t frame_size) {
    if (frame == NULL || frame_size == 0) {
        socket_rendezvous_attempt_fail(attempt);
        return false;
    }
    frame[0] = '\0';
    if (attempt == NULL ||
        (attempt->state != SOCKET_RENDEZVOUS_ATTEMPT_READY &&
         attempt->state != SOCKET_RENDEZVOUS_ATTEMPT_AUTHORIZED) ||
        socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms()) ||
        !socket_rendezvous_message_render(frame,
                                          frame_size,
                                          "client_candidate",
                                          host,
                                          port,
                                          SOCKET_CANDIDATE_NUM,
                                          attempt->ticket) ||
        !socket_rendezvous_attempt_record(attempt, false, false, strlen(frame))) {
        socket_rendezvous_attempt_fail(attempt);
        frame[0] = '\0';
        return false;
    }
    attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_WAIT_SERVER;
    return true;
}

socket_rendezvous_frame_result_t
socket_rendezvous_attempt_server_frame(socket_rendezvous_attempt_t *attempt,
                                       const char *frame,
                                       size_t frame_size,
                                       socket_direct_candidate_t *candidate) {
    char canonical[RENDEZVOUS_FRAME_MAX + 1U] = {0};
    socket_direct_candidate_t parsed = {0};
    if (candidate != NULL) {
        memset(candidate, 0, sizeof(*candidate));
    }
    if (attempt == NULL || candidate == NULL ||
        attempt->state != SOCKET_RENDEZVOUS_ATTEMPT_WAIT_SERVER ||
        socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms()) ||
        !socket_rendezvous_attempt_input(attempt, frame, frame_size, false, canonical)) {
        socket_rendezvous_attempt_fail(attempt);
        OPENSSL_cleanse(canonical, sizeof(canonical));
        return SOCKET_RENDEZVOUS_FRAME_INVALID;
    }
    if (socket_rendezvous_server_candidate_parse_raw(canonical, attempt->ticket, &parsed)) {
        if (attempt->stats.server_candidates >= SOCKET_RENDEZVOUS_SERVER_CANDIDATES_MAX) {
            socket_rendezvous_attempt_fail(attempt);
            OPENSSL_cleanse(canonical, sizeof(canonical));
            return SOCKET_RENDEZVOUS_FRAME_INVALID;
        }
        attempt->stats.server_candidates++;
        *candidate = parsed;
        OPENSSL_cleanse(canonical, sizeof(canonical));
        return SOCKET_RENDEZVOUS_FRAME_CANDIDATE;
    }
    if (socket_rendezvous_complete_parse(canonical, attempt->ticket)) {
        attempt->state = SOCKET_RENDEZVOUS_ATTEMPT_COMPLETE;
        OPENSSL_cleanse(canonical, sizeof(canonical));
        return SOCKET_RENDEZVOUS_FRAME_COMPLETE;
    }
    socket_rendezvous_attempt_fail(attempt);
    OPENSSL_cleanse(canonical, sizeof(canonical));
    return SOCKET_RENDEZVOUS_FRAME_INVALID;
}

bool socket_rendezvous_attempt_stats(const socket_rendezvous_attempt_t *attempt,
                                     socket_rendezvous_stats_t *stats) {
    if (attempt == NULL || stats == NULL) {
        return false;
    }
    *stats = attempt->stats;
    return true;
}

static bool socket_candidate_add(socket_direct_candidate_t *candidates,
                                 size_t *count,
                                 size_t capacity,
                                 const char *host,
                                 uint16_t port,
                                 socket_candidate_kind_t kind) {
    for (size_t i = 0; i < *count; i++) {
        if (candidates[i].port == port && strcmp(candidates[i].host, host) == 0) {
            return true;
        }
    }
    if (*count >= capacity) {
        return false;
    }

    snprintf(VS(candidates[*count].host), "%s", host);
    candidates[*count].port = port;
    candidates[*count].kind = kind;
    (*count)++;
    return true;
}

static bool socket_candidate_address_valid(const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *address4 = (const struct sockaddr_in *)address;
        uint32_t value = ntohl(address4->sin_addr.s_addr);
        return value != INADDR_ANY && (value >> 24) != 127 && (value & 0xf0000000U) != 0xe0000000U;
    }
#ifdef HAVE_IPV6
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *address6 = (const struct sockaddr_in6 *)address;
        return !IN6_IS_ADDR_UNSPECIFIED(&address6->sin6_addr) &&
               !IN6_IS_ADDR_LOOPBACK(&address6->sin6_addr) &&
               !IN6_IS_ADDR_LINKLOCAL(&address6->sin6_addr) &&
               !IN6_IS_ADDR_MULTICAST(&address6->sin6_addr);
    }
#endif
    return false;
}

static bool socket_candidate_address_global(const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *address4 = (const struct sockaddr_in *)address;
        uint32_t value = ntohl(address4->sin_addr.s_addr);
        return (value & 0xff000000U) != 0 && (value & 0xff000000U) != 0x0a000000U &&
               (value & 0xfff00000U) != 0xac100000U && (value & 0xffff0000U) != 0xc0a80000U &&
               (value & 0xffc00000U) != 0x64400000U && (value & 0xffff0000U) != 0xa9fe0000U &&
               (value & 0xff000000U) != 0x7f000000U && value < 0xe0000000U;
    }
#ifdef HAVE_IPV6
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *address6 = (const struct sockaddr_in6 *)address;
        return (address6->sin6_addr.s6_addr[0] & 0xe0) == 0x20;
    }
#endif
    return false;
}

bool socket_host_is_global(const char *host) {
    HARD_ASSERT(host != NULL);

    struct sockaddr_storage address;
    memset(&address, 0, sizeof(address));
    struct sockaddr_in *address4 = (struct sockaddr_in *)&address;
    if (inet_pton(AF_INET, host, &address4->sin_addr) == 1) {
        address4->sin_family = AF_INET;
        return socket_candidate_address_global((const struct sockaddr *)&address);
    }
#ifdef HAVE_IPV6
    struct sockaddr_in6 *address6 = (struct sockaddr_in6 *)&address;
    if (inet_pton(AF_INET6, host, &address6->sin6_addr) == 1) {
        address6->sin6_family = AF_INET6;
        return socket_candidate_address_global((const struct sockaddr *)&address);
    }
#endif
    return false;
}

size_t
socket_local_candidates(uint16_t port, socket_direct_candidate_t *candidates, size_t capacity) {
    HARD_ASSERT(candidates != NULL || capacity == 0);

    size_t count = 0;
#ifdef WIN32
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *adapters = xmalloc(size);
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                                    NULL,
                                    adapters,
                                    &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        adapters = xrealloc(adapters, size);
        rc = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL,
                                  adapters,
                                  &size);
    }
    if (rc == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            for (IP_ADAPTER_UNICAST_ADDRESS *entry = adapter->FirstUnicastAddress; entry != NULL;
                 entry = entry->Next) {
                const struct sockaddr *address = entry->Address.lpSockaddr;
                if (address == NULL || !socket_candidate_address_valid(address)) {
                    continue;
                }
                char host[65];
                if (getnameinfo(address,
                                entry->Address.iSockaddrLength,
                                VS(host),
                                NULL,
                                0,
                                NI_NUMERICHOST) == 0) {
                    socket_candidate_add(candidates,
                                         &count,
                                         capacity,
                                         host,
                                         port,
                                         socket_candidate_address_global(address)
                                             ? SOCKET_CANDIDATE_IPV6
                                             : SOCKET_CANDIDATE_LAN);
                }
            }
        }
    }
    free(adapters);
#else
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return 0;
    }
    for (struct ifaddrs *entry = interfaces; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL || (entry->ifa_flags & IFF_UP) == 0 ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
            !socket_candidate_address_valid(entry->ifa_addr)) {
            continue;
        }

        socklen_t address_length = entry->ifa_addr->sa_family == AF_INET
                                       ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6);
        char host[65];
        if (getnameinfo(entry->ifa_addr, address_length, VS(host), NULL, 0, NI_NUMERICHOST) == 0) {
            socket_candidate_add(candidates,
                                 &count,
                                 capacity,
                                 host,
                                 port,
                                 socket_candidate_address_global(entry->ifa_addr)
                                     ? SOCKET_CANDIDATE_IPV6
                                     : SOCKET_CANDIDATE_LAN);
        }
    }
    freeifaddrs(interfaces);
#endif
    return count;
}

static uint16_t socket_stun_u16(const unsigned char *b) {
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static uint32_t socket_stun_u32(const unsigned char *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static bool socket_stun_response_attributable(const socket_t *sc,
                                              const unsigned char *datagram,
                                              size_t length,
                                              const struct sockaddr_storage *source,
                                              socklen_t source_length);

bool socket_stun_discover_until(socket_t *sc,
                                const char *endpoint,
                                char *host,
                                size_t host_size,
                                uint16_t *port,
                                uint64_t deadline_ms) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(endpoint != NULL);
    HARD_ASSERT(host != NULL);
    HARD_ASSERT(port != NULL);

    const char *separator = strrchr(endpoint, ':');
    if (separator == NULL || separator == endpoint || separator[1] == '\0' ||
        strlen(separator + 1) >= 6) {
        LOG(ERROR, "The configured STUN endpoint is invalid");
        return false;
    }
    char stun_host[MAX_BUF], stun_port[6];
    size_t stun_host_length = (size_t)(separator - endpoint);
    if (stun_host_length >= sizeof(stun_host)) {
        return false;
    }
    memcpy(stun_host, endpoint, stun_host_length);
    stun_host[stun_host_length] = '\0';
    snprintf(VS(stun_port), "%s", separator + 1);

    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = ((struct sockaddr *)&sc->addr)->sa_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    int rc;
    bool resolution_timed_out;
    addresses = socket_stun_resolve_until(stun_host,
                                          stun_port,
                                          &hints,
                                          deadline_ms,
                                          &rc,
                                          &resolution_timed_out);
    if (resolution_timed_out) {
        LOG(ERROR, "STUN endpoint resolution timed out");
        return false;
    }
    if (rc != 0) {
        LOG(ERROR, "Cannot resolve the configured STUN endpoint: %s", gai_strerror(rc));
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        return false;
    }

    unsigned char request[20] = {0};
    request[1] = 1;
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xa4;
    request[7] = 0x42;
    if (RAND_bytes(request + 8, 12) != 1) {
        freeaddrinfo(addresses);
        return false;
    }

    bool sent = false;
    for (struct addrinfo *ai = addresses; ai != NULL; ai = ai->ai_next) {
        if (sendto(sc->handle,
                   (const char *)request,
                   sizeof(request),
                   0,
                   ai->ai_addr,
                   ai->ai_addrlen) == (ssize_t)sizeof(request)) {
            sent = true;
            memcpy(&sc->late_stun_source, ai->ai_addr, ai->ai_addrlen);
            sc->late_stun_source_length = (socklen_t)ai->ai_addrlen;
            memcpy(sc->late_stun_transaction, request + 8, sizeof(sc->late_stun_transaction));
            sc->late_stun_pending = true;
            break;
        }
    }
    if (addresses != NULL) {
        freeaddrinfo(addresses);
    }
    if (!sent) {
        LOG(ERROR, "Failed to send a request to the configured STUN endpoint");
        return false;
    }

    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_stun_after_send_t after_send = socket_stun_after_send;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
    if (after_send != NULL) {
        after_send();
    }

    unsigned char response[1024];
    struct sockaddr_storage response_source;
    socklen_t response_source_length;
    ssize_t length;
    for (;;) {
        uint64_t now_ms = socket_stun_clock_get();
        if (now_ms >= deadline_ms) {
            LOG(ERROR, "STUN request timed out");
            return false;
        }
        uint64_t timeout_ms = MIN(deadline_ms - now_ms, 3000U);
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sc->handle, &readfds);
        struct timeval timeout = {.tv_sec = (long)(timeout_ms / 1000U),
                                  .tv_usec = (long)((timeout_ms % 1000U) * 1000U)};
        if (select(sc->handle + 1, &readfds, NULL, NULL, &timeout) != 1) {
            LOG(ERROR, "STUN request timed out");
            return false;
        }
        response_source_length = sizeof(response_source);
        length = recvfrom(sc->handle,
                          (char *)response,
                          sizeof(response),
                          0,
                          (struct sockaddr *)&response_source,
                          &response_source_length);
        if (length >= 0 && socket_stun_response_attributable(sc,
                                                             response,
                                                             (size_t)length,
                                                             &response_source,
                                                             response_source_length)) {
            break;
        }
        LOG(ERROR, "Ignoring an unrelated STUN response");
    }
    if (length < 20 || socket_stun_u16(response) != 0x0101 ||
        socket_stun_u32(response + 4) != SOCKET_STUN_MAGIC ||
        memcmp(response + 8, request + 8, 12) != 0) {
        LOG(ERROR, "Invalid STUN response");
        return false;
    }

    size_t message_length = socket_stun_u16(response + 2);
    if ((message_length & 3U) != 0 || message_length != (size_t)length - 20) {
        LOG(ERROR, "Invalid STUN response");
        return false;
    }
    size_t message_end = 20 + message_length;
    for (size_t offset = 20; offset < message_end;) {
        if (message_end - offset < 4) {
            LOG(ERROR, "Invalid STUN response");
            return false;
        }
        uint16_t type = socket_stun_u16(response + offset);
        size_t value_length = socket_stun_u16(response + offset + 2);
        const unsigned char *value = response + offset + 4;
        size_t padded_length = (value_length + 3U) & ~(size_t)3U;
        if (value_length > message_end - offset - 4 || padded_length > message_end - offset - 4) {
            LOG(ERROR, "Invalid STUN response");
            return false;
        }
        if (type == 0x0020) {
            if (value_length < 4) {
                LOG(ERROR, "Invalid STUN response");
                return false;
            }
            int family = value[1];
            *port = socket_stun_u16(value + 2) ^ (uint16_t)(SOCKET_STUN_MAGIC >> 16);
            unsigned char address[16];
            size_t address_length = family == 1 ? 4 : family == 2 ? 16 : 0;
            if (address_length == 0 || value_length != 4 + address_length) {
                LOG(ERROR, "Invalid STUN response");
                return false;
            }
            unsigned char mask[16] = {0x21, 0x12, 0xa4, 0x42};
            memcpy(mask + 4, request + 8, 12);
            for (size_t i = 0; i < address_length; i++) {
                address[i] = value[4 + i] ^ mask[i];
            }
            if (*port == 0 ||
                inet_ntop(family == 1 ? AF_INET : AF_INET6, address, host, host_size) == NULL ||
                !socket_rendezvous_host_valid(host)) {
                LOG(ERROR, "STUN response contained an unusable mapped address");
                return false;
            }
            return true;
        }
        offset += 4 + padded_length;
    }

    LOG(ERROR, "STUN response did not contain XOR-MAPPED-ADDRESS");
    return false;
}

bool socket_stun_discover(socket_t *sc,
                          const char *endpoint,
                          char *host,
                          size_t host_size,
                          uint16_t *port) {
    uint64_t now_ms = datetime_monotonic_ms();
    return socket_stun_discover_until(sc, endpoint, host, host_size, port, now_ms + 3000U);
}

bool socket_udp_punch(socket_t *sc, const char *host, uint16_t port) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(host != NULL);

    char port_string[6];
    snprintf(VS(port_string), "%" PRIu16, port);
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = ((struct sockaddr *)&sc->addr)->sa_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, port_string, &hints, &addresses) != 0) {
        return false;
    }

    static const char probe[] = SOCKET_PUNCH_PROBE;
    bool ok = false;
    for (struct addrinfo *ai = addresses; ai != NULL; ai = ai->ai_next) {
        if (sendto(sc->handle, probe, sizeof(probe) - 1, 0, ai->ai_addr, ai->ai_addrlen) ==
            (ssize_t)(sizeof(probe) - 1)) {
            ok = true;
        }
    }
    freeaddrinfo(addresses);
    return ok;
}

static bool socket_address_equal(const struct sockaddr_storage *left,
                                 socklen_t left_length,
                                 const struct sockaddr_storage *right,
                                 socklen_t right_length) {
    if (left_length != right_length || left->ss_family != right->ss_family) {
        return false;
    }
    if (left->ss_family == AF_INET) {
        const struct sockaddr_in *left_v4 = (const struct sockaddr_in *)left;
        const struct sockaddr_in *right_v4 = (const struct sockaddr_in *)right;
        return left_v4->sin_port == right_v4->sin_port &&
               left_v4->sin_addr.s_addr == right_v4->sin_addr.s_addr;
    }
    if (left->ss_family == AF_INET6) {
        const struct sockaddr_in6 *left_v6 = (const struct sockaddr_in6 *)left;
        const struct sockaddr_in6 *right_v6 = (const struct sockaddr_in6 *)right;
        return left_v6->sin6_port == right_v6->sin6_port &&
               left_v6->sin6_scope_id == right_v6->sin6_scope_id &&
               memcmp(&left_v6->sin6_addr, &right_v6->sin6_addr, sizeof(left_v6->sin6_addr)) == 0;
    }
    return false;
}

static bool socket_stun_response_attributable(const socket_t *sc,
                                              const unsigned char *datagram,
                                              size_t length,
                                              const struct sockaddr_storage *source,
                                              socklen_t source_length) {
    if (!sc->late_stun_pending || length < 20 ||
        !socket_address_equal(&sc->late_stun_source,
                              sc->late_stun_source_length,
                              source,
                              source_length)) {
        return false;
    }
    uint16_t message_type = socket_stun_u16(datagram);
    return (message_type == 0x0101 || message_type == 0x0111) &&
           socket_stun_u32(datagram + 4) == SOCKET_STUN_MAGIC &&
           memcmp(datagram + 8, sc->late_stun_transaction, 12) == 0;
}

bool socket_udp_punch_receive(socket_t *sc, char *host, size_t host_size, uint16_t *port) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(host != NULL);
    HARD_ASSERT(port != NULL);

    char datagram[UINT16_MAX];
    struct sockaddr_storage source;
    socklen_t source_length = sizeof(source);
    ssize_t length;
    for (unsigned int drained = 0;; drained++) {
        source_length = sizeof(source);
        length = recvfrom(sc->handle,
                          datagram,
                          sizeof(datagram),
                          MSG_PEEK,
                          (struct sockaddr *)&source,
                          &source_length);
        if (length < 0 || !socket_stun_response_attributable(sc,
                                                             (const unsigned char *)datagram,
                                                             (size_t)length,
                                                             &source,
                                                             source_length)) {
            break;
        }
        if (drained >= SOCKET_STUN_LATE_DRAIN_MAX) {
            return false;
        }
        source_length = sizeof(source);
        if (recvfrom(sc->handle,
                     datagram,
                     sizeof(datagram),
                     0,
                     (struct sockaddr *)&source,
                     &source_length) != length) {
            return false;
        }
    }
    if ((size_t)length != sizeof(SOCKET_PUNCH_PROBE) - 1 ||
        memcmp(datagram, SOCKET_PUNCH_PROBE, sizeof(SOCKET_PUNCH_PROBE) - 1) != 0) {
        return false;
    }

    char probe[sizeof(SOCKET_PUNCH_PROBE)];
    source_length = sizeof(source);
    length =
        recvfrom(sc->handle, probe, sizeof(probe), 0, (struct sockaddr *)&source, &source_length);
    if ((size_t)length != sizeof(SOCKET_PUNCH_PROBE) - 1) {
        return false;
    }

    char service[6];
    if (getnameinfo((const struct sockaddr *)&source,
                    source_length,
                    host,
                    (socklen_t)host_size,
                    VS(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    uint64_t value;
    if (!string_parse_uint64(service, 10, 1, UINT16_MAX, &value)) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

void socket_punch_pacer_start(socket_punch_pacer_t *pacer, uint64_t now_ms, unsigned int grace_ms) {
    HARD_ASSERT(pacer != NULL);

    pacer->next_action_ms = now_ms;
    pacer->attempts = 0;
    pacer->grace_ms = grace_ms;
    pacer->active = true;
}

socket_punch_action_t socket_punch_pacer_poll(const socket_punch_pacer_t *pacer, uint64_t now_ms) {
    HARD_ASSERT(pacer != NULL);

    if (!pacer->active || now_ms < pacer->next_action_ms) {
        return SOCKET_PUNCH_WAIT;
    }
    return pacer->attempts < SOCKET_PUNCH_COUNT ? SOCKET_PUNCH_SEND : SOCKET_PUNCH_COMPLETE;
}

void socket_punch_pacer_advance(socket_punch_pacer_t *pacer,
                                uint64_t now_ms,
                                socket_punch_action_t action) {
    HARD_ASSERT(pacer != NULL);

    if (action == SOCKET_PUNCH_SEND) {
        pacer->attempts++;
        pacer->next_action_ms =
            now_ms +
            (pacer->attempts < SOCKET_PUNCH_COUNT ? SOCKET_PUNCH_INTERVAL_MS : pacer->grace_ms);
    } else if (action == SOCKET_PUNCH_COMPLETE) {
        pacer->active = false;
    }
}

static bool
socket_bound_local_candidate(socket_t *sc, char *host, size_t host_size, uint16_t *port) {
    int family = ((struct sockaddr *)&sc->addr)->sa_family;
    struct sockaddr_storage wildcard;
    socklen_t wildcard_length;
    memset(&wildcard, 0, sizeof(wildcard));
    if (family == AF_INET) {
        struct sockaddr_in *address4 = (struct sockaddr_in *)&wildcard;
        address4->sin_family = AF_INET;
        address4->sin_addr.s_addr = htonl(INADDR_ANY);
        wildcard_length = sizeof(*address4);
#ifdef HAVE_IPV6
    } else if (family == AF_INET6) {
        struct sockaddr_in6 *address6 = (struct sockaddr_in6 *)&wildcard;
        address6->sin6_family = AF_INET6;
        address6->sin6_addr = in6addr_any;
        wildcard_length = sizeof(*address6);
#endif
    } else {
        return false;
    }

    uint16_t local_port;
    if (!socket_local_port(sc, &local_port)) {
        if (bind(sc->handle, (const struct sockaddr *)&wildcard, wildcard_length) != 0 ||
            !socket_local_port(sc, &local_port)) {
            return false;
        }
    }

    socket_direct_candidate_t local_candidates[SOCKET_DIRECT_MAX_CANDIDATES];
    size_t count =
        socket_local_candidates(local_port, local_candidates, arraysize(local_candidates));
    for (size_t i = 0; i < count; i++) {
        bool family_ipv6 = false;
#ifdef HAVE_IPV6
        family_ipv6 = family == AF_INET6;
#endif
        bool candidate_ipv6 = strchr(local_candidates[i].host, ':') != NULL;
        if (family_ipv6 != candidate_ipv6 || strlen(local_candidates[i].host) >= host_size) {
            continue;
        }
        snprintf(host, host_size, "%s", local_candidates[i].host);
        *port = local_port;
        return true;
    }
    return false;
}

static bool socket_local_candidate(socket_t *sc, char *host, size_t host_size, uint16_t *port) {
    if (!socket_candidate_address_valid((const struct sockaddr *)&sc->addr)) {
        return socket_bound_local_candidate(sc, host, host_size, port);
    }

    socklen_t peer_length;
    int family = ((struct sockaddr *)&sc->addr)->sa_family;
    if (family == AF_INET) {
        peer_length = sizeof(struct sockaddr_in);
#ifdef HAVE_IPV6
    } else if (family == AF_INET6) {
        peer_length = sizeof(struct sockaddr_in6);
#endif
    } else {
        return false;
    }

    static const char probe[] = SOCKET_PUNCH_PROBE;
    if (sendto(sc->handle,
               probe,
               sizeof(probe) - 1,
               0,
               (const struct sockaddr *)&sc->addr,
               peer_length) != (ssize_t)(sizeof(probe) - 1)) {
        return false;
    }

    struct sockaddr_storage local;
    socklen_t local_length = sizeof(local);
    if (getsockname(sc->handle, (struct sockaddr *)&local, &local_length) != 0) {
        return false;
    }
    if (!socket_candidate_address_valid((const struct sockaddr *)&local)) {
        return socket_bound_local_candidate(sc, host, host_size, port);
    }
    char service[6];
    if (getnameinfo((const struct sockaddr *)&local,
                    local_length,
                    host,
                    (socklen_t)host_size,
                    VS(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    uint64_t value;
    if (!string_parse_uint64(service, 10, 1, UINT16_MAX, &value)) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static void socket_udp_punch_schedule(socket_punch_job_t *jobs,
                                      size_t capacity,
                                      const socket_direct_candidate_t *candidate) {
    if (candidate->kind != SOCKET_CANDIDATE_MAPPED && candidate->kind != SOCKET_CANDIDATE_SRFLX) {
        return;
    }

    socket_punch_job_t *available = NULL;
    for (size_t i = 0; i < capacity; i++) {
        if (jobs[i].pacer.active && jobs[i].candidate.port == candidate->port &&
            strcmp(jobs[i].candidate.host, candidate->host) == 0) {
            return;
        }
        if (!jobs[i].pacer.active && available == NULL) {
            available = &jobs[i];
        }
    }
    if (available == NULL) {
        LOG(ERROR, "Client UDP punch queue is full");
        return;
    }

    available->candidate = *candidate;
    socket_punch_pacer_start(&available->pacer, datetime_monotonic_ms(), 0);
    LOG(INFO, "Opening a paced UDP path to a rendezvous QUIC candidate");
}

static void socket_udp_punch_update(socket_t *sc,
                                    socket_punch_job_t *jobs,
                                    size_t capacity,
                                    unsigned int *attempts,
                                    unsigned int *successful) {
    uint64_t now = datetime_monotonic_ms();
    for (size_t i = 0; i < capacity; i++) {
        socket_punch_job_t *job = &jobs[i];
        socket_punch_action_t action = socket_punch_pacer_poll(&job->pacer, now);
        if (action == SOCKET_PUNCH_WAIT) {
            continue;
        }

        if (action == SOCKET_PUNCH_COMPLETE) {
            socket_punch_pacer_advance(&job->pacer, now, action);
            continue;
        }

        (*attempts)++;
        if (socket_udp_punch(sc, job->candidate.host, job->candidate.port)) {
            (*successful)++;
        }
        socket_punch_pacer_advance(&job->pacer, now, action);
    }
}

static size_t socket_udp_punch_collect(socket_t *sc,
                                       socket_direct_candidate_t *candidates,
                                       size_t *count,
                                       size_t capacity) {
    size_t received = 0;
    while (received < SOCKET_PUNCH_DRAIN_MAX) {
        char host[65];
        uint16_t port;
        if (!socket_udp_punch_receive(sc, VS(host), &port)) {
            return received;
        }
        received++;

        bool already_recorded = false;
        bool has_peer_reflexive = false;
        for (size_t i = 0; i < *count; i++) {
            if (candidates[i].kind == SOCKET_CANDIDATE_PRFLX) {
                has_peer_reflexive = true;
            }
            if (candidates[i].port == port && strcmp(candidates[i].host, host) == 0) {
                if (candidates[i].kind != SOCKET_CANDIDATE_PRFLX) {
                    candidates[i].kind = SOCKET_CANDIDATE_PRFLX;
                    LOG(INFO, "Confirmed a peer-reflexive QUIC candidate from a UDP punch");
                }
                already_recorded = true;
                break;
            }
        }
        if (already_recorded || has_peer_reflexive) {
            continue;
        }

        size_t previous_count = *count;
        socket_candidate_add(candidates, count, capacity, host, port, SOCKET_CANDIDATE_PRFLX);
        if (*count != previous_count) {
            LOG(INFO, "Learned a peer-reflexive QUIC candidate from a UDP punch");
        }
    }
    return received;
}

#if LIBCURL_VERSION_NUM >= 0x075600
static bool socket_websocket_send_text(CURL *curl, const char *message) {
    size_t sent = 0;
    size_t length = strlen(message);
    return curl_ws_send(curl, message, length, &sent, 0, CURLWS_TEXT) == CURLE_OK && sent == length;
}

static socket_connect_failure_code_t
socket_rendezvous_authorize(CURL *curl, socket_rendezvous_attempt_t *attempt) {
    char frame[RENDEZVOUS_FRAME_MAX + 1U];
    size_t used = 0;
    char proof_frame[RENDEZVOUS_FRAME_MAX + 1U];
    socket_connect_failure_code_t failure = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;

    if (!socket_rendezvous_attempt_auth_init(attempt, VS(frame)) ||
        !socket_websocket_send_text(curl, frame)) {
        goto out;
    }
    while (!socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
        socket_websocket_receive_state_t state = socket_websocket_receive(curl, VS(frame), &used);
        if (state == SOCKET_WEBSOCKET_EMPTY) {
            usleep(20000);
            continue;
        }
        if (state == SOCKET_WEBSOCKET_PARTIAL) {
            continue;
        }
        if (state != SOCKET_WEBSOCKET_MESSAGE ||
            socket_rendezvous_attempt_challenge(attempt, frame, used, VS(proof_frame)) !=
                SOCKET_RENDEZVOUS_FRAME_CHALLENGE ||
            !socket_websocket_send_text(curl, proof_frame)) {
            goto out;
        }
        break;
    }
    if (used == 0) {
        failure = SOCKET_CONNECT_FAILURE_TIMEOUT;
        goto out;
    }

    used = 0;
    while (!socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
        socket_websocket_receive_state_t state = socket_websocket_receive(curl, VS(frame), &used);
        if (state == SOCKET_WEBSOCKET_EMPTY) {
            usleep(20000);
            continue;
        }
        if (state == SOCKET_WEBSOCKET_PARTIAL) {
            continue;
        }
        if (state != SOCKET_WEBSOCKET_MESSAGE) {
            goto out;
        }
        socket_rendezvous_frame_result_t result =
            socket_rendezvous_attempt_auth_result(attempt, frame, used);
        failure = result == SOCKET_RENDEZVOUS_FRAME_AUTHORIZED ? SOCKET_CONNECT_FAILURE_NONE
                  : result == SOCKET_RENDEZVOUS_FRAME_DENIED
                      ? SOCKET_CONNECT_FAILURE_AUTHORIZATION
                      : SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        break;
    }
    if (used == 0) {
        failure = SOCKET_CONNECT_FAILURE_TIMEOUT;
    }

out:
    OPENSSL_cleanse(frame, sizeof(frame));
    OPENSSL_cleanse(proof_frame, sizeof(proof_frame));
    return failure;
}

socket_websocket_receive_state_t
socket_websocket_receive(void *handle, char *buffer, size_t capacity, size_t *used) {
    HARD_ASSERT(handle != NULL);
    HARD_ASSERT(buffer != NULL);
    HARD_ASSERT(used != NULL);
    if (capacity < 2 || *used >= capacity - 1) {
        return SOCKET_WEBSOCKET_CLOSED;
    }

    size_t received = 0;
#if LIBCURL_VERSION_NUM >= 0x080200
    const struct curl_ws_frame *frame = NULL;
#else
    struct curl_ws_frame *frame = NULL;
#endif
    CURLcode result = curl_ws_recv(handle, buffer + *used, capacity - 1 - *used, &received, &frame);
    if (result == CURLE_AGAIN) {
        return SOCKET_WEBSOCKET_EMPTY;
    }
    if (result != CURLE_OK || frame == NULL || (frame->flags & CURLWS_CLOSE) != 0 ||
        (frame->flags & CURLWS_TEXT) == 0 || received > capacity - 1 - *used) {
        return SOCKET_WEBSOCKET_CLOSED;
    }

    *used += received;
    if (frame->bytesleft != 0) {
        return SOCKET_WEBSOCKET_PARTIAL;
    }
    buffer[*used] = '\0';
    return SOCKET_WEBSOCKET_MESSAGE;
}

static bool socket_rendezvous_fallback_candidate(socket_t *sc,
                                                 bool directory_probe_allowed,
                                                 char *host,
                                                 size_t host_size,
                                                 uint16_t *port) {
    pthread_mutex_lock(&socket_stun_resolver_lock);
    socket_rendezvous_fallback_t fallback = socket_rendezvous_fallback;
    pthread_mutex_unlock(&socket_stun_resolver_lock);
    if (fallback != NULL) {
        return fallback(sc, directory_probe_allowed, host, host_size, port);
    }
    return directory_probe_allowed ? socket_local_candidate(sc, host, host_size, port)
                                   : socket_bound_local_candidate(sc, host, host_size, port);
}

size_t socket_rendezvous_client(socket_t *sc,
                                const char *url,
                                const char *stun_endpoint,
                                socket_rendezvous_attempt_t *attempt,
                                socket_direct_candidate_t *candidates,
                                size_t capacity,
                                socket_connect_failure_t *failure) {
    char host[65];
    uint16_t port;
    if (url == NULL || attempt == NULL || candidates == NULL || capacity == 0 ||
        capacity > SOCKET_DIRECT_MAX_CANDIDATES + 1U || failure == NULL) {
        return 0;
    }
    memset(candidates, 0, capacity * sizeof(*candidates));
    if (socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
        failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        return 0;
    }
    uint64_t now_ms = datetime_monotonic_ms();
    uint64_t stun_deadline_ms = socket_rendezvous_stun_deadline(now_ms, attempt->deadline_ms);
    bool have_candidate =
        stun_endpoint != NULL && stun_deadline_ms > now_ms &&
        socket_stun_discover_until(sc, stun_endpoint, VS(host), &port, stun_deadline_ms);
    if (!have_candidate) {
        have_candidate = socket_rendezvous_fallback_candidate(
            sc,
            socket_rendezvous_attempt_directory_probe_allowed(attempt),
            VS(host),
            &port);
    }
    if (!have_candidate) {
        LOG(ERROR, "Cannot determine a local rendezvous candidate");
        failure->code = socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())
                            ? SOCKET_CONNECT_FAILURE_TIMEOUT
                            : SOCKET_CONNECT_FAILURE_UNAVAILABLE;
        return 0;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return 0;
    }
    struct curl_slist *headers = NULL;
    if (attempt->authorization_required) {
        headers = curl_slist_append(NULL, "Sec-WebSocket-Protocol: " RENDEZVOUS_INVITE_SUBPROTOCOL);
        if (headers == NULL) {
            curl_easy_cleanup(curl);
            return 0;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, socket_rendezvous_attempt_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, attempt);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    now_ms = datetime_monotonic_ms();
    if (socket_rendezvous_attempt_expired(attempt, now_ms)) {
        failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }
    uint64_t remaining_ms = attempt->deadline_ms - now_ms;
    long curl_timeout_ms = remaining_ms > LONG_MAX ? LONG_MAX : (long)remaining_ms;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, curl_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, curl_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef WIN32
    curl_easy_setopt(curl, CURLOPT_CAINFO, "ca-bundle.crt");
#endif
    CURLcode result = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (result != CURLE_OK || !socket_rendezvous_attempt_protocol_valid(attempt)) {
        if (response_code == 401 || response_code == 403) {
            failure->code = SOCKET_CONNECT_FAILURE_AUTHORIZATION;
        } else if (response_code == 404 || response_code == 503) {
            failure->code = SOCKET_CONNECT_FAILURE_SERVER_OFFLINE;
        } else if (response_code == 429) {
            failure->code = SOCKET_CONNECT_FAILURE_RATE_LIMITED;
            failure->retry_after_seconds = socket_rendezvous_attempt_retry_after(attempt);
        } else if (response_code == 400 || response_code == 426 ||
                   (result == CURLE_OK && !socket_rendezvous_attempt_protocol_valid(attempt))) {
            failure->code = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        } else if (result == CURLE_OPERATION_TIMEDOUT ||
                   socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
            failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        } else {
            failure->code = SOCKET_CONNECT_FAILURE_UNAVAILABLE;
        }
        if (result != CURLE_OK) {
            LOG(ERROR, "Rendezvous connection failed: %s", curl_easy_strerror(result));
        } else {
            LOG(ERROR, "Rendezvous connection failed: invite subprotocol was not selected");
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }

    if (attempt->authorization_required) {
        socket_connect_failure_code_t authorization = socket_rendezvous_authorize(curl, attempt);
        if (authorization != SOCKET_CONNECT_FAILURE_NONE) {
            failure->code = authorization;
            LOG(ERROR, "Rendezvous invite authorization failed");
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return 0;
        }
    }
    if (socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
        failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        LOG(ERROR, "Rendezvous invite authorization failed");
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }

    char candidate[256];
    if (!socket_rendezvous_attempt_client_candidate(attempt, host, port, VS(candidate))) {
        failure->code = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }
    if (!socket_websocket_send_text(curl, candidate)) {
        failure->code = SOCKET_CONNECT_FAILURE_UNAVAILABLE;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }
    if (!socket_rendezvous_attempt_peer_traffic_allowed(attempt)) {
        failure->code = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return 0;
    }

    char response[RENDEZVOUS_FRAME_MAX + 1U];
    size_t used = 0;
    socket_direct_candidate_t parsed_candidates[SOCKET_DIRECT_MAX_CANDIDATES + 1U] = {0};
    size_t count = 0;
    bool complete = false;
    bool valid = true;
    socket_punch_job_t punch_jobs[SOCKET_DIRECT_MAX_CANDIDATES] = {0};
    unsigned int punch_attempts = 0;
    unsigned int punches_sent = 0;
    size_t punches_received = 0;
    while (!complete && valid &&
           !socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())) {
        socket_udp_punch_update(sc,
                                punch_jobs,
                                arraysize(punch_jobs),
                                &punch_attempts,
                                &punches_sent);
        punches_received +=
            socket_udp_punch_collect(sc, parsed_candidates, &count, arraysize(parsed_candidates));

        socket_websocket_receive_state_t receive_state =
            socket_websocket_receive(curl, VS(response), &used);
        if (receive_state == SOCKET_WEBSOCKET_EMPTY) {
            usleep(20000);
            continue;
        }
        if (receive_state == SOCKET_WEBSOCKET_PARTIAL) {
            continue;
        }
        if (receive_state != SOCKET_WEBSOCKET_MESSAGE) {
            valid = false;
            break;
        }
        socket_direct_candidate_t parsed_candidate;
        socket_rendezvous_frame_result_t frame_result =
            socket_rendezvous_attempt_server_frame(attempt, response, used, &parsed_candidate);
        if (frame_result == SOCKET_RENDEZVOUS_FRAME_CANDIDATE) {
            valid = socket_candidate_add(parsed_candidates,
                                         &count,
                                         arraysize(parsed_candidates),
                                         parsed_candidate.host,
                                         parsed_candidate.port,
                                         parsed_candidate.kind);
            if (valid) {
                socket_udp_punch_schedule(punch_jobs, arraysize(punch_jobs), &parsed_candidate);
            }
        } else if (frame_result == SOCKET_RENDEZVOUS_FRAME_COMPLETE) {
            complete = true;
        } else {
            valid = false;
        }
        used = 0;
    }

    if (valid) {
        socket_udp_punch_update(sc,
                                punch_jobs,
                                arraysize(punch_jobs),
                                &punch_attempts,
                                &punches_sent);
        punches_received +=
            socket_udp_punch_collect(sc, parsed_candidates, &count, arraysize(parsed_candidates));
    }
    LOG(INFO,
        "Rendezvous UDP punch summary: sent %u/%u probes, received %" PRIu64,
        punches_sent,
        punch_attempts,
        (uint64_t)punches_received);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!complete || !valid || count == 0 || count > capacity) {
        memset(candidates, 0, capacity * sizeof(*candidates));
        failure->code = socket_rendezvous_attempt_expired(attempt, datetime_monotonic_ms())
                            ? SOCKET_CONNECT_FAILURE_TIMEOUT
                        : valid ? SOCKET_CONNECT_FAILURE_SERVER_OFFLINE
                                : SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        return 0;
    }
    memcpy(candidates, parsed_candidates, count * sizeof(*candidates));
    failure->code = SOCKET_CONNECT_FAILURE_NONE;
    failure->retry_after_seconds = 0;
    return count;
}
#else
socket_websocket_receive_state_t
socket_websocket_receive(void *handle, char *buffer, size_t capacity, size_t *used) {
    (void)handle;
    (void)buffer;
    (void)capacity;
    (void)used;
    return SOCKET_WEBSOCKET_CLOSED;
}

size_t socket_rendezvous_client(socket_t *sc,
                                const char *url,
                                const char *stun_endpoint,
                                socket_rendezvous_attempt_t *attempt,
                                socket_direct_candidate_t *candidates,
                                size_t capacity,
                                socket_connect_failure_t *failure) {
    (void)attempt;
    if (failure != NULL) {
        failure->code = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        failure->retry_after_seconds = 0;
    }
    return 0;
}
#endif
