/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <global.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <initialization.h>
#include <server_main.h>
#include <player.h>
#include <object.h>
#include <los.h>
#include <spatial.h>
#include <arch.h>
#include <signal.h>

static void fixture(mapstruct **map, object **pl) {
    check_setup_env_pl(map, pl);
    object_remove(*pl, 0);
    (*pl)->x = 12;
    (*pl)->y = 12;
    *pl = object_insert_map(*pl, *map, NULL, 0);
    ck_assert_ptr_nonnull(*pl);
    CONTR(*pl)->tls = 0;
    CLEAR_FLAG(*pl, FLAG_BLIND);
    CLEAR_FLAG(*pl, FLAG_XRAYS);
}

static object *place(mapstruct *map, const char *name, int x, int y) {
    object *op = arch_get(name);
    ck_assert_ptr_nonnull(op);
    op->x = x;
    op->y = y;
    op = object_insert_map(op, map, NULL, 0);
    ck_assert_ptr_nonnull(op);
    return op;
}

static mapstruct *add_level(mapstruct *base, int depth, int top) {
    mapstruct *upper = get_empty_map(24, 24);
    base->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = base;
    upper->coords[2] = depth;
    upper->level_min = 0;
    upper->level_max = top;
    return upper;
}

START_TEST(test_roof_only_cover_prevents_fresh_gameplay_visibility) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
    object *roof = place(upper, "roof_thatch", 13, 12);
    ck_assert(object_is_roof_surface(roof));
    ck_assert(los_player_has_geometric_sight(pl, map, 13, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    ck_assert_int_eq(los_player_cell_camera(pl, map, 13, 12), LOS_CAMERA_WALL_BOUNDARY);
    /* Stale player masks cannot restore action authority. */
    memset(CONTR(pl)->blocked_los_depth, 0, sizeof(CONTR(pl)->blocked_los_depth));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    object_remove(roof, 0);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
}
END_TEST

START_TEST(test_full_stack_cover_is_not_clipped_to_displayed_depths) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 12;
    mapstruct *top = map;
    for (int z = 1; z <= 12; z++) {
        top = add_level(top, z, 12);
    }
    place(top, "roof_thatch", 13, 12);
    ck_assert(los_view_matches_default_for_test(pl));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    update_los(pl);
    ck_assert_int_eq(CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(0)]
                                               [CONTR(pl)->cs->mapx_2 + 1]
                                               [CONTR(pl)->cs->mapy_2],
                     LOS_CAMERA_WALL_BOUNDARY);
}
END_TEST

START_TEST(test_depth_masks_use_each_ray_instead_of_base_silhouette) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 3;
    mapstruct *middle = add_level(map, 1, 3);
    mapstruct *second = add_level(middle, 2, 3);
    mapstruct *upper = add_level(second, 3, 3);
    place(map, "wall_wood_1", 13, 12);
    ck_assert(!los_player_has_geometric_sight(pl, map, 14, 12));
    /* dz=3 crosses UP before EAST; dz=2 would graze the base wall
     * at an X/Z tie and must remain blocked by conservative supercover. */
    ck_assert(!los_player_has_geometric_sight(pl, second, 14, 12));
    ck_assert(los_player_has_geometric_sight(pl, upper, 14, 12));
    update_los(pl);
    int ax = CONTR(pl)->cs->mapx_2 + 2, ay = CONTR(pl)->cs->mapy_2;
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(0)][ax][ay] & BLOCKED_LOS_BLOCKED);
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(2)][ax][ay] & BLOCKED_LOS_BLOCKED);
    ck_assert(!(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(3)][ax][ay] & BLOCKED_LOS_BLOCKED));
    ck_assert_int_eq(CONTR(pl)->blocked_los[ax][ay],
                     CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(0)][ax][ay]);
}
END_TEST

START_TEST(test_cutaway_does_not_open_physical_slabs) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    place(upper, "water_still", 12, 12);
    ck_assert_int_eq(los_player_cell_camera(pl, map, 12, 12), LOS_CAMERA_VISIBLE);
    ck_assert_int_eq(los_player_cell_camera(pl, upper, 12, 12), LOS_CAMERA_HIDDEN);
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    SET_FLAG(pl, FLAG_XRAYS);
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, upper, 12, 12));
}
END_TEST

