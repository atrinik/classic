/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Functions dealing with shop handling, bargaining, etc.
 */

#include <global.h>
#include <shop.h>
#include <gameplay_journal.h>
#include <server_main.h>
#include <server_item.h>
#include <server.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <arch.h>
#include <metrics.h>
#include <player.h>

/**
 * Calculate the price of an item.
 * @param tmp
 * Object we're querying the price of.
 * @param mode
 * One of @ref COST_xxx.
 * @return
 * The price of the item.
 */
int64_t shop_get_cost(object *op, int mode) {
    HARD_ASSERT(op != NULL);

    SOFT_ASSERT_RC(op->arch != NULL, 0, "Object has no archetype: %s", object_get_str(op));

    uint32_t nrof = MAX(1, op->nrof);
    /* Money is always identified */
    if (op->type == MONEY) {
        return op->value * nrof;
    }

    int64_t val;

    if (QUERY_FLAG(op, FLAG_IDENTIFIED) || !need_identify(op)) {
        /* Handle identified items */
        if (QUERY_FLAG(op, FLAG_CURSED) || QUERY_FLAG(op, FLAG_DAMNED)) {
            /* Cursed or damned items have no value at all. */
            return 0;
        } else {
            val = op->value * nrof;
        }
    } else {
        /* This area deals with objects that are not identified, but can be. */
        if (mode == COST_BUY) {
            log_error("Asking for buy value of unidentified object: %s", object_get_str(op));
            val = op->arch->clone.value * nrof;
        } else {
            /* Trying to sell something, or get true value. */
            if (op->type == GEM || op->type == JEWEL || op->type == NUGGET || op->type == PEARL) {
                val = 3 * nrof;
            } else if (op->type == POTION) {
                val = 50 * nrof;
            } else {
                val = op->arch->clone.value * nrof;
            }
        }
    }

    /* Handle spell tool items. */
    if (OBJECT_IS_SPELL_TOOL(op)) {
        int spell = SP_NO_SPELL;
        if (op->stats.sp > SP_NO_SPELL && op->stats.sp < NROFREALSPELLS) {
            spell = op->stats.sp;
        }

        /* If we have a valid spell, increase the value using the value
         * multiplier of the spell. */
        if (spell != SP_NO_SPELL) {
            val *= spells[spell].value_mul;
        }

        int level = op->level;
        if (op->type == BOOK_SPELL) {
            /* Spell books don't have a level, so we need to use the base
             * level of the spell instead. */
            level = spells[spell].at->clone.level;
        }

        int level_value = val * level;
        if (op->type == WAND) {
            /* Wands have increased value for each charge they hold. */
            level_value *= op->stats.food;
        }

        val += level_value;

        /* For spell books, add the base value of the spell as well. */
        if (op->type == BOOK_SPELL && spell != SP_NO_SPELL) {
            val += spells[spell].at->clone.value;
        }
    }

    /* We are done if we only want get the real value. */
    if (mode == COST_TRUE) {
        return val;
    }

    double diff;
    /* Now adjust for sell or buy multiplier. */
    if (mode == COST_BUY) {
        diff = 1.0;
    } else {
        diff = 0.20;
    }

    val = (int64_t)(val * diff);
    if (val < 1 && op->value > 0) {
        val = 1;
    }

    return val;
}

/**
 * Find the coin type that is worth more than 'cost'. Starts at the 'cointype'
 * placement.
 * @param cost
 * Value we're searching.
 * @param[in,out] cointype First coin type to search. Will contain the next
 * coin ID.
 * @return
 * Coin archetype, NULL if none found.
 */
static archetype_t *shop_get_next_coin(int64_t cost, int *cointype) {
    archetype_t *coin;

    do {
        if (coins[*cointype] == NULL) {
            return NULL;
        }

        coin = arch_find(coins[*cointype]);
        if (coin == NULL) {
            return NULL;
        }

        *cointype += 1;
    } while (coin->clone.value > cost);

    return coin;
}

/**
 * Converts a price to number of coins.
 * @param cost
 * Value to transform to currency.
 * @return
 * Static buffer containing the price. Will be overwritten with the next
 * call to this function.
 */
