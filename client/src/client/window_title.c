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

static bool valid_name(const char *name, size_t length) {
    if (length == 0 || name[0] < 'a' || name[0] > 'z') {
        if (length == 0 || name[0] < '0' || name[0] > '9') {
            return false;
        }
    }
    for (size_t i = 1; i < length; i++) {
        char character = name[i];
        if ((character < 'a' || character > 'z') && (character < '0' || character > '9') &&
            character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool valid_label(const char *label, size_t length) {
    static const char topology_prefix[] = "topology ";
    static const char profile_separator[] = " - profile ";
    static const char direct_prefix[] = "profile ";
    static const char direct_suffix[] = " (direct run)";
    const size_t topology_prefix_length = strlen(topology_prefix);
    const size_t profile_separator_length = strlen(profile_separator);
    const size_t direct_prefix_length = strlen(direct_prefix);
    const size_t direct_suffix_length = strlen(direct_suffix);

    if (length > topology_prefix_length &&
        memcmp(label, topology_prefix, topology_prefix_length) == 0) {
        const char *topology = label + topology_prefix_length;
        const char *separator = strstr(topology, profile_separator);
        if (separator == NULL) {
            return false;
        }
        const char *profile = separator + profile_separator_length;
        return valid_name(topology, (size_t)(separator - topology)) &&
               valid_name(profile, length - (size_t)(profile - label));
    }

    if (length > direct_prefix_length + direct_suffix_length &&
        memcmp(label, direct_prefix, direct_prefix_length) == 0 &&
        memcmp(label + length - direct_suffix_length, direct_suffix, direct_suffix_length) == 0) {
        const char *profile = label + direct_prefix_length;
        size_t profile_length = length - direct_prefix_length - direct_suffix_length;
        return valid_name(profile, profile_length);
    }
    return false;
}

bool client_window_title_init(const char *label) {
    snprintf(window_title, sizeof(window_title), "%s", PACKAGE_NAME);
    if (label == NULL) {
        return false;
    }

    size_t length = 0;
    while (length <= CLIENT_LAUNCH_LABEL_MAX_SIZE && label[length] != '\0') {
        length++;
    }
    if (length == 0 || length > CLIENT_LAUNCH_LABEL_MAX_SIZE || !valid_label(label, length)) {
        return false;
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

SDL_Window *client_window_create(int width, int height, uint32_t flags) {
    return SDL_CreateWindow(client_window_title(), width, height, flags);
}

void client_window_title_apply(SDL_Window *window) {
    SDL_SetWindowTitle(window, client_window_title());
}
