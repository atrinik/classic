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

#include <access_attempt.h>

bool client_access_attempt_set(client_access_attempt_t *attempt,
                               const char *input,
                               size_t input_size) {
    if (attempt == NULL) {
        return false;
    }
    client_access_attempt_clear(attempt);
    if (!access_code_normalize(input, input_size, attempt->code)) {
        return false;
    }
    attempt->present = true;
    return true;
}

void client_access_attempt_clear(client_access_attempt_t *attempt) {
    if (attempt == NULL) {
        return;
    }
    access_code_clear(attempt, sizeof(*attempt));
}
