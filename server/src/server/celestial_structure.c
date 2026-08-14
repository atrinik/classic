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

/** @file Strict celestial-v1 structural parsing and validation. */

#include <global.h>

#include <arch.h>
#include <celestial_structure.h>
#include <loader.h>
#include <map.h>
#include <object.h>
#include <initialization.h>
#include <region.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#include <stdarg.h>

#define CELESTIAL_INVENTORY_MAX_ROOTS 16
#define CELESTIAL_INVENTORY_MAX_MAPS 256

static bool set_error(char *error, size_t error_size, const char *format, ...) {
    if (error != NULL && error_size != 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return false;
}

static const char *map_path(const mapstruct *map) {
    return map->path != NULL ? map->path : "<unbound>";
}

celestial_transmission_t celestial_structure_transmission(const char *value) {
    if (value == NULL) {
        return CELESTIAL_TRANSMISSION_INVALID;
    }
    if (strcmp(value, "opaque") == 0) {
        return CELESTIAL_TRANSMISSION_OPAQUE;
    }
    if (strcmp(value, "glass") == 0) {
        return CELESTIAL_TRANSMISSION_GLASS;
    }
    if (strcmp(value, "grate") == 0) {
        return CELESTIAL_TRANSMISSION_GRATE;
    }
    if (strcmp(value, "open") == 0) {
        return CELESTIAL_TRANSMISSION_OPEN;
    }
    return CELESTIAL_TRANSMISSION_INVALID;
}

static uint8_t cardinal_faces(const object *op) {
    const char *value = object_get_value(op, "celestial_faces");
    if (value == NULL || strcmp(value, "all") == 0) {
        return CELESTIAL_FACE_NORTH | CELESTIAL_FACE_EAST | CELESTIAL_FACE_SOUTH |
               CELESTIAL_FACE_WEST;
    }

    size_t value_length = strlen(value);
    if (value_length == 0 || value[value_length - 1] == ',') {
        return 0;
    }

    uint8_t result = 0;
    const char *expected[] = {"N", "E", "S", "W"};
    const uint8_t bits[] = {CELESTIAL_FACE_NORTH,
                            CELESTIAL_FACE_EAST,
                            CELESTIAL_FACE_SOUTH,
                            CELESTIAL_FACE_WEST};
    size_t position = 0;
    for (size_t i = 0; i < arraysize(expected); i++) {
        size_t length = strlen(expected[i]);
        if (strncmp(value + position, expected[i], length) == 0 &&
            (value[position + length] == ',' || value[position + length] == '\0')) {
            result |= bits[i];
            position += length;
            if (value[position] == ',') {
                position++;
            }
        }
    }
    return value[position] == '\0' && result != 0 ? result : 0;
}

uint8_t celestial_structure_faces(const object *op) {
    HARD_ASSERT(op != NULL);

    bool horizontal = QUERY_FLAG(op, FLAG_IS_FLOOR) ||
                      (op->layer == LAYER_WALL && QUERY_FLAG(op, FLAG_HIDDEN) &&
                       strcmp(STRING_SAFE(object_get_value(op, "sky_boundary")), "1") == 0);
    if (horizontal) {
        return CELESTIAL_FACE_DOWN;
    }

    if (op->type == DOOR || op->type == GATE || QUERY_FLAG(op, FLAG_BLOCKSVIEW) ||
        celestial_structure_transmission(object_get_value(op, "celestial_transmission")) ==
            CELESTIAL_TRANSMISSION_GLASS ||
        celestial_structure_transmission(object_get_value(op, "celestial_transmission")) ==
            CELESTIAL_TRANSMISSION_GRATE) {
        return cardinal_faces(op);
    }
    return 0;
}

static bool valid_aperture_id(const char *value) {
    if (value == NULL || strlen(value) != 16 || strcmp(value, "0000000000000000") == 0) {
        return false;
    }
    for (size_t i = 0; i < 16; i++) {
        if (!isdigit((unsigned char)value[i]) && (value[i] < 'a' || value[i] > 'f')) {
            return false;
        }
    }
    return true;
}

static bool archetype_value_matches(const object *op, const char *key, const char *value) {
    if (op->arch == NULL || value == NULL) {
        return false;
    }
    const char *archetype_value = object_get_value(&op->arch->clone, key);
    return archetype_value != NULL && strcmp(archetype_value, value) == 0;
}

static bool validate_archetype_clone(const archetype_t *at, char *error, size_t error_size) {
    const object *op = &at->clone;
    const char *sky_boundary = object_get_value(op, "sky_boundary");
    const char *faces = object_get_value(op, "celestial_faces");
    const char *transmission = object_get_value(op, "celestial_transmission");
    const char *closed = object_get_value(op, "celestial_transmission_closed");
    const char *open = object_get_value(op, "celestial_transmission_open");
    const char *aperture_id = object_get_value(op, "celestial_aperture_id");
    bool dynamic = op->type == DOOR || op->type == GATE;
    bool floor = QUERY_FLAG(op, FLAG_IS_FLOOR);
    bool opaque = QUERY_FLAG(op, FLAG_BLOCKSVIEW);
    celestial_transmission_t static_value = celestial_structure_transmission(transmission);
    bool vertical = dynamic || opaque || static_value == CELESTIAL_TRANSMISSION_GLASS ||
                    static_value == CELESTIAL_TRANSMISSION_GRATE;

    if (strcmp(at->name, "sky_exposure") == 0 || strcmp(at->name, "ambient_light_zone") == 0 ||
        object_get_value(op, "sky_state") != NULL ||
        object_get_value(op, "ambient_strength") != NULL || aperture_id != NULL) {
        return set_error(error,
                         error_size,
                         "archetype %s uses reserved celestial metadata",
                         at->name);
    }
    if (sky_boundary != NULL && (strcmp(sky_boundary, "1") != 0 || op->layer != LAYER_WALL ||
                                 !QUERY_FLAG(op, FLAG_HIDDEN) || op->speed != 0.0 || dynamic)) {
        return set_error(error, error_size, "archetype %s has invalid sky_boundary", at->name);
    }
    if (faces != NULL && (floor || sky_boundary != NULL || !vertical || cardinal_faces(op) == 0)) {
        return set_error(error, error_size, "archetype %s has invalid celestial_faces", at->name);
    }
    if (closed != NULL || open != NULL) {
        celestial_transmission_t closed_value = celestial_structure_transmission(closed);
        celestial_transmission_t open_value = celestial_structure_transmission(open);
        if (!dynamic || transmission != NULL || closed_value == CELESTIAL_TRANSMISSION_INVALID ||
            closed_value == CELESTIAL_TRANSMISSION_OPEN ||
            open_value != CELESTIAL_TRANSMISSION_OPEN) {
            return set_error(error,
                             error_size,
                             "archetype %s has malformed dynamic transmission",
                             at->name);
        }
    } else if (dynamic && transmission != NULL) {
        return set_error(error,
                         error_size,
                         "archetype %s mixes static and dynamic transmission",
                         at->name);
    } else if (transmission != NULL && (static_value == CELESTIAL_TRANSMISSION_INVALID ||
                                        static_value == CELESTIAL_TRANSMISSION_OPEN ||
                                        (static_value == CELESTIAL_TRANSMISSION_OPAQUE && !floor &&
                                         !opaque && sky_boundary == NULL))) {
        return set_error(error,
                         error_size,
                         "archetype %s has unsupported celestial transmission",
                         at->name);
    }
    return true;
}

bool celestial_structure_validate_archetypes(char *error, size_t error_size) {
    archetype_t *at;
    archetype_t *tmp;
    HASH_ITER(hh, arch_table, at, tmp) {
        for (archetype_t *part = at; part != NULL; part = part->more) {
            if (!validate_archetype_clone(part, error, error_size)) {
                return false;
            }
        }
    }
    return true;
}

static bool validate_object(const mapstruct *map,
                            const object *op,
                            const char *aperture_ids[CELESTIAL_MAX_RECTANGLES],
                            size_t *aperture_count,
                            char *error,
                            size_t error_size) {
    const char *sky_boundary = object_get_value(op, "sky_boundary");
    const char *faces = object_get_value(op, "celestial_faces");
    const char *transmission = object_get_value(op, "celestial_transmission");
    const char *closed = object_get_value(op, "celestial_transmission_closed");
    const char *open = object_get_value(op, "celestial_transmission_open");
    const char *aperture_id = object_get_value(op, "celestial_aperture_id");
    const char *sky_state = object_get_value(op, "sky_state");
    const char *ambient_strength = object_get_value(op, "ambient_strength");
    bool dynamic = op->type == DOOR || op->type == GATE;
    bool archetype_dynamic =
        op->arch != NULL && (op->arch->clone.type == DOOR || op->arch->clone.type == GATE);
    bool floor = QUERY_FLAG(op, FLAG_IS_FLOOR);
    bool opaque = QUERY_FLAG(op, FLAG_BLOCKSVIEW);
    celestial_transmission_t static_transmission = celestial_structure_transmission(transmission);
    bool vertical = dynamic || opaque || static_transmission == CELESTIAL_TRANSMISSION_GLASS ||
                    static_transmission == CELESTIAL_TRANSMISSION_GRATE;

    if (op->arch == NULL || dynamic != archetype_dynamic ||
        (dynamic && op->type != op->arch->clone.type) ||
        floor != QUERY_FLAG(&op->arch->clone, FLAG_IS_FLOOR) ||
        opaque != QUERY_FLAG(&op->arch->clone, FLAG_BLOCKSVIEW)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) object %s overrides its archetype structural role",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    }

    if (sky_state != NULL || ambient_strength != NULL) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) object %s uses metadata-only celestial fields",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    }

    if (op->celestial_outdoor_authored || QUERY_FLAG(op, FLAG_OUTDOOR)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) object %s uses the legacy outdoor toggle",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    }
    if (sky_boundary != NULL &&
        (strcmp(sky_boundary, "1") != 0 || op->layer != LAYER_WALL ||
         !QUERY_FLAG(op, FLAG_HIDDEN) || op->speed != 0.0 || dynamic ||
         !archetype_value_matches(op, "sky_boundary", sky_boundary) ||
         op->arch->clone.layer != LAYER_WALL || !QUERY_FLAG(&op->arch->clone, FLAG_HIDDEN) ||
         op->arch->clone.speed != 0.0)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) object %s has invalid sky_boundary",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    }
    if (faces != NULL && (floor || sky_boundary != NULL || !vertical || cardinal_faces(op) == 0)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) object %s has invalid celestial_faces",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    }

    if (dynamic) {
        celestial_transmission_t closed_value = celestial_structure_transmission(closed);
        celestial_transmission_t open_value = celestial_structure_transmission(open);
        if (transmission != NULL || closed_value == CELESTIAL_TRANSMISSION_INVALID ||
            closed_value == CELESTIAL_TRANSMISSION_OPEN ||
            open_value != CELESTIAL_TRANSMISSION_OPEN || !valid_aperture_id(aperture_id) ||
            !op->celestial_aperture_id_authored ||
            !archetype_value_matches(op, "celestial_transmission_closed", closed) ||
            !archetype_value_matches(op, "celestial_transmission_open", open) ||
            (op->arch != NULL &&
             object_get_value(&op->arch->clone, "celestial_aperture_id") != NULL)) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) object %s has malformed dynamic transmission",
                             map_path(map),
                             op->x,
                             op->y,
                             STRING_SAFE(op->name));
        }
        if (*aperture_count >= 256) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) object %s exceeds 256 dynamic apertures",
                             map_path(map),
                             op->x,
                             op->y,
                             STRING_SAFE(op->name));
        }
        for (size_t i = 0; i < *aperture_count; i++) {
            if (strcmp(aperture_ids[i], aperture_id) == 0) {
                return set_error(error,
                                 error_size,
                                 "%s (%d,%d) duplicates celestial aperture %s",
                                 map_path(map),
                                 op->x,
                                 op->y,
                                 aperture_id);
            }
        }
        aperture_ids[(*aperture_count)++] = aperture_id;
    } else if (closed != NULL || open != NULL || aperture_id != NULL) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) non-aperture object %s has dynamic transmission fields",
                         map_path(map),
                         op->x,
                         op->y,
                         STRING_SAFE(op->name));
    } else if (transmission != NULL) {
        bool structural = floor || opaque || sky_boundary != NULL ||
                          static_transmission == CELESTIAL_TRANSMISSION_GLASS ||
                          static_transmission == CELESTIAL_TRANSMISSION_GRATE;
        if (static_transmission == CELESTIAL_TRANSMISSION_INVALID || !structural ||
            static_transmission == CELESTIAL_TRANSMISSION_OPEN ||
            !archetype_value_matches(op, "celestial_transmission", transmission)) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) object %s has unsupported celestial transmission",
                             map_path(map),
                             op->x,
                             op->y,
                             STRING_SAFE(op->name));
        }
    }
    return true;
}

