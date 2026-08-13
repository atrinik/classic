/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
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
#include <gameplay_journal.h>
#include <plugin_hooklist.h>

#ifndef WIN32
#include <sys/wait.h>
#endif

static void remove_fixture(const char *directory) {
    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) == 0 ||
            strcmp(entry->d_name, "journal.lock") == 0) {
            char path[HUGE_BUF];
            snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
            ck_assert_int_eq(unlink(path), 0);
        }
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_int_eq(rmdir(journal_directory), 0);
    ck_assert_int_eq(rmdir(directory), 0);
}

START_TEST(test_intent_commit_abort_and_private_storage) {
    char directory[] = "/tmp/atrinik-gameplay-journal-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    ck_assert_int_eq(chmod(directory, 0700), 0);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server-id", &profile));
    ck_assert(gameplay_journal_available());

    const gameplay_journal_subject_t subject = {
        .account_id = "account\"configured",
        .character_id = "Hero_One\\Configured",
        .map_id = "/world/start",
        .x = 4,
        .y = 7,
    };
    gameplay_journal_change_t change = {
        .subject_id = "currency:gold",
        .lineage_id = "",
        .before = 10,
        .delta = 5,
        .after = 15,
    };
    char committed[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(gameplay_journal_begin(&subject,
                                     GAMEPLAY_JOURNAL_CURRENCY,
                                     "shop.purchase",
                                     &change,
                                     committed));
    ck_assert_uint_eq(strlen(committed), GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE - 1);
    ck_assert(!gameplay_journal_profile_boundary(&profile, "profile.pending"));
    ck_assert(gameplay_journal_commit(committed));
    ck_assert(!gameplay_journal_commit(committed));

    char aborted[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    change.before = 15;
    change.delta = -2;
    change.after = 13;
    ck_assert(
        gameplay_journal_begin(&subject, GAMEPLAY_JOURNAL_QUEST, "quest.reward", &change, aborted));
    ck_assert(gameplay_journal_abort(aborted, "quest.vetoed"));

    char rejected[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    change.after = 99;
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_CURRENCY,
                                      "bad.change",
                                      &change,
                                      rejected));
    ck_assert(!gameplay_journal_abort("unknown", "test.abort"));
    gameplay_journal_deinit();
    ck_assert(!gameplay_journal_available());

    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    struct stat metadata;
    ck_assert_int_eq(stat(journal_directory, &metadata), 0);
    ck_assert_uint_eq(metadata.st_mode & 0777, 0700);
    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    size_t files = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) != 0) {
            continue;
        }
        files++;
        char path[HUGE_BUF];
        snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
        ck_assert_int_eq(stat(path, &metadata), 0);
        ck_assert_uint_eq(metadata.st_mode & 0777, 0600);
        FILE *fp = fopen(path, "rb");
        ck_assert_ptr_ne(fp, NULL);
        char contents[HUGE_BUF];
        size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
        ck_assert(!ferror(fp));
        contents[length] = '\0';
        ck_assert_ptr_ne(strstr(contents, "\"phase\":\"intent\""), NULL);
        ck_assert_ptr_ne(strstr(contents, "\"phase\":\"commit\""), NULL);
        ck_assert_ptr_ne(strstr(contents, "\"phase\":\"abort\""), NULL);
        ck_assert_ptr_eq(strstr(contents, "password"), NULL);
        ck_assert_int_eq(fclose(fp), 0);
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_uint_eq(files, 1);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_long_lived_intent_fails_before_hard_file_limit) {
    char directory[] = "/tmp/atrinik-gameplay-journal-hard-limit-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_file_limit_for_test(1);
    gameplay_journal_hard_limit_for_test(4096);
    const gameplay_journal_subject_t subject = {
        .account_id = "account",
        .character_id = "Character",
        .map_id = "/world/start",
        .x = 1,
        .y = 2,
    };
    const gameplay_journal_change_t change = {
        .subject_id = "currency:gold",
        .lineage_id = "",
        .before = 1,
        .delta = 1,
        .after = 2,
    };
    char held[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(
        gameplay_journal_begin(&subject, GAMEPLAY_JOURNAL_CURRENCY, "test.held", &change, held));
    while (gameplay_journal_available()) {
        char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
        if (!gameplay_journal_begin(&subject,
                                    GAMEPLAY_JOURNAL_CURRENCY,
                                    "test.cycle",
                                    &change,
                                    transaction)) {
            break;
        }
        if (!gameplay_journal_commit(transaction)) {
            break;
        }
    }
    ck_assert(!gameplay_journal_available());
    gameplay_journal_file_limit_for_test(8U * 1024U * 1024U);
    gameplay_journal_hard_limit_for_test(9U * 1024U * 1024U);
    gameplay_journal_deinit();

    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) != 0) {
            continue;
        }
        char path[HUGE_BUF];
        snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
        struct stat metadata;
        ck_assert_int_eq(stat(path, &metadata), 0);
        ck_assert_int_le(metadata.st_size, 4096);
    }
    ck_assert_int_eq(closedir(dir), 0);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_plugin_journal_hooks_are_append_only) {
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, gameplay_journal_player_begin),
                      offsetof(struct plugin_hooklist, socket_server_command_queue_append));
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, gameplay_journal_commit),
                      offsetof(struct plugin_hooklist, gameplay_journal_player_begin));
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, gameplay_journal_abort),
                      offsetof(struct plugin_hooklist, gameplay_journal_commit));
}
END_TEST

