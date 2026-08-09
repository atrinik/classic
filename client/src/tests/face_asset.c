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

#include <face_asset.h>
#include <toolkit/socket.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void uint32_write(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

int main(void) {
    uint8_t png[24] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13,
        'I',  'H', 'D', 'R', 0,    0,    0,    48,   0, 0, 0, 48,
    };
    uint32_t checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(face_asset_validate(png, sizeof(png), checksum));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum + 1U));
    TEST_CHECK(!face_asset_validate(png, sizeof(png) - 1U, checksum));

    png[0] = 0;
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));
    png[0] = 0x89;
    uint32_write(png + 16U, FACE_ASSET_DIMENSION_MAX + 1U);
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));
    uint32_write(png + 16U, 1U);
    uint32_write(png + 20U, 0U);
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));

    uint8_t *oversized = calloc(ASSET_FACE_MAX_SIZE + 1U, 1U);
    TEST_CHECK(oversized != NULL);
    memcpy(oversized, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(oversized, ASSET_FACE_MAX_SIZE + 1U, 0));
    free(oversized);
    return 0;
}
