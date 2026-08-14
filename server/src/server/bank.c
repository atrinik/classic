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
 * Bank related code.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <shop.h>
#include <server_main.h>
#include <server.h>
#include <initialization.h>
#include <toolkit/string.h>
#include <arch.h>
#include <player.h>
#include <object.h>
#include <gameplay_journal.h>

/**
 * @defgroup BANK_STRING_xxx Bank info string modes
 * Modes used for #bank_info_t and bank_parse_string().
 *@{*/
/** Invalid string (did not include any valid amount). */
#define BANK_STRING_NONE 0
/** Got a valid amount of money from string. */
#define BANK_STRING_AMOUNT 1
/** The string was "all". */
#define BANK_STRING_ALL -1
/*@}*/

/**
 * Used for depositing/withdrawing coins from the bank, and using string
 * to get information about how many coins to deposit/withdraw.
 */
typedef struct bank_info {
    /** One of @ref BANK_STRING_xxx. */
    int mode;

    /** Number of amber coins. */
    uint64_t amber;

    /** Number of mithril coins. */
    uint64_t mithril;

    /** Number of jade coins. */
    uint64_t jade;

    /** Number of gold coins. */
    uint64_t gold;

    /** Number of silver coins. */
    uint64_t silver;

    /** Number of copper coins. */
    uint64_t copper;
} bank_info_t;

/**
 * Parse a string into the bank into structure.
 * @param str
 * Text to get money from.
 * @param info
 * Bank info structure.
 */
static void bank_parse_string(const char *str, bank_info_t *info) {
    memset(info, 0, sizeof(*info));

    while (isspace(*str)) {
        str++;
    }

    /* Easy, special case: all money */
    if (strncasecmp(str, "all", 3) == 0) {
        info->mode = BANK_STRING_ALL;
        return;
    }

    info->mode = BANK_STRING_NONE;

    char word[MAX_BUF];
    size_t pos = 0;
    while (string_get_word(str, &pos, ' ', VS(word), 0)) {
        if (!string_isdigit(word)) {
            continue;
        }

        errno = 0;
        char *end;
        uintmax_t parsed = strtoumax(word, &end, 10);
        if (errno != 0 || *end != '\0') {
            memset(info, 0, sizeof(*info));
            info->mode = BANK_STRING_NONE;
            return;
        }
        uint64_t value = (uint64_t)parsed;

        if (!string_get_word(str, &pos, ' ', VS(word), 0)) {
            continue;
        }

        size_t len = strlen(word);
        uint64_t *amount = NULL;
        if (strncasecmp("amber", word, len) == 0) {
            amount = &info->amber;
        } else if (strncasecmp("mithril", word, len) == 0) {
            amount = &info->mithril;
        } else if (strncasecmp("jade", word, len) == 0) {
            amount = &info->jade;
        } else if (strncasecmp("gold", word, len) == 0) {
            amount = &info->gold;
        } else if (strncasecmp("silver", word, len) == 0) {
            amount = &info->silver;
        } else if (strncasecmp("copper", word, len) == 0) {
            amount = &info->copper;
        }
        if (amount != NULL) {
            if (*amount > UINT64_MAX - value) {
                memset(info, 0, sizeof(*info));
                info->mode = BANK_STRING_NONE;
                return;
            }
            info->mode = BANK_STRING_AMOUNT;
            *amount += value;
        }
    }
}

static bool bank_info_value(const bank_info_t *info, int64_t *value) {
    const uint64_t counts[NUM_COINS] =
        {info->amber, info->mithril, info->jade, info->gold, info->silver, info->copper};
    *value = 0;
    for (int i = 0; i < NUM_COINS; i++) {
        int64_t coin_value = coins_arch[i]->clone.value;
        if (coin_value <= 0 || counts[i] > (uint64_t)(INT64_MAX / coin_value) ||
            *value > INT64_MAX - (int64_t)counts[i] * coin_value) {
            *value = 0;
            return false;
        }
        *value += (int64_t)counts[i] * coin_value;
    }
    return true;
}

/**
 * Get number of specific coins in the object's inventory.
 * @param op
 * Object to search in.
 * @param at
 * Archetype the coins must match.
 * @return
 * Number of coins in the object's inventory.
 */
static uint64_t bank_get_coins_num(object *op, archetype_t *at) {
    uint64_t num = 0;
    FOR_INV_PREPARE(op, tmp) {
        if (tmp->type == MONEY && tmp->arch == at && tmp->value == at->clone.value) {
            if (num > UINT64_MAX - tmp->nrof) {
                return UINT64_MAX;
            }
            num += tmp->nrof;
        } else if (tmp->type == CONTAINER &&
                   (tmp->race == NULL || strstr(tmp->race, "gold") != NULL)) {
            uint64_t nested = bank_get_coins_num(tmp, at);
            if (num > UINT64_MAX - nested) {
                return UINT64_MAX;
            }
            num += nested;
        }
    }
    FOR_INV_FINISH();

    return num;
}

