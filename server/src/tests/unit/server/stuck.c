/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Tests the /stuck player command and its transient effect. */

#include <math.h>

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <commands.h>
#include <map.h>
#include <movement.h>
#include <object.h>
#include <player.h>
#include <player_status.h>
#include <server_clock_fake.h>
#include <spells.h>
#include <stuck.h>
#include <toolkit/packet.h>

static long saved_pticks;

static void stuck_test_setup(void) {
    saved_pticks = pticks;
    pticks = 100;
    server_clock_fake_install(UINT64_C(100000),
                              (server_tick_t){100},
                              (server_monotonic_t){UINT64_C(1000000)},
                              (server_wall_utc_t){INT64_C(1700000000)});
}

static void stuck_test_teardown(void) {
    player_save_fail_for_test(false);
    server_clock_fake_uninstall();
    pticks = saved_pticks;
}

static void stuck_test_prepare_player(mapstruct **map, object **pl, const char *name) {
    check_setup_env_pl(map, pl);
    FREE_AND_COPY_HASH((*map)->path, "/tests/stuck-source");
    FREE_AND_COPY_HASH((*pl)->name, name);
    CONTR(*pl)->cs->state = ST_PLAYING;
}

static object *stuck_test_find_effect(object *pl) {
    for (object *effect = pl->inv; effect != NULL; effect = effect->below) {
        if (player_stuck_effect(effect)) {
            return effect;
        }
    }
    return NULL;
}

static bool stuck_test_has_message(object *pl, const char *expected) {
    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_DRAWINFO) {
            continue;
        }

        packet_reader_t reader;
        char color[64];
        char message[HUGE_BUF];
        packet_reader_init(&reader, packet->data, packet->len);
        (void)packet_reader_read_uint8(&reader);
        if (!packet_reader_read_string(&reader, VS(color)) ||
            !packet_reader_read_string(&reader, VS(message)) ||
            packet_reader_error(&reader) != PACKET_ERROR_NONE) {
            continue;
        }

        if (strcmp(message, expected) == 0) {
            return true;
        }
    }

    return false;
}

static bool stuck_test_has_status(object *pl, int32_t expected_seconds) {
    for (packet_struct *packet = CONTR(pl)->cs->packets; packet != NULL; packet = packet->next) {
        if (packet->type != CLIENT_CMD_PLAYER_STATUS) {
            continue;
        }

        packet_reader_t reader;
        char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
        char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
        char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
        packet_reader_init(&reader, packet->data, packet->len);
        if (packet_reader_read_uint8(&reader) != PLAYER_STATUS_UPSERT ||
            !packet_reader_read_string(&reader, VS(key)) || !packet_reader_read_uint16(&reader) ||
            !packet_reader_read_string(&reader, VS(name)) ||
            !packet_reader_read_string(&reader, VS(tooltip)) ||
            packet_reader_read_int32(&reader) != expected_seconds ||
            packet_reader_error(&reader) != PACKET_ERROR_NONE) {
            continue;
        }

        return strcmp(key, PLAYER_STUCK_STATUS_KEY) == 0 && strcmp(name, "stuck recovery") == 0;
    }

    return false;
}

static void stuck_test_start(object *pl) {
    command_stuck(pl, "stuck", NULL);

    object *effect = stuck_test_find_effect(pl);
    ck_assert_ptr_nonnull(effect);
    ck_assert_int_eq(effect->type, WORD_OF_RECALL);
    ck_assert_double_gt(effect->speed, 0.0);
    ck_assert(fabs(effect->speed - 1.0 / (PLAYER_STUCK_COUNTDOWN_SECONDS * MAX_TICKS)) < 0.000001);
    ck_assert_double_eq(effect->speed_left, -1.0);
    ck_assert(player_status_should_publish(effect));
    ck_assert_int_eq(CONTR(pl)->stuck_cooldown.seconds,
                     INT64_C(1700000000) + PLAYER_STUCK_COOLDOWN_SECONDS);
}

