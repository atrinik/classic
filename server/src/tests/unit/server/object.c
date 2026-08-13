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

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <commands.h>
#include <toolkit/string.h>
#include <arch.h>
#include <loader.h>
#include <living.h>
#include <monster.h>
#include <monster_data.h>
#include <movement.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <server.h>
#include <server_item.h>
#include <skills.h>
#include <spells.h>
#include <swap.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>
#include <waypoint.h>

static bool active_list_contains_at(const object *needle, const char *phase) {
    size_t visited = 0;
    const object *previous = NULL;

    for (const object *tmp = active_objects; tmp != NULL; tmp = tmp->active_next) {
        ck_assert_uint_lt(visited++, 100000);
        ck_assert_msg(!OBJECT_FREE(tmp), "free active object during %s", phase);
        ck_assert_msg(tmp->active_prev == previous, "bad active backlink during %s", phase);

        if (tmp == needle) {
            return true;
        }

        previous = tmp;
    }

    return false;
}

#define active_list_contains(needle) active_list_contains_at((needle), __func__)

START_TEST(test_return_home_waypoint_reactivation_resets_retry_progress) {
    mapstruct *map;
    object *player;
    check_setup_env_pl(&map, &player);
    FREE_AND_COPY_HASH(map->path, "/tests/return-home-reactivation");

    object *monster = arch_get("kobold");
    ck_assert_ptr_nonnull(monster);
    monster = object_insert_map(monster, map, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(monster);
    monster_data_init(monster);
    ck_assert_ptr_nonnull(living_get_base_info(monster));

    set_npc_enemy(monster, player, NULL);
    set_npc_enemy(monster, NULL, NULL);
    object *waypoint = waypoint_get_home(monster);
    ck_assert_ptr_nonnull(waypoint);

    CLEAR_FLAG(waypoint, FLAG_CURSED);
    waypoint->stats.Int = 8;
    waypoint->stats.Str = 5;
    waypoint->stats.dam = 1;

    set_npc_enemy(monster, player, NULL);
    set_npc_enemy(monster, NULL, NULL);

    ck_assert_ptr_eq(waypoint_get_home(monster), waypoint);
    ck_assert(QUERY_FLAG(waypoint, FLAG_CURSED));
    ck_assert_int_eq(waypoint->stats.Int, 0);
    ck_assert_int_eq(waypoint->stats.Str, 0);
    ck_assert_int_eq(waypoint->stats.dam, 30000);

    object_remove(monster, 0);
    object_destroy(monster);
    object_remove(player, 0);
    object_destroy(player);
}
END_TEST

static size_t invalid_direction_log_count;

static void capture_invalid_direction_log(const char *message) {
    if (strstr(message, "Rejected invalid movement direction") != NULL) {
        invalid_direction_log_count++;
    }
}

START_TEST(test_move_ob_rejects_invalid_directions) {
    static const int invalid_directions[] = {INT_MIN, 0, NUM_DIRECTION + 1, INT_MAX};
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    int direction = pl->direction;
    uint32_t anim_flags = pl->anim_flags;
    long saved_pticks = pticks;
    pticks = 1;
    invalid_direction_log_count = 0;
    logger_set_print_func(capture_invalid_direction_log);

    for (size_t i = 0; i < arraysize(invalid_directions); i++) {
        ck_assert_int_eq(move_ob(pl, invalid_directions[i], pl), 0);
        ck_assert_int_eq(pl->direction, direction);
        ck_assert_uint_eq(pl->anim_flags, anim_flags);
    }

    ck_assert_uint_eq(invalid_direction_log_count, 1);
    ck_assert(!movement_direction_valid(pl, INT_MIN, true));
    ck_assert(movement_direction_valid(pl, 0, true));
    ck_assert(movement_direction_valid(pl, 1, false));
    ck_assert(movement_direction_valid(pl, NUM_DIRECTION, false));
    ck_assert_int_eq(push_ob(pl, INT_MAX, pl), 0);
    ck_assert_int_eq(cast_spell(pl, pl, INT_MIN, -1, 0, CAST_NORMAL, NULL), 0);
    object *bow = arch_get("bow_short");
    ck_assert_ptr_nonnull(bow);
    ck_assert_int_eq(object_ranged_fire(bow, pl, INT_MAX, NULL), OBJECT_METHOD_UNHANDLED);
    object_destroy(bow);
    construction_do(pl, INT_MIN);
    pticks += 60L * MAX_TICKS;
    ck_assert_int_eq(move_ob(pl, -1, pl), 0);
    ck_assert_uint_eq(invalid_direction_log_count, 2);

    logger_set_print_func(logger_do_print);
    pticks = saved_pticks;
    object_remove(pl, 0);
    object_destroy(pl);
}
END_TEST

START_TEST(test_find_enemy_returns_valid_direction_for_tiled_exit_enemy) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/stale-rv-source");
    source->tile_path[TILED_NORTH] = add_string("/tests/stale-rv-must-not-load");

    mapstruct *bridge = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(bridge->path, "/tests/stale-rv-bridge");
    source->tile_map[TILED_EAST] = bridge;
    bridge->tile_map[TILED_WEST] = source;

    mapstruct *destination = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(destination->path, "/tests/stale-rv-destination");

    object_remove(pl, 0);
    pl->x = 12;
    pl->y = 12;
    pl = object_insert_map(pl, destination, NULL, 0);
    ck_assert_ptr_nonnull(pl);

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = 1;
    exit->y = 1;
    EXIT_X(exit) = pl->x;
    EXIT_Y(exit) = pl->y;
    FREE_AND_ADD_REF_HASH(EXIT_PATH(exit), destination->path);
    exit = object_insert_map(exit, bridge, NULL, 0);
    ck_assert_ptr_nonnull(exit);

    object *monster = arch_get("kobold");
    ck_assert_ptr_nonnull(monster);
    monster->x = 10;
    monster->y = 10;
    monster = object_insert_map(monster, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(monster);
    monster_data_init(monster);

    monster->attacked_by = pl;
    monster->attacked_by_count = pl->count;

    rv_vector rv = {
        .distance = 2,
        .distance_x = 0,
        .distance_y = 0,
        .distance_z = 0,
        .direction = INT_MIN,
        .part = NULL,
    };

    ck_assert(on_same_map(monster, pl));
    ck_assert(!get_rangevector(monster, pl, &rv, RV_DIAGONAL_DISTANCE));
    rv.direction = INT_MIN;

    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(rv.direction > 0 && rv.direction <= NUM_DIRECTION,
                  "find_enemy returned an enemy with stale rv.direction %d",
                  rv.direction);
    ck_assert_ptr_null(source->tile_map[TILED_NORTH]);

    rv.direction = INT_MIN;

    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(rv.direction > 0 && rv.direction <= NUM_DIRECTION,
                  "find_enemy returned the current enemy with stale rv.direction %d",
                  rv.direction);

    set_npc_enemy(monster, NULL, NULL);
    mapstruct *isolated = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(isolated->path, "/tests/stale-rv-isolated");
    object_remove(pl, 0);
    pl = object_insert_map(pl, isolated, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    monster->attacked_by = pl;
    monster->attacked_by_count = pl->count;

    ck_assert_ptr_null(find_enemy(monster, &rv));
    ck_assert_ptr_null(monster->attacked_by);

    object_remove(monster, 0);
    object_destroy(monster);
    object_remove(pl, 0);
    object_destroy(pl);
}
END_TEST

START_TEST(test_find_enemy_returns_valid_direction_for_exit_tiled_enemy) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/stale-rv-exit-source");

    mapstruct *bridge = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(bridge->path, "/tests/stale-rv-exit-bridge");

    mapstruct *destination = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(destination->path, "/tests/stale-rv-exit-destination");
    bridge->tile_map[TILED_EAST] = destination;
    destination->tile_map[TILED_WEST] = bridge;

    object_remove(pl, 0);
    pl->x = 12;
    pl->y = 12;
    pl = object_insert_map(pl, destination, NULL, 0);
    ck_assert_ptr_nonnull(pl);

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = 1;
    exit->y = 1;
    EXIT_X(exit) = 1;
    EXIT_Y(exit) = 1;
    FREE_AND_ADD_REF_HASH(EXIT_PATH(exit), bridge->path);
    exit = object_insert_map(exit, source, NULL, 0);
    ck_assert_ptr_nonnull(exit);

    object *monster = arch_get("kobold");
    ck_assert_ptr_nonnull(monster);
    monster->x = 10;
    monster->y = 10;
    monster = object_insert_map(monster, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(monster);
    monster_data_init(monster);

    monster->attacked_by = pl;
    monster->attacked_by_count = pl->count;

    rv_vector rv = {
        .distance = 2,
        .distance_x = 0,
        .distance_y = 0,
        .distance_z = 0,
        .direction = INT_MIN,
        .part = NULL,
    };

    ck_assert(on_same_map(monster, pl));
    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(rv.direction > 0 && rv.direction <= NUM_DIRECTION,
                  "find_enemy returned an exit-tiled enemy with stale rv.direction %d",
                  rv.direction);

    rv.direction = INT_MIN;
    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(rv.direction > 0 && rv.direction <= NUM_DIRECTION,
                  "find_enemy dropped an exit-tiled current enemy or returned stale direction %d",
                  rv.direction);

    object_remove(monster, 0);
    object_destroy(monster);
    object_remove(pl, 0);
    object_destroy(pl);
}
END_TEST

START_TEST(test_find_enemy_returns_valid_direction_for_tiled_exit_tiled_enemy) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    FREE_AND_COPY_HASH(source->path, "/tests/stale-rv-tiled-exit-tiled-source");

    mapstruct *first_bridge = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(first_bridge->path, "/tests/stale-rv-tiled-exit-tiled-first-bridge");
    source->tile_map[TILED_EAST] = first_bridge;
    first_bridge->tile_map[TILED_WEST] = source;

    mapstruct *second_bridge = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(second_bridge->path, "/tests/stale-rv-tiled-exit-tiled-second-bridge");

    mapstruct *destination = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(destination->path, "/tests/stale-rv-tiled-exit-tiled-destination");
    second_bridge->tile_map[TILED_EAST] = destination;
    destination->tile_map[TILED_WEST] = second_bridge;

    object_remove(pl, 0);
    pl->x = 12;
    pl->y = 12;
    pl = object_insert_map(pl, destination, NULL, 0);
    ck_assert_ptr_nonnull(pl);

    object *exit = arch_get("stairs_down");
    ck_assert_ptr_nonnull(exit);
    exit->x = 1;
    exit->y = 1;
    EXIT_X(exit) = 1;
    EXIT_Y(exit) = 1;
    FREE_AND_ADD_REF_HASH(EXIT_PATH(exit), second_bridge->path);
    exit = object_insert_map(exit, first_bridge, NULL, 0);
    ck_assert_ptr_nonnull(exit);

    object *monster = arch_get("kobold");
    ck_assert_ptr_nonnull(monster);
    monster->x = 10;
    monster->y = 10;
    monster = object_insert_map(monster, source, NULL, INS_NO_MERGE);
    ck_assert_ptr_nonnull(monster);
    monster_data_init(monster);

    monster->attacked_by = pl;
    monster->attacked_by_count = pl->count;

    rv_vector rv = {
        .distance = 2,
        .distance_x = 0,
        .distance_y = 0,
        .distance_z = 0,
        .direction = INT_MIN,
        .part = NULL,
    };

    ck_assert(on_same_map(monster, pl));
    ck_assert(!get_rangevector(monster,
                               pl,
                               &rv,
                               RV_DIAGONAL_DISTANCE | RV_RECURSIVE_SEARCH | RV_NO_LOAD));
    rv.direction = INT_MIN;
    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(rv.direction > 0 && rv.direction <= NUM_DIRECTION,
                  "find_enemy returned a tiled-exit-tiled enemy with stale rv.direction %d",
                  rv.direction);

    rv.direction = INT_MIN;
    ck_assert_ptr_eq(find_enemy(monster, &rv), pl);
    ck_assert_msg(
        rv.direction > 0 && rv.direction <= NUM_DIRECTION,
        "find_enemy dropped a tiled-exit-tiled current enemy or returned stale direction %d",
        rv.direction);

    object_remove(monster, 0);
    object_destroy(monster);
    object_remove(pl, 0);
    object_destroy(pl);
}
END_TEST

START_TEST(test_object_can_merge) {
    object *ob1, *ob2;

    ob1 = arch_get("bolt");
    ob2 = arch_get("bolt");
    ck_assert(object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob1->custody_lineage, "lineage-1");
    ck_assert(!object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob2->custody_lineage, "lineage-1");
    ck_assert(object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob2->custody_first, "account:character");
    ck_assert(!object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob1->custody_first, "account:character");
    ck_assert(object_can_merge(ob1, ob2));
    FREE_AND_CLEAR_HASH(ob1->custody_lineage);
    FREE_AND_CLEAR_HASH(ob2->custody_lineage);
    FREE_AND_CLEAR_HASH(ob1->custody_first);
    FREE_AND_CLEAR_HASH(ob2->custody_first);
    FREE_AND_COPY_HASH(ob2->name, "Not same name");
    ck_assert(!object_can_merge(ob1, ob2));
    object_destroy(ob2);
    ob2 = arch_get("bolt");
    FREE_AND_COPY_HASH(ob1->name_pl, "custom bolts one");
    FREE_AND_COPY_HASH(ob2->name_pl, "custom bolts two");
    ck_assert(!object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob2->name_pl, "custom bolts one");
    ck_assert(object_can_merge(ob1, ob2));
    FREE_AND_COPY_HASH(ob2->name_pl, "projectiles");
    ck_assert(!object_can_merge(ob1, ob2));
    object_destroy(ob2);
    ob2 = arch_get("bolt");
    FREE_AND_COPY_HASH(ob2->name_pl, "custom bolts one");
    ob2->type++;
    ck_assert(!object_can_merge(ob1, ob2));
    object_destroy(ob2);
    ob2 = arch_get("bolt");
    FREE_AND_COPY_HASH(ob2->name_pl, "custom bolts one");
    ob2->light_color = UINT32_C(0xff0000);
    ck_assert(!object_can_merge(ob1, ob2));
    object_destroy(ob2);
    ob2 = arch_get("bolt");
    FREE_AND_COPY_HASH(ob2->name_pl, "custom bolts one");
    ob1->nrof = INT32_MAX;
    ob2->nrof = 1;
    ck_assert(!object_can_merge(ob1, ob2));
    object_destroy(ob1);
    object_destroy(ob2);
}
END_TEST

START_TEST(test_object_custody_provenance) {
    object *player = player_get_dummy("Custody tester", NULL);
    object *item = arch_get("bolt");

    ck_assert_ptr_eq(item->custody_lineage, NULL);
    ck_assert_ptr_eq(item->custody_first, NULL);
    ck_assert_ptr_eq(item->custody_last, NULL);

    object_custody_acquire(item, player);
    ck_assert_ptr_ne(item->custody_lineage, NULL);
    ck_assert_ptr_ne(item->custody_first, NULL);
    ck_assert_ptr_ne(player->custody_actor, NULL);
    ck_assert_str_eq(item->custody_first, player->custody_actor);
    ck_assert_msg(strncmp(item->custody_lineage, "item:", 5) == 0,
                  "custody lineage must use the persistent item prefix");

    shstr *first = add_refcount(item->custody_first);
    object_custody_acquire(item, player);
    ck_assert_ptr_eq(item->custody_first, first);
    free_string_shared(first);

    object_custody_relinquish(item, player);
    ck_assert_str_eq(item->custody_last, player->custody_actor);
    object_custody_record(item, player, "custody-test");

    char item_name[] = "bolt";
    item = object_insert_into(item, player, 0);
    command_custody(player, "custody", item_name);
    command_custody(player, "custody", "");

    object *other_player = player_get_dummy("Second custody tester", NULL);
    object *other_item = arch_get("bolt");
    object_custody_relinquish(other_item, other_player);
    ck_assert_ptr_ne(other_player->custody_actor, NULL);
    ck_assert_str_eq(other_item->custody_last, other_player->custody_actor);

    object_destroy(other_item);
    object_destroy(other_player);
    object_destroy(player);
}
END_TEST

START_TEST(test_map_stack_operations_increment_update_once) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *first = arch_get("bolt");
    first->x = pl->x;
    first->y = pl->y;
    first->nrof = 1;
    first = object_insert_map(first, map, NULL, INS_NO_MERGE);

    object *second = arch_get("bolt");
    second->x = pl->x;
    second->y = pl->y;
    second->nrof = 1;
    second = object_insert_map(second, map, NULL, INS_NO_MERGE);

    object *removed = arch_get("bolt");
    removed->x = pl->x;
    removed->y = pl->y;
    removed = object_insert_map(removed, map, NULL, INS_NO_MERGE);
    uint8_t old_update = GET_MAP_UPDATE_COUNTER(map, first->x, first->y);
    object_remove(removed, REMOVE_NO_WEIGHT);
    object_destroy(removed);
    uint8_t remove_updates =
        (uint8_t)(GET_MAP_UPDATE_COUNTER(map, first->x, first->y) - old_update);

    old_update = GET_MAP_UPDATE_COUNTER(map, first->x, first->y);
    ck_assert_ptr_eq(object_merge(second), first);
    ck_assert_uint_eq(GET_MAP_UPDATE_COUNTER(map, first->x, first->y),
                      (uint8_t)(old_update + remove_updates + 1));

    old_update = GET_MAP_UPDATE_COUNTER(map, first->x, first->y);
    object *split = object_stack_get(first, 1);
    ck_assert_ptr_ne(split, first);
    ck_assert_uint_eq(GET_MAP_UPDATE_COUNTER(map, first->x, first->y), (uint8_t)(old_update + 1));
    object_destroy(split);

    first->nrof = 2;
    old_update = GET_MAP_UPDATE_COUNTER(map, first->x, first->y);
    ck_assert_ptr_eq(object_decrease(first, 1), first);
    ck_assert_uint_eq(GET_MAP_UPDATE_COUNTER(map, first->x, first->y), (uint8_t)(old_update + 1));

    object_destroy(pl);
}
END_TEST

