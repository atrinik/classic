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
#include <auth_worker.h>
#include <access_server.h>
#include <arch.h>
#include <exploration.h>
#include <server.h>
#include <toolkit/packet.h>
#include <initialization.h>
#include <player.h>
#include <object.h>
#include <toolkit/path.h>
#include <toolkit/datetime.h>

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
    ck_assert_int_eq(path_secret_create_atomic(password_path, password, sizeof(password) - 1),
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
    ck_assert_int_eq(
        path_secret_create_atomic(password_path, empty_password, sizeof(empty_password) - 1),
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
    ck_assert_int_eq(
        path_secret_create_atomic(password_path, valid_password, sizeof(valid_password) - 1),
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
    /* The release content lock may precede the companion ink archetype. This
     * bounded fixture tests provisioning, not release content acceptance. */
    archetype_t *ink_arch = arch_find("ink_bottle");
    bool previous_arch_in_init = arch_in_init;
    if (ink_arch == NULL) {
        archetype_t *oil = arch_find("oil_lamp");
        ck_assert_ptr_nonnull(oil);
        arch_in_init = true;
        ink_arch = arch_clone(oil);
        ink_arch->name = add_string("ink_bottle");
        FREE_AND_COPY_HASH(ink_arch->clone.name, "ink bottle");
        FREE_AND_COPY_HASH(ink_arch->clone.race, "writing_ink");
        ink_arch->clone.stats.food = 1000;
        ink_arch->clone.nrof = 1;
        arch_add(ink_arch);
        arch_in_init = previous_arch_in_init;
    }
    if (_i == 2) {
        HASH_DEL(arch_table, ink_arch);
    }
    player_save_fail_for_test(_i == 1);
    bool provisioned = account_provision_from_file(account_name,
                                                   password_path,
                                                   character_name,
                                                   "human_male",
                                                   "writing-books",
                                                   VS(error));
    player_save_fail_for_test(false);
    if (_i == 2) {
        arch_in_init = true;
        arch_add(ink_arch);
        arch_in_init = previous_arch_in_init;
    }
    ck_assert_uint_eq(todtick, previous_hour);
    if (_i != 0) {
        ck_assert(!provisioned);
        ck_assert_ptr_nonnull(strstr(error, _i == 1 ? "save scenario player" : "scenario item: ink_bottle"));
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

static char *exploration_test_path(const char *name) {
    char *base = account_make_path(name);
    char *path = xmalloc(strlen(base) + sizeof(".exploration"));
    sprintf(path, "%s.exploration", base);
    free(base);
    path_ensure_directories(path);
    return path;
}

static packet_error_t
exploration_test_request(socket_struct *ns, const char *path, const uint8_t *bits, size_t bytes) {
    packet_struct *packet = packet_new(SERVER_CMD_REGION_EXPLORATION, 0, 256);
    packet_writer_write_uint8(packet, 0);
    packet_writer_write_cstring(packet, path);
    packet_writer_write_uint16(packet, bytes);
    if (bytes != 0) {
        packet_writer_write_bytes(packet, bits, bytes);
    }
    packet_reader_scope_t scope;
    packet_reader_scope_begin(&scope);
    socket_command_region_exploration(ns, NULL, packet->data, packet->len, 0);
    packet_error_t error = packet_reader_scope_finish(&scope);
    packet_free(packet);
    return error;
}

static packet_struct *exploration_test_response(socket_struct *ns) {
    for (packet_struct *packet = ns->packets; packet != NULL; packet = packet->next) {
        if (packet->type == CLIENT_CMD_REGION_EXPLORATION) {
            return packet;
        }
    }
    return NULL;
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
    ck_assert_uint_eq(first.packet_queue_count, 2); /* Identity RESET only; no login replay. */
    ck_assert_int_eq(exploration_test_request(&first, "/world/start", NULL, 0), PACKET_ERROR_NONE);
    socket_buffer_clear(&first);
    exploration_flush(&first, false);
    ck_assert_ptr_nonnull(exploration_test_response(&first));
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

START_TEST(test_exploration_bounds_and_stable_layout) {
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
    ck_assert(exploration_visited(&ns, "/world", 255, 255));
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
    ck_assert(!exploration_end_checked(&ns));
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

START_TEST(test_exploration_shutdown_propagates_detached_save_failure) {
    char *path = exploration_test_path("exploreshutdown");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "exploreshutdown"};
    exploration_begin(&ns);
    ck_assert(exploration_mark(&ns, "/world", 1, 1, 0, 0));
    ck_assert_int_eq(mkdir(path, 0700), 0);
    ck_assert(!exploration_end_checked(&ns));
    ck_assert_ptr_null(ns.exploration);
    socket_buffer_clear(&ns);
    ck_assert(!exploration_shutdown_checked());
    ck_assert(exploration_shutdown_checked()); /* Empty cleanup remains idempotent. */
    ck_assert_int_eq(rmdir(path), 0);
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
    ck_assert_uint_eq(exploration_test_response(&ns)->data[0], 1);
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

START_TEST(test_exploration_reconciliation_is_sparse_and_never_grants_bits) {
    char *path = exploration_test_path("exploredelta");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "exploredelta"};
    exploration_begin(&ns);
    packet_struct *reset = exploration_test_response(&ns);
    ck_assert_ptr_nonnull(reset);
    ck_assert_uint_eq(reset->data[0], 0);
    ck_assert_str_eq((char *)reset->data + 1, ns.account);
    ck_assert(exploration_mark(&ns, "/world/delta", 16, 16, 0, 0));
    ck_assert(exploration_mark(&ns, "/world/delta", 16, 16, 8, 0));
    exploration_flush(&ns, true);
    socket_buffer_clear(&ns);

    uint8_t cached[32] = {1};
    ck_assert_int_eq(exploration_test_request(&ns, "/world/delta", cached, sizeof(cached)),
                     PACKET_ERROR_NONE);
    exploration_flush(&ns, false);
    packet_struct *response = exploration_test_response(&ns);
    ck_assert_ptr_nonnull(response);
    packet_reader_t reader;
    packet_reader_init(&reader, response->data, response->len);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 2);
    char map_path[256];
    ck_assert(packet_reader_read_string(&reader, VS(map_path)));
    ck_assert_str_eq(map_path, "/world/delta");
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 16);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 16);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    ck_assert_uint_eq(packet_reader_read_uint16(&reader), 1);
    ck_assert_uint_eq(packet_reader_read_uint8(&reader), 1);
    ck_assert(packet_reader_finish(&reader));
    socket_buffer_clear(&ns);

    cached[1] = 1;
    ck_assert_int_eq(exploration_test_request(&ns, "/world/delta", cached, sizeof(cached)),
                     PACKET_ERROR_NONE);
    exploration_flush(&ns, false);
    ck_assert_uint_eq(ns.packet_queue_count, 0);
    memset(cached, 255, sizeof(cached));
    ck_assert_int_eq(exploration_test_request(&ns, "/world/delta", cached, sizeof(cached)),
                     PACKET_ERROR_NONE);
    exploration_flush(&ns, false);
    ck_assert_uint_eq(ns.packet_queue_count, 0);
    ck_assert(!exploration_visited(&ns, "/world/delta", 1, 0));

    ck_assert_int_eq(exploration_test_request(&ns, "/world/unknown", cached, 1), PACKET_ERROR_NONE);
    exploration_flush(&ns, false);
    response = exploration_test_response(&ns);
    ck_assert_ptr_nonnull(response);
    ck_assert_uint_eq(response->data[0], 3);
    ck_assert_str_eq((char *)response->data + 1, "/world/unknown");
    ck_assert(!exploration_visited(&ns, "/world/unknown", 0, 0));
    socket_buffer_clear(&ns);
    exploration_end(&ns);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_reconciliation_rejects_malformed_requests) {
    char *path = exploration_test_path("exploreinvalid");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "exploreinvalid"};
    exploration_begin(&ns);
    ck_assert(exploration_mark(&ns, "/world", 1, 1, 0, 0));
    exploration_flush(&ns, true);
    socket_buffer_clear(&ns);
    uint8_t cached[2] = {1, 0};
    ck_assert_int_ne(exploration_test_request(&ns, "/world", cached, 2), PACKET_ERROR_NONE);
    cached[0] = 255;
    ck_assert_int_ne(exploration_test_request(&ns, "/world", cached, 1), PACKET_ERROR_NONE);
    ck_assert_int_ne(exploration_test_request(&ns, "/../world", NULL, 0), PACKET_ERROR_NONE);
    ck_assert_int_ne(exploration_test_request(&ns, "relative", NULL, 0), PACKET_ERROR_NONE);
    /* Truncated, unknown-op and trailing data fail without enqueuing a response. */
    uint8_t bad[][7] = {{0}, {1, '/', 'x', 0, 0, 0}, {0, '/', 'x', 0, 0, 0, 42}};
    const size_t bad_lengths[] = {1, 6, 7};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        packet_reader_scope_t scope;
        packet_reader_scope_begin(&scope);
        socket_command_region_exploration(&ns, NULL, bad[i], bad_lengths[i], 0);
        ck_assert_int_ne(packet_reader_scope_finish(&scope), PACKET_ERROR_NONE);
    }
    ns.state = ST_LOGIN;
    ck_assert_int_ne(exploration_test_request(&ns, "/world", NULL, 0), PACKET_ERROR_NONE);
    ns.state = ST_PLAYING;
    exploration_flush(&ns, false);
    ck_assert_uint_eq(ns.packet_queue_count, 0);
    ck_assert(exploration_visited(&ns, "/world", 0, 0));
    exploration_end(&ns);
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_session_flush_is_local_and_backpressure_retains_bits) {
    char *path = exploration_test_path("explorequeue");
    unlink(path);
    socket_struct first = {.state = ST_PLAYING, .account = "explorequeue"};
    socket_struct second = {.state = ST_PLAYING, .account = "explorequeue"};
    socket_struct unrelated[64] = {0};
    char unrelated_names[64][32];
    exploration_begin(&first);
    exploration_begin(&second);
    for (unsigned i = 0; i < 64; i++) {
        snprintf(VS(unrelated_names[i]), "exploreunrelated%02u", i);
        unrelated[i].state = ST_PLAYING;
        unrelated[i].account = unrelated_names[i];
        exploration_begin(&unrelated[i]);
        socket_buffer_clear(&unrelated[i]);
    }
    socket_buffer_clear(&first);
    socket_buffer_clear(&second);
    exploration_stats_reset();
    ck_assert(exploration_mark(&first, "/world", 24, 24, 1, 1));
    ck_assert_uint_eq(exploration_stats_get().broadcast_sessions, 2);
    first.packet_queue_bytes = 1024U * 1024U + 1;
    exploration_flush(&first, true);
    ck_assert_uint_eq(exploration_stats_get().pending_visits, 0);
    first.packet_queue_bytes = 0;
    exploration_flush(&first, false);
    ck_assert_uint_eq(exploration_stats_get().pending_visits, 1);
    ck_assert_ptr_nonnull(exploration_test_response(&first));
    ck_assert_uint_eq(second.packet_queue_count, 0);
    for (unsigned i = 0; i < 64; i++) {
        ck_assert_uint_eq(unrelated[i].packet_queue_count, 0);
    }
    exploration_flush(&second, false);
    ck_assert_ptr_nonnull(exploration_test_response(&second));
    ck_assert_uint_eq(exploration_stats_get().pending_visits, 2);
    socket_buffer_clear(&first);
    socket_buffer_clear(&second);
    exploration_stats_reset();
    for (unsigned i = 0; i < 1000; i++) {
        ck_assert(!exploration_mark(&first, "/world", 24, 24, 1, 1));
        exploration_flush(&first, false);
        exploration_flush(&second, false);
        exploration_flush(&unrelated[i % 64], false);
    }
    exploration_test_stats stats = exploration_stats_get();
    ck_assert_uint_eq(stats.account_lookups, 0);
    ck_assert_uint_eq(stats.map_lookups, 0);
    ck_assert_uint_eq(stats.pending_visits, 0);
    ck_assert_uint_eq(stats.broadcast_sessions, 0);
    ck_assert_uint_eq(stats.save_records, 0);
    exploration_end(&first);
    exploration_end(&second);
    for (unsigned i = 0; i < 64; i++) {
        exploration_end(&unrelated[i]);
    }
    unlink(path);
    free(path);
}
END_TEST

