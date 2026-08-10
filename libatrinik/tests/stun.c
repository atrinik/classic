#include "../socket_private.h"

#include <toolkit/datetime.h>
#include <toolkit/logger.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    FAKE_STUN_TIMEOUT,
} fake_stun_mode_t;

typedef struct fake_stun_server {
    int handle;
    fake_stun_mode_t mode;
} fake_stun_server_t;

static char captured_log[512];

static void capture_log(const char *message) {
    snprintf(captured_log, sizeof(captured_log), "%s", message);
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
    TEST_CHECK(sendto(server->handle,
                      response,
                      response_size,
                      0,
                      (const struct sockaddr *)&client,
                      client_length) == (ssize_t)response_size);
    return NULL;
}

static int stalled_resolver(const char *host,
                            const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **addresses) {
    usleep(200000);
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

static void test_resolver_deadline_and_privacy(void) {
    socket_t client = {.handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    TEST_CHECK(client.handle >= 0);
    ((struct sockaddr_in *)&client.addr)->sin_family = AF_INET;
    char host[65];
    uint16_t port;

    socket_stun_resolver_set_for_test(stalled_resolver);
    captured_log[0] = '\0';
    uint64_t started_ms = datetime_monotonic_ms();
    TEST_CHECK(!socket_stun_discover_until(&client,
                                           "private-resolver.example:3478",
                                           VS(host),
                                           &port,
                                           started_ms + 50U));
    uint64_t elapsed_ms = datetime_monotonic_ms() - started_ms;
    TEST_CHECK(elapsed_ms < 150U);
    TEST_CHECK(strstr(captured_log, "resolution timed out") != NULL);
    TEST_CHECK(strstr(captured_log, "private-resolver") == NULL);

    captured_log[0] = '\0';
    started_ms = datetime_monotonic_ms();
    TEST_CHECK(!socket_stun_discover_until(&client,
                                           "second-private.example:3478",
                                           VS(host),
                                           &port,
                                           started_ms + 1000U));
    TEST_CHECK(datetime_monotonic_ms() - started_ms < 100U);
    TEST_CHECK(strstr(captured_log, "Cannot resolve") != NULL);
    TEST_CHECK(strstr(captured_log, "second-private") == NULL);
    usleep(250000);

    socket_stun_resolver_set_for_test(failing_resolver);
    captured_log[0] = '\0';
    TEST_CHECK(!socket_stun_discover_until(&client,
                                           "private-override.example:3478",
                                           VS(host),
                                           &port,
                                           datetime_monotonic_ms() + 1000U));
    TEST_CHECK(strstr(captured_log, "Cannot resolve") != NULL);
    TEST_CHECK(strstr(captured_log, "private-override") == NULL);

    socket_stun_resolver_set_for_test(NULL);
    close(client.handle);
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

    fake_stun_server_t server = {.handle = server_handle, .mode = mode};
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

    TEST_CHECK(pthread_join(thread, NULL) == 0);
    TEST_CHECK(discovered == expected);
    if (expected) {
        TEST_CHECK(strcmp(host, "198.51.100.7") == 0);
        TEST_CHECK(port == 45678);
    } else {
        TEST_CHECK(strstr(captured_log, diagnostic) != NULL);
    }
    if (mode == FAKE_STUN_TIMEOUT) {
        TEST_CHECK(elapsed_ms < 5000U);
    }
    close(client_handle);
    close(server_handle);
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

    char host[65];
    uint16_t port;
    socket_t client = {.handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    TEST_CHECK(client.handle >= 0);
    ((struct sockaddr_in *)&client.addr)->sin_family = AF_INET;
    captured_log[0] = '\0';
    TEST_CHECK(!socket_stun_discover(&client, "127.0.0.1:no", VS(host), &port));
    TEST_CHECK(strstr(captured_log, "Cannot resolve") != NULL);
    close(client.handle);

    logger_set_print_func(logger_do_print);
    toolkit_deinit();
    return 0;
}