START_TEST(test_object_plural_name_contract) {
    object *ob = object_load_str("arch sack\nname torch\nname_pl torches\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_str_eq(ob->name, "torch");
    ck_assert_str_eq(ob->name_pl, "torches");

    StringBuffer *sb = object_get_display_name(ob, NULL, NULL);
    char *name = stringbuffer_finish(sb);
    ck_assert_str_eq(name, "torch");
    free(name);

    ob->nrof = 2;
    sb = object_get_display_name(ob, NULL, NULL);
    name = stringbuffer_finish(sb);
    ck_assert_str_eq(name, "torches");
    free(name);

    FREE_AND_COPY_HASH(ob->custom_name, "My Torch");
    sb = object_get_display_name(ob, NULL, NULL);
    name = stringbuffer_finish(sb);
    ck_assert_str_eq(name, "My Torch");
    free(name);

    object *clone = object_clone(ob);
    ck_assert_ptr_eq(clone->name_pl, ob->name_pl);
    ck_assert_ptr_eq(clone->custom_name, ob->custom_name);

    sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    char *dump = stringbuffer_finish(sb);
    ck_assert_ptr_nonnull(strstr(dump, "name_pl torches\n"));
    object *roundtrip = object_load_str(dump);
    ck_assert_ptr_nonnull(roundtrip);
    ck_assert_str_eq(roundtrip->name_pl, "torches");
    ck_assert_str_eq(roundtrip->custom_name, "My Torch");

    free(dump);
    object_destroy(roundtrip);
    object_destroy(clone);
    object_destroy(ob);

    ob = object_load_str("arch sack\nname_pl torches\nname torch\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_str_eq(ob->name, "torch");
    ck_assert_str_eq(ob->name_pl, "torches");
    object_destroy(ob);

    ob = object_load_str("arch sack\nname torch\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_ptr_null(ob->name_pl);
    ob->nrof = 2;
    sb = object_get_display_name(ob, NULL, NULL);
    name = stringbuffer_finish(sb);
    ck_assert_str_eq(name, "torch");
    free(name);
    object_destroy(ob);

    archetype_t *sack = arch_find("sack");
    ck_assert_ptr_nonnull(sack);
    ob = object_load_str("arch sack\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_ptr_eq(ob->name_pl, sack->clone.name_pl);
    ob->nrof = 2;
    sb = object_get_display_name(ob, NULL, NULL);
    name = stringbuffer_finish(sb);
    ck_assert_str_eq(name, sack->clone.name_pl != NULL ? sack->clone.name_pl : ob->name);
    free(name);
    object_destroy(ob);
}
END_TEST

static packet_struct *queued_command_find(socket_struct *cs, uint8_t type) {
    packet_struct *found = NULL;

    for (packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type == type) {
            found = packet;
        }
    }

    return found;
}

START_TEST(test_object_merge_updates_name_and_count) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *first = arch_get("bolt");
    FREE_AND_COPY_HASH(first->name, "torch");
    FREE_AND_COPY_HASH(first->name_pl, "torches");
    first->nrof = 1;
    first = object_insert_into(first, pl, 0);

    object *second = arch_get("bolt");
    FREE_AND_COPY_HASH(second->name, "torch");
    FREE_AND_COPY_HASH(second->name_pl, "torches");
    second->nrof = 1;
    ck_assert_ptr_eq(object_insert_into(second, pl, 0), first);
    ck_assert_uint_eq(first->nrof, 2);

    packet_struct *packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_ITEM_UPDATE);
    ck_assert_ptr_nonnull(packet);
    packet_reader_t reader;
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), UPD_NAME | UPD_NROF);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), first->count);
    char display_name[MAX_BUF];
    ck_assert(packet_reader_read_string(&reader, VS(display_name)));
    ck_assert_str_eq(display_name, "torches");
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 2);
    ck_assert(packet_reader_finish(&reader));

    ck_assert_ptr_eq(object_decrease(first, 1), first);
    ck_assert_uint_eq(first->nrof, 1);
    packet = queued_command_find(CONTR(pl)->cs, CLIENT_CMD_ITEM_UPDATE);
    ck_assert_ptr_nonnull(packet);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), UPD_NAME | UPD_NROF);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), first->count);
    ck_assert(packet_reader_read_string(&reader, VS(display_name)));
    ck_assert_str_eq(display_name, "torch");
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 1);
    ck_assert(packet_reader_finish(&reader));

    char boundary_name[ITEM_NAME_SIZE];
    memset(boundary_name, 'a', sizeof(boundary_name) - 1);
    boundary_name[sizeof(boundary_name) - 1] = '\0';
    FREE_AND_COPY_HASH(first->name, boundary_name);
    packet = packet_new(0, 128, 64);
    add_object_to_packet(packet, first, pl, CMD_APPLY_ACTION_NORMAL, UPD_NAME | UPD_NROF, 0);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), first->count);
    ck_assert(packet_reader_read_string(&reader, VS(display_name)));
    ck_assert_str_eq(display_name, boundary_name);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 1);
    ck_assert(packet_reader_finish(&reader));
    packet_free(packet);

    char oversized_name[ITEM_NAME_SIZE + 1U];
    memset(oversized_name, 'b', sizeof(oversized_name) - 1);
    oversized_name[sizeof(oversized_name) - 1] = '\0';
    FREE_AND_COPY_HASH(first->name, oversized_name);
    packet = packet_new(0, 128, 64);
    add_object_to_packet(packet, first, pl, CMD_APPLY_ACTION_NORMAL, UPD_NAME | UPD_NROF, 0);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), first->count);
    ck_assert(packet_reader_read_string(&reader, VS(display_name)));
    ck_assert_uint_eq(strlen(display_name), ITEM_NAME_SIZE - 1U);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 1);
    ck_assert(packet_reader_finish(&reader));
    packet_free(packet);

    char utf8_name[ITEM_NAME_SIZE + 2U];
    memset(utf8_name, 'c', ITEM_NAME_SIZE - 2U);
    memcpy(utf8_name + ITEM_NAME_SIZE - 2U, "\xe2\x82\xac", 3);
    utf8_name[ITEM_NAME_SIZE + 1U] = '\0';
    FREE_AND_COPY_HASH(first->custom_name, utf8_name);
    packet = packet_new(0, 128, 64);
    add_object_to_packet(packet, first, pl, CMD_APPLY_ACTION_NORMAL, UPD_NAME | UPD_NROF, 0);
    packet_reader_init(&reader, packet->data, packet->len);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), first->count);
    ck_assert(packet_reader_read_string(&reader, VS(display_name)));
    ck_assert_uint_eq(strlen(display_name), ITEM_NAME_SIZE - 2U);
    ck_assert_uint_eq(packet_reader_read_uint32(&reader), 1);
    ck_assert(packet_reader_finish(&reader));
    packet_free(packet);

    object_destroy(pl);
}
END_TEST