static const char *metadata_kind(const object *op) {
    if (op->celestial_metadata_kind == CELESTIAL_RECT_SKY_COVERED) {
        return "sky_exposure";
    }
    if (op->celestial_metadata_kind == CELESTIAL_RECT_AMBIENT) {
        return "ambient_light_zone";
    }
    return NULL;
}

static bool object_has_celestial_marker(const object *op) {
    return metadata_kind(op) != NULL || op->celestial_outdoor_authored ||
           object_get_value(op, "sky_boundary") != NULL ||
           object_get_value(op, "sky_state") != NULL ||
           object_get_value(op, "ambient_strength") != NULL ||
           object_get_value(op, "celestial_faces") != NULL ||
           object_get_value(op, "celestial_transmission") != NULL ||
           object_get_value(op, "celestial_transmission_closed") != NULL ||
           object_get_value(op, "celestial_transmission_open") != NULL ||
           object_get_value(op, "celestial_aperture_id") != NULL;
}

static bool validate_nested_objects(const mapstruct *map,
                                    const object *parent,
                                    char *error,
                                    size_t error_size) {
    for (const object *child = parent->inv; child != NULL; child = child->below) {
        if (object_has_celestial_marker(child)) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) nested object %s has celestial-v1 fields",
                             map_path(map),
                             parent->x,
                             parent->y,
                             STRING_SAFE(child->name));
        }
        if (!validate_nested_objects(map, child, error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool rectangles_overlap(const celestial_rectangle_t *a, const celestial_rectangle_t *b) {
    bool same_family = (a->type == CELESTIAL_RECT_AMBIENT) == (b->type == CELESTIAL_RECT_AMBIENT);
    return same_family && a->x < b->x + b->width && b->x < a->x + a->width &&
           a->y < b->y + b->height && b->y < a->y + a->height;
}

static bool metadata_has_exact_shape(const object *op, bool sky) {
    object *expected = arch_get("empty_archetype");
    if (expected == NULL) {
        return false;
    }
    expected->x = op->x;
    expected->y = op->y;
    expected->stats.hp = op->stats.hp;
    expected->stats.sp = op->stats.sp;
    expected->layer = LAYER_SYS;
    expected->type = op->type;
    expected->celestial_metadata_kind = op->celestial_metadata_kind;
    CLEAR_FLAG(expected, FLAG_HIDDEN);
    CLEAR_FLAG(expected, FLAG_IS_FLOOR);
    CLEAR_FLAG(expected, FLAG_BLOCKSVIEW);
    const char *structural_keys[] = {"sky_boundary",
                                     "celestial_faces",
                                     "celestial_transmission",
                                     "celestial_transmission_closed",
                                     "celestial_transmission_open",
                                     "celestial_aperture_id"};
    for (size_t i = 0; i < arraysize(structural_keys); i++) {
        object_set_value(expected, structural_keys[i], NULL, 0);
    }
    const char *key = sky ? "sky_state" : "ambient_strength";
    const char *value = object_get_value(op, key);
    if (value != NULL) {
        object_set_value(expected, key, value, 1);
    }

    StringBuffer *difference = stringbuffer_new();
    get_ob_diff(difference, op, expected);
    bool exact = stringbuffer_length(difference) == 0;
    char *contents = stringbuffer_finish(difference);
    free(contents);
    object_destroy(expected);
    return exact;
}

static bool
add_metadata(mapstruct *map, const object *op, const char *kind, char *error, size_t error_size) {
    bool sky = strcmp(kind, "sky_exposure") == 0;
    if (op->arch != arches[ARCH_EMPTY_ARCHETYPE] || op->layer != LAYER_SYS || op->speed != 0.0 ||
        op->inv != NULL || op->randomitems != NULL || op->celestial_outdoor_authored ||
        QUERY_FLAG(op, FLAG_OUTDOOR) || object_get_value(op, "sky_boundary") != NULL ||
        object_get_value(op, "celestial_faces") != NULL ||
        object_get_value(op, "celestial_transmission") != NULL ||
        object_get_value(op, "celestial_transmission_closed") != NULL ||
        object_get_value(op, "celestial_transmission_open") != NULL ||
        object_get_value(op, "celestial_aperture_id") != NULL ||
        (sky && object_get_value(op, "ambient_strength") != NULL) ||
        (!sky && object_get_value(op, "sky_state") != NULL)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) %s record has conflicting object fields",
                         map_path(map),
                         op->x,
                         op->y,
                         kind);
    }
    if (!metadata_has_exact_shape(op, sky)) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) %s record has fields outside its exact schema",
                         map_path(map),
                         op->x,
                         op->y,
                         kind);
    }
    if (map->celestial_rectangle_count >= CELESTIAL_MAX_RECTANGLES || op->x < 0 || op->y < 0 ||
        op->stats.hp < 0 || op->stats.sp < 0 || op->x >= map->width || op->y >= map->height ||
        op->stats.hp >= map->width - op->x || op->stats.sp >= map->height - op->y) {
        return set_error(error,
                         error_size,
                         "%s (%d,%d) %s rectangle is out of bounds or exceeds the limit",
                         map_path(map),
                         op->x,
                         op->y,
                         kind);
    }

    celestial_rectangle_t rectangle = {
        .x = (uint8_t)op->x,
        .y = (uint8_t)op->y,
        .width = (uint8_t)(op->stats.hp + 1),
        .height = (uint8_t)(op->stats.sp + 1),
    };
    if (sky) {
        const char *state = object_get_value(op, "sky_state");
        if (state != NULL && strcmp(state, "open") == 0) {
            rectangle.type = CELESTIAL_RECT_SKY_OPEN;
            if (map->celestial_sky_above != CELESTIAL_SKY_SEALED) {
                return set_error(error,
                                 error_size,
                                 "%s (%d,%d) open exception requires sky_above sealed",
                                 map_path(map),
                                 op->x,
                                 op->y);
            }
        } else if (state != NULL && strcmp(state, "covered") == 0) {
            rectangle.type = CELESTIAL_RECT_SKY_COVERED;
            if (map->celestial_sky_above == CELESTIAL_SKY_SEALED) {
                return set_error(error,
                                 error_size,
                                 "%s (%d,%d) covered exception has no open upper boundary",
                                 map_path(map),
                                 op->x,
                                 op->y);
            }
        } else {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) sky exception has invalid sky_state",
                             map_path(map),
                             op->x,
                             op->y);
        }
    } else {
        const char *strength = object_get_value(op, "ambient_strength");
        uint64_t parsed;
        if (strength == NULL || !string_parse_uint64(strength, 10, 0, 40959, &parsed)) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) ambient zone has invalid strength",
                             map_path(map),
                             op->x,
                             op->y);
        }
        rectangle.type = CELESTIAL_RECT_AMBIENT;
        rectangle.value = (uint16_t)parsed;
    }

    for (size_t i = 0; i < map->celestial_rectangle_count; i++) {
        if (rectangles_overlap(&map->celestial_rectangles[i], &rectangle)) {
            return set_error(error,
                             error_size,
                             "%s (%d,%d) metadata rectangle overlaps an earlier rectangle",
                             map_path(map),
                             op->x,
                             op->y);
        }
    }
    map->celestial_rectangles =
        xrealloc(map->celestial_rectangles,
                 (map->celestial_rectangle_count + 1) * sizeof(*map->celestial_rectangles));
    map->celestial_rectangles[map->celestial_rectangle_count++] = rectangle;
    return true;
}

