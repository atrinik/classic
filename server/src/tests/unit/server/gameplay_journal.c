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
#include <shop.h>
#include <player.h>
#include <object.h>
#include <metrics.h>
#include <arch.h>
#include <party.h>
#include <quest.h>
#include <spells.h>
#include <loader.h>
#include <initialization.h>

#ifndef WIN32
#include <sys/wait.h>
#endif

static bool crash_intent_field(const char *contents,
                               const char *reason,
                               const char *field,
                               char *value,
                               size_t value_size);

static size_t count_substring(const char *contents, const char *needle) {
    size_t count = 0;
    for (const char *found = contents; (found = strstr(found, needle)) != NULL;
         found += strlen(needle)) {
        count++;
    }
    return count;
}

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

static char *read_fixture(const char *directory) {
    char journal_directory[MAX_BUF];
    snprintf(VS(journal_directory), "%s/gameplay-journal", directory);
    DIR *dir = opendir(journal_directory);
    ck_assert_ptr_ne(dir, NULL);
    char path[HUGE_BUF] = "";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "journal-", 8) == 0) {
            snprintf(VS(path), "%s/%s", journal_directory, entry->d_name);
            break;
        }
    }
    ck_assert_int_eq(closedir(dir), 0);
    ck_assert_str_ne(path, "");
    struct stat metadata;
    ck_assert_int_eq(stat(path, &metadata), 0);
    char *contents = malloc((size_t)metadata.st_size + 1);
    ck_assert_ptr_ne(contents, NULL);
    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    size_t length = fread(contents, 1, (size_t)metadata.st_size, fp);
    ck_assert_uint_eq(length, (size_t)metadata.st_size);
    ck_assert_int_eq(fclose(fp), 0);
    contents[length] = '\0';
    return contents;
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
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_CURRENCY,
                                      "currency.incomplete",
                                      &change,
                                      committed));
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_ITEM,
                                      "item.incomplete",
                                      &change,
                                      committed));
    gameplay_journal_change_t item_change = {
        .subject_id = "item:lineage",
        .lineage_id = "item:lineage",
        .before = 0,
        .delta = 1,
        .after = 1,
        .archetype = "sword",
        .object_type = 15,
        .snapshot = "arch=sword;type=015;nrof=1;value=0;weight=0",
        .quantity = 1,
        .source = "service",
        .destination = "player",
        .actor = "account:actor",
        .counterparty = "",
        .provenance_before = "first=;last=",
        .provenance_after = "first=account:actor;last=",
        .price = 0,
        .currency = "",
        .funding = "",
    };
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_ITEM,
                                      "item.noncanonical-snapshot",
                                      &item_change,
                                      committed));
    item_change.snapshot = "arch=sword;type=15;nrof=1;value=0;weight=0";
    item_change.provenance_before = "password=secret";
    ck_assert(!gameplay_journal_begin(&subject,
                                      GAMEPLAY_JOURNAL_ITEM,
                                      "item.noncanonical-provenance",
                                      &item_change,
                                      committed));
    item_change.provenance_before = "first=;last=";
    ck_assert(gameplay_journal_begin(&subject,
                                     GAMEPLAY_JOURNAL_ITEM,
                                     "item.canonical",
                                     &item_change,
                                     committed));
    ck_assert(gameplay_journal_abort(committed, "test.abort"));
    ck_assert(gameplay_journal_begin(&subject,
                                     GAMEPLAY_JOURNAL_QUEST,
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

    gameplay_journal_subject_t apartment_subject = subject;
    apartment_subject.map_id = "$account/apartment";
    ck_assert(gameplay_journal_begin(&apartment_subject,
                                     GAMEPLAY_JOURNAL_QUEST,
                                     "quest.apartment",
                                     &change,
                                     aborted));
    ck_assert(gameplay_journal_abort(aborted, "quest.vetoed"));

    char rejected[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    change.after = 99;
    ck_assert(
        !gameplay_journal_begin(&subject, GAMEPLAY_JOURNAL_QUEST, "bad.change", &change, rejected));
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

static void semantic_failure_journal_init(char *directory) {
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_counts_reset_for_test();
    gameplay_journal_fail_after_writes_for_test(1);
}

static void semantic_failure_journal_deinit(char *directory) {
    gameplay_journal_fail_after_writes_for_test(SIZE_MAX);
    gameplay_journal_deinit();
    remove_fixture(directory);
}

START_TEST(test_item_terminal_failures_report_ambiguity) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *inserted = NULL;

    char grant_directory[] = "/tmp/atrinik-item-grant-failure-XXXXXX";
    semantic_failure_journal_init(grant_directory);
    object *grant = arch_get("sword");
    ck_assert_int_eq(object_insert_into_reason(grant, pl, "test.grant-failure", &inserted),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert_ptr_ne(inserted, NULL);
    ck_assert_ptr_eq(object_get_env(inserted), pl);
    ck_assert_ptr_ne(inserted->custody_lineage, NULL);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.grant-failure"), 0);
    semantic_failure_journal_deinit(grant_directory);

    char merge_directory[] = "/tmp/atrinik-item-merge-failure-XXXXXX";
    semantic_failure_journal_init(merge_directory);
    const char *actor = object_custody_actor_id(pl);
    ck_assert_ptr_ne(actor, NULL);
    object *existing = arch_get("bolt");
    existing->nrof = 4;
    existing->custody_lineage = add_string("item:test-merge");
    existing->custody_first = add_string(actor);
    existing = object_insert_into(existing, pl, 0);
    object *incoming = arch_get("bolt");
    incoming->nrof = 2;
    incoming->custody_lineage = add_string("item:test-merge");
    incoming->custody_first = add_string(actor);
    ck_assert_int_eq(object_insert_into_reason(incoming, pl, "test.merge-failure", &inserted),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert_ptr_eq(inserted, existing);
    ck_assert_uint_eq(existing->nrof, 6);
    semantic_failure_journal_deinit(merge_directory);

    char destroy_directory[] = "/tmp/atrinik-item-destroy-failure-XXXXXX";
    semantic_failure_journal_init(destroy_directory);
    object *destroyed = arch_get("sword");
    destroyed->custody_lineage = add_string("item:test-destroy");
    destroyed->custody_first = add_string(actor);
    destroyed = object_insert_into(destroyed, pl, 0);
    uint32_t destroyed_tag = destroyed->count;
    ck_assert_int_eq(object_remove_reason(destroyed, "test.destroy-failure", true),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert(!OBJECT_VALID(destroyed, destroyed_tag));
    semantic_failure_journal_deinit(destroy_directory);

    char decrease_directory[] = "/tmp/atrinik-item-decrease-failure-XXXXXX";
    semantic_failure_journal_init(decrease_directory);
    object *stack = arch_get("bolt");
    stack->nrof = 5;
    stack->custody_lineage = add_string("item:test-decrease");
    stack->custody_first = add_string(actor);
    stack = object_insert_into(stack, pl, 0);
    object *survivor = NULL;
    ck_assert_int_eq(object_decrease_reason(stack, 2, "test.decrease-failure", &survivor),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert_ptr_eq(survivor, stack);
    ck_assert_uint_eq(stack->nrof, 3);
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    semantic_failure_journal_deinit(decrease_directory);

    char currency_directory[] = "/tmp/atrinik-currency-destroy-failure-XXXXXX";
    semantic_failure_journal_init(currency_directory);
    object *coin = arch_get("coppercoin");
    coin->nrof = 7;
    coin = object_insert_into(coin, pl, 0);
    uint32_t coin_tag = coin->count;
    ck_assert_int_eq(shop_destroy_coin_reason(coin, "test.currency-destroy-failure"),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert(!OBJECT_VALID(coin, coin_tag));
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    semantic_failure_journal_deinit(currency_directory);

    object *bank = arch_get("player_info");
    FREE_AND_COPY_HASH(bank->name, "BANK_GENERAL");
    bank->value = 9;
    bank = object_insert_into(bank, pl, 0);
    char bank_directory[] = "/tmp/atrinik-bank-destroy-failure-XXXXXX";
    semantic_failure_journal_init(bank_directory);
    uint32_t bank_tag = bank->count;
    ck_assert_int_eq(bank_destroy_balance_reason(bank, "test.bank-destroy-failure"),
                     OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert(!OBJECT_VALID(bank, bank_tag));
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    semantic_failure_journal_deinit(bank_directory);

    object_destroy(pl);
}
END_TEST

START_TEST(test_shop_purchase_terminal_failure_keeps_authoritative_state) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *money = object_insert_into(arch_get("coppercoin"), pl, 0);
    money->nrof = 100;
    object *item = arch_get("sword");
    SET_FLAG(item, FLAG_IDENTIFIED);
    SET_FLAG(item, FLAG_UNPAID);
    item->value = 75;
    item = object_insert_into(item, pl, INS_NO_MERGE);
    int64_t before = shop_get_money(pl);
    uint64_t purchases = metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_SHOP_PURCHASES);

    char directory[] = "/tmp/atrinik-shop-purchase-failure-XXXXXX";
    semantic_failure_journal_init(directory);
    ck_assert(!shop_pay_items(pl));
    ck_assert(!QUERY_FLAG(item, FLAG_UNPAID));
    ck_assert_int_eq(shop_get_money(pl), before - 75);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("shop.purchase"), 0);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_SHOP_PURCHASES), purchases);
    semantic_failure_journal_deinit(directory);

    char payment_directory[] = "/tmp/atrinik-script-payment-failure-XXXXXX";
    semantic_failure_journal_init(payment_directory);
    before = shop_get_money(pl);
    ck_assert_int_eq(shop_pay_reason(pl, 1, "test.payment-failure"), OBJECT_SEMANTIC_AMBIGUOUS);
    ck_assert_int_eq(shop_get_money(pl), before - 1);
    semantic_failure_journal_deinit(payment_directory);
    object_destroy(pl);
}
END_TEST

START_TEST(test_party_loot_preparation_failure_preserves_corpse) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    form_party(pl, "journal-test-party");
    ck_assert_ptr_ne(CONTR(pl)->party, NULL);
    CONTR(pl)->party->loot = PARTY_LOOT_SPLIT;
    object *corpse = arch_get("corpse");
    object *coins = object_insert_into(arch_get("coppercoin"), corpse, 0);
    coins->nrof = 10;
    object *item = object_insert_into(arch_get("sword"), corpse, 0);

    char directory[] = "/tmp/atrinik-party-loot-failure-XXXXXX";
    semantic_failure_journal_init(directory);
    gameplay_journal_fail_writes_for_test(true);
    party_handle_corpse(pl, corpse);
    ck_assert_ptr_eq(object_get_env(item), corpse);
    ck_assert_ptr_eq(object_get_env(coins), corpse);
    gameplay_journal_fail_writes_for_test(false);
    semantic_failure_journal_deinit(directory);

    remove_party_member(CONTR(pl)->party, pl);
    object_destroy(corpse);
    object_destroy(pl);
}
END_TEST

START_TEST(test_party_random_currency_retirement_preserves_message_lifetime) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    form_party(pl, "journal-random-loot-party");
    ck_assert_ptr_ne(CONTR(pl)->party, NULL);
    CONTR(pl)->party->loot = PARTY_LOOT_RANDOM;

    object *existing = object_insert_into(arch_get("coppercoin"), pl, 0);
    existing->nrof = 2;
    object *corpse = arch_get("corpse");
    object *coins = object_insert_into(arch_get("coppercoin"), corpse, 0);
    coins->nrof = 10;
    object *invalid_coins = object_insert_into(arch_get("coppercoin"), corpse, INS_NO_MERGE);
    invalid_coins->value = 2;

    party_handle_corpse(pl, corpse);
    ck_assert_ptr_eq(object_get_env(invalid_coins), corpse);
    ck_assert_int_eq(shop_get_money(pl), 12);
    ck_assert_ptr_eq(object_find_type(pl, MONEY)->custody_lineage, NULL);
    object_remove(invalid_coins, 0);
    object_destroy(invalid_coins);
    ck_assert_ptr_eq(corpse->inv, NULL);

    coins = object_insert_into(arch_get("coppercoin"), corpse, 0);
    coins->nrof = 10;
    corpse->map = map;
    corpse->x = pl->x;
    corpse->y = pl->y;
    object_insert_into(arch_get("sword"), corpse, INS_NO_MERGE);

    char directory[] = "/tmp/atrinik-party-random-loot-XXXXXX";
    const char *old_map_path = map->path;
    map->path = add_string("/test/party apartment\\owner");
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    party_handle_corpse(pl, corpse);
    ck_assert_ptr_eq(corpse->inv, NULL);
    ck_assert_int_eq(shop_get_money(pl), 22);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("party.currency-loot"), 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.party-loot"), 1);
    gameplay_journal_deinit();
    char *random_contents = read_fixture(directory);
    ck_assert_ptr_ne(strstr(random_contents, "/test/party%20apartment%5Cowner"), NULL);
    free(random_contents);
    remove_fixture(directory);

    CONTR(pl)->party->loot = PARTY_LOOT_SPLIT;
    object *other = player_get_dummy("Journal split recipient", NULL);
    other->map = map;
    other->x = pl->x;
    other->y = pl->y;
    add_party_member(CONTR(pl)->party, other);
    pl->carrying = weight_limit[MIN(pl->stats.Str, MAX_STAT)];
    other->carrying = weight_limit[MIN(other->stats.Str, MAX_STAT)];
    coins = object_insert_into(arch_get("coppercoin"), corpse, 0);
    coins->nrof = 10;
    char split_directory[] = "/tmp/atrinik-party-split-loot-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(split_directory), NULL);
    ck_assert(gameplay_journal_init(split_directory, "server", &profile));
    party_handle_corpse(pl, corpse);
    ck_assert_int_eq(shop_get_money(pl), 22);
    int64_t recovery_total;
    ck_assert(shop_get_recovery_money(pl, &recovery_total));
    ck_assert_int_eq(recovery_total, 32);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("party.currency-split"), 2);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("party.currency-source"), 1);
    gameplay_journal_deinit();
    char *split_contents = read_fixture(split_directory);
    ck_assert_ptr_ne(strstr(split_contents, "\"before\":0,\"delta\":5,\"after\":5"), NULL);
    ck_assert_ptr_ne(strstr(split_contents, "\"before\":27,\"delta\":5,\"after\":32"), NULL);
    ck_assert_ptr_ne(strstr(split_contents, "\"before\":10,\"delta\":-10,\"after\":0"), NULL);
    char source_transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(crash_intent_field(split_contents,
                                 "party.currency-source",
                                 "transaction_id",
                                 VS(source_transaction)));
    char funding[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE + 32];
    snprintf(VS(funding), "\"funding\":\"%s\"", source_transaction);
    ck_assert_uint_eq(count_substring(split_contents, funding), 2);
    char source_terminal[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE + 64];
    snprintf(VS(source_terminal), "\"transaction_id\":\"%s\",\"sequence\"", source_transaction);
    ck_assert_uint_ge(count_substring(split_contents, source_terminal), 2);
    char grant_transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(crash_intent_field(split_contents,
                                 "party.currency-split",
                                 "transaction_id",
                                 VS(grant_transaction)));
    char source_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE + 32];
    char grant_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE + 32];
    snprintf(VS(source_id), "\"transaction_id\":\"%s\"", source_transaction);
    snprintf(VS(grant_id), "\"transaction_id\":\"%s\"", grant_transaction);
    const char *source_commit_pos = strstr(split_contents, source_id);
    while (source_commit_pos != NULL) {
        const char *line_end = strchr(source_commit_pos, '\n');
        const char *phase = strstr(source_commit_pos, "\"phase\":\"commit\"");
        if (phase != NULL && (line_end == NULL || phase < line_end)) {
            break;
        }
        source_commit_pos = strstr(source_commit_pos + 1, source_id);
    }
    const char *grant_commit_pos = strstr(split_contents, grant_id);
    while (grant_commit_pos != NULL) {
        const char *line_end = strchr(grant_commit_pos, '\n');
        const char *phase = strstr(grant_commit_pos, "\"phase\":\"commit\"");
        if (phase != NULL && (line_end == NULL || phase < line_end)) {
            break;
        }
        grant_commit_pos = strstr(grant_commit_pos + 1, grant_id);
    }
    ck_assert_ptr_ne(source_commit_pos, NULL);
    ck_assert_ptr_ne(grant_commit_pos, NULL);
    ck_assert_msg(source_commit_pos < grant_commit_pos,
                  "party source terminal must precede recipient terminals");
    ck_assert_ptr_ne(strstr(split_contents, "\"kind\":\"map-runtime\""), NULL);
    free(split_contents);
    remove_fixture(split_directory);

    remove_party_member(CONTR(pl)->party, other);
    object_destroy(other);
    remove_party_member(CONTR(pl)->party, pl);
    corpse->map = NULL;
    object_destroy(corpse);
    free_string_shared(map->path);
    map->path = old_map_path;
    object_destroy(pl);
}
END_TEST

