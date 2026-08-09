/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Atrinik Development Team                    *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Sprite pixel visibility and visible-bounds calculations.
 */

#include <global.h>

/** Calculate the transparent padding around a sprite's visible pixels. */
bool sprite_borders_get(SDL_Surface *surface, sprite_struct *sprite) {
    int minimum_x = surface->w;
    int minimum_y = surface->h;
    int maximum_x = -1;
    int maximum_y = -1;
    bool locked = false;

    if (SDL_MUSTLOCK(surface)) {
        if (!SDL_LockSurface(surface)) {
            return false;
        }
        locked = true;
    }

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            if (!surface_pixel_visible(surface, x, y)) {
                continue;
            }

            minimum_x = MIN(minimum_x, x);
            minimum_y = MIN(minimum_y, y);
            maximum_x = MAX(maximum_x, x);
            maximum_y = MAX(maximum_y, y);
        }
    }

    if (locked) {
        SDL_UnlockSurface(surface);
    }

    if (maximum_x < 0) {
        return false;
    }

    sprite->border_up = minimum_y;
    sprite->border_down = surface->h - maximum_y - 1;
    sprite->border_left = minimum_x;
    sprite->border_right = surface->w - maximum_x - 1;
    return true;
}

/** Return whether a source pixel contributes to a sprite's visible silhouette. */
bool surface_pixel_visible(SDL_Surface *surface, int x, int y) {
    if (x < 0 || x >= surface->w || y < 0 || y >= surface->h) {
        return false;
    }

    Uint32 pixel = getpixel(surface, x, y);
    Uint32 color_key;
    if (SDL_GetSurfaceColorKey(surface, &color_key) && pixel == color_key) {
        return false;
    }

    SDL_Palette *palette = SDL_GetSurfacePalette(surface);
    if (palette != NULL) {
        return pixel < (Uint32)palette->ncolors && palette->colors[pixel].a >= 64;
    }

    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    if (details == NULL) {
        return false;
    }

    if (details->Amask != 0) {
        Uint8 red, green, blue, alpha;
        SDL_GetRGBA(pixel, details, NULL, &red, &green, &blue, &alpha);
        return alpha >= 64;
    }

    return true;
}

/** Return the mapped pixel value at a surface coordinate. */
Uint32 getpixel(SDL_Surface *surface, int x, int y) {
    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    HARD_ASSERT(details != NULL);
    int bpp = details->bytes_per_pixel;
    Uint8 *pixel = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

    switch (bpp) {
        case 1:
            return *pixel;

        case 2:
            return *(Uint16 *)pixel;

        case 3:
            if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
                return pixel[0] << 16 | pixel[1] << 8 | pixel[2];
            }
            return pixel[0] | pixel[1] << 8 | pixel[2] << 16;

        case 4:
            return *(Uint32 *)pixel;
    }

    return 0;
}