START_TEST(test_object_weight_sum) {
    object *ob1, *ob2, *ob3, *ob4;
    unsigned long sum;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    ob4 = arch_get("sack");
    ob1->weight = 10;
    ob1->type = CONTAINER;
    /* 40% reduction of weight */
    ob1->weapon_speed = 0.6f;
    ob2->weight = 6;
    ob2->nrof = 10;
    ob3->weight = 7;
    ob4->weight = 8;
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob1, 0);
    object_insert_into(ob4, ob1, 0);
    sum = object_weight_sum(ob1);
    ck_assert_uint_eq(sum, 45);
    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_weight_add) {
    object *ob1, *ob2, *ob3, *ob4;
    unsigned long sum;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    ob4 = arch_get("sack");
    ob1->weight = 10;
    ob1->type = CONTAINER;
    ob2->type = CONTAINER;
    ob3->type = CONTAINER;
    /* 40% reduction of weight */
    ob1->weapon_speed = 0.6f;
    ob2->weight = 10;
    ob3->weight = 10;
    ob4->weight = 10;
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob2, 0);
    object_insert_into(ob4, ob3, 0);
    sum = object_weight_sum(ob1);
    ck_assert_uint_eq(sum, 18);
    object_weight_add(ob4, 10);
    ck_assert_int_eq(ob1->carrying, 24);
    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_weight_sub) {
    object *ob1, *ob2, *ob3, *ob4;
    unsigned long sum;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    ob4 = arch_get("sack");
    ob1->weight = 10;
    ob1->type = CONTAINER;
    ob2->type = CONTAINER;
    ob3->type = CONTAINER;
    /* 40% reduction of weight */
    ob1->weapon_speed = 0.6f;
    ob2->weight = 10;
    ob3->weight = 10;
    ob4->weight = 10;
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob2, 0);
    object_insert_into(ob4, ob3, 0);
    sum = object_weight_sum(ob1);
    ck_assert_uint_eq(sum, 18);
    object_weight_sub(ob4, 10);
    ck_assert_int_eq(ob1->carrying, 12);
    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_get_env) {
    object *ob1, *ob2, *ob3, *ob4, *result;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    ob4 = arch_get("sack");
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob2, 0);
    object_insert_into(ob4, ob3, 0);
    result = object_get_env(ob4);
    ck_assert_ptr_eq(result, ob1);
    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_is_in_inventory) {
    object *ob1, *ob2, *ob3, *ob4;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    ob4 = arch_get("sack");
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob2, 0);
    object_insert_into(ob4, ob3, 0);

    ck_assert(object_is_in_inventory(ob2, ob1));
    ck_assert(object_is_in_inventory(ob3, ob1));
    ck_assert(object_is_in_inventory(ob3, ob2));
    ck_assert(object_is_in_inventory(ob4, ob1));
    ck_assert(object_is_in_inventory(ob4, ob2));
    ck_assert(object_is_in_inventory(ob4, ob3));

    ck_assert(!object_is_in_inventory(ob1, ob1));
    ck_assert(!object_is_in_inventory(ob2, ob2));
    ck_assert(!object_is_in_inventory(ob3, ob3));
    ck_assert(!object_is_in_inventory(ob4, ob4));

    ck_assert(!object_is_in_inventory(ob1, ob2));
    ck_assert(!object_is_in_inventory(ob2, ob3));
    ck_assert(!object_is_in_inventory(ob3, ob4));

    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_dump) {
    object *ob1, *ob2, *ob3;
    StringBuffer *sb;
    char *result;

    ob1 = arch_get("sack");
    ob2 = arch_get("sack");
    ob3 = arch_get("sack");
    object_insert_into(ob2, ob1, 0);
    object_insert_into(ob3, ob2, 0);
    sb = stringbuffer_new();
    object_dump(ob1, sb);
    result = stringbuffer_finish(sb);
    ck_assert(string_startswith(result, "arch"));
    free(result);
    object_destroy(ob1);
}
END_TEST