static player *stuck_test_load_state(FILE *fp, const char *name, object **loaded) {
    object *placeholder = player_get_dummy(name, NULL);
    player *state = CONTR(placeholder);
    object_remove(placeholder, 0);
    placeholder->custom_attrset = NULL;
    object_destroy(placeholder);
    state->ob = object_get();
    ck_assert(player_load_stream(state, fp));
    *loaded = state->ob;
    (*loaded)->custom_attrset = state;
    return state;
}

static void stuck_test_run_countdown(object *pl, mapstruct *source_map) {
    object *effect = stuck_test_find_effect(pl);
    ck_assert_ptr_nonnull(effect);

    size_t ticks = (size_t)ceil(1.0 / effect->speed) + 2U;
    for (size_t i = 0; i < ticks && pl->map == source_map; i++) {
        process_events();
    }
}

START_TEST(test_stuck_command_is_registered_and_has_no_destination_arguments) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Command Syntax");

    char command[] = "/stuck somewhere";
    commands_handle(pl, command);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert_int_eq(CONTR(pl)->stuck_cooldown.seconds, 0);
    ck_assert(stuck_test_has_message(pl, "Usage: /stuck"));
}
END_TEST

START_TEST(test_stuck_start_publishes_a_ticking_status_effect) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Status Effect");

    stuck_test_start(pl);
    ck_assert(stuck_test_has_status(pl, PLAYER_STUCK_COUNTDOWN_SECONDS));
}
END_TEST

START_TEST(test_stuck_countdown_transfers_to_emergency_map) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Successful Recovery");

    stuck_test_start(pl);
    stuck_test_run_countdown(pl, map);

    ck_assert_ptr_nonnull(pl->map);
    ck_assert_ptr_ne(pl->map, map);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert(stuck_test_has_message(pl, "You have been moved to the safe recovery location."));
}
END_TEST

START_TEST(test_stuck_movement_does_not_cancel_pending_recovery) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Movement");
    stuck_test_start(pl);

    ck_assert_int_eq(object_move_to(pl, 1, pl, map, pl->x + 1, pl->y), 1);
    ck_assert_ptr_eq(pl->map, map);
    ck_assert_ptr_nonnull(stuck_test_find_effect(pl));
}
END_TEST

START_TEST(test_stuck_nonplaying_player_discards_pending_recovery) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Nonplaying Cleanup");
    stuck_test_start(pl);
    CONTR(pl)->cs->state = ST_DEAD;

    object *effect = stuck_test_find_effect(pl);
    player_stuck_process_effect(effect);
    ck_assert_ptr_eq(pl->map, map);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
}
END_TEST

START_TEST(test_stuck_healing_spell_does_not_interrupt_recovery) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Healing Spell");
    stuck_test_start(pl);
    pl->stats.hp = pl->stats.maxhp / 2;
    pl->stats.sp = 1000;

    ck_assert(cast_spell(pl, pl, 0, SP_MINOR_HEAL, 0, CAST_NORMAL, NULL));
    ck_assert_ptr_nonnull(stuck_test_find_effect(pl));
}
END_TEST

START_TEST(test_stuck_combat_removes_the_effect) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Combat Interrupt");

    stuck_test_start(pl);
    player_mark_combat(CONTR(pl));

    ck_assert_ptr_eq(pl->map, map);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert(stuck_test_has_message(pl, "Your stuck recovery was interrupted by combat."));
}
END_TEST

START_TEST(test_stuck_combat_before_request_does_not_interrupt_new_effect) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Prior Combat");

    player_mark_combat(CONTR(pl));
    stuck_test_start(pl);
    stuck_test_run_countdown(pl, map);

    ck_assert_ptr_ne(pl->map, map);
}
END_TEST