START_TEST(test_blind_and_xray_exceptions_are_bounded_and_same_story) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    place(map, "wall_wood_1", 13, 12);
    SET_FLAG(pl, FLAG_XRAYS);
    ck_assert(los_player_can_see_cell(pl, map, 15, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 16, 12));
    SET_FLAG(pl, FLAG_BLIND);
    ck_assert(los_player_can_see_cell(pl, map, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    update_los(pl);
    ck_assert(CONTR(pl)->blocked_los[CONTR(pl)->cs->mapx_2 + 1][CONTR(pl)->cs->mapy_2] &
              BLOCKED_LOS_BLOCKED);
}
END_TEST

START_TEST(test_supercover_corners_do_not_expand_gameplay_sight) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    place(map, "wall_wood_1", 13, 12);
    place(map, "wall_wood_1", 12, 13);
    ck_assert(!los_player_can_see_cell(pl, map, 13, 13));
    ck_assert(!los_player_can_see_cell(pl, map, 14, 14));
}
END_TEST

START_TEST(test_floor_top_does_not_hide_occupants_from_camera) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    place(upper, "water_still", 14, 12);
    ck_assert_int_eq(los_player_cell_camera(pl, upper, 14, 12), LOS_CAMERA_VISIBLE);
    ck_assert_int_eq(los_player_cell_camera(pl, map, 14, 12), LOS_CAMERA_WALL_BOUNDARY);
}
END_TEST

START_TEST(test_unavailable_link_and_tls_exception_fail_deliberately) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/unavailable-upper");
    ck_assert(!los_player_can_see_cell(pl, map, 13, 12));
    CONTR(pl)->tls = 1;
    SET_FLAG(pl, FLAG_BLIND);
    ck_assert(los_player_can_see_cell(pl, map, 13, 12));
}
END_TEST

START_TEST(test_camera_hidden_depths_are_pruned_before_sight_checks) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 5;
    cs->mapx_2 = cs->mapy_2 = 2;
    map->level_min = -1;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    upper->level_min = -1;
    mapstruct *lower = get_empty_map(24, 24);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    lower->coords[2] = -1;
    lower->level_min = -1;
    lower->level_max = 1;
    object *center_roof = NULL, *center_floor = NULL;
    for (int x = 10; x <= 14; x++) {
        for (int y = 10; y <= 14; y++) {
            object *roof = place(upper, "roof_thatch", x, y);
            object *floor = place(map, "water_still", x, y);
            if (x == 12 && y == 12) {
                center_roof = roof;
                center_floor = floor;
            }
        }
    }
    update_los(pl);
    size_t cells = (size_t)cs->mapx * cs->mapy;
    /* The connected overhead cutaway and current floor completely hide
     * their upper/lower depths. Each resolved cell is classified once. */
    ck_assert_uint_eq(los_view_camera_pruned_for_test(1), cells);
    ck_assert_uint_eq(los_view_camera_pruned_for_test(-1), cells);
    ck_assert_uint_eq(los_view_sight_checks_for_test(1), 0);
    ck_assert_uint_eq(los_view_sight_checks_for_test(-1), 0);
    ck_assert_uint_eq(los_view_sight_checks_for_test(0), cells);
    ck_assert_uint_le(los_view_cell_resolves_for_test(),
                      cells * (SPATIAL_STACK_LEVELS - 1 + MAP2_MAX_DEPTH + 1));
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        ck_assert_uint_le(los_view_sight_checks_for_test(depth) +
                          los_view_camera_pruned_for_test(depth), cells);
    }
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(1)][2][2] & BLOCKED_LOS_BLOCKED);
    ck_assert(CONTR(pl)->blocked_los_depth[MAP2_DEPTH_INDEX(-1)][2][2] & BLOCKED_LOS_BLOCKED);
    /* A cutaway affects camera eligibility alone. The fresh physical query
     * remains authoritative and is separately blocked by the current slab. */
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(!los_player_can_see_cell(pl, upper, 12, 12));
    object_remove(center_roof, 0);
    object_remove(center_floor, 0);
    /* Geometry changed after the cached prepass. Physical queries and fresh
     * player actions see the new opening without consulting its stale mask. */
    ck_assert(los_player_has_geometric_sight(pl, upper, 12, 12));
    ck_assert(los_player_can_see_cell(pl, upper, 12, 12));
    ck_assert_uint_eq(CONTR(pl)->camera_visibility[MAP2_DEPTH_INDEX(1)][2][2], LOS_CAMERA_HIDDEN);
    ck_assert_uint_eq(los_view_sight_checks_for_test(1), 0);
}
END_TEST

