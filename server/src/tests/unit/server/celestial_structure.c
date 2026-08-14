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
#include <initialization.h>
#include <loader.h>
#include <map.h>
#include <object.h>
#include <swap.h>

#ifndef WIN32
static ssize_t fail_inventory_write(void *cookie, const char *buffer, size_t size) {
    (void)cookie;
    (void)buffer;
    (void)size;
    errno = ENOSPC;
    return -1;
}

static char *run_inventory_capture(const char *map_ids, size_t *size) {
    snprintf(VS(settings.celestial_inventory_maps), "%s", map_ids);
    settings.celestial_inventory_limit = 32;
    FILE *capture = tmpfile();
    ck_assert_ptr_ne(capture, NULL);
    int saved_stdout = dup(STDOUT_FILENO);
    ck_assert_int_ge(saved_stdout, 0);
    ck_assert_int_eq(fflush(stdout), 0);
    ck_assert_int_ge(dup2(fileno(capture), STDOUT_FILENO), 0);
    ck_assert_int_eq(celestial_structure_inventory_run(), EXIT_SUCCESS);
    ck_assert_int_eq(fflush(stdout), 0);
    ck_assert_int_ge(dup2(saved_stdout, STDOUT_FILENO), 0);
    close(saved_stdout);
    ck_assert_int_eq(fseek(capture, 0, SEEK_END), 0);
    long length = ftell(capture);
    ck_assert_int_ge(length, 0);
    rewind(capture);
    char *output = xmalloc((size_t)length + 1);
    ck_assert_uint_eq(fread(output, 1, (size_t)length, capture), (size_t)length);
    output[length] = '\0';
    ck_assert_int_eq(fclose(capture), 0);
    *size = (size_t)length;
    return output;
}
#endif

static mapstruct *new_v1_map(const char *path, int width, int height, int sky) {
    mapstruct *map = get_empty_map(width, height);
    FREE_AND_COPY_HASH(map->path, path);
    map->celestial_schema_seen = true;
    map->celestial_schema = 1;
    map->celestial_sky_seen = true;
    map->celestial_sky_above = sky;
    map->celestial_width_seen = true;
    map->celestial_height_seen = true;
    return map;
}

static object *new_object(mapstruct *map, int x, int y) {
    object *op = arch_get("empty_archetype");
    ck_assert_ptr_ne(op, NULL);
    op->x = x;
    op->y = y;
    return object_insert_map(op, map, op, INS_NO_MERGE | INS_NO_WALK_ON);
}

static void make_metadata(object *op, uint8_t kind) {
    op->celestial_metadata_kind = kind;
    op->layer = LAYER_SYS;
}

static void make_aperture(object *op, const char *faces, const char *id) {
    op->type = DOOR;
    if (faces != NULL) {
        object_set_value(op, "celestial_faces", faces, 1);
    }
    object_set_value(op, "celestial_transmission_closed", "opaque", 1);
    object_set_value(op, "celestial_transmission_open", "open", 1);
    object_set_value(op, "celestial_aperture_id", id, 1);
    op->celestial_aperture_id_authored = true;
}

static void set_empty_aperture_contract(bool enabled) {
    object *clone = &arches[ARCH_EMPTY_ARCHETYPE]->clone;
    clone->type = enabled ? DOOR : 0;
    object_set_value(clone, "celestial_transmission_closed", enabled ? "opaque" : NULL, enabled);
    object_set_value(clone, "celestial_transmission_open", enabled ? "open" : NULL, enabled);
}

