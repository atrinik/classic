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

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <setting_value.h>
#include <toolkit/memory.h>
#include <toolkit/porting.h>

static bool setting_value_parse_int64(const char *text, int64_t *value) {
    char *end;

    errno = 0;
    intmax_t parsed = strtoimax(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < INT64_MIN ||
        parsed > INT64_MAX) {
        return false;
    }

    *value = (int64_t)parsed;
    return true;
}

bool setting_value_parse(setting_struct *setting, const char *text) {
    int64_t value;

    switch (setting->type) {
        case OPT_TYPE_BOOL:
            if (!strcasecmp(text, "yes") || !strcasecmp(text, "on") || !strcasecmp(text, "true")) {
                value = 1;
            } else if (!strcasecmp(text, "no") || !strcasecmp(text, "off") ||
                       !strcasecmp(text, "false")) {
                value = 0;
            } else if (!setting_value_parse_int64(text, &value) || (value != 0 && value != 1)) {
                return false;
            }
            break;

        case OPT_TYPE_SELECT:
            if (!setting_value_parse_int64(text, &value) || value < 0 ||
                (uint64_t)value >= SETTING_SELECT(setting)->options_len) {
                return false;
            }
            break;

        case OPT_TYPE_RANGE:
            if (!setting_value_parse_int64(text, &value) || value < SETTING_RANGE(setting)->min ||
                value > SETTING_RANGE(setting)->max || SETTING_RANGE(setting)->advance <= 0 ||
                ((uint64_t)value - (uint64_t)SETTING_RANGE(setting)->min) %
                        (uint64_t)SETTING_RANGE(setting)->advance !=
                    0) {
                return false;
            }
            break;

        case OPT_TYPE_INPUT_NUM:
        case OPT_TYPE_INT:
            if (!setting_value_parse_int64(text, &value)) {
                return false;
            }
            break;

        case OPT_TYPE_INPUT_TEXT:
        case OPT_TYPE_COLOR:
            free(setting->val.str);
            setting->val.str = xstrdup(text);
            return true;

        default:
            return false;
    }

    setting->val.i = value;
    return true;
}

bool setting_value_parse_legacy_zoom_filter(setting_struct *setting, const char *text) {
    const char *value = text;

    if (!strcasecmp(text, "yes") || !strcasecmp(text, "on") || !strcasecmp(text, "true")) {
        value = "1";
    } else if (!strcasecmp(text, "no") || !strcasecmp(text, "off") ||
               !strcasecmp(text, "false")) {
        value = "0";
    } else if (strcmp(text, "0") != 0 && strcmp(text, "1") != 0) {
        return false;
    }

    return setting_value_parse(setting, value);
}
