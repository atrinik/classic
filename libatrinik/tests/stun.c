#include "../socket_private.h"

#include <toolkit/datetime.h>
#include <toolkit/logger.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h>
#define test_close_socket close
#else
#define test_close_socket closesocket
#endif

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

typedef enum fake_stun_mode {
    FAKE_STUN_SUCCESS,
    FAKE_STUN_MALFORMED,
    FAKE_STUN_MISSING_ADDRESS,
    FAKE_STUN_UNSPECIFIED,
    FAKE_STUN_DECLARED_OVERRUN,
    FAKE_STUN_BAD_FAMILY,
    FAKE_STUN_TRUNCATED_IPV6,
    FAKE_STUN_LATE_THEN_PUNCH,
    FAKE_STUN_TIMEOUT,
} fake_stun_mode_t;

typedef struct fake_stun_server {
    int handle;
    fake_stun_mode_t mode;
    uint16_t request_port;
    uint16_t punch_port;
    bool expect_punch;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool released;
    bool sent;
} fake_stun_server_t;

static char captured_log[512];
static pthread_mutex_t captured_log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct blocking_resolver_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int entered;
    unsigned int released;
    unsigned int exited;
} blocking_resolver_state_t;

typedef struct discovery_call {
    socket_t client;
    const char *endpoint;
    uint64_t deadline_ms;
    bool result;
} discovery_call_t;

typedef struct rendezvous_call {
    socket_t *client;
    socket_rendezvous_attempt_t *attempt;
    socket_connect_failure_t failure;
    size_t count;
} rendezvous_call_t;

static blocking_resolver_state_t blocking_resolver = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static atomic_uint_fast64_t fake_clock_ms;
static atomic_uint fallback_calls;
static atomic_uint resolver_calls;

static uint64_t fake_clock(void) {
    return atomic_load_explicit(&fake_clock_ms, memory_order_relaxed);
}

static void capture_log(const char *message) {
    pthread_mutex_lock(&captured_log_mutex);
    snprintf(captured_log, sizeof(captured_log), "%s", message);
    pthread_mutex_unlock(&captured_log_mutex);
}

static void write_u16(unsigned char *output, uint16_t value) {
    output[0] = (unsigned char)(value >> 8);
    output[1] = (unsigned char)value;
}

static void *fake_stun_run(void *data) {
    fake_stun_server_t *server = data;
    unsigned char request[64];
    struct sockaddr_storage client;
    socklen_t client_length = sizeof(client);
    ssize_t received = recvfrom(server->handle,
                                request,
                                sizeof(request),
                                0,
                                (struct sockaddr *)&client,
                                &client_length);
    TEST_CHECK(received == 20);
    TEST_CHECK(client.ss_family == AF_INET);
    server->request_port = ntohs(((struct sockaddr_in *)&client)->sin_port);
    if (server->mode == FAKE_STUN_TIMEOUT) {
        return NULL;
    }

    unsigned char response[32] = {0x01, 0x01};
    response[4] = 0x21;
    response[5] = 0x12;
    response[6] = 0xa4;
    response[7] = 0x42;
    memcpy(response + 8, request + 8, 12);
    size_t response_size = 20;
    if (server->mode == FAKE_STUN_MALFORMED) {
        response[8] ^= 0xff;
    } else if (server->mode != FAKE_STUN_MISSING_ADDRESS) {
        write_u16(response + 2, 12);
        write_u16(response + 20, 0x0020);
        write_u16(response + 22, 8);
        response[25] = server->mode == FAKE_STUN_BAD_FAMILY       ? 3
                       : server->mode == FAKE_STUN_TRUNCATED_IPV6 ? 2
                                                                  : 1;
        write_u16(response + 26, (uint16_t)(45678U ^ 0x2112U));
        struct in_addr mapped;
        TEST_CHECK(inet_pton(AF_INET,
                             server->mode == FAKE_STUN_UNSPECIFIED ? "0.0.0.0" : "198.51.100.7",
                             &mapped) == 1);
        const unsigned char *address = (const unsigned char *)&mapped.s_addr;
        static const unsigned char mask[4] = {0x21, 0x12, 0xa4, 0x42};
        for (size_t i = 0; i < 4; i++) {
            response[28 + i] = address[i] ^ mask[i];
        }
        if (server->mode == FAKE_STUN_DECLARED_OVERRUN) {
            write_u16(response + 2, 4);
        }
        response_size = sizeof(response);
    }
    if (server->mode == FAKE_STUN_LATE_THEN_PUNCH) {
        pthread_mutex_lock(&server->mutex);
        while (!server->released) {
            pthread_cond_wait(&server->condition, &server->mutex);
        }
        pthread_mutex_unlock(&server->mutex);
    }
    TEST_CHECK(sendto(server->handle,
                      response,
                      response_size,
                      0,
                      (const struct sockaddr *)&client,
                      client_length) == (ssize_t)response_size);
    if (server->mode == FAKE_STUN_LATE_THEN_PUNCH) {
        static const char punch[] = "ATRINIK-PUNCH-1";
        TEST_CHECK(sendto(server->handle,
                          punch,
                          sizeof(punch) - 1U,
                          0,
                          (const struct sockaddr *)&client,
                          client_length) == (ssize_t)(sizeof(punch) - 1U));
        pthread_mutex_lock(&server->mutex);
        server->sent = true;
        pthread_cond_broadcast(&server->condition);
        pthread_mutex_unlock(&server->mutex);
    }
    if (server->expect_punch) {
        received = recvfrom(server->handle,
                            request,
                            sizeof(request),
                            0,
                            (struct sockaddr *)&client,
                            &client_length);
        TEST_CHECK(received == (ssize_t)strlen("ATRINIK-PUNCH-1"));
        TEST_CHECK(memcmp(request, "ATRINIK-PUNCH-1", (size_t)received) == 0);
        TEST_CHECK(client.ss_family == AF_INET);
        server->punch_port = ntohs(((struct sockaddr_in *)&client)->sin_port);
    }
    return NULL;
}

