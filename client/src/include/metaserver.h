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

#ifndef METASERVER_H
#define METASERVER_H

#include <stdbool.h>
#include <toolkit/curl.h>
#include <stddef.h>

typedef struct server_struct server_struct;
typedef struct client_metaserver_options client_metaserver_options_t;

/**
 * @file
 * Public declarations for the corresponding client module.
 */

/** Public API implemented in src/client/metaserver.c. */

extern void metaserver_init(void);

extern server_struct *server_get_id(size_t num);

bool metaserver_rendezvous_url(const server_struct *server, char *url, size_t url_size);

/** Main-thread resolution using the current session endpoints. */
server_struct *metaserver_access_resolve(const char *code);
/** Worker resolution using caller-owned immutable endpoint settings. */
server_struct *metaserver_access_resolve_cancellable(
    const client_metaserver_options_t *options, const char *code, const curl_cancel_t *cancel);

/** Add or release a resolved server after the user accepts or cancels its identity. */
void metaserver_server_add(server_struct *server);
void metaserver_server_free(server_struct *server);

extern size_t server_get_count(void);

extern int ms_connecting(int val);

extern void metaserver_clear_data(void);

extern void metaserver_deinit(void);

extern server_struct *metaserver_add(const char *hostname,
                                     int port,
                                     const char *name,
                                     const char *version,
                                     const char *desc);

typedef enum metaserver_provider {
    METASERVER_PROVIDER_DEFAULT,
    METASERVER_PROVIDER_DEV
} metaserver_provider_t;

/** Main-thread APIs. Provider changes affect only this client session. */
metaserver_provider_t metaserver_get_provider(void);
/** Clear selection/list and replace endpoints. Caller requests ST_META to refresh. */
void metaserver_toggle_provider(void);

/** Publish completed current requests and start the latest pending refresh. */
void metaserver_poll(void);

extern void metaserver_get_servers(void);

#endif
