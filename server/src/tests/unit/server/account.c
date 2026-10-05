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
#include <arch.h>
#include <initialization.h>
#include <player.h>
#include <object.h>
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

START_TEST(test_account_provision_brynknot_idle_preserves_clock_and_existing_player) {
    const char *account_name = "scenariobrynknot";
    const char *character_name = "Scenario Brynknot";
    char error[HUGE_BUF];
    char password_path[HUGE_BUF];
    snprintf(VS(password_path), "%s/scenario-brynknot-password", settings.datapath);
    char *account_path = account_make_path(account_name);
    char *player_path = player_make_path(character_name, "player.dat");
    char *metrics_path = player_make_path(character_name, "metrics.dat");
    unlink(account_path);
    unlink(player_path);
    unlink(metrics_path);
    unlink(password_path);
    const char password[] = "local-brynknot-9!\n";
    ck_assert_int_eq(path_secret_create_atomic(password_path, password, sizeof(password) - 1),
                     PATH_SECRET_CREATE_OK);
    unsigned long previous_hour = todtick;
    ck_assert_msg(account_provision_from_file(account_name,
                                               password_path,
                                               character_name,
                                               "human_male",
                                               "brynknot-idle",
                                               VS(error)),
                  "%s", error);
    ck_assert_uint_eq(todtick, previous_hour);
    FILE *fp = fopen(player_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[HUGE_BUF * 4];
    size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
    ck_assert(!ferror(fp));
    contents[length] = '\0';
    fclose(fp);
    ck_assert_ptr_nonnull(strstr(contents, "map /shattered_islands/world_0_70\n"));
    ck_assert_ptr_nonnull(strstr(contents, "bed_x 20\nbed_y 8\n"));
    ck_assert_ptr_nonnull(strstr(contents, "x 20\ny 8\n"));
    ck_assert_ptr_null(strstr(contents, "arch mithril_lamp"));
    ck_assert(!account_provision_from_file(account_name,
                                           password_path,
                                           character_name,
                                           "human_male",
                                           "brynknot-idle",
                                           VS(error)));
    ck_assert_ptr_nonnull(strstr(error, "cannot reserve"));
    fp = fopen(player_path, "rb");
    ck_assert_ptr_nonnull(fp);
    char retained[sizeof(contents)];
    size_t retained_length = fread(retained, 1, sizeof(retained), fp);
    ck_assert(!ferror(fp));
    fclose(fp);
    ck_assert_uint_eq(retained_length, length);
    ck_assert_int_eq(memcmp(contents, retained, length), 0);
    ck_assert_uint_eq(todtick, previous_hour);
    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    unlink(metrics_path);
    ck_assert_int_eq(unlink(password_path), 0);
    free(account_path);
    free(player_path);
    free(metrics_path);
}
END_TEST

static object *writing_scenario_item(object *pl, const char *name) {
    object *found = NULL;
    for (object *item = pl->inv; item != NULL; item = item->below) {
        if (strcmp(item->name, name) == 0) {
            ck_assert_ptr_null(found);
            found = item;
        }
    }
    ck_assert_ptr_nonnull(found);
    ck_assert_uint_eq(found->nrof, 1);
    return found;
}

START_TEST(test_account_provision_writing_books_roundtrip_and_save_failure) {
    const char *account_name = "scenariowriting";
    const char *character_name = "Scenario Writing";
    char error[HUGE_BUF];
    char password_path[HUGE_BUF];
    snprintf(VS(password_path), "%s/scenario-writing-password", settings.datapath);
    char *account_path = account_make_path(account_name);
    char *player_path = player_make_path(character_name, "player.dat");
    char *metrics_path = player_make_path(character_name, "metrics.dat");
    unlink(account_path);
    unlink(player_path);
    unlink(metrics_path);
    unlink(password_path);
    const char password[] = "local-writing-9!\n";
    ck_assert_int_eq(path_secret_create_atomic(password_path, password, sizeof(password) - 1),
                     PATH_SECRET_CREATE_OK);
    unsigned long previous_hour = todtick;
    player_save_fail_for_test(_i == 1);
    bool provisioned = account_provision_from_file(account_name,
                                                   password_path,
                                                   character_name,
                                                   "human_male",
                                                   "writing-books",
                                                   VS(error));
    player_save_fail_for_test(false);
    ck_assert_uint_eq(todtick, previous_hour);
    if (_i == 1) {
        ck_assert(!provisioned);
        ck_assert_ptr_nonnull(strstr(error, "save scenario player"));
        struct stat info;
        ck_assert_int_eq(stat(account_path, &info), -1);
        ck_assert_int_eq(stat(player_path, &info), -1);
        ck_assert_int_eq(stat(metrics_path, &info), -1);
    } else {
        ck_assert_msg(provisioned, "%s", error);
        FILE *fp = fopen(player_path, "rb");
        ck_assert_ptr_nonnull(fp);
        object *placeholder = player_get_dummy(character_name, NULL);
        player *loaded = CONTR(placeholder);
        object_remove(placeholder, 0);
        placeholder->custom_attrset = NULL;
        object_destroy(placeholder);
        loaded->ob = object_get();
        ck_assert(player_load_stream(loaded, fp));
        fclose(fp);
        loaded->ob->custom_attrset = loaded;
        ck_assert_str_eq(loaded->maplevel, "/shattered_islands/world_0_70");
        ck_assert_int_eq(loaded->ob->x, 20);
        ck_assert_int_eq(loaded->ob->y, 8);
        ck_assert_int_eq(loaded->bed_x, 20);
        ck_assert_int_eq(loaded->bed_y, 8);
        object *pen = writing_scenario_item(loaded->ob, "writing pen");
        ck_assert_int_eq(pen->type, SKILL_ITEM);
        ck_assert_int_eq(pen->stats.sp, SK_INSCRIPTION);
        ck_assert_int_eq(pen->stats.food, 1000);
        ck_assert_int_eq(pen->stats.maxhp, 1000);
        pen = writing_scenario_item(loaded->ob, "dry writing pen");
        ck_assert_int_eq(pen->stats.food, 1);
        ck_assert_int_eq(pen->stats.maxhp, 1000);
        object *ink = writing_scenario_item(loaded->ob, "ink bottle");
        ck_assert_str_eq(ink->arch->name, "ink_bottle");
        ck_assert_int_eq(ink->stats.food, 1000);
        const char *drafts[] = {"Writing Draft One", "Writing Draft Two", "Writing Draft Three"};
        for (size_t i = 0; i < sizeof(drafts) / sizeof(drafts[0]); i++) {
            object *book = writing_scenario_item(loaded->ob, drafts[i]);
            ck_assert_int_eq(book->type, BOOK);
            ck_assert_ptr_null(book->msg);
        }
        object *source = writing_scenario_item(loaded->ob, "Writing Source");
        ck_assert_int_eq(source->type, BOOK);
        ck_assert_str_eq(source->msg, "Existing source text for copying and editing.\n");
        ck_assert_ptr_nonnull(find_skill(loaded->ob, SK_LITERACY));
        ck_assert_ptr_nonnull(find_skill(loaded->ob, SK_INSCRIPTION));
        free_player(loaded);
        ck_assert_int_eq(unlink(account_path), 0);
        ck_assert_int_eq(unlink(player_path), 0);
        unlink(metrics_path);
    }
    ck_assert_int_eq(unlink(password_path), 0);
    free(account_path);
    free(player_path);
    free(metrics_path);
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

START_TEST(test_checked_logout_propagates_save_failures) {
    const char *account_name = "shutdownproof";
    const char *character_name = "Shutdown Proof";
    char error[HUGE_BUF];
    char *account_path = account_make_path(account_name);
    char *player_path = player_make_path(character_name, "player.dat");
    unlink(account_path);
    unlink(player_path);
    ck_assert(account_provision(account_name,
                                "local-test-7!",
                                character_name,
                                "human_male",
                                VS(error)));
    object *ob = player_get_dummy(character_name, NULL);
    player *pl = CONTR(ob);
    free(pl->cs->account);
    pl->cs->account = xstrdup(account_name);

    /* These independent failures must survive the logout cleanup. */
    player_save_fail_for_test(_i == 1);
    account_fail_saves_for_test(_i == 2);
    if (_i == 3) {
        pl->metrics_load_failed = true;
    }
    ck_assert_int_eq(player_disconnect_all_checked(), _i == 0);
    ck_assert_ptr_null(first_player);
    player_save_fail_for_test(false);
    account_fail_saves_for_test(false);
    ck_assert(player_disconnect_all_checked());

    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    free(account_path);
    free(player_path);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("account");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_set_timeout(tc_core, 30);
    tcase_add_loop_test(tc_core, test_checked_logout_propagates_save_failures, 0, 4);
    tcase_add_test(tc_core, test_account_provision);
    tcase_add_test(tc_core, test_account_provision_rejects_invalid_inputs);
    tcase_add_test(tc_core, test_account_provision_password_file_permissions);
    tcase_add_test(tc_core, test_account_provision_lighting_preset);
    tcase_add_test(tc_core, test_account_provision_brynknot_idle_preserves_clock_and_existing_player);
    tcase_add_test(tc_core, test_account_provision_lighting_preset_rolls_back);
    tcase_add_loop_test(tc_core, test_account_provision_writing_books_roundtrip_and_save_failure, 0, 2);
    return s;
}

void check_server_account(void) {
    check_run_suite(suite(), __FILE__);
}