static int blocking_resolver_call(const char *host,
                                  const char *service,
                                  const struct addrinfo *hints,
                                  struct addrinfo **addresses) {
    pthread_mutex_lock(&blocking_resolver.mutex);
    unsigned int ticket = ++blocking_resolver.entered;
    pthread_cond_broadcast(&blocking_resolver.condition);
    while (blocking_resolver.released < ticket) {
        pthread_cond_wait(&blocking_resolver.condition, &blocking_resolver.mutex);
    }
    blocking_resolver.exited++;
    pthread_cond_broadcast(&blocking_resolver.condition);
    pthread_mutex_unlock(&blocking_resolver.mutex);
    *addresses = NULL;
    return EAI_AGAIN;
}

static int failing_resolver(const char *host,
                            const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **addresses) {
    *addresses = NULL;
    return EAI_NONAME;
}

static int numeric_resolver(const char *host,
                            const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **addresses) {
    atomic_fetch_add_explicit(&resolver_calls, 1U, memory_order_relaxed);
    return getaddrinfo("127.0.0.1", service, hints, addresses);
}

static void expire_after_send(void) {
    atomic_store_explicit(&fake_clock_ms, 2U, memory_order_relaxed);
}

static void *discovery_call_run(void *data) {
    discovery_call_t *call = data;
    char host[65];
    uint16_t port;
    call->result = socket_stun_discover_until(&call->client,
                                              call->endpoint,
                                              VS(host),
                                              &port,
                                              call->deadline_ms);
    return NULL;
}

static discovery_call_t discovery_call_create(const char *endpoint, uint64_t deadline_ms) {
    discovery_call_t call = {
        .client = {.handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)},
        .endpoint = endpoint,
        .deadline_ms = deadline_ms,
    };
    TEST_CHECK(call.client.handle >= 0);
    ((struct sockaddr_in *)&call.client.addr)->sin_family = AF_INET;
    return call;
}

static void blocking_resolver_reset(void) {
    pthread_mutex_lock(&blocking_resolver.mutex);
    blocking_resolver.entered = 0;
    blocking_resolver.released = 0;
    blocking_resolver.exited = 0;
    pthread_mutex_unlock(&blocking_resolver.mutex);
}

static void blocking_resolver_wait(unsigned int entered, unsigned int exited) {
    pthread_mutex_lock(&blocking_resolver.mutex);
    while (blocking_resolver.entered < entered || blocking_resolver.exited < exited) {
        pthread_cond_wait(&blocking_resolver.condition, &blocking_resolver.mutex);
    }
    pthread_mutex_unlock(&blocking_resolver.mutex);
}

