/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/map_protocol.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>

#define packet_verify_data(packet, str)                                                 \
    {                                                                                   \
        char *hex;                                                                      \
        hex = xmalloc(sizeof(*hex) * ((packet)->len * 2 + 1));                          \
        string_tohex((packet)->data, (packet)->len, hex, (packet)->len * 2 + 1, false); \
        ck_assert_str_eq(hex, (str));                                                   \
        free(hex);                                                                      \
    }

START_TEST(test_packet_new) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    ck_assert_ptr_ne(packet, NULL);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 5, 0);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 5, 5);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 5, 100);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 100, 0);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 100, 100);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);

    packet = packet_new(0, 0, 100);
    packet_writer_write_cstring(packet, "hello world");
    ck_assert_str_eq((const char *)packet->data, "hello world");
    packet_free(packet);
}
END_TEST

/** Build the invariant prefix of a same-map protocol-v1078 MAP update. */
static packet_struct *map_protocol_test_packet(uint8_t level_count) {
    packet_struct *packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, MAP_UPDATE_CMD_SAME);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint8(packet, level_count);
    return packet;
}

static packet_struct *map_protocol_test_packet_with_marker(uint16_t marker) {
    packet_struct *packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, MAP_UPDATE_CMD_SAME);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint16(packet, marker);
    return packet;
}

static void map_protocol_test_level(packet_struct *packet, int8_t depth, packet_struct *level);

/** Build a complete NEW MAP update with caller-selected metadata. */
static packet_struct *map_protocol_test_new_packet(const char *map_name,
                                                   const char *music,
                                                   const char *weather,
                                                   const char *region_name,
                                                   const char *region_longname,
                                                   const char *map_path) {
    packet_struct *packet = packet_new(0, 128, PACKET_PAYLOAD_MAX);
    packet_writer_write_uint8(packet, MAP_UPDATE_CMD_NEW);
    packet_writer_write_cstring(packet, map_name);
    packet_writer_write_cstring(packet, music);
    packet_writer_write_cstring(packet, weather);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_cstring(packet, region_name);
    packet_writer_write_cstring(packet, region_longname);
    packet_writer_write_cstring(packet, map_path);
    packet_writer_write_uint8(packet, 21);
    packet_writer_write_uint8(packet, 21);
    packet_writer_write_uint8(packet, 10);
    packet_writer_write_uint8(packet, 10);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint16(packet, 0);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    return packet;
}

/** Append one framed MAP level to a test packet. */
static void map_protocol_test_level(packet_struct *packet, int8_t depth, packet_struct *level) {
    packet_writer_write_int8(packet, depth);
    packet_writer_write_uint32(packet, level != NULL ? level->len : 0);
    if (level != NULL) {
        packet_writer_write_packet(packet, level);
    }
}

START_TEST(test_map_protocol_validate_minimal_and_truncation) {
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, NULL);

    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    ck_assert(!map_protocol_validate(NULL, 0, 0, 21, 21));
    ck_assert(!map_protocol_validate(packet->data, packet->len, packet->len + 1, 21, 21));
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 0, 21));
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 33));
    for (size_t len = 0; len < packet->len; len++) {
        ck_assert(!map_protocol_validate(packet->data, len, 0, 21, 21));
    }

    packet_writer_write_uint8(packet, 0);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
}
END_TEST

START_TEST(test_map_protocol_rejects_duplicate_depth) {
    packet_struct *packet = map_protocol_test_packet(2);
    map_protocol_test_level(packet, 0, NULL);
    map_protocol_test_level(packet, 0, NULL);

    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
}
END_TEST

START_TEST(test_map_protocol_enforces_level_framing) {
    packet_struct *packet = map_protocol_test_packet(2);
    map_protocol_test_level(packet, -1, NULL);
    map_protocol_test_level(packet, 0, NULL);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 1, NULL);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet(1);
    packet_writer_write_int8(packet, 0);
    packet_writer_write_uint32(packet, 1);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, MAP2_MAX_DEPTH + 1, NULL);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
}
END_TEST