const char *shop_get_cost_string(int64_t cost) {
    static char buf[HUGE_BUF];

    int cointype = 0;
    archetype_t *coin = shop_get_next_coin(cost, &cointype);
    if (coin == NULL) {
        return "nothing";
    }

    int64_t num = cost / coin->clone.value;
    cost -= num * coin->clone.value;

    snprintf(VS(buf),
             "%" PRId64 " %s%s%s",
             num,
             materials_real[coin->clone.material_real].name,
             coin->clone.name,
             num == 1 ? "" : "s");

    archetype_t *next_coin = shop_get_next_coin(cost, &cointype);
    if (next_coin == NULL) {
        return buf;
    }

    do {
        coin = next_coin;
        num = cost / coin->clone.value;
        cost -= num * coin->clone.value;

        if (cost == 0) {
            next_coin = NULL;
        } else {
            next_coin = shop_get_next_coin(cost, &cointype);
        }

        if (next_coin != NULL) {
            /* There will be at least one more string to add to the list,
             * use a comma. */
            snprintfcat(VS(buf), ", ");
        } else {
            snprintfcat(VS(buf), " and ");
        }

        snprintfcat(VS(buf),
                    "%" PRId64 " %s%s%s",
                    num,
                    materials_real[coin->clone.material_real].name,
                    coin->clone.name,
                    num == 1 ? "" : "s");
    } while (next_coin != NULL);

    return buf;
}

/**
 * Query the cost of an item.
 *
 * This is really a wrapper for shop_get_cost_string() and shop_get_cost().
 * @param op
 * Object we're querying the price of.
 * @param mode
 * One of @ref COST_xxx.
 * @return
 * The cost string.
 */
const char *shop_get_cost_string_item(object *op, int mode) {
    return shop_get_cost_string(shop_get_cost(op, mode));
}

/**
 * Finds out how much money the player is carrying, including what is in
 * containers and in bank.
 * @param op
 * Item to get money for. Must be a player or a container.
 * @return
 * Total money the player is carrying.
 */
static bool shop_get_money_checked(object *op, int64_t *total) {
    HARD_ASSERT(op != NULL);
    FOR_INV_PREPARE(op, tmp) {
        if (tmp->type == MONEY) {
            int denomination = -1;
            for (int i = 0; i < NUM_COINS; i++) {
                if (strcmp(coins[i], tmp->arch->name) == 0 &&
                    tmp->value == tmp->arch->clone.value) {
                    denomination = i;
                    break;
                }
            }
            int64_t nrof = tmp->nrof;
            if (denomination < 0 || nrof == 0) {
                continue;
            }
            if (tmp->value <= 0 || tmp->value > INT64_MAX / nrof ||
                *total > INT64_MAX - tmp->value * nrof) {
                return false;
            }
            *total += tmp->value * nrof;
        } else if (tmp->type == CONTAINER && (QUERY_FLAG(tmp, FLAG_APPLIED) || tmp->race == NULL ||
                                              strstr(tmp->race, "gold") != NULL)) {
            if (!shop_get_money_checked(tmp, total)) {
                return false;
            }
        } else if (tmp->arch->name == shstr_cons.player_info &&
                   tmp->name == shstr_cons.BANK_GENERAL) {
            if (tmp->value < 0 || *total > INT64_MAX - tmp->value) {
                return false;
            }
            *total += tmp->value;
        }
    }
    FOR_INV_FINISH();

    return true;
}

int64_t shop_get_money(object *op) {
    HARD_ASSERT(op != NULL);
    SOFT_ASSERT_RC(op->type == PLAYER || op->type == CONTAINER,
                   0,
                   "Called with invalid object type: %s",
                   object_get_str(op));

    int64_t total = 0;
    SOFT_ASSERT_RC(shop_get_money_checked(op, &total),
                   0,
                   "Currency total overflow for object: %s",
                   object_get_str(op));
    return total;
}

bool shop_get_recovery_money(object *op, int64_t *total) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(total != NULL);
    *total = 0;
    if (!shop_get_money_checked(op, total)) {
        return false;
    }
    if (op->map == NULL) {
        return true;
    }
    FOR_MAP_PREPARE(op->map, op->x, op->y, tmp) {
        if (tmp->type != MONEY || tmp->arch == NULL || tmp->nrof == 0) {
            continue;
        }
        bool canonical = false;
        for (int i = 0; i < NUM_COINS; i++) {
            if (strcmp(coins[i], tmp->arch->name) == 0 && tmp->value == tmp->arch->clone.value) {
                canonical = true;
                break;
            }
        }
        int64_t nrof = tmp->nrof;
        if (!canonical || tmp->value <= 0 || tmp->value > INT64_MAX / nrof ||
            *total > INT64_MAX - tmp->value * nrof) {
            return false;
        }
        *total += tmp->value * nrof;
    }
    FOR_MAP_FINISH();
    return true;
}

