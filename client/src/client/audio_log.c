/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <audio_log.h>
#include <toolkit/logger.h>
#include <toolkit/toolkit.h>

void audio_log_asset_escape(const char *asset, char *buf, size_t size) {
    if (size == 0) {
        return;
    }

    const char *logical = asset != NULL ? asset : "";
    bool absolute = logical[0] == '/' || logical[0] == '\\' ||
                    (isalpha((unsigned char)logical[0]) && logical[1] == ':' &&
                     (logical[2] == '/' || logical[2] == '\\'));
    if (absolute) {
        const char *slash = strrchr(logical, '/');
        const char *backslash = strrchr(logical, '\\');
        const char *separator = slash == NULL       ? backslash
                                : backslash == NULL ? slash
                                                    : MAX(slash, backslash);
        if (separator != NULL) {
            logical = separator + 1;
        }
    }

    size_t pos = 0;
    for (const unsigned char *cp = (const unsigned char *)logical; *cp != '\0' && pos + 1 < size;
         cp++) {
        const char *escape = NULL;
        if (*cp == '"') {
            escape = "\\\"";
        } else if (*cp == '\\') {
            escape = "\\\\";
        } else if (*cp == '\n') {
            escape = "\\n";
        } else if (*cp == '\r') {
            escape = "\\r";
        } else if (*cp == '\t') {
            escape = "\\t";
        }

        if (escape != NULL) {
            size_t length = strlen(escape);
            if (length >= size - pos) {
                break;
            }
            memcpy(buf + pos, escape, length);
            pos += length;
        } else if (*cp < 0x20 || *cp == 0x7f) {
            if (size - pos <= 4) {
                break;
            }
            int written = snprintf(buf + pos, size - pos, "\\x%02x", *cp);
            if (written != 4) {
                break;
            }
            pos += 4;
        } else {
            buf[pos++] = (char)*cp;
        }
    }
    buf[pos] = '\0';
}

void audio_log_effect_started(const char *source,
                              const char *requested,
                              const char *effective,
                              int channel,
                              int volume,
                              int loop,
                              bool positioned,
                              int angle,
                              int distance) {
    char requested_log[MAX_BUF * 2], effective_log[MAX_BUF * 2];
    audio_log_asset_escape(requested, VS(requested_log));
    audio_log_asset_escape(effective, VS(effective_log));

    if (positioned) {
        LOG(AUDIO,
            "effect started source=%s requested=\"%s\" effective=\"%s\" channel=%d volume=%d "
            "loop=%d angle=%d distance=%d",
            source != NULL ? source : "unknown",
            requested_log,
            effective_log,
            channel,
            volume,
            loop,
            angle,
            distance);
    } else {
        LOG(AUDIO,
            "effect started source=%s requested=\"%s\" effective=\"%s\" channel=%d volume=%d "
            "loop=%d",
            source != NULL ? source : "unknown",
            requested_log,
            effective_log,
            channel,
            volume,
            loop);
    }
}

void audio_log_music_started(const char *source,
                             const char *requested,
                             const char *effective,
                             int volume,
                             int loop) {
    char requested_log[MAX_BUF * 2], effective_log[MAX_BUF * 2];
    audio_log_asset_escape(requested, VS(requested_log));
    audio_log_asset_escape(effective, VS(effective_log));
    LOG(AUDIO,
        "music started source=%s requested=\"%s\" effective=\"%s\" volume=%d loop=%d",
        source != NULL ? source : "unknown",
        requested_log,
        effective_log,
        volume,
        loop);
}

void audio_log_music_stopped(const char *source, const char *effective, const char *reason) {
    char effective_log[MAX_BUF * 2];
    audio_log_asset_escape(effective, VS(effective_log));
    LOG(AUDIO,
        "music stopped source=%s effective=\"%s\" reason=%s",
        source != NULL ? source : "unknown",
        effective_log,
        reason != NULL ? reason : "unknown");
}
