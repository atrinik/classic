/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <connection_failure.h>

#include <stdio.h>

bool client_connection_failure_format(const socket_connect_failure_t *failure,
                                      char *message,
                                      size_t message_size) {
    if (failure == NULL || message == NULL || message_size == 0) {
        return false;
    }

    int written;
    switch (failure->code) {
        case SOCKET_CONNECT_FAILURE_AUTHORIZATION:
            written = snprintf(message,
                               message_size,
                               "Connection failed: the server invite was rejected.");
            break;

        case SOCKET_CONNECT_FAILURE_INVITE_EXPIRED:
            written = snprintf(message,
                               message_size,
                               "Connection failed: the server invite has expired.");
            break;

        case SOCKET_CONNECT_FAILURE_RATE_LIMITED:
            if (failure->retry_after_seconds != 0) {
                written = snprintf(message,
                                   message_size,
                                   "Too many connection attempts; try again in %u seconds.",
                                   failure->retry_after_seconds);
            } else {
                written = snprintf(message,
                                   message_size,
                                   "Too many connection attempts; please try again later.");
            }
            break;

        case SOCKET_CONNECT_FAILURE_SERVER_OFFLINE:
            written = snprintf(message,
                               message_size,
                               "The server is offline or has no reachable direct route.");
            break;

        case SOCKET_CONNECT_FAILURE_TIMEOUT:
            written = snprintf(message, message_size, "The connection attempt timed out.");
            break;

        case SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION:
            written = snprintf(message,
                               message_size,
                               "The server uses an incompatible connection protocol.");
            break;

        case SOCKET_CONNECT_FAILURE_NONE:
        case SOCKET_CONNECT_FAILURE_UNAVAILABLE:
        default:
            written = snprintf(message, message_size, "Connection failed; please try again.");
            break;
    }
    return written >= 0 && (size_t)written < message_size;
}