START_TEST(test_currency_recovery_aggregate_includes_floor_delivery) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *held = object_insert_into(arch_get("coppercoin"), pl, 0);
    held->nrof = 100;
    object *floor = arch_get("coppercoin");
    floor->nrof = 7;
    floor->x = pl->x;
    floor->y = pl->y;
    object_insert_map(floor, map, NULL, INS_NO_MERGE);
    object *noncanonical = arch_get("coppercoin");
    noncanonical->value = 2;
    noncanonical->x = pl->x;
    noncanonical->y = pl->y;
    object_insert_map(noncanonical, map, NULL, INS_NO_MERGE);
    pl->carrying = weight_limit[MIN(pl->stats.Str, MAX_STAT)];

    int64_t before;
    ck_assert(shop_get_recovery_money(pl, &before));
    ck_assert_int_eq(before, 107);
    char directory[] = "/tmp/atrinik-currency-floor-recovery-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    ck_assert_int_eq(shop_insert_coins_reason(pl, 3, "test.floor-currency"),
                     OBJECT_SEMANTIC_COMMITTED);
    int64_t after;
    ck_assert(shop_get_recovery_money(pl, &after));
    ck_assert_int_eq(after, 110);
    ck_assert_int_eq(shop_get_money(pl), 100);
    gameplay_journal_deinit();
    char *contents = read_fixture(directory);
    ck_assert_ptr_ne(strstr(contents, "\"before\":107,\"delta\":3,\"after\":110"), NULL);
    free(contents);
    remove_fixture(directory);
    object_destroy(pl);
}
END_TEST

