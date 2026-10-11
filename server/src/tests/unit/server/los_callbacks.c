/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Regression coverage for MAP2's publication transaction around
 * EVENT_ASK_SHOW.  The Python fixture is deliberately used here: C callbacks
 * cannot exercise object insertion, relocation, and flag setters through the
 * plugin boundary which is where the publication epoch must be advanced.
 */
#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <atrinik/protocol/target.h>
#include <arch.h>
#include <initialization.h>
#include <los.h>
#include <map.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <plugin.h>
#include <server.h>
#include <toolkit/packet.h>

static bool plugins_initialized;

static void fixture(mapstruct **map, object **pl) {
    /* This suite assembles and inspects queued MAP2 packets directly. Unlike
     * request's wider setup, it must claim the packet toolkit explicitly. */
    toolkit_import(packet);
    check_setup_env_pl(map, pl);
    object_remove(*pl, 0);
    (*pl)->x = 12;
    (*pl)->y = 12;
    *pl = object_insert_map(*pl, *map, NULL, 0);
    ck_assert_ptr_nonnull(*pl);
    CONTR(*pl)->tls = 0;
    CLEAR_FLAG(*pl, FLAG_BLIND);
    CLEAR_FLAG(*pl, FLAG_XRAYS);

    if (!plugins_initialized) {
        init_plugins();
        plugins_initialized = true;
    }
}

static object *place(mapstruct *map, const char *arch, int x, int y) {
    object *op = arch_get(arch);
    ck_assert_ptr_nonnull(op);
    op->x = x;
    op->y = y;
    op = object_insert_map(op, map, NULL, 0);
    ck_assert_ptr_nonnull(op);
    return op;
}

static mapstruct *upper_level(mapstruct *base) {
    mapstruct *upper = get_empty_map(24, 24);
    ck_assert_ptr_nonnull(upper);
    FREE_AND_COPY_HASH(upper->path, "/tests/los-callback-upper");
    base->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = base;
    upper->coords[2] = 1;
    base->level_min = 0;
    base->level_max = upper->level_max = 1;
    upper->level_min = 0;
    return upper;
}

static void ask_show(object *actor, const char *options) {
    object *event = object_get();
    event->type = EVENT_OBJECT;
    event->sub_type = EVENT_ASK_SHOW;
    FREE_AND_COPY_HASH(event->name, "Python");
    FREE_AND_COPY_HASH(event->race, "python/tests/unit/los_ask_show.py");
    FREE_AND_COPY_HASH(event->slaying, options);
    object_insert_into(event, actor, INS_NO_MERGE);
    ck_assert(actor->event_flags & EVENT_FLAG(EVENT_ASK_SHOW));
}

static void ask_show_other(object *actor, const char *options, object *other) {
    FREE_AND_COPY_HASH(other->name, "los-callback-earlier-actor");
    ck_assert(object_set_value(actor, "los_ask_show_other_name", other->name, true));
    ask_show(actor, options);
}

static void name_player(object *pl, const char *name) {
    FREE_AND_COPY_HASH(pl->name, name);
    player *controller = CONTR(pl);
    controller->cmd_permissions = xcalloc(1, sizeof(*controller->cmd_permissions));
    controller->cmd_permissions[0] = xstrdup("kick");
    controller->num_cmd_permissions = 1;
}