START_TEST(test_scoped_cell_cache_matches_default_across_seams_and_invalid_stacks) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 5;
    cs->mapx_2 = cs->mapy_2 = 2;
    object_remove(pl, 0);
    pl->x = 22;
    pl = object_insert_map(pl, map, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    mapstruct *east = get_empty_map(24, 24);
    east->level_min = 0;
    east->level_max = 1;
    mapstruct *east_upper = add_level(east, 1, 1);
    map->tile_map[TILED_EAST] = east;
    east->tile_map[TILED_WEST] = map;
    upper->tile_map[TILED_EAST] = east_upper;
    east_upper->tile_map[TILED_WEST] = upper;
    ck_assert(los_view_matches_default_for_test(pl));
    upper->tile_map[TILED_DOWN] = NULL;
    ck_assert(los_view_matches_default_for_test(pl));
    upper->tile_map[TILED_DOWN] = map;
    upper->width = 23;
    ck_assert(los_view_matches_default_for_test(pl));
    upper->width = 24;
    FREE_AND_COPY_HASH(upper->tile_path[TILED_UP], "/missing-typed-upper");
    ck_assert(los_view_matches_default_for_test(pl));
    /* Every in-origin column shares nineteen default lookups. The five seam
     * columns retain their own exact resolution, once per queried depth. */
    update_los(pl);
    ck_assert_uint_le(los_view_cell_resolves_for_test(),
                      (SPATIAL_STACK_LEVELS - 1 + MAP2_MAX_DEPTH + 1) * (1 + cs->mapy));
}
END_TEST

static void assert_cached_masks_match_default(object *pl) {
    uint8_t camera[MAP2_LEVELS][MAP_CLIENT_X][MAP_CLIENT_Y];
    uint8_t sight[MAP2_LEVELS][MAP_CLIENT_X][MAP_CLIENT_Y];
    int base[MAP_CLIENT_X][MAP_CLIENT_Y];
    update_los(pl);
    memcpy(camera, CONTR(pl)->camera_visibility, sizeof(camera));
    memcpy(sight, CONTR(pl)->blocked_los_depth, sizeof(sight));
    memcpy(base, CONTR(pl)->blocked_los, sizeof(base));
    los_update_uncached_for_test(pl);
    ck_assert_mem_eq(camera, CONTR(pl)->camera_visibility, sizeof(camera));
    ck_assert_mem_eq(sight, CONTR(pl)->blocked_los_depth, sizeof(sight));
    ck_assert_mem_eq(base, CONTR(pl)->blocked_los, sizeof(base));
}

START_TEST(test_cached_and_default_whole_masks_match_across_mixed_topology) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 5;
    cs->mapx_2 = cs->mapy_2 = 2;
    object_remove(pl, 0);
    pl->x = 22;
    pl = object_insert_map(pl, map, NULL, 0);
    ck_assert_ptr_nonnull(pl);
    map->level_min = -1;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    mapstruct *east = get_empty_map(24, 24);
    mapstruct *east_upper = add_level(east, 1, 1);
    mapstruct *lower = get_empty_map(24, 24);
    mapstruct *east_lower = get_empty_map(24, 24);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    east->tile_map[TILED_DOWN] = east_lower;
    east_lower->tile_map[TILED_UP] = east;
    lower->coords[2] = east_lower->coords[2] = -1;
    mapstruct *west_levels[] = {lower, map, upper};
    mapstruct *east_levels[] = {east_lower, east, east_upper};
    for (size_t i = 0; i < arraysize(west_levels); i++) {
        west_levels[i]->level_min = east_levels[i]->level_min = -1;
        west_levels[i]->level_max = east_levels[i]->level_max = 1;
        west_levels[i]->tile_map[TILED_EAST] = east_levels[i];
        east_levels[i]->tile_map[TILED_WEST] = west_levels[i];
    }
    place(map, "water_still", 21, 12);
    place(upper, "roof_thatch", 23, 13);
    place(east, "wall_wood_1", 0, 11);
    place(east_upper, "roof_thatch", 0, 12);
    assert_cached_masks_match_default(pl);
    /* A vertical/horizontal diamond whose reverse edge names another storey
     * fails in precisely the same cells for the cached/default traversals. */
    east_upper->tile_map[TILED_WEST] = map;
    assert_cached_masks_match_default(pl);
    east_upper->tile_map[TILED_WEST] = upper;
    /* Local upper absence cannot be extrapolated to the neighboring column,
     * whose complete reciprocal upper stack remains loaded and queryable. */
    map->tile_map[TILED_UP] = NULL;
    upper->tile_map[TILED_DOWN] = NULL;
    FREE_AND_COPY_HASH(map->tile_path[TILED_UP], "/unavailable-upper");
    assert_cached_masks_match_default(pl);
    map->tile_map[TILED_UP] = upper;
    upper->tile_map[TILED_DOWN] = map;
    /* An invalid local stack must retain default fail-closed behavior. */
    lower->tile_map[TILED_UP] = NULL;
    assert_cached_masks_match_default(pl);
    lower->tile_map[TILED_UP] = map;
}
END_TEST