START_TEST(test_one_drop_quest_grant_commits_item_and_marker_together) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    ck_assert_ptr_ne(CONTR(pl)->quest_container, NULL);
    object *quest = arch_get(QUEST_CONTAINER_ARCHETYPE);
    quest->sub_type = QUEST_TYPE_ITEM_DROP;
    FREE_AND_COPY_HASH(quest->name, "journal-one-drop-quest");
    object *reward = object_insert_into(arch_get("sword"), quest, INS_NO_MERGE);
    SET_FLAG(reward, FLAG_ONE_DROP);

    char directory[] = "/tmp/atrinik-quest-one-drop-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    quest_handle(pl, quest);
    ck_assert_ptr_ne(object_find_arch(pl, arch_find("sword")), NULL);
    bool marker = false;
    FOR_INV_PREPARE(CONTR(pl)->quest_container, tmp) {
        if (tmp->name != NULL && strcmp(tmp->name, "journal-one-drop-quest") == 0 &&
            tmp->magic == QUEST_STATUS_COMPLETED) {
            marker = true;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert(marker);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("quest.item-grant"), 1);
    gameplay_journal_deinit();
    object_destroy(quest);
    object_destroy(pl);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_pending_domains_block_checkpoints_and_unique_maps_use_primary_component) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    map->map_flags |= MAP_FLAG_UNIQUE;
    const char *old_map_path = map->path;
    map->path = add_string("/test/apartment path\\owner");
    object *probe = arch_get("sword");
    SET_FLAG(probe, FLAG_UNIQUE);

    char directory[] = "/tmp/atrinik-journal-pending-domain-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.pending-domain",
                                              "currency:test",
                                              0,
                                              1,
                                              1,
                                              "service",
                                              "ground",
                                              "generated",
                                              transaction));
    ck_assert(!gameplay_journal_player_checkpoint_allowed(pl));
    ck_assert(gameplay_journal_map_checkpoint_allowed(map));
    ck_assert(!gameplay_journal_track_map_object(transaction, map, -100, -100, probe));
    ck_assert(gameplay_journal_track_map_object(transaction, map, pl->x, pl->y, probe));
    ck_assert(!gameplay_journal_map_checkpoint_allowed(map));
    ck_assert(gameplay_journal_commit(transaction));
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    ck_assert(gameplay_journal_map_checkpoint_allowed(map));
    ck_assert_uint_eq(map->journal_sequence, CONTR(pl)->journal_sequence);
    ck_assert_uint_eq(map->journal_unique_sequence, 0);
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.pending-attempted",
                                              "currency:test",
                                              1,
                                              1,
                                              2,
                                              "service",
                                              "ground",
                                              "generated",
                                              transaction));
    ck_assert(gameplay_journal_track_map_object(transaction, map, pl->x, pl->y, probe));
    ck_assert(!gameplay_journal_player_checkpoint_allowed(pl));
    ck_assert(!gameplay_journal_map_checkpoint_allowed(map));
    ck_assert(gameplay_journal_attempt(transaction));
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    ck_assert(gameplay_journal_map_checkpoint_allowed(map));

    char other[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.pending-failure-a",
                                              "currency:test",
                                              1,
                                              1,
                                              2,
                                              "service",
                                              "ground",
                                              "generated",
                                              transaction));
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.pending-failure-b",
                                              "currency:test",
                                              2,
                                              1,
                                              3,
                                              "service",
                                              "ground",
                                              "generated",
                                              other));
    ck_assert(gameplay_journal_track_map_object(transaction, map, pl->x, pl->y, probe));
    ck_assert(gameplay_journal_track_map_object(other, map, pl->x, pl->y, probe));
    gameplay_journal_fail_writes_for_test(true);
    ck_assert(!gameplay_journal_commit(transaction));
    ck_assert(gameplay_journal_player_checkpoint_allowed(pl));
    ck_assert(gameplay_journal_map_checkpoint_allowed(map));
    gameplay_journal_fail_writes_for_test(false);
    gameplay_journal_deinit();

    char *contents = read_fixture(directory);
    ck_assert_ptr_ne(strstr(contents, "\"phase\":\"domain\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"kind\":\"map-runtime\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "/test/apartment%20path%5Cowner"), NULL);
    ck_assert_ptr_eq(strstr(contents, "\"kind\":\"map-unique\""), NULL);
    free(contents);
    remove_fixture(directory);
    object_destroy(probe);
    free_string_shared(map->path);
    map->path = old_map_path;
    object_destroy(pl);
}
END_TEST

START_TEST(test_production_player_and_map_checkpoints_persist_component_watermarks) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    const char *old_map_path = map->path;
    map->path = add_string("/test/journal-checkpoint");
    char directory[] = "/tmp/atrinik-journal-production-checkpoint-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    char old_datapath[MAX_BUF];
    snprintf(VS(old_datapath), "%s", settings.datapath);
    snprintf(VS(settings.datapath), "%s", directory);

    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    object *runtime_probe = arch_get("sword");
    FREE_AND_COPY_HASH(runtime_probe->name, "journal runtime checkpoint probe");
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.runtime-checkpoint",
                                              "currency:test",
                                              0,
                                              1,
                                              1,
                                              "service",
                                              "ground",
                                              "generated",
                                              transaction));
    ck_assert(gameplay_journal_track_map_object(transaction, map, pl->x, pl->y, runtime_probe));
    ck_assert(gameplay_journal_commit(transaction));
    uint64_t runtime_sequence = map->journal_sequence;

    object *unique_probe = arch_get("sword");
    FREE_AND_COPY_HASH(unique_probe->name, "journal unique checkpoint probe");
    SET_FLAG(unique_probe, FLAG_UNIQUE);
    ck_assert(gameplay_journal_currency_begin(pl,
                                              "test.unique-checkpoint",
                                              "currency:test",
                                              1,
                                              1,
                                              2,
                                              "service",
                                              "ground",
                                              "generated",
                                              transaction));
    ck_assert(gameplay_journal_track_map_object(transaction, map, pl->x, pl->y, unique_probe));
    ck_assert(gameplay_journal_commit(transaction));
    uint64_t unique_sequence = map->journal_unique_sequence;
    ck_assert_uint_gt(unique_sequence, runtime_sequence);

    player_save(pl);
    char *player_path = player_make_path(pl->name, "player.dat");
    char *metrics_path = player_make_path(pl->name, "metrics.dat");
    FILE *fp = fopen(player_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    char line[MAX_BUF];
    uint64_t saved_player_sequence = 0;
    while (fgets(VS(line), fp) != NULL && strcmp(line, "endplst\n") != 0) {
        if (sscanf(line, "journal_sequence %" SCNu64, &saved_player_sequence) == 1) {
            break;
        }
    }
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_msg(saved_player_sequence == unique_sequence,
                  "saved player sequence=%" PRIu64 ", unique sequence=%" PRIu64,
                  saved_player_sequence,
                  unique_sequence);
    char runtime_path[HUGE_BUF];
    snprintf(VS(runtime_path), "%s/runtime.map", directory);
    char unique_directory[HUGE_BUF];
    snprintf(VS(unique_directory), "%s/unique-items", directory);
    if (mkdir(unique_directory, 0700) != 0) {
        ck_assert_int_eq(errno, EEXIST);
    }
    char *old_tmpname = map->tmpname;
    map->tmpname = xstrdup(runtime_path);
    runtime_probe->x = pl->x;
    runtime_probe->y = pl->y;
    runtime_probe = object_insert_map(runtime_probe, map, NULL, INS_NO_MERGE | INS_NO_WALK_ON);
    unique_probe->x = pl->x;
    unique_probe->y = pl->y;
    unique_probe = object_insert_map(unique_probe, map, NULL, INS_NO_MERGE | INS_NO_WALK_ON);
    ck_assert_int_eq(new_save_map(map, 0), 0);
    fp = fopen(runtime_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    mapstruct loaded_map = {0};
    ck_assert_int_eq(load_map_header(&loaded_map, fp), 1);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_uint_eq(loaded_map.journal_sequence, runtime_sequence);

    char unique_path[HUGE_BUF];
    snprintf(VS(unique_path), "%s/unique-items/", directory);
    size_t offset = strlen(unique_path);
    const char *map_path = map->path + 1;
    for (; *map_path != '\0' && offset + 5 < sizeof(unique_path); map_path++) {
        unique_path[offset++] = *map_path == '/' ? '@' : *map_path;
    }
    snprintf(unique_path + offset, sizeof(unique_path) - offset, ".v00");
    fp = fopen(unique_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    char marker[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    uint64_t saved_unique_sequence;
    ck_assert_int_eq(
        fscanf(fp, "# gameplay-journal %32[0-9a-f] %" SCNu64, marker, &saved_unique_sequence),
        2);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_uint_eq(saved_unique_sequence, unique_sequence);

    fp = fopen(runtime_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    bool found_runtime_probe = false;
    while (fgets(VS(line), fp) != NULL) {
        if (strstr(line, "journal runtime checkpoint probe") != NULL) {
            found_runtime_probe = true;
            break;
        }
    }
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert(found_runtime_probe);
    fp = fopen(unique_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    bool found_unique_probe = false;
    while (fgets(VS(line), fp) != NULL) {
        if (strstr(line, "journal unique checkpoint probe") != NULL) {
            found_unique_probe = true;
            break;
        }
    }
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert(found_unique_probe);

    gameplay_journal_deinit();
#ifndef WIN32
    char source_root[HUGE_BUF];
    snprintf(VS(source_root), "%s", ATRINIK_TEST_DATA_DIR);
    char *source_suffix = strstr(source_root, "/src/tests/data");
    ck_assert_ptr_ne(source_suffix, NULL);
    *source_suffix = '\0';
    char reconcile_path[HUGE_BUF];
    snprintf(VS(reconcile_path), "%s/reconcile.json", directory);
    StringBuffer *command_buffer = stringbuffer_new();
    stringbuffer_append_printf(command_buffer,
                               "python3 '%s/tools/gameplay_journal.py' "
                               "'%s/gameplay-journal' reconcile --player-save '%s/%s=%s' "
                               "--map-save '%s=%s' --map-save '%s=%s' > '%s'",
                               source_root,
                               directory,
                               CONTR(pl)->cs->account,
                               pl->name,
                               player_path,
                               map->path,
                               runtime_path,
                               map->path,
                               unique_path,
                               reconcile_path);
    char *command = stringbuffer_finish(command_buffer);
    ck_assert_int_eq(system(command), 0);
    free(command);
    fp = fopen(reconcile_path, "rb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_int_eq(fseek(fp, 0, SEEK_END), 0);
    long reconcile_size = ftell(fp);
    ck_assert_int_gt(reconcile_size, 0);
    ck_assert_int_eq(fseek(fp, 0, SEEK_SET), 0);
    char *reconcile = malloc((size_t)reconcile_size + 1);
    ck_assert_ptr_ne(reconcile, NULL);
    ck_assert_uint_eq(fread(reconcile, 1, (size_t)reconcile_size, fp), (size_t)reconcile_size);
    reconcile[reconcile_size] = '\0';
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_int_eq(reconcile[0], '[');
    ck_assert_uint_eq(count_substring(reconcile, "\"action\": \"checkpointed\""), 4);
    free(reconcile);
    ck_assert_int_eq(unlink(reconcile_path), 0);
#endif
    free(map->tmpname);
    map->tmpname = old_tmpname;
    snprintf(VS(settings.datapath), "%s", old_datapath);
    object_remove(runtime_probe, 0);
    object_destroy(runtime_probe);
    object_remove(unique_probe, 0);
    object_destroy(unique_probe);
    free_string_shared(map->path);
    map->path = old_map_path;
    object_destroy(pl);
    ck_assert_int_eq(unlink(runtime_path), 0);
    ck_assert_int_eq(unlink(unique_path), 0);
    ck_assert_int_eq(rmdir(unique_directory), 0);
    ck_assert_int_eq(unlink(metrics_path), 0);
    free(metrics_path);
    ck_assert_int_eq(unlink(player_path), 0);
    char *slash = strrchr(player_path, '/');
    ck_assert_ptr_ne(slash, NULL);
    *slash = '\0';
    for (int i = 0; i < 5; i++) {
        ck_assert_int_eq(rmdir(player_path), 0);
        slash = strrchr(player_path, '/');
        ck_assert_ptr_ne(slash, NULL);
        *slash = '\0';
    }
    free(player_path);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_checkpoint_watermarks_persist_player_and_map) {
    const char *run_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    snprintf(VS(CONTR(pl)->journal_run_id), "%s", run_id);
    CONTR(pl)->journal_sequence = 42;

    FILE *fp = tmpfile();
    ck_assert_ptr_ne(fp, NULL);
    fprintf(fp, "journal_run %s\n", CONTR(pl)->journal_run_id);
    fprintf(fp, "journal_sequence %" PRIu64 "\n", CONTR(pl)->journal_sequence);
    fprintf(fp, "endplst\n");
    object_save(pl, fp);
    rewind(fp);
    object *loaded_player = player_get_dummy("Journal watermark reload", NULL);
    ck_assert(player_load_stream(CONTR(loaded_player), fp));
    ck_assert_str_eq(CONTR(loaded_player)->journal_run_id, run_id);
    ck_assert_uint_eq(CONTR(loaded_player)->journal_sequence, 42);
    ck_assert_int_eq(fclose(fp), 0);
    object_destroy(loaded_player);

    mapstruct saved = {0};
    snprintf(VS(saved.journal_run_id), "%s", run_id);
    saved.journal_sequence = 84;
    fp = tmpfile();
    ck_assert_ptr_ne(fp, NULL);
    save_map_header(&saved, fp, 0);
    rewind(fp);
    mapstruct loaded = {0};
    ck_assert_int_eq(load_map_header(&loaded, fp), 1);
    ck_assert_str_eq(loaded.journal_run_id, run_id);
    ck_assert_uint_eq(loaded.journal_sequence, 84);
    ck_assert_int_eq(fclose(fp), 0);
    object_destroy(pl);
}
END_TEST

START_TEST(test_checkpoint_watermark_resolves_inverse_currency_aba) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *money = object_insert_into(arch_get("coppercoin"), pl, 0);
    money->nrof = 100;
    char directory[] = "/tmp/atrinik-journal-watermark-aba-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    ck_assert_int_eq(shop_insert_coins_reason(pl, 3, "test.aba-grant"), OBJECT_SEMANTIC_COMMITTED);
    uint64_t grant_sequence = CONTR(pl)->journal_sequence;
    ck_assert_int_eq(shop_pay_reason(pl, 3, "test.aba-payment"), OBJECT_SEMANTIC_COMMITTED);
    ck_assert_int_eq(shop_get_money(pl), 100);
    ck_assert_uint_gt(CONTR(pl)->journal_sequence, grant_sequence);
    ck_assert_uint_eq(map->journal_sequence, 0);
    gameplay_journal_deinit();
    remove_fixture(directory);
    object_destroy(pl);
}
END_TEST

START_TEST(test_checkpoint_sequence_remains_ordered_across_runs) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    char directory[] = "/tmp/atrinik-journal-watermark-runs-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    ck_assert_int_eq(shop_insert_coins_reason(pl, 3, "test.run-a"), OBJECT_SEMANTIC_COMMITTED);
    uint64_t first = CONTR(pl)->journal_sequence;
    char first_run[33];
    snprintf(VS(first_run), "%s", CONTR(pl)->journal_run_id);
    gameplay_journal_deinit();

    ck_assert(gameplay_journal_init(directory, "server", &profile));
    ck_assert_int_eq(shop_pay_reason(pl, 3, "test.run-b"), OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_gt(CONTR(pl)->journal_sequence, first);
    ck_assert_str_ne(CONTR(pl)->journal_run_id, first_run);
    gameplay_journal_deinit();
    remove_fixture(directory);
    object_destroy(pl);
}
END_TEST

START_TEST(test_floor_withdrawal_watermarks_player_and_map_domains) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object_insert_into(arch_get("coppercoin"), pl, 0);
    char directory[] = "/tmp/atrinik-journal-floor-withdraw-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    int64_t value;
    ck_assert_int_eq(bank_deposit(pl, "all", &value), BANK_SUCCESS);
    uint64_t before_sequence = CONTR(pl)->journal_sequence;
    pl->carrying = weight_limit[MIN(pl->stats.Str, MAX_STAT)];
    ck_assert_int_eq(bank_withdraw(pl, "all", &value), BANK_SUCCESS);
    ck_assert_int_eq(value, 1);
    ck_assert_int_eq(bank_get_balance(pl), 0);
    ck_assert_ptr_eq(object_find_type(pl, MONEY), NULL);
    ck_assert_ptr_ne(map_find_type(map, pl->x, pl->y, MONEY), NULL);
    ck_assert_uint_gt(CONTR(pl)->journal_sequence, before_sequence);
    ck_assert_uint_eq(map->journal_unique_sequence, CONTR(pl)->journal_sequence);
    ck_assert_str_eq(map->journal_unique_run_id, CONTR(pl)->journal_run_id);
    gameplay_journal_deinit();
    char *contents = read_fixture(directory);
    ck_assert_ptr_ne(strstr(contents, "\"kind\":\"player\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"kind\":\"map-unique\""), NULL);
    free(contents);
    remove_fixture(directory);
    object_destroy(pl);
}
END_TEST

START_TEST(test_semantic_item_shop_and_bank_producers) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);

    char directory[] = "/tmp/atrinik-gameplay-journal-producers-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_counts_reset_for_test();

    for (int map_veto = 0; map_veto <= 1; map_veto++) {
        object *veto_pickup = arch_get("sword");
        veto_pickup->x = pl->x;
        veto_pickup->y = pl->y;
        veto_pickup = object_insert_map(veto_pickup, map, NULL, INS_NO_MERGE);
        player_event_veto_for_test(!map_veto, false, map_veto, false);
        pick_up(pl, veto_pickup, 0);
        player_event_veto_for_test(false, false, false, false);
        ck_assert_ptr_eq(veto_pickup->map, map);

        object *veto_drop = object_insert_into(arch_get("sword"), pl, INS_NO_MERGE);
        player_event_veto_for_test(false, !map_veto, false, map_veto);
        drop_object(pl, veto_drop, 1, 0);
        player_event_veto_for_test(false, false, false, false);
        ck_assert_ptr_eq(object_get_env(veto_drop), pl);
    }
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 0);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.drop"), 0);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_ITEM_UNITS_PICKED_UP), 0);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_ITEM_UNITS_DROPPED), 0);
    char *veto_contents = read_fixture(directory);
    ck_assert_ptr_eq(strstr(veto_contents, "\"reason\":\"item.acquire\""), NULL);
    ck_assert_ptr_eq(strstr(veto_contents, "\"reason\":\"item.drop\""), NULL);
    free(veto_contents);

    object *existing = arch_get("bolt");
    existing->nrof = 4;
    existing = object_insert_into(existing, pl, 0);
    object *ground = arch_get("bolt");
    ground->nrof = 5;
    ground->x = pl->x;
    ground->y = pl->y;
    ground = object_insert_map(ground, map, NULL, INS_NO_MERGE);
    CONTR(pl)->count = 2;
    pick_up(pl, ground, 1);
    ck_assert_uint_eq(ground->nrof, 3);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_ITEM_UNITS_PICKED_UP), 2);
    ck_assert_uint_eq(strlen(CONTR(pl)->journal_run_id), 32);
    ck_assert_uint_gt(CONTR(pl)->journal_sequence, 0);
    ck_assert_str_eq(map->journal_run_id, CONTR(pl)->journal_run_id);
    ck_assert_uint_eq(map->journal_sequence, CONTR(pl)->journal_sequence);
    object *acquired = NULL;
    FOR_INV_PREPARE(pl, candidate) {
        if (candidate->arch == ground->arch && candidate->custody_lineage != NULL) {
            acquired = candidate;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert_ptr_ne(acquired, NULL);
    ck_assert_ptr_ne(acquired, existing);
    ck_assert_uint_eq(acquired->nrof, 2);
    ck_assert_uint_eq(existing->nrof, 4);
    drop_object(pl, acquired, 1, 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.drop"), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_ITEM_UNITS_DROPPED), 1);

    object *sack = object_insert_into(arch_get("sack"), pl, 0);
    SET_FLAG(sack, FLAG_APPLIED);
    object *ground_sword = arch_get("sword");
    ground_sword->x = pl->x;
    ground_sword->y = pl->y;
    ground_sword = object_insert_map(ground_sword, map, NULL, INS_NO_MERGE);
    put_object_in_sack(pl, sack, ground_sword, 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 2);
    object *inserted = NULL;
    ck_assert_int_eq(object_insert_into_reason(acquired, sack, "test.reorder", &inserted),
                     OBJECT_SEMANTIC_COMMITTED);
    acquired = inserted;
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.reorder"), 0);
    object *external = arch_get("sack");
    external->x = pl->x;
    external->y = pl->y;
    external = object_insert_map(external, map, NULL, INS_NO_MERGE);
    ck_assert_int_eq(
        object_insert_into_reason(acquired, external, "item.external-transfer", &inserted),
        OBJECT_SEMANTIC_COMMITTED);
    acquired = inserted;
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.external-transfer"), 1);

    object *blocked = arch_get("sword");
    SET_FLAG(blocked, FLAG_NO_PICK);
    blocked->x = pl->x;
    blocked->y = pl->y;
    blocked = object_insert_map(blocked, map, NULL, INS_NO_MERGE);
    pick_up(pl, blocked, 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 2);

    object *unpaid = arch_get("sword");
    SET_FLAG(unpaid, FLAG_UNPAID);
    unpaid->x = pl->x;
    unpaid->y = pl->y;
    unpaid = object_insert_map(unpaid, map, NULL, INS_NO_MERGE);
    pick_up(pl, unpaid, 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 2);

    object *money = object_insert_into(arch_get("coppercoin"), pl, 0);
    money->nrof = 100;
    int64_t value;
    ck_assert_int_eq(bank_deposit(pl, "40 copper", &value), BANK_SUCCESS);
    ck_assert_int_eq(value, 40);
    ck_assert_int_eq(bank_get_balance(pl), 40);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("bank.deposit"), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_BANK_DEPOSITS), 1);

    ck_assert_int_eq(bank_withdraw(pl, "10 copper", &value), BANK_SUCCESS);
    ck_assert_int_eq(value, 10);
    ck_assert_int_eq(bank_get_balance(pl), 30);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("bank.withdraw"), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_BANK_WITHDRAWALS), 1);
    bool tagged_withdrawal = false;
    FOR_INV_PREPARE(pl, candidate) {
        if (candidate->type == MONEY && candidate->custody_lineage != NULL &&
            strncmp(candidate->custody_lineage, "currency:", 9) == 0) {
            tagged_withdrawal = true;
            break;
        }
    }
    FOR_INV_FINISH();
    ck_assert(!tagged_withdrawal);

    object *sale = arch_get("sword");
    SET_FLAG(sale, FLAG_IDENTIFIED);
    sale->value = 75;
    ck_assert(shop_pay_item(pl, sale));
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("shop.purchase"), 1);
    ck_assert_int_eq(bank_get_balance(pl), 25);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("bank.withdraw"), 1);
    ck_assert_ptr_ne(sale->custody_lineage, NULL);
    int64_t total_after_purchase = shop_get_money(pl);
    sale->value = total_after_purchase + 1;
    ck_assert(!shop_pay_item(pl, sale));
    ck_assert_int_eq(shop_get_money(pl), total_after_purchase);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("shop.purchase"), 1);
    object_destroy(sale);

    ck_assert_int_eq(bank_withdraw(pl, "5 copper", &value), BANK_SUCCESS);
    ck_assert_int_eq(shop_pay_reason(pl, 10, "test.mixed-payment"), OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.mixed-payment"), 1);
    ck_assert_int_eq(shop_insert_coins_reason(pl, 3, "test.currency-grant"),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.currency-grant"), 1);
    shop_insert_coins(pl, 2);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("script.currency-grant"), 1);

    object *alchemy_source = object_insert_into(arch_get("coppercoin"), pl, INS_NO_MERGE);
    alchemy_source->nrof = 10;
    CONTR(pl)->mark = alchemy_source;
    CONTR(pl)->mark_count = alchemy_source->count;
    ck_assert_int_eq(cast_transform_wealth(pl), 1);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("spell.alchemy"), 1);
    object *restricted_sack = arch_get("sack");
    FREE_AND_COPY_HASH(restricted_sack->race, "food");
    restricted_sack = object_insert_into(restricted_sack, pl, INS_NO_MERGE);
    object *excluded_source = object_insert_into(arch_get("coppercoin"), restricted_sack, 0);
    excluded_source->nrof = 10;
    CONTR(pl)->mark = excluded_source;
    CONTR(pl)->mark_count = excluded_source->count;
    ck_assert_int_eq(cast_transform_wealth(pl), 0);
    ck_assert_ptr_eq(excluded_source->env, restricted_sack);
    object *mutated_source = object_insert_into(arch_get("coppercoin"), pl, INS_NO_MERGE);
    mutated_source->value = 2;
    CONTR(pl)->mark = mutated_source;
    CONTR(pl)->mark_count = mutated_source->count;
    ck_assert_int_eq(cast_transform_wealth(pl), 0);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("spell.alchemy"), 1);
    FOR_INV_PREPARE(pl, candidate) {
        if (candidate->type == MONEY) {
            ck_assert_ptr_eq(candidate->custody_lineage, NULL);
        }
    }
    FOR_INV_FINISH();

    object *grant = arch_get("sword");
    ck_assert_int_eq(object_insert_into_reason(grant, pl, "test.item-grant", &inserted),
                     OBJECT_SEMANTIC_COMMITTED);
    grant = inserted;
    ck_assert_ptr_ne(grant, NULL);
    ck_assert_ptr_ne(grant->custody_lineage, NULL);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.item-grant"), 1);
    ck_assert_int_eq(object_remove_reason(grant, "test.item-destroy", true),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.item-destroy"), 1);

    object *startequip = object_insert_into(arch_get("sword"), pl, INS_NO_MERGE);
    SET_FLAG(startequip, FLAG_STARTEQUIP);
    uint32_t startequip_tag = startequip->count;
    drop_object(pl, startequip, 1, 1);
    ck_assert(!OBJECT_VALID(startequip, startequip_tag));
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.startequip-destroy"), 1);

    object *other_player = player_get_dummy("Journal transfer recipient", NULL);
    object *transferred = NULL;
    ck_assert_int_eq(object_insert_into_reason(arch_get("sword"),
                                               pl,
                                               "test.player-transfer-stock",
                                               &transferred),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_int_eq(
        object_insert_into_reason(transferred, other_player, "item.player-transfer", &transferred),
        OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.player-transfer"), 1);
    ck_assert_uint_eq(CONTR(other_player)->journal_sequence, CONTR(pl)->journal_sequence);
    ck_assert_str_eq(CONTR(other_player)->journal_run_id, CONTR(pl)->journal_run_id);
    object_destroy(other_player);

    object *temporary = arch_get("force");
    ck_assert_int_eq(object_insert_into_reason(temporary, pl, "test.temporary-grant", &inserted),
                     OBJECT_SEMANTIC_COMMITTED);
    temporary = inserted;
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.temporary-grant"), 0);
    temporary->type = POTION_EFFECT;
    ck_assert_int_eq(object_remove_reason(temporary, "test.temporary-destroy", true),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("test.temporary-destroy"), 0);

    object *sold = arch_get("sword");
    SET_FLAG(sold, FLAG_IDENTIFIED);
    sold->value = 100;
    ck_assert_int_eq(object_insert_into_reason(sold, pl, "test.sale-stock", &inserted),
                     OBJECT_SEMANTIC_COMMITTED);
    sold = inserted;
    uint64_t sales_before = metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_SHOP_SALES);
    shop_sell_item(pl, sold);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("shop.sale"), 1);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_SHOP_SALES),
                      sales_before + 1);

    object *failure_coin = object_insert_into(arch_get("coppercoin"), pl, 0);
    failure_coin->nrof = 1;
    int64_t balance_before = bank_get_balance(pl);
    int64_t total_before = shop_get_money(pl);
    gameplay_journal_fail_writes_for_test(true);
    ck_assert_int_eq(bank_deposit(pl, "1 copper", &value), BANK_JOURNAL_ERROR);
    ck_assert_int_eq(value, 0);
    ck_assert_int_eq(bank_get_balance(pl), balance_before);
    ck_assert_int_eq(shop_get_money(pl), total_before);
    gameplay_journal_fail_writes_for_test(false);

    gameplay_journal_deinit();
    char *contents = read_fixture(directory);
    ck_assert_ptr_ne(strstr(contents, "\"reason\":\"shop.purchase\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"price\":75"), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"funding\":\"mixed\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"provenance_before\":"), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"provenance_after\":"), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"reason\":\"shop.sale\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"source\":\"ground\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"destination\":\"external-container\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"reason\":\"item.player-transfer\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"reason\":\"test.mixed-payment\""), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"before\":20,\"delta\":-5,\"after\":15"), NULL);
    ck_assert_ptr_ne(strstr(contents, "\"price\":10"), NULL);
    free(contents);
    object_destroy(pl);
    remove_fixture(directory);
}
END_TEST