START_TEST(test_exploration_ten_thousand_maps) {
    unsigned dimension = _i == 0 ? 24 : 256;
    char *path = exploration_test_path("explorescale");
    unlink(path);
    socket_struct ns = {.state = ST_PLAYING, .account = "explorescale"};
    exploration_begin(&ns);
    socket_buffer_clear(&ns);
    exploration_stats_reset();
    uint64_t started = datetime_monotonic_us();
    for (unsigned i = 0; i < 10000; i++) {
        char name[64];
        snprintf(VS(name), "/world/map%05u", i);
        ck_assert(exploration_mark(&ns, name, dimension, dimension, dimension - 1, dimension - 1));
    }
    uint64_t mark_us = datetime_monotonic_us() - started;
    ck_assert_uint_eq(exploration_stats_get().account_lookups, 0);
    ck_assert_uint_eq(exploration_stats_get().map_lookups, 10000);
    ck_assert(!exploration_mark(&ns, "/world/overflow", 1, 1, 0, 0));
    started = datetime_monotonic_us();
    exploration_end(&ns);
    uint64_t save_us = datetime_monotonic_us() - started;
    exploration_test_stats saved = exploration_stats_get();
    ck_assert_uint_eq(saved.save_records, 10000);
    struct stat st;
    ck_assert_int_eq(stat(path, &st), 0);
    ck_assert_uint_eq(saved.save_bytes, (uint64_t)st.st_size);
    exploration_stats_reset();
    started = datetime_monotonic_us();
    exploration_begin(&ns);
    uint64_t load_us = datetime_monotonic_us() - started;
    ck_assert_uint_eq(exploration_stats_get().load_records, 10000);
    ck_assert_uint_eq(exploration_stats_get().map_lookups, 10000);
    ck_assert_uint_eq(ns.packet_queue_count,
                      2); /* Only account identity, regardless of map count. */
    socket_buffer_clear(&ns);
    ck_assert(exploration_visited(&ns, "/world/map09999", dimension - 1, dimension - 1));
    exploration_stats_reset();
    for (unsigned i = 0; i < 1000; i++) {
        exploration_flush(&ns, false);
    }
    ck_assert_uint_eq(exploration_stats_get().pending_visits, 0);
    ck_assert_uint_eq(exploration_stats_get().map_lookups, 0);
    ck_assert_uint_eq(exploration_stats_get().sent_records, 0);
    printf("exploration_scale maps=10000 dimension=%u disk_bytes=%" PRIu64 " mark_us=%" PRIu64
           " save_us=%" PRIu64 " load_us=%" PRIu64 "\n",
           dimension,
           (uint64_t)st.st_size,
           mark_us,
           save_us,
           load_us);
    fflush(stdout);
    exploration_end(&ns);
    unlink(path);
    free(path);
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
    char *exploration_path = exploration_test_path(account_name);
    unlink(exploration_path);
    if (_i == 4) {
        exploration_begin(pl->cs);
        ck_assert(exploration_mark(pl->cs, "/world", 1, 1, 0, 0));
        ck_assert_int_eq(mkdir(exploration_path, 0700), 0);
    }

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
    if (_i == 4) {
        ck_assert_int_eq(rmdir(exploration_path), 0);
    }
    ck_assert(exploration_shutdown_checked());
    unlink(exploration_path);
    free(exploration_path);

    ck_assert_int_eq(unlink(account_path), 0);
    ck_assert_int_eq(unlink(player_path), 0);
    free(account_path);
    free(player_path);
}
END_TEST

