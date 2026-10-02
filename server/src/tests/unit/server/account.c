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

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <account.h>
#include <exploration.h>
#include <server.h>
#include <toolkit/packet.h>
#include <initialization.h>
#include <player.h>
#include <toolkit/path.h>

START_TEST(test_account_provision) {
    const char *account_name = "scenarioaccount";
    const char *character_name = "Scenario Hero";
    const char *password = "local-test-7!";
    char error[HUGE_BUF];
    char *account_path = account_make_path(account_name);
    char *player_path = player_make_path(character_name, "player.dat");

    unlink(account_path);
    unlink(player_path);
    ck_assert(account_provision(account_name, password, character_name, "human_male", VS(error)));

    struct stat statbuf;
    ck_assert_int_eq(stat(account_path, &statbuf), 0);
#ifndef WIN32
    ck_assert_int_eq(statbuf.st_mode & 0777, SAVE_MODE);
#endif
    ck_assert_int_eq(stat(player_path, &statbuf), 0);
    ck_assert_int_eq(statbuf.st_size, 0);

    FILE *fp = fopen(account_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[HUGE_BUF];
    size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
    contents[length] = '\0';
    fclose(fp);
    ck_assert_ptr_null(strstr(contents, password));
    ck_assert_ptr_nonnull(strstr(contents, "password $argon2id$"));
    ck_assert_ptr_nonnull(strstr(contents, "char human_male:Scenario Hero::1"));

    metric_store_t metrics;
    metrics_store_init(&metrics, METRIC_SCOPE_ACCOUNT, 0);
    ck_assert(account_metrics_load(account_name, &metrics));
    metrics_store_free(&metrics);

    ck_assert(!account_provision(account_name, password, character_name, "human_male", VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "cannot reserve"));
    ck_assert_int_eq(stat(player_path, &statbuf), 0);
    ck_assert_int_eq(statbuf.st_size, 0);

    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    free(account_path);
    free(player_path);
}
END_TEST

START_TEST(test_account_provision_rejects_invalid_inputs) {
    char error[HUGE_BUF];
    ck_assert(
        !account_provision("bad-name", "local-test-7!", "Scenario Hero", "human_male", VS(error)));
    ck_assert_str_eq(error, "invalid account name");
    ck_assert(
        !account_provision("scenarioaccount", "short", "Scenario Hero", "human_male", VS(error)));
    ck_assert_str_eq(error, "invalid password");
    ck_assert(!account_provision("scenarioaccount",
                                 "local-test-7!",
                                 "Bad_Name",
                                 "human_male",
                                 VS(error)));
    ck_assert_str_eq(error, "invalid character name");
    ck_assert(!account_provision("scenarioaccount",
                                 "local-test-7!",
                                 "Scenario Hero",
                                 "not_a_player",
                                 VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "invalid player archetype"));
}
END_TEST

START_TEST(test_account_provision_password_file_permissions) {
    const char *account_name = "scenariofile";
    const char *character_name = "Scenario File";
    char error[HUGE_BUF];
    char password_path[HUGE_BUF];
    snprintf(VS(password_path), "%s/scenario-password", settings.datapath);
    char *account_path = account_make_path(account_name);
    char *player_path = player_make_path(character_name, "player.dat");

    unlink(account_path);
    unlink(player_path);
    unlink(password_path);
    int fd = open(password_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    ck_assert_int_ge(fd, 0);
    const char password[] = "local-file-8!\n\n";
    ck_assert_int_eq(write(fd, password, sizeof(password) - 1), sizeof(password) - 1);
    ck_assert_int_eq(close(fd), 0);

#ifdef WIN32
    unlink(password_path);
    ck_assert_int_eq(path_secret_create_atomic(password_path,
                                               password,
                                               sizeof(password) - 1),
                     PATH_SECRET_CREATE_OK);
#endif

#ifndef WIN32
    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "basic-player",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "mode 0600"));
    ck_assert_int_eq(chmod(password_path, SAVE_MODE), 0);
#endif
    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "basic-player",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "exactly one line"));

    char missing_password_path[HUGE_BUF];
    snprintf(VS(missing_password_path), "%s/scenario-password-missing", settings.datapath);
    unlink(missing_password_path);
    ck_assert(!account_provision_from_file(account_name,
                                           missing_password_path,
                                           character_name,
                                           "human_male",
                                           "basic-player",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "cannot open password file"));

    static const char empty_password[] = "";
    unlink(password_path);
    ck_assert_int_eq(path_secret_create_atomic(password_path,
                                               empty_password,
                                               sizeof(empty_password) - 1),
                     PATH_SECRET_CREATE_OK);
    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "basic-player",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "cannot read password file"));

    const char valid_password[] = "local-file-8!\r\n";