/**
 * Pays the specified amount, taking the proper amount of money from the
 * object's inventory.
 * @param obj
 * Object to remove the money for.
 * @param to_pay
 * Required amount.
 * @return
 * Amount still left to pay.
 */
static int64_t shop_pay_inventory(object *obj, int64_t to_pay) {
    int64_t remain = to_pay;

    object *coins_objects[NUM_COINS];
    for (int i = 0; i < NUM_COINS; i++) {
        coins_objects[i] = NULL;
    }

    /* Remove all the money objects from the container, and store them in
     * the coin_objs pointers array. */
    FOR_INV_PREPARE(obj, tmp) {
        if (tmp->type != MONEY) {
            continue;
        }

        bool found = false;
        for (int i = 0; i < NUM_COINS; i++) {
            if (strcmp(coins[NUM_COINS - 1 - i], tmp->arch->name) != 0 ||
                tmp->value != tmp->arch->clone.value) {
                continue;
            }

            object_remove(tmp, 0);
            tmp->below = coins_objects[i];
            coins_objects[i] = tmp;

            found = true;
            break;
        }

        if (!found) {
            log_error("Did not find coin match for %s", tmp->arch->name);
        }
    }
    FOR_INV_FINISH();

    for (int i = 0; i < NUM_COINS; i++) {
        while (coins_objects[i] != NULL && remain > 0) {
            object *coin = coins_objects[i];
            coins_objects[i] = coin->below;
            coin->below = NULL;
            int64_t stack_value = (int64_t)coin->nrof * coin->value;
            uint32_t num_coins = coin->nrof;
            if (stack_value > remain) {
                int64_t needed = remain / coin->value;
                if (needed * coin->value < remain) {
                    needed++;
                }
                num_coins = (uint32_t)needed;
            }
            remain -= (int64_t)num_coins * coin->value;
            coin->nrof -= num_coins;
            if (coin->nrof == 0) {
                object_destroy(coin);
            } else {
                coin->below = coins_objects[i];
                coins_objects[i] = coin;
            }

            int count = i - 1;
            while (remain < 0 && count >= 0) {
                archetype_t *at = arch_find(coins[NUM_COINS - 1 - count]);
                HARD_ASSERT(at != NULL && at->clone.value > 0);
                int64_t change = -remain / at->clone.value;
                while (change > 0) {
                    uint32_t chunk = (uint32_t)MIN(change, UINT32_MAX);
                    object *replacement = arch_get(coins[NUM_COINS - 1 - count]);
                    replacement->nrof = chunk;
                    replacement->below = coins_objects[count];
                    coins_objects[count] = replacement;
                    change -= chunk;
                    remain += (int64_t)chunk * replacement->value;
                }
                count--;
            }
        }
    }

    for (int i = 0; i < NUM_COINS; i++) {
        while (coins_objects[i] != NULL) {
            object *coin = coins_objects[i];
            coins_objects[i] = coin->below;
            coin->below = NULL;
            object_insert_into(coin, obj, 0);
        }
    }

    return remain;
}

/**
 * Recursively attempts to pay the specified amount of money.
 * @param op
 * Who is paying.
 * @param to_pay
 * Amount to pay.
 * @return
 * Amount left to pay.
 */
static int64_t shop_pay_amount(object *op, int64_t to_pay) {
    to_pay = shop_pay_inventory(op, to_pay);
    FOR_INV_PREPARE(op, tmp) {
        if (to_pay <= 0) {
            break;
        }

        if (tmp->type != CONTAINER || tmp->inv == NULL) {
            continue;
        }

        if (QUERY_FLAG(tmp, FLAG_APPLIED) || tmp->race == NULL ||
            strstr(tmp->race, "gold") != NULL) {
            to_pay = shop_pay_amount(tmp, to_pay);
        }
    }
    FOR_INV_FINISH();

    return to_pay;
}

/**
 * Pays the specified amount of money.
 * @param op
 * Object paying.
 * @param to_pay
 * Amount to pay.
 * @return
 * False if not enough money, in which case nothing is removed, true
 * if money was removed.
 */