START_TEST(test_zero_volume_proofs_skip_empty_rays_and_respect_current_slabs) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 5;
    cs->mapx_2 = cs->mapy_2 = 2;
    map->level_min = 0;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    size_t cells = (size_t)cs->mapx * cs->mapy;
    update_los(pl);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(0), cells);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(1), cells);
    ck_assert_uint_eq(los_view_ray_traces_for_test(0), 0);
    ck_assert_uint_eq(los_view_ray_traces_for_test(1), 0);
    assert_cached_masks_match_default(pl);

    /* Current-level floors do not block same-story or upward rays. */
    object *floor = place(map, "water_still", 12, 12);
    update_los(pl);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(0), cells);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(1), cells);
    ck_assert_uint_eq(los_view_ray_traces_for_test(1), 0);
    assert_cached_masks_match_default(pl);
    object_remove(floor, 0);

    /* An upper floor owns the crossed DOWN boundary. Its presence in every
     * endpoint box forbids the zero proof, even for rays that avoid the slab. */
    floor = place(upper, "water_still", 12, 12);
    update_los(pl);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(1), 0);
    ck_assert_uint_gt(los_view_ray_traces_for_test(1), 0);
    ck_assert(!los_player_has_geometric_sight(pl, upper, 12, 12));
    assert_cached_masks_match_default(pl);
    object_remove(floor, 0);
    update_los(pl);
    ck_assert_uint_eq(los_view_clear_proofs_for_test(1), cells);
    ck_assert_uint_eq(los_view_ray_traces_for_test(1), 0);
}
END_TEST

START_TEST(test_zero_volume_proof_matches_supercover_in_varied_blocked_scenes) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    socket_struct *cs = CONTR(pl)->cs;
    cs->mapx = cs->mapy = 7;
    cs->mapx_2 = cs->mapy_2 = 3;
    map->level_min = -1;
    map->level_max = 1;
    mapstruct *upper = add_level(map, 1, 1);
    upper->level_min = -1;
    mapstruct *lower = get_empty_map(24, 24);
    map->tile_map[TILED_DOWN] = lower;
    lower->tile_map[TILED_UP] = map;
    lower->coords[2] = -1;
    lower->level_min = -1;
    lower->level_max = 1;
    place(map, "wall_wood_1", 13, 12);
    place(map, "wall_wood_1", 12, 13);
    place(upper, "roof_thatch", 14, 13);
    place(upper, "water_still", 11, 12);
    place(map, "water_still", 10, 11);
    place(lower, "wall_wood_1", 11, 11);
    update_los(pl);
    ck_assert_uint_gt(los_view_clear_proofs_for_test(0), 0);
    ck_assert_uint_gt(los_view_ray_traces_for_test(0), 0);
    ck_assert(!los_player_has_geometric_sight(pl, map, 13, 13));
    assert_cached_masks_match_default(pl);
    /* Add obstacles in all quadrants, including a hidden wall roof and floors
     * on either side of the current story; the exact traversal remains the
     * reference for every cell and depth, including conservative corner ties. */
    place(map, "wall_wood_1", 11, 12);
    place(map, "wall_wood_1", 12, 11);
    place(upper, "roof_thatch", 10, 10);
    place(lower, "water_still", 14, 14);
    assert_cached_masks_match_default(pl);
}
END_TEST

START_TEST(test_idle_masks_reuse_and_player_inputs_rebuild) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    update_los(pl);
    uint64_t builds = los_view_builds_for_test();
    uint64_t hits = los_view_cache_hits_for_test();
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), builds);
    ck_assert_uint_eq(los_view_cache_hits_for_test(), hits + 1);
    ck_assert(los_view_is_current(pl));
    CONTR(pl)->update_los = 1;
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    SET_FLAG(pl, FLAG_BLIND);
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    CLEAR_FLAG(pl, FLAG_BLIND);
    SET_FLAG(pl, FLAG_XRAYS);
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    CONTR(pl)->tls = 1;
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    CONTR(pl)->cs->mapx -= 2;
    CONTR(pl)->cs->mapx_2--;
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    pl->x++;
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
    pl->x--;
    clear_los(pl);
    ck_assert(!los_view_is_current(pl));
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), ++builds);
}
END_TEST