static object *additional_player(mapstruct *map, const char *name, int x, int y) {
    object *pl = player_get_dummy(name, NULL);
    ck_assert_ptr_nonnull(pl);
    object_remove(pl, 0);
    pl->x = x;
    pl->y = y;
    pl = object_insert_map(pl, map, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    name_player(pl, name);
    return pl;
}

static size_t queued_map_packets(const socket_struct *cs) {
    size_t count = 0;
    for (const packet_struct *packet = cs->packets; packet != NULL; packet = packet->next) {
        count += packet->type == CLIENT_CMD_MAP;
    }
    return count;
}

static void assert_aborted(object *pl) {
    player *view = CONTR(pl);
    socket_struct *cs = view->cs;
    ck_assert_uint_eq(queued_map_packets(cs), 0);
    ck_assert_ptr_null(cs->target_map_published_map);
    ck_assert_uint_eq(cs->target_map_published_count, 0);
    ck_assert_uint_eq(cs->target_map_published_depths, 0);
    ck_assert(!cs->target_map_publication_pending);
    ck_assert_ptr_null(map_client_cache_cell(&cs->lastmap, 0, cs->mapx_2, cs->mapy_2, false));
    ck_assert_uint_eq(cs->lastmap_light_generation, 0);
    ck_assert_ptr_null(view->last_update);
    ck_assert(!view->los_view_valid);
    ck_assert(view->update_los);
    ck_assert(!view->map2_serializing);
}

static void assert_fresh_publication(object *pl) {
    socket_struct *cs = CONTR(pl)->cs;
    socket_buffer_clear(cs);
    draw_client_map(pl);
    ck_assert_uint_gt(queued_map_packets(cs), 0);
    ck_assert_ptr_eq(cs->target_map_published_map, pl->map);
    ck_assert_uint_eq(cs->target_map_published_count, pl->map->count);
    ck_assert_uint_ne(cs->target_map_published_depths, 0);
    ck_assert_ptr_nonnull(map_client_cache_cell(&cs->lastmap, 0, cs->mapx_2, cs->mapy_2, false));
}

static MapCell *actor_cache_cell(socket_struct *cs, const object *pl, const object *actor) {
    return map_client_cache_cell(&cs->lastmap, 0,
                                 cs->mapx_2 + actor->x - pl->x,
                                 cs->mapy_2 + actor->y - pl->y, false);
}

static void queue_target_select(socket_struct *cs, uint8_t x, uint8_t y, int8_t depth,
                                tag_t actor_count) {
    uint8_t frame[] = {
        SERVER_CMD_TARGET,
        CMD_TARGET_MAPXY,
        x,
        y,
        (uint8_t)depth,
        (uint8_t)(actor_count >> 24),
        (uint8_t)(actor_count >> 16),
        (uint8_t)(actor_count >> 8),
        (uint8_t)actor_count,
    };
    ck_assert_uint_eq(sizeof(frame), 1U + TARGET_SELECT_PAYLOAD_SIZE);
    ck_assert(socket_server_command_queue_append(cs, frame, sizeof(frame)));
}

START_TEST(test_ask_show_blocker_aborts_stale_map2_and_recovers_fresh) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *actor = place(map, "raas", 13, 12);
    ask_show(actor, "blocker");

    socket_buffer_clear(CONTR(pl)->cs);
    uint64_t before = los_publication_epoch();
    draw_client_map2(pl);
    ck_assert_uint_gt(los_publication_epoch(), before);
    assert_aborted(pl);

    assert_fresh_publication(pl);
}
END_TEST

/* The client-target queue reaches the normal command dispatcher, whose target
 * authorization invokes ASK_SHOW before accepting the exact 1084 MAPXY
 * identity. The callback frees its own observer while handle_newcs_player is
 * still unwinding the command loop. */
START_TEST(test_queued_target_kick_observer_withdraws_dispatch_authority) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    name_player(observer, "los-target-kick-observer");
    object *actor = place(map, "raas", 13, 12);
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    draw_client_map(observer);
    socket_buffer_clear(socket);
    ask_show(actor, "kick_observer");
    tag_t count = observer->count;

    queue_target_select(socket, socket->mapx_2 + actor->x - observer->x,
                        socket->mapy_2 + actor->y - observer->y, 0, actor->count);
    ck_assert_int_eq(handle_newcs_player(controller), -1);
    ck_assert(!player_connection_is_current(observer, count, controller, socket));
}
END_TEST

/* This reaches draw_client_map2's own callback-abort cleanup: no retained
 * target invokes ASK_SHOW beforehand, and /kick frees the saved controller
 * and socket while MAP2 is being assembled. */
START_TEST(test_ask_show_kicking_observer_during_direct_map2_withdraws_authority) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    name_player(observer, "los-direct-kick-observer");
    object *actor = place(map, "raas", 13, 12);
    ask_show(actor, "kick_observer");
    player *controller = CONTR(observer);
    socket_struct *socket = controller->cs;
    tag_t count = observer->count;

    draw_client_map2(observer);

    /* The helper walks the live list before examining a captured pointer, so
     * this proves no MAP2 authority remains without dereferencing freed state. */
    ck_assert(!player_connection_is_current(observer, count, controller, socket));
    (void)map;
}
END_TEST

/* ExecuteCommand('/kick') synchronously calls player_logout(), which frees
 * both controller and socket. Retained-target authorization invokes ASK_SHOW
 * before MAP2, and post-process must skip the freed snapshot while continuing
 * to the next live player. */
START_TEST(test_ask_show_kicking_observer_withdraws_snapshot_and_continues_postprocess) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    name_player(observer, "los-kick-observer");
    object *survivor = additional_player(map, "los-kick-survivor", 10, 12);
    object *actor = place(map, "raas", 13, 12);
    player *observer_controller = CONTR(observer);
    socket_struct *observer_socket = observer_controller->cs;
    /* Publish the retained target before attaching its callback: post-process
     * must run ASK_SHOW through target authorization, before map drawing. */
    draw_client_map(observer);
    socket_buffer_clear(observer_socket);
    ask_show(actor, "kick_observer");
    tag_t observer_count = observer->count;
    player *survivor_controller = CONTR(survivor);
    socket_struct *survivor_socket = survivor_controller->cs;
    tag_t survivor_count = survivor->count;
    observer_controller->target_object = actor;
    observer_controller->target_object_count = actor->count;

    socket_server_post_process();

    ck_assert(!player_connection_is_current(observer, observer_count,
                                             observer_controller, observer_socket));
    ck_assert(player_connection_is_current(survivor, survivor_count,
                                            survivor_controller, survivor_socket));
    ck_assert_ptr_eq(survivor_socket->target_map_published_map, map);
    ck_assert_uint_eq(survivor_socket->target_map_published_count, map->count);
    player_logout(survivor_controller);
}
END_TEST