#ifdef WIN32
    unlink(password_path);
    ck_assert_int_eq(path_secret_create_atomic(password_path,
                                               valid_password,
                                               sizeof(valid_password) - 1),
                     PATH_SECRET_CREATE_OK);
#else
    fd = open(password_path, O_WRONLY | O_TRUNC);
    ck_assert_int_ge(fd, 0);
    ck_assert_int_eq(write(fd, valid_password, sizeof(valid_password) - 1),
                     sizeof(valid_password) - 1);
    ck_assert_int_eq(close(fd), 0);
#endif
    ck_assert(account_provision_from_file(account_name,
                                          password_path,
                                          character_name,
                                          "human_male",
                                          "basic-player",
                                          VS(error)));

    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    ck_assert_int_eq(unlink(password_path), 0);
    free(account_path);
    free(player_path);
}
END_TEST

START_TEST(test_account_provision_lighting_preset) {
    const char *account_name = "ScenarioLighting";
    const char *account_name_canonical = "scenariolighting";
    const char *character_name = "scenario lighting";
    const char *character_name_canonical = "Scenario Lighting";
    char error[HUGE_BUF];
    char password_path[HUGE_BUF];
    snprintf(VS(password_path), "%s/scenario-lighting-password", settings.datapath);
    char *account_path = account_make_path(account_name_canonical);
    char *player_path = player_make_path(character_name_canonical, "player.dat");

    unlink(account_path);
    unlink(player_path);
    unlink(password_path);
    int fd = open(password_path, O_WRONLY | O_CREAT | O_EXCL, SAVE_MODE);
    ck_assert_int_ge(fd, 0);
    const char password[] = "local-light-9!\n";
    ck_assert_int_eq(write(fd, password, sizeof(password) - 1), sizeof(password) - 1);
    ck_assert_int_eq(close(fd), 0);

    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "not-a-preset",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "unknown scenario preset"));
    ck_assert(account_provision_from_file(account_name,
                                          password_path,
                                          character_name,
                                          "human_male",
                                          "lighting-radiance-inside",
                                          VS(error)));
    ck_assert_uint_eq(todtick, 23);

    FILE *fp = fopen(player_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[HUGE_BUF * 4];
    size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
    ck_assert(!ferror(fp));
    contents[length] = '\0';
    fclose(fp);
    ck_assert_ptr_nonnull(
        strstr(contents,
               "map /shattered_islands/strakewood_island/greyton/house/luxury_house_0_0"));
    ck_assert_ptr_nonnull(strstr(contents, "bed_x 8\nbed_y 19\n"));
    ck_assert_ptr_nonnull(strstr(contents, "arch human_male"));
    ck_assert_ptr_nonnull(strstr(contents, "x 8\ny 19\n"));
    ck_assert_ptr_nonnull(strstr(contents, "arch mithril_lamp"));

    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    ck_assert_int_eq(unlink(password_path), 0);
    free(account_path);
    free(player_path);
}
END_TEST

