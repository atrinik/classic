/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Private metaserver implementation declarations.
 */

#ifndef CLIENT_METASERVER_PRIVATE_H
#define CLIENT_METASERVER_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <main.h>

void metaserver_server_add(server_struct *server);

void metaserver_server_free(server_struct *server);

bool metaserver_direct_parse(const char *body,
                             size_t body_size,
                             const char *rendezvous_origin,
                             uint64_t now,
                             uint64_t minimum_generation,
                             uint64_t *accepted_generation);

#endif
