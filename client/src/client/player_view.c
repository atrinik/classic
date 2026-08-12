/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Deterministic, bounded replay through the live map decoder/renderer. */

#include <global.h>
#include <image_codec.h>
#include <player_view.h>
#include <animations.h>
#include <commands.h>
#include <lighting.h>
#include <toolkit/map_protocol.h>
#include <toolkit/packet.h>
#include <openssl/evp.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define PLAYER_VIEW_SCHEMA_VERSION 1
#define PLAYER_VIEW_MAX_ASSETS 256
#define PLAYER_VIEW_MAX_ANIMATIONS 128
#define PLAYER_VIEW_MAX_ANIMATION_FRAMES 256
#define PLAYER_VIEW_MAX_INPUT_SIZE (64U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_TOTAL_INPUT_SIZE (256U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_MANIFEST_SIZE (1U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_SNAPSHOT_TEXT_SIZE (4U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_OUTPUT_SIZE (128U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_ASSET_DIMENSION 4096U
#define PLAYER_VIEW_MAX_ASSET_PIXELS (16U * 1024U * 1024U)
#define PLAYER_VIEW_MAX_TOTAL_ASSET_PIXELS (64U * 1024U * 1024U)
#define PLAYER_VIEW_SHA256_HEX_SIZE 65
#define PLAYER_VIEW_BENCHMARK_ITERATIONS 101
#define PLAYER_VIEW_BENCHMARK_WARMUPS 5
#define PLAYER_VIEW_LARGE_WIDTH 1920
#define PLAYER_VIEW_LARGE_HEIGHT 1080
#define PLAYER_VIEW_MOVEMENT_TICK_MS 125U
#define PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS 480U
#define PLAYER_VIEW_MOVEMENT_IDLE_TICKS 16U
#define PLAYER_VIEW_MOVEMENT_RESUMED_TICKS 80U

typedef enum player_view_mode {
    PLAYER_VIEW_RENDER,
    PLAYER_VIEW_BENCHMARK_STANDARD,
    PLAYER_VIEW_BENCHMARK_LARGE,
    PLAYER_VIEW_BENCHMARK_MOVEMENT,
} player_view_mode_t;

typedef struct player_view_asset {
    uint16_t face;
    char *path;
    char digest[PLAYER_VIEW_SHA256_HEX_SIZE];
} player_view_asset_t;

typedef struct player_view_animation {
    uint16_t id;
    uint8_t flags;
    uint8_t facings;
    uint16_t frames[PLAYER_VIEW_MAX_ANIMATION_FRAMES];
    size_t frames_num;
} player_view_animation_t;

typedef struct player_view_manifest {
    char *input_root;
    char *settings_path;
    char *archdef_path;
    char *snapshot_path;
    char *next_snapshot_path;
    char *font_path;
    char settings_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char archdef_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char next_snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char font_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    player_view_asset_t assets[PLAYER_VIEW_MAX_ASSETS];
    size_t assets_num;
    player_view_animation_t animations[PLAYER_VIEW_MAX_ANIMATIONS];
    size_t animations_num;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t look_width;
    uint32_t look_height;
    uint32_t map_zoom;
    uint32_t clock_ms;
    bool smooth_lighting;
    bool zoom_smoothing;
    bool primary_surface;
    bool widget_render;
    bool player_names;
    bool target_ui;
} player_view_manifest_t;

static void player_view_manifest_free(player_view_manifest_t *manifest) {
    free(manifest->input_root);
    free(manifest->settings_path);
    free(manifest->archdef_path);
    free(manifest->snapshot_path);
    free(manifest->next_snapshot_path);
    free(manifest->font_path);
    for (size_t i = 0; i < manifest->assets_num; i++) {
        free(manifest->assets[i].path);
    }
    memset(manifest, 0, sizeof(*manifest));
}