/* The callback deletes a later player already captured by post-process. Its
 * stale snapshot must be rejected without touching the freed socket. */
START_TEST(test_ask_show_kicking_next_player_rejects_postprocess_snapshot) {
    mapstruct *map;
    object *observer;
    fixture(&map, &observer);
    name_player(observer, "los-kick-current");
    object *next = additional_player(map, "los-kick-next", 10, 12);
    object *actor = place(map, "raas", 13, 12);
    player *observer_controller = CONTR(observer);
    socket_struct *observer_socket = observer_controller->cs;
    draw_client_map(observer);
    socket_buffer_clear(observer_socket);
    ck_assert(object_set_value(actor, "los_ask_show_kick_name", next->name, true));
    ask_show(actor, "kick_other");
    tag_t observer_count = observer->count;
    player *next_controller = CONTR(next);
    socket_struct *next_socket = next_controller->cs;
    tag_t next_count = next->count;
    observer_controller->target_object = actor;
    observer_controller->target_object_count = actor->count;

    socket_server_post_process();

    ck_assert(player_connection_is_current(observer, observer_count,
                                            observer_controller, observer_socket));
    ck_assert(!player_connection_is_current(next, next_count, next_controller, next_socket));
    ck_assert_ptr_eq(observer_socket->target_map_published_map, map);
    ck_assert_uint_eq(observer_socket->target_map_published_count, map->count);
    player_logout(observer_controller);
}
END_TEST

/* The east actor is serialized while the observer's trusted light override is
 * live. The later callback removes that observer state, so publishing the
 * already framed actor would disclose a dark live identity. */
START_TEST(test_ask_show_viewstate_change_withdraws_earlier_dark_actor) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->light_value = -80;
    CONTR(pl)->tli = 1;
    object *earlier = place(map, "raas", 14, 12);
    object *later = place(map, "raas", 13, 12);
    ask_show(later, "viewstate");

    update_los(pl);
    socket_struct *cs = CONTR(pl)->cs;
    socket_buffer_clear(cs);
    uint64_t before = los_publication_epoch();
    draw_client_map2(pl);
    ck_assert_uint_gt(los_publication_epoch(), before);
    ck_assert_int_eq(CONTR(pl)->tli, 0);
    assert_aborted(pl);

    assert_fresh_publication(pl);
    MapCell *cell = actor_cache_cell(cs, pl, earlier);
    ck_assert_ptr_nonnull(cell);
    size_t layer = NUM_LAYERS * earlier->sub_layer + earlier->layer - 1;
    ck_assert_uint_eq(cell->faces[layer], 0);
}
END_TEST

START_TEST(test_ask_show_upper_roof_aborts_stale_map2_and_hides_actor) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    mapstruct *upper = upper_level(map);
    object *actor = place(map, "raas", 13, 12);
    ck_assert(object_set_value(actor, "los_ask_show_upper_path", upper->path, true));
    ask_show(actor, "cover");

    socket_buffer_clear(CONTR(pl)->cs);
    uint64_t before = los_publication_epoch();
    draw_client_map2(pl);
    ck_assert_uint_gt(los_publication_epoch(), before);
    assert_aborted(pl);
    ck_assert(!los_player_can_see_cell(pl, map, actor->x, actor->y));

    assert_fresh_publication(pl);
}
END_TEST

/* The callback is reached after the first actor has already been serialized.
 * The event on the later actor hides that earlier identity, so validating only
 * the current callback object would leak its stale representation. */
START_TEST(test_ask_show_mutating_earlier_actor_aborts_whole_map2_transaction) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    /* MAP2 traverses X from east to west. The x=14 actor is therefore
     * serialized before the x=13 callback, whose fixture targets x+1. */
    object *earlier = place(map, "raas", 14, 12);
    object *later = place(map, "raas", 13, 12);
    ask_show_other(later, "hide_other", earlier);

    socket_buffer_clear(CONTR(pl)->cs);
    uint64_t before = los_publication_epoch();
    draw_client_map2(pl);
    ck_assert_uint_gt(los_publication_epoch(), before);
    assert_aborted(pl);
    ck_assert(QUERY_FLAG(earlier, FLAG_IS_INVISIBLE));

    assert_fresh_publication(pl);
}
END_TEST

