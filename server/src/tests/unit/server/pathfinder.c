/*************************************************************************
 * Atrinik server pathfinding adapter tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <object.h>
#include <pathfinder.h>

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
