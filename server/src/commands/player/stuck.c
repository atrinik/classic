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
#include <exit.h>
#include <gameplay_journal.h>
#include <map.h>
#include <object.h>
#include <player.h>
#include <server.h>
#include <server_main.h>
#include <server_clock.h>
#include <stuck.h>

#ifdef ATRINIK_TESTING
static mapstruct *test_destination;
static bool test_cancel_observed_active;
#endif

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

static bool stuck_combat_interrupted(const object *op, const player *pl) {
    if (stuck_currently_in_combat(op)) {
        return true;
    }

    return pl->combat_event_sequence != pl->stuck_combat_event_sequence;
}

static mapstruct *stuck_recovery_map(void) {
#ifdef ATRINIK_TESTING
    if (test_destination != NULL) {
        return test_destination;
    }
#endif

    return ready_map_name(EMERGENCY_MAPPATH, NULL, 0);
}

/**
 * Resolve walk-on exits before entering the recovery map.
 *
 * The production emergency map is deliberately a one-tile map whose tile
 * contains a walk-on exit. Entering that map through the normal movement path
 * would therefore make the final location depend on map callbacks. Resolve
 * the canonical exit chain up front so the transfer can target its final map
 * directly. Ambiguous or unresolvable walk-on behavior fails closed.
 */
static bool stuck_resolve_walk_on_exits(object *op, mapstruct **map, int *x, int *y) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(map != NULL);
    HARD_ASSERT(*map != NULL);
    HARD_ASSERT(x != NULL);
    HARD_ASSERT(y != NULL);

    const bool flying = QUERY_FLAG(op, FLAG_FLYING);
    const int trigger_flag = flying ? P_FLY_ON : P_WALK_ON;
    for (unsigned int depth = 0; depth < 4; depth++) {
        MapSpace *space = GET_MAP_SPACE_PTR(*map, *x, *y);
        if ((space->flags & trigger_flag) == 0) {
            return true;
        }

        object *walk_on_exit = NULL;
        for (object *candidate = GET_MAP_OB(*map, *x, *y); candidate != NULL;
             candidate = candidate->above) {
            if ((flying && !QUERY_FLAG(candidate, FLAG_FLY_ON)) ||
                (!flying && !QUERY_FLAG(candidate, FLAG_WALK_ON))) {
                continue;
            }

            if (candidate->type != EXIT || !exit_has_usable_destination(candidate) ||
                walk_on_exit != NULL) {
                return false;
            }
            walk_on_exit = candidate;
        }

        if (walk_on_exit == NULL) {
            return false;
        }

        int next_x;
        int next_y;
        mapstruct *next_map = exit_get_destination(walk_on_exit, &next_x, &next_y, true);
        if (next_map == NULL) {
            return false;
        }

        mapstruct *actual_map = get_map_from_coord(next_map, &next_x, &next_y);
        if (actual_map == NULL ||
            (actual_map == *map && next_x == *x && next_y == *y)) {
            return false;
        }

        *map = actual_map;
        *x = next_x;
        *y = next_y;
    }

    return false;
}

/**
 * Resolve and validate the fixed recovery location before any map mutation.
 */