START_TEST(test_account_provision_lighting_preset_rolls_back) {
    const char *account_name = "ScenarioRollback";
    const char *account_name_canonical = "scenariorollback";
    const char *character_name = "scenario rollback";
    const char *character_name_canonical = "Scenario Rollback";
    char error[HUGE_BUF];
    char password_path[HUGE_BUF];
    char clock_path[HUGE_BUF];
    snprintf(VS(password_path), "%s/scenario-rollback-password", settings.datapath);
    snprintf(VS(clock_path), "%s/clockdata", settings.datapath);
    char *account_path = account_make_path(account_name_canonical);
    char *player_path = player_make_path(character_name_canonical, "player.dat");
    char *metrics_path = player_make_path(character_name_canonical, "metrics.dat");

    unlink(account_path);
    unlink(player_path);
    unlink(metrics_path);
    unlink(password_path);
    unlink(clock_path);
    ck_assert_int_eq(mkdir(clock_path, 0700), 0);
    int fd = open(password_path, O_WRONLY | O_CREAT | O_EXCL, SAVE_MODE);
    ck_assert_int_ge(fd, 0);
    const char password[] = "local-rollback-9!\n";
    ck_assert_int_eq(write(fd, password, sizeof(password) - 1), sizeof(password) - 1);
    ck_assert_int_eq(close(fd), 0);

    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "lighting-radiance-inside",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "world clock"));
    struct stat statbuf;
    ck_assert_int_eq(stat(account_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    ck_assert_int_eq(stat(player_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);
    ck_assert_int_eq(stat(metrics_path, &statbuf), -1);
    ck_assert_int_eq(errno, ENOENT);

    ck_assert_int_eq(rmdir(clock_path), 0);
    ck_assert_int_eq(unlink(password_path), 0);
    free(account_path);
    free(player_path);
    free(metrics_path);
}
END_TEST

static char *exploration_test_path(const char *name) {
    char *base = account_make_path(name);
    char *path = xmalloc(strlen(base) + sizeof(".exploration"));
    sprintf(path, "%s.exploration", base);
    free(base);
    path_ensure_directories(path);
    return path;
}

START_TEST(test_exploration_account_round_trip) {
    char *path = exploration_test_path("exploretest");
    unlink(path);
    socket_struct first = {.state = ST_PLAYING, .account = "exploretest"};
    socket_struct second = {.state = ST_PLAYING, .account = "exploretest"};
    socket_struct other = {.state = ST_PLAYING, .account = "exploreother"};
    exploration_begin(&first);
    ck_assert(exploration_mark(&first, "/world/start", 17, 19, 16, 18));
    ck_assert(!exploration_mark(&first, "/world/start", 17, 19, 16, 18));
    exploration_begin(&second);
    ck_assert(exploration_visited(&second, "/world/start", 16, 18));
    ck_assert(exploration_mark(&second, "/world/start", 17, 19, 0, 0));
    ck_assert(exploration_visited(&first, "/world/start", 0, 0));
    exploration_begin(&other);
    ck_assert(!exploration_visited(&other, "/world/start", 16, 18));
    exploration_end(&other);
    exploration_end(&first);
    exploration_end(&second);
    socket_buffer_clear(&first);
    socket_buffer_clear(&second);
    socket_buffer_clear(&other);
    exploration_shutdown();

    /* Fresh process state and socket replay the account's union of characters. */
    exploration_begin(&first);
    ck_assert(exploration_visited(&first, "/world/start", 16, 18));
    ck_assert(exploration_visited(&first, "/world/start", 0, 0));
    ck_assert(!exploration_visited(&first, "/world/start", 1, 0));
    ck_assert_uint_eq(first.packet_queue_count, 4); /* RESET + MAP, each framed. */
    struct stat st;
    ck_assert_int_eq(stat(path, &st), 0);
#ifndef WIN32
    ck_assert_uint_eq(st.st_mode & 0777, 0600);
#endif
    exploration_end(&first);
    socket_buffer_clear(&first);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_bounds_and_geometry_change) {
    char *path = exploration_test_path("explorebounds");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "explorebounds"};
    exploration_begin(&ns);
    ck_assert(!exploration_mark(&ns, "/world/../secret", 1, 1, 0, 0));
    ck_assert(!exploration_mark(&ns, "relative", 1, 1, 0, 0));
    ck_assert(!exploration_mark(&ns, "/world", 0, 1, 0, 0));
    ck_assert(!exploration_mark(&ns, "/world", 257, 1, 0, 0));
    ck_assert(!exploration_mark(&ns, "/world", 1, 1, 1, 0));
    ck_assert(exploration_mark(&ns, "/world", 256, 256, 255, 255));
    ck_assert(exploration_visited(&ns, "/world", 255, 255));
    ck_assert(exploration_mark(&ns, "/world", 2, 2, 1, 1));
    ck_assert(!exploration_visited(&ns, "/world", 255, 255));
    exploration_end(&ns);
    socket_buffer_clear(&ns);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_corrupt_file_preserved) {
    char *path = exploration_test_path("explorecorrupt");
    /* Valid magic followed by a truncated record. */
    static const uint8_t bad[] = {'A', 'E', 'X', 'P', '0', '0', '0', '1', 0, 6, '/', 'x'};
    ck_assert(path_write_atomic(path, bad, sizeof(bad), 0600));
    socket_struct ns = {.state = ST_PLAYING, .account = "explorecorrupt"};
    exploration_begin(&ns);
    ck_assert(!exploration_mark(&ns, "/world", 1, 1, 0, 0));
    exploration_end(&ns);
    socket_buffer_clear(&ns);
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    uint8_t actual[sizeof(bad)];
    ck_assert_uint_eq(fread(actual, 1, sizeof(actual), fp), sizeof(actual));
    ck_assert_int_eq(memcmp(actual, bad, sizeof(bad)), 0);
    ck_assert_int_eq(fgetc(fp), EOF);
    fclose(fp);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_failed_save_retried) {
    char *path = exploration_test_path("exploreretry");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "exploreretry"};
    exploration_begin(&ns);
    ck_assert(exploration_mark(&ns, "/world", 1, 1, 0, 0));
    /* Block the atomic rename with a directory, after successful empty load. */
    ck_assert_int_eq(mkdir(path, 0700), 0);
    exploration_end(&ns);
    socket_buffer_clear(&ns);
    ck_assert_int_eq(rmdir(path), 0);
    exploration_begin(&ns);
    ck_assert(exploration_visited(&ns, "/world", 0, 0));
    exploration_end(&ns);
    socket_buffer_clear(&ns);
    exploration_shutdown();
    exploration_begin(&ns);
    ck_assert(exploration_visited(&ns, "/world", 0, 0));
    exploration_end(&ns);
    socket_buffer_clear(&ns);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_snapshot_batches) {
    char *path = exploration_test_path("explorebatch");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "explorebatch"};
    exploration_begin(&ns);
    for (unsigned i = 0; i < 40; i++) {
        char name[32];
        snprintf(VS(name), "/world/map%u", i);
        ck_assert(exploration_mark(&ns, name, 1, 1, 0, 0));
    }
    socket_buffer_clear(&ns);
    exploration_flush(&ns, true);
    ck_assert_uint_eq(ns.packet_queue_count, 64);
    socket_buffer_clear(&ns);
    exploration_flush(&ns, false);
    ck_assert_uint_eq(ns.packet_queue_count, 16);
    socket_buffer_clear(&ns);
    exploration_flush(&ns, false);
    ck_assert_uint_eq(ns.packet_queue_count, 0);
    exploration_end(&ns);
    unlink(path);
    free(path);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("account");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_set_timeout(tc_core, 30);
    tcase_add_test(tc_core, test_account_provision);
    tcase_add_test(tc_core, test_exploration_account_round_trip);
    tcase_add_test(tc_core, test_exploration_bounds_and_geometry_change);
    tcase_add_test(tc_core, test_exploration_corrupt_file_preserved);
    tcase_add_test(tc_core, test_exploration_failed_save_retried);
    tcase_add_test(tc_core, test_exploration_snapshot_batches);
    tcase_add_test(tc_core, test_account_provision_rejects_invalid_inputs);
    tcase_add_test(tc_core, test_account_provision_password_file_permissions);
    tcase_add_test(tc_core, test_account_provision_lighting_preset);
    tcase_add_test(tc_core, test_account_provision_lighting_preset_rolls_back);
    return s;
}

void check_server_account(void) {
    check_run_suite(suite(), __FILE__);
}
