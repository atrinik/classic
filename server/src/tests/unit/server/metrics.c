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
#include <metrics.h>
#include <commands.h>
#include <initialization.h>
#include <player.h>
#include <object.h>
#include <toolkit/path.h>
#include <server_clock_fake.h>

START_TEST(test_registry_is_complete_and_stable_names_are_unique) {
    for (metric_id_t id = 0; id < METRIC_COUNT; id++) {
        const metric_metadata_t *metadata = metrics_metadata(id);
        ck_assert_ptr_ne(metadata, NULL);
        ck_assert_ptr_ne(metadata->save_name, NULL);
        ck_assert_ptr_ne(metadata->category, NULL);
        ck_assert_ptr_ne(metadata->name, NULL);
        ck_assert_ptr_ne(metadata->description, NULL);
        for (metric_id_t other = id + 1; other < METRIC_COUNT; other++) {
            const metric_metadata_t *other_metadata = metrics_metadata(other);
            if (metadata->scope == other_metadata->scope) {
                ck_assert_str_ne(metadata->save_name, other_metadata->save_name);
            }
        }
    }

    for (metric_collection_id_t id = 0; id < METRIC_COLLECTION_COUNT; id++) {
        const metric_collection_metadata_t *metadata = metrics_collection_metadata(id);
        ck_assert_ptr_ne(metadata, NULL);
        ck_assert_uint_gt(metadata->limit, 0);
        for (metric_collection_id_t other = id + 1; other < METRIC_COLLECTION_COUNT; other++) {
            const metric_collection_metadata_t *other_metadata = metrics_collection_metadata(other);
            if (metadata->scope == other_metadata->scope) {
                ck_assert_str_ne(metadata->save_name, other_metadata->save_name);
            }
        }
    }

    for (metric_keyed_id_t id = 0; id < METRIC_KEYED_COUNT; id++) {
        const metric_keyed_metadata_t *metadata = metrics_keyed_metadata(id);
        ck_assert_ptr_ne(metadata, NULL);
        ck_assert_ptr_ne(metadata->save_name, NULL);
        ck_assert_uint_gt(metadata->limit, 0);
        for (metric_keyed_id_t other = id + 1; other < METRIC_KEYED_COUNT; other++) {
            const metric_keyed_metadata_t *other_metadata = metrics_keyed_metadata(other);
            if (metadata->scope == other_metadata->scope) {
                ck_assert_str_ne(metadata->save_name, other_metadata->save_name);
            }
        }
    }
}
END_TEST

START_TEST(test_content_ids_are_domain_qualified_and_validated) {
    char id[METRICS_UNIQUE_ID_MAX + 1];
    ck_assert(metrics_format_content_id(VS(id), "map", "/shattered_islands/world_0303"));
    ck_assert_str_eq(id, "map:/shattered_islands/world_0303");
    ck_assert(metrics_format_content_id(VS(id), "quest-part", "lost_memories::find_book"));
    ck_assert_str_eq(id, "quest-part:lost_memories::find_book");
    ck_assert(!metrics_format_content_id(VS(id), "", "lost_memories"));
    ck_assert(!metrics_format_content_id(VS(id), "quest", ""));
    ck_assert(!metrics_format_content_id(VS(id), "bad domain", "lost_memories"));
    ck_assert(!metrics_format_content_id(VS(id), "bad:domain", "lost_memories"));
    ck_assert(!metrics_format_content_id(VS(id), "Quest", "lost_memories"));
    ck_assert(!metrics_format_content_id(VS(id), "quest", "bad key"));

    char short_id[8];
    ck_assert(!metrics_format_content_id(VS(short_id), "quest", "lost_memories"));
}
END_TEST