static object_semantic_result_t
shop_pay_internal(object *op, int64_t to_pay, const char *reason, object *item, bool *soulbound) {
    if (soulbound != NULL) {
        *soulbound = false;
    }
    if (to_pay < 0) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (to_pay == 0 && item == NULL) {
        return OBJECT_SEMANTIC_COMMITTED;
    }

    int64_t available = 0;
    if (!shop_get_money_checked(op, &available) || to_pay > available) {
        return OBJECT_SEMANTIC_FAILED;
    }

    int64_t amount = to_pay;
    int64_t total_before = available;
    int64_t bank_before = bank_get_balance(op);
    int64_t carried_before = total_before - bank_before;
    int64_t bank_used = MAX(0, amount - carried_before);
    const char *funding = bank_used == 0 ? "carried-cash" : bank_used == amount ? "bank" : "mixed";
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE] = "";
    object_custody_transaction_t item_transaction = {0};
    bool journal_payment = op->type == PLAYER;
    bool item_journal = journal_payment && item != NULL;
    if (item_journal) {
        if (!object_custody_begin_economy(item,
                                          op,
                                          reason,
                                          "shop-service",
                                          "player",
                                          "shop-service",
                                          MAX(1, item->nrof),
                                          true,
                                          false,
                                          bank_used != 0 ? bank_before : total_before,
                                          -(bank_used != 0 ? bank_used : amount),
                                          bank_used != 0 ? bank_before - bank_used
                                                         : total_before - amount,
                                          amount,
                                          "copper-equivalent",
                                          funding,
                                          &item_transaction)) {
            return OBJECT_SEMANTIC_FAILED;
        }
    } else if (journal_payment &&
               !gameplay_journal_currency_begin_economy(op,
                                                        reason,
                                                        "currency:payment",
                                                        bank_used != 0 ? bank_before : total_before,
                                                        -(bank_used != 0 ? bank_used : amount),
                                                        bank_used != 0 ? bank_before - bank_used
                                                                       : total_before - amount,
                                                        funding,
                                                        "service",
                                                        funding,
                                                        amount,
                                                        transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    to_pay = shop_pay_amount(op, to_pay);
    if (to_pay != 0) {
        object *bank = bank_find_info(op);
        if (bank != NULL) {
            HARD_ASSERT(bank->value >= to_pay);
            bank->value = bank_before - to_pay;
            to_pay = 0;
        }
    }

    HARD_ASSERT(to_pay == 0);

    bool committed = true;
    if (item_journal) {
        CLEAR_FLAG(item, FLAG_UNPAID);
        CLEAR_FLAG(item, FLAG_STARTEQUIP);
        if (QUERY_FLAG(item, FLAG_SOULBOUND)) {
            if (object_set_value(item, "soulbound_name", op->name, true)) {
                if (soulbound != NULL) {
                    *soulbound = true;
                }
            } else {
                CLEAR_FLAG(item, FLAG_SOULBOUND);
                LOG(ERROR, "Failed to soulbind %s to %s", object_get_str(item), object_get_str(op));
            }
        }
        object_custody_apply(item, &item_transaction);
        object *survivor = QUERY_FLAG(item, FLAG_REMOVED) ? item : object_merge(item);
        committed = object_custody_finish(&item_transaction);
        if (committed && survivor == item && !QUERY_FLAG(item, FLAG_REMOVED)) {
            esrv_update_item(UPD_FLAGS, item);
        }
    } else if (journal_payment) {
        committed = gameplay_journal_semantic_commit(transaction);
    }
    if (!committed) {
        return OBJECT_SEMANTIC_AMBIGUOUS;
    }
    if (op->type == PLAYER) {
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_CURRENCY_SPENT, (uint64_t)amount);
    }

    return OBJECT_SEMANTIC_COMMITTED;
}

object_semantic_result_t shop_pay_reason(object *op, int64_t to_pay, const char *reason) {
    HARD_ASSERT(reason != NULL);
    return shop_pay_internal(op, to_pay, reason, NULL, NULL);
}

bool shop_pay(object *op, int64_t to_pay) {
    object_semantic_result_t result = shop_pay_reason(op, to_pay, "script.payment");
    /* Preserve the legacy bool contract: false always means no funds changed.
     * A post-mutation terminal failure must fail-stop without returning. */
    HARD_ASSERT(result != OBJECT_SEMANTIC_AMBIGUOUS);
    return result == OBJECT_SEMANTIC_COMMITTED;
}