START_TEST(test_stuck_cooldown_survives_save_load_without_saving_effect) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Cooldown Persistence");
    stuck_test_start(pl);
    ck_assert(player_save_checked(pl));

    char *path = player_make_path(pl->name, "player.dat");
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[HUGE_BUF * 4];
    size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
    ck_assert(!ferror(fp));
    contents[length] = '\0';
    ck_assert_ptr_nonnull(strstr(contents, "stuck_cooldown 1700000300\n"));
    ck_assert_ptr_null(strstr(contents, PLAYER_STUCK_STATUS_KEY));

    rewind(fp);
    object *restored;
    player *loaded_state = stuck_test_load_state(fp, "Stuck Cooldown Reload", &restored);
    ck_assert_int_eq(fclose(fp), 0);
    free(path);

    ck_assert_int_eq(loaded_state->stuck_cooldown.seconds, INT64_C(1700000300));
    ck_assert_ptr_null(stuck_test_find_effect(restored));

    mapstruct *reconnect_map = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(reconnect_map->path, "/tests/stuck-reconnect");
    ck_assert(object_enter_map(restored, NULL, reconnect_map, 1, 1, true));
    command_stuck(restored, "stuck", NULL);
    ck_assert_ptr_null(stuck_test_find_effect(restored));
    ck_assert(stuck_test_has_message(restored,
                                     "You must wait 300 more seconds before using /stuck again."));

    server_clock_fake_set_wall((server_wall_utc_t){INT64_C(1700000300)});
    command_stuck(restored, "stuck", NULL);
    ck_assert_ptr_nonnull(stuck_test_find_effect(restored));
}
END_TEST

START_TEST(test_stuck_malformed_persisted_cooldown_fails_closed) {
    FILE *fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    static const char embedded_nul_line[] = "stuck_cooldown 0\0garbage\n";
    ck_assert_uint_eq(fwrite(embedded_nul_line, 1, sizeof(embedded_nul_line) - 1, fp),
                      sizeof(embedded_nul_line) - 1);
    ck_assert_int_ne(fputs("stuck_cooldown\n"
                           "stuck_cooldown\t0\n"
                           "stuck_cooldown:0\n"
                           "stuck_cooldown=0\n"
                           "stuck_cooldown malformed\n"
                           "stuck_cooldown 0\n"
                           "endplst\n"
                           "arch human_male\n"
                           "end\n",
                           fp),
                     EOF);
    rewind(fp);

    object *restored;
    player *loaded_state = stuck_test_load_state(fp, "Stuck Malformed Reload", &restored);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_int_eq(loaded_state->stuck_cooldown.seconds, INT64_MAX);

    mapstruct *map = get_empty_map(24, 24);
    FREE_AND_COPY_HASH(map->path, "/tests/stuck-malformed-reconnect");
    ck_assert(object_enter_map(restored, NULL, map, 1, 1, true));
    command_stuck(restored, "stuck", NULL);
    ck_assert_ptr_null(stuck_test_find_effect(restored));
    ck_assert(stuck_test_has_message(restored,
                                     "You must wait 300 more seconds before using /stuck again."));
}
END_TEST

START_TEST(test_stuck_truncated_persisted_cooldown_fails_closed) {
    FILE *fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ne(fputs("stuck_cooldown ", fp), EOF);
    rewind(fp);

    object *restored;
    player *loaded_state = stuck_test_load_state(fp, "Stuck Truncated Reload", &restored);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_int_eq(loaded_state->stuck_cooldown.seconds, INT64_MAX);
}
END_TEST

START_TEST(test_stuck_negative_in_memory_cooldown_fails_closed) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Negative Cooldown");
    CONTR(pl)->stuck_cooldown.seconds = -1;

    command_stuck(pl, "stuck", NULL);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert(
        stuck_test_has_message(pl, "You must wait 300 more seconds before using /stuck again."));
}
END_TEST