START_TEST(test_scalar_operations_scope_kinds_and_saturation) {
    metric_store_t character, account;
    metrics_store_init(&character, METRIC_SCOPE_CHARACTER, 10);
    metrics_store_init(&account, METRIC_SCOPE_ACCOUNT, 20);

    ck_assert(metrics_add(&character, METRIC_CHARACTER_DEATHS, UINT64_MAX - 1));
    ck_assert(metrics_add(&character, METRIC_CHARACTER_DEATHS, 10));
    ck_assert_uint_eq(metrics_get(&character, METRIC_CHARACTER_DEATHS), UINT64_MAX);
    ck_assert(!metrics_add(&character, METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS, 1));
    ck_assert(!metrics_add(&character, METRIC_CHARACTER_HIGHEST_LEVEL, 1));

    ck_assert(metrics_set(&character, METRIC_CHARACTER_CURRENT_LEVEL, 7));
    ck_assert_uint_eq(metrics_get(&character, METRIC_CHARACTER_CURRENT_LEVEL), 7);
    ck_assert(metrics_update_max(&character, METRIC_CHARACTER_HIGHEST_LEVEL, 5));
    ck_assert(metrics_update_max(&character, METRIC_CHARACTER_HIGHEST_LEVEL, 3));
    ck_assert_uint_eq(metrics_get(&character, METRIC_CHARACTER_HIGHEST_LEVEL), 5);
    ck_assert(!metrics_update_max(&character, METRIC_CHARACTER_DEATHS, 50));
    ck_assert(metrics_update_min(&character, METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION, 120));
    ck_assert(metrics_update_min(&character, METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION, 90));
    ck_assert(metrics_update_min(&character, METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION, 100));
    ck_assert_uint_eq(metrics_get(&character, METRIC_CHARACTER_SHORTEST_NONTRIVIAL_SESSION), 90);
    ck_assert(!metrics_update_min(&character, METRIC_CHARACTER_DEATHS, 1));

    ck_assert(metrics_add(&account, METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS, 1));
    ck_assert_uint_eq(metrics_get(&account, METRIC_ACCOUNT_SUCCESSFUL_AUTHENTICATIONS), 1);
    ck_assert_uint_eq(metrics_get(&account, METRIC_CHARACTER_DEATHS), 0);

    metrics_store_free(&character);
    metrics_store_free(&account);
}
END_TEST

START_TEST(test_unique_collections_validate_deduplicate_and_bound) {
    metric_store_t store;
    metrics_store_init(&store, METRIC_SCOPE_CHARACTER, 10);

    ck_assert(metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "world/a"));
    ck_assert(metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "world/a"));
    ck_assert_uint_eq(metrics_unique_count(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED), 1);
    ck_assert(!metrics_mark_unique(&store, METRIC_COLLECTION_ACCOUNT_MAPS_VISITED, "/world/b"));
    ck_assert(!metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "bad id"));
    ck_assert(!metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, ""));

    for (size_t i = 1; i < METRICS_UNIQUE_DEFAULT_LIMIT; i++) {
        char id[32];
        snprintf(VS(id), "map/%zu", i);
        ck_assert(metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, id));
    }
    ck_assert_uint_eq(metrics_unique_count(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED),
                      METRICS_UNIQUE_DEFAULT_LIMIT);
    ck_assert(
        !metrics_mark_unique(&store, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "map/overflow"));
    metrics_store_free(&store);
}
END_TEST

