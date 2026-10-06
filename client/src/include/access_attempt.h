/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2 of the License, or    *
 *   (at your option) any later version.                                  *
 ************************************************************************/

#ifndef ACCESS_ATTEMPT_H
#define ACCESS_ATTEMPT_H

#include <stdbool.h>
#include <stddef.h>
#include <toolkit/access_code.h>

typedef struct client_access_attempt {
    char code[ACCESS_CODE_BUFFER_SIZE];
    bool present;
} client_access_attempt_t;

/** Normalize and retain one access code for the current connection attempt. */
bool client_access_attempt_set(client_access_attempt_t *attempt,
                               const char *input,
                               size_t input_size);

/** Idempotently cleanse an access attempt. */
void client_access_attempt_clear(client_access_attempt_t *attempt);

#endif
