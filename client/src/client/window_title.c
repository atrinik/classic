/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <window_title.h>

#include <stdio.h>
#include <string.h>

#include <version.h>

static char window_title[sizeof(PACKAGE_NAME) + sizeof(" — ") + CLIENT_LAUNCH_LABEL_MAX_SIZE] =
    PACKAGE_NAME;

bool client_window_title_init(const char *label) {
    snprintf(window_title, sizeof(window_title), "%s", PACKAGE_NAME);
    if (label == NULL) {
        return false;
    }

    size_t length = strlen(label);
    if (length == 0 || length > CLIENT_LAUNCH_LABEL_MAX_SIZE) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)label[i];
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }

    int written = snprintf(window_title, sizeof(window_title), "%s — %s", PACKAGE_NAME, label);
    if (written < 0 || (size_t)written >= sizeof(window_title)) {
        snprintf(window_title, sizeof(window_title), "%s", PACKAGE_NAME);
        return false;
    }
    return true;
}

const char *client_window_title(void) {
    return window_title;
}
