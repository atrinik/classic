/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#ifndef CLIENT_ACCESS_ADMIN_H
#define CLIENT_ACCESS_ADMIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Handle an in-game /access command locally and send a dedicated packet. */
bool client_access_admin_command(const char *command);

/** Validate and present one private ACCESS_ADMIN_RESULT JSON payload. */
bool client_access_admin_response(const uint8_t *data, size_t size);

/** Expire a request that has received no response within the protocol deadline. */
void client_access_admin_update(void);

/** Clear request correlation, cached revision, and pending operation state. */
void client_access_admin_reset(void);

#endif