START_TEST(test_semantic_commit_failure_remains_reconcilable) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    object *money = object_insert_into(arch_get("coppercoin"), pl, 0);
    money->nrof = 1;

    char directory[] = "/tmp/atrinik-gameplay-journal-commit-failure-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(directory), NULL);
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    gameplay_journal_counts_reset_for_test();
    gameplay_journal_fail_after_writes_for_test(1);

    int64_t value;
    ck_assert_int_eq(bank_deposit(pl, "1 copper", &value), BANK_JOURNAL_AMBIGUOUS);
    ck_assert_int_eq(value, 1);
    ck_assert_int_eq(bank_get_balance(pl), 1);
    ck_assert(!gameplay_journal_available());
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("bank.deposit"), 0);
    ck_assert_uint_eq(metrics_get(&CONTR(pl)->metrics, METRIC_CHARACTER_BANK_DEPOSITS), 0);

    gameplay_journal_fail_after_writes_for_test(SIZE_MAX);
    gameplay_journal_deinit();
    remove_fixture(directory);

    char withdrawal_directory[] = "/tmp/atrinik-gameplay-journal-withdraw-failure-XXXXXX";
    semantic_failure_journal_init(withdrawal_directory);
    ck_assert_int_eq(bank_withdraw(pl, "all", &value), BANK_JOURNAL_AMBIGUOUS);
    ck_assert_int_eq(value, 1);
    ck_assert_int_eq(bank_get_balance(pl), 0);
    object *withdrawn = object_find_type(pl, MONEY);
    ck_assert_ptr_ne(withdrawn, NULL);
    ck_assert_ptr_ne(withdrawn->custody_lineage, NULL);
    ck_assert_int_eq(strncmp(withdrawn->custody_lineage, "currency:", 9), 0);
    semantic_failure_journal_deinit(withdrawal_directory);

    object_destroy(pl);
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
    ck_assert(gameplay_journal_begin(&subject, GAMEPLAY_JOURNAL_QUEST, "test.held", &change, held));
    while (gameplay_journal_available()) {
        char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
        if (!gameplay_journal_begin(&subject,
                                    GAMEPLAY_JOURNAL_QUEST,
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
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, shop_insert_coins_reason),
                      offsetof(struct plugin_hooklist, player_status_set));
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, object_insert_into_reason),
                      offsetof(struct plugin_hooklist, shop_pay_reason));
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, object_enter_map_reason),
                      offsetof(struct plugin_hooklist, object_insert_map_reason));
    ck_assert_uint_gt(offsetof(struct plugin_hooklist, object_decrease_reason),
                      offsetof(struct plugin_hooklist, gameplay_journal_map_checkpoint_allowed));
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
                                         GAMEPLAY_JOURNAL_QUEST,
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
                                         GAMEPLAY_JOURNAL_QUEST,
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