static void blocking_resolver_release(unsigned int count) {
    pthread_mutex_lock(&blocking_resolver.mutex);
    blocking_resolver.released = count;
    pthread_cond_broadcast(&blocking_resolver.condition);
    pthread_mutex_unlock(&blocking_resolver.mutex);
}

static void blocking_resolver_require_exited(unsigned int count) {
    pthread_mutex_lock(&blocking_resolver.mutex);
    TEST_CHECK(blocking_resolver.exited == count);
    pthread_mutex_unlock(&blocking_resolver.mutex);
}

static void test_resolver_deadline_and_privacy(void) {
    socket_stun_resolver_set_for_test(blocking_resolver_call);
    socket_stun_clock_set_for_test(fake_clock);
    atomic_store_explicit(&fake_clock_ms, 1U, memory_order_relaxed);
    blocking_resolver_reset();
    captured_log[0] = '\0';
    discovery_call_t call = discovery_call_create("private-resolver.example:3478", 2U);
    pthread_t thread;
    TEST_CHECK(pthread_create(&thread, NULL, discovery_call_run, &call) == 0);
    blocking_resolver_wait(1, 0);
    atomic_store_explicit(&fake_clock_ms, 2U, memory_order_relaxed);
    TEST_CHECK(pthread_join(thread, NULL) == 0);
    blocking_resolver_require_exited(0);
    TEST_CHECK(!call.result);
    TEST_CHECK(strstr(captured_log, "resolution timed out") != NULL);
    TEST_CHECK(strstr(captured_log, "private-resolver") == NULL);
    test_close_socket(call.client.handle);

    socket_stun_resolver_set_for_test(failing_resolver);
    socket_stun_clock_set_for_test(NULL);
    captured_log[0] = '\0';
    call = discovery_call_create("private-override.example:3478", datetime_monotonic_ms() + 1000U);
    char host[65];
    uint16_t port;
    TEST_CHECK(!socket_stun_discover_until(&call.client,
                                           "private-override.example:3478",
                                           VS(host),
                                           &port,
                                           datetime_monotonic_ms() + 1000U));
    TEST_CHECK(strstr(captured_log, "Cannot resolve") != NULL);
    TEST_CHECK(strstr(captured_log, "private-override") == NULL);
    test_close_socket(call.client.handle);
    blocking_resolver_release(1);
    blocking_resolver_wait(1, 1);
    socket_stun_resolver_wait_for_test();

    socket_stun_resolver_set_for_test(blocking_resolver_call);
    socket_stun_clock_set_for_test(fake_clock);
    blocking_resolver_reset();
    atomic_store_explicit(&fake_clock_ms, 1U, memory_order_relaxed);
    discovery_call_t concurrent[4] = {
        discovery_call_create("concurrent-one.example:3478", 2U),
        discovery_call_create("concurrent-two.example:3478", 2U),
        discovery_call_create("concurrent-three.example:3478", 2U),
        discovery_call_create("concurrent-four.example:3478", 2U),
    };
    pthread_t concurrent_threads[arraysize(concurrent)];
    for (size_t i = 0; i < arraysize(concurrent); i++) {
        TEST_CHECK(
            pthread_create(&concurrent_threads[i], NULL, discovery_call_run, &concurrent[i]) == 0);
    }
    blocking_resolver_wait(arraysize(concurrent), 0);
    discovery_call_t overflow = discovery_call_create("bounded-overflow.example:3478", 2U);
    discovery_call_run(&overflow);
    TEST_CHECK(!overflow.result);
    test_close_socket(overflow.client.handle);
    atomic_store_explicit(&fake_clock_ms, 2U, memory_order_relaxed);
    for (size_t i = 0; i < arraysize(concurrent); i++) {
        TEST_CHECK(pthread_join(concurrent_threads[i], NULL) == 0);
        TEST_CHECK(!concurrent[i].result);
        test_close_socket(concurrent[i].client.handle);
    }
    blocking_resolver_require_exited(0);
    blocking_resolver_release(arraysize(concurrent));
    blocking_resolver_wait(arraysize(concurrent), arraysize(concurrent));
    socket_stun_resolver_wait_for_test();

    socket_stun_resolver_set_for_test(NULL);
    socket_stun_clock_set_for_test(NULL);
}