START_TEST(test_map_protocol_rejects_bad_tile_and_layer_indices) {
    packet_struct *level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 21 << 11);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, 0);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, MAP2_PROTOCOL_REAL_LAYERS);
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, MAP2_LAYER_CLEAR);
    packet_writer_write_uint8(level, MAP2_PROTOCOL_REAL_LAYERS);
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_validates_tile_record_flags) {
    packet_struct *level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, MAP2_MASK_FOW | MAP2_MASK_LIGHT_LEVEL);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint16(level, 2176);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, 0);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, 2);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_accepts_exit_flag_and_rejects_unknown_or_truncated_flags) {
    packet_struct *level = packet_new(0, 32, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, LAYER_FLOOR - 1);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_MORE);
    packet_writer_write_uint32(level, MAP2_FLAG2_EXIT);
    packet_writer_write_uint8(level, 0);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));

    size_t flags2_offset = packet->len - sizeof(uint32_t) - sizeof(uint8_t);
    packet->data[flags2_offset + 2] |= UINT8_C(0x08);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, LAYER_FLOOR - 1);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_MORE);
    packet_writer_write_uint16(level, MAP2_FLAG2_EXIT);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_enforces_layer_string_bounds) {
    packet_struct *level = packet_new(0, 128, 128);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, LAYER_LIVING - 1);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_NAME | MAP2_FLAG_MORE);
    packet_writer_write_cstring(level,
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    packet_writer_write_cstring(level, "ffffff");
    packet_writer_write_uint32(level, MAP2_FLAG2_GLOW);
    packet_writer_write_cstring(level, "ffffff");
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 128, 128);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, LAYER_LIVING - 1);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_NAME);
    packet_writer_write_cstring(level,
                                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    packet_writer_write_cstring(level, "ffffff");
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 32, 32);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, LAYER_FLOOR - 1);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_MORE);
    packet_writer_write_uint32(level, MAP2_FLAG2_GLOW);
    packet_writer_write_cstring(level, "fffffff");
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_validates_colored_light_extension) {
    packet_struct *level = packet_new(0, 32, 16);
    packet_writer_write_uint16(level, MAP2_MASK_LIGHT_LEVEL);
    packet_writer_write_uint16(level, 2176);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_EXT_LIGHT_RADIANCE_RGB16);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint16(level, 2176);
    packet_writer_write_uint16(level, 2062);
    packet_writer_write_uint16(level, 2051);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));

    for (size_t len = 0; len < packet->len; len++) {
        ck_assert(!map_protocol_validate(packet->data, len, 0, 21, 21));
    }
    packet_writer_write_uint8(packet, 0xff);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 16, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_EXT_LIGHT_RADIANCE_RGB16);
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    level->data[level->len - 1] = UINT8_C(1) << MAP2_PROTOCOL_SUB_LAYERS;
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);

    level = packet_new(0, 32, 16);
    packet_writer_write_uint16(level, MAP2_MASK_LIGHT_LEVEL | MAP2_MASK_LIGHT_LEVEL_MORE);
    packet_writer_write_uint16(level, 0);
    for (size_t sub_layer = 1; sub_layer < MAP2_PROTOCOL_SUB_LAYERS; sub_layer++) {
        packet_writer_write_uint16(level, UINT16_MAX);
    }
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, 0);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_validates_timed_light_keyframes) {
    packet_struct *packet = map_protocol_test_packet_with_marker(MAP2_CONTINUATION_TIMED_LIGHT);
    packet_writer_write_uint64(packet, 7);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint64(packet, 4600);
    packet_writer_write_uint8(packet, MAP2_LIGHT_KEYFRAME_CONTINUOUS);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));

    /* The descriptor is fixed-width and every prefix of it is malformed. */
    for (size_t len = 0; len < packet->len; len++) {
        ck_assert(!map_protocol_validate(packet->data, len, 0, 21, 21));
    }

    /* Descriptor fields begin immediately after the six-byte MAP2 prefix. */
    packet->data[6 + 7] = 0;
    packet->data[6 + 6] = 0;
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet_with_marker(MAP2_CONTINUATION_TIMED_LIGHT);
    packet_writer_write_uint64(packet, 1);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint8(packet, MAP2_LIGHT_KEYFRAME_CONTINUOUS);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet_with_marker(MAP2_CONTINUATION_TIMED_LIGHT);
    packet_writer_write_uint64(packet, 1);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint64(packet, 4600);
    packet_writer_write_uint8(packet, 0x80);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet_with_marker(MAP2_CONTINUATION_TIMED_LIGHT);
    packet_writer_write_uint64(packet, 1);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint64(packet, 4601);
    packet_writer_write_uint8(packet, MAP2_LIGHT_KEYFRAME_CONTINUOUS);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet = map_protocol_test_packet_with_marker(MAP2_CONTINUATION_TIMED_LIGHT);
    packet_writer_write_uint64(packet, 1);
    packet_writer_write_uint64(packet, 1000);
    packet_writer_write_uint64(packet, 4600);
    packet_writer_write_uint8(packet, MAP2_LIGHT_KEYFRAME_CONTINUOUS);
    packet_writer_write_uint8(packet, 1);
    map_protocol_test_level(packet, 0, NULL);
    packet->data[4] = 0;
    packet->data[5] = 1;
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    packet_struct *level = packet_new(0, 32, 16);
    packet_writer_write_uint16(level, MAP2_MASK_LIGHT_LEVEL);
    packet_writer_write_uint16(level, 2176);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_EXT_LIGHT_KEYFRAME);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint16(level, 2180);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint16(level, 2180);
    packet_writer_write_uint16(level, 2170);
    packet_writer_write_uint16(level, 2160);
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));

    /* RGB endpoints must be a subset of the scalar endpoint bitmap. */
    level->data[9] = 2;
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    /* Reserved sub-layer bits and every truncated endpoint are rejected. */
    level->data[6] = UINT8_C(1) << MAP2_PROTOCOL_SUB_LAYERS;
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
    level->data[6] = 1;
    level->data[9] = 1;
    packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    size_t packet_len = packet->len;
    for (size_t len = 0; len < packet_len; len++) {
        ck_assert(!map_protocol_validate(packet->data, len, 0, 21, 21));
    }
    packet_free(packet);
    packet_free(level);
}
END_TEST

/** One cell with an emitter layer and both client-lighting extensions. */
static packet_struct *map_protocol_client_lighting_level_full(int8_t radius,
                                                             uint8_t emitter_flags,
                                                             uint32_t color,
                                                             uint8_t level_value,
                                                             uint16_t occluder,
                                                             uint8_t vertical) {
    packet_struct *level = packet_new(0, 32, 16);
    packet_writer_write_uint16(level, 0);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint16(level, 1);
    packet_writer_write_uint8(level, 0);
    packet_writer_write_uint8(level, MAP2_FLAG_MORE);
    packet_writer_write_uint32(level, MAP2_FLAG2_EMITTER);
    packet_writer_write_int8(level, radius);
    packet_writer_write_uint8(level, emitter_flags);
    packet_writer_write_uint8(level, (uint8_t)(color >> 16));
    packet_writer_write_uint8(level, (uint8_t)(color >> 8));
    packet_writer_write_uint8(level, (uint8_t)color);
    packet_writer_write_uint8(level, MAP2_FLAG_EXT_LOCAL_LIGHT | MAP2_FLAG_EXT_OCCLUDER);
    packet_writer_write_uint8(level, 1);
    packet_writer_write_uint8(level, level_value);
    packet_writer_write_uint16(level, occluder);
    packet_writer_write_uint16(level, 0x4321);
    packet_writer_write_uint8(level, vertical);
    return level;
}

