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

/** @file Player-doll equipment slot selection regression tests. */

#include <global.h>

#include "../gui/widgets/playerdoll_equipment.h"

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

enum {
    TAG_RANGED = 1,
    TAG_SHIELD,
    TAG_MELEE,
    TAG_STALE,
};

static object ranged;
static object shield;
static object melee;

object *object_find(tag_t tag) {
    switch (tag) {
        case TAG_RANGED:
            return &ranged;
        case TAG_SHIELD:
            return &shield;
        case TAG_MELEE:
            return &melee;
        default:
            return NULL;
    }
}

static void reset_objects(void) {
    ranged = (object){.tag = TAG_RANGED};
    shield = (object){.tag = TAG_SHIELD};
    melee = (object){.tag = TAG_MELEE};
}

static void test_two_handed_ranged_uses_empty_shield_slot(void) {
    tag_t equipment[PLAYER_EQUIP_MAX] = {0};
    equipment[PLAYER_EQUIP_WEAPON_RANGED] = TAG_RANGED;
    ranged.flags = CS_FLAG_WEAPON_2H;

    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &ranged);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON_RANGED, equipment) == NULL);
}

static void test_two_handed_ranged_replaces_real_shield(void) {
    tag_t equipment[PLAYER_EQUIP_MAX] = {0};
    equipment[PLAYER_EQUIP_WEAPON_RANGED] = TAG_RANGED;
    equipment[PLAYER_EQUIP_SHIELD] = TAG_SHIELD;
    ranged.flags = CS_FLAG_WEAPON_2H;

    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &ranged);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON_RANGED, equipment) == NULL);
}

static void test_one_handed_ranged_keeps_ordinary_slots(void) {
    tag_t equipment[PLAYER_EQUIP_MAX] = {0};
    equipment[PLAYER_EQUIP_WEAPON_RANGED] = TAG_RANGED;

    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == NULL);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON_RANGED, equipment) == &ranged);

    equipment[PLAYER_EQUIP_SHIELD] = TAG_SHIELD;
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &shield);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON_RANGED, equipment) == &ranged);
}

static void test_melee_and_shield_priority_is_preserved(void) {
    tag_t equipment[PLAYER_EQUIP_MAX] = {0};
    equipment[PLAYER_EQUIP_WEAPON] = TAG_MELEE;

    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON, equipment) == &melee);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == NULL);

    melee.flags = CS_FLAG_WEAPON_2H;
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON, equipment) == &melee);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == NULL);

    equipment[PLAYER_EQUIP_SHIELD] = TAG_SHIELD;
    melee.flags = 0;
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &shield);

    melee.flags = CS_FLAG_WEAPON_2H;
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &melee);
}

static void test_stale_ids_fail_safely(void) {
    tag_t equipment[PLAYER_EQUIP_MAX] = {0};
    equipment[PLAYER_EQUIP_WEAPON_RANGED] = TAG_STALE;
    equipment[PLAYER_EQUIP_SHIELD] = TAG_SHIELD;

    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_WEAPON_RANGED, equipment) == NULL);
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == &shield);

    equipment[PLAYER_EQUIP_SHIELD] = TAG_STALE;
    TEST_CHECK(playerdoll_equipment_resolve(PLAYER_EQUIP_SHIELD, equipment) == NULL);
}

int main(void) {
    reset_objects();
    test_two_handed_ranged_uses_empty_shield_slot();
    reset_objects();
    test_two_handed_ranged_replaces_real_shield();
    reset_objects();
    test_one_handed_ranged_keeps_ordinary_slots();
    reset_objects();
    test_melee_and_shield_priority_is_preserved();
    reset_objects();
    test_stale_ids_fail_safely();
    return EXIT_SUCCESS;
}