static void write_crash_file(const char *directory, const char *name, const char *state) {
    char path[HUGE_BUF];
    snprintf(VS(path), "%s/%s", directory, name);
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_uint_eq(fwrite(state, 1, strlen(state), fp), strlen(state));
    ck_assert_int_eq(fflush(fp), 0);
    ck_assert_int_eq(fsync(fileno(fp)), 0);
    ck_assert_int_eq(fclose(fp), 0);
}

static void write_crash_state(const char *directory, const char *state) {
    write_crash_file(directory, "authoritative.state", state);
}

static void write_crash_ground(const char *directory, const object *ground) {
    if (ground == NULL) {
        write_crash_file(directory, "ground.state", "");
        return;
    }
    StringBuffer *serialized = stringbuffer_new();
    object_dump_rec(ground, serialized);
    char *state = stringbuffer_finish(serialized);
    write_crash_file(directory, "ground.state", state);
    free(state);
}

static bool crash_intent_field(const char *contents,
                               const char *reason,
                               const char *field,
                               char *value,
                               size_t value_size) {
    char reason_token[GAMEPLAY_JOURNAL_ID_MAX + 32];
    snprintf(VS(reason_token), "\"reason\":\"%s\"", reason);
    const char *reason_pos = strstr(contents, reason_token);
    if (reason_pos == NULL) {
        return false;
    }
    const char *line = reason_pos;
    while (line > contents && line[-1] != '\n') {
        line--;
    }
    const char *line_end = strchr(reason_pos, '\n');
    if (line_end == NULL) {
        line_end = contents + strlen(contents);
    }
    char field_token[128];
    snprintf(VS(field_token), "\"%s\":\"", field);
    const char *start = strstr(line, field_token);
    if (start == NULL || start >= line_end) {
        return false;
    }
    start += strlen(field_token);
    const char *end = strchr(start, '"');
    if (end == NULL || end > line_end || (size_t)(end - start) >= value_size) {
        return false;
    }
    memcpy(value, start, (size_t)(end - start));
    value[end - start] = '\0';
    return true;
}

