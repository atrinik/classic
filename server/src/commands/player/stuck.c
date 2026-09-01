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

/** @file Implements the /stuck player command. */

#include <global.h>
#include <gameplay_journal.h>
#include <arch.h>
#include <map.h>
#include <object.h>
#include <player.h>
#include <player_status.h>
#include <server.h>
#include <server_clock.h>
#include <stuck.h>

static bool stuck_cooldown_remaining(const player *pl, server_duration_t *remaining) {
    HARD_ASSERT(pl != NULL);
    HARD_ASSERT(remaining != NULL);

    if (pl->stuck_cooldown.seconds == 0) {
        remaining->microseconds = 0;
        return false;
    }

    /* Negative values and INT64_MAX are fail-closed markers. Neither should
     * be treated as an expired deadline, including when the wall clock itself
     * reaches INT64_MAX. */
    if (pl->stuck_cooldown.seconds < 0 || pl->stuck_cooldown.seconds == INT64_MAX) {
        *remaining = server_duration_from_seconds(PLAYER_STUCK_COOLDOWN_SECONDS);
        return true;
    }

    return server_wall_utc_remaining(pl->stuck_cooldown,
                                     server_wall_utc_now(),
                                     server_duration_from_seconds(PLAYER_STUCK_COOLDOWN_SECONDS),
                                     remaining);
}

static server_wall_utc_t stuck_cooldown_deadline(server_wall_utc_t now) {
    const int64_t seconds = PLAYER_STUCK_COOLDOWN_SECONDS;
    server_wall_utc_t deadline;

    deadline.seconds = now.seconds > INT64_MAX - seconds ? INT64_MAX : now.seconds + seconds;
    return deadline;
}

static uint64_t stuck_remaining_seconds(server_duration_t remaining) {
    const uint64_t second = server_duration_from_seconds(1).microseconds;

    return remaining.microseconds / second + (remaining.microseconds % second != 0);
}

static bool stuck_currently_in_combat(const object *op) {
    return OBJECT_VALID(op->enemy, op->enemy_count) ||
           OBJECT_VALID(op->attacked_by, op->attacked_by_count);
}

bool player_stuck_effect(const object *op) {
    const char *status_key;

    if (op == NULL || op->type != WORD_OF_RECALL) {
        return false;
    }

    status_key = object_get_value(op, "player_status_key");
    return status_key != NULL && strcmp(status_key, PLAYER_STUCK_STATUS_KEY) == 0;
}

static object *player_stuck_find(object *op) {
    for (object *effect = op->inv; effect != NULL; effect = effect->below) {
        if (player_stuck_effect(effect)) {
            return effect;
        }
    }
    return NULL;
}

bool player_stuck_cancel(object *op) {
    if (op == NULL || op->type != PLAYER || CONTR(op) == NULL) {
        return false;
    }

    bool cancelled = false;
    for (object *effect = op->inv, *next; effect != NULL; effect = next) {
        next = effect->below;
        if (!player_stuck_effect(effect)) {
            continue;
        }

        cancelled = true;
        object_remove(effect, 0);
        object_destroy(effect);
    }
    return cancelled;
}

void player_stuck_process_effect(object *effect) {
    HARD_ASSERT(effect != NULL);

    object *op = effect->env;
    if (op == NULL || op->type != PLAYER || CONTR(op) == NULL || CONTR(op)->cs == NULL ||
        CONTR(op)->cs->state != ST_PLAYING || !OBJECT_ACTIVE(op)) {
        if (!QUERY_FLAG(effect, FLAG_REMOVED)) {
            object_remove(effect, 0);
        }
        object_destroy(effect);
        return;
    }

    /* The effect has done its job. Remove it before transferring the player so
     * combat or a map callback cannot observe stale recovery state. */
    object_remove(effect, 0);
    object_destroy(effect);

    tag_t object_count = op->count;
    if (!object_enter_map(op, NULL, NULL, 0, 0, false)) {
        if (OBJECT_DESTROYED(op, object_count)) {
            return;
        }
        draw_info(COLOR_RED, op, "The safe recovery location is unavailable; you were not moved.");
        return;
    }

    if (!OBJECT_DESTROYED(op, object_count)) {
        draw_info(COLOR_WHITE, op, "You have been moved to the safe recovery location.");
    }
}

/** @copydoc command_func */
void command_stuck(object *op, const char *command, char *params) {
    (void)command;

    player *pl = CONTR(op);
    params = player_sanitize_input(params);
    if (params != NULL) {
        draw_info(COLOR_WHITE, op, "Usage: /stuck");
        return;
    }

    if (player_stuck_find(op) != NULL) {
        draw_info(COLOR_WHITE, op, "Your stuck recovery is already counting down.");
        return;
    }

    server_duration_t remaining;
    if (stuck_cooldown_remaining(pl, &remaining)) {
        uint64_t seconds = stuck_remaining_seconds(remaining);
        draw_info_format(COLOR_WHITE,
                         op,
                         "You must wait %" PRIu64 " more second%s before using /stuck again.",
                         seconds,
                         seconds == 1 ? "" : "s");
        return;
    }

    /* Celestial unique saves have a multi-file commit point after player.dat
     * is renamed, so do not arm a cooldown on a path whose failure could
     * leave the durable record ambiguous. */
    if (op->map == NULL || MAP_PLAYER_NO_SAVE(op->map) || MAP_UNIQUE(op->map)) {
        draw_info(
            COLOR_RED,
            op,
            "You cannot use /stuck in this location because your cooldown cannot be saved safely.");
        return;
    }

    if (!gameplay_journal_player_checkpoint_allowed(op)) {
        draw_info(COLOR_RED,
                  op,
                  "You cannot begin stuck recovery while a save transaction is pending.");
        return;
    }

    if (stuck_currently_in_combat(op)) {
        draw_info(COLOR_WHITE, op, "You cannot begin stuck recovery while in combat.");
        return;
    }

    object *effect = arch_get("force");
    if (effect == NULL) {
        draw_info(COLOR_RED, op, "Stuck recovery is temporarily unavailable.");
        return;
    }

    effect->type = WORD_OF_RECALL;
    effect->speed = 1.0 / ((double)PLAYER_STUCK_COUNTDOWN_SECONDS * MAX_TICKS);
    object_update_speed(effect);
    effect->speed_left = -1.0;
    SET_FLAG(effect, FLAG_NO_SAVE);
    if (!player_status_set(effect,
                           PLAYER_STUCK_STATUS_KEY,
                           "stuck recovery",
                           "You will be moved to safety when this effect expires.",
                           effect->face)) {
        object_destroy(effect);
        draw_info(COLOR_RED, op, "Stuck recovery is temporarily unavailable.");
        return;
    }

    server_wall_utc_t old_cooldown = pl->stuck_cooldown;
    pl->stuck_cooldown = stuck_cooldown_deadline(server_wall_utc_now());

    /* Persist before inserting the effect so reconnecting cannot reset the
     * cooldown and the countdown cannot advance while the save is committed. */
    if (!player_save_checked(op)) {
        pl->stuck_cooldown = old_cooldown;
        object_destroy(effect);
        draw_info(COLOR_RED,
                  op,
                  "Your stuck recovery cooldown could not be saved; recovery was not started.");
        return;
    }

    object_insert_into(effect, op, 0);

    draw_info_format(COLOR_WHITE,
                     op,
                     "You will be moved to safety in %u seconds if you remain out of combat.",
                     PLAYER_STUCK_COUNTDOWN_SECONDS);
}