START_TEST(test_object_insert_map) {
    mapstruct *map;
    object *first, *second, *third, *floor_ob, *got;

    map = get_empty_map(5, 5);
    ck_assert_ptr_ne(map, NULL);

    /* First, simple tests for insertion. */
    floor_ob = arch_get("water_still");
    floor_ob->x = 3;
    floor_ob->y = 3;
    got = object_insert_map(floor_ob, map, NULL, 0);
    ck_assert_ptr_eq(floor_ob, got);
    ck_assert_ptr_eq(floor_ob, GET_MAP_OB(map, 3, 3));

    first = arch_get("letter");
    first->x = 3;
    first->y = 3;
    got = object_insert_map(first, map, NULL, 0);
    ck_assert_ptr_eq(got, first);
    ck_assert_ptr_eq(floor_ob, GET_MAP_OB(map, 3, 3));
    ck_assert_ptr_eq(floor_ob->above, first);

    second = arch_get("bolt");
    second->nrof = 1;
    second->x = 3;
    second->y = 3;
    got = object_insert_map(second, map, NULL, 0);
    ck_assert_ptr_eq(got, second);
    ck_assert_ptr_eq(floor_ob, GET_MAP_OB(map, 3, 3));
    ck_assert_ptr_eq(floor_ob->above, second);
    ck_assert_ptr_eq(second->above, first);

    /* Merging tests. */
    third = arch_get("bolt");
    third->nrof = 1;
    third->x = 3;
    third->y = 3;
    got = object_insert_map(third, map, NULL, 0);
    ck_assert_ptr_eq(got, third);
    ck_assert(OBJECT_FREE(second));
    ck_assert_uint_eq(third->nrof, 2);

    second = arch_get("bolt");
    second->nrof = 1;
    second->x = 3;
    second->y = 3;
    second->value = 1;
    got = object_insert_map(second, map, NULL, 0);
    ck_assert_ptr_eq(got, second);
    ck_assert_uint_eq(second->nrof, 1);
    ck_assert_uint_eq(third->nrof, 2);
}
END_TEST

