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

#ifndef SOUND_COMMAND_INTERNAL_H
#define SOUND_COMMAND_INTERNAL_H

#include <stdint.h>

void sound_command_play_effect(const char *filename, int volume, int loop, int8_t x, int8_t y);
void sound_command_play_background(const char *filename, int volume, int loop);
void sound_command_play_absolute(const char *filename, int volume, int loop);

#endif