START_TEST(test_keyed_metrics_validate_kinds_sort_bound_and_saturate) {
    metric_store_t store;
    metrics_store_init(&store, METRIC_SCOPE_CHARACTER, 10);

    ck_assert(metrics_keyed_add(&store,
                                METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS,
                                "quest/z",
                                UINT64_MAX - 1));
    ck_assert(metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest/z", 10));
    ck_assert_uint_eq(
        metrics_keyed_get(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest/z"),
        UINT64_MAX);
    ck_assert(metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest/a", 1));
    ck_assert_str_eq(store.keyed[METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS].entries[0].id,
                     "quest/a");
    ck_assert(!metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL, "skill_x", 1));
    ck_assert(!metrics_keyed_set(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest/a", 2));
    ck_assert(metrics_keyed_set(&store, METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL, "skill_x", 3));
    ck_assert(
        metrics_keyed_update_max(&store, METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL, "skill_x", 5));
    ck_assert(
        metrics_keyed_update_max(&store, METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL, "skill_x", 4));
    ck_assert_uint_eq(
        metrics_keyed_get(&store, METRIC_KEYED_CHARACTER_SKILL_HIGHEST_LEVEL, "skill_x"),
        5);
    ck_assert(
        !metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "invalid subject", 1));
    ck_assert(metrics_keyed_set(&store, METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL, "skill_x", 0));
    ck_assert_uint_eq(metrics_keyed_count(&store, METRIC_KEYED_CHARACTER_SKILL_CURRENT_LEVEL), 0);

    for (size_t i = 2; i < METRICS_KEYED_DEFAULT_LIMIT; i++) {
        char id[32];
        snprintf(VS(id), "quest/%zu", i);
        ck_assert(metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, id, 1));
    }
    ck_assert_uint_eq(metrics_keyed_count(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS),
                      METRICS_KEYED_DEFAULT_LIMIT);
    ck_assert(
        !metrics_keyed_add(&store, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest/overflow", 1));

    metrics_store_free(&store);
}
END_TEST

START_TEST(test_content_by_name_updates_are_best_effort_at_capacity) {
    player pl = {0};
    metrics_store_init(&pl.metrics, METRIC_SCOPE_CHARACTER, 10);

    for (size_t i = 0; i < METRICS_UNIQUE_DEFAULT_LIMIT; i++) {
        char id[32];
        snprintf(VS(id), "region:test_%zu", i);
        ck_assert(
            metrics_mark_unique(&pl.metrics, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, id));
    }
    ck_assert(
        metrics_character_mark_unique_by_name(&pl, "exploration.regions", "region:capacity_noop"));
    ck_assert(
        !metrics_character_mark_unique_by_name(&pl, "exploration.regions", "invalid subject"));
    ck_assert(
        !metrics_character_mark_unique_by_name(&pl, "unknown.collection", "region:capacity_noop"));

    for (size_t i = 0; i < METRICS_KEYED_DEFAULT_LIMIT; i++) {
        char id[32];
        snprintf(VS(id), "quest:test_%zu", i);
        ck_assert(metrics_keyed_add(&pl.metrics, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, id, 1));
    }
    ck_assert(metrics_character_keyed_add_by_name(&pl,
                                                  "quests.completions_by_id",
                                                  "quest:capacity_noop",
                                                  1));
    ck_assert(!metrics_character_keyed_add_by_name(&pl,
                                                   "quests.completions_by_id",
                                                   "invalid subject",
                                                   1));
    ck_assert(
        !metrics_character_keyed_add_by_name(&pl, "unknown.series", "quest:capacity_noop", 1));
    ck_assert(!metrics_character_keyed_add_by_name(&pl,
                                                   "progression.skill_current_level",
                                                   "skill:literacy",
                                                   1));

    metrics_store_free(&pl.metrics);
}
END_TEST

static void write_text(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_int_ne(fputs(contents, fp), EOF);
    ck_assert_int_eq(fclose(fp), 0);
}

START_TEST(test_serialization_round_trip_unknown_ids_and_malformed_preservation) {
    char directory[] = "/tmp/atrinik-metrics-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    char path[MAX_BUF];
    snprintf(VS(path), "%s/metrics.dat", directory);

    metric_store_t original, loaded;
    metrics_store_init(&original, METRIC_SCOPE_CHARACTER, 1234);
    metrics_store_init(&loaded, METRIC_SCOPE_CHARACTER, 99);
    metrics_add(&original, METRIC_CHARACTER_DEATHS, UINT64_MAX);
    metrics_set(&original, METRIC_CHARACTER_LAST_PLAYED_AT, 5678);
    metrics_mark_unique(&original, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "nawerhals");
    metrics_keyed_add(&original, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "lost_memories", 3);
    ck_assert(metrics_save_file(&original, path));
    ck_assert(!original.dirty);
    struct stat statbuf;
    ck_assert_int_eq(stat(path, &statbuf), 0);
    ck_assert_uint_eq(statbuf.st_mode & 0777, 0600);
    ck_assert(metrics_load_file(&loaded, path));
    ck_assert_uint_eq(loaded.epoch, 1234);
    ck_assert_uint_eq(metrics_get(&loaded, METRIC_CHARACTER_DEATHS), UINT64_MAX);
    ck_assert_uint_eq(metrics_get(&loaded, METRIC_CHARACTER_LAST_PLAYED_AT), 5678);
    ck_assert(
        metrics_has_unique(&loaded, METRIC_COLLECTION_CHARACTER_REGIONS_VISITED, "nawerhals"));
    ck_assert_uint_eq(
        metrics_keyed_get(&loaded, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "lost_memories"),
        3);

    write_text(path,
               "metrics_version 1\nmetrics_epoch 1\nmetric future.unknown 12\n"
               "unique future.collection stable-id\nkeyed future.series stable-id 9\n"
               "metric combat.deaths 7\n");
    ck_assert(metrics_load_file(&loaded, path));
    ck_assert_uint_eq(metrics_get(&loaded, METRIC_CHARACTER_DEATHS), 7);
    ck_assert_uint_eq(loaded.opaque_lines_count, 3);
    ck_assert(metrics_save_file(&loaded, path));

    metric_store_t reloaded;
    metrics_store_init(&reloaded, METRIC_SCOPE_CHARACTER, 0);
    ck_assert(metrics_load_file(&reloaded, path));
    ck_assert_uint_eq(reloaded.opaque_lines_count, 3);
    metrics_store_free(&reloaded);

    write_text(path, "metrics_version 1\nmetric combat.deaths nope\n");
    ck_assert(!metrics_load_file(&loaded, path));
    ck_assert_uint_eq(metrics_get(&loaded, METRIC_CHARACTER_DEATHS), 7);

    ck_assert_int_eq(unlink(path), 0);
    ck_assert_int_eq(rmdir(directory), 0);
    metrics_store_free(&original);
    metrics_store_free(&loaded);
}
END_TEST

START_TEST(test_metrics_command_requires_explicit_permission) {
    player pl = {0};
    char saved_default_groups[MAX_BUF];
    memcpy(saved_default_groups, settings.default_permission_groups, sizeof(saved_default_groups));
    settings.default_permission_groups[0] = '\0';
    ck_assert(!commands_check_permission(&pl, "metrics"));
    pl.cmd_permissions = xcalloc(1, sizeof(*pl.cmd_permissions));
    pl.cmd_permissions[0] = xstrdup("metrics");
    pl.num_cmd_permissions = 1;
    ck_assert(commands_check_permission(&pl, "/metrics"));
    free(pl.cmd_permissions[0]);
    free(pl.cmd_permissions);
    memcpy(settings.default_permission_groups,
           saved_default_groups,
           sizeof(settings.default_permission_groups));
}
END_TEST

START_TEST(test_quest_outcomes_are_distinct_and_repeats_are_counted) {
    player pl = {0};
    metrics_store_init(&pl.metrics, METRIC_SCOPE_CHARACTER, 1);
    ck_assert(metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_STARTED));
    ck_assert(metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_FAILED));
    ck_assert(metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_STARTED));
    ck_assert(metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_COMPLETED));
    ck_assert(metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_COMPLETED));
    ck_assert(!metrics_character_quest_status(&pl, "quest.one", QUEST_STATUS_INVALID));
    ck_assert(!metrics_character_quest_status(&pl, "invalid quest", QUEST_STATUS_STARTED));
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_QUESTS_STARTED), 2);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_QUESTS_FAILED), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_QUESTS_COMPLETED), 2);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_QUESTS_CURRENT_ACTIVE), 0);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_QUESTS_HIGHEST_ACTIVE), 1);
    ck_assert_uint_eq(
        metrics_keyed_get(&pl.metrics, METRIC_KEYED_CHARACTER_QUEST_STARTS, "quest:quest.one"),
        2);
    ck_assert_uint_eq(
        metrics_keyed_get(&pl.metrics, METRIC_KEYED_CHARACTER_QUEST_COMPLETIONS, "quest:quest.one"),
        2);
    ck_assert_uint_eq(
        metrics_keyed_get(&pl.metrics, METRIC_KEYED_CHARACTER_QUEST_FAILURES, "quest:quest.one"),
        1);
    ck_assert_uint_eq(
        metrics_unique_count(&pl.metrics, METRIC_COLLECTION_CHARACTER_QUESTS_COMPLETED),
        1);
    metrics_store_free(&pl.metrics);
}
END_TEST