static void test_request_deadline_diagnostic(void) {
    socket_stun_resolver_set_for_test(numeric_resolver);
    socket_stun_clock_set_for_test(fake_clock);
    socket_stun_after_send_set_for_test(expire_after_send);
    atomic_store_explicit(&fake_clock_ms, 1U, memory_order_relaxed);
    atomic_store_explicit(&resolver_calls, 0U, memory_order_relaxed);
    captured_log[0] = '\0';
    discovery_call_t call = discovery_call_create("post-send-private.example:9", 2U);
    char host[65];
    uint16_t port;
    TEST_CHECK(!socket_stun_discover_until(&call.client,
                                           call.endpoint,
                                           VS(host),
                                           &port,
                                           call.deadline_ms));
    TEST_CHECK(atomic_load_explicit(&resolver_calls, memory_order_relaxed) == 1U);
    TEST_CHECK(strstr(captured_log, "STUN request timed out") != NULL);
    TEST_CHECK(strstr(captured_log, "post-send-private") == NULL);
    test_close_socket(call.client.handle);
    socket_stun_after_send_set_for_test(NULL);
    socket_stun_clock_set_for_test(NULL);
    socket_stun_resolver_set_for_test(NULL);
}

static void test_late_stun_response_before_punch(void) {
    int server_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int client_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    TEST_CHECK(server_handle >= 0 && client_handle >= 0);
    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    TEST_CHECK(
        bind(server_handle, (const struct sockaddr *)&server_address, sizeof(server_address)) == 0);
    socklen_t server_length = sizeof(server_address);
    TEST_CHECK(getsockname(server_handle, (struct sockaddr *)&server_address, &server_length) == 0);

    fake_stun_server_t server = {
        .handle = server_handle,
        .mode = FAKE_STUN_LATE_THEN_PUNCH,
    };
    TEST_CHECK(pthread_mutex_init(&server.mutex, NULL) == 0);
    TEST_CHECK(pthread_cond_init(&server.condition, NULL) == 0);
    pthread_t thread;
    TEST_CHECK(pthread_create(&thread, NULL, fake_stun_run, &server) == 0);

    socket_t client = {.handle = client_handle};
    ((struct sockaddr_in *)&client.addr)->sin_family = AF_INET;
    char endpoint[32];
    snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u", ntohs(server_address.sin_port));
    socket_stun_clock_set_for_test(fake_clock);
    socket_stun_after_send_set_for_test(expire_after_send);
    atomic_store_explicit(&fake_clock_ms, 1U, memory_order_relaxed);
    captured_log[0] = '\0';
    char host[65];
    uint16_t port;
    TEST_CHECK(!socket_stun_discover_until(&client, endpoint, VS(host), &port, 2U));
    TEST_CHECK(strstr(captured_log, "STUN request timed out") != NULL);

    pthread_mutex_lock(&server.mutex);
    server.released = true;
    pthread_cond_broadcast(&server.condition);
    while (!server.sent) {
        pthread_cond_wait(&server.condition, &server.mutex);
    }
    pthread_mutex_unlock(&server.mutex);
    TEST_CHECK(socket_udp_punch_receive(&client, VS(host), &port));
    TEST_CHECK(strcmp(host, "127.0.0.1") == 0);
    TEST_CHECK(port == ntohs(server_address.sin_port));

    TEST_CHECK(pthread_join(thread, NULL) == 0);
    TEST_CHECK(pthread_cond_destroy(&server.condition) == 0);
    TEST_CHECK(pthread_mutex_destroy(&server.mutex) == 0);
    socket_stun_after_send_set_for_test(NULL);
    socket_stun_clock_set_for_test(NULL);
    test_close_socket(client_handle);
    test_close_socket(server_handle);
}

static void *rendezvous_call_run(void *data) {
    rendezvous_call_t *call = data;
    socket_direct_candidate_t candidates[SOCKET_DIRECT_MAX_CANDIDATES + 1U];
    call->count = socket_rendezvous_client(call->client,
                                           "ws://127.0.0.1:1",
                                           "fallback-test.example:3478",
                                           call->attempt,
                                           candidates,
                                           arraysize(candidates),
                                           &call->failure);
    return NULL;
}

