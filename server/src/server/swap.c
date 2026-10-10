/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2009-2026 The Atrinik Project                         *
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
 * Controls map swap functions.
 */

#include <global.h>
#include <swap.h>
#include <server_main.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <toolkit/stringbuffer.h>
#include <toolkit/path.h>
#include <plugin.h>
#include <celestial_structure.h>

/**
 * Write maps log.
 */
bool write_map_log_checked(void) {
    char path[HUGE_BUF];
    if (snprintf(VS(path), "%s/temp.maps", settings.datapath) >= (int)sizeof(path)) {
        return false;
    }
    long current_time = time(NULL);
    StringBuffer *buffer = stringbuffer_new();
    mapstruct *map;
    DL_FOREACH(first_map, map) {
        /* Unique player maps have no temporary name. */
        if (map->in_memory != MAP_IN_MEMORY && map->tmpname && strncmp(map->path, "/random", 7)) {
            stringbuffer_append_printf(buffer,
                                       "%s:%s:%ld:%d:%d\n",
                                       map->path,
                                       map->tmpname,
                                       (map->reset_time - current_time),
                                       map->difficulty,
                                       map->darkness);
        }
    }
    char *contents = stringbuffer_finish(buffer);
    bool saved = path_write_atomic(path, contents, strlen(contents), SAVE_MODE);
    free(contents);
    if (!saved) {
        LOG(BUG, "Could not atomically write %s", path);
    }
    return saved;
}

void write_map_log(void) {
    (void)write_map_log_checked();
}

/**
 * Read map log.
 */
void read_map_log(void) {
    FILE *fp;
    mapstruct *map;
    char buf[HUGE_BUF];
    int darkness;

    snprintf(buf, sizeof(buf), "%s/temp.maps", settings.datapath);

    if (!(fp = fopen(buf, "r"))) {
        return;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        char *tmp[3];

        map = get_linked_map();

        if (string_split(buf, tmp, sizeof(tmp) / sizeof(*tmp), ':') != 3) {
            LOG(DEBUG, "%s/temp.maps: ignoring invalid line: %s", settings.datapath, buf);
            continue;
        }

        FREE_AND_COPY_HASH(map->path, tmp[0]);
        map->tmpname = xstrdup(tmp[1]);

        sscanf(tmp[2], "%ud:%d:%d\n", &map->reset_time, &map->difficulty, &darkness);

        map->in_memory = MAP_SWAPPED;
        map->darkness = darkness;
        if (celestial_structure_v1_runtime_active()) {
            map->celestial_schema = 1;
            map->celestial_v1_header_seen = true;
        }

        if (darkness == -1) {
            darkness = MAX_DARKNESS;
        }

        map->light_value = global_darkness_table[MAX_DARKNESS];
    }

    fclose(fp);
}

/**
 * Checks if the specified map can be swapped.
 * @param tiled
 * The tiled map.
 * @param map
 * Map on the Z axis.
 * @return
 * 1 if the map cannot be swapped, 0 otherwise.
 */
static int swap_map_check(mapstruct *tiled, mapstruct *map) {
    return tiled->player_first != NULL;
}

static bool swap_map_has_players(mapstruct *map) {
    MAP_TILES_WALK_START(map, swap_map_check) {
        if (MAP_TILES_WALK_RETVAL != 0) {
            return true;
        }
    }
    MAP_TILES_WALK_END
    return false;
}

/** Cancel retained expiry state before map re-entry, reuse, or teardown. */
void swap_cancel_pending(mapstruct *map) {
    map->swap_pending_order = 0;
    map->swap_pending_count = 0;
    map->swap_retry_ticks = 0;
    map->swap_failures = 0;
}

/**
 * Swaps a map to disk.
 * @param map
 * Map to swap.
 * @param force_flag
 * Force flag. If set, will not check for players.
 */
