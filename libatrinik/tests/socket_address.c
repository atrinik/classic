#include "../socket_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static void require_zero_tail(const struct sockaddr_storage *address, size_t used) {
    const unsigned char *bytes = (const unsigned char *)address;
    for (size_t i = used; i < sizeof(*address); i++) {
        REQUIRE(bytes[i] == 0);
    }
}

static void test_addrinfo_copy_ipv4(void) {
    struct sockaddr_in source = {
        .sin_family = AF_INET,
        .sin_port = htons(13327),
    };
    REQUIRE(inet_pton(AF_INET, "127.0.0.1", &source.sin_addr) == 1);

    struct addrinfo address = {
        .ai_addr = (struct sockaddr *)&source,
        .ai_addrlen = sizeof(source),
    };
    struct sockaddr_storage destination;
    memset(&destination, 0xa5, sizeof(destination));

    REQUIRE(socket_addrinfo_copy(&destination, &address));
    REQUIRE(memcmp(&destination, &source, sizeof(source)) == 0);
    require_zero_tail(&destination, sizeof(source));
}

#ifdef HAVE_IPV6
static void test_addrinfo_copy_ipv6(void) {
    struct sockaddr_in6 source = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(13327),
    };
    REQUIRE(inet_pton(AF_INET6, "::1", &source.sin6_addr) == 1);

    struct addrinfo address = {
        .ai_addr = (struct sockaddr *)&source,
        .ai_addrlen = sizeof(source),
    };
    struct sockaddr_storage destination;
    memset(&destination, 0xa5, sizeof(destination));

    REQUIRE(socket_addrinfo_copy(&destination, &address));
    REQUIRE(memcmp(&destination, &source, sizeof(source)) == 0);
    require_zero_tail(&destination, sizeof(source));
}
#endif

static void test_addrinfo_copy_rejects_invalid_lengths(void) {
    struct sockaddr_in source = {.sin_family = AF_INET};
    struct sockaddr_storage destination;
    struct sockaddr_storage original;
    memset(&destination, 0xa5, sizeof(destination));
    original = destination;

    struct addrinfo address = {
        .ai_addr = (struct sockaddr *)&source,
        .ai_addrlen = 0,
    };
    REQUIRE(!socket_addrinfo_copy(&destination, &address));
    REQUIRE(memcmp(&destination, &original, sizeof(destination)) == 0);

    address.ai_addrlen = sizeof(destination) + 1U;
    REQUIRE(!socket_addrinfo_copy(&destination, &address));
    REQUIRE(memcmp(&destination, &original, sizeof(destination)) == 0);

    address.ai_addr = NULL;
    address.ai_addrlen = sizeof(source);
    REQUIRE(!socket_addrinfo_copy(&destination, &address));
    REQUIRE(memcmp(&destination, &original, sizeof(destination)) == 0);
    REQUIRE(!socket_addrinfo_copy(NULL, &address));
    REQUIRE(!socket_addrinfo_copy(&destination, NULL));
}

static void test_numeric_address(const char *host, int family) {
    struct sockaddr_storage address;
    memset(&address, 0xa5, sizeof(address));
    REQUIRE(socket_host2addr(host, &address));
    REQUIRE(address.ss_family == family);

    char rendered[INET6_ADDRSTRLEN];
    REQUIRE(socket_addr2host(&address, rendered, sizeof(rendered)) != NULL);

    struct sockaddr_storage round_trip;
    memset(&round_trip, 0xa5, sizeof(round_trip));
    REQUIRE(socket_host2addr(rendered, &round_trip));
    REQUIRE(round_trip.ss_family == family);
    REQUIRE(socket_addr_cmp(&address, &round_trip, socket_addr_plen(&address)) == 0);
}

static void test_numeric_addresses(void) {
    test_numeric_address("127.0.0.1", AF_INET);

#ifdef HAVE_IPV6
    int handle = socket(AF_INET6, SOCK_STREAM, 0);
    if (handle != -1) {
#ifndef WIN32
        close(handle);
#else
        closesocket(handle);
#endif
        test_numeric_address("::1", AF_INET6);
    }
#endif
}

int main(void) {
    toolkit_import(socket);
    test_addrinfo_copy_ipv4();
#ifdef HAVE_IPV6
    test_addrinfo_copy_ipv6();
#endif
    test_addrinfo_copy_rejects_invalid_lengths();
    test_numeric_addresses();
    toolkit_deinit();
    return 0;
}
