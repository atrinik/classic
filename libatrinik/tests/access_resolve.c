/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <toolkit/access_resolve.h>
#include <toolkit/curl.h>
#include <toolkit/metaserver_url.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#define REQUIRE(x)                                  \
    do {                                            \
        if (!(x)) {                                 \
            fprintf(stderr, "line %d\n", __LINE__); \
            return 1;                               \
        }                                           \
    } while (0)
static const char body[] =
    "{\"schema\":\"atrinik-access-resolved-v1\",\"profile\":\"classic\",\"serverId\":"
    "\"51f341b3c529d7186d2981afd2d8519c6c15eaced3f0291acdfb408f5cba746b\",\"certificate\":"
    "\"MIIBRzCB76ADAgECAgEBMAoGCCqGSM49BAMCMCQxIjAgBgNVBAMTGUF0cmluaWsgcHVibGlzaGVyIGZpeHR1cmUwHhcN"
    "MjYwMTAxMDAwMDAwWhcNMzYwMTAxMDAwMDAwWjAkMSIwIAYDVQQDExlBdHJpbmlrIHB1Ymxpc2hlciBmaXh0dXJlMFkwEw"
    "YHKoZIzj0CAQYIKoZIzj0DAQcDQgAEPQd9J89i9RHBKxJbWY3LVTOACf0pjFZgJTnCMy6BlQ27hfl0xwZf5LkuREpxVbe0"
    "xh6ydj5V00OY5Sh5myB4pqMSMBAwDgYDVR0PAQH/"
    "BAQDAgeAMAoGCCqGSM49BAMCA0cAMEQCIAbaCO4YENW0H67c1Gj292fvPwTwvwsQ8F5ObWMOv0wyAiArIt0mJkn917HvWQ"
    "w8HrBVOCyXfGfocde6yjbjwSUcEA==\",\"name\":\"Fixture\",\"accessRequired\":true,\"generation\":"
    "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"clientNonce\":"
    "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"grant\":"
    "\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\",\"expiresAt\":"
    "\"1800000015\",\"endpoint\":{\"hostname\":\"game.example.net\",\"port\":13327}}";
static bool cancellation_requested(void *context) {
    return atomic_load((atomic_bool *)context);
}

static bool all_zero(const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Keep the real certificate, server identity, nonce and routing grant while
 * changing only the canonical descriptor name. Rejection must erase all of the
 * caller's prior result, including fields populated before name validation. */
static int test_descriptor_names(const char nonce[65]) {
    static const struct {
        const char *encoded;
        const char *expected;
    } cases[] = {
        {"Caf\xc3\xa9", "Caf\xc3\xa9"},
        {"Cost \xe2\x82\xac", "Cost \xe2\x82\xac"},
        {"Face \xf0\x9f\x98\x80", "Face \xf0\x9f\x98\x80"},
        {"Quote\\\" and slash\\\\", "Quote\" and slash\\"},
        {"", NULL},
        {"\x80", NULL},
        {"\xc2", NULL},
        {"\xc2" "A", NULL},
        {"\xc0\xaf", NULL},
        {"\xe0\x80\xaf", NULL},
        {"\xed\xa0\x80", NULL},
        {"\xf4\x90\x80\x80", NULL},
        {"\xef\xb7\x90", NULL},
        {"\xef\xbf\xbe", NULL},
        {"\xf4\x8f\xbf\xbe", NULL},
        {"\xc2\x85", NULL},
        {"Control\x1f", NULL},
        {"Control\x7f", NULL},
        {"Bad\\n", NULL},
        {"\\u0061", NULL},
    };
    const char *name = strstr(body, "\"name\":\"Fixture\"");
    REQUIRE(name != NULL);
    name += strlen("\"name\":\"");
    size_t prefix = (size_t)(name - body);
    const char *suffix = name + strlen("Fixture");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char changed[sizeof(body) + 128];
        int size = snprintf(changed,
                            sizeof(changed),
                            "%.*s%s%s",
                            (int)prefix,
                            body,
                            cases[i].encoded,
                            suffix);
        REQUIRE(size > 0 && (size_t)size < sizeof(changed));
        access_resolved_t out;
        memset(&out, 0xa5, sizeof(out));
        bool resolved = access_resolve_parse(changed, (size_t)size, nonce, 1800000000, &out);
        if (cases[i].expected != NULL) {
            REQUIRE(resolved && strcmp(out.name, cases[i].expected) == 0);
            REQUIRE(strcmp(out.grant.client_nonce, nonce) == 0 && out.port == 13327);
            access_resolved_clear(&out);
            REQUIRE(all_zero(&out, sizeof(out)));
        } else {
            REQUIRE(!resolved && all_zero(&out, sizeof(out)));
        }
    }
    return 0;
}