static bool stuck_find_destination(object *op, mapstruct **destination, int *x, int *y) {
    mapstruct *requested_map = stuck_recovery_map();
    int requested_x = EMERGENCY_X;
    int requested_y = EMERGENCY_Y;

    if (requested_map == NULL) {
        return false;
    }

    if (MAP_FIXEDLOGIN(requested_map)) {
        requested_x = MAP_ENTER_X(requested_map);
        requested_y = MAP_ENTER_Y(requested_map);
    } else {
        int free_index =
            map_free_spot_first(requested_map, requested_x, requested_y, op->arch, op);
        if (free_index == -1) {
            return false;
        }

        requested_x += freearr_x[free_index];
        requested_y += freearr_y[free_index];
    }

    /* Validate after resolving a tiled-map boundary. */
    int actual_x = requested_x;
    int actual_y = requested_y;
    mapstruct *actual_map = get_map_from_coord(requested_map, &actual_x, &actual_y);
    if (actual_map == NULL || MAP_PLAYER_NO_SAVE(requested_map) ||
        MAP_PLAYER_NO_SAVE(actual_map)) {
        return false;
    }

    mapstruct *resolved_map = actual_map;
    int resolved_x = actual_x;
    int resolved_y = actual_y;
    if (!stuck_resolve_walk_on_exits(op, &resolved_map, &resolved_x, &resolved_y)) {
        return false;
    }

    if (MAP_FIXEDLOGIN(resolved_map)) {
        resolved_x = MAP_ENTER_X(resolved_map);
        resolved_y = MAP_ENTER_Y(resolved_map);
    }

    resolved_map = get_map_from_coord(resolved_map, &resolved_x, &resolved_y);
    if (resolved_map == NULL || MAP_PLAYER_NO_SAVE(resolved_map)) {
        return false;
    }

    if (arch_blocked(op->arch, op, resolved_map, resolved_x, resolved_y) != 0) {
        return false;
    }

    *destination = resolved_map;
    *x = resolved_x;
    *y = resolved_y;
    return true;
}

/** Restore a player to its exact pre-transfer location after a failed entry. */
static bool stuck_restore_source(object *op,
                                 mapstruct *source,
                                 int source_x,
                                 int source_y,
                                 bool *destroyed) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(destroyed != NULL);

    *destroyed = false;
    if (source == NULL) {
        return false;
    }

    if (op->map == source && op->x == source_x && op->y == source_y &&
        !QUERY_FLAG(op, FLAG_REMOVED)) {
        return true;
    }

    tag_t rollback_count = op->count;
    bool entered = object_enter_map_exact(op, source, source_x, source_y, true);
    if (OBJECT_DESTROYED(op, rollback_count)) {
        *destroyed = true;
        return false;
    }

    return entered && op->map == source && op->x == source_x && op->y == source_y &&
           !QUERY_FLAG(op, FLAG_REMOVED);
}

void player_stuck_cancel(object *op) {
    if (op == NULL || op->type != PLAYER || CONTR(op) == NULL) {
        return;
    }

#ifdef ATRINIK_TESTING
    if (CONTR(op)->stuck_deadline.value != 0) {
        test_cancel_observed_active = true;
    }
#endif

    CONTR(op)->stuck_deadline.value = 0;
    CONTR(op)->stuck_started.value = 0;
    CONTR(op)->stuck_combat_event_sequence = 0;
}

