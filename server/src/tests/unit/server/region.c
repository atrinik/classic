/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <server_main.h>
#include <loader.h>
#include <light.h>
#include <region.h>

#define ROOT_FIELDS                      \
    "celestial_schema 1\n"               \
    "celestial_solar_mode global\n"      \
    "celestial_solar_rate 1/1\n"         \
    "celestial_solar_epoch 0\n"          \
    "celestial_solar_phase 0\n"          \
    "celestial_season_mode global\n"     \
    "celestial_season_rate 1/1\n"        \
    "celestial_season_epoch 0\n"         \
    "celestial_season_phase 0\n"         \
    "celestial_lunar_mode global\n"      \
    "celestial_lunar_rate 1/1\n"         \
    "celestial_lunar_epoch 0\n"          \
    "celestial_lunar_phase 0\n"          \
    "celestial_lunar_period 672\n"       \
    "celestial_day_color ffffff\n"       \
    "celestial_night_color 6080c0\n"     \
    "celestial_day_brightness 256\n"     \
    "celestial_night_brightness 256\n"   \
    "celestial_moon_color c0d0ff\n"      \
    "celestial_moon_max 20\n"            \
    "celestial_starlight_color 6080c0\n" \
    "celestial_starlight_strength 2\n"

#define ROOT_REGION "region world\n" ROOT_FIELDS "end\n"

static bool load_fixture(const char *contents, char *error, size_t error_size) {
    char path[HUGE_BUF];
    snprintf(VS(path), "%s/regions-189-fixture.reg", settings.datapath);
    FILE *fp = fopen(path, "wb");
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ne(fputs(contents, fp), EOF);
    ck_assert_int_eq(fclose(fp), 0);
    regions_free();
    bool loaded = regions_load(path, error, error_size);
    ck_assert_int_eq(unlink(path), 0);
    return loaded;
}

static bool load_with_child(const char *child, char *error, size_t error_size) {
    char contents[HUGE_BUF * 2];
    int length = snprintf(VS(contents), "%s%s", ROOT_REGION, child);
    ck_assert_int_ge(length, 0);
    ck_assert_uint_lt((size_t)length, sizeof(contents));
    return load_fixture(contents, error, error_size);
}