static void set_empty_roof_contract(bool enabled) {
    object *clone = &arches[ARCH_EMPTY_ARCHETYPE]->clone;
    object_set_value(clone, "sky_boundary", enabled ? "1" : NULL, enabled);
    clone->layer = enabled ? LAYER_WALL : 0;
    if (enabled) {
        SET_FLAG(clone, FLAG_HIDDEN);
        SET_FLAG(clone, FLAG_IS_FLOOR);
    } else {
        CLEAR_FLAG(clone, FLAG_HIDDEN);
        CLEAR_FLAG(clone, FLAG_IS_FLOOR);
    }
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

    FREE_AND_CLEAR_HASH(map->tile_path[TILED_UP]);
    celestial_structure_reset_parse_state(map);
    input = fmemopen(saved, saved_size, "r");
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_eq(load_map_header(map, input), 1);
    fclose(input);
    ck_assert_msg(celestial_structure_validate_header(map, VS(error)), "%s", error);
    ck_assert_uint_eq(map->celestial_schema, 1);
    ck_assert_uint_eq(map->celestial_sky_above, CELESTIAL_SKY_LINKED);
    ck_assert_str_eq(map->tile_path[TILED_UP], "/upper");
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

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch map\ncelestial_schema 1\nsky_above open\n"
                           "width 5junk\nheight 5\nend\n",
                           input),
                     EOF);
    rewind(input);
    map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/malformed-width");
    ck_assert_int_eq(load_map_header(map, input), 1);
    fclose(input);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    delete_map(map);

    const char *malformed_headers[] = {
        "width +5\nheight 5\n",
        "width 999999999999999999999999\nheight 5\n",
        "width 5\nwidth 5\nheight 5\n",
        "width 5\nheight 5\ntile_path_11 /outside\n",
        "width 5\nheight 5\ntile_path_9\n",
        "width 5\nheight 5\ntile_pat_9 /upper\n",
        "width 5\nheight 5\ntile_path_9 upper\n",
        "width 5\nheight 5\ntile_path_9 /a/../b\n",
        "width 5\nheight 5\ntile_path_9 /a\\b\n",
        "width 5\nheight 5\ntile_path_9 /a,b\n",
        "width 5\nheight 5\ntile_path_9 /a\tb\n",
        "width 5\nheight 5\nlight\n",
        "width 5\nheight 5\ndarkness 999999999999999999999999\n",
        "width 5\nheight 5\noutdoor 999999999999999999999999\n",
    };
    for (size_t i = 0; i < arraysize(malformed_headers); i++) {
        char contents[HUGE_BUF];
        snprintf(contents,
                 sizeof(contents),
                 "arch map\ncelestial_schema 1\nsky_above open\n%send\n",
                 malformed_headers[i]);
        input = fmemopen(contents, strlen(contents), "r");
        ck_assert_ptr_ne(input, NULL);
        map = get_empty_map(5, 5);
        FREE_AND_COPY_HASH(map->path, "/malformed-header");
        ck_assert_int_eq(load_map_header(map, input), 1);
        fclose(input);
        ck_assert(!celestial_structure_finalize_map(map, VS(error)));
        delete_map(map);
    }

    char legacy_relative[] = "arch map\nwidth 5\nheight 5\ntile_path_9 upper\nend\n";
    input = fmemopen(legacy_relative, strlen(legacy_relative), "r");
    ck_assert_ptr_ne(input, NULL);
    map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/legacy/map");
    ck_assert_int_eq(load_map_header(map, input), 1);
    fclose(input);
    ck_assert_str_eq(map->tile_path[TILED_UP], "/legacy/upper");
    ck_assert(celestial_structure_finalize_map(map, VS(error)));
    delete_map(map);

    map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/partial-schema");
    map->celestial_v1_header_seen = true;
    map->celestial_sky_seen = true;
    map->celestial_sky_above = CELESTIAL_SKY_OPEN;
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "without a schema"), NULL);
    delete_map(map);

    char truncated[] = "arch map\ncelestial_schema 1\nsky_above open\nwidth 5\nheight 5\n";
    input = fmemopen(truncated, strlen(truncated), "r");
    ck_assert_ptr_ne(input, NULL);
    map = get_empty_map(5, 5);
    FREE_AND_COPY_HASH(map->path, "/truncated-header");
    ck_assert_int_eq(load_map_header(map, input), 0);
    fclose(input);
    delete_map(map);
}
END_TEST

