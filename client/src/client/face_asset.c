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

#define FACE_ASSET_COMPRESSED_METADATA_MAX 256U
#define FACE_ASSET_COMPRESSED_METADATA_CHUNKS_MAX 4U

static uint32_t face_asset_uint32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
           bytes[3];
}

static bool face_asset_png_structure_valid(const uint8_t *data, size_t size) {
    bool saw_idat = false;
    size_t compressed_metadata_size = 0;
    unsigned int compressed_metadata_chunks = 0;
    size_t offset = 8U;
    while (offset <= size && size - offset >= 12U) {
        uint32_t chunk_size = face_asset_uint32(data + offset);
        uint64_t chunk_end = (uint64_t)offset + 12U + chunk_size;
        if (chunk_end > size) {
            return false;
        }

        const uint8_t *type = data + offset + 4U;
        for (size_t i = 0; i < 4U; i++) {
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z'))) {
                return false;
            }
        }
        /* PNG reserves the third type-code bit for future use. */
        if (type[2] < 'A' || type[2] > 'Z') {
            return false;
        }

        uint32_t expected_crc = face_asset_uint32(data + offset + 8U + chunk_size);
        uint32_t actual_crc = (uint32_t)crc32(0L, type, (uInt)((uint64_t)chunk_size + 4U));
        if (actual_crc != expected_crc) {
            return false;
        }

        /*
         * Bound input that SDL_image/libpng may inflate while parsing metadata.
         * The face format does not consume this metadata, but existing content
         * contains small instances of all three chunk types.
         */
        if (memcmp(type, "zTXt", 4U) == 0 || memcmp(type, "iTXt", 4U) == 0 ||
            memcmp(type, "iCCP", 4U) == 0) {
            compressed_metadata_chunks++;
            compressed_metadata_size += chunk_size;
            if (compressed_metadata_chunks > FACE_ASSET_COMPRESSED_METADATA_CHUNKS_MAX ||
                compressed_metadata_size > FACE_ASSET_COMPRESSED_METADATA_MAX) {
                return false;
            }
        }

        if (offset == 8U && (chunk_size != 13U || memcmp(type, "IHDR", 4U) != 0)) {
            return false;
        }
        if (memcmp(type, "IDAT", 4U) == 0) {
            saw_idat = true;
        } else if (memcmp(type, "IEND", 4U) == 0) {
            return chunk_size == 0 && saw_idat && chunk_end == size;
        } else if (offset != 8U && memcmp(type, "IHDR", 4U) == 0) {
            return false;
        }
        offset = (size_t)chunk_end;
    }
    return false;
}

bool face_asset_validate(const uint8_t *data, size_t size, uint32_t expected_crc32) {
    static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (data == NULL || size < 57U || size > ASSET_FACE_MAX_SIZE ||
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

    return face_asset_png_structure_valid(data, size) &&
           (uint32_t)crc32(1L, data, size) == expected_crc32;
}
