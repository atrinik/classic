/*************************************************************************
 * Atrinik server item-socket update regression tests.
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>
#include <object.h>
#include <server.h>

START_TEST(test_send_map_item_marks_look_stale) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    object *stack = arch_get("torch");
    stack->x = pl->x;
    stack->y = pl->y;
    stack = object_insert_map(stack, map, NULL, INS_NO_MERGE);

    uint8_t old_update = GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y);
    stack->nrof = 2;
    esrv_send_item(stack);

    ck_assert_uint_ne(GET_MAP_UPDATE_COUNTER(map, stack->x, stack->y), old_update);

    object_destroy(pl);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("item");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_send_map_item_marks_look_stale);

    return s;
}

void check_socket_item(void) {
    check_run_suite(suite(), __FILE__);
}
