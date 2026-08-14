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

#include <arch.h>
#include <celestial_structure.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <loader.h>
#include <map.h>
#include <object.h>

static mapstruct *new_v1_map(const char *path, int width, int height, int sky) {
    mapstruct *map = get_empty_map(width, height);
    FREE_AND_COPY_HASH(map->path, path);
    map->celestial_schema_seen = true;
    map->celestial_schema = 1;
    map->celestial_sky_seen = true;
    map->celestial_sky_above = sky;
    return map;
}

static object *new_object(mapstruct *map, int x, int y) {
    object *op = arch_get("empty_archetype");
    ck_assert_ptr_ne(op, NULL);
    op->x = x;
    op->y = y;
    return object_insert_map(op, map, op, INS_NO_MERGE | INS_NO_WALK_ON);
}

START_TEST(test_header_round_trip_is_canonical_and_rejects_legacy_fields) {
    FILE *input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch map\n"
                           "name fixture\n"
                           "celestial_schema 1\n"
                           "sky_above linked\n"
                           "light 80\n"
                           "width 5\n"
                           "height 5\n"
                           "tile_path_9 /upper\n"
                           "celestial_boundary_9 continuous\n"
                           "end\n",
                           input),
                     EOF);
    rewind(input);

    mapstruct *map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/lower");
    ck_assert_int_eq(load_map_header(map, input), 1);
    fclose(input);
    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);

    char *saved = NULL;
    size_t saved_size = 0;
    FILE *output = open_memstream(&saved, &saved_size);
    ck_assert_ptr_ne(output, NULL);
    save_map_header(map, output, 1);
    ck_assert_int_eq(fclose(output), 0);
    ck_assert_ptr_ne(strstr(saved, "celestial_schema 1\nsky_above linked\nlight 80\n"), NULL);
    ck_assert_ptr_ne(strstr(saved, "tile_path_9 /upper\ncelestial_boundary_9 continuous\n"), NULL);
    ck_assert_ptr_eq(strstr(saved, "darkness "), NULL);
    ck_assert_ptr_eq(strstr(saved, "outdoor "), NULL);
    free(saved);
    delete_map(map);

    map = new_v1_map("/bad", 5, 5, CELESTIAL_SKY_OPEN);
    map->celestial_legacy_darkness_seen = true;
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/bad"), NULL);
    delete_map(map);

    map = new_v1_map("/duplicate-header", 5, 5, CELESTIAL_SKY_OPEN);
    char duplicate[] = "celestial_schema 1\n";
    ck_assert_int_eq(map_set_variable(map, duplicate), LL_MORE);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    delete_map(map);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch map\ncelestial_schema 1\nsky_above open\nlight 80junk\n"
                           "width 5\nheight 5\nend\n",
                           input),
                     EOF);
    rewind(input);
    map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/malformed-light");
    ck_assert_int_eq(load_map_header(map, input), 1);
    fclose(input);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    delete_map(map);
}
END_TEST

START_TEST(test_metadata_is_validated_consumed_sorted_and_saved) {
    mapstruct *map = new_v1_map("/metadata", 8, 8, CELESTIAL_SKY_SEALED);
    object *right = new_object(map, 4, 3);
    object_set_value(right, "_celestial_metadata_kind", "ambient_light_zone", 1);
    object_set_value(right, "ambient_strength", "320", 1);
    right->stats.hp = 1;
    right->stats.sp = 0;

    object *left = new_object(map, 1, 1);
    object_set_value(left, "_celestial_metadata_kind", "sky_exposure", 1);
    object_set_value(left, "sky_state", "open", 1);
    left->stats.hp = 0;
    left->stats.sp = 1;

    object *map_info = new_object(map, 0, 0);
    map_info->type = MAP_INFO;
    FREE_AND_COPY_HASH(map_info->race, "Unchanged name");

    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    ck_assert_uint_eq(map->celestial_rectangle_count, 2);
    ck_assert_ptr_eq(GET_MAP_OB(map, 1, 1), NULL);
    ck_assert_ptr_eq(GET_MAP_OB(map, 4, 3), NULL);
    ck_assert_ptr_eq(GET_MAP_OB(map, 0, 0), map_info);
    ck_assert_str_eq(map_info->race, "Unchanged name");
    ck_assert_uint_eq(map->celestial_rectangles[0].type, CELESTIAL_RECT_SKY_OPEN);
    ck_assert_uint_eq(map->celestial_rectangles[1].type, CELESTIAL_RECT_AMBIENT);

    char *saved = NULL;
    size_t saved_size = 0;
    FILE *output = open_memstream(&saved, &saved_size);
    ck_assert_ptr_ne(output, NULL);
    celestial_structure_save_metadata(map, output);
    ck_assert_int_eq(fclose(output), 0);
    ck_assert_str_eq(saved,
                     "arch sky_exposure\nx 1\ny 1\nhp 0\nsp 1\nsky_state open\nend\n"
                     "arch ambient_light_zone\nx 4\ny 3\nhp 1\nsp 0\n"
                     "ambient_strength 320\nend\n");
    free(saved);
    delete_map(map);
}
END_TEST