START_TEST(test_root_and_per_field_inheritance_match_frozen_vectors) {
    static const char children[] = "region half\n"
                                   "parent world\n"
                                   "celestial_solar_mode scaled\n"
                                   "celestial_solar_rate 1/2\n"
                                   "end\n"
                                   "region double\n"
                                   "parent world\n"
                                   "celestial_solar_mode scaled\n"
                                   "celestial_solar_rate 2/1\n"
                                   "end\n"
                                   "region fixed_full\n"
                                   "parent world\n"
                                   "celestial_solar_mode fixed\n"
                                   "celestial_solar_rate 0/1\n"
                                   "celestial_solar_phase 12\n"
                                   "celestial_season_mode fixed\n"
                                   "celestial_season_rate 0/1\n"
                                   "celestial_season_phase 3360\n"
                                   "celestial_lunar_mode fixed\n"
                                   "celestial_lunar_rate 0/1\n"
                                   "celestial_lunar_phase 336\n"
                                   "end\n"
                                   "region warm\n"
                                   "parent world\n"
                                   "celestial_day_color ffe0c0\n"
                                   "end\n"
                                   "region dark\n"
                                   "parent world\n"
                                   "celestial_day_brightness 0\n"
                                   "end\n"
                                   "region dim\n"
                                   "parent world\n"
                                   "celestial_day_brightness 128\n"
                                   "end\n"
                                   "region bright\n"
                                   "parent world\n"
                                   "celestial_day_brightness 512\n"
                                   "end\n"
                                   "region night_dark\n"
                                   "parent world\n"
                                   "celestial_night_brightness 0\n"
                                   "end\n"
                                   "region night_dim\n"
                                   "parent world\n"
                                   "celestial_night_brightness 128\n"
                                   "end\n"
                                   "region night_bright\n"
                                   "parent world\n"
                                   "celestial_night_brightness 512\n"
                                   "end\n"
                                   "region night_color\n"
                                   "parent world\n"
                                   "celestial_night_color 112233\n"
                                   "end\n";
    char error[HUGE_BUF];
    ck_assert_msg(load_with_child(children, VS(error)), "%s", error);

    const region_struct *world = region_world();
    const region_struct *half = region_find_by_name("half");
    const region_struct *double_speed = region_find_by_name("double");
    const region_struct *fixed = region_find_by_name("fixed_full");
    ck_assert_ptr_nonnull(world);
    ck_assert_str_eq(world->celestial.digest_hex,
                     "0e2277f88f263570761db60430a0e5cbbb84c28c3fd543beec5a8bde3bdc3b08");
    ck_assert_str_eq(half->celestial.digest_hex,
                     "5d22f851bacbbcb6da58f9560a36db97ea05940416e9f4b23669bf1928632b7a");
    ck_assert_str_eq(double_speed->celestial.digest_hex,
                     "b3cff5f335d829e7b729f31b39f5fce686e0748aab3cab2b879abc4d6f143669");
    ck_assert_str_eq(fixed->celestial.digest_hex,
                     "6b5ebba1b3516c9df5395905442e202b3543a108f10a6a0aeca6af49911179cd");
    ck_assert_str_eq(region_find_by_name("warm")->celestial.digest_hex,
                     "a957a66106a014e8ec9bd8a4c00e054289b7c2b0a8ca667f1c69d4b07438ef23");
    ck_assert_str_eq(region_find_by_name("dark")->celestial.digest_hex,
                     "ab2e04a45a17742863e1a329b59109231f89559f3bb4dd084632ca2ac4885a45");
    ck_assert_str_eq(region_find_by_name("dim")->celestial.digest_hex,
                     "938ed4d391065c87c0dbd04cfb213ddea8ebc340cfb3c15073fbc07b4ca1b8e6");
    ck_assert_str_eq(region_find_by_name("bright")->celestial.digest_hex,
                     "0a477c1a6840e74f310502a11f3e8b64b3bd594c961ad9791e62c6f2d3ea8fe4");
    ck_assert_str_eq(region_find_by_name("night_dark")->celestial.digest_hex,
                     "0032fafb6a36b1baed8687f9d0223931fd9dee2e07b81af7c34f070de7c0459e");
    ck_assert_str_eq(region_find_by_name("night_dim")->celestial.digest_hex,
                     "da8fd047219d20573ac54b077b2703b858863ec712fbbc4e1b240f1b1d2c40e3");
    ck_assert_str_eq(region_find_by_name("night_bright")->celestial.digest_hex,
                     "dc825693a9efde4047e999be249282deedd2af6a10327effa224d0b2118f6d99");
    ck_assert_uint_eq(region_find_by_name("night_color")->celestial.night_color,
                      UINT32_C(0x112233));

    ck_assert_uint_eq(world->celestial.day_linear[0], 65535);
    ck_assert_uint_eq(world->celestial.night_linear[0], 14544);
    ck_assert_uint_eq(world->celestial.night_linear[1], 26837);
    ck_assert_uint_eq(world->celestial.night_linear[2], 65535);
    ck_assert_uint_eq(world->celestial.moon_linear[0], 34544);
    ck_assert_uint_eq(world->celestial.moon_linear[1], 41337);

    region_celestial_phases_t phases;
    region_celestial_phases(&world->celestial, 18, &phases);
    ck_assert_uint_eq(phases.solar, 18);
    ck_assert_uint_eq(phases.season, 18);
    ck_assert_uint_eq(phases.lunar, 18);
    region_celestial_phases(&half->celestial, 18, &phases);
    ck_assert_uint_eq(phases.solar, 9);
    ck_assert_uint_eq(phases.season, 18);
    ck_assert_uint_eq(phases.lunar, 18);
    region_celestial_phases(&double_speed->celestial, 18, &phases);
    ck_assert_uint_eq(phases.solar, 12);
    region_celestial_phases(&fixed->celestial, UINT64_MAX, &phases);
    ck_assert_uint_eq(phases.solar, 12);
    ck_assert_uint_eq(phases.season, 3360);
    ck_assert_uint_eq(phases.lunar, 336);
}
END_TEST