bool player_stuck_process(object *op) {
    HARD_ASSERT(op != NULL);

    if (op->type != PLAYER || CONTR(op) == NULL) {
        return true;
    }

    player *pl = CONTR(op);
    if (pl->cs == NULL || pl->cs->state != ST_PLAYING || !OBJECT_ACTIVE(op)) {
        player_stuck_cancel(op);
        return true;
    }

    if (pl->stuck_deadline.value == 0) {
        return true;
    }

    if (stuck_combat_interrupted(op, pl)) {
        player_stuck_cancel(op);
        draw_info(COLOR_WHITE, op, "Your stuck recovery was interrupted by combat.");
        return true;
    }

    if (!server_tick_expired(pl->stuck_deadline)) {
        return true;
    }

    player_stuck_cancel(op);

    mapstruct *destination;
    int x, y;
    if (!stuck_find_destination(op, &destination, &x, &y)) {
        draw_info(COLOR_RED,
                  op,
                  "The safe recovery location is unavailable; you were not moved.");
        return true;
    }

    mapstruct *source = op->map;
    int source_x = op->x;
    int source_y = op->y;
    tag_t object_count = op->count;
    object_semantic_result_t result =
        object_enter_map_reason_exact(op, destination, x, y, "player.stuck-recovery");
    if (OBJECT_DESTROYED(op, object_count)) {
        return false;
    }

    if (result == OBJECT_SEMANTIC_COMMITTED &&
        (!OBJECT_ACTIVE(op) || op->map != destination || op->x != x || op->y != y)) {
        result = OBJECT_SEMANTIC_AMBIGUOUS;
    }

    if (result != OBJECT_SEMANTIC_COMMITTED) {
        bool destroyed = false;
        bool restored = stuck_restore_source(op, source, source_x, source_y, &destroyed);
        if (destroyed) {
            return false;
        }

        if (result == OBJECT_SEMANTIC_AMBIGUOUS && !restored) {
            draw_info(COLOR_RED,
                      op,
                      "Safe recovery was attempted, but its result and your location could not be confirmed.");
            return false;
        }

        if (!restored) {
            draw_info(COLOR_RED,
                      op,
                      "Safe recovery failed and your original location could not be restored.");
            return false;
        }

        draw_info(COLOR_RED,
                  op,
                  "The safe recovery could not be completed; you were not moved.");
        return false;
    }

    if (!player_save_checked(op)) {
        bool destroyed = false;
        bool restored = stuck_restore_source(op, source, source_x, source_y, &destroyed);
        if (destroyed) {
            return false;
        }
        if (!restored) {
            draw_info(COLOR_RED,
                      op,
                      "Safe recovery could not be saved and your location could not be restored.");
            return false;
        }
        draw_info(COLOR_RED,
                  op,
                  "Safe recovery could not be saved; you were not moved.");
        return false;
    }

    draw_info(COLOR_WHITE, op, "You have been moved to the safe recovery location.");
    return false;
}

void player_stuck_process_all(void) {
    for (player *pl = first_player; pl != NULL;) {
        player *next = pl->next;
        if (pl->ob != NULL && !OBJECT_FREE(pl->ob)) {
            if (pl->cs == NULL || pl->cs->state != ST_PLAYING || !OBJECT_ACTIVE(pl->ob)) {
                player_stuck_cancel(pl->ob);
            } else {
                (void)player_stuck_process(pl->ob);
            }
        }
        pl = next;
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

    if (pl->stuck_deadline.value != 0) {
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
        draw_info(COLOR_RED,
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

    server_tick_duration_t countdown;
    if (!server_duration_to_ticks(server_duration_from_seconds(PLAYER_STUCK_COUNTDOWN_SECONDS),
                                  &countdown)) {
        draw_info(COLOR_RED, op, "Stuck recovery is temporarily unavailable.");
        return;
    }

    server_tick_t old_started = pl->stuck_started;
    server_tick_t old_deadline = pl->stuck_deadline;
    server_wall_utc_t old_cooldown = pl->stuck_cooldown;
    uint64_t old_combat_event_sequence = pl->stuck_combat_event_sequence;

    pl->stuck_started = server_tick_now();
    pl->stuck_deadline = server_tick_deadline_after(countdown);
    pl->stuck_cooldown = stuck_cooldown_deadline(server_wall_utc_now());
    pl->stuck_combat_event_sequence = pl->combat_event_sequence;

    /* Persist before the countdown starts so reconnecting cannot reset the cooldown. */
    if (!player_save_checked(op)) {
        pl->stuck_started = old_started;
        pl->stuck_deadline = old_deadline;
        pl->stuck_cooldown = old_cooldown;
        pl->stuck_combat_event_sequence = old_combat_event_sequence;
        draw_info(COLOR_RED,
                  op,
                  "Your stuck recovery cooldown could not be saved; recovery was not started.");
        return;
    }

    draw_info_format(COLOR_WHITE,
                     op,
                     "You will be moved to safety in %u seconds if you remain out of combat.",
                     PLAYER_STUCK_COUNTDOWN_SECONDS);
}

#ifdef ATRINIK_TESTING
void player_stuck_destination_for_test(mapstruct *destination) {
    test_destination = destination;
}

void player_stuck_cancel_observation_reset_for_test(void) {
    test_cancel_observed_active = false;
}

bool player_stuck_cancel_observed_active_for_test(void) {
    return test_cancel_observed_active;
}
#endif
