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
 * Sprite pixel access, visibility, and visible-bounds calculations.
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
        return pixel < (Uint32)palette->ncolors &&
               palette->colors[pixel].a >= SPRITE_ALPHA_VISIBLE_MIN;
    }

    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    if (details == NULL) {
        return false;
    }

    if (details->Amask != 0) {
        Uint8 red, green, blue, alpha;
        SDL_GetRGBA(pixel, details, NULL, &red, &green, &blue, &alpha);
        return alpha >= SPRITE_ALPHA_VISIBLE_MIN;
    }

    return true;
}

/** Read a mapped surface pixel while honoring SDL's direct-access contract. */
bool surface_pixel_get(SDL_Surface *surface, int x, int y, Uint32 *pixel) {
    if (surface == NULL || pixel == NULL || x < 0 || x >= surface->w || y < 0 || y >= surface->h) {
        return false;
    }

    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    if (details == NULL || details->bytes_per_pixel <= 0) {
        return false;
    }

    bool locked = false;
    if (SDL_MUSTLOCK(surface)) {
        if (!SDL_LockSurface(surface)) {
            return false;
        }
        locked = true;
    }

    bool success = surface->pixels != NULL;
    if (success) {
        *pixel = getpixel(surface, x, y);
    }

    if (locked) {
        SDL_UnlockSurface(surface);
    }

    return success;
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

/** Calculate the left border in a surface relative to a color. */
static bool surface_border_get_left(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int x = 0; x < surface->w; x++) {
        for (int y = 0; y < surface->h; y++) {
            if (getpixel(surface, x, y) != color) {
                *pos = x;
                return true;
            }
        }
    }

    return false;
}

/** Calculate the right border in a surface relative to a color. */
static bool surface_border_get_right(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int x = surface->w - 1; x >= 0; x--) {
        for (int y = 0; y < surface->h; y++) {
            if (getpixel(surface, x, y) != color) {
                *pos = (surface->w - 1) - x;
                return true;
            }
        }
    }

    return false;
}

/** Calculate the top border in a surface relative to a color. */
static bool surface_border_get_top(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            if (getpixel(surface, x, y) != color) {
                *pos = y;
                return true;
            }
        }
    }

    return false;
}

/** Calculate the bottom border in a surface relative to a color. */
static bool surface_border_get_bottom(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int y = surface->h - 1; y >= 0; y--) {
        for (int x = 0; x < surface->w; x++) {
            if (getpixel(surface, x, y) != color) {
                *pos = (surface->h - 1) - y;
                return true;
            }
        }
    }

    return false;
}

/**
 * Get borders from an SDL surface while honoring its direct-access contract.
 *
 * @return 1 if a non-color border was found, 0 if all pixels match, or -1 if
 * the surface cannot be accessed.
 */
int surface_borders_get(SDL_Surface *surface,
                        int *top,
                        int *bottom,
                        int *left,
                        int *right,
                        uint32_t color) {
    if (surface == NULL || top == NULL || bottom == NULL || left == NULL || right == NULL ||
        surface->w <= 0 || surface->h <= 0) {
        return -1;
    }

    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    if (details == NULL || details->bytes_per_pixel <= 0) {
        return -1;
    }

    bool locked = false;
    if (SDL_MUSTLOCK(surface)) {
        if (!SDL_LockSurface(surface)) {
            return -1;
        }
        locked = true;
    }

    if (surface->pixels == NULL) {
        if (locked) {
            SDL_UnlockSurface(surface);
        }
        return -1;
    }

    *top = 0;
    *bottom = 0;
    *left = 0;
    *right = 0;

    if (!surface_border_get_top(surface, top, color)) {
        if (locked) {
            SDL_UnlockSurface(surface);
        }
        return 0;
    }

    surface_border_get_bottom(surface, bottom, color);
    surface_border_get_left(surface, left, color);
    surface_border_get_right(surface, right, color);

    if (locked) {
        SDL_UnlockSurface(surface);
    }

    return 1;
}

/** Calculate markup-icon borders using a surface color key or its first pixel. */
bool surface_texture_borders_get(SDL_Surface *surface,
                                 int *top,
                                 int *bottom,
                                 int *left,
                                 int *right) {
    Uint32 color_key;
    if (surface == NULL) {
        return false;
    }

    if (!SDL_GetSurfaceColorKey(surface, &color_key) &&
        !surface_pixel_get(surface, 0, 0, &color_key)) {
        return false;
    }

    return surface_borders_get(surface, top, bottom, left, right, color_key) >= 0;
}