static bool fallback_spy(socket_t *client,
                         bool directory_probe_allowed,
                         char *host,
                         size_t host_size,
                         uint16_t *port) {
    atomic_fetch_add_explicit(&fallback_calls, 1U, memory_order_relaxed);
    return false;
}

static void test_rendezvous_reserves_fallback_budget(void) {
    uint64_t now_ms = datetime_monotonic_ms();
    TEST_CHECK(socket_rendezvous_stun_deadline(now_ms, now_ms + 15000U) == now_ms + 3000U);
    TEST_CHECK(socket_rendezvous_stun_deadline(now_ms, now_ms + 6000U) == now_ms + 1000U);
    TEST_CHECK(socket_rendezvous_stun_deadline(now_ms, now_ms + 5000U) == now_ms);

    socket_stun_resolver_set_for_test(blocking_resolver_call);
    socket_stun_clock_set_for_test(fake_clock);
    socket_rendezvous_fallback_set_for_test(fallback_spy);
    atomic_store_explicit(&fake_clock_ms, 1U, memory_order_relaxed);
    atomic_store_explicit(&fallback_calls, 0U, memory_order_relaxed);
    blocking_resolver_reset();
    discovery_call_t call = discovery_call_create("unused", now_ms);

    char server_id[RENDEZVOUS_SERVER_ID_HEX_SIZE + 1U];
    char ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    memset(server_id, 'a', sizeof(server_id) - 1U);
    memset(ticket, 'b', sizeof(ticket) - 1U);
    server_id[sizeof(server_id) - 1U] = '\0';
    ticket[sizeof(ticket) - 1U] = '\0';
    uint64_t deadline_ms = datetime_monotonic_ms() + 15000U;
    socket_rendezvous_attempt_t *attempt =
        socket_rendezvous_attempt_create(server_id, ticket, NULL, deadline_ms);
    TEST_CHECK(attempt != NULL);
    captured_log[0] = '\0';
    rendezvous_call_t rendezvous_call = {.client = &call.client, .attempt = attempt};
    pthread_t rendezvous_thread;
    TEST_CHECK(pthread_create(&rendezvous_thread, NULL, rendezvous_call_run, &rendezvous_call) ==
               0);
    blocking_resolver_wait(1, 0);
    atomic_store_explicit(&fake_clock_ms, UINT64_MAX, memory_order_relaxed);
    TEST_CHECK(pthread_join(rendezvous_thread, NULL) == 0);
    TEST_CHECK(rendezvous_call.count == 0);
    blocking_resolver_require_exited(0);
    TEST_CHECK(atomic_load_explicit(&fallback_calls, memory_order_relaxed) == 1U);
    TEST_CHECK(rendezvous_call.failure.code != SOCKET_CONNECT_FAILURE_TIMEOUT);
    TEST_CHECK(strstr(captured_log, "Cannot determine a local rendezvous candidate") != NULL);
    TEST_CHECK(datetime_monotonic_ms() < deadline_ms);
    blocking_resolver_release(1);
    blocking_resolver_wait(1, 1);
    socket_stun_resolver_wait_for_test();
    socket_rendezvous_attempt_destroy(attempt);

    socket_stun_resolver_set_for_test(numeric_resolver);
    socket_stun_clock_set_for_test(NULL);
    atomic_store_explicit(&resolver_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&fallback_calls, 0U, memory_order_relaxed);
    attempt =
        socket_rendezvous_attempt_create(server_id, ticket, NULL, datetime_monotonic_ms() + 15000U);
    TEST_CHECK(attempt != NULL);
    socket_direct_candidate_t candidates[SOCKET_DIRECT_MAX_CANDIDATES + 1U];
    socket_connect_failure_t failure = {0};
    TEST_CHECK(socket_rendezvous_client(&call.client,
                                        "ws://unused.invalid",
                                        NULL,
                                        attempt,
                                        candidates,
                                        arraysize(candidates),
                                        &failure) == 0);
    TEST_CHECK(atomic_load_explicit(&resolver_calls, memory_order_relaxed) == 0U);
    TEST_CHECK(atomic_load_explicit(&fallback_calls, memory_order_relaxed) == 1U);
    TEST_CHECK(failure.code == SOCKET_CONNECT_FAILURE_UNAVAILABLE);
    socket_rendezvous_attempt_destroy(attempt);

    socket_stun_resolver_set_for_test(NULL);
    socket_stun_clock_set_for_test(NULL);
    socket_rendezvous_fallback_set_for_test(NULL);
    test_close_socket(call.client.handle);
}

