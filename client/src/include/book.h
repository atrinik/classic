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
 * Book GUI header.
 */

#ifndef BOOK_H
#define BOOK_H

#include <stdbool.h>
#include <stddef.h>

#include <toolkit/shstr.h>

/**
 * @defgroup BOOK_TEXT_xxx Book text coords
 * Book text coordinates.
 *@{*/
/** X position of the text. */
#define BOOK_TEXT_STARTX 27
/** Y position of the text. */
#define BOOK_TEXT_STARTY 58
/** Width of the text. */
#define BOOK_TEXT_WIDTH 621
/** Height of the text. */
#define BOOK_TEXT_HEIGHT 365
/*@}*/

/**
 * @defgroup BOOK_TITLE_xxx Book title coords
 * Book title coordinates.
 *@{*/
/** X position of the title. */
#define BOOK_TITLE_STARTX 63
/** Y position of the title. */
#define BOOK_TITLE_STARTY 27
/** Width of the title. */
#define BOOK_TITLE_WIDTH 573
/** Height of the title. */
#define BOOK_TITLE_HEIGHT 22
/*@}*/

/**
 * @defgroup BOOK_SCROLLBAR_xxx Book scrollbar coords
 * Book scrollbar coordinates.
 *@{*/
/** X position of the scrollbar. */
#define BOOK_SCROLLBAR_STARTX 660
/** Y position of the scrollbar. */
#define BOOK_SCROLLBAR_STARTY 56
/** Width of the scrollbar. */
#define BOOK_SCROLLBAR_WIDTH 15
/** Height of the scrollbar. */
#define BOOK_SCROLLBAR_HEIGHT 369
/*@}*/

/** Public API implemented in src/gui/popups/book.c. */

extern UT_array *book_help_history;

extern void book_name_change(const char *name, size_t len);

/** Load book state and create its popup, returning false on canvas failure. */
extern bool book_load(const char *data, int len);

/** Display separately authenticated signature metadata without interpreting markup. */
extern bool book_load_signed(const char *data, int len, const char *signer, const char *date);

/** Load private access-management output and cleanse it when the popup closes. */
bool book_load_sensitive(const char *data, int len, const char *title);

/** Close and cleanse private access-management output without affecting an ordinary book. */
void book_sensitive_clear(void);

/** Whether private output remains retained, including without a popup. */
bool book_sensitive_active(void);

/** Whether private access-management output is currently visible. */
bool book_sensitive_visible(void);

#ifdef ATRINIK_WIDGET_TESTS
/** Whether book content remains owned after an allocation-failure path. */
extern bool book_test_content_retained(void);
/** Whether the signed reader executed its separate footer rendering path. */
extern bool book_test_signature_rendered(void);

/** Seed retained book state without creating a renderer-owned popup. */
extern bool book_test_state_seed(const char *content, bool sensitive);

/** Whether the last sensitive-state teardown observed a fully cleansed buffer. */
extern bool book_test_clear_was_observed(void);

/** Release state seeded by a non-rendering lifecycle test. */
extern void book_test_state_discard(void);
#endif

extern void book_redraw(void);

extern void book_add_help_history(const char *name);

#endif