typedef enum crash_operation {
    CRASH_ITEM_GRANT,
    CRASH_ITEM_ACQUIRE,
    CRASH_ITEM_DROP,
    CRASH_ITEM_DESTROY,
    CRASH_CURRENCY_GRANT,
    CRASH_BANK_DEPOSIT,
    CRASH_BANK_WITHDRAW,
    CRASH_SHOP_PURCHASE,
    CRASH_SHOP_SALE,
    CRASH_OPERATION_COUNT,
} crash_operation_t;

static const char *crash_operation_reason(crash_operation_t operation) {
    static const char *reasons[CRASH_OPERATION_COUNT] = {
        "test.crash-item",
        "item.acquire",
        "item.drop",
        "test.crash-destroy",
        "test.crash-currency",
        "bank.deposit",
        "bank.withdraw",
        "shop.purchase",
        "shop.sale",
    };
    return reasons[operation];
}

static void crash_semantic_writer(const char *directory,
                                  object *pl,
                                  crash_operation_t operation,
                                  bool fail_terminal,
                                  bool checkpoint_after) {
    const gameplay_journal_profile_t profile = {
        .id = "legacy-unknown",
        .schema = 0,
        .digest = "unknown",
        .effective_axes = "unknown",
    };
    object *seed = NULL;
    if (operation == CRASH_BANK_WITHDRAW) {
        object *seed = arch_get("coppercoin");
        object_insert_into(seed, pl, 0);
        int64_t deposited;
        ck_assert_int_eq(bank_deposit(pl, "1 copper", &deposited), BANK_SUCCESS);
    } else if (operation == CRASH_BANK_DEPOSIT) {
        seed = object_insert_into(arch_get("coppercoin"), pl, 0);
    } else if (operation == CRASH_SHOP_PURCHASE) {
        object *money = arch_get("coppercoin");
        money->nrof = 100;
        object_insert_into(money, pl, 0);
        seed = arch_get("sword");
        SET_FLAG(seed, FLAG_IDENTIFIED);
        SET_FLAG(seed, FLAG_UNPAID);
        seed->value = 75;
        seed = object_insert_into(seed, pl, INS_NO_MERGE);
    } else if (operation == CRASH_CURRENCY_GRANT) {
        object *money = arch_get("coppercoin");
        money->nrof = 100;
        object_insert_into(money, pl, 0);
    } else if (operation == CRASH_ITEM_DROP || operation == CRASH_ITEM_DESTROY ||
               operation == CRASH_SHOP_SALE) {
        seed = arch_get("sword");
        SET_FLAG(seed, FLAG_IDENTIFIED);
        seed->value = 100;
        seed = object_insert_into(seed, pl, INS_NO_MERGE);
    } else if (operation == CRASH_ITEM_ACQUIRE) {
        seed = arch_get("sword");
        seed->x = pl->x;
        seed->y = pl->y;
        seed = object_insert_map(seed, pl->map, NULL, INS_NO_MERGE);
    }
    StringBuffer *serialized = stringbuffer_new();
    object_dump_rec(pl, serialized);
    char *state = stringbuffer_finish(serialized);
    write_crash_state(directory, state);
    free(state);
    if (operation == CRASH_ITEM_ACQUIRE) {
        write_crash_ground(directory, seed);
    } else if (operation == CRASH_ITEM_DROP) {
        write_crash_ground(directory, NULL);
    }

    ck_assert(gameplay_journal_init(directory, "server", &profile));
    if (fail_terminal) {
        gameplay_journal_fail_after_writes_for_test(
            operation == CRASH_ITEM_ACQUIRE || operation == CRASH_ITEM_DROP ? 2 : 1);
    }
    if (operation == CRASH_ITEM_GRANT) {
        object *inserted = NULL;
        object_semantic_result_t result =
            object_insert_into_reason(arch_get("sword"),
                                      pl,
                                      crash_operation_reason(operation),
                                      &inserted);
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS : OBJECT_SEMANTIC_COMMITTED);
        ck_assert_ptr_ne(inserted, NULL);
        ck_assert_ptr_ne(inserted->custody_lineage, NULL);
    } else if (operation == CRASH_ITEM_ACQUIRE) {
        pick_up(pl, seed, 1);
        object *inserted = object_find_arch(pl, arch_find("sword"));
        ck_assert_ptr_ne(inserted, NULL);
        ck_assert_ptr_ne(inserted->custody_lineage, NULL);
    } else if (operation == CRASH_ITEM_DROP) {
        drop_object(pl, seed, 1, 1);
        ck_assert_ptr_eq(object_find_arch(pl, arch_find("sword")), NULL);
    } else if (operation == CRASH_ITEM_DESTROY) {
        object_semantic_result_t result =
            object_remove_reason(seed, crash_operation_reason(operation), true);
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS : OBJECT_SEMANTIC_COMMITTED);
    } else if (operation == CRASH_CURRENCY_GRANT) {
        object_semantic_result_t result =
            shop_insert_coins_reason(pl, 3, crash_operation_reason(operation));
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS : OBJECT_SEMANTIC_COMMITTED);
    } else if (operation == CRASH_BANK_DEPOSIT) {
        int64_t deposited;
        ck_assert_int_eq(bank_deposit(pl, "all", &deposited),
                         fail_terminal ? BANK_JOURNAL_AMBIGUOUS : BANK_SUCCESS);
        ck_assert_int_eq(deposited, 1);
        ck_assert_int_eq(bank_get_balance(pl), 1);
    } else if (operation == CRASH_BANK_WITHDRAW) {
        int64_t withdrawn;
        ck_assert_int_eq(bank_withdraw(pl, "all", &withdrawn),
                         fail_terminal ? BANK_JOURNAL_AMBIGUOUS : BANK_SUCCESS);
        object *coins = object_find_type(pl, MONEY);
        ck_assert_int_eq(bank_get_balance(pl), 0);
        ck_assert_ptr_ne(coins, NULL);
        if (fail_terminal) {
            ck_assert_ptr_ne(coins->custody_lineage, NULL);
            ck_assert_int_eq(strncmp(coins->custody_lineage, "currency:", 9), 0);
        } else {
            ck_assert_ptr_eq(coins->custody_lineage, NULL);
        }
    } else if (operation == CRASH_SHOP_PURCHASE) {
        ck_assert_int_eq(shop_pay_items(pl), !fail_terminal);
        ck_assert(!QUERY_FLAG(seed, FLAG_UNPAID));
        ck_assert_int_eq(shop_get_money(pl), 25);
    } else {
        object_custody_transaction_t transaction;
        ck_assert(shop_sell_item_begin(pl, seed, 1, &transaction));
        object_custody_apply(seed, &transaction);
        object_remove(seed, 0);
        ck_assert_int_eq(shop_sell_item_commit(pl, seed, &transaction), !fail_terminal);
    }
    if (checkpoint_after) {
        serialized = stringbuffer_new();
        object_dump_rec(pl, serialized);
        state = stringbuffer_finish(serialized);
        write_crash_state(directory, state);
        free(state);
        if (operation == CRASH_ITEM_ACQUIRE) {
            write_crash_ground(directory, NULL);
        } else if (operation == CRASH_ITEM_DROP) {
            object *ground = NULL;
            FOR_MAP_PREPARE(pl->map, pl->x, pl->y, tmp) {
                if (tmp->arch == arch_find("sword")) {
                    ground = tmp;
                    break;
                }
            }
            FOR_MAP_FINISH();
            ck_assert_ptr_ne(ground, NULL);
            write_crash_ground(directory, ground);
        }
    }
    _exit(EXIT_SUCCESS);
}

