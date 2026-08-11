/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
 ************************************************************************/

/**
 * @file
 * Private socket implementation declarations.
 */

#ifndef TOOLKIT_SOCKET_PRIVATE_H
#define TOOLKIT_SOCKET_PRIVATE_H

#include "socket.h"

#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
#include <openssl/ssl.h>

#endif

struct socket_stream {
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    SSL *ssl;
#endif
    struct sock_struct *connection;
    struct socket_stream *next;
    socket_stream_kind_t kind;
    uint8_t preface[SOCKET_STREAM_PREFACE_SIZE];
    size_t preface_pos;
    uint64_t created_ms;
    bool local;
};

struct sock_struct {
    enum {
        SOCKET_TRANSPORT_TCP,
        SOCKET_TRANSPORT_QUIC_LISTENER,
        SOCKET_TRANSPORT_QUIC_CONNECTION,
    } transport;

    int handle;
    struct sockaddr_storage addr;
    char *host;
    uint16_t port;
    char connection_id[SOCKET_CONNECTION_ID_SIZE];
    socket_role_t role;
    socket_connection_mode_t connection_mode;

#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    SSL_CTX *quic_ctx;
    SSL *quic;
    socket_stream_t *game_stream;
    socket_stream_t *pending_streams;
    size_t pending_stream_count;
    uint64_t quic_event_deadline_ms;
#endif

    struct sockaddr_storage late_stun_source;
    socklen_t late_stun_source_length;
    unsigned char late_stun_transaction[12];

    /** Whether this object owns and must close handle. */
    bool owns_handle : 1;
    /** Whether connection_id is the final shared QUIC diagnostic ID. */
    bool connection_id_final : 1;
    /** Whether a QUIC CONNECTION_CLOSE has already been requested. */
    bool quic_shutdown_sent : 1;
    /** Whether a timed-out STUN response may still arrive on handle. */
    bool late_stun_pending : 1;
};

typedef int (*socket_stun_resolver_t)(const char *host,
                                      const char *service,
                                      const struct addrinfo *hints,
                                      struct addrinfo **addresses);
typedef int (*socket_create_resolver_t)(const char *host,
                                        const char *service,
                                        const struct addrinfo *hints,
                                        struct addrinfo **addresses);
typedef void (*socket_addrinfo_free_t)(struct addrinfo *addresses);
typedef uint64_t (*socket_stun_clock_t)(void);
typedef void (*socket_stun_after_send_t)(void);
typedef bool (*socket_rendezvous_fallback_t)(socket_t *sc,
                                             bool directory_probe_allowed,
                                             char *host,
                                             size_t host_size,
                                             uint16_t *port);

#ifdef HAVE_GETADDRINFO
/** Copy one complete resolved address into zeroed caller-owned storage. */
bool socket_addrinfo_copy(struct sockaddr_storage *destination, const struct addrinfo *address);
/** Replace socket_create() resolver ownership callbacks for one isolated test. */
void socket_create_resolver_set_for_test(socket_create_resolver_t resolver,
                                         socket_addrinfo_free_t release);
#endif
bool socket_stun_discover_until(socket_t *sc,
                                const char *endpoint,
                                char *host,
                                size_t host_size,
                                uint16_t *port,
                                uint64_t deadline_ms);
void socket_stun_resolver_set_for_test(socket_stun_resolver_t resolver);
void socket_stun_clock_set_for_test(socket_stun_clock_t clock);
void socket_stun_after_send_set_for_test(socket_stun_after_send_t after_send);
void socket_stun_resolver_wait_for_test(void);
void socket_rendezvous_fallback_set_for_test(socket_rendezvous_fallback_t fallback);
uint64_t socket_rendezvous_stun_deadline(uint64_t now_ms, uint64_t attempt_deadline_ms);
bool socket_udp_punch_receive_pre_quic(socket_t *sc, char *host, size_t host_size, uint16_t *port);

size_t socket_rendezvous_client(socket_t *sc,
                                const char *url,
                                const char *stun_endpoint,
                                socket_rendezvous_attempt_t *attempt,
                                socket_direct_candidate_t *candidates,
                                size_t capacity,
                                socket_connect_failure_t *failure);

bool socket_connection_id_generate(socket_t *sc);
bool socket_connection_id_export(socket_t *sc);
double socket_candidate_kind_timeout(socket_candidate_kind_t kind);

#endif
