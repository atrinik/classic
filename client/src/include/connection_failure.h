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

#ifndef CONNECTION_FAILURE_H
#define CONNECTION_FAILURE_H

#include <stdbool.h>
#include <stddef.h>
#include <toolkit/socket.h>

/** Format a bounded, credential-free connection failure for the user. */
bool client_connection_failure_format(const socket_connect_failure_t *failure,
                                      char *message,
                                      size_t message_size);

#endif
