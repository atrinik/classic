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

/** Status key used by the transient recovery effect. */
#define PLAYER_STUCK_STATUS_KEY "command:stuck"

/** Whether the object is the active /stuck recovery effect. */
bool player_stuck_effect(const object *op);

/** Process an expired /stuck recovery effect. */
void player_stuck_process_effect(object *op);

/** Remove every active /stuck recovery effect from a player. */
bool player_stuck_cancel(object *op);

#endif
