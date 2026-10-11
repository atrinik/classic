/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../socket_fd_set.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

int main(void) {
    fd_set actual;
    FD_ZERO(&actual);
#ifdef WIN32
    /* Exercise the conversion only: no invalid socket reaches select(). */
    static const int handles[] = {0, 1, INT_MAX, -1, INT_MIN, -2};
    fd_set expected;
    FD_ZERO(&expected);
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        SOCKET implicit = handles[i];
        FD_SET(implicit, &expected);
        socket_fd_set(handles[i], &actual);
        socket_fd_set(handles[i], &actual);
        REQUIRE(actual.fd_count == expected.fd_count);
        REQUIRE(actual.fd_array[i] == implicit);
    }
    REQUIRE(actual.fd_array[3] == INVALID_SOCKET);
    REQUIRE(actual.fd_array[4] == (SOCKET)(UINT64_MAX - INT_MAX));
    REQUIRE(actual.fd_array[5] == (SOCKET)(UINT64_MAX - 1));
    REQUIRE(actual.fd_count == sizeof(handles) / sizeof(handles[0]));
#else
    static const int handles[] = {0, 1, FD_SETSIZE - 1};
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        socket_fd_set(handles[i], &actual);
        socket_fd_set(handles[i], &actual);
    }
    for (int fd = 0; fd < FD_SETSIZE; fd++) {
        REQUIRE(!!FD_ISSET(fd, &actual) == (fd == 0 || fd == 1 || fd == FD_SETSIZE - 1));
    }
#endif
    return 0;
}