/**
 * Attempts to pay for the specified item.
 * @param op
 * Object buying.
 * @param item
 * Item to buy.
 * @return
 * Whether the object was purchased successfully (and money removed).
 */
bool shop_pay_item(object *op, object *item) {
    int64_t cost = shop_get_cost(item, COST_BUY);
    object_semantic_result_t result = shop_pay_internal(op, cost, "shop.purchase", item, NULL);
    HARD_ASSERT(result != OBJECT_SEMANTIC_AMBIGUOUS);
    if (result != OBJECT_SEMANTIC_COMMITTED) {
        return false;
    }
    if (op->type == PLAYER && cost > 0) {
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_SHOP_PURCHASES, 1);
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_SHOP_CURRENCY_SPENT, (uint64_t)cost);
    }
    return true;
}

/**
 * Recursively pay for items in inventories. Used by shop_pay_items().
 * @param op
 * Object buying the stuff.
 * @param where
 * Where to look.
 * @return
 * True if everything has been paid for, false otherwise.
 */
static bool shop_pay_items_rec(object *op, object *where) {
    FOR_INV_PREPARE(where, tmp) {
        if (tmp->inv != NULL) {
            if (!shop_pay_items_rec(op, tmp)) {
                return false;
            }
        }

        if (!QUERY_FLAG(tmp, FLAG_UNPAID)) {
            continue;
        }

        char *name = object_get_name_s(tmp, op);
        int64_t cost = shop_get_cost(tmp, COST_BUY);
        bool soulbound = false;
        object_semantic_result_t result =
            shop_pay_internal(op, cost, "shop.purchase", tmp, &soulbound);
        if (result == OBJECT_SEMANTIC_FAILED) {
            CLEAR_FLAG(tmp, FLAG_UNPAID);
            int64_t need = cost - shop_get_money(op);
            draw_info_format(COLOR_WHITE,
                             op,
                             "You lack %s to buy %s.",
                             shop_get_cost_string(need),
                             name);
            free(name);
            SET_FLAG(tmp, FLAG_UNPAID);
            return false;
        } else if (result == OBJECT_SEMANTIC_AMBIGUOUS) {
            draw_info(COLOR_WHITE,
                      op,
                      "The purchase completed, but its durable journal commit is uncertain.");
            free(name);
            return false;
        } else {
            draw_info_format(COLOR_WHITE,
                             op,
                             "You paid %s for %s.",
                             shop_get_cost_string(cost),
                             name);

            if (soulbound) {
                draw_info_format(COLOR_WHITE, op, "%s becomes soulbound to you.", name);
            }

            free(name);
            if (op->type == PLAYER && cost > 0) {
                metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_SHOP_PURCHASES, 1);
                metrics_add(&CONTR(op)->metrics,
                            METRIC_CHARACTER_SHOP_CURRENCY_SPENT,
                            (uint64_t)cost);
            }
        }
    }
    FOR_INV_FINISH();

    return true;
}

/**
 * Descends inventories looking for unpaid items, and pays for them.
 * @param op
 * Object buying the stuff.
 * @return
 * True if everything has been paid for, false otherwise.
 */
bool shop_pay_items(object *op) {
    return shop_pay_items_rec(op, op);
}

/**
 * Sell an item.
 * @param op
 * Who is selling the item.
 * @param item
 * The item to sell.
 */
bool shop_sell_item_begin(object *op,
                          object *item,
                          uint32_t quantity,
                          object_custody_transaction_t *transaction) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(item != NULL);
    HARD_ASSERT(transaction != NULL);

    object priced = *item;
    priced.nrof = quantity;
    int64_t value = shop_get_cost(&priced, COST_SELL);
    if (value < 0 || !shop_coins_available()) {
        return false;
    }
    int64_t before;
    if (!shop_get_recovery_money(op, &before) || value > INT64_MAX - before) {
        return false;
    }
    return object_custody_begin_economy(item,
                                        op,
                                        "shop.sale",
                                        "player",
                                        "shop-service",
                                        "shop-service",
                                        quantity,
                                        false,
                                        true,
                                        before,
                                        value,
                                        before + value,
                                        value,
                                        "copper-equivalent",
                                        "shop-service",
                                        transaction);
}