START_TEST(test_object_decrease) {
    object *first, *second;

    first = arch_get("bolt");
    first->nrof = 5;

    second = object_decrease(first, 3);
    ck_assert_ptr_eq(second, first);
    ck_assert(!OBJECT_FREE(first));

    second = object_decrease(first, 2);
    ck_assert_ptr_eq(second, NULL);
    ck_assert(OBJECT_FREE(first));

    first = arch_get("bolt");
    first->nrof = 5;

    second = object_decrease(first, 5);
    ck_assert_ptr_eq(second, NULL);
    ck_assert(OBJECT_FREE(first));

    first = arch_get("bolt");
    first->nrof = 5;

    second = object_decrease(first, 50);
    ck_assert_ptr_eq(second, NULL);
    ck_assert(OBJECT_FREE(first));
}
END_TEST

START_TEST(test_object_insert_into) {
    object *container, *item;

    item = arch_get("bolt");
    item->weight = 50;

    container = arch_get("sack");
    object_insert_into(item, container, 0);
    ck_assert_ptr_eq(container->inv, item);
    ck_assert_int_eq(container->carrying, 50);

    object_remove(item, 0);
    ck_assert_int_eq(container->carrying, 0);

    /* 50% weight reduction. */
    container->weapon_speed = 0.5f;

    object_insert_into(item, container, 0);
    ck_assert_ptr_eq(container->inv, item);
    ck_assert_int_eq(container->carrying, 25);

    object_destroy(container);
}
END_TEST

