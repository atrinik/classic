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

/** @file Bounded validation for on-demand PNG face assets. */

#include <face_asset.h>
#include <toolkit/socket.h>
#include <string.h>
#include <zlib.h>

static uint32_t face_asset_uint32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
           bytes[3];
}

bool face_asset_validate(const uint8_t *data, size_t size, uint32_t expected_crc32) {
    static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (data == NULL || size < 24U || size > ASSET_FACE_MAX_SIZE ||
        memcmp(data, png_signature, sizeof(png_signature)) != 0 ||
        memcmp(data + 12U, "IHDR", 4U) != 0 || face_asset_uint32(data + 8U) != 13U) {
        return false;
    }

    uint32_t width = face_asset_uint32(data + 16U);
    uint32_t height = face_asset_uint32(data + 20U);
    if (width == 0 || height == 0 || width > FACE_ASSET_DIMENSION_MAX ||
        height > FACE_ASSET_DIMENSION_MAX || (uint64_t)width * height > FACE_ASSET_PIXELS_MAX) {
        return false;
    }

    return (uint32_t)crc32(1L, data, size) == expected_crc32;
}
