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
#ifndef WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif

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
#define PLAYER_VIEW_MOVEMENT_PACKETS 5U
#define PLAYER_VIEW_MOVEMENT_ACTIVE_PACKETS 4U
#define PLAYER_VIEW_MOVEMENT_SCHEMA_VERSION 4U
#define PLAYER_VIEW_MOVEMENT_WINDOW_TICKS 32U
#define PLAYER_VIEW_MOVEMENT_FIXTURE_SCHEMA 2U
#define PLAYER_VIEW_MOVEMENT_CHECKPOINTS 12U
#define PLAYER_VIEW_MOVEMENT_RNG_SEED UINT64_C(0x1961932026)
#define PLAYER_VIEW_MOVEMENT_SIMULATED_COMMAND_US UINT64_C(5000)

#ifndef ATRINIK_BUILD_TYPE
#define ATRINIK_BUILD_TYPE "unknown"
#endif
#ifndef ATRINIK_COMPILER_ID
#define ATRINIK_COMPILER_ID "unknown"
#endif
#ifndef ATRINIK_COMPILER_VERSION
#define ATRINIK_COMPILER_VERSION "unknown"
#endif
#ifndef ATRINIK_SYSTEM_NAME
#define ATRINIK_SYSTEM_NAME "unknown"
#endif
#ifndef ATRINIK_BENCHMARK_REVISION
#define ATRINIK_BENCHMARK_REVISION "unknown"
#endif
#ifndef ATRINIK_BENCHMARK_DIRTY
#define ATRINIK_BENCHMARK_DIRTY "unknown"
#endif

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
    char *transition_snapshot_path;
    char *font_path;
    char settings_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char archdef_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char next_snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char transition_snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char font_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_standard_checkpoint_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char manifest_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
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
    uint32_t resize_width_delta;
    uint32_t resize_height_delta;
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
    free(manifest->transition_snapshot_path);
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
                                           "transition-snapshot",
                                           "transition-snapshot-sha256",
                                           "font",
                                           "font-sha256",
                                           "viewport-width",
                                           "viewport-height",
                                           "resize-width-delta",
                                           "resize-height-delta",
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
                                           "expected-standard-checkpoint-sha256",
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
    char *transition_snapshot =
        success ? player_view_xml_property(root, "transition-snapshot") : NULL;
    char *transition_snapshot_digest =
        success ? player_view_xml_property(root, "transition-snapshot-sha256") : NULL;
    char *font = success ? player_view_xml_property(root, "font") : NULL;
    char *font_digest = success ? player_view_xml_property(root, "font-sha256") : NULL;
    char *viewport_width = success ? player_view_xml_property(root, "viewport-width") : NULL;
    char *viewport_height = success ? player_view_xml_property(root, "viewport-height") : NULL;
    char *resize_width_delta =
        success ? player_view_xml_property(root, "resize-width-delta") : NULL;
    char *resize_height_delta =
        success ? player_view_xml_property(root, "resize-height-delta") : NULL;
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
    char *expected_standard_checkpoint =
        success ? player_view_xml_property(root, "expected-standard-checkpoint-sha256") : NULL;
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
        ((transition_snapshot == NULL && transition_snapshot_digest == NULL) ||
         (transition_snapshot != NULL &&
          player_view_sha256_text_valid(transition_snapshot_digest))) &&
        ((font == NULL && font_digest == NULL) ||
         (font != NULL && player_view_sha256_text_valid(font_digest))) &&
        player_view_parse_uint(viewport_width, 64, 4096, &manifest->viewport_width) &&
        player_view_parse_uint(viewport_height, 64, 4096, &manifest->viewport_height) &&
        ((resize_width_delta == NULL && resize_height_delta == NULL) ||
         (player_view_parse_uint(resize_width_delta, 1, 4096, &manifest->resize_width_delta) &&
          player_view_parse_uint(resize_height_delta, 1, 4096, &manifest->resize_height_delta) &&
          manifest->resize_width_delta <= 4096 - manifest->viewport_width &&
          manifest->resize_height_delta <= 4096 - manifest->viewport_height)) &&
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
        (expected_standard_checkpoint == NULL ||
         player_view_sha256_text_valid(expected_standard_checkpoint)) &&
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
        if (transition_snapshot != NULL) {
            manifest->transition_snapshot_path = player_view_resolve_path(manifest->input_root,
                                                                          transition_snapshot,
                                                                          manifest->input_root);
        }
        if (font != NULL) {
            manifest->font_path =
                player_view_resolve_path(manifest->input_root, font, manifest->input_root);
        }
        success = manifest->settings_path != NULL && manifest->archdef_path != NULL &&
                  manifest->snapshot_path != NULL &&
                  (next_snapshot == NULL || manifest->next_snapshot_path != NULL) &&
                  (transition_snapshot == NULL || manifest->transition_snapshot_path != NULL) &&
                  (font == NULL || manifest->font_path != NULL);
    }
    if (success) {
        snprintf(VS(manifest->settings_digest), "%s", settings_digest);
        snprintf(VS(manifest->archdef_digest), "%s", archdef_digest);
        snprintf(VS(manifest->snapshot_digest), "%s", snapshot_digest);
        if (next_snapshot != NULL) {
            snprintf(VS(manifest->next_snapshot_digest), "%s", next_snapshot_digest);
        }
        if (transition_snapshot != NULL) {
            snprintf(VS(manifest->transition_snapshot_digest), "%s", transition_snapshot_digest);
        }
        if (font != NULL) {
            snprintf(VS(manifest->font_digest), "%s", font_digest);
        }
        if (expected_ui != NULL) {
            snprintf(VS(manifest->expected_ui_pixels_digest), "%s", expected_ui);
        }
        if (expected_standard_checkpoint != NULL) {
            snprintf(VS(manifest->expected_standard_checkpoint_digest),
                     "%s",
                     expected_standard_checkpoint);
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
    free(transition_snapshot);
    free(transition_snapshot_digest);
    free(font);
    free(font_digest);
    free(viewport_width);
    free(viewport_height);
    free(resize_width_delta);
    free(resize_height_delta);
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
    free(expected_standard_checkpoint);
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
    if (manifest->transition_snapshot_path != NULL &&
        !player_view_verify_input("transition snapshot",
                                  manifest->transition_snapshot_path,
                                  manifest->transition_snapshot_digest,
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
    uint32_t map_packets;
    uint32_t changed_packets;
    uint32_t noop_packets;
    uint32_t full_map_draws;
    uint32_t local_minimap_draws;
    uint32_t animation_ticks;
    struct {
        uint32_t reset_packet;
        uint32_t changed_map_packet;
        uint32_t noop_map_packet;
        uint32_t animation_only_tick;
        uint32_t resize;
        uint32_t map_transition;
    } full_draw_reasons;
    client_command_queue_statistics_t queue;
    lighting_benchmark_statistics_t lighting;
    lighting_benchmark_statistics_t lighting_before;
    lighting_benchmark_level_statistics_t lighting_levels_start[MAP2_LEVELS];
    lighting_benchmark_level_statistics_t lighting_levels_end[MAP2_LEVELS];
    map_benchmark_statistics_t map;
    render_profile_snapshot_t render;
    sprite_cache_statistics_t sprite_cache_start;
    sprite_cache_statistics_t sprite_cache_end;
    uint64_t frame_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    uint64_t wait_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    uint64_t loop_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    uint64_t queue_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    uint64_t map_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    uint64_t local_minimap_durations[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    size_t map_samples;
    size_t local_minimap_samples;
} player_view_movement_phase_t;

typedef struct player_view_movement_packet {
    const uint8_t *data;
    size_t size;
} player_view_movement_packet_t;

typedef struct player_view_movement_fixture {
    player_view_movement_packet_t packets[PLAYER_VIEW_MOVEMENT_PACKETS];
} player_view_movement_fixture_t;

typedef enum player_view_movement_stream {
    PLAYER_VIEW_MOVEMENT_COLD,
    PLAYER_VIEW_MOVEMENT_SUSTAINED,
    PLAYER_VIEW_MOVEMENT_IDLE,
    PLAYER_VIEW_MOVEMENT_RESUMED,
} player_view_movement_stream_t;

typedef struct player_view_movement_checkpoint {
    const char *name;
    char pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    uint64_t state_digest;
    int map_x;
    int map_y;
    int viewport_width;
    int viewport_height;
} player_view_movement_checkpoint_t;

typedef struct player_view_movement_replay {
    player_view_movement_phase_t phases[4];
    player_view_movement_checkpoint_t checkpoints[PLAYER_VIEW_MOVEMENT_CHECKPOINTS];
    size_t checkpoints_num;
    struct {
        uint32_t full_map_draws;
        uint32_t reset_packet;
        uint32_t changed_map_packet;
        uint32_t noop_map_packet;
        uint32_t animation_only_tick;
        uint32_t resize;
        uint32_t map_transition;
    } lifecycle;
} player_view_movement_replay_t;

typedef struct player_view_movement_clock {
    uint64_t now_us;
    uint64_t reads;
} player_view_movement_clock_t;

static bool player_view_output_write(SDL_Surface *surface,
                                     const player_view_manifest_t *manifest,
                                     const char *output,
                                     const char *expected_digest);

static uint64_t player_view_percentile(uint64_t *durations, size_t count, size_t numerator) {
    qsort(durations, count, sizeof(*durations), player_view_duration_compare);
    size_t index = (count - 1) * numerator / 100;
    return durations[index];
}

static bool
player_view_lighting_levels_get(lighting_benchmark_level_statistics_t levels[MAP2_LEVELS]) {
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        if (!lighting_benchmark_level_statistics_get(depth, &levels[MAP2_DEPTH_INDEX(depth)])) {
            return false;
        }
    }
    return true;
}

static const char *player_view_environment(const char *name) {
    const char *value = getenv(name);
    return value != NULL && *value != '\0' ? value : "unknown";
}

static bool player_view_json_string(const char *value) {
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20 || *cursor == '"' || *cursor == '\\') {
            return false;
        }
    }
    return true;
}

static uint64_t player_view_process_peak_rss_bytes(void) {
#ifdef WIN32
    return 0;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return 0;
    }
#ifdef __APPLE__
    return (uint64_t)usage.ru_maxrss;
#else
    return (uint64_t)usage.ru_maxrss * UINT64_C(1024);
#endif
#endif
}

