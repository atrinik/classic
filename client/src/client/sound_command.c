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
 * Server sound command decoding and dispatch.
 */

#include <stdint.h>

#include <toolkit/logger.h>
#include <toolkit/packet.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

#include "sound_command_internal.h"

/** @copydoc socket_command_struct::handle_func */
void socket_command_sound(uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type = packet_reader_read_uint8(&reader);
    char filename[MAX_BUF];
    packet_reader_read_string(&reader, filename, sizeof(filename));
    int loop = packet_reader_read_int8(&reader);
    int volume = packet_reader_read_int8(&reader);

    if (type == CMD_SOUND_EFFECT) {
        int8_t x = packet_reader_read_uint8(&reader);
        int8_t y = packet_reader_read_uint8(&reader);
        sound_command_play_effect(filename, volume, loop, x, y);
    } else if (type == CMD_SOUND_BACKGROUND) {
        sound_command_play_background(filename, volume, loop);
    } else if (type == CMD_SOUND_ABSOLUTE) {
        sound_command_play_absolute(filename, volume, loop);
    } else {
        LOG(BUG, "Invalid sound type: %d", type);
    }
}