bool shop_sell_item_commit(object *op, object *item, object_custody_transaction_t *transaction) {
    HARD_ASSERT(op != NULL);

    if (item->custom_name) {
        FREE_AND_CLEAR_HASH(item->custom_name);
    }

    int64_t value = shop_get_cost(item, COST_SELL);
    char *name = object_get_name_s(item, op);
    HARD_ASSERT(shop_insert_coins_exact_tagged(op, value, transaction->transaction_id));

    SET_FLAG(item, FLAG_UNPAID);
    /* Identify the item. Makes any unidentified item sold to unique shop appear
     * identified. */
    identify(item);
    if (!object_custody_commit(item, transaction)) {
        free(name);
        return false;
    }
    shop_currency_tag_retire(op, transaction->transaction_id);
    if (value == 0) {
        draw_info_format(COLOR_WHITE, op, "We're not interested in %s.", name);
    } else {
        draw_info_format(COLOR_WHITE,
                         op,
                         "You receive %s for %s.",
                         shop_get_cost_string(value),
                         name);
    }
    free(name);
    if (op->type == PLAYER && value > 0) {
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_SHOP_SALES, 1);
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_SHOP_CURRENCY_EARNED, (uint64_t)value);
    }
    return true;
}

void shop_sell_item(object *op, object *item) {
    object_custody_transaction_t transaction;
    if (!shop_sell_item_begin(op, item, MAX(1, item->nrof), &transaction)) {
        return;
    }
    (void)shop_sell_item_commit(op, item, &transaction);
}

/**
 * Insert coins into an object.
 * @param op
 * Object to receive the coins.
 * @param value
 * Value of coins to insert (for example, 120 for 1 silver and 20
 * copper).
 */
static void shop_insert_coin_stacks(object *op,
                                    object *where,
                                    archetype_t *at,
                                    int64_t nrof,
                                    bool on_floor,
                                    const char *transaction_id) {
    while (nrof > 0) {
        /* Object counts are unsigned in memory, but serializers and merge
         * guards deliberately cap persistent stacks at INT32_MAX. */
        uint32_t chunk = (uint32_t)MIN(nrof, INT32_MAX);
        object *coin = object_get();
        object_copy(coin, &at->clone, false);
        coin->nrof = chunk;
        if (transaction_id != NULL && transaction_id[0] != '\0') {
            char lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
            snprintf(VS(lineage), "currency:%s", transaction_id);
            coin->custody_lineage = add_string(lineage);
        }
        if (on_floor) {
            coin->x = op->x;
            coin->y = op->y;
            HARD_ASSERT(object_insert_map(coin, op->map, NULL, INS_NO_WALK_ON) != NULL);
        } else {
            HARD_ASSERT(object_insert_into(coin, where, 0) != NULL);
        }
        nrof -= chunk;
    }
}

bool shop_coins_available(void) {
    for (int i = 0; coins[i] != NULL; i++) {
        archetype_t *at = arch_find(coins[i]);
        if (at == NULL || at->clone.value <= 0) {
            LOG(ERROR, "Could not use coin archetype: %s", coins[i]);
            return false;
        }
    }
    return true;
}

static void shop_currency_tag_retire_inventory(object *where, const char *lineage) {
    FOR_INV_PREPARE(where, tmp) {
        if (tmp->type == CONTAINER) {
            shop_currency_tag_retire_inventory(tmp, lineage);
        }
        if (tmp->type == MONEY && tmp->custody_lineage != NULL &&
            strcmp(tmp->custody_lineage, lineage) == 0) {
            FREE_AND_CLEAR_HASH(tmp->custody_lineage);
            (void)object_merge(tmp);
        }
    }
    FOR_INV_FINISH();
}

void shop_currency_tag_retire(object *op, const char *transaction_id) {
    if (op == NULL || transaction_id == NULL || transaction_id[0] == '\0') {
        return;
    }
    char lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
    snprintf(VS(lineage), "currency:%s", transaction_id);
    shop_currency_tag_retire_inventory(op, lineage);
    if (op->map != NULL) {
        FOR_MAP_PREPARE(op->map, op->x, op->y, tmp) {
            if (tmp->type == MONEY && tmp->custody_lineage != NULL &&
                strcmp(tmp->custody_lineage, lineage) == 0) {
                FREE_AND_CLEAR_HASH(tmp->custody_lineage);
                (void)object_merge(tmp);
            }
        }
        FOR_MAP_FINISH();
    }
}

