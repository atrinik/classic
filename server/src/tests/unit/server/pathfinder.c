/*************************************************************************
 * Atrinik server pathfinding adapter tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/clioptions.h>
#include <arch.h>
#include <object.h>
#include <pathfinder.h>
#include <waypoint.h>

static path_node_t *path_last(path_node_t *path) {
    while (path != NULL && path->next != NULL) {
        path = path->next;
    }
    return path;
}

START_TEST(test_exact_search_returns_owned_route_and_metrics) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t result = path_search(pl, map, 1, 1, map, 8, 1, &options, NULL);

    ck_assert_int_eq(result.status, PATH_STATUS_FOUND);
    ck_assert_ptr_nonnull(result.path);
    ck_assert_int_eq(result.path->x, 1);
    ck_assert_int_eq(result.path->y, 1);
    path_node_t *last = path_last(result.path);
    ck_assert_ptr_nonnull(last);
    ck_assert_int_eq(last->x, 7);
    ck_assert_int_eq(last->y, 1);
    ck_assert_uint_gt(result.generated, 1);
    ck_assert_uint_gt(result.examined_transitions, 0);
    ck_assert_uint_gt(result.total_cost, 0);

    path_result_free(&result);
    ck_assert_ptr_null(result.path);
}
END_TEST

START_TEST(test_waypoint_retains_explicit_partial_path_and_best_effort_failures) {
    mapstruct *map;
    object *npc;
    check_setup_env_pl(&map, &npc);
    FREE_AND_COPY_HASH(map->path, "/unit/pathfinder");
    object *waypoint = arch_get("waypoint");
    ck_assert_ptr_nonnull(waypoint);
    waypoint = object_insert_into(waypoint, npc, 0);
    ck_assert_ptr_nonnull(waypoint);
    waypoint->stats.hp = 20;
    waypoint->stats.sp = 20;
    waypoint->stats.Int = 7;
    waypoint->stats.Str = 4;
    waypoint->stats.dam = 9;
    SET_FLAG(waypoint, FLAG_NO_ATTACK);
    char *errmsg = NULL;
    ck_assert_msg(clioptions_load_str("pathfinder_max_nodes = 8", &errmsg),
                  "%s",
                  errmsg != NULL ? errmsg : "");
    free(errmsg);

    waypoint_compute_path(waypoint);

    ck_assert_ptr_nonnull(waypoint->msg);
    ck_assert_uint_gt(strlen(waypoint->msg), 0);
    ck_assert_int_eq(waypoint->stats.Int, 7);
    ck_assert_int_eq(waypoint->stats.Str, 4);
    ck_assert_int_eq(waypoint->stats.dam, 9);
    errmsg = NULL;
    ck_assert_msg(clioptions_load_str("pathfinder_max_nodes = 10000", &errmsg),
                  "%s",
                  errmsg != NULL ? errmsg : "");
    free(errmsg);
}
END_TEST

START_TEST(test_waypoint_abandons_stuck_cross_depth_best_effort_target) {
    mapstruct *source;
    object *npc;
    check_setup_env_pl(&source, &npc);
    FREE_AND_COPY_HASH(source->path, "/unit/waypoint-source");

    mapstruct *destination = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(destination->path, "/unit/waypoint-destination");
    source->tile_map[TILED_UP] = destination;
    destination->tile_map[TILED_DOWN] = source;

    object *waypoint = arch_get("waypoint");
    ck_assert_ptr_nonnull(waypoint);
    waypoint = object_insert_into(waypoint, npc, 0);
    ck_assert_ptr_nonnull(waypoint);
    FREE_AND_COPY_HASH(waypoint->name, "home");
    FREE_AND_ADD_REF_HASH(waypoint->slaying, destination->path);
    waypoint->stats.hp = 8;
    waypoint->stats.sp = 9;
    waypoint->stats.Int = 11;
    waypoint->stats.Str = 5;
    SET_FLAG(waypoint, FLAG_CURSED);
    SET_FLAG(waypoint, FLAG_NO_ATTACK);
    SET_FLAG(waypoint, FLAG_WP_PATH_REQUESTED);

    path_node_t local_step = {
        .map = source,
        .x = npc->x + 1,
        .y = npc->y,
    };
    waypoint->msg = path_encode(&local_step);
    waypoint->attacked_by_distance = strlen(waypoint->msg);

    rv_vector rv;
    ck_assert(get_rangevector_from_mapcoords(source,
                                             npc->x,
                                             npc->y,
                                             destination,
                                             waypoint->stats.hp,
                                             waypoint->stats.sp,
                                             &rv,
                                             RV_RECURSIVE_SEARCH | RV_DIAGONAL_DISTANCE));
    ck_assert_int_ne(rv.distance_z, 0);
    waypoint->stats.dam = 1;

    waypoint_move(waypoint, npc);

    ck_assert_ptr_eq(waypoint->slaying, source->path);
    ck_assert_int_eq(waypoint->stats.hp, npc->x);
    ck_assert_int_eq(waypoint->stats.sp, npc->y);
    ck_assert_int_eq(waypoint->stats.Int, 12);

    CLEAR_FLAG(waypoint, FLAG_WP_PATH_REQUESTED);
    waypoint_move(waypoint, npc);
    ck_assert(!QUERY_FLAG(waypoint, FLAG_CURSED));
}
END_TEST

START_TEST(test_budget_partial_is_explicit_and_exact_search_stays_empty) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    path_search_options_t options;
    path_search_options_init(&options);
    options.max_generated = 2;
    path_result_t exact = path_search(pl, map, 1, 1, map, 20, 20, &options, NULL);
    ck_assert_int_eq(exact.status, PATH_STATUS_LIMIT_REACHED);
    ck_assert_ptr_null(exact.path);
    ck_assert_uint_eq(exact.generated, 2);

    options.return_partial = true;
    path_result_t partial = path_search(pl, map, 1, 1, map, 20, 20, &options, NULL);
    ck_assert_int_eq(partial.status, PATH_STATUS_PARTIAL);
    ck_assert_ptr_nonnull(partial.path);
    ck_assert_uint_eq(partial.generated, 2);

    path_result_free(&exact);
    path_result_free(&partial);
}
END_TEST

START_TEST(test_no_path_is_distinct_from_budget_exhaustion) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    for (int direction = 1; direction <= SIZEOFFREE1; direction++) {
        int x = 12 + freearr_x[direction];
        int y = 12 + freearr_y[direction];
        SET_MAP_FLAGS(map, x, y, P_NO_PASS);
    }

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t result = path_search(pl, map, 12, 12, map, 20, 20, &options, NULL);
    ck_assert_int_eq(result.status, PATH_STATUS_NO_PATH);
    ck_assert_ptr_null(result.path);
    ck_assert_uint_eq(result.generated, 1);
    path_result_free(&result);
}
END_TEST

START_TEST(test_tiled_border_uses_alternate_open_crossing) {
    mapstruct *source;
    object *pl;
    check_setup_env_pl(&source, &pl);
    mapstruct *destination = get_empty_map(24, 24);
    source->tile_map[TILED_EAST] = destination;
    destination->tile_map[TILED_WEST] = source;

    for (int y = 0; y < 23; y++) {
        SET_MAP_FLAGS(source, 23, y, P_NO_PASS);
        SET_MAP_FLAGS(destination, 0, y, P_NO_PASS);
    }

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t result = path_search(pl, source, 2, 2, destination, 8, 23, &options, NULL);
    ck_assert_int_eq(result.status, PATH_STATUS_FOUND);
    ck_assert_ptr_nonnull(result.path);
    bool crossed = false;
    for (path_node_t *node = result.path; node != NULL; node = node->next) {
        if (node->map == destination) {
            crossed = true;
            ck_assert_int_eq(node->y, 23);
            break;
        }
    }
    ck_assert(crossed);
    path_result_free(&result);
}
END_TEST

START_TEST(test_results_remain_isolated_across_searches) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t first = path_search(pl, map, 1, 1, map, 8, 1, &options, NULL);
    ck_assert_int_eq(first.status, PATH_STATUS_FOUND);
    path_node_t *first_last = path_last(first.path);
    ck_assert_ptr_nonnull(first_last);
    ck_assert_int_eq(first_last->x, 7);

    path_result_t second = path_search(pl, map, 2, 2, map, 2, 9, &options, NULL);
    ck_assert_int_eq(second.status, PATH_STATUS_FOUND);
    ck_assert_int_eq(first_last->x, 7);
    ck_assert_int_eq(first_last->y, 1);

    path_result_free(&second);
    path_result_free(&first);
}
END_TEST

START_TEST(test_weighted_terrain_is_avoided) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    SET_MAP_MOVE_FLAGS(map, 4, 1, UINT16_MAX);

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t result = path_search(pl, map, 1, 1, map, 8, 1, &options, NULL);

    ck_assert_int_eq(result.status, PATH_STATUS_FOUND);
    for (path_node_t *node = result.path; node != NULL; node = node->next) {
        ck_assert(!(node->x == 4 && node->y == 1));
    }
    ck_assert_uint_lt(result.total_cost, UINT16_MAX);
    path_result_free(&result);
}
END_TEST

START_TEST(test_representative_barrier_search_stays_bounded) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    for (int y = 0; y < 23; y++) {
        SET_MAP_FLAGS(map, 12, y, P_NO_PASS);
    }

    path_search_options_t options;
    path_search_options_init(&options);
    path_result_t result = path_search(pl, map, 1, 1, map, 22, 22, &options, NULL);

    ck_assert_int_eq(result.status, PATH_STATUS_FOUND);
    ck_assert_ptr_nonnull(result.path);
    ck_assert_uint_lt(result.generated, 24U * 24U);
    ck_assert_uint_gt(result.examined_transitions, result.generated);
    path_result_free(&result);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("pathfinder");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_exact_search_returns_owned_route_and_metrics);
    tcase_add_test(tc_core, test_budget_partial_is_explicit_and_exact_search_stays_empty);
    tcase_add_test(tc_core, test_waypoint_retains_explicit_partial_path_and_best_effort_failures);
    tcase_add_test(tc_core, test_waypoint_abandons_stuck_cross_depth_best_effort_target);
    tcase_add_test(tc_core, test_no_path_is_distinct_from_budget_exhaustion);
    tcase_add_test(tc_core, test_tiled_border_uses_alternate_open_crossing);
    tcase_add_test(tc_core, test_results_remain_isolated_across_searches);
    tcase_add_test(tc_core, test_weighted_terrain_is_avoided);
    tcase_add_test(tc_core, test_representative_barrier_search_stays_bounded);
    return s;
}

void check_server_pathfinder(void) {
    check_run_suite(suite(), __FILE__);
}