static packet_struct *map_protocol_client_lighting_level_color(int8_t radius, uint32_t color,
                                                              uint8_t level_value,
                                                              uint16_t occluder) {
    return map_protocol_client_lighting_level_full(radius, 0, color, level_value, occluder, 0);
}

static bool map_protocol_client_lighting_valid_full(int8_t radius, uint8_t emitter_flags,
                                                    uint8_t vertical, uint8_t ext_extra) {
    packet_struct *level = map_protocol_client_lighting_level_full(
        radius, emitter_flags, radius > 0 ? UINT32_C(0xff8040) : 0, 0, 0, vertical);
    if (ext_extra != 0) {
        /* Rewrite the extended flags byte that follows the emitter payload. */
        level->data[level->len - 8] |= ext_extra;
    }
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    bool valid = map_protocol_validate(packet->data, packet->len, 0, 21, 21);
    packet_free(packet);
    packet_free(level);
    return valid;
}

/** Positive emitters get an orange color and darkness the achromatic one. */
static packet_struct *map_protocol_client_lighting_level(int8_t radius, uint8_t level_value,
                                                        uint16_t occluder) {
    return map_protocol_client_lighting_level_color(radius, radius > 0 ? UINT32_C(0xff8040) : 0,
                                                    level_value, occluder);
}

static bool map_protocol_client_lighting_valid_color(int8_t radius, uint32_t color) {
    packet_struct *level = map_protocol_client_lighting_level_color(radius, color, 0, 0);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    bool valid = map_protocol_validate(packet->data, packet->len, 0, 21, 21);
    packet_free(packet);
    packet_free(level);
    return valid;
}

static bool map_protocol_client_lighting_valid(int8_t radius, uint8_t level_value,
                                               uint16_t occluder) {
    packet_struct *level = map_protocol_client_lighting_level(radius, level_value, occluder);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    bool valid = map_protocol_validate(packet->data, packet->len, 0, 21, 21);
    packet_free(packet);
    packet_free(level);
    return valid;
}

START_TEST(test_map_protocol_validates_client_lighting_fields) {
    uint8_t all_geometry = MAP2_OCCLUDER_BLOCKSVIEW | MAP2_OCCLUDER_FLOOR | MAP2_OCCLUDER_ROOF |
                           MAP2_OCCLUDER_DOOR_CLOSED;
    ck_assert(map_protocol_client_lighting_valid(13,
                                                 map_protocol_local_light_encode(15, true),
                                                 all_geometry));
    ck_assert(map_protocol_client_lighting_valid(-13,
                                                 map_protocol_local_light_encode(-15, false),
                                                 0x0f0f));
    /* The former single attenuation nibble is reserved: faces have their own. */
    ck_assert(!map_protocol_client_lighting_valid(1, 0, 0x0010));
    /* Up and down face bits are valid; bits 14-15 are reserved. */
    ck_assert(map_protocol_client_lighting_valid(1, 0, 0x3000));
    ck_assert(!map_protocol_client_lighting_valid(1, 0, 0x4000));
    ck_assert(map_protocol_client_lighting_valid(1, map_protocol_local_light_encode(0, false), 0));
    /* A zero radius, level -16 and reserved local-light bits are malformed.
     * Every occluder byte is meaningful. */
    ck_assert(!map_protocol_client_lighting_valid(0, 0, 0));
    /* Radii are unclamped: the whole nonzero int8 range is valid. */
    ck_assert(map_protocol_client_lighting_valid(INT8_MAX, 0, 0));
    ck_assert(map_protocol_client_lighting_valid(INT8_MIN, 0, 0));
    ck_assert(!map_protocol_client_lighting_valid(1, 0x10, 0));
    ck_assert(!map_protocol_client_lighting_valid(1, 0x20, 0));
    /* Bit 6 marks a remote sample; it does not make level -16 valid. */
    ck_assert(map_protocol_client_lighting_valid(1, MAP2_LOCAL_LIGHT_REMOTE, 0));
    ck_assert(map_protocol_client_lighting_valid(
        1, map_protocol_local_light_encode(-15, true) | MAP2_LOCAL_LIGHT_REMOTE, 0));
    ck_assert(!map_protocol_client_lighting_valid(1, MAP2_LOCAL_LIGHT_REMOTE | 0x10, 0));
    /* Positive light may be black (valid content the server renders as a
     * zero linear vector) and darkness is achromatic. */
    ck_assert(map_protocol_client_lighting_valid_color(5, UINT32_C(0x000001)));
    ck_assert(map_protocol_client_lighting_valid_color(5, 0));
    ck_assert(map_protocol_client_lighting_valid_color(-5, 0));
    ck_assert(!map_protocol_client_lighting_valid_color(-5, UINT32_C(0x000100)));
    /* Flicker is a light-only opt-in; other emitter flag bits are reserved. */
    ck_assert(map_protocol_client_lighting_valid_full(5, MAP2_EMITTER_FLICKER, 0, 0));
    ck_assert(!map_protocol_client_lighting_valid_full(-5, MAP2_EMITTER_FLICKER, 0, 0));
    ck_assert(!map_protocol_client_lighting_valid_full(5, 0x02, 0, 0));
    ck_assert(!map_protocol_client_lighting_valid_full(5, 0x80, 0, 0));
    /* The vertical byte carries a nibble; its high nibble is reserved. */
    ck_assert(map_protocol_client_lighting_valid_full(5, 0, 0x0f, 0));
    ck_assert(!map_protocol_client_lighting_valid_full(5, 0, 0x10, 0));
    /* Levels come with a count of 1..NUM_SUB_LAYERS. */
    for (int count = 0; count <= NUM_SUB_LAYERS + 1; count++) {
        packet_struct *level = packet_new(0, 32, 16);
        packet_writer_write_uint16(level, 0);
        packet_writer_write_uint8(level, 0);
        packet_writer_write_uint8(level, MAP2_FLAG_EXT_LOCAL_LIGHT);
        packet_writer_write_uint8(level, (uint8_t)count);
        for (int i = 0; i < count; i++) {
            packet_writer_write_uint8(level, map_protocol_local_light_encode(i - 3, false));
        }
        packet_struct *packet = map_protocol_test_packet(1);
        map_protocol_test_level(packet, 0, level);
        bool valid = map_protocol_validate(packet->data, packet->len, 0, 21, 21);
        ck_assert_int_eq(valid, count >= 1 && count <= NUM_SUB_LAYERS);
        packet_free(packet);
        packet_free(level);
    }
    /* The former aggregate bitmap flag is no longer part of 1085. */
    ck_assert(!map_protocol_client_lighting_valid_full(5, 0, 0, 32));

    /* Signed base radiance is odd-symmetric and saturates symmetrically. */
    static const int64_t raw[] = {0, 1, 160, 1280, 20479, 20480, INT64_C(1) << 40};
    static const int16_t expected[] = {0, 2, 256, 2048, 32766, 32767, 32767};
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        ck_assert_int_eq(map_protocol_signed_radiance_encode(raw[i]), expected[i]);
        ck_assert_int_eq(map_protocol_signed_radiance_encode(-raw[i]), -expected[i]);
    }
    ck_assert_int_eq(map_protocol_signed_radiance_encode(INT64_MIN), -32767);

    for (int level = -MAP2_LOCAL_LIGHT_LEVEL_MAX; level <= MAP2_LOCAL_LIGHT_LEVEL_MAX; level++) {
        for (int floor_bit = 0; floor_bit < 2; floor_bit++) {
            int8_t decoded = 0;
            bool vision_floor = false;
            ck_assert(map_protocol_local_light_decode(
                map_protocol_local_light_encode(level, floor_bit != 0), &decoded, &vision_floor));
            ck_assert_int_eq(decoded, level);
            ck_assert_int_eq(vision_floor, floor_bit != 0);
        }
    }

    packet_struct *level = map_protocol_client_lighting_level(4, 3, MAP2_OCCLUDER_FLOOR);
    packet_struct *packet = map_protocol_test_packet(1);
    map_protocol_test_level(packet, 0, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    size_t packet_len = packet->len;
    for (size_t len = 0; len < packet_len; len++) {
        ck_assert(!map_protocol_validate(packet->data, len, 0, 21, 21));
    }
    packet_free(packet);
    packet_free(level);
}
END_TEST

