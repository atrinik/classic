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

#ifndef AUDIO_LOG_H
#define AUDIO_LOG_H

#include <stdbool.h>
#include <stddef.h>

void audio_log_asset_escape(const char *asset, char *buf, size_t size);
void audio_log_effect_started(const char *source,
                              const char *requested,
                              const char *effective,
                              int channel,
                              int volume,
                              int loop,
                              bool positioned,
                              int angle,
                              int distance);
void audio_log_music_started(const char *source,
                             const char *requested,
                             const char *effective,
                             int volume,
                             int loop);
void audio_log_music_stopped(const char *source, const char *effective, const char *reason);

#endif