static int compare_rectangles(const void *left, const void *right) {
    const celestial_rectangle_t *a = left;
    const celestial_rectangle_t *b = right;
    if (a->y != b->y) {
        return (int)a->y - (int)b->y;
    }
    if (a->x != b->x) {
        return (int)a->x - (int)b->x;
    }
    return (int)a->type - (int)b->type;
}

bool celestial_structure_validate_header(mapstruct *map, char *error, size_t error_size) {
    if (!map->celestial_schema_seen || map->celestial_schema != 1 || !map->celestial_sky_seen ||
        map->celestial_header_invalid || map->celestial_tile_path_invalid) {
        return set_error(error, error_size, "%s has malformed celestial-v1 header", map_path(map));
    }
    if (!map->celestial_width_seen || !map->celestial_height_seen || map->width < 1 ||
        map->height < 1 || map->width > 64 || map->height > 64 || map->light_value < 0 ||
        map->light_value > 40959 || map->celestial_legacy_darkness_seen ||
        map->celestial_legacy_outdoor_seen) {
        return set_error(error,
                         error_size,
                         "%s has invalid celestial-v1 map values",
                         map_path(map));
    }
    if (!map->celestial_region_seen) {
        map->region = region_find_by_name("world");
    }
    if (map->region == NULL) {
        return set_error(error, error_size, "%s has no valid region", map_path(map));
    }
    bool has_up = map->tile_path[TILED_UP] != NULL;
    if ((map->celestial_sky_above == CELESTIAL_SKY_LINKED) != has_up ||
        ((map->celestial_sky_above == CELESTIAL_SKY_OPEN ||
          map->celestial_sky_above == CELESTIAL_SKY_SEALED) &&
         has_up)) {
        return set_error(error,
                         error_size,
                         "%s sky anchor contradicts its upper link",
                         map_path(map));
    }
    for (size_t i = 0; i < TILED_NUM; i++) {
        if ((map->tile_path[i] != NULL) !=
            (map->celestial_boundary[i] != CELESTIAL_BOUNDARY_UNSET)) {
            return set_error(error,
                             error_size,
                             "%s tile_path_%zu and celestial_boundary_%zu disagree",
                             map_path(map),
                             i + 1,
                             i + 1);
        }
    }
    return true;
}