START_TEST(test_map_protocol_accepts_bounded_continuation) {
    ck_assert_uint_eq(MAP2_PARTIAL_HEADER_SIZE + MAP2_LEVEL_FRAME_SIZE + MAP2_LEVEL_PAYLOAD_MAX,
                      PACKET_PAYLOAD_MAX);
    ck_assert_uint_gt(MAP2_PARTIAL_HEADER_SIZE + MAP2_LEVEL_FRAME_SIZE + MAP2_LEVEL_PAYLOAD_MAX + 1,
                      PACKET_PAYLOAD_MAX);
    ck_assert(map_protocol_level_payload_fits(MAP2_LEVEL_PAYLOAD_MAX - 1));
    ck_assert(map_protocol_level_payload_fits(MAP2_LEVEL_PAYLOAD_MAX));
    ck_assert(!map_protocol_level_payload_fits(MAP2_LEVEL_PAYLOAD_MAX + 1));

    packet_struct *packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, MAP_UPDATE_CMD_PARTIAL);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_uint16(packet, 1);
    packet_writer_write_uint8(packet, 1);
    packet_struct *level = packet_new(0, 2, 2);
    packet_writer_write_uint16(level, MAP2_MASK_CLEAR);
    map_protocol_test_level(packet, 2, level);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    ck_assert_uint_le(packet->len, PACKET_PAYLOAD_MAX);
    packet->data[4] = (uint8_t)(MAP2_PROTOCOL_CONTINUATIONS_MAX >> 8);
    packet->data[5] = (uint8_t)MAP2_PROTOCOL_CONTINUATIONS_MAX;
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet->data[4] = (uint8_t)((MAP2_PROTOCOL_CONTINUATIONS_MAX + 1) >> 8);
    packet->data[5] = (uint8_t)(MAP2_PROTOCOL_CONTINUATIONS_MAX + 1);
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet->data[4] = packet->data[5] = 0;
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet->data[4] = packet->data[5] = UINT8_MAX;
    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet->data[4] = 0;
    packet->data[5] = 1;
    /* Partial continuation frames cannot consume a sequence without data. */
    packet->data[8] = packet->data[9] = packet->data[10] = packet->data[11] = 0;
    ck_assert(!map_protocol_validate(packet->data, packet->len - 2, 0, 21, 21));
    packet_free(level);
    packet_free(packet);
}
END_TEST