static bool bank_coin_canonical(const object *coin) {
    for (int i = 0; i < NUM_COINS; i++) {
        if (coin->arch == coins_arch[i] && coin->value == coins_arch[i]->clone.value) {
            return true;
        }
    }
    return false;
}

/** Sum exactly the coin locations accepted by bank_remove_coins_internal(). */
static bool bank_get_deposit_value(object *op, int64_t *value) {
    FOR_INV_PREPARE(op, tmp) {
        if (tmp->type == MONEY) {
            int64_t nrof = tmp->nrof;
            if (nrof == 0) {
                continue;
            }
            if (!bank_coin_canonical(tmp) || tmp->value <= 0 || tmp->value > INT64_MAX / nrof ||
                *value > INT64_MAX - tmp->value * nrof) {
                return false;
            }
            *value += tmp->value * nrof;
        } else if (tmp->type == CONTAINER &&
                   (tmp->race == NULL || strstr(tmp->race, "gold") != NULL) &&
                   !bank_get_deposit_value(tmp, value)) {
            return false;
        }
    }
    FOR_INV_FINISH();
    return true;
}

/**
 * Remove money by the specified coin type.
 * @param op
 * Object we're removing from.
 * @param at
 * Archetype the coins must match.
 * @param nrof
 * Amount of money to remove. Has no effect if 'at' is NULL.
 * @return
 * Removed amount.
 */
static int64_t bank_remove_coins_internal(object *op, archetype_t *at, uint64_t *remaining) {
    int64_t amount = 0;

    FOR_INV_PREPARE(op, tmp) {
        if (at != NULL && *remaining == 0) {
            return amount;
        }

        if (tmp->type == MONEY && tmp->nrof != 0 &&
            (at == NULL ? bank_coin_canonical(tmp)
                        : (tmp->arch == at && tmp->value == at->clone.value))) {
            if (at == NULL || tmp->nrof <= *remaining) {
                if (at != NULL) {
                    *remaining -= tmp->nrof;
                }

                amount += tmp->nrof * tmp->value;
                object_remove(tmp, 0);
                object_destroy(tmp);
            } else {
                tmp->nrof -= *remaining;
                esrv_update_item(UPD_NAME | UPD_NROF, tmp);
                amount += *remaining * tmp->value;
                *remaining = 0;
            }
        } else if (tmp->type == CONTAINER &&
                   (tmp->race == NULL || strstr(tmp->race, "gold") != NULL)) {
            amount += bank_remove_coins_internal(tmp, at, remaining);
        }
    }
    FOR_INV_FINISH();

    return amount;
}

static int64_t bank_remove_coins(object *op, archetype_t *at, uint64_t nrof) {
    uint64_t remaining = nrof;
    return bank_remove_coins_internal(op, at, &remaining);
}

/**
 * Insert coins into the object.
 * @param op
 * Object.
 * @param at
 * Money arch to insert.
 * @param nrof
 * Number of coins.
 */
static void
bank_insert_coins(object *op, archetype_t *at, uint32_t nrof, const char *transaction_id) {
    object *tmp = object_get();
    object_copy(tmp, &at->clone, false);
    tmp->nrof = nrof;
    if (transaction_id != NULL && transaction_id[0] != '\0') {
        char lineage[GAMEPLAY_JOURNAL_ID_MAX + 1];
        snprintf(VS(lineage), "currency:%s", transaction_id);
        tmp->custody_lineage = add_string(lineage);
    }
    object_insert_into(tmp, op, 0);
}

/**
 * Find bank player info object in player's inventory.
 * @param op
 * Where to look for the player info object.
 * @return
 * The player info object if found, NULL otherwise.
 */
object *bank_find_info(object *op) {
    FOR_INV_PREPARE(op, tmp) {
        if (tmp->arch->name == shstr_cons.player_info && tmp->name == shstr_cons.BANK_GENERAL) {
            return tmp;
        }
    }
    FOR_INV_FINISH();

    return NULL;
}

/**
 * Create a new bank player info object and insert it to 'op'.
 * @param op
 * Player.
 * @return
 * The created player info object.
 */
static object *bank_create_info(object *op) {
    object *bank = arch_get(shstr_cons.player_info);

    FREE_AND_COPY_HASH(bank->name, shstr_cons.BANK_GENERAL);
    return object_insert_into(bank, op, 0);
}

/**
 * Convenience function to either find a bank player info object and if
 * not found, create a new one.
 * @param op
 * Player object.
 * @return
 * The bank player info object. Never NULL.
 */
static object *bank_get_info(object *op) {
    object *bank = bank_find_info(op);
    if (bank == NULL) {
        bank = bank_create_info(op);
    }
    return bank;
}