START_TEST(test_nonblocking_actor_reuses_masks_but_cover_invalidates) {
    mapstruct *map;
    object *pl;
    fixture(&map, &pl);
    update_los(pl);
    uint64_t builds = los_view_builds_for_test();
    uint64_t epoch = los_geometry_epoch();
    object *actor = place(map, "kobold", 14, 12);
    object_remove(actor, 0);
    actor->x++;
    ck_assert_ptr_nonnull(object_insert_map(actor, map, NULL, 0));
    ck_assert_uint_eq(los_geometry_epoch(), epoch);
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), builds);
    object *wall = place(map, "wall_wood_1", 13, 12);
    ck_assert_uint_gt(los_geometry_epoch(), epoch);
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), builds + 1);
    ck_assert(CONTR(pl)->blocked_los[CONTR(pl)->cs->mapx_2 + 2]
                                         [CONTR(pl)->cs->mapy_2] & BLOCKED_LOS_BLOCKED);
    object_remove(wall, 0);
    update_los(pl);
    ck_assert_uint_eq(los_view_builds_for_test(), builds + 2);
    ck_assert(!(CONTR(pl)->blocked_los[CONTR(pl)->cs->mapx_2 + 2]
                                          [CONTR(pl)->cs->mapy_2] & BLOCKED_LOS_BLOCKED));
}
END_TEST

START_TEST(test_los_epochs_reach_max_without_wrapping) {
    uint64_t saved_geometry = los_geometry_epoch();
    uint64_t saved_publication = los_publication_epoch();
    los_epochs_set_for_test(UINT64_MAX - 1, UINT64_MAX - 1);
    los_geometry_changed();
    uint64_t geometry = los_geometry_epoch(), publication = los_publication_epoch();
    los_epochs_set_for_test(saved_geometry, saved_publication);
    ck_assert_uint_eq(geometry, UINT64_MAX);
    ck_assert_uint_eq(publication, UINT64_MAX);
}
END_TEST

START_TEST(test_los_geometry_epoch_overflow_is_fatal) {
    los_epochs_set_for_test(UINT64_MAX, 1);
    los_geometry_changed();
    exit(EXIT_SUCCESS); /* A returning/wrapped update must fail the death test. */
}
END_TEST

START_TEST(test_los_publication_epoch_overflow_is_fatal) {
    los_epochs_set_for_test(1, UINT64_MAX);
    los_publication_changed();
    exit(EXIT_SUCCESS);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("los");
    TCase *tc = tcase_create("Core");
    tcase_add_unchecked_fixture(tc, check_setup, check_teardown);
    tcase_add_checked_fixture(tc, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc);
    tcase_add_test(tc, test_los_epochs_reach_max_without_wrapping);
#ifdef NDEBUG
    tcase_add_exit_test(tc, test_los_geometry_epoch_overflow_is_fatal, EXIT_FAILURE);
    tcase_add_exit_test(tc, test_los_publication_epoch_overflow_is_fatal, EXIT_FAILURE);
#else
    tcase_add_test_raise_signal(tc, test_los_geometry_epoch_overflow_is_fatal, SIGABRT);
    tcase_add_test_raise_signal(tc, test_los_publication_epoch_overflow_is_fatal, SIGABRT);
#endif
    tcase_add_test(tc, test_idle_masks_reuse_and_player_inputs_rebuild);
    tcase_add_test(tc, test_nonblocking_actor_reuses_masks_but_cover_invalidates);
    tcase_add_test(tc, test_roof_only_cover_prevents_fresh_gameplay_visibility);
    tcase_add_test(tc, test_full_stack_cover_is_not_clipped_to_displayed_depths);
    tcase_add_test(tc, test_depth_masks_use_each_ray_instead_of_base_silhouette);
    tcase_add_test(tc, test_cutaway_does_not_open_physical_slabs);
    tcase_add_test(tc, test_blind_and_xray_exceptions_are_bounded_and_same_story);
    tcase_add_test(tc, test_supercover_corners_do_not_expand_gameplay_sight);
    tcase_add_test(tc, test_floor_top_does_not_hide_occupants_from_camera);
    tcase_add_test(tc, test_unavailable_link_and_tls_exception_fail_deliberately);
    tcase_add_test(tc, test_camera_hidden_depths_are_pruned_before_sight_checks);
    tcase_add_test(tc, test_scoped_cell_cache_matches_default_across_seams_and_invalid_stacks);
    tcase_add_test(tc, test_cached_and_default_whole_masks_match_across_mixed_topology);
    tcase_add_test(tc, test_zero_volume_proofs_skip_empty_rays_and_respect_current_slabs);
    tcase_add_test(tc, test_zero_volume_proof_matches_supercover_in_varied_blocked_scenes);
    return s;
}

void check_server_los(void) {
    check_run_suite(suite(), __FILE__);
}
