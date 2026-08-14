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
    ck_assert_int_eq(shop_pay_reason(pl, 1, "test.payment-failure"),
                     OBJECT_SEMANTIC_AMBIGUOUS);
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

    object *veto_pickup = arch_get("sword");
    veto_pickup->x = pl->x;
    veto_pickup->y = pl->y;
    veto_pickup = object_insert_map(veto_pickup, map, NULL, INS_NO_MERGE);
    player_event_veto_for_test(true, false);
    pick_up(pl, veto_pickup, 1);
    player_event_veto_for_test(false, false);
    ck_assert_ptr_eq(veto_pickup->map, map);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.acquire"), 0);

    object *veto_drop = object_insert_into(arch_get("sword"), pl, INS_NO_MERGE);
    player_event_veto_for_test(false, true);
    drop_object(pl, veto_drop, 1, 1);
    player_event_veto_for_test(false, false);
    ck_assert_ptr_eq(object_get_env(veto_drop), pl);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.drop"), 0);

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

    object *other_player = player_get_dummy("Journal transfer recipient", NULL);
    object *transferred = NULL;
    ck_assert_int_eq(object_insert_into_reason(arch_get("sword"),
                                               pl,
                                               "test.player-transfer-stock",
                                               &transferred),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_int_eq(object_insert_into_reason(transferred,
                                               other_player,
                                               "item.player-transfer",
                                               &transferred),
                     OBJECT_SEMANTIC_COMMITTED);
    ck_assert_uint_eq(gameplay_journal_committed_count_for_test("item.player-transfer"), 1);
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

static void write_crash_state(const char *directory, const char *state) {
    char path[HUGE_BUF];
    snprintf(VS(path), "%s/authoritative.state", directory);
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_ne(fp, NULL);
    ck_assert_uint_eq(fwrite(state, 1, strlen(state), fp), strlen(state));
    ck_assert_int_eq(fflush(fp), 0);
    ck_assert_int_eq(fsync(fileno(fp)), 0);
    ck_assert_int_eq(fclose(fp), 0);
}

typedef enum crash_operation {
    CRASH_ITEM_GRANT,
    CRASH_ITEM_ACQUIRE,
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
                                  bool fail_terminal) {
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
    } else if (operation == CRASH_ITEM_DESTROY || operation == CRASH_SHOP_SALE) {
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
    ck_assert(gameplay_journal_init(directory, "server", &profile));
    if (fail_terminal) {
        gameplay_journal_fail_after_writes_for_test(1);
    }
    if (operation == CRASH_ITEM_GRANT || operation == CRASH_ITEM_ACQUIRE) {
        object *inserted = NULL;
        object *item = operation == CRASH_ITEM_GRANT ? arch_get("sword") : seed;
        object_semantic_result_t result = object_insert_into_reason(
            item, pl, crash_operation_reason(operation), &inserted);
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS
                                       : OBJECT_SEMANTIC_COMMITTED);
        ck_assert_ptr_ne(inserted, NULL);
        ck_assert_ptr_ne(inserted->custody_lineage, NULL);
    } else if (operation == CRASH_ITEM_DESTROY) {
        object_semantic_result_t result =
            object_remove_reason(seed, crash_operation_reason(operation), true);
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS
                                       : OBJECT_SEMANTIC_COMMITTED);
    } else if (operation == CRASH_CURRENCY_GRANT) {
        object_semantic_result_t result =
            shop_insert_coins_reason(pl, 3, crash_operation_reason(operation));
        ck_assert_int_eq(result,
                         fail_terminal ? OBJECT_SEMANTIC_AMBIGUOUS
                                       : OBJECT_SEMANTIC_COMMITTED);
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
    StringBuffer *serialized = stringbuffer_new();
    object_dump_rec(pl, serialized);
    char *state = stringbuffer_finish(serialized);
    write_crash_state(directory, state);
    free(state);
    _exit(EXIT_SUCCESS);
}

