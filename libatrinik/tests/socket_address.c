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

static struct sockaddr first_socket_address;
static struct sockaddr_in selected_socket_address;
static struct addrinfo selected_socket_result;
static struct addrinfo first_socket_result;
static bool fake_socket_addresses_freed;

static int fake_socket_resolver(const char *host,
                                const char *service,
                                const struct addrinfo *hints,
                                struct addrinfo **addresses) {
    REQUIRE(strcmp(host, "selected.example") == 0);
    REQUIRE(strcmp(service, "13327") == 0);
    REQUIRE(hints->ai_family == AF_UNSPEC);
    REQUIRE(hints->ai_socktype == SOCK_STREAM);

    memset(&first_socket_address, 0, sizeof(first_socket_address));
    first_socket_result = (struct addrinfo){
        .ai_family = -1,
        .ai_socktype = SOCK_STREAM,
        .ai_addr = &first_socket_address,
        .ai_addrlen = 1,
        .ai_next = &selected_socket_result,
    };
    selected_socket_address = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = htons(13327),
    };
    REQUIRE(inet_pton(AF_INET, "127.0.0.1", &selected_socket_address.sin_addr) == 1);
    selected_socket_result = (struct addrinfo){
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_addr = (struct sockaddr *)&selected_socket_address,
        .ai_addrlen = sizeof(selected_socket_address),
    };
    *addresses = &first_socket_result;
    return 0;
}

static void fake_socket_addresses_free(struct addrinfo *addresses) {
    REQUIRE(addresses == &first_socket_result);
    fake_socket_addresses_freed = true;
}

static void test_socket_create_copies_selected_result_length(void) {
    fake_socket_addresses_freed = false;
    socket_create_resolver_set_for_test(fake_socket_resolver, fake_socket_addresses_free);
    socket_t *connection = socket_create("selected.example", 13327, SOCKET_ROLE_CLIENT, false);
    socket_create_resolver_set_for_test(NULL, NULL);

    REQUIRE(connection != NULL);
    REQUIRE(fake_socket_addresses_freed);
    REQUIRE(connection->addr.ss_family == AF_INET);
    REQUIRE(memcmp(&connection->addr, &selected_socket_address, sizeof(selected_socket_address)) ==
            0);
    require_zero_tail(&connection->addr, sizeof(selected_socket_address));
    socket_destroy(connection);
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
    test_socket_create_copies_selected_result_length();
    test_numeric_addresses();
    toolkit_deinit();
    return 0;
}