START_TEST(test_map_protocol_continuation_state_is_bounded_and_ordered) {
    map_protocol_continuation_state_t state = {0};
    uint16_t declared = (UINT16_C(1) << MAP2_DEPTH_INDEX(0)) | (UINT16_C(1) << MAP2_DEPTH_INDEX(2));

    ck_assert(!map_protocol_continuation_matches(&state, 1, 3, 4, 1, declared));
    map_protocol_continuation_begin(&state, 2, 3, 4, 1, declared);
    ck_assert(!map_protocol_continuation_matches(&state, 2, 3, 4, 1, declared));
    ck_assert(!map_protocol_continuation_matches(&state, 1, 4, 4, 1, declared));
    ck_assert(!map_protocol_continuation_matches(&state, 1, 3, 5, 1, declared));
    ck_assert(!map_protocol_continuation_matches(&state, 1, 3, 4, 2, declared));
    ck_assert(!map_protocol_continuation_matches(&state,
                                                 1,
                                                 3,
                                                 4,
                                                 1,
                                                 UINT16_C(1) << MAP2_DEPTH_INDEX(-1)));
    ck_assert(
        map_protocol_continuation_matches(&state, 1, 3, 4, 1, UINT16_C(1) << MAP2_DEPTH_INDEX(2)));
    map_protocol_continuation_advance(&state);
    ck_assert(!map_protocol_continuation_matches(&state, 1, 3, 4, 1, declared));
    ck_assert(map_protocol_continuation_matches(&state, 2, 3, 4, 1, declared));
    map_protocol_continuation_advance(&state);
    ck_assert(!state.pending);

    map_protocol_continuation_begin(&state, 1, 3, 4, 1, declared);
    map_protocol_continuation_reset(&state);
    ck_assert(!map_protocol_continuation_matches(&state, 1, 3, 4, 1, declared));

    map_protocol_continuation_begin(&state, UINT16_MAX, 3, 4, 1, declared);
    state.next = UINT16_MAX;
    ck_assert(map_protocol_continuation_matches(&state, UINT16_MAX, 3, 4, 1, declared));
    map_protocol_continuation_advance(&state);
    ck_assert(!state.pending);
}
END_TEST

START_TEST(test_map_protocol_rejects_unterminated_metadata) {
    packet_struct *packet = packet_new(0, 16, 16);
    packet_writer_write_uint8(packet, MAP_UPDATE_CMD_NEW);
    packet_writer_write_string(packet, "unterminated");

    ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);
}
END_TEST

START_TEST(test_map_protocol_enforces_metadata_string_bounds) {
    char long_boundary[MAP2_PROTOCOL_METADATA_LONG_MAX + 1];
    char long_oversized[MAP2_PROTOCOL_METADATA_LONG_MAX + 2];
    char short_boundary[MAP2_PROTOCOL_METADATA_SHORT_MAX + 1];
    char short_oversized[MAP2_PROTOCOL_METADATA_SHORT_MAX + 2];
    memset(long_boundary, 'a', sizeof(long_boundary) - 1);
    long_boundary[sizeof(long_boundary) - 1] = '\0';
    memset(long_oversized, 'a', sizeof(long_oversized) - 1);
    long_oversized[sizeof(long_oversized) - 1] = '\0';
    memset(short_boundary, 'a', sizeof(short_boundary) - 1);
    short_boundary[sizeof(short_boundary) - 1] = '\0';
    memset(short_oversized, 'a', sizeof(short_oversized) - 1);
    short_oversized[sizeof(short_oversized) - 1] = '\0';

    packet_struct *packet = map_protocol_test_new_packet(long_boundary,
                                                         long_boundary,
                                                         short_boundary,
                                                         short_boundary,
                                                         short_boundary,
                                                         long_boundary);
    ck_assert(map_protocol_validate(packet->data, packet->len, 0, 21, 21));
    packet_free(packet);

    const char *valid = "valid";
    for (size_t field = 0; field < 6; field++) {
        const char *values[] = {valid, valid, valid, valid, valid, valid};
        values[field] = field == 0 || field == 1 || field == 5 ? long_oversized : short_oversized;
        packet = map_protocol_test_new_packet(values[0],
                                              values[1],
                                              values[2],
                                              values[3],
                                              values[4],
                                              values[5]);
        ck_assert(!map_protocol_validate(packet->data, packet->len, 0, 21, 21));
        packet_free(packet);
    }
}
END_TEST

START_TEST(test_packet_dup) {
    packet_struct *packet, *packet2;

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, 50);
    packet_verify_data(packet, "00000032");
    packet2 = packet_dup(packet);
    packet_verify_data(packet2, "00000032");
    packet_free(packet);
    packet_free(packet2);

    packet = packet_new(0, 0, 0);
    packet_verify_data(packet, "");
    packet2 = packet_dup(packet);
    packet_verify_data(packet2, "");
    packet_free(packet);
    packet_free(packet2);
}
END_TEST

