/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
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
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

#ifndef CLIENT_SOCKET_H
#define CLIENT_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <toolkit/access_code.h>
#include <toolkit/socket.h>

typedef struct client_socket client_socket_t;
struct packet_struct;

/**
 * @file
 * Public declarations for the corresponding client module.
 */

/** Public API implemented in src/client/socket.c. */

/* Open, close, shutdown requests, shutdown polling and deinitialization run on
 * the main thread: they also clear account-local UI and editor draft state. */

extern void socket_send_packet(struct packet_struct *packet);
/** Always consumes packet; false leaves a bounded producer responsible for retry. */
bool socket_send_packet_bounded(struct packet_struct *packet, size_t queue_limit);

/** Queue the one access-auth payload without generic packet debug serialization. */
bool client_socket_send_access_auth(const char code[ACCESS_CODE_LENGTH]);

/** Queue one bounded in-game access-management request without packet diagnostics. */
bool client_socket_send_access_admin(const char *json, size_t size);

extern void socket_thread_start(void);

extern void socket_thread_stop(void);

/** Ask the transport thread to close after a protocol or authentication failure. */
void client_socket_request_shutdown(void);

extern int handle_socket_shutdown(void);

/** Whether the transport thread has requested main-thread shutdown handling. */
extern bool client_socket_shutdown_pending(void);

/** Whether a live client connection is present. */
extern bool client_socket_active(void);

#ifdef ATRINIK_WIDGET_TESTS
/** Set the transport shutdown flag without starting an I/O thread. */
extern void client_socket_shutdown_test_set(bool pending);
#endif

/** Snapshot the live QUIC connection mode while holding its lifetime lock. */
extern bool client_socket_connection_mode(socket_connection_mode_t *mode);

extern void client_socket_close(client_socket_t *csock);

extern void client_socket_deinitialize(void);

extern bool client_socket_open(client_socket_t *csock,
                               const char *host,
                               int port,
                               const char *quic_certificate_sha256,
                               socket_connection_preference_t preference);

#endif