bool shop_insert_coins_exact_tagged(object *op, int64_t value, const char *transaction_id) {
    HARD_ASSERT(op != NULL);
    if (value < 0 || !shop_coins_available()) {
        return false;
    }

    for (int i = 0; coins[i] != NULL; i++) {
        archetype_t *at = arch_find(coins[i]);

        if (value / at->clone.value <= 0) {
            continue;
        }

        FOR_INV_PREPARE(op, tmp) {
            if (tmp->type != CONTAINER) {
                continue;
            }

            if (!QUERY_FLAG(tmp, FLAG_APPLIED)) {
                continue;
            }

            if (tmp->race == NULL || strstr(tmp->race, "gold") == NULL) {
                continue;
            }

            int64_t nrof = value / at->clone.value;

            double weight = at->clone.weight * tmp->weapon_speed;
            if (tmp->weight_limit != 0 && tmp->carrying + weight > tmp->weight_limit) {
                continue;
            }

            if (weight > 0.0 && tmp->weight_limit != 0 &&
                (tmp->weight_limit - tmp->carrying) / weight < nrof) {
                nrof = MIN(nrof, (int64_t)((tmp->weight_limit - tmp->carrying) / weight));
            }

            shop_insert_coin_stacks(op, tmp, at, nrof, false, transaction_id);
            value -= nrof * at->clone.value;
        }
        FOR_INV_FINISH();

        if (value / at->clone.value > 0) {
            int64_t nrof = value / at->clone.value;
            uint32_t weight_max = weight_limit[MIN(op->stats.Str, MAX_STAT)];

            if (nrof > 0 && op->carrying <= weight_max &&
                at->clone.weight <= weight_max - op->carrying) {
                if (at->clone.weight > 0 && (weight_max - op->carrying) / at->clone.weight < nrof) {
                    nrof = MIN(nrof, (int64_t)((weight_max - op->carrying) / at->clone.weight));
                }

                shop_insert_coin_stacks(op, op, at, nrof, false, transaction_id);
                value -= nrof * at->clone.value;
            }
        }

        if (value / at->clone.value > 0) {
            int64_t nrof = value / at->clone.value;
            if (op->map != NULL) {
                shop_insert_coin_stacks(op, NULL, at, nrof, true, transaction_id);
            } else {
                shop_insert_coin_stacks(op, op, at, nrof, false, transaction_id);
            }
            value -= nrof * at->clone.value;
        }
    }

    return value == 0;
}

bool shop_insert_coins_exact(object *op, int64_t value) {
    return shop_insert_coins_exact_tagged(op, value, NULL);
}

void shop_insert_coins(object *op, int64_t value) {
    object_semantic_result_t result = shop_insert_coins_reason(op, value, "script.currency-grant");
    /* The legacy void API cannot expose a post-mutation ambiguous result. */
    HARD_ASSERT(result != OBJECT_SEMANTIC_AMBIGUOUS);
    SOFT_ASSERT(result != OBJECT_SEMANTIC_FAILED,
                "Could not insert exact value %" PRId64 " for object: %s",
                value,
                object_get_str(op));
}

object_semantic_result_t shop_insert_coins_reason(object *op, int64_t value, const char *reason) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(reason != NULL);
    if (value < 0) {
        return OBJECT_SEMANTIC_FAILED;
    }
    if (value == 0) {
        return OBJECT_SEMANTIC_COMMITTED;
    }
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE] = "";
    if (op->type == PLAYER) {
        int64_t before;
        if (!shop_get_recovery_money(op, &before) || value > INT64_MAX - before ||
            !gameplay_journal_currency_begin(op,
                                             reason,
                                             "currency:grant",
                                             before,
                                             value,
                                             before + value,
                                             "service",
                                             "player-or-ground",
                                             "generated",
                                             transaction)) {
            return OBJECT_SEMANTIC_FAILED;
        }
    }
    if (!shop_insert_coins_exact_tagged(op, value, transaction)) {
        if (op->type == PLAYER) {
            (void)gameplay_journal_abort(transaction, "coin-materialization-failed");
        }
        return OBJECT_SEMANTIC_FAILED;
    }
    if (op->type == PLAYER && !gameplay_journal_semantic_commit(transaction)) {
        return OBJECT_SEMANTIC_AMBIGUOUS;
    }
    shop_currency_tag_retire(op, transaction);
    return OBJECT_SEMANTIC_COMMITTED;
}
