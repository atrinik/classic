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

#ifndef ACCESS_PROTOCOL_H
#define ACCESS_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Parse the exact two-byte access-policy payload. */
bool client_access_policy_parse(const uint8_t *data, size_t size, bool *required);

/** Parse the exact two-byte access-result payload. */
bool client_access_result_parse(const uint8_t *data, size_t size, bool *accepted);

#endif
