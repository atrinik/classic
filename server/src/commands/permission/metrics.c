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

/** @file Implements the operator-only /metrics command. */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <account.h>
#include <metrics.h>
#include <player.h>
#include <object.h>
#include <toolkit/string.h>

static void metrics_draw_value(object *op, const metric_metadata_t *metadata, uint64_t value) {
    if (metadata->kind == METRIC_KIND_DURATION) {
        draw_info_format(COLOR_WHITE,
                         op,
                         "  %s: %" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                         metadata->name,
                         value / 3600,
                         value / 60 % 60,
                         value % 60);
    } else if (metadata->kind == METRIC_KIND_TIMESTAMP) {
        draw_info_format(COLOR_WHITE, op, "  %s: UTC %" PRIu64, metadata->name, value);
    } else {
        draw_info_format(COLOR_WHITE,
                         op,
                         "  %s: %" PRIu64 " %s",
                         metadata->name,
                         value,
                         metrics_unit_name(metadata->unit));
    }
}

static void metrics_draw_store(object *op,
                               const metric_store_t *store,
                               const char *owner,
                               const char *category) {
    draw_info_format(COLOR_NAVY,
                     op,
                     "[b]%s metrics: %s[/b] (epoch UTC %" PRIu64 ")",
                     metrics_scope_name(store->scope),
                     owner,
                     store->epoch);

    bool any = false;
    for (metric_id_t id = 0; id < METRIC_COUNT; id++) {
        const metric_metadata_t *metadata = metrics_metadata(id);
        uint64_t value = metrics_get(store, id);
        if (metadata == NULL || metadata->scope != store->scope || value == 0 ||
            (category != NULL && strcasecmp(metadata->category, category) != 0)) {
            continue;
        }
        metrics_draw_value(op, metadata, value);
        any = true;
    }

    for (metric_collection_id_t id = 0; id < METRIC_COLLECTION_COUNT; id++) {
        const metric_collection_metadata_t *metadata = metrics_collection_metadata(id);
        size_t count = metrics_unique_count(store, id);
        if (metadata == NULL || metadata->scope != store->scope || count == 0 ||
            (category != NULL && strcasecmp(metadata->category, category) != 0)) {
            continue;
        }
        draw_info_format(COLOR_WHITE,
                         op,
                         "  %s: %" PRIuMAX " unique IDs",
                         metadata->name,
                         (uintmax_t)count);
        any = true;
    }

    for (metric_keyed_id_t id = 0; id < METRIC_KEYED_COUNT; id++) {
        const metric_keyed_metadata_t *metadata = metrics_keyed_metadata(id);
        size_t count = metrics_keyed_count(store, id);
        if (metadata == NULL || metadata->scope != store->scope || count == 0 ||
            (category != NULL && strcasecmp(metadata->category, category) != 0)) {
            continue;
        }
        draw_info_format(COLOR_WHITE,
                         op,
                         "  %s: %" PRIuMAX " keyed IDs",
                         metadata->name,
                         (uintmax_t)count);
        any = true;
    }

    if (!any) {
        draw_info(COLOR_WHITE, op, "  No recorded values in this view.");
    }
}

/** @copydoc command_func */
void command_metrics(object *op, const char *command, char *params) {
    char target_name[MAX_BUF], scope[MAX_BUF], category[MAX_BUF];
    size_t pos = 0;
    const char *args = params != NULL ? params : "";

    if (!string_get_word(args, &pos, ' ', VS(target_name), 0)) {
        draw_info(COLOR_WHITE, op, "Usage: /metrics <player> [character|account|all] [category]");
        return;
    }
    string_get_word(args, &pos, ' ', VS(scope), 0);
    string_get_word(args, &pos, ' ', VS(category), 0);
    if (*scope == '\0') {
        snprintf(VS(scope), "all");
    }
    if (strcasecmp(scope, "character") != 0 && strcasecmp(scope, "account") != 0 &&
        strcasecmp(scope, "all") != 0) {
        draw_info(COLOR_WHITE, op, "Scope must be character, account, or all.");
        return;
    }

    player *target = find_player(target_name);
    if (target == NULL || target->cs == NULL || target->cs->state != ST_PLAYING) {
        draw_info(COLOR_WHITE, op, "That character is not online.");
        return;
    }

    LOG(SYSTEM,
        "Operator %s inspected %s metrics for character %s (account %s)",
        op->name,
        scope,
        target->ob->name,
        target->cs->account);
    const char *filter = *category != '\0' ? category : NULL;
    if (strcasecmp(scope, "character") == 0 || strcasecmp(scope, "all") == 0) {
        metrics_character_session_checkpoint(target);
        metrics_draw_store(op, &target->metrics, target->ob->name, filter);
    }
    if (strcasecmp(scope, "account") == 0 || strcasecmp(scope, "all") == 0) {
        metric_store_t account_metrics;
        metrics_store_init(&account_metrics, METRIC_SCOPE_ACCOUNT, 0);
        if (!account_metrics_load(target->cs->account, &account_metrics)) {
            draw_info(COLOR_RED, op, "The target account metrics could not be loaded.");
        } else {
            metrics_draw_store(op, &account_metrics, target->cs->account, filter);
        }
        metrics_store_free(&account_metrics);
    }
}
