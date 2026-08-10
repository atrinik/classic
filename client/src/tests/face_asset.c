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

static void chunk_crc_write(uint8_t *chunk) {
    uint32_t size = ((uint32_t)chunk[0] << 24U) | ((uint32_t)chunk[1] << 16U) |
                    ((uint32_t)chunk[2] << 8U) | chunk[3];
    uint32_write(chunk + 8U + size, (uint32_t)crc32(0L, chunk + 4U, size + 4U));
}

int main(void) {
    uint8_t png[57] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint32_write(png + 8U, 13U);
    memcpy(png + 12U, "IHDR", 4U);
    uint32_write(png + 16U, 48U);
    uint32_write(png + 20U, 48U);
    png[24U] = 8U;
    png[25U] = 6U;
    memcpy(png + 37U, "IDAT", 4U);
    memcpy(png + 49U, "IEND", 4U);
    chunk_crc_write(png + 8U);
    chunk_crc_write(png + 33U);
    chunk_crc_write(png + 45U);
    uint32_t checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(face_asset_validate(png, sizeof(png), checksum));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum + 1U));
    TEST_CHECK(!face_asset_validate(png, sizeof(png) - 1U, checksum));

    enum {
        METADATA_SIZE = 257U
    };
    uint8_t metadata_png[sizeof(png) + 12U + METADATA_SIZE];
    memcpy(metadata_png, png, 45U);
    uint8_t *metadata_chunk = metadata_png + 45U;
    uint32_write(metadata_chunk, METADATA_SIZE);
    memcpy(metadata_chunk + 4U, "zTXt", 4U);
    metadata_chunk[8U] = 'x';
    metadata_chunk[9U] = '\0';
    metadata_chunk[10U] = 0;
    memset(metadata_chunk + 11U, 0, METADATA_SIZE - 3U);
    chunk_crc_write(metadata_chunk);
    memcpy(metadata_chunk + 12U + METADATA_SIZE, png + 45U, 12U);
    checksum = (uint32_t)crc32(1L, metadata_png, sizeof(metadata_png));
    TEST_CHECK(!face_asset_validate(metadata_png, sizeof(metadata_png), checksum));

    png[0] = 0;
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));
    png[0] = 0x89;
    uint32_write(png + 16U, FACE_ASSET_DIMENSION_MAX + 1U);
    chunk_crc_write(png + 8U);
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));
    uint32_write(png + 16U, 1U);
    uint32_write(png + 20U, 0U);
    chunk_crc_write(png + 8U);
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));

    uint32_write(png + 20U, 1U);
    chunk_crc_write(png + 8U);
    png[41U] ^= 0x80U;
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));
    png[41U] ^= 0x80U;
    uint32_write(png + 33U, 1U);
    checksum = (uint32_t)crc32(1L, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(png, sizeof(png), checksum));

    uint8_t *oversized = calloc(ASSET_FACE_MAX_SIZE + 1U, 1U);
    TEST_CHECK(oversized != NULL);
    memcpy(oversized, png, sizeof(png));
    TEST_CHECK(!face_asset_validate(oversized, ASSET_FACE_MAX_SIZE + 1U, 0));
    free(oversized);
    return 0;
}