START_TEST(test_init_fails_closed_for_unsafe_directory_or_profile) {
    char directory[] = "/tmp/atrinik-gameplay-journal-unsafe-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    char path[MAX_BUF];
    snprintf(VS(path), "%s/gameplay-journal", directory);
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_int_eq(fclose(fp), 0);
    const gameplay_journal_profile_t profile = {
        .id = "invalid profile",
        .schema = 1,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(!gameplay_journal_init(directory, "server", &profile));
    ck_assert(!gameplay_journal_available());
    ck_assert_int_eq(unlink(path), 0);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_init_rejects_insecure_directory_permissions) {
    char directory[] = "/tmp/atrinik-gameplay-journal-mode-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    char path[MAX_BUF];
    snprintf(VS(path), "%s/gameplay-journal", directory);
    ck_assert_int_eq(mkdir(path, 0755), 0);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
#ifndef WIN32
    ck_assert(!gameplay_journal_init(directory, "server", &profile));
    ck_assert(!gameplay_journal_available());
#endif
    ck_assert_int_eq(rmdir(path), 0);
    ck_assert_int_eq(rmdir(directory), 0);
}
END_TEST

START_TEST(test_retention_stays_bounded_when_opening_a_file) {
    char directory[] = "/tmp/atrinik-gameplay-journal-retention-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    ck_assert_int_eq(mkdir(journal_directory, 0700), 0);
    char sentinel[HUGE_BUF];
    snprintf(VS(sentinel), "%s/journal-investigation.jsonl", journal_directory);
    int sentinel_fd = open(sentinel, O_WRONLY | O_CREAT | O_EXCL, 0600);
    ck_assert_int_ge(sentinel_fd, 0);
    ck_assert_int_eq(close(sentinel_fd), 0);
    for (size_t i = 0; i < 16; i++) {
        char path[HUGE_BUF];
        snprintf(VS(path),
                 "%s/journal-00000000000000000000000000000000-%04zu.jsonl",
                 journal_directory,
                 i);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        ck_assert_int_ge(fd, 0);
        ck_assert_int_eq(close(fd), 0);
    }
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_deinit();

    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    size_t files = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) == 0 &&
            strcmp(entry->d_name, "journal-investigation.jsonl") != 0) {
            files++;
        }
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_uint_eq(files, 16);
    ck_assert_int_eq(access(sentinel, F_OK), 0);
    ck_assert_int_eq(unlink(sentinel), 0);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_real_rotation_and_retention_keep_complete_transactions) {
    char directory[] = "/tmp/atrinik-gameplay-journal-rotation-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_file_limit_for_test(1);
    const gameplay_journal_subject_t subject = {
        .account_id = "account",
        .character_id = "Character",
        .map_id = "/world/start",
        .x = 1,
        .y = 2,
    };
    const gameplay_journal_change_t change = {
        .subject_id = "currency:gold",
        .lineage_id = "",
        .before = 1,
        .delta = 1,
        .after = 2,
    };
    for (size_t i = 0; i < 18; i++) {
        char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
        ck_assert(gameplay_journal_begin(&subject,
                                         GAMEPLAY_JOURNAL_CURRENCY,
                                         "test.rotate",
                                         &change,
                                         transaction));
        ck_assert(gameplay_journal_commit(transaction));
    }
    gameplay_journal_file_limit_for_test(8U * 1024U * 1024U);
    gameplay_journal_deinit();

    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    size_t files = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) == 0) {
            files++;
            char path[HUGE_BUF];
            snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
            struct stat metadata;
            ck_assert_int_eq(stat(path, &metadata), 0);
            ck_assert_int_gt(metadata.st_size, 0);
        }
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_uint_eq(files, 16);
    remove_fixture(directory);
}
END_TEST