START_TEST(test_packet_writer_mark) {
    packet_struct *packet;
    packet_writer_mark_t packet_save_buf;

    packet = packet_new(0, 0, 0);
    packet_writer_mark(packet, &packet_save_buf);
    ck_assert_uint_eq(packet_save_buf.pos, 0);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_mark(packet, &packet_save_buf);
    ck_assert_uint_eq(packet_save_buf.pos, 4);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_mark(packet, &packet_save_buf);
    ck_assert_uint_eq(packet_save_buf.pos, 4);
    packet_writer_write_uint32(packet, 42);
    ck_assert_uint_eq(packet_save_buf.pos, 4);
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_rollback) {
    packet_struct *packet;
    packet_writer_mark_t packet_save_buf;

    packet = packet_new(0, 0, 0);
    packet_writer_mark(packet, &packet_save_buf);
    packet_writer_rollback(packet, &packet_save_buf);
    ck_assert_uint_eq(packet->len, 0);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_mark(packet, &packet_save_buf);
    packet_writer_rollback(packet, &packet_save_buf);
    ck_assert_uint_eq(packet->len, 4);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_mark(packet, &packet_save_buf);
    packet_writer_rollback(packet, &packet_save_buf);
    ck_assert_uint_eq(packet->len, 4);
    packet_writer_write_uint32(packet, 42);
    ck_assert_uint_eq(packet->len, 8);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_writer_mark(packet, &packet_save_buf);
    packet_writer_write_uint32(packet, 42);
    packet_writer_rollback(packet, &packet_save_buf);
    ck_assert_uint_eq(packet->len, 4);
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_uint8) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 0);
    packet_verify_data(packet, "00");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, 42);
    packet_verify_data(packet, "2A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint8(packet, UINT8_MAX);
    packet_verify_data(packet, "FF");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_int8) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_int8(packet, 0);
    packet_verify_data(packet, "00");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int8(packet, 42);
    packet_verify_data(packet, "2A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int8(packet, -42);
    packet_verify_data(packet, "D6");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int8(packet, INT8_MAX);
    packet_verify_data(packet, "7F");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int8(packet, INT8_MIN);
    packet_verify_data(packet, "80");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_uint16) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint16(packet, 0);
    packet_verify_data(packet, "0000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint16(packet, 42);
    packet_verify_data(packet, "002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint16(packet, UINT16_MAX);
    packet_verify_data(packet, "FFFF");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_int16) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_int16(packet, 0);
    packet_verify_data(packet, "0000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int16(packet, 42);
    packet_verify_data(packet, "002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int16(packet, -42);
    packet_verify_data(packet, "FFD6");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int16(packet, INT16_MAX);
    packet_verify_data(packet, "7FFF");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int16(packet, INT16_MIN);
    packet_verify_data(packet, "8000");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_uint32) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 0);
    packet_verify_data(packet, "00000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, 42);
    packet_verify_data(packet, "0000002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint32(packet, UINT32_MAX);
    packet_verify_data(packet, "FFFFFFFF");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_int32) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, 0);
    packet_verify_data(packet, "00000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, 42);
    packet_verify_data(packet, "0000002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, -42);
    packet_verify_data(packet, "FFFFFFD6");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, INT32_MAX);
    packet_verify_data(packet, "7FFFFFFF");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int32(packet, INT32_MIN);
    packet_verify_data(packet, "80000000");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_uint64) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint64(packet, 0);
    packet_verify_data(packet, "0000000000000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint64(packet, 42);
    packet_verify_data(packet, "000000000000002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_uint64(packet, UINT64_MAX);
    packet_verify_data(packet, "FFFFFFFFFFFFFFFF");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_int64) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_int64(packet, 0);
    packet_verify_data(packet, "0000000000000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int64(packet, 42);
    packet_verify_data(packet, "000000000000002A");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int64(packet, -42);
    packet_verify_data(packet, "FFFFFFFFFFFFFFD6");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int64(packet, INT64_MAX);
    packet_verify_data(packet, "7FFFFFFFFFFFFFFF");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_int64(packet, INT64_MIN);
    packet_verify_data(packet, "8000000000000000");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_float) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_float(packet, 1.0);
    packet_verify_data(packet, "3F800000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_float(packet, 0.0001);
    packet_verify_data(packet, "38D1B717");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_float(packet, -10.0);
    packet_verify_data(packet, "C1200000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_float(packet, 12345678.);
    packet_verify_data(packet, "4B3C614E");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_float(packet, -109.56);
    packet_verify_data(packet, "C2DB1EB8");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_double) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_double(packet, 1.0);
    packet_verify_data(packet, "3FF0000000000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_double(packet, 0.0001);
    packet_verify_data(packet, "3F1A36E2EB1C432D");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_double(packet, -10.0);
    packet_verify_data(packet, "C024000000000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_double(packet, 123456789);
    packet_verify_data(packet, "419D6F3454000000");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_double(packet, -109.56);
    packet_verify_data(packet, "C05B63D70A3D70A4");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_bytes) {
    packet_struct *packet;
    const uint8_t data[] = {0xff, 0x03, 0x00};

    packet = packet_new(0, 0, 0);
    packet_writer_write_bytes(packet, data, 0);
    packet_verify_data(packet, "");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_bytes(packet, data, 1);
    packet_verify_data(packet, "FF");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_bytes(packet, data, 3);
    packet_verify_data(packet, "FF0300");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_string_n) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_string_n(packet, "", 0);
    packet_verify_data(packet, "");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_string_n(packet, "test", 4);
    packet_verify_data(packet, "74657374");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_string_n(packet, "test", 2);
    packet_verify_data(packet, "7465");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_string) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_string(packet, "");
    packet_verify_data(packet, "");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_string(packet, "test");
    packet_verify_data(packet, "74657374");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_cstring_n) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_cstring_n(packet, "", 0);
    packet_verify_data(packet, "00");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_cstring_n(packet, "test", 2);
    packet_verify_data(packet, "746500");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_cstring) {
    packet_struct *packet;

    packet = packet_new(0, 0, 0);
    packet_writer_write_cstring(packet, "");
    packet_verify_data(packet, "00");
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    packet_writer_write_cstring(packet, "test");
    packet_verify_data(packet, "7465737400");
    packet_free(packet);
}
END_TEST

START_TEST(test_packet_writer_write_packet) {
    packet_struct *packet, *packet2;

    packet = packet_new(0, 0, 0);
    packet2 = packet_new(0, 0, 0);
    packet_verify_data(packet, "");
    packet_verify_data(packet2, "");
    packet_writer_write_packet(packet, packet2);
    packet_verify_data(packet, "");
    packet_verify_data(packet2, "");
    packet_free(packet);
    packet_free(packet2);

    packet = packet_new(0, 0, 0);
    packet2 = packet_new(0, 0, 0);
    packet_writer_write_cstring(packet2, "test");
    packet_verify_data(packet, "");
    packet_verify_data(packet2, "7465737400");
    packet_writer_write_packet(packet, packet2);
    packet_verify_data(packet, "7465737400");
    packet_verify_data(packet2, "7465737400");
    packet_free(packet);
    packet_free(packet2);

    packet = packet_new(0, 512, 512);
    packet2 = packet_new(0, PACKET_PAYLOAD_MAX - 5, 0);
    uint8_t *bytes = xcalloc(PACKET_PAYLOAD_MAX - 5, 1);
    packet_writer_write_bytes(packet2, bytes, PACKET_PAYLOAD_MAX - 5);
    free(bytes);
    for (size_t i = 0; i < 5; i++) {
        packet_writer_write_uint8(packet, 0);
    }
    packet_writer_write_packet(packet, packet2);
    ck_assert(packet_writer_finish(packet));
    ck_assert_uint_eq(packet->len, PACKET_PAYLOAD_MAX);
    packet_free(packet);
    packet_free(packet2);
}
END_TEST

START_TEST(test_packet_reader_scalars_round_trip) {
    const uint8_t data[] = {0x7f, 0x80, 0x12, 0x34, 0xff, 0xfe, 0x89, 0xab, 0xcd, 0xef, 0xff,
                            0xff, 0xff, 0xfe, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    packet_reader_t reader;
    packet_reader_init(&reader, data, sizeof(data));

    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 0x7f);
    ck_assert_int_eq(packet_reader_read_int8(&reader), INT8_MIN);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 0x1234);
    ck_assert_int_eq(packet_reader_read_int16(&reader), -2);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), UINT32_C(0x89abcdef));
    ck_assert_int_eq(packet_reader_read_int32(&reader), -2);
    ck_assert_uint_eq(packet_reader_read_uint64(&reader), UINT64_C(0x0123456789abcdef));
    ck_assert(packet_reader_finish(&reader));
}
END_TEST

