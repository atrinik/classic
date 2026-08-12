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
#include <initialization.h>
#include <player.h>

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

    fd = open(password_path, O_WRONLY | O_TRUNC);
    ck_assert_int_ge(fd, 0);
    const char valid_password[] = "local-file-8!\r\n";
    ck_assert_int_eq(write(fd, valid_password, sizeof(valid_password) - 1),
                     sizeof(valid_password) - 1);
    ck_assert_int_eq(close(fd), 0);
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

static Suite *suite(void) {
    Suite *s = suite_create("account");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_set_timeout(tc_core, 30);
    tcase_add_test(tc_core, test_account_provision);
    tcase_add_test(tc_core, test_account_provision_rejects_invalid_inputs);
    tcase_add_test(tc_core, test_account_provision_password_file_permissions);
    tcase_add_test(tc_core, test_account_provision_lighting_preset);
    tcase_add_test(tc_core, test_account_provision_lighting_preset_rolls_back);
    return s;
}

void check_server_account(void) {
    check_run_suite(suite(), __FILE__);
}
