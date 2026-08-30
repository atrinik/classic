/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <text.h>
#include <toolkit/toolkit.h>


/**
 * Parse the given string as an HTML notation color, and store the opaque
 * color in 'color'.
 * @param color_notation
 * The HTML notation to parse.
 * @param color
 * Where the color will be stored.
 * @return
 * 1 if the notation was parsed successfully, 0 otherwise.
 */
int text_color_parse(const char *color_notation, SDL_Color *color) {
    unsigned int r, g, b;

    if (*color_notation == '#') {
        color_notation++;
    }

    if (sscanf(color_notation, "%2X%2X%2X", &r, &g, &b) == 3) {
        if (color) {
            color->r = r;
            color->g = g;
            color->b = b;
            color->a = SDL_ALPHA_OPAQUE;
        }

        return 1;
    }

    return 0;
}