#ifndef WIN32
static void crash_writer(const char *directory, int phase) {
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    if (phase != 0) {
        const gameplay_journal_subject_t subject = {
            .account_id = "account",
            .character_id = "Character",
            .map_id = "/world/start",
            .x = 1,
            .y = 2,
        };
        const gameplay_journal_change_t change = {
            .subject_id = "currency:gold",
            .lineage_id = "",
            .before = 1,
            .delta = 1,
            .after = 2,
        };
        char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
        ck_assert(gameplay_journal_begin(&subject,
                                         GAMEPLAY_JOURNAL_CURRENCY,
                                         "test.crash",
                                         &change,
                                         transaction));
        if (phase == 2) {
            ck_assert(gameplay_journal_commit(transaction));
        }
    }
    _exit(EXIT_SUCCESS);
}

START_TEST(test_abrupt_process_crash_preserves_synced_phases) {
    for (int phase = 0; phase < 3; phase++) {
        char directory[] = "/tmp/atrinik-gameplay-journal-crash-XXXXXX";
        ck_assert_ptr_ne(mkdtemp(directory), NULL);
        pid_t child = fork();
        ck_assert_int_ge(child, 0);
        if (child == 0) {
            crash_writer(directory, phase);
        }
        int status;
        ck_assert_int_eq(waitpid(child, &status, 0), child);
        ck_assert(WIFEXITED(status));
        ck_assert_int_eq(WEXITSTATUS(status), EXIT_SUCCESS);

        char journal_directory[MAX_BUF];
        snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
        DIR *dir = opendir(journal_directory);
        ck_assert_ptr_ne(dir, NULL);
        struct dirent *entry;
        bool saw_intent = false, saw_commit = false;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "journal-", 8) != 0) {
                continue;
            }
            char path[HUGE_BUF];
            snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
            FILE *fp = fopen(path, "rb");
            ck_assert_ptr_ne(fp, NULL);
            char contents[HUGE_BUF];
            size_t length = fread(contents, 1, sizeof(contents) - 1, fp);
            contents[length] = '\0';
            saw_intent |= strstr(contents, "\"phase\":\"intent\"") != NULL;
            saw_commit |= strstr(contents, "\"phase\":\"commit\"") != NULL;
            ck_assert_int_eq(fclose(fp), 0);
        }
        ck_assert_int_eq(closedir(dir), 0);
        ck_assert_int_eq(saw_intent, phase >= 1);
        ck_assert_int_eq(saw_commit, phase == 2);
        remove_fixture(directory);
    }
}
END_TEST

START_TEST(test_second_writer_is_rejected_while_lock_is_held) {
    char directory[] = "/tmp/atrinik-gameplay-journal-lock-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    pid_t child = fork();
    ck_assert_int_ge(child, 0);
    if (child == 0) {
        bool initialized = gameplay_journal_init(directory, "server", &profile);
        _exit(initialized ? EXIT_FAILURE : EXIT_SUCCESS);
    }
    int status;
    ck_assert_int_eq(waitpid(child, &status, 0), child);
    ck_assert(WIFEXITED(status));
    ck_assert_int_eq(WEXITSTATUS(status), EXIT_SUCCESS);
    gameplay_journal_deinit();
    remove_fixture(directory);
}
END_TEST
#endif

START_TEST(test_append_failure_disables_journal) {
#ifndef WIN32
    char directory[] = "/tmp/atrinik-gameplay-journal-write-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));

    const gameplay_journal_subject_t subject = {
        .account_id = "account",
        .character_id = "character",
        .map_id = "/world/start",
        .x = 1,
        .y = 2,
    };
    const gameplay_journal_change_t change = {
        .subject_id = "currency:gold",
        .lineage_id = "",
        .before = 1,
        .delta = 1,
        .after = 2,
    };
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    gameplay_journal_fail_writes_for_test(true);
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_CURRENCY,
                                      "test.write",
                                      &change,
                                      transaction));
    ck_assert(!gameplay_journal_available());

    gameplay_journal_fail_writes_for_test(false);
    gameplay_journal_deinit();
    remove_fixture(directory);
#endif
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("gameplay_journal");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_test(tc_core, test_intent_commit_abort_and_private_storage);
    tcase_add_test(tc_core, test_plugin_journal_hooks_are_append_only);
    tcase_add_test(tc_core, test_init_fails_closed_for_unsafe_directory_or_profile);
    tcase_add_test(tc_core, test_init_rejects_insecure_directory_permissions);
    tcase_add_test(tc_core, test_long_lived_intent_fails_before_hard_file_limit);
    tcase_add_test(tc_core, test_retention_stays_bounded_when_opening_a_file);
    tcase_add_test(tc_core, test_real_rotation_and_retention_keep_complete_transactions);
    tcase_add_test(tc_core, test_append_failure_disables_journal);
#ifndef WIN32
    tcase_add_test(tc_core, test_abrupt_process_crash_preserves_synced_phases);
    tcase_add_test(tc_core, test_second_writer_is_rejected_while_lock_is_held);
#endif
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_gameplay_journal(void) {
    check_run_suite(suite(), __FILE__);
}