START_TEST(test_abrupt_semantic_operations_leave_reconcilable_authoritative_state) {
    mapstruct *map;
    object *pl;
    check_setup_env_pl(&map, &pl);
    for (int operation = 0; operation < CRASH_OPERATION_COUNT; operation++) {
        for (int fail_terminal = 0; fail_terminal <= 1; fail_terminal++) {
            char directory[] = "/tmp/atrinik-gameplay-journal-semantic-crash-XXXXXX";
            ck_assert_ptr_ne(mkdtemp(directory), NULL);
            pid_t child = fork();
            ck_assert_int_ge(child, 0);
            if (child == 0) {
                crash_semantic_writer(
                    directory, pl, (crash_operation_t)operation, fail_terminal != 0);
            }
            int status;
            ck_assert_int_eq(waitpid(child, &status, 0), child);
            ck_assert(WIFEXITED(status));
            ck_assert_int_eq(WEXITSTATUS(status), EXIT_SUCCESS);

            char *contents = read_fixture(directory);
            ck_assert_ptr_ne(strstr(contents, "\"phase\":\"intent\""), NULL);
            ck_assert_int_eq(strstr(contents, "\"phase\":\"commit\"") != NULL,
                             !fail_terminal);
            char reason[GAMEPLAY_JOURNAL_ID_MAX + 32];
            snprintf(VS(reason),
                     "\"reason\":\"%s\"",
                     crash_operation_reason((crash_operation_t)operation));
            ck_assert_ptr_ne(strstr(contents, reason), NULL);
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
                ck_assert_ptr_ne(item, NULL);
                ck_assert_ptr_ne(item->custody_lineage, NULL);
            } else if (operation == CRASH_ITEM_DESTROY) {
                ck_assert_ptr_eq(object_find_arch(reloaded, arch_find("sword")), NULL);
            } else if (operation == CRASH_BANK_DEPOSIT) {
                ck_assert_int_eq(bank_get_balance(reloaded), 1);
                ck_assert_ptr_eq(object_find_type(reloaded, MONEY), NULL);
            } else if (operation == CRASH_BANK_WITHDRAW) {
                ck_assert_int_eq(bank_get_balance(reloaded), 0);
                object *coins = object_find_type(reloaded, MONEY);
                ck_assert_ptr_ne(coins, NULL);
                if (fail_terminal) {
                    ck_assert_ptr_ne(coins->custody_lineage, NULL);
                    ck_assert_int_eq(strncmp(coins->custody_lineage, "currency:", 9), 0);
                } else {
                    ck_assert_ptr_eq(coins->custody_lineage, NULL);
                }
            } else if (operation == CRASH_SHOP_PURCHASE) {
                object *item = object_find_arch(reloaded, arch_find("sword"));
                ck_assert_ptr_ne(item, NULL);
                ck_assert(!QUERY_FLAG(item, FLAG_UNPAID));
                ck_assert_int_eq(shop_get_money(reloaded), 25);
            } else {
                object *coins = object_find_type(reloaded, MONEY);
                ck_assert_ptr_ne(coins, NULL);
                if (fail_terminal) {
                    ck_assert_ptr_ne(coins->custody_lineage, NULL);
                } else {
                    ck_assert_ptr_eq(coins->custody_lineage, NULL);
                }
            }
            object_destroy(reloaded);
            free(state_text);
            ck_assert_int_eq(unlink(state_path), 0);
            remove_fixture(directory);
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
#ifndef WIN32
    tcase_add_test(tc_core, test_abrupt_process_crash_preserves_synced_phases);
    tcase_add_test(tc_core,
                   test_abrupt_semantic_operations_leave_reconcilable_authoritative_state);
    tcase_add_test(tc_core, test_second_writer_is_rejected_while_lock_is_held);
#endif
    suite_add_tcase(s, tc_core);
    return s;
}

void check_server_gameplay_journal(void) {
    check_run_suite(suite(), __FILE__);
}