START_TEST(test_saved_v1_map_swaps_and_reloads_mutable_state) {
    mapstruct *map = new_v1_map("/test/celestial-swap", 5, 5, CELESTIAL_SKY_OPEN);
    object *marker = new_object(map, 2, 3);
    FREE_AND_COPY_HASH(marker->name, "saved celestial marker");
    FREE_AND_COPY_HASH(marker->name_pl, "saved celestial markers");
    map->reset_time = seconds() + 3600;

    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    swap_map(map, 1);
    ck_assert_int_eq(map->in_memory, MAP_SWAPPED);

    map = ready_map_name("/test/celestial-swap", NULL, 0);
    ck_assert_ptr_ne(map, NULL);
    ck_assert_uint_eq(map->celestial_schema, 1);
    ck_assert_uint_eq(map->celestial_sky_above, CELESTIAL_SKY_OPEN);
    marker = GET_MAP_OB(map, 2, 3);
    ck_assert_ptr_ne(marker, NULL);
    ck_assert_str_eq(marker->name, "saved celestial marker");
    delete_map(map);
}
END_TEST

START_TEST(test_metadata_is_validated_consumed_sorted_and_saved) {
    mapstruct *map = new_v1_map("/metadata", 8, 8, CELESTIAL_SKY_SEALED);
    object *right = new_object(map, 4, 3);
    make_metadata(right, CELESTIAL_RECT_AMBIENT);
    object_set_value(right, "ambient_strength", "320", 1);
    right->stats.hp = 1;
    right->stats.sp = 0;

    object *left = new_object(map, 1, 1);
    make_metadata(left, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(left, "sky_state", "open", 1);
    left->stats.hp = 0;
    left->stats.sp = 1;

    object *map_info = new_object(map, 0, 0);
    map_info->type = MAP_INFO;
    FREE_AND_COPY_HASH(map_info->race, "Unchanged name");
    object_set_value(map_info, "_celestial_metadata_kind", "ambient_light_zone", 1);

    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    ck_assert_uint_eq(map->celestial_rectangle_count, 2);
    ck_assert_ptr_eq(GET_MAP_OB(map, 1, 1), NULL);
    ck_assert_ptr_eq(GET_MAP_OB(map, 4, 3), NULL);
    ck_assert_ptr_eq(GET_MAP_OB(map, 0, 0), map_info);
    ck_assert_str_eq(map_info->race, "Unchanged name");
    ck_assert_str_eq(object_get_value(map_info, "_celestial_metadata_kind"), "ambient_light_zone");
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
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(covered, "sky_state", "covered", 1);
    covered->stats.hp = 1;

    char error[HUGE_BUF];
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/rectangles (3,3)"), NULL);
    delete_map(map);

    map = new_v1_map("/covered", 4, 4, CELESTIAL_SKY_SEALED);
    covered = new_object(map, 1, 1);
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(covered, "sky_state", "covered", 1);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "no open upper boundary"), NULL);
    delete_map(map);

    map = new_v1_map("/conflicting-metadata", 4, 4, CELESTIAL_SKY_OPEN);
    covered = new_object(map, 1, 1);
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(covered, "sky_state", "covered", 1);
    object_set_value(covered, "celestial_transmission", "glass", 1);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "conflicting object fields"), NULL);
    delete_map(map);

    map = new_v1_map("/extra-metadata", 4, 4, CELESTIAL_SKY_OPEN);
    covered = new_object(map, 1, 1);
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(covered, "sky_state", "covered", 1);
    FREE_AND_COPY_HASH(covered->name, "not metadata");
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "outside its exact schema"), NULL);
    delete_map(map);

    map = new_v1_map("/nested-metadata", 4, 4, CELESTIAL_SKY_OPEN);
    covered = new_object(map, 1, 1);
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(covered, "sky_state", "covered", 1);
    object_insert_into(arch_get("empty_archetype"), covered, 0);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "conflicting object fields"), NULL);
    delete_map(map);

    map = new_v1_map("/nested-celestial", 4, 4, CELESTIAL_SKY_OPEN);
    object *container = new_object(map, 1, 1);
    object *nested = arch_get("empty_archetype");
    object_set_value(nested, "celestial_transmission", "glass", 1);
    object_insert_into(nested, container, 0);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "nested object"), NULL);
    delete_map(map);

    map = get_empty_map(4, 4);
    FREE_AND_COPY_HASH(map->path, "/legacy-nested-celestial");
    container = new_object(map, 1, 1);
    nested = arch_get("empty_archetype");
    object_set_value(nested, "sky_state", "covered", 1);
    object_insert_into(nested, container, 0);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "nested object"), NULL);
    delete_map(map);
}
END_TEST