START_TEST(test_object_can_pick) {
    mapstruct *map;
    object *pl, *ob;

    check_setup_env_pl(&map, &pl);

    ob = arch_get("sack");
    ck_assert(object_can_pick(pl, ob));
    ob->weight = 0;
    ck_assert(!object_can_pick(pl, ob));
    object_destroy(ob);

    ob = arch_get("sack");
    SET_FLAG(ob, FLAG_NO_PICK);
    ck_assert(!object_can_pick(pl, ob));
    SET_FLAG(ob, FLAG_UNPAID);
    ck_assert(object_can_pick(pl, ob));
    object_destroy(ob);

    ob = arch_get("sack");
    SET_FLAG(ob, FLAG_IS_INVISIBLE);
    ck_assert(!object_can_pick(pl, ob));
    object_destroy(ob);

    ob = arch_get("raas");
    ck_assert(!object_can_pick(pl, ob));
    object_destroy(ob);
}
END_TEST

START_TEST(test_object_clone) {
    object *ob, *clone_ob;

    ob = arch_get("raas");
    object_insert_into(arch_get("sack"), ob, 0);
    clone_ob = object_clone(ob);
    ck_assert_str_eq(clone_ob->name, ob->name);
    ck_assert_ptr_ne(clone_ob->inv, NULL);
    ck_assert_str_eq(clone_ob->inv->name, ob->inv->name);

    object_destroy(ob);
    object_destroy(clone_ob);
}
END_TEST

