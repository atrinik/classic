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

/**
 * @file
 * Small helpers for querying SDL mouse state.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stddef.h>

#include <SDL3/SDL.h>

static inline SDL_MouseButtonFlags mouse_get_state(int *x, int *y) {
    float mouse_x, mouse_y;
    SDL_MouseButtonFlags state =
        SDL_GetMouseState(x != NULL ? &mouse_x : NULL, y != NULL ? &mouse_y : NULL);

    if (x != NULL) {
        *x = (int)mouse_x;
    }
    if (y != NULL) {
        *y = (int)mouse_y;
    }

    return state;
}

#endif