START_TEST(test_packet_reader_truncation_is_sticky) {
    for (size_t len = 0; len < sizeof(uint64_t); len++) {
        uint8_t data[sizeof(uint64_t)] = {0};
        packet_reader_t reader;
        packet_reader_init(&reader, data, len);

        ck_assert_uint_eq(packet_reader_read_uint64(&reader), 0);
        ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_TRUNCATED);
        size_t failed_pos = reader.pos;
        ck_assert_uint_eq(packet_reader_read_uint8(&reader), 0);
        ck_assert_uint_eq(reader.pos, failed_pos);
        ck_assert(!packet_reader_finish(&reader));
    }
}
END_TEST

START_TEST(test_packet_reader_strings_views_and_limits) {
    const uint8_t data[] = {'h', 'e', 'l', 'l', 'o', '\0', 0x2a};
    packet_reader_t reader;
    packet_reader_init(&reader, data, sizeof(data));
    packet_view_t view = packet_reader_read_string_view(&reader, 5);

    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_NONE);
    ck_assert_uint_eq(view.len, 5);
    ck_assert_int_eq(memcmp(view.data, "hello", view.len), 0);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 42);
    ck_assert(packet_reader_finish(&reader));

    char dest[5] = "keep";
    packet_reader_init(&reader, data, sizeof(data));
    ck_assert(!packet_reader_read_string_bounded(&reader, VS(dest), 5));
    ck_assert_str_eq(dest, "keep");
    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_LIMIT_EXCEEDED);

    packet_reader_init(&reader, data, sizeof(data));
    (void)packet_reader_read_string_view(&reader, 4);
    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_LIMIT_EXCEEDED);

    packet_reader_init(&reader, "hello", 5);
    (void)packet_reader_read_string_view(&reader, 5);
    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_TRUNCATED);
}
END_TEST

START_TEST(test_packet_reader_finish_and_counts) {
    const uint8_t data[] = {3, 0xff};
    packet_reader_t reader;
    size_t count = 0;
    packet_reader_init(&reader, data, sizeof(data));

    ck_assert(packet_reader_read_count8(&reader, 3, &count));
    ck_assert_uint_eq(count, 3);
    ck_assert(!packet_reader_finish(&reader));
    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_TRAILING_DATA);

    packet_reader_init(&reader, data, sizeof(data));
    ck_assert(!packet_reader_read_count8(&reader, 2, &count));
    ck_assert_int_eq(packet_reader_error(&reader), PACKET_ERROR_LIMIT_EXCEEDED);
}
END_TEST

START_TEST(test_packet_writer_limit_is_sticky) {
    packet_writer_t *writer = packet_new(0, 0, 0);
    packet_writer_set_limit(writer, 2);

    packet_writer_write_uint16(writer, UINT16_C(0x1234));
    ck_assert(packet_writer_finish(writer));
    ck_assert_uint_eq(writer->len, 2);
    packet_writer_write_uint8(writer, 1);
    ck_assert(!packet_writer_finish(writer));
    ck_assert_int_eq(packet_writer_error(writer), PACKET_ERROR_LIMIT_EXCEEDED);
    ck_assert_uint_eq(writer->len, 2);
    packet_writer_write_uint8(writer, 2);
    ck_assert_uint_eq(writer->len, 2);
    packet_free(writer);
}
END_TEST

START_TEST(test_packet_writer_strings_fail_atomically) {
    packet_writer_t *writer = packet_new(0, 0, 0);
    writer->limit = 4;

    packet_writer_write_string_n(writer, "oversized", 9);
    ck_assert_int_eq(packet_writer_error(writer), PACKET_ERROR_LIMIT_EXCEEDED);
    ck_assert_uint_eq(writer->len, 0);
    packet_free(writer);

    writer = packet_new(0, 0, 0);
    writer->limit = 4;
    packet_writer_write_cstring_n(writer, "four", 4);
    ck_assert_int_eq(packet_writer_error(writer), PACKET_ERROR_LIMIT_EXCEEDED);
    ck_assert_uint_eq(writer->len, 0);
    packet_free(writer);
}
END_TEST

