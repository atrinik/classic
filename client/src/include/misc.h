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
 * Misc definitions.
 */

#ifndef MISC_H
#define MISC_H

#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#define MAX_INPUT_STR 256

/** Public API implemented in src/client/misc.c. */

extern void browser_open(const char *url);

extern char *package_get_version_full(char *dst, size_t dstlen);

extern char *package_get_version_partial(char *dst, size_t dstlen);

extern void screenshot_create(const SDL_Rect *rect);

/** Monotonic clock used by visual UI state. */
extern uint32_t client_ui_ticks(void);

#ifdef ATRINIK_WIDGET_TESTS
/** Freeze every visual UI animation at a deterministic fixture time. */
extern void client_ui_test_clock_set(uint32_t ticks);
/** Restore the production monotonic UI clock. */
extern void client_ui_test_clock_reset(void);
/** Redirect the next player-facing screenshot completion to the fixture. */
extern void screenshot_test_begin(void);
/** Take ownership of the completed player-facing screenshot, if ready. */
extern SDL_Surface *screenshot_test_take(void);
#endif

#endif
