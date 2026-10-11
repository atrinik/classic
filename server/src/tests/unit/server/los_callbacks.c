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
#include <arch.h>
#include <initialization.h>
#include <los.h>
#include <map.h>
#include <object.h>
#include <object_methods.h>
#include <player.h>
#include <plugin.h>
#include <server.h>

static bool plugins_initialized;

static void fixture(mapstruct **map, object **pl) {
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
    ck_assert_ptr_nonnull(find_plugin("Python"));
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
    ck_assert_uint_eq(cs->target_map_published_count, pl->count);
    ck_assert_uint_ne(cs->target_map_published_depths, 0);
    ck_assert_ptr_nonnull(map_client_cache_cell(&cs->lastmap, 0, cs->mapx_2, cs->mapy_2, false));
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
    tcase_add_test(tc, test_ask_show_blocker_aborts_stale_map2_and_recovers_fresh);
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