static bool player_view_sha256_text_valid(const char *text) {
    if (text == NULL || strlen(text) != PLAYER_VIEW_SHA256_HEX_SIZE - 1) {
        return false;
    }
    for (size_t i = 0; i < PLAYER_VIEW_SHA256_HEX_SIZE - 1; i++) {
        if (!isxdigit((unsigned char)text[i]) || isupper((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static bool
player_view_parse_uint(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value) {
    if (text == NULL || *text == '\0' || isspace((unsigned char)*text) || *text == '-') {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool player_view_parse_bool(const char *text, bool *value) {
    if (text == NULL) {
        return false;
    }
    if (strcmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static char *player_view_xml_property(xmlNode *node, const char *name) {
    xmlChar *property = xmlGetProp(node, BAD_CAST name);
    if (property == NULL) {
        return NULL;
    }
    char *copy = xstrdup((const char *)property);
    xmlFree(property);
    return copy;
}

static bool
player_view_attribute_known(const xmlAttr *attribute, const char *const names[], size_t names_num) {
    for (size_t i = 0; i < names_num; i++) {
        if (xmlStrEqual(attribute->name, BAD_CAST names[i])) {
            return true;
        }
    }
    return false;
}

static bool
player_view_attributes_closed(xmlNode *node, const char *const names[], size_t names_num) {
    for (xmlAttr *attribute = node->properties; attribute != NULL; attribute = attribute->next) {
        if (attribute->ns != NULL || !player_view_attribute_known(attribute, names, names_num)) {
            fprintf(stderr,
                    "player-view: unknown attribute '%s' on <%s>\n",
                    (const char *)attribute->name,
                    (const char *)node->name);
            return false;
        }
    }
    return true;
}

static bool player_view_element_empty(xmlNode *node) {
    for (xmlNode *child = node->children; child != NULL; child = child->next) {
        if (child->type != XML_COMMENT_NODE) {
            fprintf(stderr,
                    "player-view: <%s> must not contain nested content\n",
                    (const char *)node->name);
            return false;
        }
    }
    return true;
}

static char *player_view_directory(const char *path) {
    char *directory = xstrdup(path);
    char *slash = strrchr(directory, '/');
#ifdef WIN32
    char *backslash = strrchr(directory, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }
#endif
    if (slash == NULL) {
        free(directory);
        return xstrdup(".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return directory;
}

static bool player_view_path_relative(const char *path) {
    return path != NULL && *path != '\0' && path[0] != '/' && path[0] != '\\' &&
           !(isalpha((unsigned char)path[0]) && path[1] == ':');
}

static char *player_view_realpath(const char *path) {
#ifdef WIN32
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

static bool player_view_path_within(const char *root, const char *path) {
    size_t root_length = strlen(root);
#ifdef WIN32
    if (_strnicmp(root, path, root_length) != 0) {
#else
    if (strncmp(root, path, root_length) != 0) {
#endif
        return false;
    }
    if (root_length == 1 && (root[0] == '/' || root[0] == '\\')) {
        return true;
    }
    return path[root_length] == '\0' || path[root_length] == '/' || path[root_length] == '\\';
}

static char *player_view_resolve_path(const char *base, const char *relative, const char *root) {
    if (!player_view_path_relative(relative)) {
        return NULL;
    }

    size_t size = strlen(base) + strlen(relative) + 2;
    if (size > HUGE_BUF * 4U) {
        return NULL;
    }
    char *joined = xmalloc(size);
    snprintf(joined, size, "%s/%s", base, relative);
    char *resolved = player_view_realpath(joined);
    free(joined);
    if (resolved == NULL || (root != NULL && !player_view_path_within(root, resolved))) {
        free(resolved);
        return NULL;
    }

    struct stat attributes;
    if (stat(resolved, &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
        attributes.st_size < 0 || (uint64_t)attributes.st_size > PLAYER_VIEW_MAX_INPUT_SIZE) {
        free(resolved);
        return NULL;
    }
    return resolved;
}

static bool player_view_file_sha256(const char *path, char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool success = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    uint8_t buffer[16384];
    size_t total = 0;
    while (success) {
        size_t count = fread(buffer, 1, sizeof(buffer), stream);
        if (count != 0) {
            total += count;
            if (total > PLAYER_VIEW_MAX_INPUT_SIZE ||
                EVP_DigestUpdate(context, buffer, count) != 1) {
                success = false;
            }
        }
        if (count < sizeof(buffer)) {
            if (ferror(stream)) {
                success = false;
            }
            break;
        }
    }

    uint8_t raw[EVP_MAX_MD_SIZE];
    unsigned int raw_size = 0;
    if (success && (EVP_DigestFinal_ex(context, raw, &raw_size) != 1 || raw_size != 32)) {
        success = false;
    }
    EVP_MD_CTX_free(context);
    if (fclose(stream) != 0) {
        success = false;
    }
    if (!success) {
        return false;
    }

    for (size_t i = 0; i < 32; i++) {
        snprintf(&digest[i * 2], 3, "%02x", raw[i]);
    }
    return true;
}

static bool player_view_verify_input(const char *kind,
                                     const char *path,
                                     const char *expected,
                                     uint64_t *total_size) {
    struct stat attributes;
    if (stat(path, &attributes) != 0 || !S_ISREG(attributes.st_mode) || attributes.st_size < 0 ||
        (uint64_t)attributes.st_size > PLAYER_VIEW_MAX_TOTAL_INPUT_SIZE - *total_size) {
        fprintf(stderr, "player-view: total frozen input size exceeds the limit\n");
        return false;
    }
    *total_size += (uint64_t)attributes.st_size;

    char actual[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!player_view_file_sha256(path, actual)) {
        fprintf(stderr, "player-view: cannot hash %s input %s\n", kind, path);
        return false;
    }
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr,
                "player-view: %s digest mismatch for %s (expected %s, got %s)\n",
                kind,
                path,
                expected,
                actual);
        return false;
    }
    return true;
}

static bool player_view_parse_animation_frames(const char *text,
                                               player_view_animation_t *animation) {
    char *copy = xstrdup(text);
    char *cursor = copy;
    while (*cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        uint32_t face;
        if (animation->frames_num == arraysize(animation->frames) ||
            !player_view_parse_uint(cursor, 1, MAX_FACE_TILES - 1, &face)) {
            free(copy);
            return false;
        }
        animation->frames[animation->frames_num++] = (uint16_t)face;
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
        if (*cursor == '\0') {
            free(copy);
            return false;
        }
    }
    free(copy);
    return animation->frames_num != 0 && animation->frames_num % animation->facings == 0;
}

static bool player_view_manifest_parse(const char *manifest_path,
                                       player_view_manifest_t *manifest) {
    memset(manifest, 0, sizeof(*manifest));
    char *canonical_manifest = player_view_realpath(manifest_path);
    if (canonical_manifest == NULL) {
        fprintf(stderr, "player-view: cannot resolve manifest %s\n", manifest_path);
        return false;
    }

    struct stat manifest_stat;
    if (stat(canonical_manifest, &manifest_stat) != 0 || !S_ISREG(manifest_stat.st_mode) ||
        manifest_stat.st_size <= 0 ||
        (uint64_t)manifest_stat.st_size > PLAYER_VIEW_MAX_MANIFEST_SIZE) {
        fprintf(stderr, "player-view: manifest is not a bounded regular file\n");
        free(canonical_manifest);
        return false;
    }

    xmlDoc *document =
        xmlReadFile(canonical_manifest,
                    NULL,
                    XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (document == NULL || document->intSubset != NULL || document->extSubset != NULL) {
        fprintf(stderr, "player-view: manifest is not safe, closed XML\n");
        xmlFreeDoc(document);
        free(canonical_manifest);
        return false;
    }
    xmlNode *root = xmlDocGetRootElement(document);
    const char *const root_attributes[] = {"version",
                                           "renderer",
                                           "input-root",
                                           "settings",
                                           "settings-sha256",
                                           "archdef",
                                           "archdef-sha256",
                                           "snapshot",
                                           "snapshot-sha256",
                                           "next-snapshot",
                                           "next-snapshot-sha256",
                                           "font",
                                           "font-sha256",
                                           "viewport-width",
                                           "viewport-height",
                                           "look-width",
                                           "look-height",
                                           "map-zoom",
                                           "smooth-lighting",
                                           "zoom-smoothing",
                                           "primary-surface",
                                           "widget-render",
                                           "player-names",
                                           "target-ui",
                                           "clock-ms",
                                           "expected-ui-pixels-sha256",
                                           "expected-pixels-sha256"};
    bool success = root != NULL && root->ns == NULL && root->nsDef == NULL &&
                   xmlStrEqual(root->name, BAD_CAST "player-view") &&
                   player_view_attributes_closed(root, root_attributes, arraysize(root_attributes));

    char *version = success ? player_view_xml_property(root, "version") : NULL;
    char *renderer = success ? player_view_xml_property(root, "renderer") : NULL;
    char *input_root = success ? player_view_xml_property(root, "input-root") : NULL;
    char *settings = success ? player_view_xml_property(root, "settings") : NULL;
    char *settings_digest = success ? player_view_xml_property(root, "settings-sha256") : NULL;
    char *archdef = success ? player_view_xml_property(root, "archdef") : NULL;
    char *archdef_digest = success ? player_view_xml_property(root, "archdef-sha256") : NULL;
    char *snapshot = success ? player_view_xml_property(root, "snapshot") : NULL;
    char *snapshot_digest = success ? player_view_xml_property(root, "snapshot-sha256") : NULL;
    char *next_snapshot = success ? player_view_xml_property(root, "next-snapshot") : NULL;
    char *next_snapshot_digest =
        success ? player_view_xml_property(root, "next-snapshot-sha256") : NULL;
    char *font = success ? player_view_xml_property(root, "font") : NULL;
    char *font_digest = success ? player_view_xml_property(root, "font-sha256") : NULL;
    char *viewport_width = success ? player_view_xml_property(root, "viewport-width") : NULL;
    char *viewport_height = success ? player_view_xml_property(root, "viewport-height") : NULL;
    char *look_width = success ? player_view_xml_property(root, "look-width") : NULL;
    char *look_height = success ? player_view_xml_property(root, "look-height") : NULL;
    char *map_zoom = success ? player_view_xml_property(root, "map-zoom") : NULL;
    char *smooth_lighting = success ? player_view_xml_property(root, "smooth-lighting") : NULL;
    char *zoom_smoothing = success ? player_view_xml_property(root, "zoom-smoothing") : NULL;
    char *primary_surface = success ? player_view_xml_property(root, "primary-surface") : NULL;
    char *widget_render = success ? player_view_xml_property(root, "widget-render") : NULL;
    char *player_names = success ? player_view_xml_property(root, "player-names") : NULL;
    char *target_ui = success ? player_view_xml_property(root, "target-ui") : NULL;
    char *clock_ms = success ? player_view_xml_property(root, "clock-ms") : NULL;
    char *expected_ui =
        success ? player_view_xml_property(root, "expected-ui-pixels-sha256") : NULL;
    char *expected = success ? player_view_xml_property(root, "expected-pixels-sha256") : NULL;

    uint32_t parsed_version;
    success =
        success &&
        player_view_parse_uint(version,
                               PLAYER_VIEW_SCHEMA_VERSION,
                               PLAYER_VIEW_SCHEMA_VERSION,
                               &parsed_version) &&
        strcmp(renderer != NULL ? renderer : "", "software") == 0 &&
        player_view_path_relative(input_root) && settings != NULL &&
        player_view_sha256_text_valid(settings_digest) && archdef != NULL &&
        player_view_sha256_text_valid(archdef_digest) && snapshot != NULL &&
        player_view_sha256_text_valid(snapshot_digest) &&
        ((next_snapshot == NULL && next_snapshot_digest == NULL) ||
         (next_snapshot != NULL && player_view_sha256_text_valid(next_snapshot_digest))) &&
        ((font == NULL && font_digest == NULL) ||
         (font != NULL && player_view_sha256_text_valid(font_digest))) &&
        player_view_parse_uint(viewport_width, 64, 4096, &manifest->viewport_width) &&
        player_view_parse_uint(viewport_height, 64, 4096, &manifest->viewport_height) &&
        player_view_parse_uint(look_width, 9, 17, &manifest->look_width) &&
        player_view_parse_uint(look_height, 9, 17, &manifest->look_height) &&
        player_view_parse_uint(map_zoom, 50, 400, &manifest->map_zoom) &&
        player_view_parse_bool(smooth_lighting, &manifest->smooth_lighting) &&
        player_view_parse_bool(zoom_smoothing, &manifest->zoom_smoothing) &&
        (primary_surface == NULL ||
         player_view_parse_bool(primary_surface, &manifest->primary_surface)) &&
        (widget_render == NULL ||
         player_view_parse_bool(widget_render, &manifest->widget_render)) &&
        (player_names == NULL || player_view_parse_bool(player_names, &manifest->player_names)) &&
        (target_ui == NULL || player_view_parse_bool(target_ui, &manifest->target_ui)) &&
        player_view_parse_uint(clock_ms, 0, UINT32_MAX, &manifest->clock_ms) &&
        player_view_sha256_text_valid(expected);
    if (success && primary_surface == NULL) {
        manifest->primary_surface = true;
    }
    bool ui_test = manifest->player_names && manifest->target_ui;
    success = success && (!manifest->widget_render || manifest->primary_surface) &&
              manifest->player_names == manifest->target_ui &&
              ((ui_test && manifest->widget_render && font != NULL &&
                player_view_sha256_text_valid(expected_ui)) ||
               (!ui_test && font == NULL && expected_ui == NULL));
#ifndef ATRINIK_WIDGET_TESTS
    success = success && !manifest->widget_render && font == NULL;
#endif

    char *manifest_directory = player_view_directory(canonical_manifest);
    if (success) {
        size_t root_size = strlen(manifest_directory) + strlen(input_root) + 2;
        char *root_joined = xmalloc(root_size);
        snprintf(root_joined, root_size, "%s/%s", manifest_directory, input_root);
        manifest->input_root = player_view_realpath(root_joined);
        free(root_joined);
        struct stat root_stat;
        success = manifest->input_root != NULL && stat(manifest->input_root, &root_stat) == 0 &&
                  S_ISDIR(root_stat.st_mode);
    }
    if (success) {
        manifest->settings_path =
            player_view_resolve_path(manifest->input_root, settings, manifest->input_root);
        manifest->archdef_path =
            player_view_resolve_path(manifest->input_root, archdef, manifest->input_root);
        manifest->snapshot_path =
            player_view_resolve_path(manifest->input_root, snapshot, manifest->input_root);
        if (next_snapshot != NULL) {
            manifest->next_snapshot_path =
                player_view_resolve_path(manifest->input_root, next_snapshot, manifest->input_root);
        }
        if (font != NULL) {
            manifest->font_path =
                player_view_resolve_path(manifest->input_root, font, manifest->input_root);
        }
        success = manifest->settings_path != NULL && manifest->archdef_path != NULL &&
                  manifest->snapshot_path != NULL &&
                  (next_snapshot == NULL || manifest->next_snapshot_path != NULL) &&
                  (font == NULL || manifest->font_path != NULL);
    }
    if (success) {
        snprintf(VS(manifest->settings_digest), "%s", settings_digest);
        snprintf(VS(manifest->archdef_digest), "%s", archdef_digest);
        snprintf(VS(manifest->snapshot_digest), "%s", snapshot_digest);
        if (next_snapshot != NULL) {
            snprintf(VS(manifest->next_snapshot_digest), "%s", next_snapshot_digest);
        }
        if (font != NULL) {
            snprintf(VS(manifest->font_digest), "%s", font_digest);
        }
        if (expected_ui != NULL) {
            snprintf(VS(manifest->expected_ui_pixels_digest), "%s", expected_ui);
        }
        snprintf(VS(manifest->expected_pixels_digest), "%s", expected);
    }

    for (xmlNode *node = success ? root->children : NULL; node != NULL; node = node->next) {
        if (node->type == XML_COMMENT_NODE) {
            continue;
        }
        if (node->type != XML_ELEMENT_NODE) {
            fprintf(stderr, "player-view: unexpected manifest content\n");
            success = false;
            break;
        }
        if (node->ns != NULL || node->nsDef != NULL) {
            fprintf(stderr, "player-view: XML namespaces are not supported\n");
            success = false;
            break;
        }
        if (xmlStrEqual(node->name, BAD_CAST "asset")) {
            const char *const attributes[] = {"face", "path", "sha256"};
            if (manifest->assets_num == arraysize(manifest->assets) ||
                !player_view_attributes_closed(node, attributes, arraysize(attributes)) ||
                !player_view_element_empty(node)) {
                success = false;
                break;
            }
            char *face = player_view_xml_property(node, "face");
            char *path = player_view_xml_property(node, "path");
            char *digest = player_view_xml_property(node, "sha256");
            uint32_t parsed_face;
            player_view_asset_t *asset = &manifest->assets[manifest->assets_num];
            success = player_view_parse_uint(face, 1, MAX_FACE_TILES - 1, &parsed_face) &&
                      path != NULL && player_view_sha256_text_valid(digest);
            for (size_t i = 0; success && i < manifest->assets_num; i++) {
                success = manifest->assets[i].face != parsed_face;
            }
            if (success) {
                asset->face = (uint16_t)parsed_face;
                asset->path =
                    player_view_resolve_path(manifest->input_root, path, manifest->input_root);
                success = asset->path != NULL;
            }
            if (success) {
                snprintf(VS(asset->digest), "%s", digest);
                manifest->assets_num++;
            }
            free(face);
            free(path);
            free(digest);
        } else if (xmlStrEqual(node->name, BAD_CAST "animation")) {
            const char *const attributes[] = {"id", "flags", "facings", "frames"};
            if (manifest->animations_num == arraysize(manifest->animations) ||
                !player_view_attributes_closed(node, attributes, arraysize(attributes)) ||
                !player_view_element_empty(node)) {
                success = false;
                break;
            }
            char *id = player_view_xml_property(node, "id");
            char *flags = player_view_xml_property(node, "flags");
            char *facings = player_view_xml_property(node, "facings");
            char *frames = player_view_xml_property(node, "frames");
            uint32_t parsed_id, parsed_flags, parsed_facings;
            player_view_animation_t *animation = &manifest->animations[manifest->animations_num];
            success = player_view_parse_uint(id, 1, MAX_FACE_TILES - 1, &parsed_id) &&
                      player_view_parse_uint(flags, 0, UINT8_MAX, &parsed_flags) &&
                      player_view_parse_uint(facings, 1, UINT8_MAX, &parsed_facings) &&
                      frames != NULL;
            for (size_t i = 0; success && i < manifest->animations_num; i++) {
                success = manifest->animations[i].id != parsed_id;
            }
            if (success) {
                animation->id = (uint16_t)parsed_id;
                animation->flags = (uint8_t)parsed_flags;
                animation->facings = (uint8_t)parsed_facings;
                success = player_view_parse_animation_frames(frames, animation);
            }
            if (success) {
                manifest->animations_num++;
            }
            free(id);
            free(flags);
            free(facings);
            free(frames);
        } else {
            fprintf(stderr,
                    "player-view: unknown manifest element <%s>\n",
                    (const char *)node->name);
            success = false;
        }
        if (!success) {
            break;
        }
    }

    for (size_t i = 0; success && i < manifest->animations_num; i++) {
        for (size_t j = 0; success && j < manifest->animations[i].frames_num; j++) {
            bool found = false;
            for (size_t k = 0; k < manifest->assets_num; k++) {
                found |= manifest->assets[k].face == manifest->animations[i].frames[j];
            }
            success = found;
        }
    }
    if (success && manifest->assets_num == 0) {
        success = false;
    }
    if (!success) {
        fprintf(stderr, "player-view: invalid or incomplete manifest %s\n", manifest_path);
    }

    free(version);
    free(renderer);
    free(input_root);
    free(settings);
    free(settings_digest);
    free(archdef);
    free(archdef_digest);
    free(snapshot);
    free(snapshot_digest);
    free(next_snapshot);
    free(next_snapshot_digest);
    free(font);
    free(font_digest);
    free(viewport_width);
    free(viewport_height);
    free(look_width);
    free(look_height);
    free(map_zoom);
    free(smooth_lighting);
    free(zoom_smoothing);
    free(primary_surface);
    free(widget_render);
    free(player_names);
    free(target_ui);
    free(clock_ms);
    free(expected_ui);
    free(expected);
    free(manifest_directory);
    xmlFreeDoc(document);
    free(canonical_manifest);
    if (!success) {
        player_view_manifest_free(manifest);
    }
    return success;
}

static bool player_view_inputs_verify(const player_view_manifest_t *manifest) {
    uint64_t total_size = 0;
    if (!player_view_verify_input("settings",
                                  manifest->settings_path,
                                  manifest->settings_digest,
                                  &total_size) ||
        !player_view_verify_input("multipart",
                                  manifest->archdef_path,
                                  manifest->archdef_digest,
                                  &total_size) ||
        !player_view_verify_input("snapshot",
                                  manifest->snapshot_path,
                                  manifest->snapshot_digest,
                                  &total_size)) {
        return false;
    }
    if (manifest->next_snapshot_path != NULL &&
        !player_view_verify_input("next snapshot",
                                  manifest->next_snapshot_path,
                                  manifest->next_snapshot_digest,
                                  &total_size)) {
        return false;
    }
    if (manifest->font_path != NULL && !player_view_verify_input("font",
                                                                 manifest->font_path,
                                                                 manifest->font_digest,
                                                                 &total_size)) {
        return false;
    }
    for (size_t i = 0; i < manifest->assets_num; i++) {
        if (!player_view_verify_input("asset",
                                      manifest->assets[i].path,
                                      manifest->assets[i].digest,
                                      &total_size)) {
            return false;
        }
    }
    return true;
}

static int player_view_hex_value(int character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool player_view_snapshot_load(const char *path, uint8_t **snapshot, size_t *snapshot_size) {
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return false;
    }
    struct stat attributes;
    if (fstat(fileno(stream), &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
        attributes.st_size <= 0 ||
        (uint64_t)attributes.st_size > PLAYER_VIEW_MAX_SNAPSHOT_TEXT_SIZE ||
        (uint64_t)attributes.st_size >= SIZE_MAX) {
        fclose(stream);
        return false;
    }
    size_t text_size = (size_t)attributes.st_size;
    char *text = xcalloc(text_size + 1, 1);
    bool success = fread(text, 1, text_size, stream) == text_size;
    if (fclose(stream) != 0) {
        success = false;
    }
    if (!success) {
        free(text);
        return false;
    }
    uint8_t *bytes = xmalloc(text_size / 2 + 1);
    size_t bytes_num = 0;
    int high = -1;
    for (size_t i = 0; i < text_size; i++) {
        if (isspace((unsigned char)text[i])) {
            continue;
        }
        int value = player_view_hex_value((unsigned char)text[i]);
        if (value < 0) {
            success = false;
            break;
        }
        if (high < 0) {
            high = value;
        } else {
            bytes[bytes_num++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    free(text);
    if (!success || high >= 0 || bytes_num == 0 || bytes_num > PACKET_PAYLOAD_MAX) {
        free(bytes);
        return false;
    }
    *snapshot = bytes;
    *snapshot_size = bytes_num;
    return true;
}

static bool player_view_animations_init(const player_view_manifest_t *manifest) {
    size_t count = 1;
    for (size_t i = 0; i < manifest->animations_num; i++) {
        count = MAX(count, (size_t)manifest->animations[i].id + 1);
    }
    animations_num = count;
    animations = xcalloc(animations_num, sizeof(*animations));
    anim_table = xcalloc(animations_num, sizeof(*anim_table));

    for (size_t i = 0; i < manifest->animations_num; i++) {
        const player_view_animation_t *animation = &manifest->animations[i];
        size_t size = 4 + animation->frames_num * 2;
        uint8_t *command = xmalloc(size);
        command[0] = animation->id >> 8;
        command[1] = animation->id & UINT8_MAX;
        command[2] = animation->flags;
        command[3] = animation->facings;
        for (size_t frame = 0; frame < animation->frames_num; frame++) {
            command[4 + frame * 2] = animation->frames[frame] >> 8;
            command[5 + frame * 2] = animation->frames[frame] & UINT8_MAX;
        }
        anim_table[animation->id].anim_cmd = command;
        anim_table[animation->id].len = size;
    }
    return true;
}

static bool player_view_png_dimensions_valid(SDL_IOStream *stream, uint64_t *total_pixels) {
    static const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    uint8_t header[24];
    if (SDL_ReadIO(stream, header, sizeof(header)) != sizeof(header) ||
        SDL_SeekIO(stream, 0, SDL_IO_SEEK_SET) < 0 ||
        memcmp(header, signature, sizeof(signature)) != 0 ||
        memcmp(&header[8], "\0\0\0\x0d", 4) != 0 || memcmp(&header[12], "IHDR", 4) != 0) {
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    for (size_t i = 0; i < 4; i++) {
        width = (width << 8) | header[16 + i];
        height = (height << 8) | header[20 + i];
    }
    uint64_t pixels = (uint64_t)width * height;
    if (width == 0 || height == 0 || width > PLAYER_VIEW_MAX_ASSET_DIMENSION ||
        height > PLAYER_VIEW_MAX_ASSET_DIMENSION || pixels > PLAYER_VIEW_MAX_ASSET_PIXELS ||
        pixels > PLAYER_VIEW_MAX_TOTAL_ASSET_PIXELS - *total_pixels) {
        return false;
    }
    *total_pixels += pixels;
    return true;
}

static bool player_view_assets_load(const player_view_manifest_t *manifest) {
    uint64_t total_pixels = 0;
    for (size_t i = 0; i < manifest->assets_num; i++) {
        const player_view_asset_t *asset = &manifest->assets[i];
        SDL_IOStream *stream = SDL_IOFromFile(asset->path, "rb");
        sprite_struct *sprite =
            stream != NULL && player_view_png_dimensions_valid(stream, &total_pixels)
                ? sprite_tryload_file(NULL, 0, stream)
                : NULL;
        if (stream != NULL) {
            SDL_CloseIO(stream);
        }
        if (sprite == NULL) {
            fprintf(stderr, "player-view: cannot decode asset %s\n", asset->path);
            return false;
        }
        FaceList[asset->face].name = xstrdup(asset->path);
        FaceList[asset->face].sprite = sprite;
    }
    return true;
}

static bool player_view_surface_sha256(SDL_Surface *surface,
                                       char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    SDL_Surface *canonical = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (canonical == NULL) {
        return false;
    }

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool success = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    uint8_t dimensions[8];
    uint32_t width = (uint32_t)canonical->w;
    uint32_t height = (uint32_t)canonical->h;
    for (size_t i = 0; i < 4; i++) {
        dimensions[i] = (uint8_t)(width >> (24 - i * 8));
        dimensions[4 + i] = (uint8_t)(height >> (24 - i * 8));
    }
    success = success && EVP_DigestUpdate(context, dimensions, sizeof(dimensions)) == 1;

    bool locked = !SDL_MUSTLOCK(canonical) || SDL_LockSurface(canonical);
    success = success && locked;
    for (int y = 0; success && y < canonical->h; y++) {
        const uint8_t *row = (const uint8_t *)canonical->pixels + (size_t)y * canonical->pitch;
        if (EVP_DigestUpdate(context, row, (size_t)canonical->w * 4) != 1) {
            success = false;
        }
    }
    if (locked && SDL_MUSTLOCK(canonical)) {
        SDL_UnlockSurface(canonical);
    }

    uint8_t raw[EVP_MAX_MD_SIZE];
    unsigned int raw_size = 0;
    if (success && (EVP_DigestFinal_ex(context, raw, &raw_size) != 1 || raw_size != 32)) {
        success = false;
    }
    EVP_MD_CTX_free(context);
    SDL_DestroySurface(canonical);
    if (!success) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        snprintf(&digest[i * 2], 3, "%02x", raw[i]);
    }
    return true;
}

static int player_view_duration_compare(const void *left, const void *right) {
    const uint64_t left_duration = *(const uint64_t *)left;
    const uint64_t right_duration = *(const uint64_t *)right;
    return (left_duration > right_duration) - (left_duration < right_duration);
}

static uint64_t player_view_benchmark(SDL_Surface *surface) {
    for (size_t i = 0; i < PLAYER_VIEW_BENCHMARK_WARMUPS; i++) {
        map_draw_map(surface);
    }

    uint64_t durations[PLAYER_VIEW_BENCHMARK_ITERATIONS];
    for (size_t i = 0; i < arraysize(durations); i++) {
        uint64_t started = SDL_GetTicksNS();
        map_draw_map(surface);
        durations[i] = SDL_GetTicksNS() - started;
    }
    qsort(durations, arraysize(durations), sizeof(*durations), player_view_duration_compare);
    return durations[arraysize(durations) / 2];
}

typedef struct player_view_movement_phase {
    const char *name;
    uint32_t ticks;
    uint64_t durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
} player_view_movement_phase_t;

static uint64_t player_view_percentile(uint64_t *durations, size_t count, size_t numerator) {
    qsort(durations, count, sizeof(*durations), player_view_duration_compare);
    size_t index = (count - 1) * numerator / 100;
    return durations[index];
}

static bool player_view_movement_draw(player_view_movement_phase_t *phase,
                                      SDL_Surface *surface,
                                      const uint8_t *snapshot,
                                      size_t snapshot_size,
                                      uint32_t *clock_ms) {
    for (uint32_t tick = 0; tick < phase->ticks; tick++) {
        LastTick = *clock_ms;
        socket_command_map((uint8_t *)snapshot, snapshot_size, 0);
        uint64_t started = SDL_GetTicksNS();
        map_draw_map(surface);
        phase->durations[tick] = SDL_GetTicksNS() - started;
        *clock_ms += PLAYER_VIEW_MOVEMENT_TICK_MS;
    }
    return true;
}

static bool player_view_movement_benchmark(SDL_Surface *surface,
                                           const player_view_manifest_t *manifest,
                                           const uint8_t *snapshot,
                                           size_t snapshot_size,
                                           const uint8_t *next_snapshot,
                                           size_t next_snapshot_size) {
    if (next_snapshot == NULL) {
        fprintf(stderr, "player-view: movement benchmark requires next-snapshot\n");
        return false;
    }

    player_view_movement_phase_t phases[] = {
        {.name = "cold", .ticks = 1},
        {.name = "sustained", .ticks = PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS},
        {.name = "idle", .ticks = PLAYER_VIEW_MOVEMENT_IDLE_TICKS},
        {.name = "resumed", .ticks = PLAYER_VIEW_MOVEMENT_RESUMED_TICKS},
    };
    uint32_t clock_ms = manifest->clock_ms;
    for (size_t i = 0; i < arraysize(phases); i++) {
        const uint8_t *packet = i == 0 ? snapshot : next_snapshot;
        size_t packet_size = i == 0 ? snapshot_size : next_snapshot_size;
        if (!player_view_movement_draw(&phases[i], surface, packet, packet_size, &clock_ms)) {
            return false;
        }
    }

    char digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!player_view_surface_sha256(surface, digest)) {
        return false;
    }
    printf("{\"schema_version\":1,\"benchmark\":\"player-view-movement\","
           "\"tick_ms\":%u,\"checkpoint_sha256\":\"%s\",\"phases\":[",
           PLAYER_VIEW_MOVEMENT_TICK_MS,
           digest);
    for (size_t i = 0; i < arraysize(phases); i++) {
        player_view_movement_phase_t *phase = &phases[i];
        uint64_t sorted[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
        memcpy(sorted, phase->durations, phase->ticks * sizeof(*sorted));
        uint64_t p50 = player_view_percentile(sorted, phase->ticks, 50);
        memcpy(sorted, phase->durations, phase->ticks * sizeof(*sorted));
        uint64_t p95 = player_view_percentile(sorted, phase->ticks, 95);
        memcpy(sorted, phase->durations, phase->ticks * sizeof(*sorted));
        uint64_t p99 = player_view_percentile(sorted, phase->ticks, 99);
        uint64_t maximum = sorted[phase->ticks - 1];
        printf("%s{\"name\":\"%s\",\"samples\":%u,\"p50_ns\":%" PRIu64
               ",\"p95_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64
               ",\"max_ns\":%" PRIu64 "}",
               i == 0 ? "" : ",",
               phase->name,
               phase->ticks,
               p50,
               p95,
               p99,
               maximum);
    }
    printf("]}\n");
    return true;
}

static bool player_view_output_write(SDL_Surface *surface,
                                     const player_view_manifest_t *manifest,
                                     const char *output,
                                     const char *expected_digest) {
    if (strcmp(output, "-") == 0) {
        return true;
    }
    if (*output == '\0') {
        fprintf(stderr, "player-view: output path must not be empty\n");
        return false;
    }

    struct stat attributes;
#ifdef WIN32
    int output_status = stat(output, &attributes);
#else
    int output_status = lstat(output, &attributes);
#endif
    if (output_status == 0 || errno != ENOENT) {
        fprintf(stderr, "player-view: refusing to overwrite output %s\n", output);
        return false;
    }

    char *output_directory = player_view_directory(output);
    char *canonical_directory = player_view_realpath(output_directory);
    free(output_directory);
    if (canonical_directory == NULL ||
        player_view_path_within(manifest->input_root, canonical_directory)) {
        fprintf(stderr, "player-view: output must be outside the frozen input tree\n");
        free(canonical_directory);
        return false;
    }
    free(canonical_directory);

    SDL_IOStream *encoded = SDL_IOFromDynamicMem();
    if (encoded == NULL) {
        fprintf(stderr, "player-view: cannot create output encoder: %s\n", SDL_GetError());
        return false;
    }
    if (!image_codec_save_png_io(surface, encoded, false)) {
        fprintf(stderr, "player-view: cannot encode output: %s\n", SDL_GetError());
        SDL_CloseIO(encoded);
        return false;
    }
    Sint64 encoded_size = SDL_GetIOSize(encoded);
    void *encoded_data = SDL_GetPointerProperty(SDL_GetIOProperties(encoded),
                                                SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER,
                                                NULL);
    if (encoded_size <= 0 || encoded_size > PLAYER_VIEW_MAX_OUTPUT_SIZE || encoded_data == NULL) {
        fprintf(stderr, "player-view: encoded output is invalid or exceeds the limit\n");
        SDL_CloseIO(encoded);
        return false;
    }

    SDL_IOStream *reader = SDL_IOFromConstMem(encoded_data, (size_t)encoded_size);
    SDL_Surface *saved = reader != NULL ? image_codec_load_png_io(reader) : NULL;
    if (reader != NULL) {
        SDL_CloseIO(reader);
    }
    char saved_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    bool success = saved != NULL && player_view_surface_sha256(saved, saved_digest) &&
                   strcmp(saved_digest, expected_digest) == 0;
    SDL_DestroySurface(saved);
    if (!success) {
        fprintf(stderr, "player-view: encoded output did not preserve pixels\n");
        SDL_CloseIO(encoded);
        return false;
    }

    size_t temporary_size = strlen(output) + sizeof(".tmp.XXXXXX");
    char *temporary = xmalloc(temporary_size);
    snprintf(temporary, temporary_size, "%s.tmp.XXXXXX", output);
    int descriptor = mkstemp(temporary);
    if (descriptor == -1) {
        fprintf(stderr, "player-view: cannot create temporary output: %s\n", strerror(errno));
        SDL_CloseIO(encoded);
        free(temporary);
        return false;
    }

    FILE *stream = fdopen(descriptor, "wb");
    if (stream == NULL) {
        close(descriptor);
        remove(temporary);
        SDL_CloseIO(encoded);
        free(temporary);
        return false;
    }
    success = fwrite(encoded_data, 1, (size_t)encoded_size, stream) == (size_t)encoded_size &&
              fflush(stream) == 0;
#ifndef WIN32
    if (success && fsync(descriptor) != 0) {
        success = false;
    }
#endif
    if (fclose(stream) != 0) {
        success = false;
    }
    SDL_CloseIO(encoded);
    if (!success) {
        fprintf(stderr, "player-view: cannot write temporary output: %s\n", strerror(errno));
        remove(temporary);
        free(temporary);
        return false;
    }

#ifdef WIN32
    success = rename(temporary, output) == 0;
#else
    success = link(temporary, output) == 0;
#endif
    if (!success) {
        fprintf(stderr, "player-view: cannot publish output %s: %s\n", output, strerror(errno));
        remove(temporary);
        free(temporary);
        return false;
    }
#ifndef WIN32
    if (remove(temporary) != 0) {
        fprintf(stderr, "player-view: warning: cannot remove temporary output %s\n", temporary);
    }
#endif
    free(temporary);
    return true;
}

int player_view_main(int argc, char *argv[]) {
    player_view_mode_t mode = PLAYER_VIEW_RENDER;
    if (argc == 3 && (strcmp(argv[0], "--player-view-benchmark") == 0 ||
                      strcmp(argv[0], "--player-view-movement-benchmark") == 0)) {
        if (strcmp(argv[2], "standard") == 0) {
            mode = strcmp(argv[0], "--player-view-benchmark") == 0
                       ? PLAYER_VIEW_BENCHMARK_STANDARD
                       : PLAYER_VIEW_BENCHMARK_MOVEMENT;
        } else if (strcmp(argv[2], "large") == 0) {
            mode = strcmp(argv[0], "--player-view-benchmark") == 0
                       ? PLAYER_VIEW_BENCHMARK_LARGE
                       : PLAYER_VIEW_BENCHMARK_MOVEMENT;
        } else {
            fprintf(stderr, "player-view: benchmark viewport must be standard or large\n");
            return 2;
        }
    } else if (argc != 3 || strcmp(argv[0], "--player-view") != 0) {
        fprintf(stderr,
                "usage: atrinik --player-view MANIFEST OUTPUT.png|-\n"
                "       atrinik --player-view-benchmark MANIFEST standard|large\n"
                "       atrinik --player-view-movement-benchmark MANIFEST standard|large\n");
        return 2;
    }

    player_view_manifest_t manifest;
    if (!player_view_manifest_parse(argv[1], &manifest)) {
        return 3;
    }
    if ((mode == PLAYER_VIEW_BENCHMARK_LARGE ||
         (mode == PLAYER_VIEW_BENCHMARK_MOVEMENT && strcmp(argv[2], "large") == 0))) {
        manifest.viewport_width = PLAYER_VIEW_LARGE_WIDTH;
        manifest.viewport_height = PLAYER_VIEW_LARGE_HEIGHT;
    }
    if (!player_view_inputs_verify(&manifest)) {
        player_view_manifest_free(&manifest);
        return 4;
    }

    uint8_t *snapshot = NULL;
    size_t snapshot_size = 0;
    uint8_t *next_snapshot = NULL;
    size_t next_snapshot_size = 0;
    if (!player_view_snapshot_load(manifest.snapshot_path, &snapshot, &snapshot_size) ||
        !map_protocol_validate(snapshot,
                               snapshot_size,
                               0,
                               MAP_LOOK_TO_WIRE_SIZE(manifest.look_width),
                               MAP_LOOK_TO_WIRE_SIZE(manifest.look_height))) {
        fprintf(stderr, "player-view: malformed or incompatible MAP snapshot\n");
        free(snapshot);
        player_view_manifest_free(&manifest);
        return 5;
    }
    if (manifest.next_snapshot_path != NULL &&
        (!player_view_snapshot_load(manifest.next_snapshot_path,
                                    &next_snapshot,
                                    &next_snapshot_size) ||
         !map_protocol_validate(next_snapshot,
                                next_snapshot_size,
                                0,
                                MAP_LOOK_TO_WIRE_SIZE(manifest.look_width),
                                MAP_LOOK_TO_WIRE_SIZE(manifest.look_height)))) {
        fprintf(stderr, "player-view: malformed or incompatible next MAP snapshot\n");
        free(next_snapshot);
        free(snapshot);
        player_view_manifest_free(&manifest);
        return 5;
    }

    bool settings_ready = settings_init_read_only(manifest.settings_path);
    bool sdl_ready = false;
    bool text_ready = false;
    bool map_ready = false;
    SDL_Surface *surface = NULL;
    SDL_Surface *map_widget_surface = NULL;
    int result = 6;
    if (!settings_ready) {
        fprintf(stderr, "player-view: cannot load immutable setting defaults\n");
        goto cleanup;
    }
    setting_set_int(OPT_CAT_MAP, OPT_MAP_WIDTH, manifest.look_width);
    setting_set_int(OPT_CAT_MAP, OPT_MAP_HEIGHT, manifest.look_height);
    setting_set_int(OPT_CAT_MAP, OPT_MAP_ZOOM, manifest.map_zoom);
    setting_set_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING, manifest.smooth_lighting);
    setting_set_int(OPT_CAT_MAP, OPT_PLAYER_NAMES, manifest.player_names ? 1 : 0);
    setting_set_int(OPT_CAT_CLIENT, OPT_ZOOM_SMOOTH, manifest.zoom_smoothing);

    if (!SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE)) {
        fprintf(stderr, "player-view: cannot select the offscreen SDL driver\n");
        goto cleanup;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "player-view: cannot initialize offscreen SDL: %s\n", SDL_GetError());
        goto cleanup;
    }
    sdl_ready = true;
    if (manifest.font_path != NULL) {
#ifdef ATRINIK_WIDGET_TESTS
        text_test_font_path_set(manifest.font_path);
        text_init();
        text_ready = true;
#endif
    }
    sprite_init_system();
    memset(&cpl, 0, sizeof(cpl));
    if (manifest.target_ui) {
        cpl.target_code = CMD_TARGET_ENEMY;
        cpl.target_hp = 64;
        snprintf(VS(cpl.target_color), "%s", "ffffff");
        snprintf(VS(cpl.target_name), "%s", "Local Player");
    }
    memset(&MapData, 0, sizeof(MapData));
    memset(FaceList, 0, sizeof(FaceList));
    map_runtime_init();
    map_ready = true;
    if (!load_mapdef_file(manifest.archdef_path) || !player_view_assets_load(&manifest) ||
        !player_view_animations_init(&manifest)) {
        fprintf(stderr, "player-view: cannot initialize frozen renderer inputs\n");
        goto cleanup;
    }

    surface = SDL_CreateSurface((int)manifest.viewport_width,
                                (int)manifest.viewport_height,
                                SDL_PIXELFORMAT_ARGB8888);
    if (surface == NULL || !SDL_FillSurfaceRect(surface, NULL, 0)) {
        fprintf(stderr, "player-view: cannot create viewport: %s\n", SDL_GetError());
        goto cleanup;
    }
    map_widget_surface = manifest.widget_render ? NULL : surface;
    if (!manifest.primary_surface) {
        map_widget_surface = SDL_CreateSurface((int)manifest.viewport_width,
                                               (int)manifest.viewport_height,
                                               SDL_PIXELFORMAT_ARGB8888);
        if (map_widget_surface == NULL) {
            fprintf(stderr,
                    "player-view: cannot create primary-surface sentinel: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
    }
    widgetdata map_widget = {
        .surface = map_widget_surface,
        .w = (int)manifest.viewport_width,
        .h = (int)manifest.viewport_height,
    };
    cur_widget[MAP_ID] = &map_widget;
    ScreenSurface = surface;
    LastTick = manifest.clock_ms;
    image_missing_faces_reset();
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.player_names && manifest.target_ui) {
        widget_map_ui_test_begin();
    }
#endif
    socket_command_map(snapshot, snapshot_size, 0);
    if (image_missing_faces_detected()) {
        fprintf(stderr, "player-view: snapshot references an unavailable face\n");
        goto cleanup;
    }
    uint64_t benchmark_median_ns = 0;
    if (mode == PLAYER_VIEW_BENCHMARK_STANDARD || mode == PLAYER_VIEW_BENCHMARK_LARGE) {
        benchmark_median_ns = player_view_benchmark(surface);
    } else if (mode == PLAYER_VIEW_BENCHMARK_MOVEMENT) {
        if (!player_view_movement_benchmark(surface,
                                            &manifest,
                                            snapshot,
                                            snapshot_size,
                                            next_snapshot,
                                            next_snapshot_size)) {
            goto cleanup;
        }
    } else if (manifest.widget_render) {
#ifdef ATRINIK_WIDGET_TESTS
        widget_map_draw_test(&map_widget);
        map_widget_surface = map_widget.surface;
#endif
    } else {
        map_draw_map(surface);
    }

    if (next_snapshot != NULL) {
        LastTick = 0;
        socket_command_map(next_snapshot, next_snapshot_size, 0);
        LastTick = manifest.clock_ms;
        if (image_missing_faces_detected()) {
            fprintf(stderr, "player-view: next snapshot references an unavailable face\n");
            goto cleanup;
        }
        SDL_FillSurfaceRect(surface, NULL, 0);
        if (manifest.widget_render) {
#ifdef ATRINIK_WIDGET_TESTS
            widget_map_draw_test(&map_widget);
            map_widget_surface = map_widget.surface;
#endif
        } else {
            map_draw_map(surface);
        }
    }

#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.player_names && manifest.target_ui) {
        if (!widget_map_ui_test_end()) {
            fprintf(stderr, "player-view: name or target UI was not rendered\n");
            goto cleanup;
        }
        char ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
        if (!player_view_surface_sha256(surface, ui_pixels_digest)) {
            fprintf(stderr, "player-view: cannot hash name and target UI pixels\n");
            goto cleanup;
        }
        if (strcmp(ui_pixels_digest, manifest.expected_ui_pixels_digest) != 0) {
            fprintf(stderr,
                    "player-view: name and target UI pixel mismatch (expected %s, got %s)\n",
                    manifest.expected_ui_pixels_digest,
                    ui_pixels_digest);
            result = 7;
            goto cleanup;
        }
        setting_set_int(OPT_CAT_MAP, OPT_PLAYER_NAMES, 0);
        cpl.target_code = 0;
        map_redraw_flag = 1;
        SDL_FillSurfaceRect(surface, NULL, 0);
        widget_map_draw_test(&map_widget);
        map_widget_surface = map_widget.surface;
    }
#endif

    if (!player_view_inputs_verify(&manifest)) {
        fprintf(stderr, "player-view: frozen inputs changed during replay\n");
        goto cleanup;
    }

    char pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!player_view_surface_sha256(surface, pixels_digest)) {
        fprintf(stderr, "player-view: cannot hash rendered pixels\n");
        goto cleanup;
    }
    if (mode == PLAYER_VIEW_RENDER && strcmp(pixels_digest, manifest.expected_pixels_digest) != 0) {
        fprintf(stderr,
                "player-view: pixel mismatch (expected %s, got %s)\n",
                manifest.expected_pixels_digest,
                pixels_digest);
        result = 7;
        goto cleanup;
    }
    if (mode == PLAYER_VIEW_RENDER) {
        if (!player_view_output_write(surface, &manifest, argv[2], pixels_digest)) {
            goto cleanup;
        }
        printf("%s  %s\n", pixels_digest, argv[2]);
    } else if (mode != PLAYER_VIEW_BENCHMARK_MOVEMENT) {
        printf("player-view-benchmark\t%s\t%" PRIu64 "\t%" PRIu64 "\n",
               mode == PLAYER_VIEW_BENCHMARK_STANDARD ? "standard" : "large",
               (uint64_t)PLAYER_VIEW_BENCHMARK_ITERATIONS,
               benchmark_median_ns);
    }
    result = 0;

cleanup:
    cur_widget[MAP_ID] = NULL;
    ScreenSurface = NULL;
    if (map_widget_surface != surface) {
        SDL_DestroySurface(map_widget_surface);
    }
    SDL_DestroySurface(surface);
    if (map_ready) {
        map_runtime_deinit();
    }
    if (animations != NULL || anim_table != NULL) {
        anims_deinit();
    }
    image_bmaps_deinit();
    if (FormatHolder != NULL) {
        SDL_DestroySurface(FormatHolder);
        FormatHolder = NULL;
    }
    if (sdl_ready) {
        if (text_ready) {
            text_deinit();
#ifdef ATRINIK_WIDGET_TESTS
            text_test_font_path_set(NULL);
#endif
        }
        SDL_Quit();
    }
    if (settings_ready) {
        settings_deinit_read_only();
    }
    free(snapshot);
    free(next_snapshot);
    player_view_manifest_free(&manifest);
    return result;
}
