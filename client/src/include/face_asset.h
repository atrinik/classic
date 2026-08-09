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

#ifndef FACE_ASSET_H
#define FACE_ASSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum decoded face dimension accepted before SDL_image is invoked. */
#define FACE_ASSET_DIMENSION_MAX 512U
/** Maximum decoded face pixel count accepted before SDL_image is invoked. */
#define FACE_ASSET_PIXELS_MAX (512U * 512U)

bool face_asset_validate(const uint8_t *data, size_t size, uint32_t expected_crc32);

#endif