START_TEST(test_scaled_epoch_wrap_and_lunar_mapping_are_independent) {
    static const char child[] = "region epoch_ahead\n"
                                "parent world\n"
                                "celestial_solar_mode scaled\n"
                                "celestial_solar_rate 1/2\n"
                                "celestial_solar_epoch 5\n"
                                "celestial_lunar_mode scaled\n"
                                "celestial_lunar_rate 2/1\n"
                                "celestial_lunar_epoch 1\n"
                                "celestial_lunar_phase 7\n"
                                "end\n";
    char error[HUGE_BUF];
    ck_assert_msg(load_with_child(child, VS(error)), "%s", error);
    const region_struct *region = region_find_by_name("epoch_ahead");
    region_celestial_phases_t phases;
    region_celestial_phases(&region->celestial, 3, &phases);
    ck_assert_uint_eq(phases.solar, 23);
    ck_assert_uint_eq(phases.season, 3);
    ck_assert_uint_eq(phases.lunar, 11);

    region_celestial_phases(&region->celestial, UINT64_MAX, &phases);
    ck_assert_uint_lt(phases.solar, 24);
    ck_assert_uint_lt(phases.season, 8064);
    ck_assert_uint_lt(phases.lunar, 672);
}
END_TEST

START_TEST(test_parser_rejects_invalid_fields_and_graphs) {
    static const char *invalid_children[] = {
        "region child\nparent world\ncelestial_unknown 1\nend\n",
        "region child\nparent world\ncelestial_day_brightness 1\ncelestial_day_brightness 2\nend\n",
        "region child\nparent world\ncelestial_solar_mode scaled\ncelestial_solar_rate 1/0\nend\n",
        "region child\nparent world\ncelestial_solar_mode scaled\ncelestial_solar_rate +1/2\nend\n",
        "region child\nparent world\ncelestial_solar_mode scaled\ncelestial_solar_rate "
        "999/1\nend\n",
        "region child\nparent world\ncelestial_solar_mode scaled\ncelestial_solar_rate 2/2\nend\n",
        "region child\nparent world\ncelestial_solar_rate 2/1\nend\n",
        "region child\nparent world\ncelestial_solar_mode fixed\ncelestial_solar_rate "
        "0/1\ncelestial_solar_epoch 1\nend\n",
        "region child\nparent world\ncelestial_solar_mode fixed\ncelestial_solar_rate "
        "0/1\ncelestial_solar_phase 24\nend\n",
        "region child\nparent world\ncelestial_day_color #ffffff\nend\n",
        "region child\nparent world\ncelestial_night_brightness 1025\nend\n",
        "region child\nparent world\ncelestial_moon_max 21\nend\n",
        "region child\nparent world\ncelestial_starlight_strength 3\nend\n",
        "region child\nparent world\ncelestial_lunar_period 169\nend\n",
        "region child\nparent missing\nend\n",
        "region child\nend\n",
        "region a\nparent b\nend\nregion b\nparent a\nend\n",
        "region world\nparent world\nend\n",
    };
    char error[HUGE_BUF];
    for (size_t i = 0; i < arraysize(invalid_children); i++) {
        error[0] = '\0';
        ck_assert_msg(!load_with_child(invalid_children[i], VS(error)),
                      "accepted invalid fixture %zu",
                      i);
        ck_assert_str_ne(error, "");
    }

    static const char incomplete_root[] = "region world\n"
                                          "celestial_schema 1\n"
                                          "end\n";
    ck_assert(!load_fixture(incomplete_root, VS(error)));
    ck_assert_ptr_null(first_region);
}
END_TEST

START_TEST(test_map_lookup_defaults_to_world_and_unknown_fails) {
    char error[HUGE_BUF];
    ck_assert_msg(load_fixture(ROOT_REGION, VS(error)), "%s", error);

    mapstruct *map = get_empty_map(1, 1);
    FILE *fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ne(fputs("arch map\nwidth 1\nheight 1\nend\n", fp), EOF);
    rewind(fp);
    ck_assert(load_map_header(map, fp));
    ck_assert_ptr_eq(map->region, region_world());
    ck_assert_ptr_eq(region_celestial_for_map(map), &region_world()->celestial);
    ck_assert_int_eq(fclose(fp), 0);
    delete_map(map);

    map = get_empty_map(1, 1);
    fp = tmpfile();
    ck_assert_ptr_nonnull(fp);
    ck_assert_int_ne(fputs("arch map\nregion missing\nwidth 1\nheight 1\nend\n", fp), EOF);
    rewind(fp);
    ck_assert(!load_map_header(map, fp));
    ck_assert_int_eq(fclose(fp), 0);
    delete_map(map);
}
END_TEST