START_TEST(test_abrupt_semantic_operations_leave_reconcilable_authoritative_state) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    for (int operation = 0; operation < CRASH_OPERATION_COUNT; operation++) {
        for (int fail_terminal = 0; fail_terminal <= 1; fail_terminal++) {
            for (int checkpoint_after = 0; checkpoint_after <= 1; checkpoint_after++) {
                char directory[] = "/tmp/atrinik-gameplay-journal-semantic-crash-XXXXXX";
                ck_assert_ptr_ne(mkdtemp(directory), NULL);
                pid_t child = fork();
                ck_assert_int_ge(child, 0);
                if (child == 0) {
                    crash_semantic_writer(directory,
                                          pl,
                                          (crash_operation_t)operation,
                                          fail_terminal != 0,
                                          checkpoint_after != 0);
                }
                int status;
                ck_assert_int_eq(waitpid(child, &status, 0), child);
                ck_assert(WIFEXITED(status));
                ck_assert_int_eq(WEXITSTATUS(status), EXIT_SUCCESS);

                char *contents = read_fixture(directory);
                ck_assert_ptr_ne(strstr(contents, "\"phase\":\"intent\""), NULL);
                ck_assert_int_eq(strstr(contents, "\"phase\":\"commit\"") != NULL, !fail_terminal);
                char reason[GAMEPLAY_JOURNAL_ID_MAX + 32];
                snprintf(VS(reason),
                         "\"reason\":\"%s\"",
                         crash_operation_reason((crash_operation_t)operation));
                ck_assert_ptr_ne(strstr(contents, reason), NULL);
                char intent_transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
                char intent_lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
                ck_assert(crash_intent_field(contents,
                                             crash_operation_reason((crash_operation_t)operation),
                                             "transaction_id",
                                             VS(intent_transaction)));
                ck_assert(crash_intent_field(contents,
                                             crash_operation_reason((crash_operation_t)operation),
                                             "lineage_id",
                                             VS(intent_lineage)));
                static const int64_t expected_arithmetic[CRASH_OPERATION_COUNT][3] = {
                    {0, 1, 1},
                    {0, 1, 1},
                    {1, -1, 0},
                    {1, -1, 0},
                    {100, 3, 103},
                    {0, 1, 1},
                    {1, -1, 0},
                    {100, -75, 25},
                    {0, 20, 20},
                };
                char arithmetic[160];
                snprintf(VS(arithmetic),
                         "\"before\":%" PRId64 ",\"delta\":%" PRId64 ",\"after\":%" PRId64,
                         expected_arithmetic[operation][0],
                         expected_arithmetic[operation][1],
                         expected_arithmetic[operation][2]);
                ck_assert_ptr_ne(strstr(contents, arithmetic), NULL);
                free(contents);

                char state_path[HUGE_BUF];
                snprintf(VS(state_path), "%s/authoritative.state", directory);
                struct stat metadata;
                ck_assert_int_eq(stat(state_path, &metadata), 0);
                char *state_text = malloc((size_t)metadata.st_size + 1);
                ck_assert_ptr_ne(state_text, NULL);
                FILE *state = fopen(state_path, "rb");
                ck_assert_ptr_ne(state, NULL);
                ck_assert_uint_eq(fread(state_text, 1, (size_t)metadata.st_size, state),
                                  (size_t)metadata.st_size);
                state_text[metadata.st_size] = '\0';
                ck_assert_int_eq(fclose(state), 0);
                object *reloaded = object_load_str(state_text);
                ck_assert_ptr_ne(reloaded, NULL);
                if (operation == CRASH_ITEM_GRANT || operation == CRASH_ITEM_ACQUIRE) {
                    object *item = object_find_arch(reloaded, arch_find("sword"));
                    ck_assert_int_eq(item != NULL, checkpoint_after);
                    if (item != NULL) {
                        ck_assert_ptr_ne(item->custody_lineage, NULL);
                        ck_assert_str_eq(item->custody_lineage, intent_lineage);
                    }
                } else if (operation == CRASH_ITEM_DROP || operation == CRASH_ITEM_DESTROY) {
                    ck_assert_int_eq(object_find_arch(reloaded, arch_find("sword")) == NULL,
                                     checkpoint_after);
                } else if (operation == CRASH_CURRENCY_GRANT) {
                    ck_assert_int_eq(shop_get_money(reloaded), checkpoint_after ? 103 : 100);
                    if (checkpoint_after && fail_terminal) {
                        char expected[GAMEPLAY_JOURNAL_ID_MAX + 1];
                        snprintf(VS(expected), "currency:%s", intent_transaction);
                        bool found = false;
                        FOR_INV_PREPARE(reloaded, coin) {
                            if (coin->type == MONEY && coin->custody_lineage != NULL &&
                                strcmp(coin->custody_lineage, expected) == 0) {
                                found = true;
                                break;
                            }
                        }
                        FOR_INV_FINISH();
                        ck_assert(found);
                    }
                } else if (operation == CRASH_BANK_DEPOSIT) {
                    ck_assert_int_eq(bank_get_balance(reloaded), checkpoint_after ? 1 : 0);
                    ck_assert_int_eq(object_find_type(reloaded, MONEY) == NULL, checkpoint_after);
                } else if (operation == CRASH_BANK_WITHDRAW) {
                    ck_assert_int_eq(bank_get_balance(reloaded), checkpoint_after ? 0 : 1);
                    object *coins = object_find_type(reloaded, MONEY);
                    ck_assert_int_eq(coins != NULL, checkpoint_after);
                    if (coins != NULL && fail_terminal) {
                        ck_assert_ptr_ne(coins->custody_lineage, NULL);
                        char expected[GAMEPLAY_JOURNAL_ID_MAX + 1];
                        snprintf(VS(expected), "currency:%s", intent_transaction);
                        ck_assert_str_eq(coins->custody_lineage, expected);
                    } else if (coins != NULL) {
                        ck_assert_ptr_eq(coins->custody_lineage, NULL);
                    }
                } else if (operation == CRASH_SHOP_PURCHASE) {
                    object *item = object_find_arch(reloaded, arch_find("sword"));
                    ck_assert_ptr_ne(item, NULL);
                    ck_assert_int_eq(!QUERY_FLAG(item, FLAG_UNPAID), checkpoint_after);
                    ck_assert_int_eq(shop_get_money(reloaded), checkpoint_after ? 25 : 100);
                } else {
                    object *coins = object_find_type(reloaded, MONEY);
                    ck_assert_int_eq(coins != NULL, checkpoint_after);
                    ck_assert_int_eq(shop_get_money(reloaded), checkpoint_after ? 20 : 0);
                    if (coins != NULL && fail_terminal) {
                        ck_assert_ptr_ne(coins->custody_lineage, NULL);
                        char expected[GAMEPLAY_JOURNAL_ID_MAX + 1];
                        snprintf(VS(expected), "currency:%s", intent_transaction);
                        ck_assert_str_eq(coins->custody_lineage, expected);
                    } else if (coins != NULL) {
                        ck_assert_ptr_eq(coins->custody_lineage, NULL);
                    }
                    ck_assert_int_eq(object_find_arch(reloaded, arch_find("sword")) == NULL,
                                     checkpoint_after);
                }
                object_destroy(reloaded);
                free(state_text);
                ck_assert_int_eq(unlink(state_path), 0);
                if (operation == CRASH_ITEM_ACQUIRE || operation == CRASH_ITEM_DROP) {
                    snprintf(VS(state_path), "%s/ground.state", directory);
                    ck_assert_int_eq(stat(state_path, &metadata), 0);
                    bool ground_expected =
                        operation == CRASH_ITEM_ACQUIRE ? !checkpoint_after : checkpoint_after;
                    ck_assert_int_eq(metadata.st_size != 0, ground_expected);
                    if (ground_expected) {
                        state_text = malloc((size_t)metadata.st_size + 1);
                        ck_assert_ptr_ne(state_text, NULL);
                        state = fopen(state_path, "rb");
                        ck_assert_ptr_ne(state, NULL);
                        ck_assert_uint_eq(fread(state_text, 1, (size_t)metadata.st_size, state),
                                          (size_t)metadata.st_size);
                        state_text[metadata.st_size] = '\0';
                        ck_assert_int_eq(fclose(state), 0);
                        object *ground = object_load_str(state_text);
                        ck_assert_ptr_ne(ground, NULL);
                        ck_assert_ptr_eq(ground->arch, arch_find("sword"));
                        if (checkpoint_after) {
                            ck_assert_ptr_ne(ground->custody_lineage, NULL);
                            ck_assert_str_eq(ground->custody_lineage, intent_lineage);
                        }
                        object_destroy(ground);
                        free(state_text);
                    }
                    ck_assert_int_eq(unlink(state_path), 0);
                }
                remove_fixture(directory);
            }
        }
    }
    object_destroy(pl);
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
                                      GAMEPLAY_JOURNAL_QUEST,
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
    tcase_add_test(tc_core, test_semantic_item_shop_and_bank_producers);
    tcase_add_test(tc_core, test_semantic_commit_failure_remains_reconcilable);
    tcase_add_test(tc_core, test_item_terminal_failures_report_ambiguity);
    tcase_add_test(tc_core, test_shop_purchase_terminal_failure_keeps_authoritative_state);
    tcase_add_test(tc_core, test_party_loot_preparation_failure_preserves_corpse);
    tcase_add_test(tc_core, test_party_random_currency_retirement_preserves_message_lifetime);
    tcase_add_test(tc_core, test_currency_recovery_aggregate_includes_floor_delivery);
    tcase_add_test(tc_core, test_one_drop_quest_grant_commits_item_and_marker_together);
    tcase_add_test(tc_core,
                   test_pending_domains_block_checkpoints_and_unique_maps_use_primary_component);
    tcase_add_test(tc_core,
                   test_production_player_and_map_checkpoints_persist_component_watermarks);
    tcase_add_test(tc_core, test_checkpoint_watermarks_persist_player_and_map);
    tcase_add_test(tc_core, test_checkpoint_watermark_resolves_inverse_currency_aba);
    tcase_add_test(tc_core, test_checkpoint_sequence_remains_ordered_across_runs);
    tcase_add_test(tc_core, test_floor_withdrawal_watermarks_player_and_map_domains);
#ifndef WIN32
    tcase_add_test(tc_core, test_abrupt_process_crash_preserves_synced_phases);
    tcase_add_test(tc_core, test_abrupt_semantic_operations_leave_reconcilable_authoritative_state);
    tcase_add_test(tc_core, test_second_writer_is_rejected_while_lock_is_held);
#endif
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_gameplay_journal(void) {
    check_run_suite(suite(), __FILE__);
}