/**
 * Query how much money player has stored in bank.
 * @param op
 * Player to query for.
 * @return
 * The money stored.
 */
int64_t bank_get_balance(object *op) {
    HARD_ASSERT(op != NULL);

    object *bank = bank_find_info(op);
    if (bank == NULL) {
        return 0;
    }

    return bank->value;
}

object_semantic_result_t
bank_set_balance_reason(object *bank, int64_t value, const char *reason) {
    HARD_ASSERT(bank != NULL);
    HARD_ASSERT(reason != NULL);

    object *root = object_get_env(bank);
    if (root->type != PLAYER || bank->arch == NULL || bank->arch->name != shstr_cons.player_info ||
        bank->name != shstr_cons.BANK_GENERAL || bank->value < 0 || value < 0) {
        return OBJECT_SEMANTIC_FAILED;
    }
    int64_t before = bank->value;
    if (before == value) {
        return OBJECT_SEMANTIC_COMMITTED;
    }
    int64_t delta = value >= before ? value - before : -(before - value);
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE] = "";
    if (!gameplay_journal_currency_begin(root,
                                         reason,
                                         "currency:bank",
                                         before,
                                         delta,
                                         value,
                                         "hidden-bank",
                                         "hidden-bank",
                                         "script",
                                         transaction)) {
        return OBJECT_SEMANTIC_FAILED;
    }
    bank->value = value;
    return gameplay_journal_semantic_commit(transaction) ? OBJECT_SEMANTIC_COMMITTED
                                                         : OBJECT_SEMANTIC_AMBIGUOUS;
}

/**
 * Deposit money to player's bank object.
 * @param op
 * Player.
 * @param text
 * What was said to trigger this.
 * @param[out] value Will contain the deposited amount.
 * @return
 * One of @ref BANK_xxx.
 */
int bank_deposit(object *op, const char *text, int64_t *value) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(text != NULL);
    HARD_ASSERT(value != NULL);

    bank_info_t info;
    bank_parse_string(text, &info);
    *value = 0;

    if (info.mode == BANK_STRING_NONE) {
        return BANK_SYNTAX_ERROR;
    } else if (info.mode == BANK_STRING_ALL) {
        if (!bank_get_deposit_value(op, value)) {
            *value = 0;
            return BANK_JOURNAL_ERROR;
        }
    } else {
        if (info.amber != 0) {
            if (bank_get_coins_num(op, coins_arch[0]) < info.amber) {
                return BANK_DEPOSIT_AMBER;
            }
        }

        if (info.mithril != 0) {
            if (bank_get_coins_num(op, coins_arch[1]) < info.mithril) {
                return BANK_DEPOSIT_MITHRIL;
            }
        }

        if (info.jade != 0) {
            if (bank_get_coins_num(op, coins_arch[2]) < info.jade) {
                return BANK_DEPOSIT_JADE;
            }
        }

        if (info.gold != 0) {
            if (bank_get_coins_num(op, coins_arch[3]) < info.gold) {
                return BANK_DEPOSIT_GOLD;
            }
        }

        if (info.silver != 0) {
            if (bank_get_coins_num(op, coins_arch[4]) < info.silver) {
                return BANK_DEPOSIT_SILVER;
            }
        }

        if (info.copper != 0) {
            if (bank_get_coins_num(op, coins_arch[5]) < info.copper) {
                return BANK_DEPOSIT_COPPER;
            }
        }

        if (!bank_info_value(&info, value)) {
            return BANK_JOURNAL_ERROR;
        }
    }

    if (*value == 0) {
        return BANK_SUCCESS;
    }
    int64_t before = bank_get_balance(op);
    if (before < 0 || *value > INT64_MAX - before) {
        *value = 0;
        return BANK_JOURNAL_ERROR;
    }
    if (info.mode == BANK_STRING_ALL && !shop_coins_available()) {
        *value = 0;
        return BANK_JOURNAL_ERROR;
    }
    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    if (!gameplay_journal_currency_begin(op,
                                         "bank.deposit",
                                         "currency:bank",
                                         before,
                                         *value,
                                         before + *value,
                                         "carried-cash",
                                         "bank",
                                         "carried-cash",
                                         transaction)) {
        *value = 0;
        return BANK_JOURNAL_ERROR;
    }

    int64_t removed = 0;
    if (info.mode == BANK_STRING_ALL) {
        removed = bank_remove_coins(op, NULL, 0);
    } else {
        removed += bank_remove_coins(op, coins_arch[0], info.amber);
        removed += bank_remove_coins(op, coins_arch[1], info.mithril);
        removed += bank_remove_coins(op, coins_arch[2], info.jade);
        removed += bank_remove_coins(op, coins_arch[3], info.gold);
        removed += bank_remove_coins(op, coins_arch[4], info.silver);
        removed += bank_remove_coins(op, coins_arch[5], info.copper);
    }
    if (removed != *value) {
        (void)gameplay_journal_semantic_abort(transaction, "bank.deposit_failed");
        *value = 0;
        return BANK_JOURNAL_ERROR;
    }
    object *bank = bank_get_info(op);
    bank->value = before + *value;
    if (!gameplay_journal_semantic_commit(transaction)) {
        return BANK_JOURNAL_AMBIGUOUS;
    }

    if (op->type == PLAYER && *value > 0) {
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_BANK_DEPOSITS, 1);
        metrics_add(&CONTR(op)->metrics,
                    METRIC_CHARACTER_BANK_CURRENCY_DEPOSITED,
                    (uint64_t)*value);
    }
    return BANK_SUCCESS;
}

