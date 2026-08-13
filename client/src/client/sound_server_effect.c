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

/**
 * @file
 * Dispatch of server-selected sound effects.
 */

#include <global.h>

/**
 * Play a concrete sound effect selected by the server.
 *
 * The filename is an opaque logical asset identifier. Gameplay state such as
 * the local player's gender must not reinterpret it.
 *
 * @param filename
 * Sound effect filename received from the server.
 * @param volume
 * Playback volume.
 * @param loop
 * Number of repeats, or -1 to loop indefinitely.
 * @return
 * Playback channel, or -1 on failure.
 */
int sound_play_server_effect(const char *filename, int volume, int loop) {
    return sound_play_effect_loop(filename, volume, loop);
}
