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

#ifndef WINDOW_TITLE_H
#define WINDOW_TITLE_H

#include <stdbool.h>

#define CLIENT_LAUNCH_LABEL_ENV "ATRINIK_LAUNCH_LABEL"
#define CLIENT_LAUNCH_LABEL_MAX_SIZE 96

/**
 * Set the process-local native window title from a wrapper launch label.
 *
 * Invalid or absent labels select the ordinary package title.
 * @param label
 * Printable ASCII launch label, or NULL for an unmanaged launch.
 * @return true when label was accepted, false when the package-title fallback
 * was selected.
 */
extern bool client_window_title_init(const char *label);

/** Return the immutable title selected during client initialization. */
extern const char *client_window_title(void);

#endif