START_TEST(test_access_preserves_normal_command_permissions) {
    char saved_groups[sizeof(settings.default_permission_groups)];
    memcpy(saved_groups, settings.default_permission_groups, sizeof(saved_groups));
    bool saved_required = settings.access_required;
    player pl = {0};
    settings.access_required = true;
    snprintf(VS(settings.default_permission_groups), "[OP]");
    const char *commands[] = {"console", "create", "patch", "config", "password", "/console",
                              "kick", "ban", "freeze"};
    for (size_t i = 0; i < arraysize(commands); i++)
        ck_assert_int_eq(commands_check_permission(&pl, commands[i]), 1);

    settings.default_permission_groups[0] = '\0';
    ck_assert_int_eq(commands_check_permission(&pl, "console"), 0);
    char *permissions[] = {"console", "create", "patch", "config", "password", "kick"};
    pl.cmd_permissions = permissions;
    pl.num_cmd_permissions = arraysize(permissions);
    for (size_t i = 0; i < arraysize(permissions); i++)
        ck_assert_int_eq(commands_check_permission(&pl, permissions[i]), 1);
    ck_assert_int_eq(commands_check_permission(&pl, "ban"), 0);
    settings.access_required = saved_required;
    memcpy(settings.default_permission_groups, saved_groups, sizeof(saved_groups));
}
END_TEST

