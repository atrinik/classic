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
#include <server_main.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#include <openssl/evp.h>
#include <openssl/sha.h>
#ifndef WIN32
#include <dirent.h>
#endif
#include <stdarg.h>

#define CELESTIAL_PREFLIGHT_FILE_LIMIT (64U * 1024U * 1024U)

static bool celestial_v1_runtime_active;
static char celestial_artifact_commit[41];

#ifndef WIN32
static bool preflight_private_maps(char *error, size_t error_size);
#endif

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

static bool celestial_private_source_from_path(const char *map_path_value,
                                               char source_path[HUGE_BUF]) {
    if (map_path_value == NULL || !string_startswith(map_path_value, settings.datapath)) {
        return false;
    }
    char *demangled = path_basename(map_path_value);
    if (demangled == NULL || strchr(demangled, '$') == NULL) {
        free(demangled);
        return false;
    }
    string_replace_char(demangled, "$", '/');
    while (*demangled == '/') {
        memmove(demangled, demangled + 1, strlen(demangled));
    }
    int written = snprintf(source_path, HUGE_BUF, "/%s", demangled);
    free(demangled);
    return written >= 0 && (size_t)written < HUGE_BUF &&
           celestial_structure_logical_map_id_valid(source_path);
}

static bool preflight_hex(const char *value, size_t length) {
    if (value == NULL || strlen(value) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool preflight_json_string(const char *start,
                                  const char *end,
                                  const char *key,
                                  char *value,
                                  size_t value_size) {
    char marker[128];
    int marker_length = snprintf(marker, sizeof(marker), "\"%s\"", key);
    if (marker_length < 0 || (size_t)marker_length >= sizeof(marker)) {
        return false;
    }
    const char *cursor = start;
    while (cursor < end) {
        const char *found = strstr(cursor, marker);
        if (found == NULL || found >= end) {
            return false;
        }
        const char *colon = found + marker_length;
        while (colon < end && isspace((unsigned char)*colon)) {
            colon++;
        }
        if (colon >= end || *colon++ != ':') {
            cursor = found + marker_length;
            continue;
        }
        while (colon < end && isspace((unsigned char)*colon)) {
            colon++;
        }
        if (colon >= end || *colon++ != '"') {
            return false;
        }
        const char *finish = colon;
        while (finish < end && *finish != '"') {
            if (*finish == '\\' || (unsigned char)*finish < 0x20) {
                return false;
            }
            finish++;
        }
        if (finish >= end || (size_t)(finish - colon) >= value_size) {
            return false;
        }
        memcpy(value, colon, (size_t)(finish - colon));
        value[finish - colon] = '\0';
        return true;
    }
    return false;
}

static bool preflight_json_uint(const char *start,
                                const char *end,
                                const char *key,
                                uint64_t *value) {
    char marker[128];
    int marker_length = snprintf(marker, sizeof(marker), "\"%s\"", key);
    if (marker_length < 0 || (size_t)marker_length >= sizeof(marker)) {
        return false;
    }
    const char *found = strstr(start, marker);
    if (found == NULL || found >= end) {
        return false;
    }
    const char *cursor = found + marker_length;
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (cursor >= end || *cursor++ != ':') {
        return false;
    }
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (cursor >= end || !isdigit((unsigned char)*cursor)) {
        return false;
    }
    errno = 0;
    char *finish;
    unsigned long long parsed = strtoull(cursor, &finish, 10);
    if (errno != 0 || finish == cursor || finish > end) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static const char *preflight_json_matching(const char *open,
                                           const char *limit,
                                           char opening,
                                           char closing) {
    size_t depth = 0;
    bool string = false;
    bool escaped = false;
    for (const char *cursor = open; cursor < limit; cursor++) {
        if (string) {
            if (escaped) {
                escaped = false;
            } else if (*cursor == '\\') {
                escaped = true;
            } else if (*cursor == '"') {
                string = false;
            }
            continue;
        }
        if (*cursor == '"') {
            string = true;
        } else if (*cursor == opening) {
            depth++;
        } else if (*cursor == closing && depth-- == 1) {
            return cursor;
        }
    }
    return NULL;
}

static char *preflight_read_file(const char *path, size_t *size, char *error, size_t error_size) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        set_error(error, error_size, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        set_error(error, error_size, "cannot size %s", path);
        return NULL;
    }
    long length = ftell(fp);
    if (length < 0 || (uintmax_t)length > CELESTIAL_PREFLIGHT_FILE_LIMIT) {
        fclose(fp);
        set_error(error, error_size, "artifact file %s exceeds the bounded preflight limit", path);
        return NULL;
    }
    rewind(fp);
    char *contents = xmalloc((size_t)length + 1);
    if (fread(contents, 1, (size_t)length, fp) != (size_t)length || ferror(fp)) {
        free(contents);
        fclose(fp);
        set_error(error, error_size, "cannot read %s", path);
        return NULL;
    }
    fclose(fp);
    contents[length] = '\0';
    if (size != NULL) {
        *size = (size_t)length;
    }
    return contents;
}

static bool preflight_sha256_file(const char *path, char output[SHA256_DIGEST_LENGTH * 2 + 1]) {
    size_t size;
    char *contents = preflight_read_file(path, &size, NULL, 0);
    if (contents == NULL) {
        return false;
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256((const unsigned char *)contents, size, digest) == NULL) {
        free(contents);
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(output + i * 2, 3, "%02x", digest[i]);
    }
    output[sizeof(digest) * 2] = '\0';
    free(contents);
    return true;
}

static bool preflight_activation_marker(const char *migration_digest,
                                        char *error,
                                        size_t error_size) {
    char marker_path[HUGE_BUF];
    if (snprintf(marker_path,
                 sizeof(marker_path),
                 "%s/celestial-activation.json",
                 settings.datapath) < 0 ||
        strlen(marker_path) >= sizeof(marker_path)) {
        return set_error(error, error_size, "celestial activation marker path is too long");
    }

    if (path_exists(marker_path)) {
        size_t marker_size;
        char *marker = preflight_read_file(marker_path, &marker_size, error, error_size);
        if (marker == NULL) {
            return false;
        }
        const char *end = marker + marker_size;
        uint64_t schema;
        char commit[41], recorded_digest[SHA256_DIGEST_LENGTH * 2 + 1];
        bool valid = preflight_json_uint(marker, end, "schema_version", &schema) && schema == 1 &&
                     preflight_json_string(marker, end, "content_commit", VS(commit)) &&
                     preflight_hex(commit, 40) && strcmp(commit, celestial_artifact_commit) == 0 &&
                     preflight_json_string(marker,
                                           end,
                                           "migration_index_sha256",
                                           VS(recorded_digest)) &&
                     preflight_hex(recorded_digest, SHA256_DIGEST_LENGTH * 2) &&
                     strcmp(recorded_digest, migration_digest) == 0;
        free(marker);
        if (!valid) {
            return set_error(error,
                             error_size,
                             "celestial activation marker does not match the immutable artifact");
        }
        return true;
    }

    char contents[512];
    int written = snprintf(contents,
                           sizeof(contents),
                           "{\n"
                           "  \"schema_version\": 1,\n"
                           "  \"content_commit\": \"%s\",\n"
                           "  \"migration_index_sha256\": \"%s\"\n"
                           "}\n",
                           celestial_artifact_commit,
                           migration_digest);
    if (written < 0 || (size_t)written >= sizeof(contents) ||
        !path_write_atomic(marker_path, contents, (size_t)written, SAVE_MODE)) {
        return set_error(error, error_size, "cannot durably publish the celestial activation marker");
    }
    return true;
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
        (!dynamic && opaque != QUERY_FLAG(&op->arch->clone, FLAG_BLOCKSVIEW))) {
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
    if (map->celestial_generated_origin_seen &&
        (map->celestial_generated_origin == NULL ||
         !celestial_structure_logical_map_id_valid(map->celestial_generated_origin))) {
        return set_error(error,
                         error_size,
                         "%s has an invalid generated-map origin",
                         map_path(map));
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
                             "%s tile_path_%u and celestial_boundary_%u disagree",
                             map_path(map),
                             (unsigned)(i + 1),
                             (unsigned)(i + 1));
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
                             "%s tile_path_%u corner has more than four apertures on its seam",
                             map_path(map),
                             (unsigned)(tile + 1));
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
                             "%s tile_path_%u cell %d has more than four apertures on its seam",
                             map_path(map),
                             (unsigned)(tile + 1),
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
                    "%s tile_path_%u has unresolved or disagreeing reciprocal coordinates",
                    map_path(cursor),
                    (unsigned)(i + 1));
            }
            if ((i == TILED_UP || i == TILED_DOWN) &&
                (cursor->width != other->width || cursor->height != other->height)) {
                return set_error(error,
                                 error_size,
                                 "%s tile_path_%u has non-identity dimensions",
                                 map_path(cursor),
                                 (unsigned)(i + 1));
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
                    "ATRINIK_CELESTIAL_STRUCTURE\tlink\t%s\t%u\t%s\t%s\n",
                    map_path(map),
                    (unsigned)(i + 1),
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
    FREE_AND_CLEAR_HASH(map->celestial_generated_origin);
}

