/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef TOOLKIT_MAP_PROTOCOL_H
#define TOOLKIT_MAP_PROTOCOL_H

#include "toolkit.h"
#include "packet.h"

/** Bytes preceding the first framed level in a partial MAP2 payload. */
#define MAP2_PARTIAL_HEADER_SIZE 7U
/** Depth and uint32 payload length preceding one MAP2 level chunk. */
#define MAP2_LEVEL_FRAME_SIZE 5U
/** Largest level payload guaranteed to fit in a partial MAP2 envelope. */
#define MAP2_LEVEL_PAYLOAD_MAX \
    (PACKET_PAYLOAD_MAX - MAP2_PARTIAL_HEADER_SIZE - MAP2_LEVEL_FRAME_SIZE)

static inline bool map_protocol_level_payload_fits(size_t payload_size) {
    return payload_size <= MAP2_LEVEL_PAYLOAD_MAX;
}

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

/** Read-only routing information for one validated MAP2 envelope. */
typedef struct map_protocol_packet_info {
    uint8_t mapstat;
    uint8_t x;
    uint8_t y;
    uint8_t sub_layer;
    uint16_t continuation;
    uint16_t depths;
} map_protocol_packet_info_t;

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
 * Validate one complete protocol-v1080 CLIENT_CMD_MAP payload.
 *
 * No endpoint state is changed. The caller supplies the negotiated wire look
 * dimensions used to bound tile coordinates.
 */
bool map_protocol_validate(const uint8_t *data,
                           size_t len,
                           size_t pos,
                           int map_width,
                           int map_height);

/**
 * Encode a protocol-1085 coarse local light byte.
 * @param level Signed level, -MAP2_LOCAL_LIGHT_LEVEL_MAX..MAP2_LOCAL_LIGHT_LEVEL_MAX.
 * @param vision_floor Whether the viewer's vision floor applies after composition.
 */
uint8_t map_protocol_local_light_encode(int level, bool vision_floor);

/**
 * Decode a protocol-1085 coarse local light byte.
 * @return False for reserved bits or the invalid level -16; outputs are then
 * unchanged.
 */
bool map_protocol_local_light_decode(uint8_t value, int8_t *level, bool *vision_floor);

/** Largest signed Q4.11 radiance magnitude; -32768 is never encoded (1085). */
#define MAP2_SIGNED_RADIANCE_MAX 32767

/**
 * Encode raw light as protocol-1085 signed Q4.11 base radiance: the legacy
 * Q5.11 rounding applied to the magnitude, saturated to +/-32767.
 */
int16_t map_protocol_signed_radiance_encode(int64_t raw_light);

/** Validate a MAP2 envelope and return side-effect-free routing metadata. */
bool map_protocol_inspect(const uint8_t *data,
                          size_t len,
                          size_t pos,
                          int map_width,
                          int map_height,
                          map_protocol_packet_info_t *info);

#endif