static void test_mode(fake_stun_mode_t mode, bool expected, const char *diagnostic) {
    int server_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int client_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    TEST_CHECK(server_handle >= 0 && client_handle >= 0);

    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    TEST_CHECK(
        bind(server_handle, (const struct sockaddr *)&server_address, sizeof(server_address)) == 0);
    socklen_t server_length = sizeof(server_address);
    TEST_CHECK(getsockname(server_handle, (struct sockaddr *)&server_address, &server_length) == 0);

    fake_stun_server_t server = {
        .handle = server_handle,
        .mode = mode,
        .expect_punch = mode == FAKE_STUN_SUCCESS,
    };
    pthread_t thread;
    TEST_CHECK(pthread_create(&thread, NULL, fake_stun_run, &server) == 0);

    socket_t client = {.handle = client_handle};
    ((struct sockaddr_in *)&client.addr)->sin_family = AF_INET;
    char endpoint[32];
    snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u", ntohs(server_address.sin_port));
    char host[65] = "unchanged";
    uint16_t port = 1;
    captured_log[0] = '\0';
    uint64_t started_ms = datetime_monotonic_ms();
    bool discovered = socket_stun_discover(&client, endpoint, VS(host), &port);
    uint64_t elapsed_ms = datetime_monotonic_ms() - started_ms;

    if (discovered && server.expect_punch) {
        TEST_CHECK(socket_udp_punch(&client, "127.0.0.1", ntohs(server_address.sin_port)));
    }

    TEST_CHECK(pthread_join(thread, NULL) == 0);
    TEST_CHECK(discovered == expected);
    if (expected) {
        TEST_CHECK(strcmp(host, "198.51.100.7") == 0);
        TEST_CHECK(port == 45678);
        TEST_CHECK(server.request_port != 0);
        TEST_CHECK(server.punch_port == server.request_port);
    } else {
        TEST_CHECK(strstr(captured_log, diagnostic) != NULL);
    }
    if (mode == FAKE_STUN_TIMEOUT) {
        TEST_CHECK(elapsed_ms < 5000U);
    }
    test_close_socket(client_handle);
    test_close_socket(server_handle);
}

int main(void) {
    toolkit_import(logger);
    toolkit_import(socket);
    logger_set_print_func(capture_log);

    test_mode(FAKE_STUN_SUCCESS, true, "");
    test_mode(FAKE_STUN_MALFORMED, false, "Invalid STUN response");
    test_mode(FAKE_STUN_MISSING_ADDRESS, false, "XOR-MAPPED-ADDRESS");
    test_mode(FAKE_STUN_UNSPECIFIED, false, "unusable mapped address");
    test_mode(FAKE_STUN_DECLARED_OVERRUN, false, "Invalid STUN response");
    test_mode(FAKE_STUN_BAD_FAMILY, false, "Invalid STUN response");
    test_mode(FAKE_STUN_TRUNCATED_IPV6, false, "Invalid STUN response");
    test_mode(FAKE_STUN_TIMEOUT, false, "timed out");
    test_resolver_deadline_and_privacy();
    test_request_deadline_diagnostic();
    test_late_stun_response_before_punch();
    test_rendezvous_reserves_fallback_budget();

    char host[65];
    uint16_t port;
    socket_t client = {.handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    TEST_CHECK(client.handle >= 0);
    ((struct sockaddr_in *)&client.addr)->sin_family = AF_INET;
    captured_log[0] = '\0';
    TEST_CHECK(!socket_stun_discover(&client, "127.0.0.1:no", VS(host), &port));
    TEST_CHECK(strstr(captured_log, "Cannot resolve") != NULL);
    test_close_socket(client.handle);

    logger_set_print_func(logger_do_print);
    toolkit_deinit();
    return 0;
}
