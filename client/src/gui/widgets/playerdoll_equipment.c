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

/** @file Player-doll equipment slot selection policy. */

#include "playerdoll_equipment.h"
#include <item.h>

object *playerdoll_equipment_resolve(int slot, const tag_t equipment[PLAYER_EQUIP_MAX]) {
    object *obj = equipment[slot] == 0 ? NULL : object_find(equipment[slot]);

    if (slot == PLAYER_EQUIP_SHIELD) {
        object *weapon = NULL;

        if (equipment[PLAYER_EQUIP_WEAPON_RANGED] != 0) {
            weapon = object_find(equipment[PLAYER_EQUIP_WEAPON_RANGED]);
        } else if (obj != NULL && equipment[PLAYER_EQUIP_WEAPON] != 0) {
            weapon = object_find(equipment[PLAYER_EQUIP_WEAPON]);
        }

        if (weapon != NULL && weapon->flags & CS_FLAG_WEAPON_2H) {
            obj = weapon;
        }
    } else if (slot == PLAYER_EQUIP_WEAPON_RANGED && obj != NULL &&
               obj->flags & CS_FLAG_WEAPON_2H) {
        return NULL;
    }

    return obj;
}
