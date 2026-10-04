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

#include <access_protocol.h>

static bool access_boolean_payload(const uint8_t *data, size_t size, bool *value) {
    if (data == NULL || value == NULL || size != 2 || data[0] != 1 || data[1] > 1) {
        return false;
    }
    *value = data[1] == 1;
    return true;
}

bool client_access_policy_parse(const uint8_t *data, size_t size, bool *required) {
    return access_boolean_payload(data, size, required);
}

bool client_access_result_parse(const uint8_t *data, size_t size, bool *accepted) {
    bool unavailable;
    if (!access_boolean_payload(data, size, &unavailable)) {
        return false;
    }
    *accepted = !unavailable;
    return true;
}