bool celestial_structure_finalize_map(mapstruct *map, char *error, size_t error_size) {
    HARD_ASSERT(map != NULL);
    if (map->celestial_schema == 0 && !map->celestial_schema_seen) {
        if (map->celestial_v1_header_seen) {
            return set_error(error,
                             error_size,
                             "%s has celestial-v1 records without a schema",
                             map_path(map));
        }
        for (int y = 0; y < map->height; y++) {
            for (int x = 0; x < map->width; x++) {
                for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                    if (object_has_celestial_marker(op)) {
                        return set_error(
                            error,
                            error_size,
                            "%s (%d,%d) has celestial-v1 object fields without a schema",
                            map_path(map),
                            x,
                            y);
                    }
                    if (!validate_nested_objects(map, op, error, error_size)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    if (!celestial_structure_validate_header(map, error, error_size)) {
        return false;
    }

    const char *aperture_ids[CELESTIAL_MAX_RECTANGLES];
    size_t aperture_count = 0;
    uint8_t horizontal_edges[65][64] = {{0}};
    uint8_t vertical_edges[64][65] = {{0}};
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            size_t cell_apertures = 0;
            size_t edge_apertures[4] = {0};
            object *op = GET_MAP_OB(map, x, y);
            while (op != NULL) {
                object *next = op->above;
                if (!validate_nested_objects(map, op, error, error_size)) {
                    return false;
                }
                const char *kind = metadata_kind(op);
                if (kind != NULL) {
                    if (!add_metadata(map, op, kind, error, error_size)) {
                        return false;
                    }
                    object_remove(op, REMOVE_NO_WALK_OFF);
                    object_destroy(op);
                    op = next;
                    continue;
                }
                if (!validate_object(map, op, aperture_ids, &aperture_count, error, error_size)) {
                    return false;
                }
                if (op->type == DOOR || op->type == GATE) {
                    if (++cell_apertures > 4) {
                        return set_error(error,
                                         error_size,
                                         "%s (%d,%d) has more than four dynamic apertures",
                                         map_path(map),
                                         x,
                                         y);
                    }
                    uint8_t faces = cardinal_faces(op);
                    const uint8_t bits[] = {CELESTIAL_FACE_NORTH,
                                            CELESTIAL_FACE_EAST,
                                            CELESTIAL_FACE_SOUTH,
                                            CELESTIAL_FACE_WEST};
                    for (size_t i = 0; i < arraysize(bits); i++) {
                        if ((faces & bits[i]) != 0 && ++edge_apertures[i] > 4) {
                            return set_error(error,
                                             error_size,
                                             "%s (%d,%d) has more than four apertures on one edge",
                                             map_path(map),
                                             x,
                                             y);
                        }
                    }
                    if (((faces & CELESTIAL_FACE_NORTH) != 0 && ++horizontal_edges[y][x] > 4) ||
                        ((faces & CELESTIAL_FACE_SOUTH) != 0 && ++horizontal_edges[y + 1][x] > 4) ||
                        ((faces & CELESTIAL_FACE_WEST) != 0 && ++vertical_edges[y][x] > 4) ||
                        ((faces & CELESTIAL_FACE_EAST) != 0 && ++vertical_edges[y][x + 1] > 4)) {
                        return set_error(
                            error,
                            error_size,
                            "%s (%d,%d) has more than four apertures on one physical edge",
                            map_path(map),
                            x,
                            y);
                    }
                }
                op = next;
            }
        }
    }
    if (map->celestial_rectangle_count > 1) {
        qsort(map->celestial_rectangles,
              map->celestial_rectangle_count,
              sizeof(*map->celestial_rectangles),
              compare_rectangles);
    }
    return true;
}

static bool path_equals(const shstr *left, const shstr *right) {
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static bool rectangle_contains(const celestial_rectangle_t *rectangle, int x, int y) {
    return x >= rectangle->x && x < rectangle->x + rectangle->width && y >= rectangle->y &&
           y < rectangle->y + rectangle->height;
}

static uint8_t sky_exception_at(const mapstruct *map,
                                int x,
                                int y,
                                const mapstruct *ignored_map,
                                const celestial_rectangle_t *ignored_rectangle) {
    for (size_t i = 0; i < map->celestial_rectangle_count; i++) {
        const celestial_rectangle_t *rectangle = &map->celestial_rectangles[i];
        if ((map == ignored_map && rectangle == ignored_rectangle) ||
            rectangle->type == CELESTIAL_RECT_AMBIENT || !rectangle_contains(rectangle, x, y)) {
            continue;
        }
        return rectangle->type;
    }
    return 0;
}

static bool cell_has_down_face(const mapstruct *map, int x, int y) {
    for (const object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
        if ((celestial_structure_faces(op) & CELESTIAL_FACE_DOWN) != 0) {
            return true;
        }
    }
    return false;
}

static bool cell_exposed_ignoring(const mapstruct *map,
                                  int x,
                                  int y,
                                  const mapstruct *ignored_map,
                                  const celestial_rectangle_t *ignored_rectangle) {
    const mapstruct *cursor = map;
    for (size_t depth = 0; depth < MAP2_LEVELS; depth++) {
        uint8_t exception = sky_exception_at(cursor, x, y, ignored_map, ignored_rectangle);
        if (exception == CELESTIAL_RECT_SKY_COVERED) {
            return false;
        }
        if (exception == CELESTIAL_RECT_SKY_OPEN) {
            return true;
        }
        if (cursor->celestial_sky_above == CELESTIAL_SKY_OPEN) {
            return true;
        }
        if (cursor->celestial_sky_above != CELESTIAL_SKY_LINKED ||
            cursor->tile_map[TILED_UP] == NULL) {
            return false;
        }
        cursor = cursor->tile_map[TILED_UP];
        if (cell_has_down_face(cursor, x, y)) {
            return false;
        }
    }
    return false;
}

bool celestial_structure_cell_exposed(const mapstruct *map, int x, int y) {
    HARD_ASSERT(map != NULL);
    if (map->celestial_schema != 1 || x < 0 || y < 0 || x >= map->width || y >= map->height) {
        return false;
    }
    return cell_exposed_ignoring(map, x, y, NULL, NULL);
}

static bool validate_exceptions(mapstruct *map, char *error, size_t error_size) {
    for (size_t i = 0; i < map->celestial_rectangle_count; i++) {
        const celestial_rectangle_t *rectangle = &map->celestial_rectangles[i];
        if (rectangle->type != CELESTIAL_RECT_SKY_COVERED) {
            continue;
        }
        bool redundant = true;
        for (int y = rectangle->y; y < rectangle->y + rectangle->height && redundant; y++) {
            for (int x = rectangle->x; x < rectangle->x + rectangle->width; x++) {
                if (cell_exposed_ignoring(map, x, y, map, rectangle)) {
                    redundant = false;
                    break;
                }
            }
        }
        if (redundant) {
            return set_error(error,
                             error_size,
                             "%s depth 0 cell (%u,%u) covered exception duplicates resolved "
                             "upper structure",
                             map_path(map),
                             rectangle->x,
                             rectangle->y);
        }
    }
    return true;
}

static size_t count_dynamic_face(const mapstruct *map, int x, int y, uint8_t face) {
    size_t count = 0;
    for (const object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
        if ((op->type == DOOR || op->type == GATE) && (cardinal_faces(op) & face) != 0) {
            count++;
        }
    }
    return count;
}

static bool validate_cardinal_seam_apertures(const mapstruct *map,
                                             size_t tile,
                                             char *error,
                                             size_t error_size) {
    const mapstruct *other = map->tile_map[tile];
    if (other == NULL) {
        return true;
    }
    if (tile >= TILED_NORTHEAST && tile <= TILED_NORTHWEST) {
        int x = tile == TILED_NORTHEAST || tile == TILED_SOUTHEAST ? map->width - 1 : 0;
        int y = tile == TILED_SOUTHEAST || tile == TILED_SOUTHWEST ? map->height - 1 : 0;
        int other_x = tile == TILED_NORTHEAST || tile == TILED_SOUTHEAST ? 0 : other->width - 1;
        int other_y = tile == TILED_NORTHEAST || tile == TILED_NORTHWEST ? other->height - 1 : 0;
        uint8_t horizontal = tile == TILED_NORTHEAST || tile == TILED_NORTHWEST
                                 ? CELESTIAL_FACE_NORTH
                                 : CELESTIAL_FACE_SOUTH;
        uint8_t reverse_horizontal =
            horizontal == CELESTIAL_FACE_NORTH ? CELESTIAL_FACE_SOUTH : CELESTIAL_FACE_NORTH;
        uint8_t vertical = tile == TILED_NORTHEAST || tile == TILED_SOUTHEAST ? CELESTIAL_FACE_EAST
                                                                              : CELESTIAL_FACE_WEST;
        uint8_t reverse_vertical =
            vertical == CELESTIAL_FACE_EAST ? CELESTIAL_FACE_WEST : CELESTIAL_FACE_EAST;
        if (count_dynamic_face(map, x, y, horizontal) +
                    count_dynamic_face(other, other_x, other_y, reverse_horizontal) >
                4 ||
            count_dynamic_face(map, x, y, vertical) +
                    count_dynamic_face(other, other_x, other_y, reverse_vertical) >
                4) {
            return set_error(error,
                             error_size,
                             "%s tile_path_%zu corner has more than four apertures on its seam",
                             map_path(map),
                             tile + 1);
        }
        return true;
    }
    if (tile > TILED_WEST) {
        return true;
    }
    int cells = (tile == TILED_NORTH || tile == TILED_SOUTH) ? MIN(map->width, other->width)
                                                             : MIN(map->height, other->height);
    for (int cell = 0; cell < cells; cell++) {
        int x = tile == TILED_EAST ? map->width - 1 : tile == TILED_WEST ? 0 : cell;
        int y = tile == TILED_SOUTH ? map->height - 1 : tile == TILED_NORTH ? 0 : cell;
        int other_x = tile == TILED_WEST ? other->width - 1 : tile == TILED_EAST ? 0 : cell;
        int other_y = tile == TILED_NORTH ? other->height - 1 : tile == TILED_SOUTH ? 0 : cell;
        uint8_t face = tile == TILED_NORTH   ? CELESTIAL_FACE_NORTH
                       : tile == TILED_EAST  ? CELESTIAL_FACE_EAST
                       : tile == TILED_SOUTH ? CELESTIAL_FACE_SOUTH
                                             : CELESTIAL_FACE_WEST;
        uint8_t reverse_face = tile == TILED_NORTH   ? CELESTIAL_FACE_SOUTH
                               : tile == TILED_EAST  ? CELESTIAL_FACE_WEST
                               : tile == TILED_SOUTH ? CELESTIAL_FACE_NORTH
                                                     : CELESTIAL_FACE_EAST;
        if (count_dynamic_face(map, x, y, face) +
                count_dynamic_face(other, other_x, other_y, reverse_face) >
            4) {
            return set_error(error,
                             error_size,
                             "%s tile_path_%zu cell %d has more than four apertures on its seam",
                             map_path(map),
                             tile + 1,
                             cell);
        }
    }
    return true;
}

bool celestial_structure_validate_topology(mapstruct *map, char *error, size_t error_size) {
    HARD_ASSERT(map != NULL);
    if (!celestial_structure_validate_header(map, error, error_size)) {
        return false;
    }
    mapstruct *bottom = map;
    size_t descent = 0;
    while (bottom->tile_path[TILED_DOWN] != NULL) {
        if (bottom->tile_map[TILED_DOWN] == NULL || ++descent >= MAP2_LEVELS) {
            return set_error(error,
                             error_size,
                             "%s has unresolved or cyclic lower stack",
                             map_path(map));
        }
        bottom = bottom->tile_map[TILED_DOWN];
        if (bottom == map) {
            return set_error(error, error_size, "%s has cyclic vertical stack", map_path(map));
        }
    }
    size_t members = 1;
    mapstruct *cursor = bottom;
    for (;;) {
        if (!celestial_structure_validate_header(cursor, error, error_size)) {
            return false;
        }
        for (size_t i = 0; i < TILED_NUM; i++) {
            if (cursor->tile_path[i] == NULL) {
                continue;
            }
            mapstruct *other = cursor->tile_map[i];
            size_t reverse = map_tiled_reverse[i];
            if (other == NULL || other->in_memory != MAP_IN_MEMORY ||
                other->celestial_schema != 1 || !path_equals(other->path, cursor->tile_path[i]) ||
                !path_equals(cursor->path, other->tile_path[reverse]) ||
                other->tile_map[reverse] != cursor ||
                other->celestial_boundary[reverse] != cursor->celestial_boundary[i]) {
                return set_error(
                    error,
                    error_size,
                    "%s tile_path_%zu has unresolved or disagreeing reciprocal coordinates",
                    map_path(cursor),
                    i + 1);
            }
            if ((i == TILED_UP || i == TILED_DOWN) &&
                (cursor->width != other->width || cursor->height != other->height)) {
                return set_error(error,
                                 error_size,
                                 "%s tile_path_%zu has non-identity dimensions",
                                 map_path(cursor),
                                 i + 1);
            }
            if (!validate_cardinal_seam_apertures(cursor, i, error, error_size)) {
                return false;
            }
        }
        if (!validate_exceptions(cursor, error, error_size)) {
            return false;
        }
        if (cursor->tile_path[TILED_UP] == NULL) {
            break;
        }
        cursor = cursor->tile_map[TILED_UP];
        if (cursor == NULL || ++members > MAP2_LEVELS || cursor == bottom) {
            return set_error(error,
                             error_size,
                             "%s vertical stack exceeds the supported depth",
                             map_path(map));
        }
    }
    return true;
}

void celestial_structure_save_metadata(const mapstruct *map, FILE *fp) {
    HARD_ASSERT(map != NULL);
    HARD_ASSERT(fp != NULL);
    for (size_t i = 0; i < map->celestial_rectangle_count; i++) {
        const celestial_rectangle_t *rectangle = &map->celestial_rectangles[i];
        fprintf(fp,
                "arch %s\nx %u\ny %u\nhp %u\nsp %u\n",
                rectangle->type == CELESTIAL_RECT_AMBIENT ? "ambient_light_zone" : "sky_exposure",
                rectangle->x,
                rectangle->y,
                rectangle->width - 1,
                rectangle->height - 1);
        if (rectangle->type == CELESTIAL_RECT_AMBIENT) {
            fprintf(fp, "ambient_strength %u\n", rectangle->value);
        } else {
            fprintf(fp,
                    "sky_state %s\n",
                    rectangle->type == CELESTIAL_RECT_SKY_OPEN ? "open" : "covered");
        }
        fprintf(fp, "end\n");
    }
}

static int compare_inventory_objects(const void *left, const void *right) {
    const object *a = *(const object *const *)left;
    const object *b = *(const object *const *)right;
    if (a->y != b->y) {
        return a->y - b->y;
    }
    if (a->x != b->x) {
        return a->x - b->x;
    }
    int result = strcmp(a->arch != NULL ? a->arch->name : "", b->arch != NULL ? b->arch->name : "");
    if (result != 0) {
        return result;
    }
    const char *transmission_keys[] = {"celestial_transmission",
                                       "celestial_transmission_closed",
                                       "celestial_transmission_open"};
    for (size_t i = 0; i < arraysize(transmission_keys); i++) {
        result = strcmp(STRING_SAFE(object_get_value(a, transmission_keys[i])),
                        STRING_SAFE(object_get_value(b, transmission_keys[i])));
        if (result != 0) {
            return result;
        }
    }
    result = strcmp(STRING_SAFE(object_get_value(a, "celestial_aperture_id")),
                    STRING_SAFE(object_get_value(b, "celestial_aperture_id")));
    if (result != 0) {
        return result;
    }
    return (int)celestial_structure_faces(a) - (int)celestial_structure_faces(b);
}

static const char *inventory_value(const object *op, const char *key) {
    const char *value = object_get_value(op, key);
    return value != NULL ? value : "";
}

bool celestial_structure_inventory(const mapstruct *map, FILE *fp, size_t max_records) {
    HARD_ASSERT(map != NULL);
    if (max_records == 0 || map->celestial_schema != 1) {
        return false;
    }

    size_t records = 1 + map->celestial_rectangle_count;
    for (size_t i = 0; i < TILED_NUM; i++) {
        if (map->tile_path[i] != NULL) {
            records++;
        }
    }
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            for (const object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                if (celestial_structure_faces(op) != 0 || op->type == DOOR || op->type == GATE) {
                    records++;
                }
            }
        }
    }
    if (records > max_records) {
        return false;
    }
    if (fp == NULL) {
        return true;
    }

    const object **objects = xcalloc(records, sizeof(*objects));
    size_t object_count = 0;
    for (int y = 0; y < map->height; y++) {
        for (int x = 0; x < map->width; x++) {
            for (const object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                if (celestial_structure_faces(op) != 0 || op->type == DOOR || op->type == GATE) {
                    objects[object_count++] = op;
                }
            }
        }
    }
    qsort(objects, object_count, sizeof(*objects), compare_inventory_objects);

    bool written = fprintf(fp,
                           "ATRINIK_CELESTIAL_STRUCTURE\tmap\t%s\t%dx%d\t%u\t%u\n",
                           map_path(map),
                           map->width,
                           map->height,
                           map->celestial_sky_above,
                           map->celestial_rectangle_count) >= 0;
    for (size_t i = 0; i < TILED_NUM; i++) {
        if (map->tile_path[i] != NULL &&
            fprintf(fp,
                    "ATRINIK_CELESTIAL_STRUCTURE\tlink\t%s\t%zu\t%s\t%s\n",
                    map_path(map),
                    i + 1,
                    map->tile_path[i],
                    map->celestial_boundary[i] == CELESTIAL_BOUNDARY_CONTINUOUS
                        ? "continuous"
                        : "discontinuous") < 0) {
            written = false;
        }
    }
    for (size_t i = 0; i < map->celestial_rectangle_count; i++) {
        const celestial_rectangle_t *r = &map->celestial_rectangles[i];
        if (fprintf(fp,
                    "ATRINIK_CELESTIAL_STRUCTURE\trect\t%s\t%u\t%u\t%u\t%u\t%u\t%u\n",
                    map_path(map),
                    r->y,
                    r->x,
                    r->type,
                    r->width,
                    r->height,
                    r->value) < 0) {
            written = false;
        }
    }
    for (size_t i = 0; i < object_count; i++) {
        const object *op = objects[i];
        const char *aperture_id = object_get_value(op, "celestial_aperture_id");
        if (fprintf(fp,
                    "ATRINIK_CELESTIAL_STRUCTURE\tobject\t%s\t%d\t%d\t%s\t%u\t%s\t%s\t%s\t%s\n",
                    map_path(map),
                    op->x,
                    op->y,
                    op->arch != NULL ? op->arch->name : "<none>",
                    celestial_structure_faces(op),
                    inventory_value(op, "celestial_transmission"),
                    inventory_value(op, "celestial_transmission_closed"),
                    inventory_value(op, "celestial_transmission_open"),
                    aperture_id != NULL ? aperture_id : "") < 0) {
            written = false;
        }
    }
    free(objects);
    return written && !ferror(fp);
}

void celestial_structure_free(mapstruct *map) {
    HARD_ASSERT(map != NULL);
    FREE_AND_NULL_PTR(map->celestial_rectangles);
    map->celestial_rectangle_count = 0;
}

void celestial_structure_reset_parse_state(mapstruct *map) {
    HARD_ASSERT(map != NULL);
    celestial_structure_free(map);
    memset(map->celestial_boundary, 0, sizeof(map->celestial_boundary));
    memset(map->celestial_tile_path_seen, 0, sizeof(map->celestial_tile_path_seen));
    map->celestial_tile_path_invalid = false;
    map->celestial_schema = 0;
    map->celestial_sky_above = 0;
    map->celestial_schema_seen = false;
    map->celestial_sky_seen = false;
    map->celestial_v1_header_seen = false;
    map->celestial_header_invalid = false;
    map->celestial_legacy_darkness_seen = false;
    map->celestial_legacy_outdoor_seen = false;
    map->celestial_light_seen = false;
    map->celestial_width_seen = false;
    map->celestial_height_seen = false;
    map->celestial_region_seen = false;
    map->light_value = 0;
    map->width = 0;
    map->height = 0;
    map->region = NULL;
}

bool celestial_structure_logical_map_id_valid(const char *path) {
    if (path == NULL) {
        return false;
    }
    size_t length = strlen(path);
    if (length < 2 || length >= MAX_BUF || path[0] != '/' || !path_is_safe_relative(path + 1)) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)path; *cp != '\0'; cp++) {
        if (*cp < 0x21 || *cp > 0x7e || *cp == ',' || *cp == '\\') {
            return false;
        }
    }
    return true;
}