START_TEST(test_stuck_save_failure_does_not_arm_or_persist_cooldown) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Save Failure");
    ck_assert(player_save_checked(pl));

    player_save_fail_for_test(true);
    command_stuck(pl, "stuck", NULL);
    player_save_fail_for_test(false);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert_int_eq(CONTR(pl)->stuck_cooldown.seconds, 0);
    ck_assert(stuck_test_has_message(
        pl,
        "Your stuck recovery cooldown could not be saved; recovery was not started."));

    char *path = player_make_path(pl->name, "player.dat");
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    object *restored;
    player *loaded_state = stuck_test_load_state(fp, "Stuck Save Failure Reload", &restored);
    ck_assert_int_eq(fclose(fp), 0);
    free(path);
    ck_assert_int_eq(loaded_state->stuck_cooldown.seconds, 0);
}
END_TEST

START_TEST(test_stuck_rejects_unique_map_without_starting) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Unique Map");

    map->map_flags |= MAP_FLAG_UNIQUE;
    command_stuck(pl, "stuck", NULL);
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert_int_eq(CONTR(pl)->stuck_cooldown.seconds, 0);
    ck_assert(stuck_test_has_message(
        pl,
        "You cannot use /stuck in this location because your cooldown cannot be saved safely."));
}
END_TEST

START_TEST(test_stuck_cancel_removes_effect_but_keeps_cooldown) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Disconnect Cleanup");
    stuck_test_start(pl);

    ck_assert(player_stuck_cancel(pl));
    ck_assert_ptr_null(stuck_test_find_effect(pl));
    ck_assert_int_eq(CONTR(pl)->stuck_cooldown.seconds, INT64_C(1700000300));
}
END_TEST

START_TEST(test_stuck_logout_removes_effect_before_saving) {
    mapstruct *map;
    object *pl;
    stuck_test_prepare_player(&map, &pl, "Stuck Logout Cleanup");
    stuck_test_start(pl);

    char *path = player_make_path(pl->name, "player.dat");
    CONTR(pl)->cs->state = ST_DEAD;
    player_logout(CONTR(pl));

    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[HUGE_BUF * 4];
    size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
    ck_assert(!ferror(fp));
    contents[length] = '\0';
    ck_assert_ptr_nonnull(strstr(contents, "stuck_cooldown 1700000300\n"));
    ck_assert_ptr_null(strstr(contents, PLAYER_STUCK_STATUS_KEY));
    ck_assert_int_eq(fclose(fp), 0);
    free(path);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("stuck");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, stuck_test_setup, stuck_test_teardown);
    tcase_add_test(tc_core, test_stuck_command_is_registered_and_has_no_destination_arguments);
    tcase_add_test(tc_core, test_stuck_start_publishes_a_ticking_status_effect);
    tcase_add_test(tc_core, test_stuck_countdown_transfers_to_emergency_map);
    tcase_add_test(tc_core, test_stuck_movement_does_not_cancel_pending_recovery);
    tcase_add_test(tc_core, test_stuck_nonplaying_player_discards_pending_recovery);
    tcase_add_test(tc_core, test_stuck_healing_spell_does_not_interrupt_recovery);
    tcase_add_test(tc_core, test_stuck_combat_removes_the_effect);
    tcase_add_test(tc_core, test_stuck_combat_before_request_does_not_interrupt_new_effect);
    tcase_add_test(tc_core, test_stuck_cooldown_survives_save_load_without_saving_effect);
    tcase_add_test(tc_core, test_stuck_malformed_persisted_cooldown_fails_closed);
    tcase_add_test(tc_core, test_stuck_truncated_persisted_cooldown_fails_closed);
    tcase_add_test(tc_core, test_stuck_negative_in_memory_cooldown_fails_closed);
    tcase_add_test(tc_core, test_stuck_save_failure_does_not_arm_or_persist_cooldown);
    tcase_add_test(tc_core, test_stuck_rejects_unique_map_without_starting);
    tcase_add_test(tc_core, test_stuck_cancel_removes_effect_but_keeps_cooldown);
    tcase_add_test(tc_core, test_stuck_logout_removes_effect_before_saving);
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_stuck(void) {
    check_run_suite(suite(), __FILE__);
}
