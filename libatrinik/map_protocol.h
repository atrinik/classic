/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef TOOLKIT_MAP_PROTOCOL_H
#define TOOLKIT_MAP_PROTOCOL_H

#include "toolkit.h"

/** Bounded client state for one ordered MAP2 continuation sequence. */
typedef struct map_protocol_continuation_state {
    bool pending;
    uint8_t x;
    uint8_t y;
    uint8_t sub_layer;
    uint16_t depths;
    uint16_t total;
    uint16_t next;
} map_protocol_continuation_state_t;

void map_protocol_continuation_reset(map_protocol_continuation_state_t *state);
void map_protocol_continuation_begin(map_protocol_continuation_state_t *state,
                                     uint16_t count,
                                     uint8_t x,
                                     uint8_t y,
                                     uint8_t sub_layer,
                                     uint16_t depths);
bool map_protocol_continuation_matches(const map_protocol_continuation_state_t *state,
                                       uint16_t sequence,
                                       uint8_t x,
                                       uint8_t y,
                                       uint8_t sub_layer,
                                       uint16_t depths);
void map_protocol_continuation_advance(map_protocol_continuation_state_t *state);

/**
 * Validate one complete protocol-v1075 CLIENT_CMD_MAP payload.
 *
 * No endpoint state is changed. The caller supplies the negotiated wire look
 * dimensions used to bound tile coordinates.
 */
bool map_protocol_validate(const uint8_t *data,
                           size_t len,
                           size_t pos,
                           int map_width,
                           int map_height);

#endif