static size_t parse_inventory_map_ids(const char *input,
                                      char maps[CELESTIAL_INVENTORY_MAX_ROOTS][MAX_BUF]) {
    if (input == NULL) {
        return 0;
    }
    size_t length = strlen(input);
    if (length == 0 || length >= sizeof(settings.celestial_inventory_maps) || input[0] == ',' ||
        input[length - 1] == ',' || strstr(input, ",,") != NULL) {
        return 0;
    }

    size_t count = 0;
    size_t position = 0;
    while (count < CELESTIAL_INVENTORY_MAX_ROOTS &&
           string_get_word(input, &position, ',', maps[count], sizeof(maps[count]), 0) != NULL) {
        if (!celestial_structure_logical_map_id_valid(maps[count])) {
            return 0;
        }
        for (size_t i = 0; i < count; i++) {
            if (strcmp(maps[i], maps[count]) == 0) {
                return 0;
            }
        }
        count++;
    }
    char extra[MAX_BUF];
    if (string_get_word(input, &position, ',', VS(extra), 0) != NULL) {
        return 0;
    }
    return count;
}

bool celestial_structure_inventory_maps_valid(const char *input) {
    char maps[CELESTIAL_INVENTORY_MAX_ROOTS][MAX_BUF];
    return parse_inventory_map_ids(input, maps) != 0;
}