START_TEST(test_diagnostic_is_bounded_and_reports_identity_and_overrides) {
    static const char child[] = "region dim\n"
                                "parent world\n"
                                "celestial_day_brightness 128\n"
                                "end\n";
    char error[HUGE_BUF];
    ck_assert_msg(load_with_child(child, VS(error)), "%s", error);
    const region_struct *region = region_find_by_name("dim");
    char diagnostic[HUGE_BUF];
    ck_assert(region_celestial_diagnostic(region, 18, VS(diagnostic)));
    ck_assert_ptr_nonnull(strstr(diagnostic, "region=dim"));
    ck_assert_ptr_nonnull(strstr(diagnostic, region->celestial.digest_hex));
    ck_assert_ptr_nonnull(strstr(diagnostic, "solar=18 season=18 lunar=18"));
    ck_assert_ptr_nonnull(strstr(diagnostic, "overrides=celestial_day_brightness"));
    char short_buffer[8];
    ck_assert(!region_celestial_diagnostic(region, 18, VS(short_buffer)));
}
END_TEST

START_TEST(test_reload_load_order_and_map_transitions_are_deterministic) {
    static const char fixture[] = "region child\n"
                                  "parent world\n"
                                  "celestial_night_brightness 0\n"
                                  "end\n"
                                  "region world\n" ROOT_FIELDS "end\n";
    char error[HUGE_BUF];
    ck_assert_msg(load_fixture(fixture, VS(error)), "%s", error);
    const region_struct *child = region_find_by_name("child");
    char digest[sizeof(child->celestial.digest_hex)];
    snprintf(VS(digest), "%s", child->celestial.digest_hex);
    region_celestial_phases_t before;
    todtick = ULONG_MAX;
    region_celestial_phases(&child->celestial, todtick, &before);
    ck_assert_uint_eq(todtick, ULONG_MAX);

    mapstruct *upper = get_empty_map(1, 1);
    mapstruct *lower = get_empty_map(1, 1);
    upper->region = region_world();
    lower->region = (region_struct *)child;
    upper->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = upper;
    ck_assert_ptr_ne(region_celestial_for_map(upper), region_celestial_for_map(lower));
    ck_assert_str_eq(region_celestial_for_map(lower)->digest_hex, digest);

    MapSpace local = {.light_source_value = -40};
    uint16_t scalar_before, rgb_before[3], scalar_after, rgb_after[3];
    light_radiance_from_raw(&local, 80, &scalar_before, rgb_before);
    (void)region_celestial_for_map(upper);
    (void)region_celestial_for_map(lower);
    light_radiance_from_raw(&local, 80, &scalar_after, rgb_after);
    ck_assert_uint_eq(scalar_before, scalar_after);
    ck_assert_mem_eq(rgb_before, rgb_after, sizeof(rgb_before));
    delete_map(upper);
    delete_map(lower);

    ck_assert_msg(load_fixture(fixture, VS(error)), "%s", error);
    child = region_find_by_name("child");
    region_celestial_phases_t after;
    region_celestial_phases(&child->celestial, ULONG_MAX, &after);
    ck_assert_str_eq(child->celestial.digest_hex, digest);
    ck_assert_mem_eq(&before, &after, sizeof(before));
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("region");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_root_and_per_field_inheritance_match_frozen_vectors);
    tcase_add_test(tc_core, test_scaled_epoch_wrap_and_lunar_mapping_are_independent);
    tcase_add_test(tc_core, test_parser_rejects_invalid_fields_and_graphs);
    tcase_add_test(tc_core, test_map_lookup_defaults_to_world_and_unknown_fails);
    tcase_add_test(tc_core, test_diagnostic_is_bounded_and_reports_identity_and_overrides);
    tcase_add_test(tc_core, test_reload_load_order_and_map_transitions_are_deterministic);
    return s;
}

void check_server_region(void) {
    check_run_suite(suite(), __FILE__);
}
