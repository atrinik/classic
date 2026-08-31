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
 * Player stuck-recovery command and lifecycle API.
 */

#ifndef STUCK_H
#define STUCK_H

#include <stdbool.h>

#include <decls.h>

/** Seconds a player must remain out of combat before recovery. */
#define PLAYER_STUCK_COUNTDOWN_SECONDS 10U

/** Seconds before the player may request another recovery. */
#define PLAYER_STUCK_COOLDOWN_SECONDS (5U * 60U)

/**
 * Process the active recovery countdown.
 *
 * @return
 * True if the caller may continue processing the player this tick; false if
 * map transfer was attempted or the player object was destroyed.
 */
bool player_stuck_process(object *op);
/** Process all active recovery requests after the current object tick. */
void player_stuck_process_all(void);
void player_stuck_cancel(object *op);

#ifdef ATRINIK_TESTING
/** Reset the test observation of active-countdown cancellation. */
void player_stuck_cancel_observation_reset_for_test(void);
/** Whether a test has observed an active countdown being cancelled. */
bool player_stuck_cancel_observed_active_for_test(void);
#endif

#endif