START_TEST(test_rectangles_fail_closed_with_coordinates) {
    mapstruct *map = new_v1_map("/rectangles", 4, 4, CELESTIAL_SKY_OPEN);
    object *covered = new_object(map, 3, 3);
    object_set_value(covered, "_celestial_metadata_kind", "sky_exposure", 1);
    object_set_value(covered, "sky_state", "covered", 1);
    covered->stats.hp = 1;

    char error[HUGE_BUF];
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/rectangles (3,3)"), NULL);
    delete_map(map);

    map = new_v1_map("/covered", 4, 4, CELESTIAL_SKY_SEALED);
    covered = new_object(map, 1, 1);
    object_set_value(covered, "_celestial_metadata_kind", "sky_exposure", 1);
    object_set_value(covered, "sky_state", "covered", 1);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "no open upper boundary"), NULL);
    delete_map(map);
}
END_TEST

START_TEST(test_transmission_faces_and_aperture_identity) {
    mapstruct *map = new_v1_map("/objects", 5, 5, CELESTIAL_SKY_OPEN);
    object *floor = new_object(map, 1, 1);
    SET_FLAG(floor, FLAG_IS_FLOOR);
    ck_assert_uint_eq(celestial_structure_faces(floor), CELESTIAL_FACE_DOWN);

    object *irrelevant = new_object(map, 0, 0);
    object_set_value(irrelevant, "celestial_faces", "N", 1);

    object *door = new_object(map, 2, 2);
    door->type = DOOR;
    object_set_value(door, "celestial_faces", "N,E", 1);
    object_set_value(door, "celestial_transmission_closed", "opaque", 1);
    object_set_value(door, "celestial_transmission_open", "open", 1);
    object_set_value(door, "celestial_aperture_id", "00000000000000ba", 1);
    ck_assert_uint_eq(celestial_structure_faces(door), CELESTIAL_FACE_NORTH | CELESTIAL_FACE_EAST);

    char error[HUGE_BUF];
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "invalid celestial_faces"), NULL);
    object_remove(irrelevant, REMOVE_NO_WALK_OFF);
    object_destroy(irrelevant);
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    ck_assert_int_eq(celestial_structure_transmission("glass"), 192);
    ck_assert_int_eq(celestial_structure_transmission("grate"), 224);
    delete_map(map);

    map = new_v1_map("/duplicate", 5, 5, CELESTIAL_SKY_OPEN);
    for (int x = 1; x <= 2; x++) {
        door = new_object(map, x, 2);
        door->type = DOOR;
        object_set_value(door, "celestial_transmission_closed", "opaque", 1);
        object_set_value(door, "celestial_transmission_open", "open", 1);
        object_set_value(door, "celestial_aperture_id", "00000000000000ba", 1);
    }
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/duplicate (2,2)"), NULL);
    delete_map(map);

    FILE *input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch empty_archetype\ncelestial_transmission glass\n"
                           "celestial_transmission grate\nend\n",
                           input),
                     EOF);
    rewind(input);
    object *parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);
}
END_TEST

START_TEST(test_reciprocal_vertical_topology_fails_closed) {
    mapstruct *lower = new_v1_map("/lower", 5, 5, CELESTIAL_SKY_LINKED);
    mapstruct *upper = new_v1_map("/upper", 5, 5, CELESTIAL_SKY_OPEN);
    FREE_AND_COPY_HASH(lower->tile_path[TILED_UP], "/upper");
    FREE_AND_COPY_HASH(upper->tile_path[TILED_DOWN], "/lower");
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    lower->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    upper->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;

    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_validate_topology(lower, VS(error)), "%s", error);
    upper->width = 4;
    ck_assert(!celestial_structure_validate_topology(lower, VS(error)));
    ck_assert_ptr_ne(strstr(error, "non-identity dimensions"), NULL);
    upper->width = 5;
    upper->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_DISCONTINUOUS;
    ck_assert(!celestial_structure_validate_topology(lower, VS(error)));
    ck_assert_ptr_ne(strstr(error, "disagreeing reciprocal"), NULL);

    delete_map(lower);
    delete_map(upper);
}
END_TEST