bool swap_map_checked(mapstruct *map, int force_flag) {
    if (map->in_memory != MAP_IN_MEMORY) {
        LOG(BUG, "Tried to swap out map which was not in memory (%s).", map->path);
        return false;
    }

    if (!force_flag && swap_map_has_players(map)) {
        return false;
    }

    /* Update the reset time. */
    if (!MAP_FIXED_RESETTIME(map)) {
        set_map_reset_time(map);
    }

    /* If it is immediate reset time, don't bother saving it - just get
     * rid of it right away. */
    if (map->reset_time <= (uint32_t)seconds()) {
        if (map->events) {
            /* Trigger the map reset event */
            trigger_map_event(MEVENT_RESET, map, NULL, NULL, NULL, map->path, 0);
        }

        delete_map(map);
        return true;
    }

    if (new_save_map(map, 0) != 0) {
        LOG(BUG, "Failed to swap map %s.", map->path);
        return false;
    } else {
        free_map(map, 1);
    }
    return true;
}

void swap_map(mapstruct *map, int force_flag) {
    (void)swap_map_checked(map, force_flag);
}

/**
 * Age resident maps and attempt at most one eligible ordinary swap per call.
 *
 * Tickets live in the maps, so no queue retains a deleted map pointer. Failed
 * saves keep the map resident, back off by 2..32 ticks and move behind waiting
 * work. Explicit resets and shutdown still use the complete checked save path.
 */
void check_active_maps(void) {
    static uint64_t next_order;
    mapstruct *map, *candidate = NULL;

    DL_FOREACH(first_map, map) {
        if (map->in_memory != MAP_IN_MEMORY) {
            swap_cancel_pending(map);
            continue;
        }

        if (map->swap_pending_order != 0 &&
            (map->swap_pending_count != map->count || map->timeout != 0)) {
            swap_cancel_pending(map);
        }

        if (map->player_first != NULL) {
            swap_cancel_pending(map);
            map->timeout = 0;
            continue;
        }

        if (map->swap_pending_order == 0) {
            if (map->timeout == 0) {
                set_map_timeout(map);
                continue;
            }

            if (map->timeout > 1) {
                map->timeout--;
                continue;
            }

            map->timeout = 0;
            map->swap_pending_order = ++next_order;
            map->swap_pending_count = map->count;
        }

        if (map->swap_retry_ticks != 0) {
            map->swap_retry_ticks--;
            continue;
        }

        /* Recheck the complete resident stack at the point of scheduling. A
         * player on a linked map cancels expiry just as direct occupancy does. */
        if (swap_map_has_players(map)) {
            swap_cancel_pending(map);
            continue;
        }

        if (candidate == NULL || map->swap_pending_order < candidate->swap_pending_order) {
            candidate = map;
        }
    }

    if (candidate != NULL && !swap_map_checked(candidate, 0)) {
        /* A successful reset may delete candidate; only a failed save retains
         * a resident map that may be inspected here. */
        if (candidate->swap_failures < 5) {
            candidate->swap_failures++;
        }
        candidate->swap_retry_ticks = 1U << candidate->swap_failures;
        candidate->swap_pending_order = ++next_order;
    }
}

/**
 * Removes temporary files of maps which are going to be reset next time
 * they are visited.
 *
 * This is very useful if the tmp-disk is very full.
 */
void flush_old_maps(void) {
    mapstruct *m, *tmp;
    long sec = seconds();

    DL_FOREACH_SAFE(first_map, m, tmp) {
        /* There can be cases (ie death) where a player leaves a map and
         * the timeout is not set so it isn't swapped out. */
        if (m->in_memory == MAP_IN_MEMORY && m->timeout == 0 &&
            m->swap_pending_order == 0 && !m->player_first) {
            set_map_timeout(m);
        }

        /* Per player unique maps are never really reset. */
        if (MAP_UNIQUE(m) && m->in_memory == MAP_SWAPPED) {
            delete_map(m);
            continue;
        }

        if (m->in_memory != MAP_SWAPPED || m->tmpname == NULL || (uint32_t)sec < m->reset_time) {
            /* No need to flush them if there are no resets */
            continue;
        }

        if (m->events != NULL) {
            /* Trigger the map reset event */
            trigger_map_event(MEVENT_RESET, m, NULL, NULL, NULL, m->path, 0);
        }

        clean_tmp_map(m);
        delete_map(m);
    }
}
