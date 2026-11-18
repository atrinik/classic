/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Prove, against the real linked libcurl, that the reply to an empty WebSocket
 * PING (the rendezvous control heartbeat) reaches socket_websocket_receive_ex
 * callers as a counted PONG. A peer that never surfaces PONGs would make the
 * server's pong deadline unusable, so this runs over loopback, not a fake.
 */

#include <toolkit/socket.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

#define TEST_SKIP 77
#define FIXTURE_TIMEOUT_MS 5000

#if LIBCURL_VERSION_NUM >= 0x075600
typedef struct websocket_fixture {
    int listener;
    bool ping_valid;
    bool completed;
} websocket_fixture_t;

static bool fixture_wait(int fd, short events) {
    struct pollfd descriptor = {.fd = fd, .events = events};
    return poll(&descriptor, 1, FIXTURE_TIMEOUT_MS) == 1 && (descriptor.revents & events) != 0;
}

static bool fixture_read(int fd, unsigned char *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        if (!fixture_wait(fd, POLLIN)) {
            return false;
        }
        ssize_t received = recv(fd, buffer + done, size - done, 0);
        if (received <= 0) {
            return false;
        }
        done += (size_t)received;
    }
    return true;
}

static bool fixture_write(int fd, const void *buffer, size_t size) {
    const unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < size) {
        if (!fixture_wait(fd, POLLOUT)) {
            return false;
        }
        ssize_t sent = send(fd, bytes + done, size - done, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        done += (size_t)sent;
    }
    return true;
}

static bool fixture_handshake(int client) {
    char request[4096] = {0};
    size_t used = 0;
    while (strstr(request, "\r\n\r\n") == NULL) {
        if (used == sizeof(request) - 1U || !fixture_wait(client, POLLIN)) {
            return false;
        }
        ssize_t received = recv(client, request + used, sizeof(request) - 1U - used, 0);
        if (received <= 0) {
            return false;
        }
        used += (size_t)received;
        request[used] = '\0';
    }

    static const char header[] = "\r\nSec-WebSocket-Key:";
    const char *key = NULL;
    for (const char *cursor = request; *cursor != '\0'; cursor++) {
        if (strncasecmp(cursor, header, sizeof(header) - 1U) == 0) {
            key = cursor + sizeof(header) - 1U;
            break;
        }
    }
    if (key == NULL) {
        return false;
    }
    key += strspn(key, " \t");
    size_t key_length = strcspn(key, "\r\n");
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char material[128];
    if (key_length == 0 || key_length + sizeof(guid) > sizeof(material)) {
        return false;
    }
    memcpy(material, key, key_length);
    memcpy(material + key_length, guid, sizeof(guid));

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    char accept[64];
    if (EVP_Digest(material,
                   key_length + sizeof(guid) - 1U,
                   digest,
                   &digest_size,
                   EVP_sha1(),
                   NULL) != 1 ||
        digest_size != 20U) {
        return false;
    }
    EVP_EncodeBlock((unsigned char *)accept, digest, (int)digest_size);

    char response[256];
    int length = snprintf(response,
                          sizeof(response),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
    return length > 0 && (size_t)length < sizeof(response) &&
           fixture_write(client, response, (size_t)length);
}

static void *fixture_run(void *user_data) {
    websocket_fixture_t *fixture = user_data;
    if (!fixture_wait(fixture->listener, POLLIN)) {
        return NULL;
    }
    int client = accept(fixture->listener, NULL, NULL);
    if (client < 0) {
        return NULL;
    }
    if (fixture_handshake(client)) {
        /* A client PING with no payload: FIN|PING, MASK|0, then the mask key. */
        unsigned char ping[6];
        fixture->ping_valid =
            fixture_read(client, ping, sizeof(ping)) && ping[0] == 0x89 && ping[1] == 0x80;
        /* The unmasked empty PONG a conforming peer answers with, then text. */
        static const unsigned char reply[] = {0x8a, 0x00, 0x81, 0x02, 'o', 'k'};
        if (fixture->ping_valid && fixture_write(client, reply, sizeof(reply))) {
            /* Hold the connection until the client has read the reply. */
            unsigned char ignored;
            (void)fixture_wait(client, POLLIN);
            (void)recv(client, &ignored, sizeof(ignored), 0);
            fixture->completed = true;
        }
    }
    close(client);
    return NULL;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static bool curl_supports_ws(void) {
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    for (const char *const *protocol = version->protocols; *protocol != NULL; protocol++) {
        if (strcmp(*protocol, "ws") == 0) {
            return true;
        }
    }
    return false;
}

static int test_empty_ping_reply_is_counted(void) {
    if (!curl_supports_ws()) {
        fprintf(stderr, "libcurl lacks WebSocket support; skipping\n");
        return TEST_SKIP;
    }

    websocket_fixture_t fixture = {.listener = socket(AF_INET, SOCK_STREAM, 0)};
    TEST_CHECK(fixture.listener >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    socklen_t address_size = sizeof(address);
    TEST_CHECK(bind(fixture.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    TEST_CHECK(listen(fixture.listener, 1) == 0);
    TEST_CHECK(getsockname(fixture.listener, (struct sockaddr *)&address, &address_size) == 0);
    char url[64];
    int url_length = snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", ntohs(address.sin_port));
    TEST_CHECK(url_length > 0 && (size_t)url_length < sizeof(url));

    pthread_t server;
    TEST_CHECK(pthread_create(&server, NULL, fixture_run, &fixture) == 0);

    CURL *curl = curl_easy_init();
    TEST_CHECK(curl != NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)FIXTURE_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    TEST_CHECK(curl_easy_perform(curl) == CURLE_OK);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    TEST_CHECK(http_code == 101);

    /* Exactly the rendezvous control heartbeat frame. */
    size_t sent = 1;
    TEST_CHECK(curl_ws_send(curl, "", 0, &sent, 0, CURLWS_PING) == CURLE_OK);
    TEST_CHECK(sent == 0);

    char message[16] = {0};
    size_t used = 0;
    uint32_t pongs = 0;
    socket_websocket_receive_state_t state = SOCKET_WEBSOCKET_EMPTY;
    uint64_t deadline = monotonic_ms() + FIXTURE_TIMEOUT_MS;
    while (state != SOCKET_WEBSOCKET_MESSAGE && monotonic_ms() < deadline) {
        socket_websocket_receive_info_t info;
        state = socket_websocket_receive_ex(curl, message, sizeof(message), &used, &info);
        pongs += info.pongs_received;
        TEST_CHECK(state == SOCKET_WEBSOCKET_EMPTY || state == SOCKET_WEBSOCKET_PARTIAL ||
                   state == SOCKET_WEBSOCKET_MESSAGE);
        if (state == SOCKET_WEBSOCKET_EMPTY) {
            usleep(1000);
        }
    }
    TEST_CHECK(state == SOCKET_WEBSOCKET_MESSAGE);
    TEST_CHECK(strcmp(message, "ok") == 0);
    TEST_CHECK(pongs == 1);

    /* Release the fixture's final read; libcurl may close without a frame. */
    curl_easy_cleanup(curl);
    TEST_CHECK(pthread_join(server, NULL) == 0);
    close(fixture.listener);
    TEST_CHECK(fixture.ping_valid);
    TEST_CHECK(fixture.completed);
    return 0;
}
#endif

int main(void) {
#if LIBCURL_VERSION_NUM >= 0x075600
    TEST_CHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    int result = test_empty_ping_reply_is_counted();
    curl_global_cleanup();
    return result;
#else
    return TEST_SKIP;
#endif
}