/**
 * Withdraw money player previously stored in bank object.
 * @param op
 * Player.
 * @param bank
 * Bank object in player's inventory.
 * @param text
 * What was said to trigger this.
 * @param[out] value Will contain the withdrawn amount.
 * @return
 * One of @ref BANK_xxx.
 */
int bank_withdraw(object *op, const char *text, int64_t *value) {
    HARD_ASSERT(op != NULL);
    HARD_ASSERT(text != NULL);
    HARD_ASSERT(value != NULL);

    bank_info_t info;
    bank_parse_string(text, &info);

    object *bank = bank_find_info(op);
    *value = 0;

    if (bank == NULL || bank->value == 0) {
        return BANK_WITHDRAW_MISSING;
    }
    if (bank->value < 0) {
        return BANK_JOURNAL_ERROR;
    }

    if (info.mode == BANK_STRING_NONE) {
        return BANK_SYNTAX_ERROR;
    } else if (info.mode == BANK_STRING_ALL) {
        *value = bank->value;
    } else {
        if (info.amber > 100000 || info.mithril > 100000 || info.jade > 100000 ||
            info.gold > 100000 || info.silver > 1000000 || info.copper > 1000000) {
            return BANK_WITHDRAW_HIGH;
        }

        int64_t big_value;
        if (!bank_info_value(&info, &big_value)) {
            return BANK_WITHDRAW_HIGH;
        }

        if (big_value > bank->value) {
            return BANK_WITHDRAW_MISSING;
        }

        if (!player_can_carry(op,
                              info.amber * coins_arch[0]->clone.weight +
                                  info.mithril * coins_arch[1]->clone.weight +
                                  info.jade * coins_arch[2]->clone.weight +
                                  info.gold * coins_arch[3]->clone.weight +
                                  info.silver * coins_arch[4]->clone.weight +
                                  info.copper * coins_arch[5]->clone.weight)) {
            return BANK_WITHDRAW_OVERWEIGHT;
        }

        *value = big_value;
    }

    char transaction[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    if (!gameplay_journal_currency_begin(op,
                                         "bank.withdraw",
                                         "currency:bank",
                                         bank->value,
                                         -*value,
                                         bank->value - *value,
                                         "bank",
                                         info.mode == BANK_STRING_ALL ? "player-or-ground"
                                                                      : "carried-cash",
                                         "bank",
                                         transaction)) {
        *value = 0;
        return BANK_JOURNAL_ERROR;
    }

    bank->value -= *value;
    if (info.mode == BANK_STRING_ALL) {
        HARD_ASSERT(shop_insert_coins_exact_tagged(op, *value, transaction));
    } else {
        if (info.amber != 0) {
            bank_insert_coins(op, coins_arch[0], (uint32_t)info.amber, transaction);
        }
        if (info.mithril != 0) {
            bank_insert_coins(op, coins_arch[1], (uint32_t)info.mithril, transaction);
        }
        if (info.jade != 0) {
            bank_insert_coins(op, coins_arch[2], (uint32_t)info.jade, transaction);
        }
        if (info.gold != 0) {
            bank_insert_coins(op, coins_arch[3], (uint32_t)info.gold, transaction);
        }
        if (info.silver != 0) {
            bank_insert_coins(op, coins_arch[4], (uint32_t)info.silver, transaction);
        }
        if (info.copper != 0) {
            bank_insert_coins(op, coins_arch[5], (uint32_t)info.copper, transaction);
        }
    }
    if (!gameplay_journal_semantic_commit(transaction)) {
        return BANK_JOURNAL_AMBIGUOUS;
    }
    shop_currency_tag_retire(op, transaction);

    if (op->type == PLAYER && *value > 0) {
        metrics_add(&CONTR(op)->metrics, METRIC_CHARACTER_BANK_WITHDRAWALS, 1);
        metrics_add(&CONTR(op)->metrics,
                    METRIC_CHARACTER_BANK_CURRENCY_WITHDRAWN,
                    (uint64_t)*value);
    }
    return BANK_SUCCESS;
}