static uint64_t player_view_state_hash_uint64(uint64_t hash, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); i++) {
        hash ^= (uint8_t)value;
        hash *= UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

static void player_view_timing_summary(const uint64_t *durations,
                                       size_t count,
                                       uint64_t *p50,
                                       uint64_t *p95,
                                       uint64_t *p99,
                                       uint64_t *maximum) {
    uint64_t sorted[PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS];
    HARD_ASSERT(count > 0 && count <= arraysize(sorted));
    memcpy(sorted, durations, count * sizeof(*sorted));
    *p50 = player_view_percentile(sorted, count, 50);
    memcpy(sorted, durations, count * sizeof(*sorted));
    *p95 = player_view_percentile(sorted, count, 95);
    memcpy(sorted, durations, count * sizeof(*sorted));
    *p99 = player_view_percentile(sorted, count, 99);
    *maximum = sorted[count - 1];
}

static void player_view_timing_json(const uint64_t *durations, size_t count) {
    uint64_t p50 = 0, p95 = 0, p99 = 0, maximum = 0;
    if (count != 0) {
        player_view_timing_summary(durations, count, &p50, &p95, &p99, &maximum);
    }
    printf("{\"unit\":\"ns\",\"samples\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64
           ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64,
           (uint64_t)count,
           p50,
           p95,
           p99,
           maximum);
    printf(",\"windows\":[");
    for (size_t start = 0; start < count; start += PLAYER_VIEW_MOVEMENT_WINDOW_TICKS) {
        size_t samples = MIN((size_t)PLAYER_VIEW_MOVEMENT_WINDOW_TICKS, count - start);
        uint64_t sorted[PLAYER_VIEW_MOVEMENT_WINDOW_TICKS];
        memcpy(sorted, &durations[start], samples * sizeof(*sorted));
        uint64_t window_p95 = player_view_percentile(sorted, samples, 95);
        printf("%s{\"start_tick\":%" PRIu64 ",\"samples\":%" PRIu64 ",\"p95_ns\":%" PRIu64 "}",
               start == 0 ? "" : ",",
               (uint64_t)start,
               (uint64_t)samples,
               window_p95);
    }
    printf("]}");
}

static void player_view_lighting_counters_json(const lighting_benchmark_counters_t *counters) {
    printf("{\"field_begins\":%" PRIu64 ",\"field_dirty_marks\":%" PRIu64
           ",\"field_dirty_pixels\":%" PRIu64 ",\"field_rebuilds\":%" PRIu64
           ",\"field_reuses\":%" PRIu64 ",\"render_calls\":%" PRIu64 ",\"render_failures\":%" PRIu64
           ",\"lit_sprite_draws\":%" PRIu64 ",\"lit_sprite_lookups\":%" PRIu64
           ",\"lit_sprite_hits\":%" PRIu64 ",\"lit_sprite_misses\":%" PRIu64
           ",\"lit_sprite_insertions\":%" PRIu64 ",\"lit_sprite_evictions\":%" PRIu64
           ",\"lit_sprite_fallbacks\":%" PRIu64 ",\"lit_sprite_clears\":%" PRIu64
           ",\"lit_sprite_cleared_entries\":%" PRIu64 "}",
           counters->field_begins,
           counters->field_dirty_marks,
           counters->field_dirty_pixels,
           counters->field_rebuilds,
           counters->field_reuses,
           counters->render_calls,
           counters->render_failures,
           counters->lit_sprite_draws,
           counters->lit_sprite_lookups,
           counters->lit_sprite_hits,
           counters->lit_sprite_misses,
           counters->lit_sprite_insertions,
           counters->lit_sprite_evictions,
           counters->lit_sprite_fallbacks,
           counters->lit_sprite_clears,
           counters->lit_sprite_cleared_entries);
}

static void player_view_lighting_state_json(const lighting_benchmark_statistics_t *statistics,
                                            bool start) {
    printf(
        "{\"allocated_levels\":%" PRIu64 ",\"active_levels\":%" PRIu64
        ",\"cache_valid_levels\":%" PRIu64 ",\"dirty_levels\":%" PRIu64
        ",\"lit_sprite_entries\":%" PRIu64 ",\"lit_sprite_bytes\":%" PRIu64
        ",\"retained_field_bytes\":%" PRIu64 ",\"state_digest\":\"%016" PRIx64 "\"}",
        (uint64_t)(start ? statistics->start_allocated_levels : statistics->allocated_levels),
        (uint64_t)(start ? statistics->start_active_levels : statistics->active_levels),
        (uint64_t)(start ? statistics->start_cache_valid_levels : statistics->cache_valid_levels),
        (uint64_t)(start ? statistics->start_dirty_levels : statistics->dirty_levels),
        (uint64_t)(start ? statistics->start_lit_sprite_entries : statistics->lit_sprite_entries),
        (uint64_t)(start ? statistics->start_lit_sprite_bytes : statistics->lit_sprite_bytes),
        (uint64_t)(start ? statistics->start_retained_field_bytes
                         : statistics->retained_field_bytes),
        start ? statistics->start_state_digest : statistics->state_digest);
}

static void player_view_lighting_level_json(const lighting_benchmark_level_statistics_t *start,
                                            const lighting_benchmark_level_statistics_t *end) {
    printf("{\"depth\":%d,\"start\":{\"allocated\":%s,\"cache_valid\":%s"
           ",\"dirty\":%s,\"entries\":%" PRIu64 ",\"bytes\":%" PRIu64
           ",\"retained_field_bytes\":%" PRIu64 ",\"state_digest\":\"%016" PRIx64
           "\"},\"end\":{\"allocated\":%s"
           ",\"cache_valid\":%s,\"dirty\":%s,\"entries\":%" PRIu64 ",\"bytes\":%" PRIu64
           ",\"retained_field_bytes\":%" PRIu64 ",\"state_digest\":\"%016" PRIx64
           "\"},\"peak\":{\"entries\":%" PRIu64 ",\"bytes\":%" PRIu64
           ",\"retained_field_bytes\":%" PRIu64 "},\"counters\":",
           end->depth,
           start->allocated ? "true" : "false",
           start->cache_valid ? "true" : "false",
           start->update_needed ? "true" : "false",
           (uint64_t)start->lit_sprite_entries,
           (uint64_t)start->lit_sprite_bytes,
           (uint64_t)start->retained_field_bytes,
           start->state_digest,
           end->allocated ? "true" : "false",
           end->cache_valid ? "true" : "false",
           end->update_needed ? "true" : "false",
           (uint64_t)end->lit_sprite_entries,
           (uint64_t)end->lit_sprite_bytes,
           (uint64_t)end->retained_field_bytes,
           end->state_digest,
           (uint64_t)end->peak_lit_sprite_entries,
           (uint64_t)end->peak_lit_sprite_bytes,
           (uint64_t)end->peak_retained_field_bytes);
    player_view_lighting_counters_json(&end->counters);
    printf("}");
}

static void player_view_map_json(const map_benchmark_statistics_t *statistics) {
    printf("{\"map_draws\":%" PRIu64 ",\"primary_map_draws\":%" PRIu64
           ",\"auxiliary_map_draws\":%" PRIu64 ",\"presents\":%" PRIu64
           ",\"present_failures\":%" PRIu64 ",\"render_failures\":%" PRIu64
           ",\"fault_injections\":%" PRIu64 ",\"fault_detections\":%" PRIu64
           ",\"level_draws\":%" PRIu64 ",\"render_commands\":%" PRIu64 ",\"annotations\":%" PRIu64
           ",\"ui_tiles\":%" PRIu64 ",\"peak_render_commands\":%" PRIu64
           ",\"peak_active_levels\":%" PRIu64 ",\"renderer_allocation_statistics_available\":%s"
           ",\"renderer_allocations\":%" PRIu64 ",\"renderer_allocation_bytes\":%" PRIu64 "}",
           statistics->map_draws,
           statistics->primary_map_draws,
           statistics->auxiliary_map_draws,
           statistics->presents,
           statistics->present_failures,
           statistics->render_failures,
           statistics->fault_injections,
           statistics->fault_detections,
           statistics->level_draws,
           statistics->render_commands,
           statistics->annotations,
           statistics->ui_tiles,
           statistics->peak_render_commands,
           statistics->peak_active_levels,
           statistics->renderer_allocation_statistics_available ? "true" : "false",
           statistics->renderer_allocations,
           statistics->renderer_allocation_bytes);
}

static void player_view_render_stages_json(const render_profile_snapshot_t *statistics) {
    static const struct {
        const char *name;
        render_profile_stage_t stage;
    } stages[] = {
        {"map", RENDER_PROFILE_MAP},
        {"map_scratch_clear", RENDER_PROFILE_MAP_SCRATCH_CLEAR},
        {"ground", RENDER_PROFILE_MAP_GROUND},
        {"ground_composite", RENDER_PROFILE_MAP_GROUND_COMPOSITE},
        {"lighting", RENDER_PROFILE_LIGHTING},
        {"objects", RENDER_PROFILE_MAP_OBJECTS},
        {"paint", RENDER_PROFILE_MAP_PAINT},
        {"command_sort", RENDER_PROFILE_MAP_COMMAND_SORT},
        {"door_occlusion", RENDER_PROFILE_MAP_DOOR_OCCLUSION},
        {"sprite_effects", RENDER_PROFILE_MAP_SPRITE_EFFECTS},
        {"hint_replay", RENDER_PROFILE_MAP_HINT_REPLAY},
        {"ui", RENDER_PROFILE_MAP_UI},
    };
    printf("{");
    for (size_t i = 0; i < arraysize(stages); i++) {
        render_profile_stage_t stage = stages[i].stage;
        render_profile_stage_metadata_t metadata;
        if (!render_profiler_stage_metadata_get(stage, &metadata)) {
            metadata.scope = RENDER_PROFILE_SCOPE_FRAME;
        }
        printf("%s\"%s\":{\"unit\":\"us\",\"elapsed\":%" PRIu64 ",\"calls\":%u,\"scope\":\"%s\"}",
               i == 0 ? "" : ",",
               stages[i].name,
               statistics->elapsed_us[stage],
               statistics->calls[stage],
               render_profiler_scope_name(metadata.scope));
    }
    printf("}");
}

static void player_view_sprite_cache_json(const sprite_cache_statistics_t *start,
                                          const sprite_cache_statistics_t *end) {
    printf("{\"available\":%s,\"limits\":{\"entries\":%u,\"estimated_bytes\":%u},\"counters\":{"
           "\"lookups\":%" PRIu64 ",\"hits\":%" PRIu64 ",\"misses\":%" PRIu64
           ",\"insertions\":%" PRIu64 ",\"evictions\":%" PRIu64 ",\"gc_runs\":%" PRIu64
           ",\"gc_removals\":%" PRIu64 ",\"gc_time_ns\":%" PRIu64
           "},\"start\":{\"entries\":%" PRIu64 ",\"estimated_bytes\":%" PRIu64 "}"
           ",\"end\":{\"entries\":%" PRIu64 ",\"estimated_bytes\":%" PRIu64 "}"
           ",\"peak\":{\"entries\":%" PRIu64 ",\"estimated_bytes\":%" PRIu64 "}}",
           SPRITE_CACHE_STATISTICS_VERSION > 0 ? "true" : "false",
           SPRITE_CACHE_MAX_ENTRIES,
           SPRITE_CACHE_MAX_BYTES,
           end->lookups,
           end->hits,
           end->misses,
           end->insertions,
           end->evictions,
           end->gc_runs,
           end->gc_removals,
           end->gc_time_ns,
           (uint64_t)start->entries,
           (uint64_t)start->estimated_bytes,
           (uint64_t)end->entries,
           (uint64_t)end->estimated_bytes,
           (uint64_t)end->peak_entries,
           (uint64_t)end->peak_estimated_bytes);
}

static bool player_view_movement_fixture_parse(const uint8_t *data,
                                               size_t size,
                                               int wire_width,
                                               int wire_height,
                                               player_view_movement_fixture_t *fixture) {
    static const uint8_t magic[] = {'P', 'V', 'M', '1'};
    size_t cursor = sizeof(magic);
    if (data == NULL || fixture == NULL || size < cursor + 1 ||
        memcmp(data, magic, sizeof(magic)) != 0 || data[cursor++] != PLAYER_VIEW_MOVEMENT_PACKETS) {
        return false;
    }

    memset(fixture, 0, sizeof(*fixture));
    for (size_t i = 0; i < arraysize(fixture->packets); i++) {
        if (size - cursor < sizeof(uint32_t)) {
            return false;
        }
        uint32_t packet_size = (uint32_t)data[cursor] << 24 | (uint32_t)data[cursor + 1] << 16 |
                               (uint32_t)data[cursor + 2] << 8 | data[cursor + 3];
        cursor += sizeof(uint32_t);
        if (packet_size == 0 || packet_size > size - cursor || packet_size > PACKET_PAYLOAD_MAX ||
            data[cursor] != MAP_UPDATE_CMD_SAME ||
            !map_protocol_validate(&data[cursor], packet_size, 0, wire_width, wire_height)) {
            return false;
        }
        fixture->packets[i].data = &data[cursor];
        fixture->packets[i].size = packet_size;
        cursor += packet_size;
    }
    return cursor == size;
}

static bool player_view_movement_fixture_closed(const player_view_movement_fixture_t *fixture,
                                                int origin_x,
                                                int origin_y) {
    static const int8_t offsets[PLAYER_VIEW_MOVEMENT_PACKETS][2] = {
        {1, 0},
        {0, 0},
        {0, 1},
        {0, 0},
        {0, 0},
    };
    for (size_t i = 0; i < arraysize(fixture->packets); i++) {
        const uint8_t *packet = fixture->packets[i].data;
        if ((int)packet[1] != origin_x + offsets[i][0] ||
            (int)packet[2] != origin_y + offsets[i][1]) {
            return false;
        }
    }
    return true;
}

static uint64_t player_view_movement_queue_clock(void *user_data) {
    player_view_movement_clock_t *clock = user_data;
    /* Drain reads once at its boundary and then in start/finish pairs. Charge
     * only a completed dispatch, never clock observation itself. */
    if (clock->reads != 0 && clock->reads % 2 == 0) {
        clock->now_us += PLAYER_VIEW_MOVEMENT_SIMULATED_COMMAND_US;
    }
    clock->reads++;
    return clock->now_us;
}

static bool player_view_movement_queue_enqueue(player_view_movement_packet_t packet,
                                               uint64_t arrival_us) {
    if (packet.data == NULL || packet.size == 0 || packet.size >= SIZE_MAX) {
        return false;
    }
    uint8_t *envelope = xmalloc(packet.size + 1);
    envelope[0] = CLIENT_CMD_MAP;
    memcpy(&envelope[1], packet.data, packet.size);
    bool success = client_command_queue_enqueue_envelope_at(envelope, packet.size + 1, arrival_us);
    free(envelope);
    return success;
}

static bool player_view_movement_checkpoint_capture(player_view_movement_replay_t *replay,
                                                    SDL_Surface *surface,
                                                    const player_view_manifest_t *manifest,
                                                    const char *prefix,
                                                    const char *name) {
    if (replay->checkpoints_num == arraysize(replay->checkpoints)) {
        return false;
    }
    player_view_movement_checkpoint_t *checkpoint = &replay->checkpoints[replay->checkpoints_num++];
    lighting_benchmark_statistics_t lighting;
    sprite_cache_statistics_t sprite;
    lighting_benchmark_statistics_get(&lighting);
    sprite_cache_statistics_get(&sprite);
    *checkpoint = (player_view_movement_checkpoint_t){
        .name = name,
        .map_x = MapData.posx,
        .map_y = MapData.posy,
        .viewport_width = surface->w,
        .viewport_height = surface->h,
    };
    if (!player_view_surface_sha256(surface, checkpoint->pixels_digest)) {
        return false;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = player_view_state_hash_uint64(hash, (uint32_t)MapData.posx);
    hash = player_view_state_hash_uint64(hash, (uint32_t)MapData.posy);
    hash = player_view_state_hash_uint64(hash, (uint32_t)MapData.xlen);
    hash = player_view_state_hash_uint64(hash, (uint32_t)MapData.ylen);
    hash = player_view_state_hash_uint64(hash, lighting.state_digest);
    hash = player_view_state_hash_uint64(hash, sprite.entries);
    hash = player_view_state_hash_uint64(hash, sprite.estimated_bytes);
    hash = player_view_state_hash_uint64(hash, (uint32_t)surface->w);
    hash = player_view_state_hash_uint64(hash, (uint32_t)surface->h);
    for (const unsigned char *cursor = (const unsigned char *)MapData.name; *cursor != '\0';
         cursor++) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    for (const unsigned char *cursor = (const unsigned char *)MapData.map_path; *cursor != '\0';
         cursor++) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    for (const unsigned char *cursor = (const unsigned char *)checkpoint->pixels_digest;
         *cursor != '\0';
         cursor++) {
        hash ^= *cursor;
        hash *= UINT64_C(1099511628211);
    }
    checkpoint->state_digest = hash;
    const char *directory = getenv("ATRINIK_MOVEMENT_CHECKPOINT_DIR");
    if (directory != NULL && *directory != '\0') {
        size_t path_size = strlen(directory) + strlen(prefix) + strlen(name) + sizeof("//-.png");
        if (path_size > HUGE_BUF * 4U) {
            fprintf(stderr, "player-view: checkpoint output path exceeds the limit\n");
            return false;
        }
        char *path = xmalloc(path_size);
        snprintf(path, path_size, "%s/%s-%s.png", directory, prefix, name);
        bool written = player_view_output_write(surface, manifest, path, checkpoint->pixels_digest);
        free(path);
        if (!written) {
            return false;
        }
    }
    return true;
}

static const char *player_view_movement_phase_checkpoint(player_view_movement_stream_t stream,
                                                         uint32_t tick,
                                                         uint32_t ticks) {
    if (stream == PLAYER_VIEW_MOVEMENT_COLD && tick == 0) {
        return "cold";
    }
    if (stream == PLAYER_VIEW_MOVEMENT_SUSTAINED) {
        static const char *const first_cycle[] = {
            "step_b",
            "restored_a_1",
            "step_c",
            "restored_a_2",
        };
        if (tick < arraysize(first_cycle)) {
            return first_cycle[tick];
        }
        if (tick + 1 == ticks) {
            return "sustained_end";
        }
    } else if (stream == PLAYER_VIEW_MOVEMENT_IDLE && tick + 1 == ticks) {
        return "idle_end";
    } else if (stream == PLAYER_VIEW_MOVEMENT_RESUMED && tick + 1 == ticks) {
        return "resumed_end";
    }
    return NULL;
}

static void player_view_movement_draw_reasons_record(player_view_movement_phase_t *phase,
                                                     player_view_movement_stream_t stream,
                                                     uint32_t reasons,
                                                     bool map_packet) {
    if (map_packet && (reasons & MAP_REDRAW_REASON_MAP_PACKET) != 0) {
        if (stream == PLAYER_VIEW_MOVEMENT_COLD) {
            phase->full_draw_reasons.reset_packet++;
        } else if (stream == PLAYER_VIEW_MOVEMENT_SUSTAINED ||
                   stream == PLAYER_VIEW_MOVEMENT_RESUMED) {
            phase->full_draw_reasons.changed_map_packet++;
        } else if (stream == PLAYER_VIEW_MOVEMENT_IDLE) {
            phase->full_draw_reasons.noop_map_packet++;
        }
    }
    if ((reasons & MAP_REDRAW_REASON_ANIMATION) != 0) {
        phase->full_draw_reasons.animation_only_tick++;
    }
}

static bool player_view_movement_draw(player_view_movement_replay_t *replay,
                                      player_view_movement_phase_t *phase,
                                      SDL_Surface *surface,
                                      SDL_Surface *local_minimap_surface,
                                      const player_view_movement_fixture_t *fixture,
                                      const player_view_manifest_t *manifest,
                                      const char *checkpoint_prefix,
                                      player_view_movement_packet_t reset_packet,
                                      player_view_movement_stream_t stream,
                                      size_t *active_packet,
                                      uint64_t *next_local_minimap_us,
                                      uint64_t *tick_us) {
    if (!client_command_queue_statistics_reset()) {
        fprintf(stderr, "player-view: movement phase began with queued commands\n");
        return false;
    }
    lighting_benchmark_statistics_get(&phase->lighting_before);
    lighting_benchmark_statistics_reset();
    if (!player_view_lighting_levels_get(phase->lighting_levels_start)) {
        fprintf(stderr, "player-view: cannot read starting per-depth lighting telemetry\n");
        return false;
    }
    map_benchmark_statistics_reset();
    render_profiler_statistics_reset();
    sprite_cache_statistics_get(&phase->sprite_cache_start);
    sprite_cache_statistics_reset();
    for (uint32_t tick = 0; tick < phase->ticks; tick++) {
        sprite_cache_clock_override_set((time_t)(*tick_us / UINT64_C(1000000)));
        size_t produced = 1;
        if (stream == PLAYER_VIEW_MOVEMENT_RESUMED && tick < 8) {
            produced = 2;
        } else if (stream == PLAYER_VIEW_MOVEMENT_RESUMED && tick < 16) {
            produced = 0;
        } else if (stream == PLAYER_VIEW_MOVEMENT_IDLE && tick % 2 != 0) {
            produced = 0;
        }
        for (size_t i = 0; i < produced; i++) {
            bool movement =
                stream == PLAYER_VIEW_MOVEMENT_SUSTAINED || stream == PLAYER_VIEW_MOVEMENT_RESUMED;
            player_view_movement_packet_t packet =
                fixture->packets[PLAYER_VIEW_MOVEMENT_ACTIVE_PACKETS];
            if (stream == PLAYER_VIEW_MOVEMENT_COLD) {
                packet = reset_packet;
            } else if (movement) {
                packet = fixture->packets[*active_packet];
                *active_packet = (*active_packet + 1) % PLAYER_VIEW_MOVEMENT_ACTIVE_PACKETS;
            }
            if (!player_view_movement_queue_enqueue(packet, *tick_us)) {
                fprintf(stderr, "player-view: cannot enqueue movement command envelope\n");
                return false;
            }
            phase->map_packets++;
            if (movement || stream == PLAYER_VIEW_MOVEMENT_COLD) {
                phase->changed_packets++;
            } else {
                phase->noop_packets++;
            }
        }
        LastTick = (uint32_t)(*tick_us / 1000);
        uint64_t frame_started = SDL_GetTicksNS();
        player_view_movement_clock_t queue_clock = {.now_us = *tick_us};
        client_command_queue_drain_result_t drain;
        uint64_t queue_started = SDL_GetTicksNS();
        client_commands_drain_with_clock(CLIENT_COMMAND_QUEUE_BUDGET_US,
                                         player_view_movement_queue_clock,
                                         &queue_clock,
                                         &drain);
        phase->queue_durations[tick] = SDL_GetTicksNS() - queue_started;
        map_animate();
        phase->animation_ticks++;
        bool drawn = map_redraw_due();
        if (drawn) {
            uint32_t reasons = map_redraw_pending_reasons();
            if (!SDL_FillSurfaceRect(surface, NULL, 0)) {
                fprintf(stderr,
                        "player-view: cannot clear movement benchmark surface: %s\n",
                        SDL_GetError());
                return false;
            }
            uint64_t map_started = SDL_GetTicksNS();
            map_draw_map(surface);
            phase->map_durations[phase->map_samples++] = SDL_GetTicksNS() - map_started;
            phase->full_map_draws++;
            if (*tick_us >= *next_local_minimap_us) {
                if (!SDL_FillSurfaceRect(local_minimap_surface, NULL, 0)) {
                    fprintf(stderr,
                            "player-view: cannot clear local minimap benchmark surface: %s\n",
                            SDL_GetError());
                    return false;
                }
                uint64_t minimap_started = SDL_GetTicksNS();
                map_draw_map(local_minimap_surface);
                phase->local_minimap_durations[phase->local_minimap_samples++] =
                    SDL_GetTicksNS() - minimap_started;
                phase->local_minimap_draws++;
                do {
                    *next_local_minimap_us += (uint64_t)MINIMAP_DYNAMIC_REDRAW_INTERVAL * 1000U;
                } while (*next_local_minimap_us <= *tick_us);
            }
            player_view_movement_draw_reasons_record(phase, stream, reasons, drain.commands != 0);
            map_redraw_consume();
            effect_sprites_play();
#ifdef ATRINIK_WIDGET_TESTS
            map_benchmark_fault_status_t fault_status;
            map_benchmark_fault_status_get(&fault_status);
            if (fault_status.detected) {
                return false;
            }
#endif
        }
        sprite_cache_gc();
        render_profiler_frame_finished(drawn);
        uint64_t elapsed = SDL_GetTicksNS() - frame_started;
        uint64_t target = (uint64_t)PLAYER_VIEW_MOVEMENT_TICK_MS * UINT64_C(1000000);
        phase->frame_durations[tick] = elapsed;
        phase->wait_durations[tick] = elapsed < target ? target - elapsed : 0;
        phase->loop_durations[tick] = MAX(elapsed, target);
        const char *checkpoint = player_view_movement_phase_checkpoint(stream, tick, phase->ticks);
        if (checkpoint != NULL && !player_view_movement_checkpoint_capture(replay,
                                                                           surface,
                                                                           manifest,
                                                                           checkpoint_prefix,
                                                                           checkpoint)) {
            fprintf(stderr, "player-view: cannot capture movement checkpoint\n");
            return false;
        }
        *tick_us += (uint64_t)PLAYER_VIEW_MOVEMENT_TICK_MS * 1000;
    }
    client_command_queue_statistics_get(*tick_us, &phase->queue);
    if (phase->queue.due || phase->queue.depth != 0) {
        fprintf(stderr, "player-view: movement replay phase left queued packets\n");
        return false;
    }
    lighting_benchmark_statistics_get(&phase->lighting);
    phase->lighting.field_rebuilds = phase->lighting.counters.field_rebuilds;
    phase->lighting.field_reuses = phase->lighting.counters.field_reuses;
    phase->lighting.lit_sprite_lookups = phase->lighting.counters.lit_sprite_lookups;
    phase->lighting.lit_sprite_hits = phase->lighting.counters.lit_sprite_hits;
    phase->lighting.lit_sprite_misses = phase->lighting.counters.lit_sprite_misses;
    phase->lighting.lit_sprite_evictions = phase->lighting.counters.lit_sprite_evictions;
    phase->lighting.start_allocated_levels = phase->lighting_before.allocated_levels;
    phase->lighting.start_active_levels = phase->lighting_before.active_levels;
    phase->lighting.start_cache_valid_levels = phase->lighting_before.cache_valid_levels;
    phase->lighting.start_dirty_levels = phase->lighting_before.dirty_levels;
    phase->lighting.start_lit_sprite_entries = phase->lighting_before.lit_sprite_entries;
    phase->lighting.start_lit_sprite_bytes = phase->lighting_before.lit_sprite_bytes;
    phase->lighting.start_retained_field_bytes = phase->lighting_before.retained_field_bytes;
    phase->lighting.start_state_digest = phase->lighting_before.state_digest;
    if (!player_view_lighting_levels_get(phase->lighting_levels_end)) {
        fprintf(stderr, "player-view: cannot read ending per-depth lighting telemetry\n");
        return false;
    }
    map_benchmark_statistics_get(&phase->map);
    render_profiler_statistics_get(&phase->render);
    sprite_cache_statistics_get(&phase->sprite_cache_end);
    return true;
}

static void player_view_movement_replay_initialize(player_view_movement_replay_t *replay) {
    memset(replay, 0, sizeof(*replay));
    replay->phases[0] = (player_view_movement_phase_t){.name = "cold", .ticks = 1};
    replay->phases[1] = (player_view_movement_phase_t){
        .name = "sustained",
        .ticks = PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS,
    };
    replay->phases[2] = (player_view_movement_phase_t){
        .name = "idle",
        .ticks = PLAYER_VIEW_MOVEMENT_IDLE_TICKS,
    };
    replay->phases[3] = (player_view_movement_phase_t){
        .name = "resumed",
        .ticks = PLAYER_VIEW_MOVEMENT_RESUMED_TICKS,
    };
}

static bool player_view_movement_lifecycle_draw(player_view_movement_replay_t *replay,
                                                SDL_Surface *surface,
                                                const player_view_manifest_t *manifest,
                                                const char *checkpoint_prefix,
                                                const char *checkpoint,
                                                map_redraw_reason_t reason) {
    map_benchmark_statistics_t before, after;
    map_benchmark_statistics_get(&before);
    map_redraw_request(reason);
    if (!SDL_FillSurfaceRect(surface, NULL, 0)) {
        return false;
    }
    map_draw_map(surface);
    map_redraw_consume();
    effect_sprites_play();
    sprite_cache_gc();
    render_profiler_frame_finished(true);
    map_benchmark_statistics_get(&after);
    if (after.render_failures != before.render_failures) {
        fprintf(stderr, "player-view: lifecycle checkpoint renderer failed\n");
        return false;
    }
    replay->lifecycle.full_map_draws++;
    if (reason == MAP_REDRAW_REASON_RESIZE) {
        replay->lifecycle.resize++;
    } else if (strcmp(checkpoint, "reset") == 0) {
        replay->lifecycle.reset_packet++;
    } else if (strcmp(checkpoint, "transition") == 0) {
        replay->lifecycle.map_transition++;
    }
    return player_view_movement_checkpoint_capture(replay,
                                                   surface,
                                                   manifest,
                                                   checkpoint_prefix,
                                                   checkpoint);
}

static bool player_view_movement_lifecycle_packet(player_view_movement_packet_t packet,
                                                  uint64_t tick_us) {
    if (!client_command_queue_statistics_reset() ||
        !player_view_movement_queue_enqueue(packet, tick_us)) {
        return false;
    }
    player_view_movement_clock_t queue_clock = {.now_us = tick_us};
    client_command_queue_drain_result_t drain;
    client_commands_drain_with_clock(CLIENT_COMMAND_QUEUE_BUDGET_US,
                                     player_view_movement_queue_clock,
                                     &queue_clock,
                                     &drain);
    client_command_queue_statistics_t statistics;
    client_command_queue_statistics_get(queue_clock.now_us, &statistics);
    return drain.commands == 1 && !statistics.due && statistics.depth == 0 &&
           statistics.order_digests_comparable &&
           statistics.enqueued_order_digest == statistics.dequeued_order_digest;
}

static bool player_view_movement_lifecycle(player_view_movement_replay_t *replay,
                                           SDL_Surface *surface,
                                           const player_view_manifest_t *manifest,
                                           const char *checkpoint_prefix,
                                           player_view_movement_packet_t reset_packet,
                                           player_view_movement_packet_t transition_packet,
                                           uint64_t tick_us) {
    int original_width = surface->w;
    int original_height = surface->h;
    SDL_Surface *resized = SDL_CreateSurface(original_width + (int)manifest->resize_width_delta,
                                             original_height + (int)manifest->resize_height_delta,
                                             surface->format);
    if (resized == NULL) {
        return false;
    }
    widgetdata *map_widget = cur_widget[MAP_ID];
    map_widget->surface = resized;
    map_widget->w = resized->w;
    map_widget->h = resized->h;
    ScreenSurface = resized;
    bool success = player_view_movement_lifecycle_draw(replay,
                                                       resized,
                                                       manifest,
                                                       checkpoint_prefix,
                                                       "resized",
                                                       MAP_REDRAW_REASON_RESIZE);
    map_widget->surface = surface;
    map_widget->w = original_width;
    map_widget->h = original_height;
    ScreenSurface = surface;
    if (success) {
        success = player_view_movement_lifecycle_draw(replay,
                                                      surface,
                                                      manifest,
                                                      checkpoint_prefix,
                                                      "resize_restored",
                                                      MAP_REDRAW_REASON_RESIZE);
    }
    SDL_DestroySurface(resized);
    if (!success || !player_view_movement_lifecycle_packet(reset_packet, tick_us) ||
        !player_view_movement_lifecycle_draw(replay,
                                             surface,
                                             manifest,
                                             checkpoint_prefix,
                                             "reset",
                                             MAP_REDRAW_REASON_MAP_PACKET) ||
        !player_view_movement_lifecycle_packet(transition_packet,
                                               tick_us + PLAYER_VIEW_MOVEMENT_TICK_MS * 1000U) ||
        !player_view_movement_lifecycle_draw(replay,
                                             surface,
                                             manifest,
                                             checkpoint_prefix,
                                             "transition",
                                             MAP_REDRAW_REASON_MAP_PACKET)) {
        return false;
    }
    return true;
}

static bool player_view_movement_replay_run(player_view_movement_replay_t *replay,
                                            SDL_Surface *surface,
                                            SDL_Surface *local_minimap_surface,
                                            const player_view_manifest_t *manifest,
                                            const char *checkpoint_prefix,
                                            const player_view_movement_fixture_t *fixture,
                                            player_view_movement_packet_t reset_packet,
                                            player_view_movement_packet_t transition_packet) {
    player_view_movement_replay_initialize(replay);
    uint64_t tick_us = (uint64_t)manifest->clock_ms * 1000U;
    uint64_t next_local_minimap_us = tick_us;
    sprite_cache_clock_override_set((time_t)(tick_us / UINT64_C(1000000)));
    size_t active_packet = 0;
    int origin_x = -1;
    int origin_y = -1;
    for (size_t i = 0; i < arraysize(replay->phases); i++) {
        if (!player_view_movement_draw(replay,
                                       &replay->phases[i],
                                       surface,
                                       local_minimap_surface,
                                       fixture,
                                       manifest,
                                       checkpoint_prefix,
                                       reset_packet,
                                       (player_view_movement_stream_t)i,
                                       &active_packet,
                                       &next_local_minimap_us,
                                       &tick_us)) {
            return false;
        }
        sprite_cache_clock_override_set((time_t)(tick_us / UINT64_C(1000000)));
        if (i == PLAYER_VIEW_MOVEMENT_COLD) {
            origin_x = MapData.posx;
            origin_y = MapData.posy;
            if (!player_view_movement_fixture_closed(fixture, origin_x, origin_y) ||
                image_missing_faces_detected()) {
                fprintf(stderr, "player-view: movement fixture is not closed or self-contained\n");
                return false;
            }
        }
    }
    if (MapData.posx != origin_x || MapData.posy != origin_y || active_packet != 0) {
        fprintf(stderr, "player-view: movement replay did not restore its origin\n");
        return false;
    }
    return player_view_movement_lifecycle(replay,
                                          surface,
                                          manifest,
                                          checkpoint_prefix,
                                          reset_packet,
                                          transition_packet,
                                          tick_us) &&
           replay->checkpoints_num == PLAYER_VIEW_MOVEMENT_CHECKPOINTS;
}

static bool player_view_movement_checkpoints_equal(const player_view_movement_replay_t *first,
                                                   const player_view_movement_replay_t *repeat) {
    if (first->checkpoints_num != repeat->checkpoints_num) {
        return false;
    }
    for (size_t i = 0; i < first->checkpoints_num; i++) {
        const player_view_movement_checkpoint_t *left = &first->checkpoints[i];
        const player_view_movement_checkpoint_t *right = &repeat->checkpoints[i];
        if (strcmp(left->name, right->name) != 0 ||
            strcmp(left->pixels_digest, right->pixels_digest) != 0 ||
            left->state_digest != right->state_digest || left->map_x != right->map_x ||
            left->map_y != right->map_y || left->viewport_width != right->viewport_width ||
            left->viewport_height != right->viewport_height) {
            return false;
        }
    }
    return true;
}

static bool player_view_movement_checkpoint_digest(const player_view_movement_replay_t *replay,
                                                   char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    static const char prefix[] = "pvm-checkpoints-v1\n";
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool success = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
                   EVP_DigestUpdate(context, prefix, sizeof(prefix) - 1) == 1;
    for (size_t i = 0; success && i < replay->checkpoints_num; i++) {
        const player_view_movement_checkpoint_t *checkpoint = &replay->checkpoints[i];
        char row[256];
        int length = snprintf(row,
                              sizeof(row),
                              "%s\t%s\t%d\t%d\t%d\t%d\n",
                              checkpoint->name,
                              checkpoint->pixels_digest,
                              checkpoint->map_x,
                              checkpoint->map_y,
                              checkpoint->viewport_width,
                              checkpoint->viewport_height);
        success = length > 0 && (size_t)length < sizeof(row) &&
                  EVP_DigestUpdate(context, row, (size_t)length) == 1;
    }

    uint8_t raw[EVP_MAX_MD_SIZE];
    unsigned int raw_size = 0;
    if (success && (EVP_DigestFinal_ex(context, raw, &raw_size) != 1 || raw_size != 32)) {
        success = false;
    }
    EVP_MD_CTX_free(context);
    if (!success) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        snprintf(&digest[i * 2], 3, "%02x", raw[i]);
    }
    return true;
}

static void player_view_movement_checkpoint_json(const player_view_movement_checkpoint_t *point) {
    printf("{\"name\":\"%s\",\"pixels_sha256\":\"%s\",\"state_digest\":\"%016" PRIx64
           "\",\"map_x\":%d,\"map_y\":%d,\"viewport_width\":%d,\"viewport_height\":%d}",
           point->name,
           point->pixels_digest,
           point->state_digest,
           point->map_x,
           point->map_y,
           point->viewport_width,
           point->viewport_height);
}

static void player_view_movement_phase_json(const player_view_movement_phase_t *phase,
                                            size_t index) {
    uint64_t work_p50 = 0, work_p95 = 0, work_p99 = 0, work_max = 0;
    player_view_timing_summary(phase->frame_durations,
                               phase->ticks,
                               &work_p50,
                               &work_p95,
                               &work_p99,
                               &work_max);
    printf("%s{\"name\":\"%s\",\"samples\":%u,\"map_packets\":%u,"
           "\"changed_map_packets\":%u,\"noop_map_packets\":%u,"
           "\"full_map_draws\":%u,\"animation_ticks\":%u,\"full_draw_reasons\":{"
           "\"reset_packet\":%u,\"changed_map_packet\":%u,\"noop_map_packet\":%u,"
           "\"animation_only_tick\":%u,\"resize\":%u,\"map_transition\":%u},"
           "\"frame_time\":",
           index == 0 ? "" : ",",
           phase->name,
           phase->ticks,
           phase->map_packets,
           phase->changed_packets,
           phase->noop_packets,
           phase->full_map_draws,
           phase->animation_ticks,
           phase->full_draw_reasons.reset_packet,
           phase->full_draw_reasons.changed_map_packet,
           phase->full_draw_reasons.noop_map_packet,
           phase->full_draw_reasons.animation_only_tick,
           phase->full_draw_reasons.resize,
           phase->full_draw_reasons.map_transition);
    player_view_timing_json(phase->frame_durations, phase->ticks);
    printf(",\"main_loop\":{\"update_cadence_hz\":%.3f,"
           "\"update_interval_ns\":%" PRIu64 ",\"work_time\":",
           1000.0 / PLAYER_VIEW_MOVEMENT_TICK_MS,
           (uint64_t)PLAYER_VIEW_MOVEMENT_TICK_MS * UINT64_C(1000000));
    player_view_timing_json(phase->frame_durations, phase->ticks);
    printf(",\"simulated_wait_time\":");
    player_view_timing_json(phase->wait_durations, phase->ticks);
    printf(",\"simulated_update_loop_time\":");
    player_view_timing_json(phase->loop_durations, phase->ticks);
    printf(",\"work_capacity_fps\":{\"p50\":%.3f,\"p95\":%.3f}},\"map_time\":",
           work_p50 == 0 ? 0.0 : 1000000000.0 / work_p50,
           work_p95 == 0 ? 0.0 : 1000000000.0 / work_p95);
    player_view_timing_json(phase->map_durations, phase->map_samples);
    printf(",\"local_minimap\":{\"enabled\":true,\"update_interval_ms\":%u,"
           "\"surface_width\":%d,\"surface_height\":%d,\"map_draws\":%u,\"map_time\":",
           MINIMAP_DYNAMIC_REDRAW_INTERVAL,
           MINIMAP_DYNAMIC_SURFACE_WIDTH,
           MINIMAP_DYNAMIC_SURFACE_HEIGHT,
           phase->local_minimap_draws);
    player_view_timing_json(phase->local_minimap_durations, phase->local_minimap_samples);
    printf("},\"queue\":{\"enqueued\":%" PRIu64 ",\"dequeued\":%" PRIu64
           ",\"budget_yields\":%" PRIu64 ",\"recoveries\":%" PRIu64
           ",\"start_depth\":0,\"end_depth\":%" PRIu64 ",\"peak_depth\":%" PRIu64
           ",\"start_bytes\":0,\"end_bytes\":%" PRIu64 ",\"peak_bytes\":%" PRIu64
           ",\"oldest_age_us\":%" PRIu64 ",\"current_oldest_age_us\":%" PRIu64
           ",\"processing_us\":%" PRIu64 ",\"due\":%s,\"budget_due\":%s,"
           "\"service_clock\":\"simulated\",\"simulated_command_us\":%" PRIu64 ","
           "\"order_digests_comparable\":%s,"
           "\"enqueued_order_digest\":\"%016" PRIx64 "\",\"dequeued_order_digest\":\"%016" PRIx64
           "\"",
           phase->queue.enqueued,
           phase->queue.dequeued,
           phase->queue.budget_yields,
           phase->queue.recoveries,
           phase->queue.depth,
           phase->queue.peak_depth,
           phase->queue.bytes,
           phase->queue.peak_bytes,
           phase->queue.oldest_age_us,
           phase->queue.current_oldest_age_us,
           phase->queue.processing_us,
           phase->queue.due ? "true" : "false",
           phase->queue.budget_due ? "true" : "false",
           PLAYER_VIEW_MOVEMENT_SIMULATED_COMMAND_US,
           phase->queue.order_digests_comparable ? "true" : "false",
           phase->queue.enqueued_order_digest,
           phase->queue.dequeued_order_digest);
    printf(",\"drain_time\":");
    player_view_timing_json(phase->queue_durations, phase->ticks);
    printf("},\"map\":");
    player_view_map_json(&phase->map);
    printf(",\"render_stages\":");
    player_view_render_stages_json(&phase->render);
    printf(",\"lighting\":{\"available\":%s,\"start\":",
           LIGHTING_BENCHMARK_STATISTICS_VERSION > 0 ? "true" : "false");
    player_view_lighting_state_json(&phase->lighting, true);
    printf(",\"end\":");
    player_view_lighting_state_json(&phase->lighting, false);
    printf(",\"peak\":{\"allocated_levels\":%" PRIu64 ",\"active_levels\":%" PRIu64
           ",\"cache_valid_levels\":%" PRIu64 ",\"dirty_levels\":%" PRIu64
           ",\"lit_sprite_entries\":%" PRIu64 ",\"lit_sprite_bytes\":%" PRIu64
           ",\"retained_field_bytes\":%" PRIu64 "},\"counters\":",
           (uint64_t)phase->lighting.peak_allocated_levels,
           (uint64_t)phase->lighting.peak_active_levels,
           (uint64_t)phase->lighting.peak_cache_valid_levels,
           (uint64_t)phase->lighting.peak_dirty_levels,
           (uint64_t)phase->lighting.peak_lit_sprite_entries,
           (uint64_t)phase->lighting.peak_lit_sprite_bytes,
           (uint64_t)phase->lighting.peak_retained_field_bytes);
    player_view_lighting_counters_json(&phase->lighting.counters);
    printf(",\"levels\":[");
    for (size_t level = 0; level < arraysize(phase->lighting_levels_end); level++) {
        printf("%s", level == 0 ? "" : ",");
        player_view_lighting_level_json(&phase->lighting_levels_start[level],
                                        &phase->lighting_levels_end[level]);
    }
    printf("]},\"sprite_cache\":");
    player_view_sprite_cache_json(&phase->sprite_cache_start, &phase->sprite_cache_end);
    printf("}");
}

static void player_view_cpu_model(char *buffer, size_t size) {
    snprintf(buffer, size, "%s", player_view_environment("ATRINIK_BENCHMARK_CPU_MODEL"));
#ifndef WIN32
    if (strcmp(buffer, "unknown") == 0) {
        FILE *stream = fopen("/proc/cpuinfo", "r");
        char line[512];
        while (stream != NULL && fgets(line, sizeof(line), stream) != NULL) {
            if (strncmp(line, "model name", 10) != 0 && strncmp(line, "Hardware", 8) != 0) {
                continue;
            }
            char *value = strchr(line, ':');
            if (value != NULL) {
                value++;
                while (isspace((unsigned char)*value)) {
                    value++;
                }
                value[strcspn(value, "\r\n")] = '\0';
                snprintf(buffer, size, "%s", *value != '\0' ? value : "unknown");
            }
            break;
        }
        if (stream != NULL) {
            fclose(stream);
        }
    }
#else
    if (strcmp(buffer, "unknown") == 0) {
        snprintf(buffer, size, "%s", player_view_environment("PROCESSOR_IDENTIFIER"));
    }
#endif
}

static bool player_view_movement_benchmark(SDL_Surface *surface,
                                           const player_view_manifest_t *manifest,
                                           const uint8_t *snapshot,
                                           size_t snapshot_size,
                                           const uint8_t *next_snapshot,
                                           size_t next_snapshot_size,
                                           bool large_viewport) {
    bool success = false;
    uint8_t *transition = NULL;
    size_t transition_size = 0;
    bool queue_ready = false;
    SDL_Surface *local_minimap_surface = NULL;
    player_view_movement_fixture_t fixture;
    int wire_width = MAP_LOOK_TO_WIRE_SIZE(manifest->look_width);
    int wire_height = MAP_LOOK_TO_WIRE_SIZE(manifest->look_height);
    if (next_snapshot == NULL || manifest->transition_snapshot_path == NULL ||
        manifest->expected_standard_checkpoint_digest[0] == '\0' ||
        manifest->resize_width_delta == 0 || manifest->resize_height_delta == 0 ||
        snapshot[0] != MAP_UPDATE_CMD_NEW ||
        !player_view_movement_fixture_parse(next_snapshot,
                                            next_snapshot_size,
                                            wire_width,
                                            wire_height,
                                            &fixture) ||
        !player_view_snapshot_load(manifest->transition_snapshot_path,
                                   &transition,
                                   &transition_size) ||
        transition[0] != MAP_UPDATE_CMD_NEW ||
        !map_protocol_validate(transition, transition_size, 0, wire_width, wire_height)) {
        fprintf(stderr, "player-view: invalid or incomplete movement lifecycle fixture\n");
        goto cleanup;
    }
    const char *fault = getenv("ATRINIK_MOVEMENT_FAULT");
    bool fault_requested = fault != NULL && *fault != '\0';
    bool mutable_rle_fault = fault_requested && strcmp(fault, "mutable-rle") == 0;
    bool sprite_cache_clock_fault = fault_requested && strcmp(fault, "sprite-cache-clock") == 0;
    if (fault_requested && !mutable_rle_fault && !sprite_cache_clock_fault) {
        fprintf(stderr, "player-view: unknown movement fault '%s'\n", fault);
        goto cleanup;
    }
#ifdef ATRINIK_WIDGET_TESTS
    map_benchmark_fault_configure(mutable_rle_fault ? MAP_BENCHMARK_FAULT_MUTABLE_RLE
                                                    : MAP_BENCHMARK_FAULT_NONE);
#else
    if (fault_requested) {
        fprintf(stderr, "player-view: movement fault injection is unavailable\n");
        goto cleanup;
    }
#endif
    if (!client_command_queue_initialize()) {
        fprintf(stderr, "player-view: cannot initialize production command queue\n");
        goto cleanup;
    }
    queue_ready = true;
    local_minimap_surface = SDL_CreateSurface(MINIMAP_DYNAMIC_SURFACE_WIDTH,
                                              MINIMAP_DYNAMIC_SURFACE_HEIGHT,
                                              surface->format);
    if (local_minimap_surface == NULL || !SDL_FillSurfaceRect(local_minimap_surface, NULL, 0)) {
        fprintf(stderr,
                "player-view: cannot create local minimap benchmark surface: %s\n",
                SDL_GetError());
        goto cleanup;
    }
    effects_deinit();
    render_profiler_set_enabled(true);
    lighting_benchmark_statistics_reset();
    sprite_cache_statistics_reset();
    rndm_seed(PLAYER_VIEW_MOVEMENT_RNG_SEED);
    player_view_movement_packet_t reset_packet = {.data = snapshot, .size = snapshot_size};
    player_view_movement_packet_t transition_packet = {
        .data = transition,
        .size = transition_size,
    };
#ifdef ATRINIK_WIDGET_TESTS
    if (sprite_cache_clock_fault) {
        player_view_movement_replay_t clock_probe;
        player_view_movement_replay_initialize(&clock_probe);
        uint64_t tick_us = (uint64_t)manifest->clock_ms * 1000U;
        uint64_t next_local_minimap_us = tick_us;
        size_t active_packet = 0;
        if (!player_view_movement_draw(&clock_probe,
                                       &clock_probe.phases[PLAYER_VIEW_MOVEMENT_COLD],
                                       surface,
                                       local_minimap_surface,
                                       &fixture,
                                       manifest,
                                       "clock-probe",
                                       reset_packet,
                                       PLAYER_VIEW_MOVEMENT_COLD,
                                       &active_packet,
                                       &next_local_minimap_us,
                                       &tick_us)) {
            fprintf(stderr, "player-view: movement sprite-cache clock probe failed\n");
            goto cleanup;
        }
        sprite_cache_statistics_t before, after;
        sprite_cache_statistics_get(&before);
        sprite_cache_clock_override_set((time_t)(tick_us / UINT64_C(1000000)) +
                                        SPRITE_CACHE_GC_FREE_TIME + 1);
        sprite_cache_gc_force();
        sprite_cache_statistics_get(&after);
        if (before.entries != 0 && after.entries < before.entries &&
            after.gc_removals > before.gc_removals) {
            fprintf(stderr,
                    "player-view: movement fault sprite-cache-clock was injected and detected\n");
        } else {
            fprintf(stderr, "player-view: movement fault sprite-cache-clock was not detected\n");
        }
        goto cleanup;
    }
#endif
    player_view_movement_replay_t first;
    bool first_success = player_view_movement_replay_run(&first,
                                                         surface,
                                                         local_minimap_surface,
                                                         manifest,
                                                         "first",
                                                         &fixture,
                                                         reset_packet,
                                                         transition_packet);
#ifdef ATRINIK_WIDGET_TESTS
    if (mutable_rle_fault) {
        map_benchmark_fault_status_t status;
        map_benchmark_fault_status_get(&status);
        if (status.injected && status.detected) {
            fprintf(stderr, "player-view: movement fault mutable-rle was injected and detected\n");
        } else {
            fprintf(stderr, "player-view: movement fault mutable-rle was not detected\n");
        }
        goto cleanup;
    }
#endif
    if (!first_success) {
        goto cleanup;
    }
    for (size_t i = 0; i < arraysize(first.phases); i++) {
        if (first.phases[i].map.render_failures != 0) {
            fprintf(stderr, "player-view: movement renderer reported a failure\n");
            goto cleanup;
        }
    }
    sprite_cache_free_all();
    map_runtime_deinit();
    memset(&MapData, 0, sizeof(MapData));
    map_runtime_init();
    map_redraw_consume();
    image_missing_faces_reset();
    if (!SDL_FillSurfaceRect(surface, NULL, 0)) {
        goto cleanup;
    }
    rndm_seed(PLAYER_VIEW_MOVEMENT_RNG_SEED);
    lighting_benchmark_statistics_reset();
    sprite_cache_statistics_reset();
    player_view_movement_replay_t repeat;
    if (!player_view_movement_replay_run(&repeat,
                                         surface,
                                         local_minimap_surface,
                                         manifest,
                                         "repeat",
                                         &fixture,
                                         reset_packet,
                                         transition_packet) ||
        !player_view_movement_checkpoints_equal(&first, &repeat)) {
        fprintf(stderr, "player-view: same-process replay checkpoints diverged\n");
        goto cleanup;
    }
    for (size_t i = 0; i < arraysize(repeat.phases); i++) {
        if (repeat.phases[i].map.render_failures != 0) {
            fprintf(stderr, "player-view: repeated movement renderer reported a failure\n");
            goto cleanup;
        }
    }
    char first_checkpoint_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char repeat_checkpoint_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!player_view_movement_checkpoint_digest(&first, first_checkpoint_digest) ||
        !player_view_movement_checkpoint_digest(&repeat, repeat_checkpoint_digest) ||
        strcmp(first_checkpoint_digest, repeat_checkpoint_digest) != 0) {
        fprintf(stderr, "player-view: cannot verify the ordered movement lifecycle digest\n");
        goto cleanup;
    }
    if (!large_viewport &&
        strcmp(first_checkpoint_digest, manifest->expected_standard_checkpoint_digest) != 0) {
        fprintf(stderr,
                "player-view: standard movement lifecycle digest mismatch: expected %s, got %s\n",
                manifest->expected_standard_checkpoint_digest,
                first_checkpoint_digest);
        goto cleanup;
    }
    if (!player_view_inputs_verify(manifest)) {
        fprintf(stderr, "player-view: frozen inputs changed during movement replay\n");
        goto cleanup;
    }
    const char *runner_os = player_view_environment("RUNNER_OS");
    const char *runner_arch = player_view_environment("RUNNER_ARCH");
    const char *runner_image_os = player_view_environment("ImageOS");
    const char *runner_image_version = player_view_environment("ImageVersion");
    const char *ci = player_view_environment("CI");
    char cpu_model[512];
    player_view_cpu_model(cpu_model, sizeof(cpu_model));
    const char *identity_values[] = {
        ATRINIK_BENCHMARK_REVISION,
        ATRINIK_BUILD_TYPE,
        ATRINIK_COMPILER_ID,
        ATRINIK_COMPILER_VERSION,
        ATRINIK_SYSTEM_NAME,
        SDL_GetPlatform(),
        runner_os,
        runner_arch,
        runner_image_os,
        runner_image_version,
        ci,
        cpu_model,
    };
    for (size_t i = 0; i < arraysize(identity_values); i++) {
        if (!player_view_json_string(identity_values[i])) {
            fprintf(stderr, "player-view: benchmark identity contains unsafe text\n");
            goto cleanup;
        }
    }
    const char *viewport_name = large_viewport ? "large" : "standard";
    bool dirty_known = strcmp(ATRINIK_BENCHMARK_DIRTY, "unknown") != 0;
    const char *dirty =
        !dirty_known ? "null" : (strcmp(ATRINIK_BENCHMARK_DIRTY, "true") == 0 ? "true" : "false");
    int sdl_version = SDL_GetVersion();
    int cpu_count = MAX(1, SDL_GetNumLogicalCPUCores());
    uint64_t peak_rss = player_view_process_peak_rss_bytes();
    const player_view_movement_checkpoint_t *final = &first.checkpoints[first.checkpoints_num - 1];
    const player_view_movement_checkpoint_t *repeat_final =
        &repeat.checkpoints[repeat.checkpoints_num - 1];
    printf("{\"schema_version\":%u,\"benchmark\":\"player-view-movement\","
           "\"tick_ms\":%u,\"simulated_tick_hz\":%u,\"identity\":{"
           "\"instrumentation\":{\"schema_version\":%u,\"fixture_schema_version\":%u,"
           "\"workload\":\"pvm1-map2-lifecycle-v3\",\"lighting_statistics_version\":%u,"
           "\"map_statistics_version\":%u,\"render_profiler_statistics_version\":%u,"
           "\"sprite_cache_statistics_version\":%u},\"implementation\":{"
           "\"revision\":\"%s\",\"dirty\":%s,\"dirty_known\":%s,"
           "\"build_type\":\"%s\",\"compiler_id\":\"%s\","
           "\"compiler_version\":\"%s\",\"cmake_system\":\"%s\","
           "\"sdl_version\":\"%d.%d.%d\",\"sdl_platform\":\"%s\"},"
           "\"run\":{\"runner_os\":\"%s\",\"runner_arch\":\"%s\","
           "\"runner_image_os\":\"%s\",\"runner_image_version\":\"%s\","
           "\"ci\":\"%s\",\"cpu_count\":%d,\"cpu_model\":\"%s\",\"viewport\":{"
           "\"name\":\"%s\",\"width\":%d,\"height\":%d},\"mode\":\"%s\"}},"
           "\"fixture\":{\"manifest_schema_version\":%u,\"manifest_sha256\":\"%s\","
           "\"snapshot_sha256\":\"%s\",\"movement_stream_sha256\":\"%s\","
           "\"transition_snapshot_sha256\":\"%s\","
           "\"expected_standard_checkpoint_sha256\":\"%s\",\"resize_width_delta\":%u,"
           "\"resize_height_delta\":%u,\"rng_seed\":%" PRIu64 ",\"look_width\":%u,"
           "\"look_height\":%u,\"smooth_lighting\":%s},"
           "\"checkpoint_sha256\":\"%s\",\"same_process_checkpoint_sha256\":\"%s\","
           "\"final_state_digest\":\"%016" PRIx64 "\","
           "\"same_process_final_state_digest\":\"%016" PRIx64 "\","
           "\"process_peak_rss_bytes\":%" PRIu64 ",\"process_peak_rss_available\":%s,"
           "\"checkpoints\":[",
           PLAYER_VIEW_MOVEMENT_SCHEMA_VERSION,
           PLAYER_VIEW_MOVEMENT_TICK_MS,
           1000U / PLAYER_VIEW_MOVEMENT_TICK_MS,
           PLAYER_VIEW_MOVEMENT_SCHEMA_VERSION,
           PLAYER_VIEW_MOVEMENT_FIXTURE_SCHEMA,
           LIGHTING_BENCHMARK_STATISTICS_VERSION,
           MAP_BENCHMARK_STATISTICS_VERSION,
           RENDER_PROFILER_STATISTICS_VERSION,
           SPRITE_CACHE_STATISTICS_VERSION,
           ATRINIK_BENCHMARK_REVISION,
           dirty,
           dirty_known ? "true" : "false",
           ATRINIK_BUILD_TYPE,
           ATRINIK_COMPILER_ID,
           ATRINIK_COMPILER_VERSION,
           ATRINIK_SYSTEM_NAME,
           SDL_VERSIONNUM_MAJOR(sdl_version),
           SDL_VERSIONNUM_MINOR(sdl_version),
           SDL_VERSIONNUM_MICRO(sdl_version),
           SDL_GetPlatform(),
           runner_os,
           runner_arch,
           runner_image_os,
           runner_image_version,
           ci,
           cpu_count,
           cpu_model,
           viewport_name,
           surface->w,
           surface->h,
           manifest->smooth_lighting ? "smooth" : "discrete",
           PLAYER_VIEW_SCHEMA_VERSION,
           manifest->manifest_digest,
           manifest->snapshot_digest,
           manifest->next_snapshot_digest,
           manifest->transition_snapshot_digest,
           manifest->expected_standard_checkpoint_digest,
           manifest->resize_width_delta,
           manifest->resize_height_delta,
           PLAYER_VIEW_MOVEMENT_RNG_SEED,
           manifest->look_width,
           manifest->look_height,
           manifest->smooth_lighting ? "true" : "false",
           first_checkpoint_digest,
           repeat_checkpoint_digest,
           final->state_digest,
           repeat_final->state_digest,
           peak_rss,
           peak_rss != 0 ? "true" : "false");
    for (size_t i = 0; i < first.checkpoints_num; i++) {
        printf("%s", i == 0 ? "" : ",");
        player_view_movement_checkpoint_json(&first.checkpoints[i]);
    }
    printf("],\"same_process_checkpoints\":[");
    for (size_t i = 0; i < repeat.checkpoints_num; i++) {
        printf("%s", i == 0 ? "" : ",");
        player_view_movement_checkpoint_json(&repeat.checkpoints[i]);
    }
    printf("],\"lifecycle\":{\"full_map_draws\":%u,\"full_draw_reasons\":{"
           "\"reset_packet\":%u,\"changed_map_packet\":%u,\"noop_map_packet\":%u,"
           "\"animation_only_tick\":%u,\"resize\":%u,\"map_transition\":%u}},"
           "\"phases\":[",
           first.lifecycle.full_map_draws,
           first.lifecycle.reset_packet,
           first.lifecycle.changed_map_packet,
           first.lifecycle.noop_map_packet,
           first.lifecycle.animation_only_tick,
           first.lifecycle.resize,
           first.lifecycle.map_transition);
    for (size_t i = 0; i < arraysize(first.phases); i++) {
        player_view_movement_phase_json(&first.phases[i], i);
    }
    printf("]}\n");
    success = true;

cleanup:
    sprite_cache_clock_override_clear();
#ifdef ATRINIK_WIDGET_TESTS
    map_benchmark_fault_clear();
#endif
    if (queue_ready) {
        client_command_queue_deinitialize();
    }
    SDL_DestroySurface(local_minimap_surface);
    free(transition);
    return success;
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
            mode = strcmp(argv[0], "--player-view-benchmark") == 0 ? PLAYER_VIEW_BENCHMARK_STANDARD
                                                                   : PLAYER_VIEW_BENCHMARK_MOVEMENT;
        } else if (strcmp(argv[2], "large") == 0) {
            mode = strcmp(argv[0], "--player-view-benchmark") == 0 ? PLAYER_VIEW_BENCHMARK_LARGE
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
    if (!player_view_manifest_parse(argv[1], &manifest) ||
        !player_view_file_sha256(argv[1], manifest.manifest_digest)) {
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
    if (manifest.next_snapshot_path != NULL) {
        player_view_movement_fixture_t movement_fixture;
        bool loaded = player_view_snapshot_load(manifest.next_snapshot_path,
                                                &next_snapshot,
                                                &next_snapshot_size);
        bool compatible =
            loaded &&
            (mode == PLAYER_VIEW_BENCHMARK_MOVEMENT
                 ? player_view_movement_fixture_parse(next_snapshot,
                                                      next_snapshot_size,
                                                      MAP_LOOK_TO_WIRE_SIZE(manifest.look_width),
                                                      MAP_LOOK_TO_WIRE_SIZE(manifest.look_height),
                                                      &movement_fixture)
                 : map_protocol_validate(next_snapshot,
                                         next_snapshot_size,
                                         0,
                                         MAP_LOOK_TO_WIRE_SIZE(manifest.look_width),
                                         MAP_LOOK_TO_WIRE_SIZE(manifest.look_height)));
        if (!compatible) {
            fprintf(stderr, "player-view: malformed or incompatible next MAP input\n");
            free(next_snapshot);
            free(snapshot);
            player_view_manifest_free(&manifest);
            return 5;
        }
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
    if (mode == PLAYER_VIEW_RENDER && manifest.player_names && manifest.target_ui) {
        widget_map_ui_test_begin();
    }
#endif
    if (mode != PLAYER_VIEW_BENCHMARK_MOVEMENT) {
        socket_command_map(snapshot, snapshot_size, 0);
        if (image_missing_faces_detected()) {
            fprintf(stderr, "player-view: snapshot references an unavailable face\n");
            goto cleanup;
        }
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
                                            next_snapshot_size,
                                            strcmp(argv[2], "large") == 0)) {
            goto cleanup;
        }
        /* The movement path verifies every frozen input, hashes every
         * checkpoint, and emits its record atomically. Avoid fallible generic
         * render/output work after that record has reached stdout. */
        result = 0;
        goto cleanup;
    } else if (manifest.widget_render) {
#ifdef ATRINIK_WIDGET_TESTS
        widget_map_draw_test(&map_widget);
        map_widget_surface = map_widget.surface;
#endif
    } else {
        map_draw_map(surface);
    }

    if (next_snapshot != NULL && mode != PLAYER_VIEW_BENCHMARK_MOVEMENT) {
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
    if (mode == PLAYER_VIEW_RENDER && manifest.player_names && manifest.target_ui) {
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