#ifdef __linux__
static access_outcome_t registration_access_route(void *context, const access_route_t *route) {
    (void)context;
    (void)route;
    return ACCESS_COMMITTED;
}

START_TEST(test_access_preserves_ordinary_registration) {
    char directory[] = "/tmp/atrinik-registration-access-XXXXXX";
    ck_assert_ptr_nonnull(mkdtemp(directory));
    uint8_t identity[32];
    memset(identity, 0x11, sizeof(identity));
    char identity_hex[65];
    memset(identity_hex, '1', 64);
    identity_hex[64] = 0;
    access_store_t *store = NULL;
    ck_assert_int_eq(access_store_open(&store, directory, identity, true, true), ACCESS_COMMITTED);
    access_result_t issue = access_store_issue(store, "11111111111111111111111111111111", 1,
                                              "Registration admission", false, 0, time(NULL),
                                              registration_access_route, NULL);
    ck_assert_int_eq(issue.outcome, ACCESS_COMMITTED);
    access_store_close(store);
    bool saved_required = settings.access_required;
    bool saved_initialize = settings.access_initialize;
    char saved_store[sizeof(settings.access_store)];
    memcpy(saved_store, settings.access_store, sizeof(saved_store));
    settings.access_required = true;
    settings.access_initialize = false;
    snprintf(VS(settings.access_store), "%s", directory);
    access_server_route_for_test(registration_access_route);
    ck_assert(access_server_init(identity_hex));

    object *ob = player_get_dummy("Registration Proof", NULL);
    socket_struct *cs = CONTR(ob)->cs;
    free(cs->account);
    cs->account = NULL;
    cs->state = ST_LOGIN;
    cs->socket_version = SOCKET_VERSION;
    cs->access_transport_authenticated = true;
    cs->access_policy_sent = true;
    cs->setup_completed = true;
    cs->access_authenticated = false;
    cs->access_auth_job = access_server_auth_submit(issue.code);
    access_result_cleanse(&issue);
    ck_assert_uint_ne(cs->access_auth_job, 0);
    uint64_t deadline = datetime_monotonic_ms() + 3000;
    while (cs->access_auth_job != 0 && datetime_monotonic_ms() < deadline) {
        socket_access_poll_for_test(cs, NULL);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    ck_assert_uint_eq(cs->access_auth_job, 0);
    ck_assert(cs->access_authenticated);
    ck_assert(socket_connection_admitted(cs));
    access_session_state_t admission = access_server_session_check(&cs->access_token);
    while (admission == ACCESS_SESSION_BUSY && datetime_monotonic_ms() < deadline) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
        admission = access_server_session_check(&cs->access_token);
    }
    ck_assert_int_eq(admission, ACCESS_SESSION_VALID);
    socket_login_deadline_refresh(cs);
    ck_assert(account_auth_start());
    char name[] = "reservationproof";
    char password[] = "local-test-7!";
    char *path = account_make_path(name);
    unlink(path);
    account_register(cs, name, password, password);
    ck_assert_uint_ne(cs->auth_request, 0);
    deadline = datetime_monotonic_ms() + 5000;
    while (cs->auth_request != 0 && datetime_monotonic_ms() < deadline) {
        account_auth_poll();
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    ck_assert_uint_eq(cs->auth_request, 0);
    ck_assert_ptr_nonnull(cs->account);
    ck_assert_str_eq(cs->account, name);
    ck_assert(path_exists(path));
    account_deinit();
    ck_assert_int_eq(unlink(path), 0);
    free(path);
    ck_assert(access_server_shutdown());
    access_server_deinit();
    access_server_route_for_test(NULL);
    settings.access_required = saved_required;
    settings.access_initialize = saved_initialize;
    memcpy(settings.access_store, saved_store, sizeof(saved_store));
    char snapshot[256];
    snprintf(VS(snapshot), "%s/%s", directory, ACCESS_STORE_FILENAME);
    ck_assert_int_eq(unlink(snapshot), 0);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST
#endif

static Suite *suite(void) {
    Suite *s = suite_create("account");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_set_timeout(tc_core, 30);
    tcase_add_loop_test(tc_core, test_checked_logout_propagates_save_failures, 0, 5);
    tcase_add_test(tc_core, test_access_preserves_normal_command_permissions);
#ifdef __linux__
    tcase_add_test(tc_core, test_access_preserves_ordinary_registration);
#endif
    tcase_add_test(tc_core, test_account_provision);
    tcase_add_test(tc_core, test_exploration_account_round_trip);
    tcase_add_test(tc_core, test_exploration_bounds_and_stable_layout);
    tcase_add_test(tc_core, test_exploration_corrupt_file_preserved);
    tcase_add_test(tc_core, test_exploration_failed_save_retried);
    tcase_add_test(tc_core, test_exploration_shutdown_propagates_detached_save_failure);
    tcase_add_test(tc_core, test_exploration_snapshot_batches);
    tcase_add_test(tc_core, test_exploration_reconciliation_is_sparse_and_never_grants_bits);
    tcase_add_test(tc_core, test_exploration_reconciliation_rejects_malformed_requests);
    tcase_add_test(tc_core, test_exploration_session_flush_is_local_and_backpressure_retains_bits);
    tcase_add_loop_test(tc_core, test_exploration_ten_thousand_maps, 0, 2);
    tcase_add_test(tc_core, test_account_provision_rejects_invalid_inputs);
    tcase_add_test(tc_core, test_account_provision_password_file_permissions);
    tcase_add_test(tc_core, test_account_provision_lighting_preset);
    tcase_add_test(tc_core, test_account_provision_brynknot_idle_preserves_clock_and_existing_player);
    tcase_add_test(tc_core, test_account_provision_lighting_preset_rolls_back);
    tcase_add_loop_test(tc_core, test_account_provision_writing_books_roundtrip_and_save_failure, 0, 3);
    return s;
}

void check_server_account(void) {
    check_run_suite(suite(), __FILE__);
}