START_TEST(test_object_load_str) {
    object *ob;

    ob = object_load_str("arch sack\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_str_eq(ob->arch->name, "sack");
    ck_assert_uint_eq(ob->light_color, UINT32_C(0xffffff));
    object_destroy(ob);

    ob = object_load_str("arch sack\ndirection -1\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_int_eq(ob->direction, NUM_DIRECTION);
    object_destroy(ob);

    ob = object_load_str("arch sack\ndirection -2147483648\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_int_ge(ob->direction, 0);
    ck_assert_int_le(ob->direction, NUM_DIRECTION);
    object_destroy(ob);

    ob = object_load_str("arch sack\nlight_color 12aBcF\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_uint_eq(ob->light_color, UINT32_C(0x12abcf));
    StringBuffer *sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    char *dump = stringbuffer_finish(sb);
    ck_assert_ptr_ne(strstr(dump, "light_color 12abcf\n"), NULL);
    free(dump);
    object *clone = object_clone(ob);
    ck_assert_uint_eq(clone->light_color, ob->light_color);
    object_destroy(clone);
    object_destroy(ob);

    ck_assert_ptr_eq(object_load_str("arch sack\nlight_color fffff\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch sack\nlight_color #ffffff\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch sack\nlight_color fffffg\nend\n"), NULL);

    ob = object_load_str("arch sack\nname magic sack\nweight 129\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_str_eq(ob->name, "magic sack");
    ck_assert_int_eq(ob->weight, 129);
    object_destroy(ob);

    ob = object_load_str("arch sack\narch sword\narch sword\ntitle of swords\n"
                         "end\nend\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_str_eq(ob->arch->name, "sack");
    ck_assert_ptr_ne(ob->inv, NULL);
    ck_assert_str_eq(ob->inv->arch->name, "sword");
    ck_assert_ptr_eq(ob->inv->title, NULL);
    ck_assert_ptr_ne(ob->inv->inv, NULL);
    ck_assert_str_eq(ob->inv->inv->arch->name, "sword");
    ck_assert_str_eq(ob->inv->inv->title, "of swords");
    object_destroy(ob);
}
END_TEST

START_TEST(test_object_stable_identity_lookup) {
    for (int i = 0; i < NROFREALSPELLS; i++) {
        ck_assert_int_eq(spell_index_from_id(spells[i].id), i);
        ck_assert_str_eq(spell_id_from_index(i), spells[i].id);
    }

    ck_assert_int_eq(spell_index_from_id("unknown"), SP_NO_SPELL);
    ck_assert_int_eq(spell_index_from_id(NULL), SP_NO_SPELL);
    ck_assert_ptr_eq(spell_id_from_index(SP_NO_SPELL), NULL);

    for (int i = 0; i < NROFSKILLS; i++) {
        ck_assert_int_eq(skill_index_from_id(skills[i].id), i);
        ck_assert_str_eq(skill_id_from_index(i), skills[i].id);
    }

    ck_assert_int_eq(skill_index_from_id("unknown"), -1);
    ck_assert_int_eq(skill_index_from_id(NULL), -1);
    ck_assert_ptr_eq(skill_id_from_index(-1), NULL);
    ck_assert_ptr_eq(skill_id_from_index(NROFSKILLS), NULL);
}
END_TEST

START_TEST(test_object_stable_identity_serialization) {
    object *ob = object_load_str("arch bolt\ncustody_lineage 42\n"
                                 "custody_first account:original\n"
                                 "custody_last account:latest\n"
                                 "custody_actor account:opaque-character\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_str_eq(ob->custody_lineage, "42");
    ck_assert_str_eq(ob->custody_first, "account:original");
    ck_assert_str_eq(ob->custody_last, "account:latest");
    ck_assert_str_eq(ob->custody_actor, "account:opaque-character");
    StringBuffer *sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    char *dump = stringbuffer_finish(sb);
    ck_assert_ptr_ne(strstr(dump, "custody_lineage 42\n"), NULL);
    ck_assert_ptr_ne(strstr(dump, "custody_first account:original\n"), NULL);
    ck_assert_ptr_ne(strstr(dump, "custody_last account:latest\n"), NULL);
    ck_assert_ptr_ne(strstr(dump, "custody_actor account:opaque-character\n"), NULL);
    free(dump);
    object_destroy(ob);

    ob = object_load_str("arch wand\nspell_id spell_firestorm\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_int_eq(ob->stats.sp, SP_FIRESTORM);
    sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    dump = stringbuffer_finish(sb);
    ck_assert_ptr_ne(strstr(dump, "spell_id spell_firestorm\n"), NULL);
    ck_assert_ptr_eq(strstr(dump, "\nsp "), NULL);
    free(dump);
    object_destroy(ob);

    ob = object_load_str("arch skill_literacy\nskill_id skill_throwing\nend\n");
    ck_assert_ptr_ne(ob, NULL);
    ck_assert_int_eq(ob->stats.sp, SK_THROWING);
    sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    dump = stringbuffer_finish(sb);
    ck_assert_ptr_ne(strstr(dump, "skill_id skill_throwing\n"), NULL);
    ck_assert_ptr_eq(strstr(dump, "\nsp "), NULL);
    free(dump);
    object_destroy(ob);
}
END_TEST

START_TEST(test_object_stable_identity_validation) {
    object *ob;

    ck_assert_ptr_eq(object_load_str("arch wand\nsp 19\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch wand\nspell_id unknown\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch sack\nspell_id spell_firestorm\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch sack\nskill_id skill_literacy\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch wand\narch wand\nsp 19\nend\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch wand\nsp -1\nspell_id spell_firestorm\nend\n"), NULL);
    ck_assert_ptr_eq(object_load_str("arch wand\nspell_id spell_firestorm\n"
                                     "spell_id spell_icestorm\nend\n"),
                     NULL);
    ck_assert_ptr_eq(object_load_str("arch skill_literacy\nskill_id skill_literacy\n"
                                     "skill_id skill_throwing\nend\n"),
                     NULL);

    ob = object_load_str("arch sack\nspell_id spell_firestorm\ntype 109\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_int_eq(ob->stats.sp, spell_index_from_id("spell_firestorm"));
    object_destroy(ob);

    ob = object_load_str("arch sack\nskill_id skill_literacy\ntype 43\nend\n");
    ck_assert_ptr_nonnull(ob);
    ck_assert_int_eq(ob->stats.sp, skill_index_from_id("skill_literacy"));
    object_destroy(ob);

    ob = object_load_str("arch wand\nend\n");
    ck_assert_ptr_nonnull(ob);
    ob->stats.sp = 9999;
    StringBuffer *sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    char *dump = stringbuffer_finish(sb);
    ck_assert_ptr_nonnull(strstr(dump, "spell_id __invalid_runtime_index_9999"));
    ck_assert_ptr_eq(strstr(dump, "\nsp 9999"), NULL);
    free(dump);
    object_destroy(ob);

    ob = object_load_str("arch wand\nend\n");
    ck_assert_ptr_nonnull(ob);
    int original_spell = ob->stats.sp;
    ck_assert_int_eq(set_variable(ob, "sp 19\n"), LL_ERROR);
    ck_assert_int_eq(ob->stats.sp, original_spell);
    object_destroy(ob);
}
END_TEST

START_TEST(test_object_stable_identity_file_ordering) {
    FILE *fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ge(fputs("arch sack\nspell_id spell_firestorm\ntype 109\nend\n", fp), 0);
    rewind(fp);
    object *ob = object_get();
    ck_assert_int_eq(load_object_fp(fp, ob, 0), LL_NORMAL);
    ck_assert_int_eq(ob->stats.sp, SP_FIRESTORM);
    object_destroy(ob);
    fclose(fp);

    fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ge(fputs("arch sack\nsp 19\ntype 109\nend\n", fp), 0);
    rewind(fp);
    ob = object_get();
    ck_assert_int_eq(load_object_fp(fp, ob, 0), LL_ERROR);
    object_destroy(ob);
    fclose(fp);
}
END_TEST

START_TEST(test_object_reverse_inventory) {
    char *cp, *cp2;
    object *ob;
    StringBuffer *sb;

    cp = path_file_contents(ATRINIK_TEST_DATA_DIR "/test_object_reverse_inventory.arc");
    ob = object_load_str(cp);

    object_reverse_inventory(ob);

    sb = stringbuffer_new();
    object_dump_rec(ob, sb);
    cp2 = stringbuffer_finish(sb);

    ck_assert_str_eq(cp, cp2);

    object_destroy(ob);
    free(cp);
    free(cp2);
}
END_TEST

START_TEST(test_object_create_singularity) {
    object *obj;

    obj = object_create_singularity("JO3584jke");
    ck_assert_ptr_ne(obj, NULL);
    ck_assert_ptr_ne(obj->name, NULL);
    ck_assert(strstr(obj->name, "JO3584jke") != NULL);
    object_destroy(obj);

    obj = object_create_singularity(NULL);
    ck_assert_ptr_ne(obj, NULL);
    ck_assert_ptr_ne(obj->name, NULL);
    ck_assert(strstr(obj->name, "singularity") != NULL);
    object_destroy(obj);
}
END_TEST

START_TEST(test_invalid_object_type_uses_base_method_fallback) {
    object *ob = arch_get("sack");
    uint8_t original_type = ob->type;

    ob->type = UINT8_MAX;
    ck_assert_ptr_ne(object_methods_get(ob->type), NULL);
    object_cb_deinit(ob);

    ob->type = original_type;
    object_destroy(ob);
}
END_TEST

START_TEST(test_OBJECT_DESTROYED) {
    object *ob, *ob2;
    tag_t ob_tag, ob2_tag;
    mapstruct *m;

    m = get_empty_map(1, 1);
    ck_assert_ptr_ne(m, NULL);

    ob = arch_get("sack");
    ob_tag = ob->count;
    object_insert_map(ob, m, ob, 0);
    ck_assert(!OBJECT_DESTROYED(ob, ob_tag));
    ob2 = arch_get("bolt");
    ob2_tag = ob2->count;
    object_insert_into(ob2, ob, 0);
    ck_assert(!OBJECT_DESTROYED(ob2, ob2_tag));
    object_remove(ob, 0);
    ck_assert(!OBJECT_DESTROYED(ob, ob_tag));
    object_destroy(ob);
    ck_assert(OBJECT_DESTROYED(ob, ob_tag));
    ck_assert(OBJECT_DESTROYED(ob2, ob2_tag));
}
END_TEST

START_TEST(test_object_map_reload_preserves_active_list) {
    ck_assert_ptr_eq(active_objects, NULL);

    object *player = arch_get("sack");
    player->speed = 1.0f;
    object_update_speed(player);
    ck_assert_ptr_eq(active_objects, player);

    for (int cycle = 0; cycle < 3; cycle++) {
        mapstruct *map = get_empty_map(1, 1);
        ck_assert_ptr_ne(map, NULL);

        object *spawn = object_load_str("arch spawn_point\n"
                                        "arch lom_lobon\n"
                                        "type 83\n"
                                        "arch ability_firestorm\n"
                                        "end\n"
                                        "arch ability_firestorm\n"
                                        "end\n"
                                        "end\n"
                                        "end\n");
        ck_assert_ptr_ne(spawn, NULL);
        ck_assert_ptr_ne(spawn->inv, NULL);
        ck_assert_int_eq(spawn->inv->type, SPAWN_POINT_MOB);
        ck_assert_ptr_ne(spawn->inv->inv, NULL);
        ck_assert_ptr_ne(spawn->inv->inv->below, NULL);

        spawn->x = 0;
        spawn->y = 0;
        ck_assert_ptr_eq(object_insert_map(spawn, map, NULL, 0), spawn);

        object *template = spawn->inv;
        object *template_slot = template;
        template->type = MONSTER;
        object_update_speed(template);
        ck_assert_ptr_eq(active_objects, template);
        ck_assert_ptr_eq(template->active_next, spawn);

        /* Reproduce a template whose type changed after it became active.
         * Map destruction must unlink it despite its non-zero speed. */
        template->type = SPAWN_POINT_MOB;
        delete_map(map);

        ck_assert_ptr_eq(active_objects, player);
        ck_assert_ptr_eq(player->active_prev, NULL);
        ck_assert(!active_list_contains(template_slot));

        object *claimed[8];
        size_t claimed_count = 0;
        bool template_reused = false;

        while (claimed_count < arraysize(claimed)) {
            claimed[claimed_count] = object_get();
            template_reused = claimed[claimed_count] == template_slot;
            claimed_count++;

            ck_assert_ptr_eq(active_objects, player);
            ck_assert(!active_list_contains(template_slot));

            if (template_reused) {
                break;
            }
        }

        ck_assert(template_reused);
        ck_assert_ptr_eq(claimed[claimed_count - 1]->active_next, NULL);
        ck_assert_ptr_eq(claimed[claimed_count - 1]->active_prev, NULL);

        while (claimed_count > 0) {
            object_destroy(claimed[--claimed_count]);
        }
    }

    object_destroy(player);
    ck_assert_ptr_eq(active_objects, NULL);
}
END_TEST

START_TEST(test_underground_city_map_reloads_preserve_active_list) {
    static const char *path =
        "/shattered_islands/strakewood_island/underground_city/underground_city_5_3_-1";

    ck_assert_ptr_eq(active_objects, NULL);

    object *player = arch_get("sack");
    player->speed = 1.0f;
    object_update_speed(player);

    for (int cycle = 0; cycle < 3; cycle++) {
        mapstruct *map = ready_map_name(path, NULL, cycle == 0 ? MAP_FLUSH : 0);
        ck_assert_ptr_ne(map, NULL);

        size_t matching_templates = 0;
        for (int x = 0; x < MAP_WIDTH(map); x++) {
            for (int y = 0; y < MAP_HEIGHT(map); y++) {
                for (object *spawn = GET_MAP_OB(map, x, y); spawn != NULL; spawn = spawn->above) {
                    if (spawn->type != SPAWN_POINT || spawn->inv == NULL ||
                        spawn->inv->type != SPAWN_POINT_MOB || spawn->inv->inv == NULL ||
                        spawn->inv->inv->below == NULL) {
                        continue;
                    }

                    matching_templates++;
                    ck_assert(!active_list_contains_at(spawn->inv, "loaded-map template scan"));
                }
            }
        }

        ck_assert_uint_gt(matching_templates, 0);
        ck_assert(active_list_contains_at(player, "before map swap"));

        swap_map(map, 1);

        ck_assert(active_list_contains_at(player, "after map swap"));
    }

    object_destroy(player);
    ck_assert_ptr_eq(active_objects, NULL);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("object");
    TCase *tc_core = tcase_create("Core");
    TCase *tc_movement = tcase_create("Movement");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    tcase_add_unchecked_fixture(tc_movement, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_movement, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_return_home_waypoint_reactivation_resets_retry_progress);
    suite_add_tcase(s, tc_movement);
    tcase_add_test(tc_movement, test_move_ob_rejects_invalid_directions);
    tcase_add_test(tc_core, test_find_enemy_returns_valid_direction_for_tiled_exit_enemy);
    tcase_add_test(tc_core, test_find_enemy_returns_valid_direction_for_exit_tiled_enemy);
    tcase_add_test(tc_core, test_find_enemy_returns_valid_direction_for_tiled_exit_tiled_enemy);
    tcase_add_test(tc_core, test_object_can_merge);
    tcase_add_test(tc_core, test_object_custody_provenance);
    tcase_add_test(tc_core, test_object_plural_name_contract);
    tcase_add_test(tc_core, test_object_merge_updates_name_and_count);
    tcase_add_test(tc_core, test_map_stack_operations_increment_update_once);
    tcase_add_test(tc_core, test_object_weight_sum);
    tcase_add_test(tc_core, test_object_weight_add);
    tcase_add_test(tc_core, test_object_weight_sub);
    tcase_add_test(tc_core, test_object_get_env);
    tcase_add_test(tc_core, test_object_is_in_inventory);
    tcase_add_test(tc_core, test_object_dump);
    tcase_add_test(tc_core, test_object_insert_map);
    tcase_add_test(tc_core, test_object_decrease);
    tcase_add_test(tc_core, test_object_insert_into);
    tcase_add_test(tc_core, test_object_can_pick);
    tcase_add_test(tc_core, test_object_clone);
    tcase_add_test(tc_core, test_object_load_str);
    tcase_add_test(tc_core, test_object_stable_identity_lookup);
    tcase_add_test(tc_core, test_object_stable_identity_serialization);
    tcase_add_test(tc_core, test_object_stable_identity_validation);
    tcase_add_test(tc_core, test_object_stable_identity_file_ordering);
    tcase_add_test(tc_core, test_object_reverse_inventory);
    tcase_add_test(tc_core, test_object_create_singularity);
    tcase_add_test(tc_core, test_invalid_object_type_uses_base_method_fallback);
    tcase_add_test(tc_core, test_OBJECT_DESTROYED);
    tcase_add_test(tc_core, test_object_map_reload_preserves_active_list);
    tcase_add_test(tc_core, test_underground_city_map_reloads_preserve_active_list);

    return s;
}

void check_server_object(void) {
    check_run_suite(suite(), __FILE__);
}
