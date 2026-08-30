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

#include <client_command_queue.h>

/**
 * @file
 * Public declarations for the corresponding client module.
 */

/** Public API implemented in src/client/socket.c. */

extern void socket_send_packet(struct packet_struct *packet);

extern void socket_thread_start(void);

extern void socket_thread_stop(void);

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
