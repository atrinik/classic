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

#include <global.h>
#include <server_main.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <arch.h>

START_TEST(test_object_matches_string) {
    mapstruct *map;
    object *pl, *o1, *o2;

    check_setup_env_pl(&map, &pl);

    o1 = arch_get("cloak");
    ck_assert_ptr_ne(o1, NULL);
    FREE_AND_COPY_HASH(o1->title, "of Moroch");
    CLEAR_FLAG(o1, FLAG_IDENTIFIED);

    ck_assert_int_eq(object_matches_string(o1, pl, "all"), 1);
    ck_assert_int_eq(object_matches_string(o1, pl, "Moroch"), 0);
    ck_assert_int_eq(object_matches_string(o1, pl, "random"), 0);

    SET_FLAG(o1, FLAG_IDENTIFIED);
    ck_assert_int_ne(object_matches_string(o1, pl, "Moroch"), 0);

    o2 = arch_get("cloak");
    SET_FLAG(o2, FLAG_UNPAID);
    ck_assert_int_eq(object_matches_string(o2, pl, "unpaid"), 2);
    ck_assert_int_ne(object_matches_string(o2, pl, "cloak"), 0);
    ck_assert_int_eq(object_matches_string(o2, pl, "wrong"), 0);

    object_destroy(o1);
    object_destroy(o2);
}
END_TEST

START_TEST(test_arch_to_object) {
    archetype_t *arch;
    object *obj;

    arch = arch_find("empty_archetype");
    obj = arch_to_object(arch);
    ck_assert_ptr_ne(obj, NULL);
    object_destroy(obj);
}
END_TEST

START_TEST(test_arch_get) {
    object *obj;

    obj = arch_get("empty_archetype");
    ck_assert_ptr_ne(obj, NULL);
    object_destroy(obj);

    obj = arch_get("AA938DFEPQ54FH");
    ck_assert_ptr_ne(obj, NULL);
    ck_assert_ptr_ne(obj->name, NULL);
    ck_assert(strstr(obj->name, "AA938DFEPQ54FH") != NULL);
    object_destroy(obj);
}
END_TEST

START_TEST(test_arch_find) {
    ck_assert_ptr_ne(arch_find("empty_archetype"), NULL);
    ck_assert_ptr_eq(arch_find("AA938DFEPQ54FH"), NULL);
}
END_TEST

/* Spell caches borrow from archetypes and must follow each server lifetime. */
START_TEST(test_spells_reinitialize) {
    for (int lifetime = 0; lifetime < 2; lifetime++) {
        check_teardown();
        check_setup();

        /* Scan every cached spell, independent of treasure-generation RNG. */
        int spell = spell_get_random(MAXLEVEL, SPELL_USE_BALM);
        ck_assert_int_ge(spell, 0);
        ck_assert_int_lt(spell, NROFREALSPELLS);
        ck_assert((spells[spell].spell_use & SPELL_USE_BALM) != 0);
        archetype_t *chosen = arch_find(spells[spell].id);
        ck_assert_ptr_ne(chosen, NULL);
        ck_assert_int_le(chosen->clone.level, MAXLEVEL);

        for (int i = 0; i < NROFREALSPELLS; i++) {
            archetype_t *at = arch_find(spells[i].id);
            ck_assert_ptr_ne(at, NULL);
            ck_assert_ptr_eq(spells[i].at, at);
            ck_assert_int_eq(at->clone.stats.sp, i);
            ck_assert_ptr_eq(spellarch[i],
                             spells[i].archname != NULL ? arch_find(spells[i].archname) : NULL);
        }
    }
}
END_TEST

START_TEST(test_spells_book_eligibility) {
    archetype_t *at = arch_find(spells[0].id);
    ck_assert_ptr_ne(at, NULL);
    int64_t original_value = at->clone.value;
    int other_flags = spells[0].spell_use & ~SPELL_USE_BOOK;

    /* An initialization call must refresh both directions of this derived bit. */
    at->clone.value = 1;
    init_spells();
    ck_assert_int_eq(spells[0].spell_use, other_flags | SPELL_USE_BOOK);

    at->clone.value = 0;
    init_spells();
    ck_assert_int_eq(spells[0].spell_use, other_flags);

    at->clone.value = 1;
    init_spells();
    ck_assert_int_eq(spells[0].spell_use, other_flags | SPELL_USE_BOOK);

    at->clone.value = original_value;
    init_spells();
    ck_assert_int_eq(spells[0].spell_use,
                     other_flags | (original_value != 0 ? SPELL_USE_BOOK : 0));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("arch");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);

    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_object_matches_string);
    tcase_add_test(tc_core, test_arch_to_object);
    tcase_add_test(tc_core, test_arch_get);
    tcase_add_test(tc_core, test_arch_find);
    tcase_add_test(tc_core, test_spells_reinitialize);
    tcase_add_test(tc_core, test_spells_book_eligibility);

    return s;
}

void check_server_arch(void) {
    check_run_suite(suite(), __FILE__);
}