static int test_precancelled_resolve(void) {
    atomic_bool cancelled = true;
    curl_cancel_t cancel = {.cancelled = cancellation_requested, .context = &cancelled};
    access_resolved_t out;
    const char *origin = "https://127.0.0.1:9";
#ifndef _WIN32
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    bool listening = bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
                     listen(listener, 1) == 0;
    socklen_t length = sizeof(address);
    listening = listening && getsockname(listener, (struct sockaddr *)&address, &length) == 0;
    if (!listening) {
        close(listener);
        return 1;
    }
    char loopback[128];
    snprintf(loopback, sizeof(loopback), "http://127.0.0.1:%u", ntohs(address.sin_port));
    origin = loopback;
#endif
    memset(&out, 0xa5, sizeof(out));
    bool resolved = access_resolve_cancellable(origin, "000G40R40M30E209", &out, &cancel);
    bool cleared = all_zero(&out, sizeof(out));
#ifndef _WIN32
    struct pollfd connection = {.fd = listener, .events = POLLIN};
    bool no_connection = poll(&connection, 1, 100) == 0;
    close(listener);
    REQUIRE(no_connection);
#endif
    REQUIRE(!resolved && cleared);
    return 0;
}

int main(void) {
    access_resolved_t out;
    char nonce[65];
    memset(nonce, 'c', 64);
    nonce[64] = 0;
    REQUIRE(access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000000, &out));
    REQUIRE(strcmp(out.name, "Fixture") == 0 && out.port == 13327);
    REQUIRE(test_descriptor_names(nonce) == 0);
    for (size_t n = 0; n < sizeof(body) - 1; n++) {
        memset(&out, 0xa5, sizeof(out));
        REQUIRE(!access_resolve_parse(body, n, nonce, 1800000000, &out));
        REQUIRE(out.server_id[0] == 0 && out.grant.grant[0] == 0);
    }
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000015, &out));
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1799999999, &out));
    nonce[0] = 'a';
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000000, &out));
    nonce[0] = 'c';
    char changed[sizeof(body) + 5];
    memcpy(changed, body, sizeof(body));
    char *identity = strstr(changed, "\"serverId\":\"");
    identity[12] = identity[12] == 'a' ? 'b' : 'a';
    REQUIRE(!access_resolve_parse(changed, strlen(changed), nonce, 1800000000, &out));
    memcpy(changed, body, sizeof(body));
    changed[sizeof(body) - 1] = ' ';
    changed[sizeof(body)] = 0;
    REQUIRE(!access_resolve_parse(changed, strlen(changed), nonce, 1800000000, &out));
    char url[1024];
    REQUIRE(metaserver_url_access("https://rendezvous.meta.atrinik.org",
                                  NULL,
                                  false,
                                  url,
                                  sizeof(url)));
    REQUIRE(strcmp(url, "https://rendezvous.meta.atrinik.org/v1/access/resolve") == 0);
    REQUIRE(!metaserver_url_access("http://example.net", NULL, false, url, sizeof(url)));
    REQUIRE(!metaserver_url_access("https://example.net/?secret=x", NULL, false, url, sizeof(url)));
    REQUIRE(!metaserver_url_access("https://example.net/base", NULL, false, url, sizeof(url)));
    REQUIRE(metaserver_url_access("http://127.0.0.1:8787", NULL, false, url, sizeof(url)));
    REQUIRE(test_precancelled_resolve() == 0);
    return 0;
}