static int compare_inventory_maps(const void *left, const void *right) {
    const mapstruct *const *a = left;
    const mapstruct *const *b = right;
    return strcmp(map_path(*a), map_path(*b));
}

static void delete_inventory_maps(mapstruct *maps[CELESTIAL_INVENTORY_MAX_MAPS], size_t count) {
    for (size_t i = 0; i < count; i++) {
        delete_map(maps[i]);
    }
}

int celestial_structure_inventory_run(void) {
    char roots[CELESTIAL_INVENTORY_MAX_ROOTS][MAX_BUF];
    size_t root_count = parse_inventory_map_ids(settings.celestial_inventory_maps, roots);
    if (root_count == 0) {
        LOG(ERROR, "Celestial inventory requires 1-16 unique canonical logical map IDs.");
        return EXIT_FAILURE;
    }

    char paths[CELESTIAL_INVENTORY_MAX_MAPS][MAX_BUF];
    size_t count = root_count;
    for (size_t i = 0; i < root_count; i++) {
        memcpy(paths[i], roots[i], sizeof(paths[i]));
    }
    mapstruct *loaded[CELESTIAL_INVENTORY_MAX_MAPS] = {0};
    for (size_t i = 0; i < count; i++) {
        loaded[i] = ready_map_name(paths[i], NULL, MAP_FLUSH | MAP_NO_DYNAMIC);
        if (loaded[i] == NULL) {
            LOG(ERROR, "Celestial inventory could not load %s.", paths[i]);
            delete_inventory_maps(loaded, i);
            return EXIT_FAILURE;
        }
        for (size_t tile = 0; tile < TILED_NUM; tile++) {
            const char *path = loaded[i]->tile_path[tile];
            if (path == NULL) {
                continue;
            }
            bool found = false;
            for (size_t j = 0; j < count; j++) {
                if (strcmp(paths[j], path) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
            if (count == CELESTIAL_INVENTORY_MAX_MAPS) {
                LOG(ERROR,
                    "Celestial inventory topology exceeds its %d-map closure limit.",
                    CELESTIAL_INVENTORY_MAX_MAPS);
                delete_inventory_maps(loaded, i + 1);
                return EXIT_FAILURE;
            }
            snprintf(paths[count], sizeof(paths[count]), "%s", path);
            count++;
        }
    }
    qsort(loaded, count, sizeof(*loaded), compare_inventory_maps);

    char error[HUGE_BUF];
    for (size_t i = 0; i < count; i++) {
        if (!celestial_structure_validate_topology(loaded[i], VS(error))) {
            LOG(ERROR, "Celestial inventory topology rejected %s: %s", map_path(loaded[i]), error);
            delete_inventory_maps(loaded, count);
            return EXIT_FAILURE;
        }
        if (!celestial_structure_inventory(loaded[i], NULL, settings.celestial_inventory_limit)) {
            LOG(ERROR,
                "Celestial inventory rejected %s or exceeded its %u-record limit.",
                map_path(loaded[i]),
                settings.celestial_inventory_limit);
            delete_inventory_maps(loaded, count);
            return EXIT_FAILURE;
        }
    }

    printf("ATRINIK_CELESTIAL_STRUCTURE\tformat\t1\n");
    printf("ATRINIK_CELESTIAL_STRUCTURE\tlimit\t%u\n", settings.celestial_inventory_limit);
    for (size_t i = 0; i < count; i++) {
        if (!celestial_structure_inventory(loaded[i], stdout, settings.celestial_inventory_limit)) {
            delete_inventory_maps(loaded, count);
            return EXIT_FAILURE;
        }
    }
    delete_inventory_maps(loaded, count);
    if (fflush(stdout) == EOF || ferror(stdout)) {
        LOG(ERROR, "Celestial inventory output failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