START_TEST(test_death_causes_are_exclusive_and_reset_survival_time) {
    player pl = {0};
    metrics_store_init(&pl.metrics, METRIC_SCOPE_CHARACTER, 1);
    metrics_set(&pl.metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH, 42);

    metrics_character_death(&pl, false, false);
    metrics_character_death(&pl, true, false);
    metrics_character_death(&pl, false, true);

    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_DEATHS), 3);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_PVE_DEATHS), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_PVP_DEATHS), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_ENVIRONMENTAL_DEATHS), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH), 0);
    metrics_store_free(&pl.metrics);
}
END_TEST

START_TEST(test_session_timing_uses_monotonic_clock_and_afk_transitions) {
    player pl = {0};
    object ob = {0};
    pl.ob = &ob;
    ob.level = 4;
    metrics_store_init(&pl.metrics, METRIC_SCOPE_CHARACTER, 1);

    server_clock_fake_install(UINT64_C(100000),
                              (server_tick_t){0},
                              (server_monotonic_t){UINT64_C(1000000)},
                              (server_wall_utc_t){100});
    metrics_character_session_start(&pl);
    server_clock_fake_advance_monotonic((server_duration_t){UINT64_C(2500000)});
    server_clock_fake_set_wall((server_wall_utc_t){INT64_MAX});
    metrics_character_afk_changed(&pl, true);
    server_clock_fake_advance_monotonic((server_duration_t){UINT64_C(3750000)});
    metrics_character_afk_changed(&pl, false);
    server_clock_fake_advance_monotonic((server_duration_t){UINT64_C(750000)});
    metrics_character_session_end(&pl);

    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_SESSIONS_STARTED), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_SESSIONS_COMPLETED), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_ACTIVE_PLAY_TIME), 3);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_AFK_TIME), 3);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_SESSION_DURATION), 7);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_LONGEST_SESSION), 7);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_LONGEST_ACTIVE_SESSION), 3);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_AFK_ENTRIES), 1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_ACTIVE_TIME_SINCE_DEATH), 3);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_LONGEST_ACTIVE_TIME_WITHOUT_DEATH),
                      3);
    ck_assert_uint_eq(metrics_unique_count(&pl.metrics, METRIC_COLLECTION_CHARACTER_ACTIVE_DAYS),
                      1);
    ck_assert_uint_eq(metrics_get(&pl.metrics, METRIC_CHARACTER_LAST_PLAYED_AT), 100);

    server_clock_fake_uninstall();
    metrics_store_free(&pl.metrics);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("metrics");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_test(tc_core, test_registry_is_complete_and_stable_names_are_unique);
    tcase_add_test(tc_core, test_content_ids_are_domain_qualified_and_validated);
    tcase_add_test(tc_core, test_scalar_operations_scope_kinds_and_saturation);
    tcase_add_test(tc_core, test_unique_collections_validate_deduplicate_and_bound);
    tcase_add_test(tc_core, test_keyed_metrics_validate_kinds_sort_bound_and_saturate);
    tcase_add_test(tc_core, test_content_by_name_updates_are_best_effort_at_capacity);
    tcase_add_test(tc_core, test_serialization_round_trip_unknown_ids_and_malformed_preservation);
    tcase_add_test(tc_core, test_metrics_command_requires_explicit_permission);
    tcase_add_test(tc_core, test_quest_outcomes_are_distinct_and_repeats_are_counted);
    tcase_add_test(tc_core, test_death_causes_are_exclusive_and_reset_survival_time);
    tcase_add_test(tc_core, test_session_timing_uses_monotonic_clock_and_afk_transitions);
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_metrics(void) {
    check_run_suite(suite(), __FILE__);
}