START_TEST(test_packet_delete_moves_only_trailing_bytes) {
    packet_writer_t *writer = packet_new(0, 0, 0);
    packet_writer_write_bytes(writer, (const uint8_t *)"0123456789", 10);

    packet_delete(writer, 3, 2);
    ck_assert_uint_eq(writer->len, 8);
    ck_assert_mem_eq(writer->data, "01256789", 8);

    packet_delete(writer, 6, 2);
    ck_assert_uint_eq(writer->len, 6);
    ck_assert_mem_eq(writer->data, "012567", 6);
    packet_free(writer);
}
END_TEST

START_TEST(test_packet_reader_scope_tracks_completion_and_errors) {
    const uint8_t data[] = {1, 2};
    packet_reader_scope_t scope;
    packet_reader_t reader;

    packet_reader_scope_begin(&scope);
    packet_reader_init(&reader, data, sizeof(data));
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);
    ck_assert_int_eq(packet_reader_scope_finish(&scope), PACKET_ERROR_TRAILING_DATA);

    packet_reader_scope_begin(&scope);
    packet_reader_init(&reader, data, 1);
    (void)packet_reader_read_uint16(&reader);
    ck_assert_int_eq(packet_reader_scope_finish(&scope), PACKET_ERROR_TRUNCATED);

    packet_reader_scope_begin(&scope);
    packet_reader_init(&reader, data, sizeof(data));
    (void)packet_reader_read_uint16(&reader);
    ck_assert_int_eq(packet_reader_scope_finish(&scope), PACKET_ERROR_NONE);

    packet_reader_scope_begin(&scope);
    packet_reader_init(&reader, data, sizeof(data));
    packet_reader_t nested;
    packet_reader_init(&nested, data + 1, 1);
    (void)packet_reader_read_uint16(&reader);
    ck_assert_int_eq(packet_reader_scope_finish(&scope), PACKET_ERROR_NONE);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("packet");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_packet_new);
    tcase_add_test(tc_core, test_packet_dup);
    tcase_add_test(tc_core, test_packet_writer_mark);
    tcase_add_test(tc_core, test_packet_writer_rollback);
    tcase_add_test(tc_core, test_packet_writer_write_uint8);
    tcase_add_test(tc_core, test_packet_writer_write_int8);
    tcase_add_test(tc_core, test_packet_writer_write_uint16);
    tcase_add_test(tc_core, test_packet_writer_write_int16);
    tcase_add_test(tc_core, test_packet_writer_write_uint32);
    tcase_add_test(tc_core, test_packet_writer_write_int32);
    tcase_add_test(tc_core, test_packet_writer_write_uint64);
    tcase_add_test(tc_core, test_packet_writer_write_int64);
    tcase_add_test(tc_core, test_packet_writer_write_float);
    tcase_add_test(tc_core, test_packet_writer_write_double);
    tcase_add_test(tc_core, test_packet_writer_write_bytes);
    tcase_add_test(tc_core, test_packet_writer_write_string_n);
    tcase_add_test(tc_core, test_packet_writer_write_string);
    tcase_add_test(tc_core, test_packet_writer_write_cstring_n);
    tcase_add_test(tc_core, test_packet_writer_write_cstring);
    tcase_add_test(tc_core, test_packet_writer_write_packet);
    tcase_add_test(tc_core, test_packet_reader_scalars_round_trip);
    tcase_add_test(tc_core, test_packet_reader_truncation_is_sticky);
    tcase_add_test(tc_core, test_packet_reader_strings_views_and_limits);
    tcase_add_test(tc_core, test_packet_reader_finish_and_counts);
    tcase_add_test(tc_core, test_packet_writer_limit_is_sticky);
    tcase_add_test(tc_core, test_packet_writer_strings_fail_atomically);
    tcase_add_test(tc_core, test_packet_delete_moves_only_trailing_bytes);
    tcase_add_test(tc_core, test_packet_reader_scope_tracks_completion_and_errors);
    tcase_add_test(tc_core, test_map_protocol_validate_minimal_and_truncation);
    tcase_add_test(tc_core, test_map_protocol_rejects_duplicate_depth);
    tcase_add_test(tc_core, test_map_protocol_enforces_level_framing);
    tcase_add_test(tc_core, test_map_protocol_rejects_bad_tile_and_layer_indices);
    tcase_add_test(tc_core, test_map_protocol_validates_tile_record_flags);
    tcase_add_test(tc_core,
                   test_map_protocol_accepts_exit_flag_and_rejects_unknown_or_truncated_flags);
    tcase_add_test(tc_core, test_map_protocol_enforces_layer_string_bounds);
    tcase_add_test(tc_core, test_map_protocol_validates_colored_light_extension);
    tcase_add_test(tc_core, test_map_protocol_validates_timed_light_keyframes);
    tcase_add_test(tc_core, test_map_protocol_accepts_bounded_continuation);
    tcase_add_test(tc_core, test_map_protocol_continuation_state_is_bounded_and_ordered);
    tcase_add_test(tc_core, test_map_protocol_rejects_unterminated_metadata);
    tcase_add_test(tc_core, test_map_protocol_enforces_metadata_string_bounds);
    tcase_add_test(tc_core, test_map_protocol_validates_client_lighting_fields);

    return s;
}

void check_server_packet(void) {
    check_run_suite(suite(), __FILE__);
}
