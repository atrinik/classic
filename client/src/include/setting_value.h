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

#ifndef SETTING_VALUE_H
#define SETTING_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include <settings.h>

/** Parse and apply a setting value without changing the setting on failure. */
bool setting_value_parse(setting_struct *setting, const char *text);

/** Parse a pre-473 boolean zoom-filter value into the new select setting. */
bool setting_value_parse_legacy_zoom_filter(setting_struct *setting, const char *text);

#endif
