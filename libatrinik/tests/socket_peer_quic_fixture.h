/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ATRINIK_TEST_SOCKET_PEER_QUIC_FIXTURE_H
#define ATRINIK_TEST_SOCKET_PEER_QUIC_FIXTURE_H

#include <toolkit/socket.h>

#include <stddef.h>

/* Called synchronously after an accepted peer's address has been refreshed and
 * verified, initially and repeatedly during interleaved traffic. Index 0 is
 * 127.0.0.2; index 1 is 127.0.0.3. The fixture owns the peer: callbacks may inspect
 * it and invoke policy checks but must not retain it, destroy it, or consume its
 * application streams. Callbacks must assert their own expected policy result.
 * The callback is optional and receives no stale-address injection state. */
typedef void (*socket_peer_quic_observer_t)(socket_t *peer, size_t client_index);

/* Owns toolkit initialization, two clients, listener, accepted connections and
 * temporary identity. Returns zero after all assertions and normal cleanup;
 * failed assertions terminate the test. Compile only when QUIC peer-address
 * support is required: this fixture has no unsupported-platform skip path. */
int socket_peer_quic_fixture_run(socket_peer_quic_observer_t observe);

#endif