START_TEST(test_stack_derives_cover_and_rejects_redundant_exceptions) {
    mapstruct *lower = new_v1_map("/stack/lower", 5, 5, CELESTIAL_SKY_LINKED);
    mapstruct *upper = new_v1_map("/stack/upper", 5, 5, CELESTIAL_SKY_LINKED);
    mapstruct *top = new_v1_map("/stack/top", 5, 5, CELESTIAL_SKY_OPEN);
    FREE_AND_COPY_HASH(lower->tile_path[TILED_UP], upper->path);
    FREE_AND_COPY_HASH(upper->tile_path[TILED_DOWN], lower->path);
    FREE_AND_COPY_HASH(upper->tile_path[TILED_UP], top->path);
    FREE_AND_COPY_HASH(top->tile_path[TILED_DOWN], upper->path);
    lower->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = lower;
    upper->tile_map[TILED_UP] = top;
    top->tile_map[TILED_DOWN] = upper;
    lower->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    upper->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;
    upper->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    top->celestial_boundary[TILED_DOWN] = CELESTIAL_BOUNDARY_CONTINUOUS;

    object *balcony = new_object(upper, 1, 1);
    SET_FLAG(balcony, FLAG_IS_FLOOR);
    object *roof = new_object(upper, 2, 1);
    roof->layer = LAYER_WALL;
    SET_FLAG(roof, FLAG_HIDDEN);
    object_set_value(roof, "sky_boundary", "1", 1);
    object *storey = new_object(top, 3, 1);
    SET_FLAG(storey, FLAG_IS_FLOOR);

    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(lower, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_finalize_map(upper, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_finalize_map(top, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_validate_topology(lower, VS(error)), "%s", error);
    ck_assert(celestial_structure_cell_exposed(lower, 0, 0));
    ck_assert(!celestial_structure_cell_exposed(lower, 1, 1));
    ck_assert(!celestial_structure_cell_exposed(lower, 2, 1));
    ck_assert(!celestial_structure_cell_exposed(lower, 3, 1));
    ck_assert(celestial_structure_cell_exposed(upper, 1, 1));

    object *house_cover = new_object(lower, 4, 4);
    object_set_value(house_cover, "_celestial_metadata_kind", "sky_exposure", 1);
    object_set_value(house_cover, "sky_state", "covered", 1);
    ck_assert_msg(celestial_structure_finalize_map(lower, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_validate_topology(lower, VS(error)), "%s", error);
    ck_assert(!celestial_structure_cell_exposed(lower, 4, 4));
    ck_assert(celestial_structure_cell_exposed(lower, 4, 3));

    object *covered = new_object(lower, 1, 1);
    object_set_value(covered, "_celestial_metadata_kind", "sky_exposure", 1);
    object_set_value(covered, "sky_state", "covered", 1);
    ck_assert_msg(celestial_structure_finalize_map(lower, VS(error)), "%s", error);
    ck_assert(!celestial_structure_validate_topology(lower, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/stack/lower depth 0 cell (1,1)"), NULL);

    delete_map(lower);
    delete_map(upper);
    delete_map(top);
}
END_TEST

START_TEST(test_inventory_is_bounded_deterministic_and_read_only) {
    ck_assert(celestial_structure_inventory_maps_valid("/fixture,/greyton/house"));
    ck_assert(!celestial_structure_inventory_maps_valid("fixture"));
    ck_assert(!celestial_structure_inventory_maps_valid("/fixture,/fixture"));
    ck_assert(!celestial_structure_inventory_maps_valid("/fixture,../escape"));

    mapstruct *map = new_v1_map("/inventory", 3, 3, CELESTIAL_SKY_OPEN);
    object *wall = new_object(map, 1, 1);
    SET_FLAG(wall, FLAG_BLOCKSVIEW);
    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);

    char *first = NULL;
    char *second = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    FILE *first_fp = open_memstream(&first, &first_size);
    FILE *second_fp = open_memstream(&second, &second_size);
    ck_assert(!celestial_structure_inventory(map, first_fp, 1));
    ck_assert(celestial_structure_inventory(map, second_fp, 2));
    ck_assert_int_eq(fclose(first_fp), 0);
    ck_assert_int_eq(fclose(second_fp), 0);
    ck_assert_uint_eq(first_size, 0);
    ck_assert_ptr_ne(strstr(second, "\tobject\t/inventory\t1\t1\t"), NULL);

    char *repeat = NULL;
    size_t repeat_size = 0;
    FILE *repeat_fp = open_memstream(&repeat, &repeat_size);
    ck_assert(celestial_structure_inventory(map, repeat_fp, 2));
    ck_assert_int_eq(fclose(repeat_fp), 0);
    ck_assert_uint_eq(second_size, repeat_size);
    ck_assert_int_eq(memcmp(second, repeat, second_size), 0);
    free(first);
    free(second);
    free(repeat);
    delete_map(map);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_structure");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_header_round_trip_is_canonical_and_rejects_legacy_fields);
    tcase_add_test(tc_core, test_metadata_is_validated_consumed_sorted_and_saved);
    tcase_add_test(tc_core, test_rectangles_fail_closed_with_coordinates);
    tcase_add_test(tc_core, test_transmission_faces_and_aperture_identity);
    tcase_add_test(tc_core, test_reciprocal_vertical_topology_fails_closed);
    tcase_add_test(tc_core, test_stack_derives_cover_and_rejects_redundant_exceptions);
    tcase_add_test(tc_core, test_inventory_is_bounded_deterministic_and_read_only);
    return s;
}

void check_server_celestial_structure(void) {
    check_run_suite(suite(), __FILE__);
}