START_TEST(test_ask_show_moving_earlier_actor_aborts_whole_map2_transaction) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    object *earlier = place(map, "raas", 14, 12);
    object *later = place(map, "raas", 13, 12);
    ask_show_other(later, "move_other", earlier);

    socket_buffer_clear(CONTR(pl)->cs);
    uint64_t before = los_publication_epoch();
    draw_client_map2(pl);
    ck_assert_uint_gt(los_publication_epoch(), before);
    assert_aborted(pl);
    ck_assert_int_eq(earlier->x, 15);
    ck_assert_int_eq(earlier->y, 13);

    assert_fresh_publication(pl);
}
END_TEST

/* These cases cross the real Python flag setter.  They warm the view first so
 * a passing case proves both immediate physical authority and invalidation of
 * the retained LOS masks, rather than only the native object-update hook. */
static void assert_python_roof_flag_transaction(const char *mode,
                                                bool initial_floor,
                                                bool initial_hidden,
                                                bool initially_visible,
                                                bool finally_visible) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    mapstruct *upper = upper_level(map);
    object *actor = place(map, "raas", 13, 12);
    object *roof = place(upper, "roof_thatch", 13, 12);
    if (initial_floor) {
        SET_FLAG(roof, FLAG_IS_FLOOR);
    } else {
        CLEAR_FLAG(roof, FLAG_IS_FLOOR);
    }
    if (initial_hidden) {
        SET_FLAG(roof, FLAG_HIDDEN);
    } else {
        CLEAR_FLAG(roof, FLAG_HIDDEN);
    }
    object_update(roof, UP_OBJ_FLAGS);
    ck_assert(object_set_value(actor, "los_ask_show_upper_path", upper->path, true));
    ask_show(actor, mode);

    update_los(pl);
    ck_assert(los_view_is_current(pl));
    ck_assert_int_eq(los_player_can_see_cell(pl, map, actor->x, actor->y), initially_visible);
    uint64_t geometry_before = los_geometry_epoch();
    uint64_t publication_before = los_publication_epoch();
    socket_buffer_clear(CONTR(pl)->cs);
    draw_client_map2(pl);
    ck_assert_uint_gt(los_geometry_epoch(), geometry_before);
    ck_assert_uint_gt(los_publication_epoch(), publication_before);
    assert_aborted(pl);

    /* Fresh physical queries must observe the Python-set flag before a second
     * MAP2 publication has a chance to rebuild the cached view. */
    ck_assert_int_eq(los_player_can_see_cell(pl, map, actor->x, actor->y), finally_visible);
    ck_assert_int_eq(los_player_cell_camera(pl, map, actor->x, actor->y),
                     finally_visible ? LOS_CAMERA_VISIBLE : LOS_CAMERA_WALL_BOUNDARY);
    update_los(pl);
    ck_assert(los_view_is_current(pl));
    ck_assert_int_eq(los_player_can_see_cell(pl, map, actor->x, actor->y), finally_visible);
}

START_TEST(test_ask_show_python_floor_setter_invalidates_warmed_los) {
    assert_python_roof_flag_transaction("floor_on", false, false, true, false);
    assert_python_roof_flag_transaction("floor_off", true, false, false, true);
}
END_TEST

START_TEST(test_ask_show_python_hidden_setter_invalidates_warmed_los) {
    assert_python_roof_flag_transaction("hidden_on", false, false, true, false);
    assert_python_roof_flag_transaction("hidden_off", false, true, false, true);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("server.los_callbacks");
    TCase *tc = tcase_create("callbacks");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    tcase_add_test(tc, test_ask_show_blocker_aborts_stale_map2_and_recovers_fresh);
    tcase_add_test(tc, test_ask_show_viewstate_change_withdraws_earlier_dark_actor);
    tcase_add_test(tc, test_ask_show_kicking_observer_withdraws_snapshot_and_continues_postprocess);
    tcase_add_test(tc, test_ask_show_kicking_observer_during_direct_map2_withdraws_authority);
    tcase_add_test(tc, test_queued_target_kick_observer_withdraws_dispatch_authority);
    tcase_add_test(tc, test_ask_show_kicking_next_player_rejects_postprocess_snapshot);
    tcase_add_test(tc, test_ask_show_upper_roof_aborts_stale_map2_and_hides_actor);
    tcase_add_test(tc, test_ask_show_mutating_earlier_actor_aborts_whole_map2_transaction);
    tcase_add_test(tc, test_ask_show_moving_earlier_actor_aborts_whole_map2_transaction);
    tcase_add_test(tc, test_ask_show_python_floor_setter_invalidates_warmed_los);
    tcase_add_test(tc, test_ask_show_python_hidden_setter_invalidates_warmed_los);
    suite_add_tcase(s, tc);
    return s;
}

void check_server_los_callbacks(void) {
    check_run_suite(suite(), __FILE__);
}
