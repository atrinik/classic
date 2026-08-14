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

#ifndef SHOP_H
#define SHOP_H

#include <decls.h>

struct object_custody_transaction;

/**
 * @file
 * Public declarations for the corresponding server module.
 */

/** Public API implemented in src/server/shop.c. */

extern int64_t shop_get_cost(object *op, int mode);

extern const char *shop_get_cost_string(int64_t cost);

extern const char *shop_get_cost_string_item(object *op, int flag);

extern int64_t shop_get_money(object *op);

extern bool shop_pay(object *op, int64_t to_pay);

/** Reason-aware payment; AMBIGUOUS means funds changed but terminal sync failed. */
extern object_semantic_result_t shop_pay_reason(object *op, int64_t to_pay, const char *reason);

extern bool shop_pay_item(object *op, object *item);

extern bool shop_pay_items(object *op);

extern void shop_sell_item(object *op, object *item);

extern bool shop_sell_item_begin(object *op,
                                 object *item,
                                 uint32_t quantity,
                                 struct object_custody_transaction *transaction);

extern bool
shop_sell_item_commit(object *op, object *item, struct object_custody_transaction *transaction);

extern int64_t bank_get_balance(object *op);

extern object *bank_find_info(object *op);

extern int bank_deposit(object *op, const char *text, int64_t *value);

extern int bank_withdraw(object *op, const char *text, int64_t *value);

extern void shop_insert_coins(object *op, int64_t value);
extern bool shop_insert_coins_exact(object *op, int64_t value);
extern bool shop_insert_coins_exact_tagged(object *op, int64_t value, const char *transaction_id);
/**
 * Get the recovery aggregate spanning player-held/bank currency and canonical
 * currency on the player's current delivery tile.
 */
extern bool shop_get_recovery_money(object *op, int64_t *total);
extern bool shop_get_held_money(object *op, int64_t *total);
extern bool shop_get_tile_money(object *op, int64_t *total);
extern bool shop_money_object_value(const object *money, int64_t *value);
extern bool shop_money_object_counted(const object *root, const object *money);
/**
 * Retire transaction lineage only after its terminal commit is durable.
 * Matching coin stacks may merge, so all pointers to them are invalid afterward.
 */
extern void shop_currency_tag_retire(object *op, const char *transaction_id);
extern bool shop_coins_available(void);

extern object_semantic_result_t
shop_insert_coins_reason(object *op, int64_t value, const char *reason);

#endif