void celestial_structure_reset_parse_state(mapstruct *map) {
    HARD_ASSERT(map != NULL);
    celestial_structure_free(map);
    memset(map->celestial_boundary, 0, sizeof(map->celestial_boundary));
    memset(map->celestial_tile_path_seen, 0, sizeof(map->celestial_tile_path_seen));
    map->celestial_tile_path_invalid = false;
    map->celestial_schema = 0;
    map->celestial_sky_above = 0;
    map->celestial_generated_origin_seen = false;
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
    map->celestial_structure_revision = 0;
    map->celestial_light_key = 0;
    map->celestial_light_valid = false;
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

static bool preflight_manifest_files(const char *manifest,
                                     const char *artifact_root,
                                     char *error,
                                     size_t error_size) {
    const char *files = strstr(manifest, "\"files\"");
    if (files == NULL) {
        return set_error(error, error_size, "Classic artifact has no canonical files manifest");
    }
    const char *array = strchr(files, '[');
    const char *array_end = array != NULL
                                ? preflight_json_matching(array,
                                                          manifest + strlen(manifest),
                                                          '[',
                                                          ']')
                                : NULL;
    if (array == NULL || array_end == NULL) {
        return set_error(error, error_size, "Classic artifact files manifest is malformed");
    }

    EVP_MD_CTX *aggregate = EVP_MD_CTX_new();
    if (aggregate == NULL || EVP_DigestInit_ex(aggregate, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(aggregate);
        return set_error(error, error_size, "cannot initialize artifact manifest digest");
    }
    char previous[MAX_BUF] = "";
    size_t entries = 0;
    const char *cursor = array + 1;
    while (cursor < array_end) {
        const char *path_marker = strstr(cursor, "\"path\"");
        if (path_marker == NULL || path_marker >= array_end) {
            break;
        }
        const char *object_start = path_marker;
        while (object_start > array && object_start[-1] != '{') {
            object_start--;
        }
        if (object_start > array) {
            object_start--;
        }
        const char *object_end = object_start > array
                                     ? preflight_json_matching(object_start,
                                                               array_end,
                                                               '{',
                                                               '}')
                                     : NULL;
        if (object_end == NULL || object_end > array_end) {
            return set_error(error, error_size, "Classic artifact has a truncated file entry");
        }
        char relative[MAX_BUF], expected[SHA256_DIGEST_LENGTH * 2 + 1];
        uint64_t expected_size;
        if (!preflight_json_string(object_start,
                                   object_end,
                                   "path",
                                   VS(relative)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "sha256",
                                   VS(expected)) ||
            !preflight_json_uint(object_start, object_end, "size", &expected_size) ||
            !path_is_safe_relative(relative) || relative[0] == '\0' ||
            !preflight_hex(expected, SHA256_DIGEST_LENGTH * 2) ||
            (entries != 0 && strcmp(previous, relative) >= 0)) {
            return set_error(error,
                             error_size,
                             "Classic artifact file manifest has a noncanonical or duplicate entry");
        }
        snprintf(previous, sizeof(previous), "%s", relative);

        char path[HUGE_BUF];
        if (snprintf(path, sizeof(path), "%s/%s", artifact_root, relative) < 0 ||
            strlen(path) >= sizeof(path)) {
            return set_error(error, error_size, "Classic artifact file path is too long: %s", relative);
        }
        struct stat metadata;
        if (stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            (uint64_t)metadata.st_size != expected_size) {
            return set_error(error, error_size, "Classic artifact file is missing or changed: %s", relative);
        }
        char actual[SHA256_DIGEST_LENGTH * 2 + 1];
        if (!preflight_sha256_file(path, actual) || strcmp(actual, expected) != 0) {
            return set_error(error, error_size, "Classic artifact digest mismatch: %s", relative);
        }

        char size_text[32];
        int written = snprintf(size_text, sizeof(size_text), "%" PRIu64 "\n", expected_size);
        unsigned char zero = 0;
        if (written < 0 || (size_t)written >= sizeof(size_text) ||
            EVP_DigestUpdate(aggregate, relative, strlen(relative)) != 1 ||
            EVP_DigestUpdate(aggregate, &zero, sizeof(zero)) != 1 ||
            EVP_DigestUpdate(aggregate, expected, strlen(expected)) != 1 ||
            EVP_DigestUpdate(aggregate, &zero, sizeof(zero)) != 1 ||
            EVP_DigestUpdate(aggregate, size_text, (size_t)written) != 1) {
            EVP_MD_CTX_free(aggregate);
            return set_error(error, error_size, "cannot calculate artifact manifest digest");
        }
        entries++;
        cursor = object_end + 1;
    }
    if (entries == 0) {
        return set_error(error, error_size, "Classic artifact files manifest is empty");
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    char actual_aggregate[SHA256_DIGEST_LENGTH * 2 + 1];
    unsigned int digest_length = 0;
    if (EVP_DigestFinal_ex(aggregate, digest, &digest_length) != 1 ||
        digest_length != SHA256_DIGEST_LENGTH) {
        EVP_MD_CTX_free(aggregate);
        return set_error(error, error_size, "cannot finalize artifact manifest digest");
    }
    EVP_MD_CTX_free(aggregate);
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(actual_aggregate + i * 2, 3, "%02x", digest[i]);
    }
    actual_aggregate[sizeof(digest) * 2] = '\0';
    char expected_aggregate[SHA256_DIGEST_LENGTH * 2 + 1];
    if (!preflight_json_string(manifest,
                               manifest + strlen(manifest),
                               "celestial_manifest_files_sha256",
                               VS(expected_aggregate)) ||
        strcmp(actual_aggregate, expected_aggregate) != 0) {
        return set_error(error, error_size, "Classic artifact aggregate file digest mismatch");
    }
    return true;
}

static bool preflight_migration_index(const char *index,
                                      char *error,
                                      size_t error_size) {
    const char *index_end = index + strlen(index);
    uint64_t schema, expected_maps;
    char migration[64], source_commit[64], classic_sha[64];
    if (!preflight_json_uint(index, index_end, "schema_version", &schema) || schema != 1 ||
        !preflight_json_string(index, index_end, "migration", VS(migration)) ||
        strcmp(migration, "celestial-v1") != 0 ||
        !preflight_json_string(index, index_end, "source_commit", VS(source_commit)) ||
        !preflight_hex(source_commit, 40) ||
        !preflight_json_string(index, index_end, "classic_compatible_sha", VS(classic_sha)) ||
        !preflight_hex(classic_sha, 40) ||
        !preflight_json_uint(index, index_end, "maps", &expected_maps)) {
        return set_error(error, error_size, "celestial migration index identity is malformed");
    }

    const char *maps_marker = strstr(index, "\"maps\"");
    const char *array = NULL;
    while (maps_marker != NULL && maps_marker < index_end) {
        const char *colon = strchr(maps_marker, ':');
        if (colon != NULL && colon < index_end) {
            while (++colon < index_end && isspace((unsigned char)*colon)) {
            }
            if (colon < index_end && *colon == '[') {
                array = colon;
                break;
            }
        }
        maps_marker = strstr(maps_marker + 7, "\"maps\"");
    }
    const char *array_end = array != NULL
                                ? preflight_json_matching(array, index_end, '[', ']')
                                : NULL;
    if (array == NULL || array_end == NULL) {
        return set_error(error, error_size, "celestial migration index map array is missing");
    }

    char previous[MAX_BUF] = "";
    size_t actual_maps = 0;
    const char *cursor = array + 1;
    while (cursor < array_end) {
        const char *path_marker = strstr(cursor, "\"path\"");
        if (path_marker == NULL || path_marker >= array_end) {
            break;
        }
        const char *object_start = path_marker;
        while (object_start > array && object_start[-1] != '{') {
            object_start--;
        }
        if (object_start > array) {
            object_start--;
        }
        const char *object_end = object_start > array
                                     ? preflight_json_matching(object_start,
                                                               array_end,
                                                               '{',
                                                               '}')
                                     : NULL;
        if (object_end == NULL || object_end > array_end) {
            return set_error(error, error_size, "celestial migration index has a truncated map record");
        }
        char logical[MAX_BUF], predecessor[SHA256_DIGEST_LENGTH * 2 + 1];
        char expected[SHA256_DIGEST_LENGTH * 2 + 1];
        char expected_region[MAX_BUF], expected_sky[16], disposition[32];
        uint64_t expected_light, expected_boundaries;
        if (!preflight_json_string(object_start,
                                   object_end,
                                   "path",
                                   VS(logical)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "predecessor_sha256",
                                   VS(predecessor)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "migrated_sha256",
                                   VS(expected)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "region",
                                   VS(expected_region)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "sky_above",
                                   VS(expected_sky)) ||
            !preflight_json_string(object_start,
                                   object_end,
                                   "legacy_disposition",
                                   VS(disposition)) ||
            !preflight_json_uint(object_start, object_end, "target_light", &expected_light) ||
            !preflight_json_uint(object_start,
                                 object_end,
                                 "horizontal_boundaries",
                                 &expected_boundaries) ||
            !celestial_structure_logical_map_id_valid(logical) ||
            !preflight_hex(predecessor, SHA256_DIGEST_LENGTH * 2) ||
            !preflight_hex(expected, SHA256_DIGEST_LENGTH * 2) ||
            expected_region[0] == '\0' ||
            (strcmp(expected_sky, "open") != 0 && strcmp(expected_sky, "linked") != 0 &&
             strcmp(expected_sky, "sealed") != 0) ||
            (strcmp(disposition, "absent-zero") != 0 &&
             strcmp(disposition, "ignored-outdoor") != 0 &&
             strcmp(disposition, "translated-darkness") != 0 &&
             strcmp(disposition, "reviewed-ambient") != 0) ||
            expected_light > 40959 ||
            (actual_maps != 0 && strcmp(previous, logical) >= 0)) {
            return set_error(error, error_size, "celestial migration index has a noncanonical map record");
        }
        snprintf(previous, sizeof(previous), "%s", logical);

        char filename[HUGE_BUF];
        if (snprintf(filename, sizeof(filename), "%s%s", settings.mapspath, logical) < 0 ||
            strlen(filename) >= sizeof(filename)) {
            return set_error(error, error_size, "celestial map path is too long: %s", logical);
        }
        char actual[SHA256_DIGEST_LENGTH * 2 + 1];
        if (!preflight_sha256_file(filename, actual) || strcmp(actual, expected) != 0) {
            return set_error(error, error_size, "celestial map digest mismatch: %s", logical);
        }
        FILE *map_file = fopen(filename, "rb");
        mapstruct *map = get_linked_map();
        FREE_AND_COPY_HASH(map->path, logical);
        bool map_valid = map_file != NULL && load_map_header(map, map_file) &&
                         map->celestial_schema == 1;
        if (map_file != NULL) {
            fclose(map_file);
        }
        if (map_valid) {
            char header_error[HUGE_BUF];
            map_valid = celestial_structure_validate_header(map, VS(header_error));
        }
        if (map_valid &&
            (map->region == NULL || strcmp(map->region->name, expected_region) != 0 ||
             (strcmp(expected_sky, "open") == 0 && map->celestial_sky_above != CELESTIAL_SKY_OPEN) ||
             (strcmp(expected_sky, "linked") == 0 &&
              map->celestial_sky_above != CELESTIAL_SKY_LINKED) ||
             (strcmp(expected_sky, "sealed") == 0 &&
              map->celestial_sky_above != CELESTIAL_SKY_SEALED) ||
             (uint64_t)map->light_value != expected_light)) {
            map_valid = false;
        }
        if (map_valid) {
            uint64_t boundaries = 0;
            for (size_t tile = 0; tile < TILED_NUM; tile++) {
                if (map->tile_path[tile] != NULL) {
                    boundaries++;
                }
            }
            map_valid = boundaries == expected_boundaries;
        }
        if (!map_valid) {
            delete_map(map);
            return set_error(error, error_size, "celestial map failed structural load: %s", logical);
        }
        for (size_t tile = 0; tile < TILED_NUM; tile++) {
            if (map->tile_path[tile] == NULL) {
                continue;
            }
            char target[HUGE_BUF];
            if (snprintf(target, sizeof(target), "%s%s", settings.mapspath, map->tile_path[tile]) < 0 ||
                strlen(target) >= sizeof(target) || !path_exists(target)) {
                delete_map(map);
                return set_error(error,
                                 error_size,
                                 "celestial map %s has an unindexed or missing link",
                                 logical);
            }
        }
        delete_map(map);
        actual_maps++;
        cursor = object_end + 1;
    }
    if (actual_maps != expected_maps) {
        return set_error(error,
                         error_size,
                         "celestial migration index is incomplete: expected %" PRIu64 " maps, found %zu",
                         expected_maps,
                         actual_maps);
    }
    return true;
}

bool celestial_structure_startup_preflight(char *error, size_t error_size) {
    celestial_v1_runtime_active = false;
    celestial_artifact_commit[0] = '\0';
    if (!celestial_structure_recover_map_transactions(error, error_size)) {
        return false;
    }
    char manifest_path[HUGE_BUF];
    if (snprintf(manifest_path,
                 sizeof(manifest_path),
                 "%s/../manifest.json",
                 settings.mapspath) < 0 || strlen(manifest_path) >= sizeof(manifest_path)) {
        return set_error(error, error_size, "Classic artifact manifest path is too long");
    }
    size_t manifest_size;
    char *manifest = preflight_read_file(manifest_path, &manifest_size, error, error_size);
    if (manifest == NULL) {
        return false;
    }
    (void)manifest_size;
    char value[HUGE_BUF];
    uint64_t number;
    bool identity = preflight_json_uint(manifest,
                                        manifest + strlen(manifest),
                                        "schema_version",
                                        &number) &&
                    number == 2 &&
                    preflight_json_string(manifest,
                                          manifest + strlen(manifest),
                                          "target",
                                          VS(value)) &&
                    strcmp(value, "classic") == 0 &&
                    preflight_json_string(manifest,
                                          manifest + strlen(manifest),
                                          "artifact_format",
                                          VS(value)) &&
                    strcmp(value, "atrinik-classic-runtime-content-v1") == 0;
    char repository[128], branch[64], commit[64], migration_index[128];
    char migration_digest[SHA256_DIGEST_LENGTH * 2 + 1];
    identity = identity &&
               preflight_json_string(manifest,
                                     manifest + strlen(manifest),
                                     "repository",
                                     VS(repository)) &&
               strcmp(repository, "atrinik/content") == 0 &&
               preflight_json_string(manifest,
                                     manifest + strlen(manifest),
                                     "branch",
                                     VS(branch)) &&
               strcmp(branch, "main") == 0 &&
               preflight_json_string(manifest,
                                     manifest + strlen(manifest),
                                     "commit",
                                     VS(commit)) &&
               preflight_hex(commit, 40) &&
               preflight_json_uint(manifest,
                                   manifest + strlen(manifest),
                                   "celestial_schema_version",
                                   &number) &&
               number == 1 &&
               preflight_json_uint(manifest,
                                   manifest + strlen(manifest),
                                   "celestial_runtime_factory_version",
                                   &number) &&
               number == 1 &&
               preflight_json_string(manifest,
                                     manifest + strlen(manifest),
                                     "celestial_migration_index",
                                     VS(migration_index)) &&
               strcmp(migration_index, "maps/celestial-migration-index.json") == 0 &&
               preflight_json_string(manifest,
                                     manifest + strlen(manifest),
                                     "celestial_migration_index_sha256",
                                     VS(migration_digest)) &&
               preflight_hex(migration_digest, SHA256_DIGEST_LENGTH * 2);
    if (!identity) {
        free(manifest);
        return set_error(error, error_size, "Classic artifact does not declare celestial-v1");
    }
    memcpy(celestial_artifact_commit, commit, sizeof(celestial_artifact_commit) - 1);
    celestial_artifact_commit[sizeof(celestial_artifact_commit) - 1] = '\0';
    char artifact_root[HUGE_BUF];
    if (snprintf(artifact_root, sizeof(artifact_root), "%s/..", settings.mapspath) < 0 ||
        strlen(artifact_root) >= sizeof(artifact_root) ||
        !preflight_manifest_files(manifest, artifact_root, error, error_size)) {
        free(manifest);
        return false;
    }
    free(manifest);

    char index_path[HUGE_BUF];
    if (snprintf(index_path, sizeof(index_path), "%s/celestial-migration-index.json", settings.mapspath) < 0 ||
        strlen(index_path) >= sizeof(index_path)) {
        return set_error(error, error_size, "celestial migration index path is too long");
    }
    size_t index_size;
    char *index = preflight_read_file(index_path, &index_size, error, error_size);
    if (index == NULL) {
        return false;
    }
    (void)index_size;
    char actual_index_digest[SHA256_DIGEST_LENGTH * 2 + 1];
    bool digest_valid = preflight_sha256_file(index_path, actual_index_digest) &&
                        strcmp(actual_index_digest, migration_digest) == 0;
    bool valid = digest_valid && preflight_migration_index(index, error, error_size);
    if (!digest_valid) {
        set_error(error, error_size, "celestial migration index digest does not match the artifact");
    }
    free(index);
    if (!valid) {
        return false;
    }
#ifndef WIN32
    if (!preflight_private_maps(error, error_size)) {
        return false;
    }
#endif
    if (!preflight_activation_marker(migration_digest, error, error_size)) {
        return false;
    }
    celestial_v1_runtime_active = true;
    return true;
}

bool celestial_structure_v1_runtime_active(void) {
    return celestial_v1_runtime_active;
}

static bool provenance_path(const char *map_path_value,
                            char output[HUGE_BUF],
                            char digest_output[SHA256_DIGEST_LENGTH * 2 + 1]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (map_path_value == NULL || SHA256((const unsigned char *)map_path_value,
                                         strlen(map_path_value),
                                         digest) == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(digest_output + i * 2, 3, "%02x", digest[i]);
    }
    digest_output[sizeof(digest) * 2] = '\0';
    return snprintf(output,
                    HUGE_BUF,
                    "%s/celestial-provenance/%s.json",
                    settings.datapath,
                    digest_output) >= 0;
}

static bool provenance_source(const mapstruct *map,
                              char source_path[HUGE_BUF],
                              char source_file[HUGE_BUF],
                              char source_sha[SHA256_DIGEST_LENGTH * 2 + 1]) {
    if (map == NULL || map->path == NULL) {
        return false;
    }
    const char *logical = map->path;
    char logical_path[HUGE_BUF];
    if (map->celestial_generated_origin != NULL) {
        logical = map->celestial_generated_origin;
        if (snprintf(logical_path, sizeof(logical_path), "%s", logical) < 0 ||
            strlen(logical_path) >= sizeof(logical_path)) {
            return false;
        }
    } else if (MAP_UNIQUE(map) && string_startswith(map->path, settings.datapath)) {
        if (!celestial_private_source_from_path(map->path, logical_path)) {
            return false;
        }
    } else if (snprintf(logical_path, sizeof(logical_path), "%s", logical) < 0 ||
               strlen(logical_path) >= sizeof(logical_path)) {
        return false;
    }
    if (!celestial_structure_logical_map_id_valid(logical_path) ||
        snprintf(source_path, HUGE_BUF, "%s", logical_path) < 0 ||
        strlen(source_path) >= HUGE_BUF ||
        snprintf(source_file, HUGE_BUF, "%s%s", settings.mapspath, logical_path) < 0 ||
        strlen(source_file) >= HUGE_BUF ||
        !preflight_sha256_file(source_file, source_sha)) {
        return false;
    }
    return true;
}

static bool celestial_map_identity_valid(const char *path) {
    if (celestial_structure_logical_map_id_valid(path)) {
        return true;
    }
    if (path == NULL || !string_startswith(path, settings.datapath)) {
        return false;
    }
    char *demangled = path_basename(path);
    if (demangled == NULL || strchr(demangled, '$') == NULL) {
        free(demangled);
        return false;
    }
    string_replace_char(demangled, "$", '/');
    while (*demangled == '/') {
        memmove(demangled, demangled + 1, strlen(demangled));
    }
    char normalized[HUGE_BUF];
    int written = snprintf(normalized, sizeof(normalized), "/%s", demangled);
    free(demangled);
    return written >= 0 && (size_t)written < sizeof(normalized) &&
           celestial_structure_logical_map_id_valid(normalized);
}

static bool provenance_map_file(const mapstruct *map,
                                char map_file[HUGE_BUF],
                                char map_sha[SHA256_DIGEST_LENGTH * 2 + 1]) {
    if (map == NULL) {
        return false;
    }
    if (map->tmpname != NULL) {
        if (snprintf(map_file, HUGE_BUF, "%s", map->tmpname) < 0 ||
            strlen(map_file) >= HUGE_BUF) {
            return false;
        }
    } else if (MAP_UNIQUE(map) && map->path != NULL &&
               string_startswith(map->path, settings.datapath)) {
        if (snprintf(map_file, HUGE_BUF, "%s", map->path) < 0 ||
            strlen(map_file) >= HUGE_BUF) {
            return false;
        }
    } else {
        char *resolved = create_pathname(map->path);
        if (resolved == NULL) {
            return false;
        }
        bool copied = snprintf(map_file, HUGE_BUF, "%s", resolved) >= 0 &&
                      strlen(map_file) < HUGE_BUF;
        if (!copied) {
            return false;
        }
    }
    return preflight_sha256_file(map_file, map_sha);
}

bool celestial_structure_write_provenance(const mapstruct *map, char *error, size_t error_size) {
    if (map == NULL || map->celestial_schema != 1 || map->path == NULL) {
        return set_error(error, error_size, "cannot publish provenance for a non-v1 map");
    }
    char ledger[HUGE_BUF], ledger_id[SHA256_DIGEST_LENGTH * 2 + 1];
    char source_path[HUGE_BUF], source_file[HUGE_BUF];
    char source_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    char map_file[HUGE_BUF], map_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    source_path[0] = '\0';
    source_file[0] = '\0';
    if (!provenance_path(map->path, ledger, ledger_id)) {
        return set_error(error, error_size, "cannot resolve provenance identity for %s", map_path(map));
    }
    if (!provenance_source(map, source_path, source_file, source_sha)) {
        return set_error(error,
                         error_size,
                         "cannot resolve immutable source lineage for %s under %s (source=%s file=%s)",
                         map_path(map),
                         settings.mapspath,
                         source_path,
                         source_file);
    }
    if (!provenance_map_file(map, map_file, map_sha)) {
        return set_error(error,
                         error_size,
                         "cannot hash mutable map file for %s",
                         map_path(map));
    }
    char contents[HUGE_BUF];
    int written = snprintf(contents,
                           sizeof(contents),
                           "{\n"
                           "  \"schema_version\": 1,\n"
                           "  \"map_path\": \"%s\",\n"
                           "  \"source_path\": \"%s\",\n"
                           "  \"source_sha256\": \"%s\",\n"
                           "  \"map_sha256\": \"%s\",\n"
                           "  \"content_commit\": \"%s\"\n"
                           "}\n",
                           map->path,
                           source_path,
                           source_sha,
                           map_sha,
                           celestial_artifact_commit[0] != '\0'
                               ? celestial_artifact_commit
                               : "0000000000000000000000000000000000000000");
    if (written < 0 || (size_t)written >= sizeof(contents) ||
        !path_write_atomic(ledger, contents, (size_t)written, SAVE_MODE)) {
        return set_error(error, error_size, "cannot atomically publish provenance ledger %s", ledger_id);
    }
    return true;
}

bool celestial_structure_validate_provenance(const mapstruct *map, char *error, size_t error_size) {
    if (map == NULL || map->path == NULL) {
        return set_error(error, error_size, "temporary v1 map has no provenance identity");
    }
    char ledger[HUGE_BUF], ledger_id[SHA256_DIGEST_LENGTH * 2 + 1];
    if (!provenance_path(map->path, ledger, ledger_id)) {
        return set_error(error, error_size, "temporary v1 map has an invalid provenance identity");
    }
    size_t ledger_size;
    char *contents = preflight_read_file(ledger, &ledger_size, error, error_size);
    if (contents == NULL) {
        return set_error(error,
                         error_size,
                         "temporary v1 map %s has no provenance ledger (%s)",
                         map_path(map),
                         ledger_id);
    }
    char map_path_value[HUGE_BUF], source_path[HUGE_BUF], source_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    char map_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    char content_commit[41];
    uint64_t schema;
    const char *end = contents + ledger_size;
    bool valid = preflight_json_uint(contents, end, "schema_version", &schema) && schema == 1 &&
                 preflight_json_string(contents, end, "map_path", VS(map_path_value)) &&
                 strcmp(map_path_value, map->path) == 0 &&
                 preflight_json_string(contents, end, "source_path", VS(source_path)) &&
                 celestial_structure_logical_map_id_valid(source_path) &&
                 preflight_json_string(contents, end, "source_sha256", VS(source_sha)) &&
                 preflight_hex(source_sha, SHA256_DIGEST_LENGTH * 2) &&
                 preflight_json_string(contents, end, "map_sha256", VS(map_sha)) &&
                 preflight_hex(map_sha, SHA256_DIGEST_LENGTH * 2) &&
                 preflight_json_string(contents, end, "content_commit", VS(content_commit)) &&
                 preflight_hex(content_commit, 40);
    if (valid && celestial_artifact_commit[0] != '\0' &&
        strcmp(content_commit, celestial_artifact_commit) != 0) {
        valid = false;
    }
    char source_file[HUGE_BUF], actual_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    if (valid && (snprintf(source_file, sizeof(source_file), "%s%s", settings.mapspath, source_path) < 0 ||
                  !preflight_sha256_file(source_file, actual_sha) ||
                  strcmp(actual_sha, source_sha) != 0)) {
        valid = false;
    }
    char map_file[HUGE_BUF], actual_map_sha[SHA256_DIGEST_LENGTH * 2 + 1];
    if (valid && (!provenance_map_file(map, map_file, actual_map_sha) ||
                  strcmp(actual_map_sha, map_sha) != 0)) {
        valid = false;
    }
    free(contents);
    return valid ? true
                 : set_error(error,
                              error_size,
                             "temporary v1 map %s has changed or unprovable source lineage",
                             map_path(map));
}

#ifndef WIN32
static bool quarantine_private_map_file(const char *path, const char *reason) {
    char directory[HUGE_BUF], destination[HUGE_BUF];
    char *base = path_basename(path);
    if (base == NULL || snprintf(directory,
                                 sizeof(directory),
                                 "%s/celestial-quarantine",
                                 settings.datapath) < 0 ||
        strlen(directory) >= sizeof(directory)) {
        free(base);
        return false;
    }
    path_ensure_directories(directory);
    int written = snprintf(destination,
                           sizeof(destination),
                           "%s/private-%ld-%s",
                           directory,
                           (long)getpid(),
                           base);
    free(base);
    if (written < 0 || (size_t)written >= sizeof(destination)) {
        return false;
    }
    for (unsigned int attempt = 0; path_exists(destination) && attempt < 100; attempt++) {
        char *attempt_base = path_basename(path);
        if (attempt_base == NULL) {
            return false;
        }
        written = snprintf(destination,
                           sizeof(destination),
                           "%s/private-%ld-%u-%s",
                           directory,
                           (long)getpid(),
                           attempt + 1,
                           attempt_base);
        free(attempt_base);
        if (written < 0 || (size_t)written >= sizeof(destination)) {
            return false;
        }
    }
    if (path_exists(destination) || path_rename(path, destination) != 0) {
        return false;
    }
    LOG(ERROR,
        "Quarantined celestial private map %s as %s (%s).",
        path,
        destination,
        reason != NULL ? reason : "unprovable state");
    return true;
}

static bool preflight_private_map_file(const char *path, char *error, size_t error_size) {
    char reason[HUGE_BUF] = "";
    char source_path[HUGE_BUF];
    if (!celestial_private_source_from_path(path, source_path)) {
        snprintf(VS(reason), "invalid $-demangled authored source");
    }

    mapstruct *map = NULL;
    FILE *fp = NULL;
    if (reason[0] == '\0') {
        fp = fopen(path, "rb");
        map = get_linked_map();
        FREE_AND_COPY_HASH(map->path, path);
        map->map_flags |= MAP_FLAG_UNIQUE;
        if (fp == NULL || !load_map_header(map, fp)) {
            snprintf(VS(reason), "malformed private map header");
        } else if (map->celestial_schema != 1 ||
                   !celestial_structure_validate_header(map, VS(reason))) {
            if (reason[0] == '\0') {
                snprintf(VS(reason), "private map is not a valid celestial-v1 map");
            }
        } else if (!celestial_structure_validate_provenance(map, VS(reason))) {
            if (reason[0] == '\0') {
                snprintf(VS(reason), "private map provenance is invalid");
            }
        }
    }
    if (fp != NULL) {
        fclose(fp);
    }
    if (map != NULL) {
        delete_map(map);
    }
    if (reason[0] == '\0') {
        return true;
    }
    if (!quarantine_private_map_file(path, reason)) {
        return set_error(error,
                         error_size,
                         "cannot quarantine celestial private map %s (%s)",
                         path,
                         reason);
    }
    return true;
}

static bool preflight_private_map_directory(const char *directory,
                                            unsigned int depth,
                                            size_t *records,
                                            char *error,
                                            size_t error_size) {
    if (depth > 32) {
        return set_error(error, error_size, "celestial private-map directory nesting is too deep");
    }
    DIR *directory_handle = opendir(directory);
    if (directory_handle == NULL) {
        return set_error(error,
                         error_size,
                         "cannot inspect celestial private maps: %s",
                         strerror(errno));
    }
    struct dirent *entry;
    while ((entry = readdir(directory_handle)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[HUGE_BUF];
        int written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            closedir(directory_handle);
            return set_error(error, error_size, "celestial private-map path is too long");
        }
        struct stat statbuf;
        if (lstat(path, &statbuf) != 0) {
            closedir(directory_handle);
            return set_error(error,
                             error_size,
                             "cannot inspect celestial private map %s: %s",
                             path,
                             strerror(errno));
        }
        if (S_ISDIR(statbuf.st_mode)) {
            if (!preflight_private_map_directory(path, depth + 1, records, error, error_size)) {
                closedir(directory_handle);
                return false;
            }
        } else if (strchr(entry->d_name, '$') != NULL) {
            if (++*records > 65536) {
                closedir(directory_handle);
                return set_error(error, error_size, "celestial private-map inventory is not bounded");
            }
            if (!S_ISREG(statbuf.st_mode)) {
                if (!quarantine_private_map_file(path, "private map is not a regular file")) {
                    closedir(directory_handle);
                    return set_error(error,
                                     error_size,
                                     "cannot quarantine non-regular celestial private map %s",
                                     path);
                }
            } else if (!preflight_private_map_file(path, error, error_size)) {
                closedir(directory_handle);
                return false;
            }
        }
    }
    closedir(directory_handle);
    return true;
}

static bool preflight_private_maps(char *error, size_t error_size) {
    char directory[HUGE_BUF];
    int written = snprintf(directory, sizeof(directory), "%s/players", settings.datapath);
    if (written < 0 || (size_t)written >= sizeof(directory)) {
        return set_error(error, error_size, "celestial private-map root path is too long");
    }
    if (!path_exists(directory)) {
        return true;
    }
    size_t records = 0;
    return preflight_private_map_directory(directory, 0, &records, error, error_size);
}
#endif

static bool map_transaction_path(const mapstruct *map,
                                 char output[HUGE_BUF],
                                 char transaction_id[SHA256_DIGEST_LENGTH * 2 + 1]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (map == NULL || map->path == NULL ||
        SHA256((const unsigned char *)map->path, strlen(map->path), digest) == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(transaction_id + i * 2, 3, "%02x", digest[i]);
    }
    transaction_id[sizeof(digest) * 2] = '\0';
    int written = snprintf(output,
                           HUGE_BUF,
                           "%s/celestial-transactions/%s.json",
                           settings.datapath,
                           transaction_id);
    return written >= 0 && (size_t)written < HUGE_BUF;
}

static bool map_transaction_write(const mapstruct *map,
                                  const char *state,
                                  const char *map_file,
                                  const char *unique_file,
                                  char *error,
                                  size_t error_size) {
    char transaction[HUGE_BUF], transaction_id[SHA256_DIGEST_LENGTH * 2 + 1];
    if (!map_transaction_path(map, transaction, transaction_id) || map_file == NULL ||
        unique_file == NULL) {
        return set_error(error, error_size, "cannot resolve celestial map transaction identity");
    }
    char contents[HUGE_BUF];
    int written = snprintf(contents,
                           sizeof(contents),
                           "{\n"
                           "  \"schema_version\": 1,\n"
                           "  \"state\": \"%s\",\n"
                           "  \"map_path\": \"%s\",\n"
                           "  \"map_file\": \"%s\",\n"
                           "  \"unique_file\": \"%s\"\n"
                           "}\n",
                           state,
                           map->path,
                           map_file,
                           unique_file);
    if (written < 0 || (size_t)written >= sizeof(contents)) {
        return set_error(error, error_size, "celestial map transaction record is too large");
    }
    char directory[HUGE_BUF];
    if (snprintf(directory, sizeof(directory), "%s/celestial-transactions", settings.datapath) < 0 ||
        strlen(directory) >= sizeof(directory)) {
        return set_error(error, error_size, "celestial map transaction directory is too long");
    }
    path_ensure_directories(directory);
    if (!path_write_atomic(transaction, contents, (size_t)written, SAVE_MODE)) {
        return set_error(error,
                         error_size,
                         "cannot durably publish celestial map transaction %s",
                         transaction_id);
    }
    return true;
}

bool celestial_structure_begin_map_transaction(const mapstruct *map,
                                               const char *map_file,
                                               const char *unique_file,
                                               char *error,
                                               size_t error_size) {
    if (map == NULL || map->celestial_schema != 1 || map->path == NULL) {
        return set_error(error, error_size, "cannot journal a non-v1 map transaction");
    }
    return map_transaction_write(map, "prepared", map_file, unique_file, error, error_size);
}

bool celestial_structure_commit_map_transaction(const mapstruct *map, char *error, size_t error_size) {
    char transaction[HUGE_BUF], transaction_id[SHA256_DIGEST_LENGTH * 2 + 1];
    if (!map_transaction_path(map, transaction, transaction_id)) {
        return set_error(error, error_size, "cannot resolve celestial map transaction identity");
    }
    size_t size;
    char *contents = preflight_read_file(transaction, &size, error, error_size);
    if (contents == NULL) {
        return false;
    }
    const char *end = contents + size;
    char map_file[HUGE_BUF], unique_file[HUGE_BUF], map_path_value[HUGE_BUF], state[32];
    uint64_t schema;
    bool valid = preflight_json_uint(contents, end, "schema_version", &schema) && schema == 1 &&
                 preflight_json_string(contents, end, "state", VS(state)) &&
                 strcmp(state, "prepared") == 0 &&
                 preflight_json_string(contents, end, "map_path", VS(map_path_value)) &&
                 strcmp(map_path_value, map->path) == 0 &&
                 preflight_json_string(contents, end, "map_file", VS(map_file)) &&
                 preflight_json_string(contents, end, "unique_file", VS(unique_file));
    free(contents);
    if (!valid) {
        return set_error(error, error_size, "celestial map transaction %s is not prepared", transaction_id);
    }
    return map_transaction_write(map, "committed", map_file, unique_file, error, error_size);
}

bool celestial_structure_finish_map_transaction(const mapstruct *map, char *error, size_t error_size) {
    char transaction[HUGE_BUF], transaction_id[SHA256_DIGEST_LENGTH * 2 + 1];
    if (!map_transaction_path(map, transaction, transaction_id)) {
        return set_error(error, error_size, "cannot resolve celestial map transaction identity");
    }
    if (unlink(transaction) != 0 && errno != ENOENT) {
        return set_error(error,
                         error_size,
                         "cannot retire committed celestial map transaction %s",
                         transaction_id);
    }
    return true;
}

#ifndef WIN32
static bool quarantine_transaction_file(const char *source, const char *transaction_id) {
    if (source == NULL || !path_exists(source)) {
        return true;
    }
    char directory[HUGE_BUF], destination[HUGE_BUF];
    if (snprintf(directory, sizeof(directory), "%s/celestial-quarantine", settings.datapath) < 0 ||
        strlen(directory) >= sizeof(directory)) {
        return false;
    }
    path_ensure_directories(directory);
    int written = snprintf(destination,
                           sizeof(destination),
                           "%s/transaction-%s-%ld",
                           directory,
                           transaction_id,
                           (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(destination)) {
        return false;
    }
    for (unsigned int attempt = 0; path_exists(destination) && attempt < 100; attempt++) {
        written = snprintf(destination,
                           sizeof(destination),
                           "%s/transaction-%s-%ld-%u",
                           directory,
                           transaction_id,
                           (long)getpid(),
                           attempt + 1);
        if (written < 0 || (size_t)written >= sizeof(destination)) {
            return false;
        }
    }
    return !path_exists(destination) && path_rename(source, destination) == 0;
}
#endif

bool celestial_structure_recover_map_transactions(char *error, size_t error_size) {
#ifdef WIN32
    (void)error;
    (void)error_size;
    return true;
#else
    char directory[HUGE_BUF];
    if (snprintf(directory, sizeof(directory), "%s/celestial-transactions", settings.datapath) < 0 ||
        strlen(directory) >= sizeof(directory) || !path_exists(directory)) {
        return true;
    }
    DIR *directory_handle = opendir(directory);
    if (directory_handle == NULL) {
        return set_error(error, error_size, "cannot inspect celestial map transactions: %s", strerror(errno));
    }
    struct dirent *entry;
    size_t records = 0;
    while ((entry = readdir(directory_handle)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        if (name_length != SHA256_DIGEST_LENGTH * 2 + 5 ||
            strcmp(entry->d_name + name_length - 5, ".json") != 0) {
            continue;
        }
        char transaction_id[SHA256_DIGEST_LENGTH * 2 + 1];
        memcpy(transaction_id, entry->d_name, sizeof(transaction_id) - 1);
        transaction_id[sizeof(transaction_id) - 1] = '\0';
        if (!preflight_hex(transaction_id, SHA256_DIGEST_LENGTH * 2) || ++records > 256) {
            closedir(directory_handle);
            return set_error(error, error_size, "celestial map transaction directory is not bounded");
        }
        char transaction[HUGE_BUF];
        if (snprintf(transaction, sizeof(transaction), "%s/%s", directory, entry->d_name) < 0 ||
            strlen(transaction) >= sizeof(transaction)) {
            closedir(directory_handle);
            return set_error(error, error_size, "celestial map transaction path is too long");
        }
        size_t size;
        char *contents = preflight_read_file(transaction, &size, error, error_size);
        if (contents == NULL) {
            closedir(directory_handle);
            return false;
        }
        const char *end = contents + size;
        char state[32], map_file[HUGE_BUF], unique_file[HUGE_BUF], map_path_value[HUGE_BUF];
        uint64_t schema;
        bool valid = preflight_json_uint(contents, end, "schema_version", &schema) && schema == 1 &&
                     preflight_json_string(contents, end, "state", VS(state)) &&
                     preflight_json_string(contents, end, "map_path", VS(map_path_value)) &&
                     celestial_map_identity_valid(map_path_value) &&
                     preflight_json_string(contents, end, "map_file", VS(map_file)) &&
                     preflight_json_string(contents, end, "unique_file", VS(unique_file));
        free(contents);
        if (!valid || (strcmp(state, "prepared") != 0 && strcmp(state, "committed") != 0)) {
            closedir(directory_handle);
            return set_error(error, error_size, "celestial map transaction %s is malformed", transaction_id);
        }
        char ledger[HUGE_BUF], ledger_id[SHA256_DIGEST_LENGTH * 2 + 1];
        bool ledger_present = provenance_path(map_path_value, ledger, ledger_id) &&
                              path_exists(ledger);
        if (strcmp(state, "committed") == 0 && path_exists(map_file) && ledger_present) {
            unlink(transaction);
            continue;
        }
        if (!quarantine_transaction_file(map_file, transaction_id) ||
            !quarantine_transaction_file(unique_file, transaction_id)) {
            closedir(directory_handle);
            return set_error(error,
                             error_size,
                             "cannot quarantine interrupted celestial map transaction %s",
                             transaction_id);
        }
        unlink(transaction);
    }
    closedir(directory_handle);
    return true;
#endif
}

/** Validate the relative path accepted by the Python generated-map factory. */
static bool generated_map_path_valid(const char *path) {
    if (path == NULL) {
        return false;
    }

    size_t length = strlen(path);
    if (length == 0 || length > 240 || path[0] == '/' || path[length - 1] == '/') {
        return false;
    }

    size_t segment_length = 0;
    for (size_t i = 0; i <= length; i++) {
        unsigned char value = (unsigned char)path[i];
        if (value == '/' || value == '\0') {
            if (segment_length == 0 || segment_length > 64) {
                return false;
            }
            segment_length = 0;
            continue;
        }
        if (value < 0x21 || value > 0x7e) {
            return false;
        }
        if (segment_length == 0) {
            if (!isalnum(value)) {
                return false;
            }
        } else if (!isalnum(value) && value != '_' && value != '-' && value != '.') {
            return false;
        }
        segment_length++;
    }
    return true;
}

bool celestial_structure_initialize_generated_map(mapstruct *map,
                                                  const char *path,
                                                  mapstruct *origin,
                                                  int light,
                                                  char *error,
                                                  size_t error_size) {
    if (map == NULL || map->width < 1 || map->width > 64 || map->height < 1 ||
        map->height > 64 || !celestial_structure_logical_map_id_valid(path)) {
        set_error(error, error_size, "generated map path or dimensions are invalid");
        return false;
    }
    if (origin == NULL || origin->in_memory != MAP_IN_MEMORY || origin->spaces == NULL ||
        origin->path == NULL || origin->celestial_schema != 1) {
        set_error(error, error_size, "generated map origin is not a resident celestial-v1 map");
        return false;
    }
    if (light < 0 || light > 40959) {
        set_error(error, error_size, "generated map light must be between 0 and 40959");
        return false;
    }
    char origin_error[HUGE_BUF];
    if (!celestial_structure_validate_header(origin, VS(origin_error))) {
        set_error(error, error_size, "generated map origin is invalid: %s", origin_error);
        return false;
    }

    if (path_exists(path)) {
        set_error(error, error_size, "generated map path collides or is not canonical");
        return false;
    }

    mapstruct *cursor;
    DL_FOREACH(first_map, cursor) {
        if (cursor != map && cursor->path != NULL && strcmp(cursor->path, path) == 0) {
            set_error(error, error_size, "generated map path already exists: %s", path);
            return false;
        }
    }

    FREE_AND_COPY_HASH(map->path, path);
    map->celestial_schema = 1;
    map->celestial_schema_seen = true;
    map->celestial_v1_header_seen = true;
    map->celestial_sky_seen = true;
    map->celestial_sky_above = CELESTIAL_SKY_SEALED;
    map->celestial_width_seen = true;
    map->celestial_height_seen = true;
    map->celestial_light_seen = true;
    map->light_value = light;
    map->celestial_region_seen = true;
    map->region = origin->region != NULL ? origin->region : region_world();
    FREE_AND_COPY_HASH(map->celestial_generated_origin, origin->path);
    map->celestial_generated_origin_seen = true;
    map->darkness = 0;
    map->map_flags &= ~MAP_FLAG_OUTDOOR;
    return true;
}

mapstruct *celestial_structure_create_map(int width,
                                          int height,
                                          const char *path,
                                          mapstruct *origin,
                                          const char *sky_above,
                                          int light,
                                          char *error,
                                          size_t error_size) {
    if (width < 1 || width > 64 || height < 1 || height > 64) {
        set_error(error, error_size, "generated map dimensions must be between 1 and 64");
        return NULL;
    }
    if (!generated_map_path_valid(path)) {
        set_error(error, error_size, "generated map path is not canonical");
        return NULL;
    }
    if (sky_above == NULL ||
        (strcmp(sky_above, "open") != 0 && strcmp(sky_above, "sealed") != 0)) {
        set_error(error, error_size, "generated map sky_above must be open or sealed");
        return NULL;
    }

    char full_path[HUGE_BUF];
    int written = snprintf(full_path, sizeof(full_path), "/python-maps/%s", path);
    if (written < 0 || (size_t)written >= sizeof(full_path)) {
        set_error(error, error_size, "generated map path is too long");
        return NULL;
    }

    mapstruct *map = get_empty_map(width, height);
    if (!celestial_structure_initialize_generated_map(map,
                                                       full_path,
                                                       origin,
                                                       light,
                                                       error,
                                                       error_size)) {
        delete_map(map);
        return NULL;
    }
    map->celestial_sky_seen = true;
    map->celestial_sky_above = strcmp(sky_above, "open") == 0 ? CELESTIAL_SKY_OPEN
                                                               : CELESTIAL_SKY_SEALED;
    return map;
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