START_TEST(test_transmission_faces_and_aperture_identity) {
    char error[HUGE_BUF];
    object *empty_clone = &arches[ARCH_EMPTY_ARCHETYPE]->clone;
    object_set_value(empty_clone, "ambient_strength", "0", 1);
    ck_assert(!celestial_structure_validate_archetypes(VS(error)));
    ck_assert_ptr_ne(strstr(error, "reserved celestial metadata"), NULL);
    object_set_value(empty_clone, "ambient_strength", NULL, 0);
    ck_assert(celestial_structure_validate_archetypes(VS(error)));

    mapstruct *map = new_v1_map("/objects", 5, 5, CELESTIAL_SKY_OPEN);
    object *floor = new_object(map, 1, 1);
    SET_FLAG(floor, FLAG_IS_FLOOR);
    ck_assert_uint_eq(celestial_structure_faces(floor), CELESTIAL_FACE_DOWN);
    object_remove(floor, REMOVE_NO_WALK_OFF);
    object_destroy(floor);

    object *irrelevant = new_object(map, 0, 0);
    object_set_value(irrelevant, "celestial_faces", "N", 1);

    object *door = new_object(map, 2, 2);
    door->type = DOOR;
    object_set_value(door, "celestial_faces", "N,E", 1);
    object_set_value(door, "celestial_transmission_closed", "opaque", 1);
    object_set_value(door, "celestial_transmission_open", "open", 1);
    object_set_value(door, "celestial_aperture_id", "00000000000000ba", 1);
    door->celestial_aperture_id_authored = true;
    ck_assert_uint_eq(celestial_structure_faces(door), CELESTIAL_FACE_NORTH | CELESTIAL_FACE_EAST);

    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "invalid celestial_faces"), NULL);
    object_remove(irrelevant, REMOVE_NO_WALK_OFF);
    object_destroy(irrelevant);
    set_empty_aperture_contract(true);
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    ck_assert_int_eq(celestial_structure_transmission("glass"), 192);
    ck_assert_int_eq(celestial_structure_transmission("grate"), 224);
    set_empty_aperture_contract(false);
    delete_map(map);

    map = new_v1_map("/ordinary-metadata-field", 5, 5, CELESTIAL_SKY_OPEN);
    object *ordinary = new_object(map, 1, 1);
    object_set_value(ordinary, "sky_state", "covered", 1);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "metadata-only"), NULL);
    delete_map(map);

    map = new_v1_map("/duplicate", 5, 5, CELESTIAL_SKY_OPEN);
    for (int x = 1; x <= 2; x++) {
        door = new_object(map, x, 2);
        door->type = DOOR;
        object_set_value(door, "celestial_transmission_closed", "opaque", 1);
        object_set_value(door, "celestial_transmission_open", "open", 1);
        object_set_value(door, "celestial_aperture_id", "00000000000000ba", 1);
        door->celestial_aperture_id_authored = true;
    }
    set_empty_aperture_contract(true);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/duplicate (2,2)"), NULL);
    set_empty_aperture_contract(false);
    delete_map(map);

    map = new_v1_map("/inherited-id", 5, 5, CELESTIAL_SKY_OPEN);
    door = new_object(map, 2, 2);
    door->type = DOOR;
    object_set_value(door, "celestial_transmission_closed", "opaque", 1);
    object_set_value(door, "celestial_transmission_open", "open", 1);
    object_set_value(door, "celestial_aperture_id", "00000000000000bc", 1);
    set_empty_aperture_contract(true);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "malformed dynamic transmission"), NULL);
    set_empty_aperture_contract(false);
    delete_map(map);

    map = new_v1_map("/placement-only-aperture", 5, 5, CELESTIAL_SKY_OPEN);
    door = new_object(map, 2, 2);
    make_aperture(door, "N", "00000000000000be");
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "overrides its archetype structural role"), NULL);
    delete_map(map);

    map = new_v1_map("/placement-only-transmission", 5, 5, CELESTIAL_SKY_OPEN);
    object *window = new_object(map, 2, 2);
    object_set_value(window, "celestial_transmission", "glass", 1);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "unsupported celestial transmission"), NULL);
    delete_map(map);

    map = new_v1_map("/placement-only-floor", 5, 5, CELESTIAL_SKY_OPEN);
    object *placed_floor = new_object(map, 2, 2);
    SET_FLAG(placed_floor, FLAG_IS_FLOOR);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "overrides its archetype structural role"), NULL);
    delete_map(map);

    map = new_v1_map("/outdoor-zero", 5, 5, CELESTIAL_SKY_OPEN);
    object *authored_outdoor = new_object(map, 1, 1);
    authored_outdoor->celestial_outdoor_authored = true;
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "legacy outdoor toggle"), NULL);
    delete_map(map);

    map = new_v1_map("/dynamic-roof", 5, 5, CELESTIAL_SKY_OPEN);
    door = new_object(map, 2, 2);
    door->type = DOOR;
    door->layer = LAYER_WALL;
    SET_FLAG(door, FLAG_HIDDEN);
    object_set_value(door, "sky_boundary", "1", 1);
    object_set_value(door, "celestial_transmission_closed", "opaque", 1);
    object_set_value(door, "celestial_transmission_open", "open", 1);
    object_set_value(door, "celestial_aperture_id", "00000000000000bd", 1);
    door->celestial_aperture_id_authored = true;
    set_empty_aperture_contract(true);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "invalid sky_boundary"), NULL);
    set_empty_aperture_contract(false);
    delete_map(map);

    map = new_v1_map("/physical-edge", 5, 5, CELESTIAL_SKY_OPEN);
    for (int i = 0; i < 5; i++) {
        int x = i < 3 ? 1 : 2;
        door = new_object(map, x, 2);
        door->type = DOOR;
        object_set_value(door, "celestial_faces", i < 3 ? "E" : "W", 1);
        object_set_value(door, "celestial_transmission_closed", "opaque", 1);
        object_set_value(door, "celestial_transmission_open", "open", 1);
        char id[17];
        snprintf(id, sizeof(id), "%016x", i + 1);
        object_set_value(door, "celestial_aperture_id", id, 1);
        door->celestial_aperture_id_authored = true;
    }
    set_empty_aperture_contract(true);
    ck_assert(!celestial_structure_finalize_map(map, VS(error)));
    ck_assert_ptr_ne(strstr(error, "physical edge"), NULL);
    set_empty_aperture_contract(false);
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

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(
        fputs("arch empty_archetype\noutdoor 999999999999999999999999999999999999\nend\n", input),
        EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("sky_state covered\narch empty_archetype\nend\n", input), EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("sky_state covered\n", input), EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch sky_exposure\nx 0\ny 0\nhp 0\nsp 0\n"
                           "sky_state covered\nspeed 0\nend\n",
                           input),
                     EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch empty_archetype\ncelestial_transmisson glass\nend\n", input), EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(fputs("arch empty_archetype\nname truncated", input), EOF);
    rewind(input);
    parsed = object_get();
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);

    input = tmpfile();
    ck_assert_ptr_ne(input, NULL);
    ck_assert_int_ne(
        fputs("arch empty_archetype\nx 999999999999999999999999999999999999\nend\n", input),
        EOF);
    rewind(input);
    parsed = object_get();
    parsed->map = map = new_v1_map("/huge-coordinate", 5, 5, CELESTIAL_SKY_OPEN);
    ck_assert_int_eq(load_object_fp(input, parsed, 0), LL_ERROR);
    fclose(input);
    object_destroy(parsed);
    delete_map(map);
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

    mapstruct *west = new_v1_map("/west", 5, 5, CELESTIAL_SKY_OPEN);
    mapstruct *east = new_v1_map("/east", 5, 5, CELESTIAL_SKY_OPEN);
    FREE_AND_COPY_HASH(west->tile_path[TILED_EAST], east->path);
    FREE_AND_COPY_HASH(east->tile_path[TILED_WEST], west->path);
    west->tile_map[TILED_EAST] = east;
    east->tile_map[TILED_WEST] = west;
    west->celestial_boundary[TILED_EAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    east->celestial_boundary[TILED_WEST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    set_empty_aperture_contract(true);
    for (int i = 0; i < 5; i++) {
        char id[17];
        snprintf(id, sizeof(id), "%016x", 100 + i);
        object *door = new_object(i < 2 ? west : east, i < 2 ? 4 : 0, 2);
        make_aperture(door, i < 2 ? "E" : "W", id);
    }
    ck_assert_msg(celestial_structure_finalize_map(west, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_finalize_map(east, VS(error)), "%s", error);
    ck_assert(!celestial_structure_validate_topology(west, VS(error)));
    ck_assert_ptr_ne(strstr(error, "on its seam"), NULL);
    set_empty_aperture_contract(false);
    delete_map(west);
    delete_map(east);

    mapstruct *southwest = new_v1_map("/southwest", 5, 5, CELESTIAL_SKY_OPEN);
    mapstruct *northeast = new_v1_map("/northeast", 5, 5, CELESTIAL_SKY_OPEN);
    FREE_AND_COPY_HASH(southwest->tile_path[TILED_NORTHEAST], northeast->path);
    FREE_AND_COPY_HASH(northeast->tile_path[TILED_SOUTHWEST], southwest->path);
    southwest->tile_map[TILED_NORTHEAST] = northeast;
    northeast->tile_map[TILED_SOUTHWEST] = southwest;
    southwest->celestial_boundary[TILED_NORTHEAST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    northeast->celestial_boundary[TILED_SOUTHWEST] = CELESTIAL_BOUNDARY_CONTINUOUS;
    set_empty_aperture_contract(true);
    for (int i = 0; i < 5; i++) {
        char id[17];
        snprintf(id, sizeof(id), "%016x", 200 + i);
        object *door = new_object(i < 2 ? southwest : northeast, i < 2 ? 4 : 0, i < 2 ? 0 : 4);
        make_aperture(door, i < 2 ? "N" : "S", id);
    }
    ck_assert_msg(celestial_structure_finalize_map(southwest, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_finalize_map(northeast, VS(error)), "%s", error);
    ck_assert(!celestial_structure_validate_topology(southwest, VS(error)));
    ck_assert_ptr_ne(strstr(error, "corner"), NULL);
    set_empty_aperture_contract(false);
    delete_map(southwest);
    delete_map(northeast);
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
    SET_FLAG(roof, FLAG_IS_FLOOR);
    object_set_value(roof, "sky_boundary", "1", 1);
    object *storey = new_object(top, 3, 1);
    SET_FLAG(storey, FLAG_IS_FLOOR);
    set_empty_roof_contract(true);

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

    set_empty_roof_contract(false);
    object *upper_redundant = new_object(upper, 3, 1);
    make_metadata(upper_redundant, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(upper_redundant, "sky_state", "covered", 1);
    set_empty_roof_contract(true);
    ck_assert_msg(celestial_structure_finalize_map(upper, VS(error)), "%s", error);
    ck_assert(!celestial_structure_validate_topology(lower, VS(error)));
    ck_assert_ptr_ne(strstr(error, "/stack/upper"), NULL);
    upper->celestial_rectangle_count--;
    set_empty_roof_contract(false);

    object *house_cover = new_object(lower, 4, 4);
    make_metadata(house_cover, CELESTIAL_RECT_SKY_COVERED);
    object_set_value(house_cover, "sky_state", "covered", 1);
    ck_assert_msg(celestial_structure_finalize_map(lower, VS(error)), "%s", error);
    ck_assert_msg(celestial_structure_validate_topology(lower, VS(error)), "%s", error);
    ck_assert(!celestial_structure_cell_exposed(lower, 4, 4));
    ck_assert(celestial_structure_cell_exposed(lower, 4, 3));

    object *covered = new_object(lower, 1, 1);
    make_metadata(covered, CELESTIAL_RECT_SKY_COVERED);
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
    SET_FLAG(&arches[ARCH_EMPTY_ARCHETYPE]->clone, FLAG_BLOCKSVIEW);
    char error[HUGE_BUF];
    ck_assert_msg(celestial_structure_finalize_map(map, VS(error)), "%s", error);
    CLEAR_FLAG(&arches[ARCH_EMPTY_ARCHETYPE]->clone, FLAG_BLOCKSVIEW);

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
#ifndef WIN32
    cookie_io_functions_t failing_io = {.write = fail_inventory_write};
    FILE *failing_fp = fopencookie(NULL, "w", failing_io);
    ck_assert_ptr_ne(failing_fp, NULL);
    ck_assert_int_eq(setvbuf(failing_fp, NULL, _IONBF, 0), 0);
    ck_assert(!celestial_structure_inventory(map, failing_fp, 2));
    fclose(failing_fp);
#endif
    free(first);
    free(second);
    free(repeat);
    delete_map(map);

    map = new_v1_map("/inventory-links", 3, 3, CELESTIAL_SKY_LINKED);
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/inventory-upper");
    map->celestial_boundary[TILED_UP] = CELESTIAL_BOUNDARY_CONTINUOUS;
    char *links = NULL;
    size_t links_size = 0;
    FILE *links_fp = open_memstream(&links, &links_size);
    ck_assert(celestial_structure_inventory(map, links_fp, 2));
    ck_assert_int_eq(fclose(links_fp), 0);
    ck_assert_ptr_ne(strstr(links, "\tlink\t/inventory-links\t9\t/inventory-upper\tcontinuous\n"),
                     NULL);
    free(links);
    delete_map(map);

#ifndef WIN32
    char temporary_maps[] = "/tmp/atrinik-celestial-inventory-XXXXXX";
    ck_assert_ptr_ne(mkdtemp(temporary_maps), NULL);
    char west_path[HUGE_BUF];
    char east_path[HUGE_BUF];
    snprintf(VS(west_path), "%s/west", temporary_maps);
    snprintf(VS(east_path), "%s/east", temporary_maps);
    FILE *west = fopen(west_path, "w");
    FILE *east = fopen(east_path, "w");
    ck_assert_ptr_ne(west, NULL);
    ck_assert_ptr_ne(east, NULL);
    ck_assert_int_ne(
        fputs("arch map\ncelestial_schema 1\nsky_above open\ndifficulty 1\nwidth 3\nheight 3\n"
              "tile_path_2 /east\ncelestial_boundary_2 continuous\nend\n",
              west),
        EOF);
    ck_assert_int_ne(
        fputs("arch map\ncelestial_schema 1\nsky_above open\ndifficulty 1\nwidth 3\nheight 3\n"
              "tile_path_4 /west\ncelestial_boundary_4 continuous\nend\n",
              east),
        EOF);
    ck_assert_int_eq(fclose(west), 0);
    ck_assert_int_eq(fclose(east), 0);
    char saved_mapspath[MAX_BUF];
    char saved_inventory_maps[HUGE_BUF];
    snprintf(VS(saved_mapspath), "%s", settings.mapspath);
    snprintf(VS(saved_inventory_maps), "%s", settings.celestial_inventory_maps);
    uint16_t saved_inventory_limit = settings.celestial_inventory_limit;
    snprintf(VS(settings.mapspath), "%s", temporary_maps);
    size_t forward_size;
    size_t reverse_size;
    char *forward = run_inventory_capture("/west,/east", &forward_size);
    char *reverse = run_inventory_capture("/east,/west", &reverse_size);
    ck_assert_uint_eq(forward_size, reverse_size);
    ck_assert_msg(memcmp(forward, reverse, forward_size) == 0,
                  "forward:\n%s\nreverse:\n%s",
                  forward,
                  reverse);
    ck_assert_ptr_ne(strstr(forward, "\tmap\t/east\t"), NULL);
    ck_assert_ptr_ne(strstr(forward, "\tmap\t/west\t"), NULL);
    free(forward);
    free(reverse);
    snprintf(VS(settings.mapspath), "%s", saved_mapspath);
    snprintf(VS(settings.celestial_inventory_maps), "%s", saved_inventory_maps);
    settings.celestial_inventory_limit = saved_inventory_limit;
    ck_assert_int_eq(unlink(west_path), 0);
    ck_assert_int_eq(unlink(east_path), 0);
    ck_assert_int_eq(rmdir(temporary_maps), 0);
#endif
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("celestial_structure");
    TCase *tc_core = tcase_create("Core");
    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_header_round_trip_is_canonical_and_rejects_legacy_fields);
    tcase_add_test(tc_core, test_saved_v1_map_swaps_and_reloads_mutable_state);
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
