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

/** @file Deterministic, bounded replay through the live map decoder/renderer.
 */

#include <global.h>

#include <animations.h>
#include <commands.h>
#include <image_codec.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <lighting.h>
#include <map_transform.h>
#include <metaserver.h>
#include <notification.h>
#include <openssl/evp.h>
#include <gpu_player_view.h>
#include <region_map.h>
#include <resources.h>
#include <toolkit/map_protocol.h>
#include <toolkit/packet.h>
#include <wrapper.h>
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
#define PLAYER_VIEW_ARTIFACT_PATH_SIZE 512
#define PLAYER_VIEW_DEATH_FACE 9
#define PLAYER_VIEW_BENCHMARK_ITERATIONS 40
#define PLAYER_VIEW_BENCHMARK_WARMUPS 3
#define PLAYER_VIEW_LIFECYCLE_EVENTS 12
#define PLAYER_VIEW_LARGE_WIDTH 1920
#define PLAYER_VIEW_LARGE_HEIGHT 1080
#define PLAYER_VIEW_BRYNKNOT_WIDTH 1024
#define PLAYER_VIEW_BRYNKNOT_HEIGHT 780
#define PLAYER_VIEW_MOVEMENT_TICK_MS 125U
#define PLAYER_VIEW_MOVEMENT_SUSTAINED_TICKS 480U
#define PLAYER_VIEW_MOVEMENT_IDLE_TICKS 16U
#define PLAYER_VIEW_MOVEMENT_RESUMED_TICKS 80U
#define PLAYER_VIEW_MOVEMENT_PACKETS 5U
#define PLAYER_VIEW_MOVEMENT_ACTIVE_PACKETS 4U
#define PLAYER_VIEW_MOVEMENT_SCHEMA_VERSION 10U
#define PLAYER_VIEW_MOVEMENT_WINDOW_TICKS 32U
#define PLAYER_VIEW_MOVEMENT_FIXTURE_SCHEMA 3U
#define PLAYER_VIEW_MOVEMENT_CHECKPOINTS 12U
#define PLAYER_VIEW_MOVEMENT_RNG_SEED UINT64_C(0x1961932026)
#define PLAYER_VIEW_MOVEMENT_SIMULATED_COMMAND_US UINT64_C(5000)
#define PLAYER_VIEW_CURSOR_TICKS 16U
#define PLAYER_VIEW_CURSOR_SCHEMA_VERSION 1U
#define PLAYER_VIEW_CURSOR_DIRTY_RADIUS 128

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
#ifndef ATRINIK_GPU_SHADER_COHORT
#define ATRINIK_GPU_SHADER_COHORT "unknown"
#endif

typedef enum player_view_mode {
    PLAYER_VIEW_RENDER,
    PLAYER_VIEW_BENCHMARK_STANDARD,
    PLAYER_VIEW_BENCHMARK_LARGE,
    PLAYER_VIEW_BENCHMARK_MOVEMENT,
    PLAYER_VIEW_BENCHMARK_CURSOR,
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
    char *interface_path;
    char *layout_path;
    char *font_path;
    char *mono_font_path;
    char settings_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char archdef_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char next_snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char transition_snapshot_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char interface_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char layout_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char font_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char mono_font_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char expected_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char archived_software_ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char archived_software_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
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
    uint32_t widget_x;
    uint32_t widget_y;
    int32_t animation_depth;
    int32_t animation_x_offset;
    int32_t animation_y_offset;
    uint32_t animation_sub_layer;
    bool smooth_lighting;
    uint32_t zoom_filter;
    bool primary_surface;
    bool widget_render;
    bool player_names;
    bool target_ui;
    bool ui_closure;
    bool damage_animation;
    bool kill_animation;
    bool visibility_fade_test;
    bool map_interaction_test;
    bool animation_elevated;
    bool animation_layer_content;
    bool animation_coordinates_set;
} player_view_manifest_t;

typedef struct player_view_movement_packet {
    uint8_t *data;
    size_t size;
} player_view_movement_packet_t;

typedef struct player_view_movement_fixture {
    player_view_movement_packet_t packets[PLAYER_VIEW_MOVEMENT_PACKETS];
} player_view_movement_fixture_t;

static void player_view_manifest_free(player_view_manifest_t *manifest) {
    free(manifest->input_root);
    free(manifest->settings_path);
    free(manifest->archdef_path);
    free(manifest->snapshot_path);
    free(manifest->next_snapshot_path);
    free(manifest->transition_snapshot_path);
    free(manifest->interface_path);
    free(manifest->layout_path);
    free(manifest->font_path);
    free(manifest->mono_font_path);
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

static bool
player_view_parse_int(const char *text, int32_t minimum, int32_t maximum, int32_t *value) {
    if (text == NULL || *text == '\0' || isspace((unsigned char)*text)) {
        return false;
    }
    errno = 0;
    char *end;
    long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (int32_t)parsed;
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
                                           "interface",
                                           "interface-sha256",
                                           "layout",
                                           "layout-sha256",
                                           "font",
                                           "font-sha256",
                                           "mono-font",
                                           "mono-font-sha256",
                                           "viewport-width",
                                           "viewport-height",
                                           "resize-width-delta",
                                           "resize-height-delta",
                                           "look-width",
                                           "look-height",
                                           "map-zoom",
                                           "smooth-lighting",
                                           "zoom-smoothing",
                                           "zoom-filter",
                                           "primary-surface",
                                           "widget-render",
                                           "widget-x",
                                           "widget-y",
                                           "player-names",
                                           "target-ui",
                                           "ui-closure",
                                           "damage-animation",
                                           "kill-animation",
                                           "visibility-fade-test",
                                           "map-interaction-test",
                                           "animation-depth",
                                           "animation-sub-layer",
                                           "animation-x-offset",
                                           "animation-y-offset",
                                           "animation-elevated",
                                           "animation-layer-content",
                                           "clock-ms",
                                           "archived-software-ui-pixels-sha256",
                                           "archived-software-pixels-sha256",
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
    char *interface_file = success ? player_view_xml_property(root, "interface") : NULL;
    char *interface_digest = success ? player_view_xml_property(root, "interface-sha256") : NULL;
    char *layout = success ? player_view_xml_property(root, "layout") : NULL;
    char *layout_digest = success ? player_view_xml_property(root, "layout-sha256") : NULL;
    char *font = success ? player_view_xml_property(root, "font") : NULL;
    char *font_digest = success ? player_view_xml_property(root, "font-sha256") : NULL;
    char *mono_font = success ? player_view_xml_property(root, "mono-font") : NULL;
    char *mono_font_digest = success ? player_view_xml_property(root, "mono-font-sha256") : NULL;
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
    char *zoom_filter = success ? player_view_xml_property(root, "zoom-filter") : NULL;
    char *primary_surface = success ? player_view_xml_property(root, "primary-surface") : NULL;
    char *widget_render = success ? player_view_xml_property(root, "widget-render") : NULL;
    char *widget_x = success ? player_view_xml_property(root, "widget-x") : NULL;
    char *widget_y = success ? player_view_xml_property(root, "widget-y") : NULL;
    char *player_names = success ? player_view_xml_property(root, "player-names") : NULL;
    char *target_ui = success ? player_view_xml_property(root, "target-ui") : NULL;
    char *ui_closure = success ? player_view_xml_property(root, "ui-closure") : NULL;
    char *damage_animation = success ? player_view_xml_property(root, "damage-animation") : NULL;
    char *kill_animation = success ? player_view_xml_property(root, "kill-animation") : NULL;
    char *visibility_fade_test =
        success ? player_view_xml_property(root, "visibility-fade-test") : NULL;
    char *map_interaction_test =
        success ? player_view_xml_property(root, "map-interaction-test") : NULL;
    char *animation_depth = success ? player_view_xml_property(root, "animation-depth") : NULL;
    char *animation_sub_layer =
        success ? player_view_xml_property(root, "animation-sub-layer") : NULL;
    char *animation_x_offset =
        success ? player_view_xml_property(root, "animation-x-offset") : NULL;
    char *animation_y_offset =
        success ? player_view_xml_property(root, "animation-y-offset") : NULL;
    char *animation_elevated =
        success ? player_view_xml_property(root, "animation-elevated") : NULL;
    char *animation_layer_content =
        success ? player_view_xml_property(root, "animation-layer-content") : NULL;
    char *clock_ms = success ? player_view_xml_property(root, "clock-ms") : NULL;
    char *expected_ui =
        success ? player_view_xml_property(root, "expected-ui-pixels-sha256") : NULL;
    char *archived_software_ui =
        success ? player_view_xml_property(root, "archived-software-ui-pixels-sha256") : NULL;
    char *archived_software =
        success ? player_view_xml_property(root, "archived-software-pixels-sha256") : NULL;
    char *expected_standard_checkpoint =
        success ? player_view_xml_property(root, "expected-standard-checkpoint-sha256") : NULL;
    char *expected = success ? player_view_xml_property(root, "expected-pixels-sha256") : NULL;

    uint32_t parsed_version;
    bool legacy_zoom_smoothing = false;
    success =
        success &&
        player_view_parse_uint(version,
                               PLAYER_VIEW_SCHEMA_VERSION,
                               PLAYER_VIEW_SCHEMA_VERSION,
                               &parsed_version) &&
        strcmp(renderer != NULL ? renderer : "", "gpu") == 0 &&
        player_view_path_relative(input_root) && settings != NULL &&
        player_view_sha256_text_valid(settings_digest) && archdef != NULL &&
        player_view_sha256_text_valid(archdef_digest) && snapshot != NULL &&
        player_view_sha256_text_valid(snapshot_digest) &&
        ((next_snapshot == NULL && next_snapshot_digest == NULL) ||
         (next_snapshot != NULL && player_view_sha256_text_valid(next_snapshot_digest))) &&
        ((transition_snapshot == NULL && transition_snapshot_digest == NULL) ||
         (transition_snapshot != NULL &&
          player_view_sha256_text_valid(transition_snapshot_digest))) &&
        ((interface_file == NULL && interface_digest == NULL) ||
         (interface_file != NULL && player_view_sha256_text_valid(interface_digest))) &&
        ((layout == NULL && layout_digest == NULL) ||
         (layout != NULL && player_view_sha256_text_valid(layout_digest))) &&
        ((font == NULL && font_digest == NULL) ||
         (font != NULL && player_view_sha256_text_valid(font_digest))) &&
        ((mono_font == NULL && mono_font_digest == NULL) ||
         (mono_font != NULL && player_view_sha256_text_valid(mono_font_digest))) &&
        player_view_parse_uint(viewport_width, 64, 4096, &manifest->viewport_width) &&
        player_view_parse_uint(viewport_height, 64, 4096, &manifest->viewport_height) &&
        ((resize_width_delta == NULL && resize_height_delta == NULL) ||
         (player_view_parse_uint(resize_width_delta, 1, 4096, &manifest->resize_width_delta) &&
          player_view_parse_uint(resize_height_delta, 1, 4096, &manifest->resize_height_delta) &&
          manifest->resize_width_delta <= 4096 - manifest->viewport_width &&
          manifest->resize_height_delta <= 4096 - manifest->viewport_height)) &&
        player_view_parse_uint(look_width, 9, 28, &manifest->look_width) &&
        player_view_parse_uint(look_height, 9, 28, &manifest->look_height) &&
        player_view_parse_uint(map_zoom,
                               MAP_DISPLAY_ZOOM_MIN,
                               MAP_DISPLAY_ZOOM_MAX,
                               &manifest->map_zoom) &&
        player_view_parse_bool(smooth_lighting, &manifest->smooth_lighting) &&
        ((zoom_filter != NULL && zoom_smoothing == NULL &&
          player_view_parse_uint(zoom_filter,
                                 ZOOM_FILTER_OFF,
                                 ZOOM_FILTER_NUM - 1,
                                 &manifest->zoom_filter)) ||
         (zoom_filter == NULL && player_view_parse_bool(zoom_smoothing, &legacy_zoom_smoothing))) &&
        (primary_surface == NULL ||
         player_view_parse_bool(primary_surface, &manifest->primary_surface)) &&
        (widget_render == NULL ||
         player_view_parse_bool(widget_render, &manifest->widget_render)) &&
        (widget_x == NULL ||
         player_view_parse_uint(widget_x, 0, manifest->viewport_width - 1, &manifest->widget_x)) &&
        (widget_y == NULL ||
         player_view_parse_uint(widget_y, 0, manifest->viewport_height - 1, &manifest->widget_y)) &&
        (player_names == NULL || player_view_parse_bool(player_names, &manifest->player_names)) &&
        (target_ui == NULL || player_view_parse_bool(target_ui, &manifest->target_ui)) &&
        (ui_closure == NULL || player_view_parse_bool(ui_closure, &manifest->ui_closure)) &&
        (damage_animation == NULL ||
         player_view_parse_bool(damage_animation, &manifest->damage_animation)) &&
        (kill_animation == NULL ||
         player_view_parse_bool(kill_animation, &manifest->kill_animation)) &&
        (visibility_fade_test == NULL ||
         player_view_parse_bool(visibility_fade_test, &manifest->visibility_fade_test)) &&
        (map_interaction_test == NULL ||
         player_view_parse_bool(map_interaction_test, &manifest->map_interaction_test)) &&
        ((animation_depth == NULL && animation_sub_layer == NULL) ||
         (animation_depth != NULL && animation_sub_layer != NULL &&
          player_view_parse_int(animation_depth,
                                -MAP2_MAX_DEPTH,
                                MAP2_MAX_DEPTH,
                                &manifest->animation_depth) &&
          player_view_parse_uint(animation_sub_layer,
                                 0,
                                 NUM_SUB_LAYERS - 1,
                                 &manifest->animation_sub_layer))) &&
        (animation_elevated == NULL ||
         player_view_parse_bool(animation_elevated, &manifest->animation_elevated)) &&
        (animation_layer_content == NULL ||
         player_view_parse_bool(animation_layer_content, &manifest->animation_layer_content)) &&
        (animation_x_offset == NULL || player_view_parse_int(animation_x_offset,
                                                             -(int32_t)(manifest->look_width / 2),
                                                             (int32_t)(manifest->look_width / 2),
                                                             &manifest->animation_x_offset)) &&
        (animation_y_offset == NULL || player_view_parse_int(animation_y_offset,
                                                             -(int32_t)(manifest->look_height / 2),
                                                             (int32_t)(manifest->look_height / 2),
                                                             &manifest->animation_y_offset)) &&
        player_view_parse_uint(clock_ms, 0, UINT32_MAX, &manifest->clock_ms) &&
        (expected_standard_checkpoint == NULL ||
         player_view_sha256_text_valid(expected_standard_checkpoint)) &&
        (archived_software_ui == NULL || player_view_sha256_text_valid(archived_software_ui)) &&
        (archived_software == NULL || player_view_sha256_text_valid(archived_software)) &&
        player_view_sha256_text_valid(expected);
    if (success && zoom_filter == NULL) {
        /* Keep existing fixtures on the old boolean manifest contract. */
        manifest->zoom_filter = legacy_zoom_smoothing ? ZOOM_FILTER_PIXELART : ZOOM_FILTER_OFF;
    }
    if (success && primary_surface == NULL) {
        manifest->primary_surface = true;
    }
    bool ui_test = manifest->player_names && manifest->target_ui;
    bool overlay_test = manifest->damage_animation || manifest->kill_animation;
    manifest->animation_coordinates_set = animation_depth != NULL;
    success =
        success && (!manifest->widget_render || manifest->primary_surface) &&
        ((manifest->widget_x == 0 && manifest->widget_y == 0) || manifest->widget_render) &&
        manifest->player_names == manifest->target_ui &&
        (!manifest->ui_closure || (manifest->widget_render && ui_test)) && interface_file != NULL &&
        layout != NULL &&
        ((ui_test && manifest->widget_render && font != NULL) || (!ui_test && font == NULL)) &&
        ((overlay_test && manifest->widget_render && mono_font != NULL) ||
         (!overlay_test && mono_font == NULL)) &&
        (!manifest->animation_coordinates_set || overlay_test) &&
        ((animation_x_offset == NULL && animation_y_offset == NULL) || overlay_test) &&
        (!manifest->animation_elevated || overlay_test) &&
        (!manifest->animation_layer_content || overlay_test) &&
        ((ui_test && player_view_sha256_text_valid(expected_ui)) ||
         (!ui_test && expected_ui == NULL));
#ifndef ATRINIK_WIDGET_TESTS
    success = success && !manifest->widget_render && font == NULL && mono_font == NULL;
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
        if (interface_file != NULL) {
            manifest->interface_path = player_view_resolve_path(manifest->input_root,
                                                                interface_file,
                                                                manifest->input_root);
        }
        if (layout != NULL) {
            manifest->layout_path =
                player_view_resolve_path(manifest->input_root, layout, manifest->input_root);
        }
        if (font != NULL) {
            manifest->font_path =
                player_view_resolve_path(manifest->input_root, font, manifest->input_root);
        }
        if (mono_font != NULL) {
            manifest->mono_font_path =
                player_view_resolve_path(manifest->input_root, mono_font, manifest->input_root);
        }
        success = manifest->settings_path != NULL && manifest->archdef_path != NULL &&
                  manifest->snapshot_path != NULL &&
                  (next_snapshot == NULL || manifest->next_snapshot_path != NULL) &&
                  (transition_snapshot == NULL || manifest->transition_snapshot_path != NULL) &&
                  (interface_file == NULL || manifest->interface_path != NULL) &&
                  (layout == NULL || manifest->layout_path != NULL) &&
                  (font == NULL || manifest->font_path != NULL) &&
                  (mono_font == NULL || manifest->mono_font_path != NULL);
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
        if (interface_file != NULL) {
            snprintf(VS(manifest->interface_digest), "%s", interface_digest);
        }
        if (layout != NULL) {
            snprintf(VS(manifest->layout_digest), "%s", layout_digest);
        }
        if (font != NULL) {
            snprintf(VS(manifest->font_digest), "%s", font_digest);
        }
        if (mono_font != NULL) {
            snprintf(VS(manifest->mono_font_digest), "%s", mono_font_digest);
        }
        if (expected_ui != NULL) {
            snprintf(VS(manifest->expected_ui_pixels_digest), "%s", expected_ui);
        }
        if (archived_software_ui != NULL) {
            snprintf(VS(manifest->archived_software_ui_pixels_digest), "%s", archived_software_ui);
        }
        if (archived_software != NULL) {
            snprintf(VS(manifest->archived_software_pixels_digest), "%s", archived_software);
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
    free(interface_file);
    free(interface_digest);
    free(layout);
    free(layout_digest);
    free(font);
    free(font_digest);
    free(mono_font);
    free(mono_font_digest);
    free(viewport_width);
    free(viewport_height);
    free(resize_width_delta);
    free(resize_height_delta);
    free(look_width);
    free(look_height);
    free(map_zoom);
    free(smooth_lighting);
    free(zoom_smoothing);
    free(zoom_filter);
    free(primary_surface);
    free(widget_render);
    free(widget_x);
    free(widget_y);
    free(player_names);
    free(target_ui);
    free(ui_closure);
    free(damage_animation);
    free(kill_animation);
    free(visibility_fade_test);
    free(map_interaction_test);
    free(animation_depth);
    free(animation_sub_layer);
    free(animation_x_offset);
    free(animation_y_offset);
    free(animation_elevated);
    free(animation_layer_content);
    free(clock_ms);
    free(expected_ui);
    free(archived_software_ui);
    free(archived_software);
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
    static const struct {
        const char *path;
        const char *digest;
    } ui_fonts[] = {
        {"fonts/arial.ttf", "35c0f3559d8db569e36c31095b8a60d441643d95f59139de40e23fada819b833"},
        {"fonts/logisoso.ttf", "bf21ff53c1820898d8f0ad5f44ec012ff0efc0c52a5af78886e52f62db163eaf"},
        {"fonts/mono.ttf", "da4281dc7db17a3dfce64a62ced92875c5895340055ec8ba24a3914eb97b349d"},
        {"fonts/sans.ttf", "c4c45690b345435b2cba52ecabe275f05e49b389b39fe68ad03afbb551288d3d"},
        {"fonts/serif.ttf", "6afd927937d84fc00831441cbe0165b2d926b5bb9c18ac9018e5df6e4b0c006a"},
    };
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
    if (manifest->interface_path != NULL && !player_view_verify_input("interface defaults",
                                                                      manifest->interface_path,
                                                                      manifest->interface_digest,
                                                                      &total_size)) {
        return false;
    }
    if (manifest->layout_path != NULL && !player_view_verify_input("interface layout",
                                                                   manifest->layout_path,
                                                                   manifest->layout_digest,
                                                                   &total_size)) {
        return false;
    }
    if (manifest->font_path != NULL && !player_view_verify_input("font",
                                                                 manifest->font_path,
                                                                 manifest->font_digest,
                                                                 &total_size)) {
        return false;
    }
    if (manifest->mono_font_path != NULL && !player_view_verify_input("mono font",
                                                                      manifest->mono_font_path,
                                                                      manifest->mono_font_digest,
                                                                      &total_size)) {
        return false;
    }
    for (size_t i = 0; i < arraysize(ui_fonts); i++) {
        char *path =
            player_view_resolve_path(manifest->input_root, ui_fonts[i].path, manifest->input_root);
        bool verified =
            path != NULL &&
            player_view_verify_input("default UI font", path, ui_fonts[i].digest, &total_size);
        free(path);
        if (!verified) {
            return false;
        }
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

static bool player_view_movement_fixture_parse(uint8_t *data,
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
        fixture->packets[i] = (player_view_movement_packet_t){
            .data = &data[cursor],
            .size = packet_size,
        };
        cursor += packet_size;
    }
    return cursor == size;
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

static bool player_view_surface_rect_sha256(SDL_Surface *surface,
                                            const SDL_Rect *rect,
                                            char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    SDL_Surface *canonical = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (canonical == NULL) {
        return false;
    }
    SDL_Rect area = rect != NULL ? *rect : (SDL_Rect){0, 0, canonical->w, canonical->h};
    if (area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 || area.x > canonical->w - area.w ||
        area.y > canonical->h - area.h) {
        SDL_DestroySurface(canonical);
        return false;
    }

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool success = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    uint8_t dimensions[8];
    uint32_t width = (uint32_t)area.w;
    uint32_t height = (uint32_t)area.h;
    for (size_t i = 0; i < 4; i++) {
        dimensions[i] = (uint8_t)(width >> (24 - i * 8));
        dimensions[4 + i] = (uint8_t)(height >> (24 - i * 8));
    }
    success = success && EVP_DigestUpdate(context, dimensions, sizeof(dimensions)) == 1;

    bool locked = !SDL_MUSTLOCK(canonical) || SDL_LockSurface(canonical);
    success = success && locked;
    for (int y = 0; success && y < area.h; y++) {
        const uint8_t *row = (const uint8_t *)canonical->pixels +
                             (size_t)(area.y + y) * canonical->pitch + (size_t)area.x * 4U;
        if (EVP_DigestUpdate(context, row, (size_t)area.w * 4U) != 1) {
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

static bool player_view_surface_sha256(SDL_Surface *surface,
                                       char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    return player_view_surface_rect_sha256(surface, NULL, digest);
}

static bool gpu_player_view_qualified(void) {
    const char *value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                   "ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE");
    return value != NULL && strcmp(value, "1") == 0;
}

static const char *gpu_player_view_hardware_tier(void) {
    const char *value =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_HARDWARE_TIER");
    return value != NULL && *value != '\0' ? value : "unavailable";
}

static bool gpu_player_view_identity_valid(void) {
    const char *tier = gpu_player_view_hardware_tier();
    return strcmp(ATRINIK_BENCHMARK_REVISION, "unknown") != 0 &&
           strlen(ATRINIK_BENCHMARK_REVISION) == 40 &&
           strcmp(ATRINIK_BENCHMARK_DIRTY, "false") == 0 &&
           strcmp(ATRINIK_BUILD_TYPE, "Release") == 0 &&
           (strcmp(tier, "reference") == 0 || strcmp(tier, "minimum") == 0);
}

static bool gpu_player_view_digest_zero(const char *digest) {
    for (size_t i = 0; i < PLAYER_VIEW_SHA256_HEX_SIZE - 1; i++) {
        if (digest[i] != '0') {
            return false;
        }
    }
    return digest[PLAYER_VIEW_SHA256_HEX_SIZE - 1] == '\0';
}

static bool gpu_player_view_render_complete(void);
static bool gpu_player_view_recover_once(SDL_Window *window);

static bool gpu_player_view_render(widgetdata *widget, bool widget_render) {
#ifdef ATRINIK_WIDGET_TESTS
    (void)widget;
    (void)widget_render;
    return gpu_player_view_render_complete();
#else
    if (!gpu_renderer_begin_frame()) {
        return false;
    }
    (void)widget_render;
    map_draw_map(widget->surface);
    if (!gpu_renderer_draw_map((float)widget->x,
                               (float)widget->y,
                               (float)widget->w,
                               (float)widget->h)) {
        return false;
    }
    return gpu_renderer_frame_valid() && gpu_renderer_present() && gpu_renderer_wait_idle();
#endif
}

static bool gpu_player_view_render_complete(void) {
    if (!gpu_renderer_begin_frame()) {
        return false;
    }
#ifdef ATRINIK_WIDGET_TESTS
    uint64_t ui_started = gpu_renderer_timing_begin();
    if (cpl.state <= ST_WAITFORPLAY) {
        intro_show();
    } else if (cpl.state == ST_PLAY) {
        process_widgets(1);
    }
    popup_render_all();
    tooltip_show();
    if (event_dragging_check()) {
        int mx, my;
        mouse_get_state(&mx, &my);
        object_show_centered(OfflineRenderSurface,
                             object_find(cpl.dragging_tag),
                             mx,
                             my,
                             INVENTORY_ICON_SIZE,
                             INVENTORY_ICON_SIZE,
                             false);
    }
    if (cpl.state == ST_PLAY) {
        map_draw_pointer_overlay();
    }
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_UI, ui_started);
#else
    return false;
#endif
    return gpu_renderer_frame_valid() && gpu_renderer_present() && gpu_renderer_wait_idle();
}

static char gpu_player_view_review_prefix[256];

static bool gpu_player_view_review_save(SDL_Surface *surface,
                                        const char *label,
                                        char artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE],
                                        char artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    const char *directory = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                       "ATRINIK_GPU_CONFORMANCE_REVIEW_DIRECTORY");
    artifact[0] = '\0';
    artifact_digest[0] = '\0';
    if (directory == NULL || *directory == '\0') {
        return true;
    }
    char path[HUGE_BUF];
    if (snprintf(artifact,
                 PLAYER_VIEW_ARTIFACT_PATH_SIZE,
                 "review/%s-%s.png",
                 gpu_player_view_review_prefix,
                 label) >= PLAYER_VIEW_ARTIFACT_PATH_SIZE ||
        snprintf(path,
                 sizeof(path),
                 "%s/%s-%s.png",
                 directory,
                 gpu_player_view_review_prefix,
                 label) >= (int)sizeof(path)) {
        return false;
    }
    mkdir_ensure(path);
    return image_codec_save_png(surface, path) && player_view_file_sha256(path, artifact_digest);
}

static bool gpu_player_view_checkpoint_named(const char *label,
                                             char digest[PLAYER_VIEW_SHA256_HEX_SIZE],
                                             char artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE],
                                             char artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    SDL_Surface *completed = gpu_renderer_readback(NULL);
    bool success = completed != NULL && player_view_surface_sha256(completed, digest) &&
                   gpu_player_view_review_save(completed, label, artifact, artifact_digest);
    SDL_DestroySurface(completed);
    return success;
}

static bool gpu_player_view_checkpoint(char digest[PLAYER_VIEW_SHA256_HEX_SIZE]) {
    char artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE];
    char artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    return gpu_player_view_checkpoint_named("checkpoint", digest, artifact, artifact_digest);
}

static bool
gpu_player_view_slot_uniform_uploads_bounded(const gpu_renderer_statistics_t *statistics) {
    return statistics->slot_uniform_upload_count <= UINT64_MAX / 1024U &&
           statistics->slot_uniform_upload_bytes >= statistics->slot_uniform_upload_count * 16U &&
           statistics->slot_uniform_upload_bytes <= statistics->slot_uniform_upload_count * 1024U;
}

static void gpu_player_view_json_string_to(FILE *output, const char *value) {
    fputc('"', output);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor == '"' || *cursor == '\\') {
            fputc('\\', output);
            fputc(*cursor, output);
        } else if (*cursor >= 0x20) {
            fputc(*cursor, output);
        }
    }
    fputc('"', output);
}

static void gpu_player_view_json_string(const char *value) {
    gpu_player_view_json_string_to(stdout, value);
}

#ifdef ATRINIK_WIDGET_TESTS
#define PLAYER_VIEW_UI_STATES 19

typedef struct gpu_player_view_ui_state {
    const char *name;
    char digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    int width;
    int height;
    gpu_renderer_statistics_t steady;
    text_root_glyph_statistics_t root_glyphs;
    const char *command;
    bool asynchronous;
    char artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE];
    char artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
} gpu_player_view_ui_state_t;

typedef struct gpu_player_view_ui_closure {
    gpu_player_view_ui_state_t states[PLAYER_VIEW_UI_STATES];
    size_t states_num;
} gpu_player_view_ui_closure_t;

static gpu_player_view_ui_closure_t gpu_player_view_ui_closure;

static bool gpu_player_view_root_glyphs_match(const gpu_player_view_ui_state_t *state) {
    uint64_t expected_count;
    uint64_t expected_hash;
    if (strcmp(state->name, "intro_server_browser") == 0) {
        expected_count = 383;
        expected_hash = UINT64_C(0x29c427a4eff9acbd);
    } else if (strcmp(state->name, "login_popup") == 0 ||
               strcmp(state->name, "popup_character_selection") == 0) {
        expected_count = 385;
        expected_hash = UINT64_C(0x4266544b0b8b6fbd);
    } else {
        return true;
    }
    return state->root_glyphs.count == expected_count &&
           state->root_glyphs.semantic_hash == expected_hash;
}

static bool gpu_player_view_ui_capture(const char *name, bool notification_fade) {
    if (gpu_player_view_ui_closure.states_num >= PLAYER_VIEW_UI_STATES) {
        SDL_SetError("UI closure state capacity exceeded before %s", name);
        return false;
    }
    if (notification_fade && !notification_test_fade(2500)) {
        SDL_SetError("could not advance notification fade before %s", name);
        return false;
    }
    if (!gpu_player_view_render_complete()) {
        SDL_SetError("could not render cold UI closure state %s", name);
        return false;
    }
    gpu_renderer_statistics_reset();
    text_root_glyph_statistics_reset();
    const char *suppressed_root_glyph =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                   "ATRINIK_GPU_CONFORMANCE_TEST_SUPPRESS_ROOT_GLYPH");
    if (suppressed_root_glyph != NULL && strcmp(suppressed_root_glyph, name) == 0) {
        text_root_glyph_test_suppress_once();
    }
    if (notification_fade && !notification_test_fade(2500)) {
        SDL_SetError("could not advance notification fade for steady %s", name);
        return false;
    }
    if (!gpu_player_view_render_complete()) {
        SDL_SetError("could not render steady UI closure state %s", name);
        return false;
    }
    gpu_player_view_ui_state_t *state =
        &gpu_player_view_ui_closure.states[gpu_player_view_ui_closure.states_num++];
    state->name = name;
    gpu_renderer_statistics_get(&state->steady);
    text_root_glyph_statistics_get(&state->root_glyphs);
    if (state->steady.upload_count != state->steady.slot_uniform_upload_count ||
        state->steady.upload_bytes != state->steady.slot_uniform_upload_bytes ||
        !gpu_player_view_slot_uniform_uploads_bounded(&state->steady) ||
        state->steady.resource_creations != 0 || state->steady.resource_destructions != 0 ||
        state->steady.readbacks != 0 || state->steady.fallbacks != 0 ||
        !gpu_player_view_root_glyphs_match(state) ||
        !gpu_renderer_output_size(&state->width, &state->height) ||
        !gpu_player_view_checkpoint_named(name,
                                          state->digest,
                                          state->artifact,
                                          state->artifact_digest)) {
        SDL_SetError("GPU UI closure state %s did not retain a stable frame", name);
        return false;
    }
    return true;
}

static bool gpu_player_view_ui_screenshot(const char *name,
                                          const char *command,
                                          int expected_width,
                                          int expected_height,
                                          const SDL_Rect *derived_crop,
                                          char derived_crop_digest[PLAYER_VIEW_SHA256_HEX_SIZE],
                                          const char *expected_digest) {
    if (gpu_player_view_ui_closure.states_num >= PLAYER_VIEW_UI_STATES ||
        !gpu_player_view_render_complete()) {
        return false;
    }
    gpu_renderer_statistics_reset();
    screenshot_test_begin();
    if (client_command_check(command) != 1 || screenshot_test_take() != NULL) {
        SDL_SetError("player-facing screenshot did not enqueue asynchronously");
        return false;
    }
    if (!gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_readback_poll();
    SDL_Surface *surface = screenshot_test_take();
    gpu_player_view_ui_state_t *state =
        &gpu_player_view_ui_closure.states[gpu_player_view_ui_closure.states_num++];
    state->name = name;
    state->command = command;
    state->asynchronous = true;
    gpu_renderer_statistics_get(&state->steady);
    bool success =
        surface != NULL && surface->w == expected_width && surface->h == expected_height &&
        player_view_surface_sha256(surface, state->digest) &&
        gpu_player_view_review_save(surface, name, state->artifact, state->artifact_digest);
    if (success && derived_crop != NULL) {
        success = derived_crop_digest != NULL &&
                  player_view_surface_rect_sha256(surface, derived_crop, derived_crop_digest);
    }
    if (success && expected_digest != NULL) {
        if (strcmp(state->digest, expected_digest) != 0) {
            SDL_SetError("player-facing map screenshot is not the exact completed-window crop");
            success = false;
        }
    }
    if (surface != NULL) {
        state->width = surface->w;
        state->height = surface->h;
    }
    SDL_DestroySurface(surface);
    return success && state->steady.readbacks == 1 && state->steady.fallbacks == 0;
}

static bool gpu_player_view_ui_delayed_repeat(const gpu_player_view_ui_state_t *expected) {
    SDL_Delay(1100);
    if (!gpu_player_view_render_complete()) {
        return false;
    }
    char digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!gpu_player_view_checkpoint(digest) || strcmp(digest, expected->digest) != 0) {
        SDL_SetError("frozen UI state changed after a wall-clock delay");
        return false;
    }
    return true;
}

static bool gpu_player_view_ui_region_map_prepare(void) {
    region_map_t *region = MapData.region_map;
    if (region == NULL) {
        SDL_SetError("UI fixture region-map model is missing");
        return false;
    }
    if (region->surface != NULL) {
        SDL_SetError("UI fixture region-map surface was populated before its transition");
        return false;
    }
    region->surface = SDL_CreateSurface(800, 600, SDL_PIXELFORMAT_RGBA32);
    if (region->surface == NULL ||
        !SDL_FillSurfaceRect(region->surface, NULL, surface_map_rgb(region->surface, 32, 48, 64))) {
        SDL_SetError("could not create the UI fixture region-map surface");
        return false;
    }
    SDL_Rect landmark = {240, 160, 320, 280};
    if (!SDL_FillSurfaceRect(region->surface,
                             &landmark,
                             surface_map_rgb(region->surface, 112, 144, 80))) {
        SDL_SetError("could not fill the UI fixture region-map landmark");
        return false;
    }
    region->def->pixel_size = 4;
    region->def->map_size_x = 200;
    region->def->map_size_y = 150;
    region->def->num_maps = 1;
    region->def->maps = xcalloc(1, sizeof(*region->def->maps));
    region->def->maps[0].path = xstrdup("/gpu-ui-closure");
    region->fow->path = xstrdup("gpu-ui-closure-in-memory.fow");
    region->fow->bitmap = xcalloc(1, RM_MAP_FOW_BITMAP_SIZE(region));
    snprintf(VS(MapData.map_path), "%s", region->def->maps[0].path);
    snprintf(VS(MapData.region_name), "%s", "gpu-ui-closure");
    snprintf(VS(MapData.region_longname), "%s", "GPU UI Closure Region");
    MapData.posx = 75;
    MapData.posy = 50;
    region_map_fow_update(region);
    /* The production minimap initializes this shared model at 50%. Normalize
     * to a non-100% fixture zoom so both scaled image and FOW resources exist. */
    region_map_resize(region, 75 - region->zoom);
    if (region->fow->surface == NULL || region->zoomed == NULL || region->fow_zoomed == NULL) {
        SDL_SetError("UI fixture region-map derivatives missing: fow=%d zoomed=%d fow_zoomed=%d",
                     region->fow->surface != NULL,
                     region->zoomed != NULL,
                     region->fow_zoomed != NULL);
        return false;
    }
    return true;
}

static bool gpu_player_view_ui_models_prepare(void) {
    object *item = object_create(cpl.ob, 477, 1);
    item->face = 3;
    item->itype = TYPE_MISC_OBJECT;
    item->nrof = 12;
    snprintf(VS(item->s_name), "%s", "Retained GPU qualification tokens");
    draw_info(COLOR_GREEN, "The complete retained GPU interface is active.");
    draw_info(COLOR_GREEN, "Root object markup: [obj=477 32 32 0]");

    /* The first draw initializes model-owning widget backgrounds such as the
     * party list. Populate those models only after that production init path. */
    widget_redraw_everything();
    if (!gpu_player_view_render_complete()) {
        SDL_SetError("could not initialize UI model canvases");
        return false;
    }
    packet_struct *party = packet_new(0, 128, 32);
    packet_writer_write_uint8(party, CMD_PARTY_LIST);
    packet_writer_write_cstring(party, "GPU Explorers");
    packet_writer_write_cstring(party, "Renderer Maintainer");
    socket_command_party(party->data, party->len, 0);
    packet_free(party);
    widget_redraw_everything();
    widgetdata *inventory = widget_find(NULL, INVENTORY_ID, "main", NULL);
    bool textwin_valid = false;
    for (widgetdata *textwin = cur_widget[CHATWIN_ID]; textwin != NULL;
         textwin = textwin->type_next) {
        size_t game_tab = 0;
        if (textwin_tab_find(textwin, CHAT_TYPE_GAME, NULL, &game_tab) &&
            TEXTWIN(textwin)->tabs[game_tab].entries_size != 0) {
            textwin_valid = true;
            break;
        }
    }
    static const char *stat_ids[] = {"health", "mana", "food", "exp"};
    bool stats_valid = true;
    for (size_t index = 0; index < arraysize(stat_ids); index++) {
        stats_valid &= widget_stat_test_valid(widget_find(NULL, STAT_ID, stat_ids[index], NULL));
    }
    if (inventory == NULL || INVENTORY(inventory)->display != INVENTORY_DISPLAY_MAIN ||
        cpl.ob->inv == NULL || !textwin_valid || widget_party_test_rows() != 1 || !stats_valid) {
        SDL_SetError("UI fixture models incomplete: inventory=%d object=%d text=%d party=%" PRIu64
                     " stats=%d",
                     inventory != NULL && INVENTORY(inventory)->display == INVENTORY_DISPLAY_MAIN,
                     cpl.ob->inv != NULL,
                     textwin_valid,
                     (uint64_t)widget_party_test_rows(),
                     stats_valid);
        return false;
    }
    return true;
}

static bool gpu_player_view_ui_painting_prepare(const player_view_manifest_t *manifest) {
    const char *path = NULL;
    for (size_t index = 0; index < manifest->assets_num; index++) {
        if (manifest->assets[index].face == 3) {
            path = manifest->assets[index].path;
            break;
        }
    }
    size_t size = 0;
    void *contents = path != NULL ? SDL_LoadFile(path, &size) : NULL;
    unsigned char digest[SHA512_DIGEST_LENGTH];
    bool success = contents != NULL && SHA512(contents, size, digest) != NULL;
    SDL_free(contents);
    if (!success) {
        return false;
    }
    packet_struct *resource = packet_new(0, 128, 32);
    packet_writer_write_cstring(resource, "gpu-ui-closure-resource");
    packet_writer_write_bytes(resource, digest, sizeof(digest));
    socket_command_resource(resource->data, resource->len, 0);
    packet_free(resource);
    return resources_test_bind_loaded_file("gpu-ui-closure-resource", path);
}

static bool gpu_player_view_ui_closure_run(widgetdata *map_widget,
                                           const player_view_manifest_t *manifest) {
    memset(&gpu_player_view_ui_closure, 0, sizeof(gpu_player_view_ui_closure));
    popup_destroy_all();
    tooltip_dismiss();
    notification_destroy();
    region_map_test_fow_persistence_set(false);
    metaserver_clear_data();
    selected_server = metaserver_add("fixture.invalid",
                                     13327,
                                     "Frozen GPU Qualification",
                                     PACKAGE_VERSION,
                                     "Immutable offline UI fixture server");

    intro_test_begin();
    cpl.state = ST_START;
    if (!gpu_player_view_ui_capture("intro_server_browser", false)) {
        return false;
    }
    login_start();
    cpl.state = ST_LOGIN;
    if (!gpu_player_view_ui_capture("login_popup", false) || !login_test_form_rendered()) {
        SDL_SetError("ready login form did not render its production controls");
        return false;
    }
    popup_destroy_all();

    packet_struct *characters_packet = packet_new(0, 256, 32);
    char connection_id[SOCKET_CONNECTION_ID_SIZE];
    memset(connection_id, 'a', sizeof(connection_id) - 1U);
    connection_id[sizeof(connection_id) - 1U] = '\0';
    packet_writer_write_uint8(characters_packet, CLIENT_CMD_CHARACTERS);
    packet_writer_write_cstring(characters_packet, "renderer-fixture");
    packet_writer_write_cstring(characters_packet, connection_id);
    packet_writer_write_cstring(characters_packet, "");
    packet_writer_write_uint64(characters_packet, 0);
    popup_test_surface_allocation_fail_once();
    cpl.state = ST_START;
    bool characters_dispatch_complete =
        client_command_dispatch_test(characters_packet->data, characters_packet->len);
    packet_free(characters_packet);
    if (characters_dispatch_complete || cpl.state != ST_START ||
        !gpu_renderer_recreation_take_request() || !gpu_player_view_recover_once(ScreenWindow) ||
        cpl.state != ST_CHARACTERS || popup_get_head() == NULL) {
        SDL_SetError("pre-frame CHARACTERS popup failure was not retained and replayed");
        return false;
    }
    popup_destroy_all();

    characters_open();
    cpl.state = ST_CHARACTERS;
    if (!gpu_player_view_ui_capture("popup_character_selection", false)) {
        return false;
    }
    popup_destroy_all();
    intro_deinit();
    cpl.state = ST_PLAY;
    toolkit_widget_fixture_show_all();

    /* Exercise the production minimap's first allocation before its 250 ms
     * refresh interval has elapsed. Other widgets keep the fixture's normal
     * clock so unrelated startup timers do not enter artificial wraparound. */
    client_ui_test_clock_set(100);
    map_benchmark_statistics_reset();
    bool minimap_cold_start = gpu_renderer_begin_frame();
    if (minimap_cold_start) {
        widget_minimap_cold_start_draw_test(cur_widget[MINIMAP_ID]);
        minimap_cold_start =
            gpu_renderer_frame_valid() && gpu_renderer_present() && gpu_renderer_wait_idle();
    }
    map_benchmark_statistics_t minimap_cold_start_map;
    map_benchmark_statistics_get(&minimap_cold_start_map);
    client_ui_test_clock_set(manifest->clock_ms);
    if (!minimap_cold_start || minimap_cold_start_map.auxiliary_map_draws != 1U) {
        SDL_SetError(
            "dynamic minimap did not compose its first canvas before the refresh interval: "
            "frame=%d auxiliary=%llu failures=%llu",
            minimap_cold_start,
            (unsigned long long)minimap_cold_start_map.auxiliary_map_draws,
            (unsigned long long)minimap_cold_start_map.render_failures);
        return false;
    }

    if (!gpu_player_view_ui_models_prepare()) {
        return false;
    }
    event_dragging_start(477, -100, -100);
    if (!gpu_player_view_ui_capture("gameplay_widgets_text_windows", false)) {
        event_dragging_stop();
        return false;
    }
    if (!gpu_player_view_ui_delayed_repeat(
            &gpu_player_view_ui_closure.states[gpu_player_view_ui_closure.states_num - 1U])) {
        event_dragging_stop();
        return false;
    }
    event_dragging_stop();

    widgetdata *menu = create_menu(40, 40, map_widget);
    add_menuitem(menu, "Inspect retained GPU state", NULL, MENU_NORMAL, 0);
    add_menuitem(menu, "Walk here", NULL, MENU_NORMAL, 0);
    menu_finalize(menu);
    tooltip_create(260, 180, FONT_ARIAL11, "GPU retained tooltip");
    tooltip_multiline(220);
    packet_struct *notification_packet = packet_new(0, 128, 32);
    packet_writer_write_uint8(notification_packet, CMD_NOTIFICATION_TEXT);
    packet_writer_write_cstring(notification_packet,
                                "Retained GPU notification with [b]markup[/b]");
    packet_writer_write_uint8(notification_packet, CMD_NOTIFICATION_DELAY);
    packet_writer_write_uint32(notification_packet, 10000);
    socket_command_notification(notification_packet->data, notification_packet->len, 0);
    packet_free(notification_packet);
    if (!gpu_player_view_ui_capture("context_menu_tooltip_notification", false)) {
        return false;
    }

    /* A tooltip is a frame-renewed transient by contract. Its dedicated
     * closure state above proves GPU composition, but it is not durable state
     * for device recovery. Establish the recovery oracle after dismissing it. */
    tooltip_dismiss();
    if (!gpu_player_view_render_complete()) {
        SDL_SetError("could not establish stable UI recovery baseline");
        return false;
    }
    char recovery_expected_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!gpu_player_view_checkpoint(recovery_expected_digest)) {
        SDL_SetError("could not capture stable UI recovery baseline");
        return false;
    }
    uint64_t notification_compositions = notification_test_canvas_compositions();
    if (minimap_redraw_due()) {
        SDL_SetError("dynamic minimap was not throttled before immediate recovery proof");
        return false;
    }
    map_benchmark_statistics_reset();
    if (!gpu_player_view_recover_once(ScreenWindow)) {
        return false;
    }
    char recovery_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    map_benchmark_statistics_t recovery_map;
    map_benchmark_statistics_get(&recovery_map);
    bool recovery_checkpointed = gpu_player_view_checkpoint(recovery_digest);
    bool recovery_digest_matches =
        recovery_checkpointed && strcmp(recovery_digest, recovery_expected_digest) == 0;
    if (!recovery_digest_matches || recovery_map.auxiliary_map_draws != 1 ||
        notification_test_canvas_compositions() != notification_compositions + 1U) {
        SDL_SetError("immediate GPU recovery did not reconstruct minimap/notification canvases: "
                     "auxiliary=%llu notification=%llu/%llu digest=%d",
                     (unsigned long long)recovery_map.auxiliary_map_draws,
                     (unsigned long long)notification_test_canvas_compositions(),
                     (unsigned long long)(notification_compositions + 1U),
                     recovery_digest_matches);
        return false;
    }
    if (!gpu_player_view_ui_capture("notification_fading", true)) {
        return false;
    }
    tooltip_dismiss();
    notification_destroy();
    remove_widget_object(menu);

    server_add_open();
    if (!gpu_player_view_ui_capture("popup_generic_input_controls", false)) {
        return false;
    }
    popup_destroy_all();

    const char *book = "[b]Retained GPU book[/b]\nEvery glyph and border is GPU composed.";
    book_load(book, (int)strlen(book));
    if (!gpu_player_view_ui_capture("popup_book", false)) {
        return false;
    }
    popup_destroy_all();

    settings_client_open();
    if (!gpu_player_view_ui_capture("popup_settings_controls", false)) {
        return false;
    }
    popup_destroy_all();

    color_chooser_open();
    if (!gpu_player_view_ui_capture("popup_color_picker", false)) {
        return false;
    }
    popup_destroy_all();

    /* Popup allocation can fail while processing input, before begin_frame().
     * Preserve the user intent, latch recovery, and publish it only after the
     * replacement device has reconstructed the complete scene. Repeating the
     * failure also proves that the superseded copied server state is released. */
    popup_test_surface_allocation_fail_once();
    connection_preference_open(selected_server);
    if (!connection_preference_test_pending()) {
        SDL_SetError("pre-frame popup allocation failure did not retain its model");
        return false;
    }
    popup_test_surface_allocation_fail_once();
    connection_preference_open(selected_server);
    if (!connection_preference_test_pending() || !gpu_renderer_recreation_take_request() ||
        !gpu_player_view_recover_once(ScreenWindow) || !connection_preference_test_active()) {
        SDL_SetError("pre-frame popup allocation failure did not recover and republish");
        return false;
    }
    popup_destroy_all();
    if (connection_preference_test_pending() || connection_preference_test_active()) {
        SDL_SetError("recovered connection-preference popup retained stale state");
        return false;
    }

    connection_preference_open(selected_server);
    if (!gpu_player_view_ui_capture("popup_connection_preference", false)) {
        return false;
    }
    popup_destroy_all();

    join_password_open(selected_server);
    if (!gpu_player_view_ui_capture("popup_join_password", false)) {
        return false;
    }
    popup_destroy_all();

    credits_test_show("[b]Atrinik GPU qualification[/b]\nRetained scrolling credits text.");
    if (!gpu_player_view_ui_capture("popup_credits", false)) {
        return false;
    }
    popup_destroy_all();

    if (!gpu_player_view_ui_painting_prepare(manifest)) {
        return false;
    }
    packet_struct *painting_packet = packet_new(0, 256, 64);
    packet_writer_write_cstring(painting_packet, "gpu-ui-closure-resource");
    packet_writer_write_cstring(painting_packet, "Retained GPU Painting");
    packet_writer_write_cstring(painting_packet,
                                "Painting popup chrome, title, message, and viewport.");
    socket_command_painting(painting_packet->data, painting_packet->len, 0);
    packet_free(painting_packet);
    if (!gpu_player_view_ui_capture("popup_painting", false) ||
        !popup_painting_test_viewport_rendered()) {
        return false;
    }
    popup_destroy_all();

    if (!gpu_player_view_ui_region_map_prepare()) {
        return false;
    }
    region_map_open();
    if (!gpu_player_view_ui_capture("popup_region_map_minimap", false)) {
        return false;
    }
    if (!region_map_fow_set_visited(MapData.region_map,
                                    &MapData.region_map->def->maps[0],
                                    NULL,
                                    85,
                                    60)) {
        SDL_SetError("could not mark the visible UI fixture FOW transition tile");
        return false;
    }
    region_map_fow_update(MapData.region_map);
    minimap_redraw_flag = 1;
    widget_redraw_everything();
    if (!gpu_player_view_ui_capture("region_map_fow_transition", false) ||
        !gpu_player_view_ui_capture("region_map_fow_retained", false)) {
        return false;
    }
    gpu_player_view_ui_state_t *initial = &gpu_player_view_ui_closure.states[14];
    gpu_player_view_ui_state_t *changed = &gpu_player_view_ui_closure.states[15];
    gpu_player_view_ui_state_t *retained = &gpu_player_view_ui_closure.states[16];
    if (strcmp(initial->digest, changed->digest) == 0 ||
        strcmp(changed->digest, retained->digest) != 0) {
        SDL_SetError("region-map FOW hashes invalid: initial_changed=%d changed_retained=%d",
                     strcmp(initial->digest, changed->digest) != 0,
                     strcmp(changed->digest, retained->digest) == 0);
        return false;
    }
    popup_destroy_all();

    int output_width, output_height;
    if (!gpu_renderer_output_size(&output_width, &output_height)) {
        return false;
    }
    int map_screenshot_width = output_width * 3 / 4;
    int map_screenshot_height = output_height * 3 / 4;
    map_widget->x = (output_width - map_screenshot_width) / 2;
    map_widget->y = (output_height - map_screenshot_height) / 2;
    map_widget->w = map_screenshot_width;
    map_widget->h = map_screenshot_height;
    widget_redraw_everything();
    SDL_Rect map_screenshot_rect = {map_widget->x, map_widget->y, map_widget->w, map_widget->h};
    char map_crop_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!gpu_player_view_ui_screenshot("screenshot_window",
                                       "/screenshot",
                                       output_width,
                                       output_height,
                                       &map_screenshot_rect,
                                       map_crop_digest,
                                       NULL)) {
        return false;
    }
    int left = MAX(0, widget_x(map_widget));
    int top = MAX(0, widget_y(map_widget));
    int right = MIN(output_width, widget_x(map_widget) + widget_w(map_widget));
    int bottom = MIN(output_height, widget_y(map_widget) + widget_h(map_widget));
    return right > left && bottom > top &&
           gpu_player_view_ui_screenshot("screenshot_map",
                                         "/screenshot map",
                                         right - left,
                                         bottom - top,
                                         NULL,
                                         NULL,
                                         map_crop_digest) &&
           gpu_player_view_ui_closure.states_num == PLAYER_VIEW_UI_STATES;
}

static void gpu_player_view_ui_closure_write(void) {
    if (gpu_player_view_ui_closure.states_num == 0) {
        fputs("null", stdout);
        return;
    }
    fputc('[', stdout);
    for (size_t index = 0; index < gpu_player_view_ui_closure.states_num; index++) {
        const gpu_player_view_ui_state_t *state = &gpu_player_view_ui_closure.states[index];
        printf("%s{\"name\":", index == 0 ? "" : ",");
        gpu_player_view_json_string(state->name);
        printf(",\"pixels_sha256\":\"%s\",\"output_size\":[%d,%d],"
               "\"command\":",
               state->digest,
               state->width,
               state->height);
        gpu_player_view_json_string(state->command != NULL ? state->command : "");
        printf(",\"artifact\":\"%s\",\"artifact_sha256\":\"%s\","
               "\"asynchronous\":%s,\"root_glyphs\":{"
               "\"count\":%" PRIu64 ",\"semantic_hash\":\"%016" PRIx64 "\"},"
               "\"steady_state\":{"
               "\"uploads\":%" PRIu64 ",\"upload_bytes\":%" PRIu64
               ",\"slot_uniform_uploads\":%" PRIu64 ",\"slot_uniform_upload_bytes\":%" PRIu64
               ",\"resource_creations\":%" PRIu64 ",\"resource_destructions\":%" PRIu64
               ",\"readbacks\":%" PRIu64 ",\"fallbacks\":%" PRIu64 "}}",
               state->artifact,
               state->artifact_digest,
               state->asynchronous ? "true" : "false",
               state->root_glyphs.count,
               state->root_glyphs.semantic_hash,
               state->steady.upload_count,
               state->steady.upload_bytes,
               state->steady.slot_uniform_upload_count,
               state->steady.slot_uniform_upload_bytes,
               state->steady.resource_creations,
               state->steady.resource_destructions,
               state->steady.readbacks,
               state->steady.fallbacks);
    }
    fputc(']', stdout);
}
#else
static bool gpu_player_view_ui_closure_run(widgetdata *map_widget,
                                           const player_view_manifest_t *manifest) {
    (void)map_widget;
    (void)manifest;
    return false;
}

static void gpu_player_view_ui_closure_write(void) {
    fputs("null", stdout);
}
#endif

static void gpu_player_view_record(const player_view_manifest_t *manifest,
                                   const char *fixture,
                                   const char *pixels_digest,
                                   const char *ui_pixels_digest,
                                   const char *timed_endpoint_digest,
                                   uint64_t borrowed_temporal_samples,
                                   bool movement_lifecycle,
                                   bool golden_verified,
                                   const char *initial_digest,
                                   const char *transition_digest,
                                   const char *initial_artifact,
                                   const char *initial_artifact_digest,
                                   const char *final_artifact,
                                   const char *final_artifact_digest) {
    fputs("{\"schema_version\":2,\"renderer\":\"gpu-production-player-view\",\"fixture\":", stdout);
    gpu_player_view_json_string(fixture);
    fputs(",\"revision\":", stdout);
    gpu_player_view_json_string(ATRINIK_BENCHMARK_REVISION);
    fputs(",\"dirty\":", stdout);
    if (strcmp(ATRINIK_BENCHMARK_DIRTY, "true") == 0 ||
        strcmp(ATRINIK_BENCHMARK_DIRTY, "false") == 0) {
        fputs(ATRINIK_BENCHMARK_DIRTY, stdout);
    } else {
        gpu_player_view_json_string(ATRINIK_BENCHMARK_DIRTY);
    }
    printf(",\"manifest_sha256\":\"%s\",\"snapshot_sha256\":\"%s\","
           "\"pixels_sha256\":\"%s\",\"golden_verified\":%s,"
           "\"movement_lifecycle\":%s,\"logical_view\":[%u,%u],"
           "\"viewport\":[%u,%u],"
           "\"ui_pixels_sha256\":\"%s\","
           "\"assertions\":{"
           "\"ui_names_targets\":%s,\"visibility_fade\":%s,"
           "\"map_interaction\":%s,\"damage_animation\":%s,"
           "\"kill_animation\":%s,\"elevated_animation\":%s,"
           "\"layer_content_animation\":%s},"
           "\"timed_light_lifecycle\":%s,"
           "\"timed_endpoint_pixels_sha256\":\"%s\","
           "\"borrowed_temporal_light_samples\":%" PRIu64 ","
           "\"initial_pixels_sha256\":\"%s\",\"transition_pixels_sha256\":\"%s\","
           "\"initial_artifact\":\"%s\",\"initial_artifact_sha256\":\"%s\","
           "\"final_artifact\":\"%s\",\"final_artifact_sha256\":\"%s\","
           "\"archived_software_pixels_sha256\":\"%s\","
           "\"archived_software_ui_pixels_sha256\":\"%s\","
           "\"archived_software_lifecycle_sha256\":\"%s\","
           "\"backend\":",
           manifest->manifest_digest,
           manifest->snapshot_digest,
           pixels_digest,
           golden_verified ? "true" : "false",
           movement_lifecycle ? "true" : "false",
           manifest->look_width,
           manifest->look_height,
           manifest->viewport_width,
           manifest->viewport_height,
           ui_pixels_digest,
           manifest->player_names && manifest->target_ui ? "true" : "false",
           manifest->visibility_fade_test ? "true" : "false",
           manifest->map_interaction_test ? "true" : "false",
           manifest->damage_animation ? "true" : "false",
           manifest->kill_animation ? "true" : "false",
           manifest->animation_elevated ? "true" : "false",
           manifest->animation_layer_content ? "true" : "false",
           timed_endpoint_digest[0] != '\0' ? "true" : "false",
           timed_endpoint_digest,
           borrowed_temporal_samples,
           initial_digest,
           transition_digest,
           initial_artifact,
           initial_artifact_digest,
           final_artifact,
           final_artifact_digest,
           manifest->archived_software_pixels_digest,
           manifest->archived_software_ui_pixels_digest,
           movement_lifecycle ? manifest->expected_standard_checkpoint_digest : "");
    gpu_player_view_json_string(gpu_renderer_backend());
    fputs(",\"device\":", stdout);
    gpu_player_view_json_string(gpu_renderer_device_name());
    fputs(",\"driver_name\":", stdout);
    gpu_player_view_json_string(gpu_renderer_driver_name());
    fputs(",\"driver_version\":", stdout);
    gpu_player_view_json_string(gpu_renderer_driver_version());
    fputs(",\"hardware_tier\":", stdout);
    gpu_player_view_json_string(gpu_player_view_hardware_tier());
    fputs(",\"build\":{\"type\":", stdout);
    gpu_player_view_json_string(ATRINIK_BUILD_TYPE);
    fputs(",\"compiler\":", stdout);
    gpu_player_view_json_string(ATRINIK_COMPILER_ID " " ATRINIK_COMPILER_VERSION);
    fputs(",\"system\":", stdout);
    gpu_player_view_json_string(ATRINIK_SYSTEM_NAME);
    printf("},\"qualified_hardware\":%s,\"ui_closure\":",
           gpu_renderer_hardware_verified() ? "true" : "false");
    gpu_player_view_ui_closure_write();
    fputs("}\n", stdout);
}

typedef struct gpu_player_view_workload {
    const char *name;
    uint32_t logical_size;
    uint32_t width;
    uint32_t height;
    uint8_t active_depths;
    bool animation_only;
} gpu_player_view_workload_t;

static const gpu_player_view_workload_t gpu_player_view_workloads[] = {
    {"dense-17x17-five-depth-1080p", 17, 1920, 1080, 5, false},
    {"dense-25x25-seven-depth-1440p", 25, 2560, 1440, 7, false},
    {"wire-ceiling-28x28-thirteen-depth-1440p", 28, 2560, 1440, 13, false},
    {"wire-ceiling-28x28-thirteen-depth-4k", 28, 3840, 2160, 13, false},
    {"actor-door-roof-animation-25x25", 25, 2560, 1440, 7, true},
};

static int gpu_player_view_uint64_compare(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static uint64_t gpu_player_view_percentile(const uint64_t *samples, unsigned int percentile) {
    uint64_t sorted[PLAYER_VIEW_BENCHMARK_ITERATIONS];
    memcpy(sorted, samples, sizeof(sorted));
    qsort(sorted, arraysize(sorted), sizeof(*sorted), gpu_player_view_uint64_compare);
    size_t rank = (arraysize(sorted) * percentile + 99U) / 100U;
    return sorted[MAX(rank, 1U) - 1U];
}

static const gpu_player_view_workload_t *gpu_player_view_workload_find(const char *name) {
    for (size_t index = 0; index < arraysize(gpu_player_view_workloads); index++) {
        if (strcmp(name, gpu_player_view_workloads[index].name) == 0) {
            return &gpu_player_view_workloads[index];
        }
    }
    return NULL;
}

static unsigned int gpu_player_view_depth_mask_count(uint16_t mask) {
    unsigned int count = 0;
    while (mask != 0) {
        count += mask & 1U;
        mask >>= 1U;
    }
    return count;
}

static bool gpu_player_view_benchmark(const player_view_manifest_t *manifest,
                                      widgetdata *map_widget,
                                      widgetdata *minimap_widget,
                                      const gpu_player_view_workload_t *workload) {
    unsigned int active_depths = map_active_level_count();
    int window_width, window_height, output_width, output_height;
    if (!SDL_GetWindowSize(ScreenWindow, &window_width, &window_height) ||
        !gpu_renderer_output_size(&output_width, &output_height) ||
        window_width != (int)workload->width || window_height != (int)workload->height ||
        output_width != (int)workload->width || output_height != (int)workload->height) {
        SDL_SetError("production benchmark does not have its required 1:1 pixel output");
        return false;
    }
    if (manifest->look_width != workload->logical_size ||
        manifest->look_height != workload->logical_size ||
        manifest->viewport_width != workload->width ||
        manifest->viewport_height != workload->height || active_depths != workload->active_depths ||
        !manifest->widget_render) {
        SDL_SetError("production benchmark manifest does not match its workload row");
        return false;
    }

    gpu_renderer_statistics_reset();
    map_benchmark_statistics_reset();
    for (unsigned int frame = 0; frame < PLAYER_VIEW_BENCHMARK_WARMUPS; frame++) {
        if (workload->animation_only) {
            LastTick += PLAYER_VIEW_MOVEMENT_TICK_MS;
            animate_objects();
            map_animate();
        } else {
            map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
        }
        if (!gpu_player_view_render_complete()) {
            return false;
        }
    }
    gpu_renderer_statistics_t cold;
    gpu_renderer_statistics_get(&cold);

    gpu_renderer_statistics_reset();
    map_benchmark_statistics_reset();
    uint64_t frame_samples[PLAYER_VIEW_BENCHMARK_ITERATIONS];
    uint64_t stage_samples[GPU_RENDERER_TIMING_NUM][PLAYER_VIEW_BENCHMARK_ITERATIONS];
    for (unsigned int frame = 0; frame < PLAYER_VIEW_BENCHMARK_ITERATIONS; frame++) {
        uint64_t started = SDL_GetTicksNS();
        if (workload->animation_only) {
            LastTick += PLAYER_VIEW_MOVEMENT_TICK_MS;
            animate_objects();
            map_animate();
        } else {
            map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
        }
        if (frame % 10U == 0) {
            widget_minimap_refresh_test(minimap_widget);
        }
        gpu_renderer_statistics_t before;
        gpu_renderer_statistics_t after;
        gpu_renderer_statistics_get(&before);
        if (!gpu_player_view_render_complete()) {
            return false;
        }
        frame_samples[frame] = SDL_GetTicksNS() - started;
        gpu_renderer_statistics_get(&after);
        for (size_t stage = 0; stage < GPU_RENDERER_TIMING_NUM; stage++) {
            stage_samples[stage][frame] =
                after.timings[stage].elapsed_ns - before.timings[stage].elapsed_ns;
        }
    }
    gpu_renderer_statistics_t measured;
    map_benchmark_statistics_t map_measured;
    gpu_renderer_statistics_get(&measured);
    map_benchmark_statistics_get(&map_measured);
    bool animation_submission_verified =
        !workload->animation_only ||
        (map_measured.animation_draws == PLAYER_VIEW_BENCHMARK_ITERATIONS &&
         map_measured.animation_reason_draws == PLAYER_VIEW_BENCHMARK_ITERATIONS &&
         map_measured.animation_command_transitions > 0 &&
         map_measured.reused_render_commands > map_measured.compiled_render_commands &&
         map_measured.door_commands > 0 && map_measured.roof_commands > 0 &&
         map_measured.primary_frames_with_door == PLAYER_VIEW_BENCHMARK_ITERATIONS &&
         map_measured.primary_frames_with_roof == PLAYER_VIEW_BENCHMARK_ITERATIONS &&
         map_measured.door_depth_mask != 0 &&
         gpu_player_view_depth_mask_count(map_measured.roof_depth_mask) == workload->active_depths);
    bool instance_uploads_verified =
        workload->animation_only
            ? measured.instance_upload_count <= PLAYER_VIEW_BENCHMARK_ITERATIONS * 64U &&
                  measured.instance_upload_bytes <= PLAYER_VIEW_BENCHMARK_ITERATIONS * 4096U
            : measured.instance_upload_count == 0 && measured.instance_upload_bytes == 0;
    uint64_t completed_maps = map_measured.primary_map_draws + map_measured.auxiliary_map_draws;
    bool slot_uniform_uploads_verified =
        measured.batches >= completed_maps &&
        measured.slot_uniform_upload_count == measured.batches - completed_maps &&
        gpu_player_view_slot_uniform_uploads_bounded(&measured);
    if (map_measured.primary_map_draws != PLAYER_VIEW_BENCHMARK_ITERATIONS ||
        map_measured.auxiliary_map_draws != PLAYER_VIEW_BENCHMARK_ITERATIONS / 10U ||
        map_measured.stretched_commands == 0 || map_measured.double_commands == 0 ||
        map_measured.living_commands < UINT64_C(64) * PLAYER_VIEW_BENCHMARK_ITERATIONS ||
        map_measured.primary_frames_with_stretch != PLAYER_VIEW_BENCHMARK_ITERATIONS ||
        map_measured.primary_frames_with_living != PLAYER_VIEW_BENCHMARK_ITERATIONS ||
        gpu_player_view_depth_mask_count(map_measured.command_depth_mask) !=
            workload->active_depths ||
        !animation_submission_verified || measured.source_upload_count != 0 ||
        measured.source_upload_bytes != 0 || measured.light_upload_count != 0 ||
        measured.light_upload_bytes != 0 || !slot_uniform_uploads_verified ||
        measured.upload_count !=
            measured.instance_upload_count + measured.slot_uniform_upload_count ||
        measured.upload_bytes !=
            measured.instance_upload_bytes + measured.slot_uniform_upload_bytes ||
        !instance_uploads_verified || measured.resource_creations != 0 ||
        measured.resource_destructions != 0 || measured.readbacks != 0 || measured.fallbacks != 0) {
        SDL_SetError("production benchmark did not reach its retained stretch/actor plateau");
        return false;
    }

    char animation_checkpoints[3][PLAYER_VIEW_SHA256_HEX_SIZE] = {{0}};
    bool animation_path_verified = false;
    if (workload->animation_only) {
        for (size_t index = 0; index < arraysize(animation_checkpoints); index++) {
            LastTick += PLAYER_VIEW_MOVEMENT_TICK_MS;
            animate_objects();
            map_animate();
            if (!gpu_player_view_render_complete() ||
                !gpu_player_view_checkpoint(animation_checkpoints[index])) {
                return false;
            }
        }
        animation_path_verified =
            animation_submission_verified &&
            (strcmp(animation_checkpoints[0], animation_checkpoints[1]) != 0 ||
             strcmp(animation_checkpoints[1], animation_checkpoints[2]) != 0);
        if (!animation_path_verified) {
            SDL_SetError("door/roof/actor animation workload produced no changing frame");
            return false;
        }
    }
    char checkpoint[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!gpu_player_view_checkpoint(checkpoint)) {
        return false;
    }
    const char *output_path =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_OUTPUT");
    FILE *output = output_path != NULL && *output_path != '\0' ? fopen(output_path, "w") : stdout;
    if (output == NULL) {
        return false;
    }
    static const char *stage_names[GPU_RENDERER_TIMING_NUM] = {
        "command_build",
        "albedo_owner",
        "light_tone",
        "ui",
        "cpu_submission",
        "gpu_completion_wait",
        "present_wait",
    };
    fputs("{\"schema_version\":3,\"benchmark\":\"gpu-interop-stress-qualification\","
          "\"revision\":",
          output);
    gpu_player_view_json_string_to(output, ATRINIK_BENCHMARK_REVISION);
    fputs(",\"dirty\":", output);
    if (strcmp(ATRINIK_BENCHMARK_DIRTY, "false") == 0 ||
        strcmp(ATRINIK_BENCHMARK_DIRTY, "true") == 0) {
        fputs(ATRINIK_BENCHMARK_DIRTY, output);
    } else {
        fputc('"', output);
        fputs(ATRINIK_BENCHMARK_DIRTY, output);
        fputc('"', output);
    }
    fputs(",\"gpu\":{\"backend\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_backend());
    fputs(",\"device\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_device_name());
    fputs(",\"driver_name\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_driver_name());
    fputs(",\"driver_version\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_driver_version());
    fputs(",\"hardware_tier\":", output);
    gpu_player_view_json_string_to(output, gpu_player_view_hardware_tier());
    fprintf(output,
            ",\"qualified_hardware\":%s},\"build\":{\"type\":",
            gpu_renderer_hardware_verified() ? "true" : "false");
    gpu_player_view_json_string_to(output, ATRINIK_BUILD_TYPE);
    fputs(",\"compiler\":", output);
    gpu_player_view_json_string_to(output, ATRINIK_COMPILER_ID " " ATRINIK_COMPILER_VERSION);
    fputs(",\"system\":", output);
    gpu_player_view_json_string_to(output, ATRINIK_SYSTEM_NAME);
    fprintf(output,
            "},\"shader_cohort\":\"%s\","
            "\"workload\":{\"name\":\"%s\",\"logical_view\":%u,"
            "\"window_logical_width\":%u,\"window_logical_height\":%u,"
            "\"render_output_width\":%u,\"render_output_height\":%u,"
            "\"active_depths\":%u,\"animation_only\":%s,\"actors\":64,"
            "\"production_path\":true,\"stretch_exercised\":true,"
            "\"animation_path_verified\":%s,"
            "\"redraw_reason_frames\":%" PRIu64 ","
            "\"animated_command_transitions\":%" PRIu64 ","
            "\"command_depth_mask\":%u,\"living_depth_mask\":%u,"
            "\"door_depth_mask\":%u,\"roof_depth_mask\":%u,"
            "\"frames_with_stretch\":%" PRIu64 ","
            "\"frames_with_living\":%" PRIu64 ","
            "\"frames_with_door\":%" PRIu64 ","
            "\"frames_with_roof\":%" PRIu64 "},"
            "\"cold\":{\"scope\":\"fresh-process\",\"uploads\":%" PRIu64
            ",\"upload_bytes\":%" PRIu64 ",\"source_uploads\":%" PRIu64
            ",\"source_upload_bytes\":%" PRIu64 ",\"instance_uploads\":%" PRIu64
            ",\"instance_upload_bytes\":%" PRIu64 ",\"light_uploads\":%" PRIu64
            ",\"light_upload_bytes\":%" PRIu64 ",\"slot_uniform_uploads\":%" PRIu64
            ",\"slot_uniform_upload_bytes\":%" PRIu64 ",\"resource_creations\":%" PRIu64
            ",\"peak_retained_bytes\":%" PRIu64 "},\"frame_windows_ns\":[",
            ATRINIK_GPU_SHADER_COHORT,
            workload->name,
            workload->logical_size,
            (uint32_t)window_width,
            (uint32_t)window_height,
            (uint32_t)output_width,
            (uint32_t)output_height,
            workload->active_depths,
            workload->animation_only ? "true" : "false",
            animation_path_verified ? "true" : "false",
            map_measured.animation_reason_draws,
            map_measured.animation_command_transitions,
            map_measured.command_depth_mask,
            map_measured.living_depth_mask,
            map_measured.door_depth_mask,
            map_measured.roof_depth_mask,
            map_measured.primary_frames_with_stretch,
            map_measured.primary_frames_with_living,
            map_measured.primary_frames_with_door,
            map_measured.primary_frames_with_roof,
            cold.upload_count,
            cold.upload_bytes,
            cold.source_upload_count,
            cold.source_upload_bytes,
            cold.instance_upload_count,
            cold.instance_upload_bytes,
            cold.light_upload_count,
            cold.light_upload_bytes,
            cold.slot_uniform_upload_count,
            cold.slot_uniform_upload_bytes,
            cold.resource_creations,
            cold.peak_retained_bytes);
    for (size_t sample = 0; sample < arraysize(frame_samples); sample++) {
        fprintf(output, "%s%" PRIu64, sample == 0 ? "" : ",", frame_samples[sample]);
    }
    fprintf(output,
            "],\"frame_ns\":{\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64
            "},\"stretch_frame_windows_ns\":[",
            gpu_player_view_percentile(frame_samples, 95),
            gpu_player_view_percentile(frame_samples, 99),
            gpu_player_view_percentile(frame_samples, 100));
    for (size_t sample = 0; sample < arraysize(frame_samples); sample++) {
        fprintf(output, "%s%" PRIu64, sample == 0 ? "" : ",", frame_samples[sample]);
    }
    fputs("],\"stage_windows_ns\":{", output);
    for (size_t stage = 0; stage < GPU_RENDERER_TIMING_NUM; stage++) {
        fprintf(output, "%s\"%s\":[", stage == 0 ? "" : ",", stage_names[stage]);
        for (size_t sample = 0; sample < PLAYER_VIEW_BENCHMARK_ITERATIONS; sample++) {
            fprintf(output, "%s%" PRIu64, sample == 0 ? "" : ",", stage_samples[stage][sample]);
        }
        fputc(']', output);
    }
    fprintf(output,
            "},\"checkpoint\":{\"algorithm\":\"sha256-rgba32-with-dimensions\","
            "\"timed\":false,\"pixels_sha256\":\"%s\","
            "\"animation_pixels_sha256\":[\"%s\",\"%s\",\"%s\"]},"
            "\"steady_state\":{"
            "\"uploads\":%" PRIu64 ",\"upload_bytes\":%" PRIu64 ",\"source_uploads\":%" PRIu64
            ",\"source_upload_bytes\":%" PRIu64 ",\"instance_uploads\":%" PRIu64
            ",\"instance_upload_bytes\":%" PRIu64 ",\"light_uploads\":%" PRIu64
            ",\"light_upload_bytes\":%" PRIu64 ",\"slot_uniform_uploads\":%" PRIu64
            ",\"slot_uniform_upload_bytes\":%" PRIu64 ",\"resource_creations\":%" PRIu64
            ",\"resource_destructions\":%" PRIu64 ",\"readbacks\":%" PRIu64 ",\"commands\":%" PRIu64
            ",\"batches\":%" PRIu64 ",\"draws\":%" PRIu64 ",\"retained_bytes\":%" PRIu64
            ",\"peak_retained_bytes\":%" PRIu64 ",\"fallbacks\":%" PRIu64 "}}\n",
            checkpoint,
            animation_checkpoints[0],
            animation_checkpoints[1],
            animation_checkpoints[2],
            measured.upload_count,
            measured.upload_bytes,
            measured.source_upload_count,
            measured.source_upload_bytes,
            measured.instance_upload_count,
            measured.instance_upload_bytes,
            measured.light_upload_count,
            measured.light_upload_bytes,
            measured.slot_uniform_upload_count,
            measured.slot_uniform_upload_bytes,
            measured.resource_creations,
            measured.resource_destructions,
            measured.readbacks,
            measured.commands,
            measured.batches,
            measured.draws,
            measured.retained_bytes,
            measured.peak_retained_bytes,
            measured.fallbacks);
    return output == stdout || fclose(output) == 0;
}

typedef struct gpu_player_view_lifecycle_event {
    const char *name;
    unsigned int recovery_attempts;
    bool fullscreen;
    bool action_asynchronous;
    uint64_t action_readbacks;
    uint64_t action_duration_ns;
    gpu_renderer_statistics_t action;
    uint64_t frame_samples[PLAYER_VIEW_BENCHMARK_ITERATIONS];
    gpu_renderer_statistics_t steady;
    int output_width;
    int output_height;
    int display_mode_width;
    int display_mode_height;
    char checkpoint[PLAYER_VIEW_SHA256_HEX_SIZE];
    char artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE];
    char artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
} gpu_player_view_lifecycle_event_t;

static uint64_t gpu_player_view_lifecycle_action_begin(void) {
    (void)gpu_renderer_recreation_take_request();
    gpu_renderer_statistics_reset();
    return SDL_GetTicksNS();
}

static void gpu_player_view_lifecycle_action_end(gpu_player_view_lifecycle_event_t *event,
                                                 uint64_t started) {
    event->action_duration_ns = SDL_GetTicksNS() - started;
    gpu_renderer_statistics_get(&event->action);
}

static bool gpu_player_view_recovery_apply(void *userdata) {
    (void)userdata;
    return resize_window_recovery_apply();
}

static bool gpu_player_view_recovery_republish(void *userdata) {
    (void)userdata;
    if (!client_command_retry_deferred() || !connection_preference_recover()) {
        return false;
    }
    map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
    minimap_redraw_force();
    widget_redraw_everything();
    popup_redraw_all();
    return gpu_player_view_render_complete();
}

static bool gpu_player_view_lifecycle_sustain(gpu_player_view_lifecycle_event_t *event) {
    gpu_renderer_statistics_reset();
    map_benchmark_statistics_reset();
    for (unsigned int frame = 0; frame < PLAYER_VIEW_BENCHMARK_ITERATIONS; frame++) {
        uint64_t started = SDL_GetTicksNS();
        map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
        if (frame % 10U == 0 && cur_widget[MINIMAP_ID] != NULL) {
            widget_minimap_refresh_test(cur_widget[MINIMAP_ID]);
        }
        if (!gpu_player_view_render_complete()) {
            return false;
        }
        event->frame_samples[frame] = SDL_GetTicksNS() - started;
    }
    gpu_renderer_statistics_get(&event->steady);
    if (event->steady.upload_count != event->steady.slot_uniform_upload_count ||
        event->steady.upload_bytes != event->steady.slot_uniform_upload_bytes ||
        !gpu_player_view_slot_uniform_uploads_bounded(&event->steady) ||
        event->steady.slot_uniform_upload_count > event->steady.batches ||
        event->steady.resource_creations != 0 || event->steady.resource_destructions != 0 ||
        event->steady.readbacks != 0 || event->steady.fallbacks != 0 ||
        event->steady.commands == 0 || event->steady.batches == 0 || event->steady.draws == 0) {
        SDL_SetError("production lifecycle did not return to a retained steady state");
        return false;
    }
    return gpu_renderer_output_size(&event->output_width, &event->output_height) &&
           gpu_player_view_checkpoint_named(event->name,
                                            event->checkpoint,
                                            event->artifact,
                                            event->artifact_digest);
}

static bool gpu_player_view_lifecycle_recover(SDL_Window *window,
                                              gpu_player_view_lifecycle_event_t *event) {
    return gpu_renderer_recover_and_republish(window,
                                              &event->recovery_attempts,
                                              !gpu_player_view_qualified(),
                                              gpu_player_view_recovery_apply,
                                              gpu_player_view_recovery_republish,
                                              NULL) &&
           event->recovery_attempts == 1U;
}

static bool gpu_player_view_recover_once(SDL_Window *window) {
    unsigned int attempts = 0;
    return gpu_renderer_recover_and_republish(window,
                                              &attempts,
                                              !gpu_player_view_qualified(),
                                              gpu_player_view_recovery_apply,
                                              gpu_player_view_recovery_republish,
                                              NULL) &&
           attempts == 1U;
}

static bool gpu_player_view_lifecycle_window_event(SDL_Window *window,
                                                   gpu_player_view_lifecycle_event_t *event) {
    (void)window;
    uint64_t deadline = SDL_GetTicks() + 5000U;
    while (SDL_GetTicks() <= deadline) {
        Event_PollInputDevice();
        if (gpu_renderer_recreation_take_request()) {
            return gpu_player_view_lifecycle_recover(window, event);
        }
        SDL_Delay(1);
    }
    SDL_SetError("real production window transition did not request GPU reconstruction");
    return false;
}

static bool gpu_player_view_lifecycle_write(
    const gpu_player_view_lifecycle_event_t events[PLAYER_VIEW_LIFECYCLE_EVENTS],
    const player_view_manifest_t *manifest,
    const char *checkpoint) {
    const char *output_path =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                   "ATRINIK_GPU_CONFORMANCE_LIFECYCLE_OUTPUT");
    FILE *output = output_path != NULL && *output_path != '\0' ? fopen(output_path, "w") : stdout;
    if (output == NULL) {
        return false;
    }
    fprintf(output,
            "{\"schema_version\":2,\"benchmark\":\"gpu-production-recovery-lifecycle\","
            "\"fixture\":\"brynknot-movement\",\"manifest_sha256\":\"%s\","
            "\"viewport\":[%u,%u],\"resize_delta\":[%u,%u],\"revision\":",
            manifest->manifest_digest,
            manifest->viewport_width,
            manifest->viewport_height,
            manifest->resize_width_delta,
            manifest->resize_height_delta);
    gpu_player_view_json_string_to(output, ATRINIK_BENCHMARK_REVISION);
    fprintf(output, ",\"dirty\":%s,\"build\":{\"type\":", ATRINIK_BENCHMARK_DIRTY);
    gpu_player_view_json_string_to(output, ATRINIK_BUILD_TYPE);
    fputs(",\"compiler\":", output);
    gpu_player_view_json_string_to(output, ATRINIK_COMPILER_ID " " ATRINIK_COMPILER_VERSION);
    fputs(",\"system\":", output);
    gpu_player_view_json_string_to(output, ATRINIK_SYSTEM_NAME);
    fputs("},\"gpu\":{\"backend\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_backend());
    fputs(",\"device\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_device_name());
    fputs(",\"driver_name\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_driver_name());
    fputs(",\"driver_version\":", output);
    gpu_player_view_json_string_to(output, gpu_renderer_driver_version());
    fputs(",\"hardware_tier\":", output);
    gpu_player_view_json_string_to(output, gpu_player_view_hardware_tier());
    fprintf(output,
            ",\"qualified_hardware\":%s},\"shader_cohort\":\"%s\","
            "\"production_path\":true,\"sustained_frames_per_event\":%u,\"events\":[",
            gpu_renderer_hardware_verified() ? "true" : "false",
            ATRINIK_GPU_SHADER_COHORT,
            PLAYER_VIEW_BENCHMARK_ITERATIONS);
    for (size_t index = 0; index < PLAYER_VIEW_LIFECYCLE_EVENTS; index++) {
        const gpu_player_view_lifecycle_event_t *event = &events[index];
        fprintf(output,
                "%s{\"name\":\"%s\",\"recovery_attempts\":%u,\"fullscreen\":%s,"
                "\"action\":{\"asynchronous\":%s,\"duration_ns\":%" PRIu64 ",\"uploads\":%" PRIu64
                ",\"upload_bytes\":%" PRIu64 ",\"source_uploads\":%" PRIu64
                ",\"source_upload_bytes\":%" PRIu64 ",\"instance_uploads\":%" PRIu64
                ",\"instance_upload_bytes\":%" PRIu64 ",\"light_uploads\":%" PRIu64
                ",\"light_upload_bytes\":%" PRIu64 ",\"slot_uniform_uploads\":%" PRIu64
                ",\"slot_uniform_upload_bytes\":%" PRIu64 ",\"resource_creations\":%" PRIu64
                ",\"resource_destructions\":%" PRIu64 ",\"device_recoveries\":%" PRIu64
                ",\"recovery_failures\":%" PRIu64 ",\"readbacks\":%" PRIu64
                ",\"fallbacks\":%" PRIu64 "},"
                "\"output_size\":[%d,%d],\"display_mode_size\":[%d,%d],"
                "\"pixels_sha256\":\"%s\",\"artifact\":\"%s\","
                "\"artifact_sha256\":\"%s\","
                "\"frame_windows_ns\":[",
                index == 0 ? "" : ",",
                event->name,
                event->recovery_attempts,
                event->fullscreen ? "true" : "false",
                event->action_asynchronous ? "true" : "false",
                event->action_duration_ns,
                event->action.upload_count,
                event->action.upload_bytes,
                event->action.source_upload_count,
                event->action.source_upload_bytes,
                event->action.instance_upload_count,
                event->action.instance_upload_bytes,
                event->action.light_upload_count,
                event->action.light_upload_bytes,
                event->action.slot_uniform_upload_count,
                event->action.slot_uniform_upload_bytes,
                event->action.resource_creations,
                event->action.resource_destructions,
                event->action.device_recoveries,
                event->action.recovery_failures,
                event->action_readbacks,
                event->action.fallbacks,
                event->output_width,
                event->output_height,
                event->display_mode_width,
                event->display_mode_height,
                event->checkpoint,
                event->artifact,
                event->artifact_digest);
        for (size_t frame = 0; frame < PLAYER_VIEW_BENCHMARK_ITERATIONS; frame++) {
            fprintf(output, "%s%" PRIu64, frame == 0 ? "" : ",", event->frame_samples[frame]);
        }
        fprintf(output,
                "],\"frame_ns\":{\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64
                "},\"steady_state\":{\"uploads\":%" PRIu64 ",\"upload_bytes\":%" PRIu64
                ",\"slot_uniform_uploads\":%" PRIu64 ",\"slot_uniform_upload_bytes\":%" PRIu64
                ",\"resource_creations\":%" PRIu64 ",\"resource_destructions\":%" PRIu64
                ",\"readbacks\":%" PRIu64 ",\"commands\":%" PRIu64 ",\"batches\":%" PRIu64
                ",\"draws\":%" PRIu64 ",\"retained_bytes\":%" PRIu64 ",\"fallbacks\":%" PRIu64 "}}",
                gpu_player_view_percentile(event->frame_samples, 95),
                gpu_player_view_percentile(event->frame_samples, 99),
                gpu_player_view_percentile(event->frame_samples, 100),
                event->steady.upload_count,
                event->steady.upload_bytes,
                event->steady.slot_uniform_upload_count,
                event->steady.slot_uniform_upload_bytes,
                event->steady.resource_creations,
                event->steady.resource_destructions,
                event->steady.readbacks,
                event->steady.commands,
                event->steady.batches,
                event->steady.draws,
                event->steady.retained_bytes,
                event->steady.fallbacks);
    }
    fprintf(output,
            "],\"final_checkpoint\":{\"algorithm\":\"sha256-rgba32-with-dimensions\","
            "\"pixels_sha256\":\"%s\"}}\n",
            checkpoint);
    return output == stdout || fclose(output) == 0;
}

static bool gpu_player_view_lifecycle(SDL_Window *window,
                                      const player_view_manifest_t *manifest,
                                      uint8_t *snapshot,
                                      size_t snapshot_size,
                                      uint8_t *transition_snapshot,
                                      size_t transition_snapshot_size,
                                      uint64_t cold_duration_ns,
                                      const gpu_renderer_statistics_t *cold_statistics) {
    static const char *names[PLAYER_VIEW_LIFECYCLE_EVENTS] = {
        "cold_asset_upload",
        "resize_grow",
        "resize_restore",
        "teleport",
        "reconnect",
        "foreground_resume",
        "fullscreen_enter",
        "fullscreen_leave",
        "display_migration",
        "swapchain_recreation",
        "device_loss",
        "screenshot_readback",
    };
    gpu_player_view_lifecycle_event_t events[PLAYER_VIEW_LIFECYCLE_EVENTS] = {0};
    for (size_t index = 0; index < arraysize(events); index++) {
        events[index].name = names[index];
    }
    events[0].action_duration_ns = cold_duration_ns;
    events[0].action = *cold_statistics;
    if (events[0].action_duration_ns == 0 || events[0].action.upload_count == 0 ||
        events[0].action.resource_creations == 0 ||
        !gpu_player_view_lifecycle_sustain(&events[0])) {
        return false;
    }
    uint64_t action_started = gpu_player_view_lifecycle_action_begin();
    if (!SDL_SetWindowSize(window,
                           (int)(manifest->viewport_width + manifest->resize_width_delta),
                           (int)(manifest->viewport_height + manifest->resize_height_delta)) ||
        !SDL_SyncWindow(window) || !gpu_player_view_lifecycle_window_event(window, &events[1])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[1], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[1])) {
        return false;
    }

    action_started = gpu_player_view_lifecycle_action_begin();
    if (!SDL_SetWindowSize(window, (int)manifest->viewport_width, (int)manifest->viewport_height) ||
        !SDL_SyncWindow(window) || !gpu_player_view_lifecycle_window_event(window, &events[2])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[2], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[2]) || transition_snapshot == NULL) {
        return false;
    }

    action_started = gpu_player_view_lifecycle_action_begin();
    socket_command_map(transition_snapshot, transition_snapshot_size, 0);
    if (!gpu_player_view_render_complete()) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[3], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[3])) {
        return false;
    }

    action_started = gpu_player_view_lifecycle_action_begin();
    cpl.state = ST_START;
    popup_destroy_all();
    effect_stop();
    clear_map(true);
    resources_reload();
    map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
    minimap_redraw_flag = 1;
    if (!gpu_player_view_render_complete()) {
        return false;
    }
    if (selected_server == NULL) {
        selected_server = metaserver_add("fixture.invalid",
                                         13327,
                                         "Frozen GPU Qualification",
                                         PACKAGE_VERSION,
                                         "Immutable reconnect fixture server");
    }
    login_start();
    cpl.state = ST_LOGIN;
    if (!gpu_player_view_render_complete()) {
        return false;
    }
    popup_destroy_all();
    cpl.state = ST_PLAY;
    socket_command_map(snapshot, snapshot_size, 0);
    if (!gpu_player_view_render_complete()) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[4], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[4])) {
        return false;
    }

    action_started = gpu_player_view_lifecycle_action_begin();
    if (!SDL_MinimizeWindow(window) || !SDL_SyncWindow(window) || !SDL_RestoreWindow(window) ||
        !SDL_SyncWindow(window) || !gpu_player_view_lifecycle_window_event(window, &events[5])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[5], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[5])) {
        return false;
    }

    action_started = gpu_player_view_lifecycle_action_begin();
    if (!SDL_SetWindowFullscreen(window, true) || !SDL_SyncWindow(window) ||
        !gpu_player_view_lifecycle_window_event(window, &events[6]) ||
        !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[6], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[6])) {
        return false;
    }
    events[6].fullscreen = true;
    const SDL_DisplayMode *display_mode =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (display_mode == NULL) {
        return false;
    }
    events[6].display_mode_width = display_mode->w;
    events[6].display_mode_height = display_mode->h;
    if (events[6].output_width != events[6].display_mode_width ||
        events[6].output_height != events[6].display_mode_height) {
        SDL_SetError("fullscreen output does not match the active display mode");
        return false;
    }
    action_started = gpu_player_view_lifecycle_action_begin();
    if (!SDL_SetWindowFullscreen(window, false) || !SDL_SyncWindow(window) ||
        !gpu_player_view_lifecycle_window_event(window, &events[7]) ||
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[7], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[7])) {
        return false;
    }
    int display_count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&display_count);
    SDL_DisplayID original_display = SDL_GetDisplayForWindow(window);
    SDL_DisplayID target_display = 0;
    for (int index = 0; displays != NULL && index < display_count; index++) {
        if (displays[index] != original_display) {
            target_display = displays[index];
            break;
        }
    }
    if (displays == NULL || target_display == 0) {
        SDL_free(displays);
        SDL_SetError("qualified display migration requires a distinct target display");
        return false;
    }
    action_started = gpu_player_view_lifecycle_action_begin();
    SDL_SetWindowPosition(window,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(target_display),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(target_display));
    SDL_free(displays);
    if (!SDL_SyncWindow(window) || SDL_GetDisplayForWindow(window) != target_display ||
        !gpu_player_view_lifecycle_window_event(window, &events[8])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[8], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[8])) {
        return false;
    }
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    action_started = gpu_player_view_lifecycle_action_begin();
    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_SWAPCHAIN);
    if (gpu_player_view_render_complete() ||
        !gpu_player_view_lifecycle_recover(window, &events[9])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[9], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[9])) {
        return false;
    }
    action_started = gpu_player_view_lifecycle_action_begin();
    gpu_renderer_conformance_fault_set(GPU_RENDERER_CONFORMANCE_FAULT_DEVICE_LOSS);
    if (gpu_player_view_render_complete() ||
        !gpu_player_view_lifecycle_recover(window, &events[10])) {
        return false;
    }
    gpu_player_view_lifecycle_action_end(&events[10], action_started);
    if (!gpu_player_view_lifecycle_sustain(&events[10])) {
        return false;
    }
    action_started = gpu_player_view_lifecycle_action_begin();
    screenshot_test_begin();
    if (client_command_check("/screenshot") != 1 || screenshot_test_take() != NULL ||
        !gpu_renderer_wait_idle()) {
        return false;
    }
    gpu_renderer_readback_poll();
    SDL_Surface *screenshot = screenshot_test_take();
    gpu_renderer_statistics_t screenshot_statistics;
    gpu_renderer_statistics_get(&screenshot_statistics);
    if (screenshot == NULL || screenshot->w != (int)manifest->viewport_width ||
        screenshot->h != (int)manifest->viewport_height || screenshot_statistics.readbacks != 1 ||
        screenshot_statistics.fallbacks != 0) {
        SDL_DestroySurface(screenshot);
        SDL_SetError("player-facing lifecycle screenshot did not complete asynchronously");
        return false;
    }
    SDL_DestroySurface(screenshot);
    events[11].action_asynchronous = true;
    gpu_player_view_lifecycle_action_end(&events[11], action_started);
    events[11].action_readbacks = events[11].action.readbacks;
#else
    return false;
#endif
    if (!gpu_player_view_lifecycle_sustain(&events[11])) {
        return false;
    }
    const char *baseline = events[0].checkpoint;
    if (events[1].output_width != (int)(manifest->viewport_width + manifest->resize_width_delta) ||
        events[1].output_height !=
            (int)(manifest->viewport_height + manifest->resize_height_delta) ||
        strcmp(events[2].checkpoint, baseline) != 0 ||
        strcmp(events[3].checkpoint, baseline) == 0 ||
        strcmp(events[4].checkpoint, baseline) != 0 ||
        strcmp(events[5].checkpoint, baseline) != 0 ||
        events[6].output_width != events[6].display_mode_width ||
        events[6].output_height != events[6].display_mode_height ||
        strcmp(events[7].checkpoint, baseline) != 0 ||
        strcmp(events[8].checkpoint, baseline) != 0 ||
        strcmp(events[9].checkpoint, baseline) != 0 ||
        strcmp(events[10].checkpoint, baseline) != 0 ||
        strcmp(events[11].checkpoint, baseline) != 0) {
        SDL_SetError("production lifecycle checkpoint did not preserve its expected scene");
        return false;
    }
    char checkpoint[PLAYER_VIEW_SHA256_HEX_SIZE];
    return gpu_player_view_checkpoint(checkpoint) && strcmp(checkpoint, baseline) == 0 &&
           gpu_player_view_lifecycle_write(events, manifest, checkpoint);
}

int gpu_player_view_main(int argc, char *argv[]) {
    bool benchmark_mode = argc == 3 && strcmp(argv[0], "--gpu-player-view-benchmark") == 0;
    bool lifecycle_mode = argc == 2 && strcmp(argv[0], "--gpu-player-view-lifecycle") == 0;
    const gpu_player_view_workload_t *workload =
        benchmark_mode ? gpu_player_view_workload_find(argv[2]) : NULL;
    if ((!benchmark_mode && !lifecycle_mode &&
         (argc != 2 || strcmp(argv[0], "--gpu-player-view") != 0)) ||
        (benchmark_mode && workload == NULL)) {
        fprintf(stderr,
                "usage: atrinik --gpu-player-view MANIFEST | "
                "--gpu-player-view-benchmark MANIFEST WORKLOAD | "
                "--gpu-player-view-lifecycle MANIFEST\n");
        return 2;
    }

    player_view_manifest_t manifest;
    if (!player_view_manifest_parse(argv[1], &manifest) ||
        !player_view_file_sha256(argv[1], manifest.manifest_digest)) {
        return 3;
    }
    if (!player_view_inputs_verify(&manifest)) {
        player_view_manifest_free(&manifest);
        return 4;
    }
    const char *fixture = strrchr(argv[1], '/');
#ifdef WIN32
    const char *windows_fixture = strrchr(argv[1], '\\');
    if (windows_fixture != NULL && (fixture == NULL || windows_fixture > fixture)) {
        fixture = windows_fixture;
    }
#endif
    fixture = fixture != NULL ? fixture + 1 : argv[1];
    size_t fixture_length = strlen(fixture);
    if (fixture_length <= 4 || strcmp(fixture + fixture_length - 4, ".xml") != 0) {
        fprintf(stderr, "gpu-player-view: manifest must have a named XML fixture\n");
        player_view_manifest_free(&manifest);
        return 3;
    }
    char fixture_id[256];
    if (fixture_length - 4 >= sizeof(fixture_id)) {
        fprintf(stderr, "gpu-player-view: fixture identifier is too long\n");
        player_view_manifest_free(&manifest);
        return 3;
    }
    memcpy(fixture_id, fixture, fixture_length - 4);
    fixture_id[fixture_length - 4] = '\0';
    for (const unsigned char *cursor = (const unsigned char *)fixture_id; *cursor != '\0';
         cursor++) {
        if (!isalnum(*cursor) && *cursor != '-') {
            fprintf(stderr, "gpu-player-view: fixture identifier is not closed\n");
            player_view_manifest_free(&manifest);
            return 3;
        }
    }
    snprintf(VS(gpu_player_view_review_prefix), "%s", fixture_id);

    uint8_t *snapshot = NULL;
    size_t snapshot_size = 0;
    uint8_t *next_snapshot = NULL;
    size_t next_snapshot_size = 0;
    uint8_t *transition_snapshot = NULL;
    size_t transition_snapshot_size = 0;
    player_view_movement_fixture_t movement_fixture;
    bool movement_lifecycle = false;
    int wire_width = MAP_LOOK_TO_WIRE_SIZE(manifest.look_width);
    int wire_height = MAP_LOOK_TO_WIRE_SIZE(manifest.look_height);
    if (!player_view_snapshot_load(manifest.snapshot_path, &snapshot, &snapshot_size) ||
        snapshot[0] != MAP_UPDATE_CMD_NEW ||
        !map_protocol_validate(snapshot, snapshot_size, 0, wire_width, wire_height)) {
        fprintf(stderr, "gpu-player-view: malformed or incompatible MAP snapshot\n");
        free(snapshot);
        player_view_manifest_free(&manifest);
        return 5;
    }
    if (manifest.next_snapshot_path != NULL) {
        bool loaded = player_view_snapshot_load(manifest.next_snapshot_path,
                                                &next_snapshot,
                                                &next_snapshot_size);
        movement_lifecycle = loaded && manifest.transition_snapshot_path != NULL &&
                             player_view_movement_fixture_parse(next_snapshot,
                                                                next_snapshot_size,
                                                                wire_width,
                                                                wire_height,
                                                                &movement_fixture);
        bool ordinary =
            loaded && !movement_lifecycle &&
            map_protocol_validate(next_snapshot, next_snapshot_size, 0, wire_width, wire_height);
        if (!movement_lifecycle && !ordinary) {
            fprintf(stderr, "gpu-player-view: malformed or incompatible next MAP input\n");
            free(next_snapshot);
            free(snapshot);
            player_view_manifest_free(&manifest);
            return 5;
        }
    }
    if (movement_lifecycle && (!player_view_snapshot_load(manifest.transition_snapshot_path,
                                                          &transition_snapshot,
                                                          &transition_snapshot_size) ||
                               transition_snapshot[0] != MAP_UPDATE_CMD_NEW ||
                               !map_protocol_validate(transition_snapshot,
                                                      transition_snapshot_size,
                                                      0,
                                                      wire_width,
                                                      wire_height))) {
        fprintf(stderr, "gpu-player-view: malformed or incompatible transition MAP snapshot\n");
        free(transition_snapshot);
        free(next_snapshot);
        free(snapshot);
        player_view_manifest_free(&manifest);
        return 5;
    }

    memset(&cpl, 0, sizeof(cpl));
#ifdef ATRINIK_WIDGET_TESTS
    /* Establish the closed fixture identity before any subsystem can resolve
     * user-data, server, or per-player paths. Cleanup always disables it. */
    wrapper_test_user_data_isolated_set(true);
    widget_mplayer_test_isolated_set(true);
    widget_render_profiler_test_isolated_set(true);
    region_map_test_fow_persistence_set(false);
    snprintf(VS(cpl.account), "%s", "renderer-fixture");
    snprintf(VS(cpl.name), "%s", "Renderer Maintainer");
#endif
    bool settings_ready = settings_init_read_only(manifest.settings_path);
    bool sdl_ready = false;
    bool text_ready = false;
    bool texture_ready = false;
    bool widgets_ready = false;
    bool objects_ready = false;
    bool resources_ready = false;
    bool metaserver_ready = false;
    bool server_settings_ready = false;
    SDL_Window *window = NULL;
    int result = 6;
    if (gpu_player_view_qualified() && !gpu_player_view_identity_valid()) {
        fprintf(stderr, "gpu-player-view: qualified evidence requires an exact clean revision\n");
        goto cleanup;
    }
    if (!settings_ready) {
        fprintf(stderr, "gpu-player-view: cannot load immutable setting defaults\n");
        goto cleanup;
    }
    setting_set_int(OPT_CAT_MAP, OPT_MAP_WIDTH, manifest.look_width);
    setting_set_int(OPT_CAT_MAP, OPT_MAP_HEIGHT, manifest.look_height);
    setting_set_int(OPT_CAT_MAP, OPT_MAP_ZOOM, manifest.map_zoom);
    setting_set_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING, manifest.smooth_lighting);
    setting_set_int(OPT_CAT_MAP, OPT_PLAYER_NAMES, manifest.player_names ? 1 : 0);
    setting_set_int(OPT_CAT_CLIENT, OPT_ZOOM_FILTER, manifest.zoom_filter);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gpu-player-view: cannot initialize SDL video: %s\n", SDL_GetError());
        result = 77;
        goto cleanup;
    }
    sdl_ready = true;
    window = SDL_CreateWindow("Atrinik frozen GPU player view",
                              (int)manifest.viewport_width,
                              (int)manifest.viewport_height,
                              SDL_WINDOW_RESIZABLE | (lifecycle_mode ? 0 : SDL_WINDOW_HIDDEN));
    if (window == NULL) {
        fprintf(stderr, "gpu-player-view: cannot create hidden window: %s\n", SDL_GetError());
        result = 77;
        goto cleanup;
    }
    ScreenWindow = window;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    bool renderer_ready = gpu_player_view_qualified() ? gpu_renderer_create(window)
                                                      : gpu_renderer_create_conformance(window);
#else
    bool renderer_ready = gpu_renderer_create(window);
#endif
    if (!renderer_ready) {
        fprintf(stderr, "gpu-player-view: cannot create GPU renderer: %s\n", SDL_GetError());
        result = 77;
        goto cleanup;
    }
    int initial_window_width, initial_window_height, initial_output_width, initial_output_height;
    if (!SDL_GetWindowSize(window, &initial_window_width, &initial_window_height) ||
        !gpu_renderer_output_size(&initial_output_width, &initial_output_height) ||
        initial_window_width != (int)manifest.viewport_width ||
        initial_window_height != (int)manifest.viewport_height ||
        initial_output_width != initial_window_width ||
        initial_output_height != initial_window_height) {
        fprintf(stderr, "gpu-player-view: fixture requires a 1:1 logical/output pixel contract\n");
        goto cleanup;
    }
    if (gpu_player_view_qualified() && !gpu_renderer_hardware_verified()) {
        fprintf(stderr, "gpu-player-view: qualified evidence lacks hardware attestation\n");
        goto cleanup;
    }
    if (gpu_player_view_qualified() &&
        (strcmp(gpu_renderer_device_name(), "unavailable") == 0 ||
         strcmp(gpu_renderer_driver_name(), "unavailable") == 0 ||
         strcmp(gpu_renderer_driver_version(), "unavailable") == 0)) {
        fprintf(stderr, "gpu-player-view: qualified evidence lacks exact GPU identity\n");
        goto cleanup;
    }

    if (manifest.font_path != NULL) {
#ifdef ATRINIK_WIDGET_TESTS
        text_test_font_path_set(manifest.font_path);
#endif
    }
    if (manifest.mono_font_path != NULL) {
#ifdef ATRINIK_WIDGET_TESTS
        text_test_mono_font_path_set(manifest.mono_font_path);
#endif
    }
    /* Every replay loads the canonical text-bearing widget tree. SDL_ttf is
     * therefore part of the production path even when a fixture has no
     * map-specific font override. */
    text_init();
    text_ready = true;
#ifdef ATRINIK_WIDGET_TESTS
    if (!text_test_measurement_preserves_selection(FONT_ARIAL11)) {
        fprintf(stderr, "gpu-player-view: text measurement mutated an active root selection\n");
        goto cleanup;
    }
#endif
    sprite_init_system();
    texture_init();
    texture_ready = true;
    object_init();
    objects_ready = true;
    resources_init();
    resources_ready = true;
    metaserver_init();
    metaserver_ready = true;
    selected_server = metaserver_add("fixture.invalid",
                                     13327,
                                     "Frozen GPU Qualification",
                                     PACKAGE_VERSION,
                                     "Immutable offline fixture server");
    if (selected_server == NULL) {
        fprintf(stderr, "gpu-player-view: cannot initialize frozen server identity\n");
        goto cleanup;
    }
    if (manifest.target_ui) {
        cpl.target_code = CMD_TARGET_ENEMY;
        cpl.target_hp = 64;
        snprintf(VS(cpl.target_color), "%s", "ffffff");
        snprintf(VS(cpl.target_name), "%s", "Local Player");
    }
    memset(&MapData, 0, sizeof(MapData));
    memset(FaceList, 0, sizeof(FaceList));
    cpl.state = ST_PLAY;
#ifdef ATRINIK_WIDGET_TESTS
    client_ui_test_clock_set(manifest.clock_ms);
    server_settings_test_init();
    server_settings_ready = true;
    cpl.stats.level = 1;
    cpl.stats.hp = 75;
    cpl.stats.maxhp = 100;
    cpl.stats.sp = 60;
    cpl.stats.maxsp = 100;
    cpl.stats.food = 700;
    cpl.stats.exp = 175;
#endif
    toolkit_widget_fixture_init(manifest.interface_path, manifest.layout_path);
    widgets_ready = true;
    if (!load_mapdef_file(manifest.archdef_path) || !player_view_assets_load(&manifest) ||
        !player_view_animations_init(&manifest)) {
        fprintf(stderr, "gpu-player-view: cannot initialize frozen inputs\n");
        goto cleanup;
    }
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.kill_animation && (FaceList[PLAYER_VIEW_DEATH_FACE].sprite == NULL ||
                                    FaceList[PLAYER_VIEW_DEATH_FACE].sprite->bitmap == NULL)) {
        fprintf(stderr, "gpu-player-view: overlay fixture lacks the death texture\n");
        goto cleanup;
    }
    if (manifest.kill_animation) {
        widget_map_animation_test_death_texture_set(
            FaceList[PLAYER_VIEW_DEATH_FACE].sprite->bitmap);
    }
#endif

    widgetdata *map_widget = cur_widget[MAP_ID];
    widgetdata *minimap_widget = cur_widget[MINIMAP_ID];
    if (map_widget == NULL || minimap_widget == NULL) {
        fprintf(stderr, "gpu-player-view: complete production widget tree is incomplete\n");
        goto cleanup;
    }
    map_widget->x = (int)manifest.widget_x;
    map_widget->y = (int)manifest.widget_y;
    map_widget->w = (int)manifest.viewport_width;
    map_widget->h = (int)manifest.viewport_height;
    minimap_widget->x = MAX(8, (int)manifest.viewport_width - 200);
    minimap_widget->y = 8;
    minimap_widget->w = 192;
    minimap_widget->h = 192;
    minimap_redraw_flag = 1;
    widget_redraw_everything();
    OfflineRenderSurface = NULL;
    LastTick = manifest.clock_ms;
    image_missing_faces_reset();
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.player_names && manifest.target_ui) {
        widget_map_ui_test_begin();
    }
#endif
    socket_command_map(snapshot, snapshot_size, 0);
    bool timed_light_lifecycle = MapData.light_keyframe_valid;
    uint64_t timed_midpoint_seconds = 0;
    if (timed_light_lifecycle) {
        if (MapData.light_keyframe_end_seconds <= MapData.light_keyframe_start_seconds) {
            fprintf(stderr, "gpu-player-view: invalid timed-light interval\n");
            goto cleanup;
        }
        timed_midpoint_seconds =
            MapData.light_keyframe_start_seconds +
            (MapData.light_keyframe_end_seconds - MapData.light_keyframe_start_seconds) / 2;
        telemetry_game_time_sync(timed_midpoint_seconds, UINT32_MAX);
        map_animate();
        map_benchmark_statistics_reset();
    }
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.visibility_fade_test && !widget_map_visibility_test()) {
        fprintf(stderr, "gpu-player-view: visibility fade regression failed\n");
        goto cleanup;
    }
    if (manifest.damage_animation || manifest.kill_animation) {
        widget_map_animation_test_begin();
    }
    if (manifest.damage_animation) {
        widget_map_animation_test_add(
            ANIM_DAMAGE,
            manifest.animation_x_offset,
            manifest.animation_y_offset,
            manifest.animation_coordinates_set ? (int)manifest.animation_sub_layer
                                               : MapData.player_sub_layer,
            manifest.animation_coordinates_set ? (int)manifest.animation_depth : 0,
            37,
            425);
    }
    if (manifest.kill_animation) {
        widget_map_animation_test_add(
            ANIM_KILL,
            manifest.animation_x_offset + 1,
            manifest.animation_y_offset,
            manifest.animation_coordinates_set ? (int)manifest.animation_sub_layer
                                               : MapData.player_sub_layer,
            manifest.animation_coordinates_set ? (int)manifest.animation_depth : 0,
            1,
            425);
    }
#endif
    if (benchmark_mode) {
        if (image_missing_faces_detected() ||
            !gpu_player_view_benchmark(&manifest, map_widget, minimap_widget, workload)) {
            fprintf(stderr, "gpu-player-view: production benchmark failed: %s\n", SDL_GetError());
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    uint64_t lifecycle_cold_started = 0;
    gpu_renderer_statistics_t lifecycle_cold_statistics = {0};
    if (lifecycle_mode) {
        lifecycle_cold_started = gpu_player_view_lifecycle_action_begin();
    }
    if (image_missing_faces_detected() ||
        !gpu_player_view_render(map_widget, manifest.widget_render)) {
        fprintf(stderr, "gpu-player-view: initial production frame failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    if (lifecycle_mode) {
        uint64_t lifecycle_cold_duration_ns = SDL_GetTicksNS() - lifecycle_cold_started;
        gpu_renderer_statistics_get(&lifecycle_cold_statistics);
        if (!gpu_player_view_lifecycle(window,
                                       &manifest,
                                       snapshot,
                                       snapshot_size,
                                       transition_snapshot,
                                       transition_snapshot_size,
                                       lifecycle_cold_duration_ns,
                                       &lifecycle_cold_statistics)) {
            fprintf(stderr, "gpu-player-view: production lifecycle failed: %s\n", SDL_GetError());
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (manifest.ui_closure && !gpu_player_view_ui_closure_run(map_widget, &manifest)) {
        fprintf(stderr,
                "gpu-player-view: complete-screen GPU closure failed: %s\n",
                SDL_GetError());
        goto cleanup;
    }
    if (manifest.ui_closure) {
        /* The closure intentionally ends with the region-map screenshot. Restore
         * an unobscured gameplay frame so map names/targets and the primary
         * golden remain independent production evidence. */
        popup_destroy_all();
        widget_redraw_everything();
        map_redraw_request(MAP_REDRAW_REASON_UI);
        if (!gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: post-closure gameplay restore failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
    }
    uint64_t borrowed_temporal_samples = 0;
    if (timed_light_lifecycle) {
        map_benchmark_statistics_t statistics;
        map_benchmark_statistics_get(&statistics);
        borrowed_temporal_samples = statistics.borrowed_temporal_light_samples;
        if (statistics.temporal_light_samples == 0 || statistics.borrowed_light_samples == 0 ||
            borrowed_temporal_samples == 0) {
            fprintf(stderr,
                    "gpu-player-view: timed-light fixture did not exercise temporal FOW "
                    "borrowing\n");
            goto cleanup;
        }
    }
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.map_interaction_test &&
        (!widget_map_fog_click_test(map_widget) || !widget_map_interaction_test(map_widget))) {
        fprintf(stderr, "gpu-player-view: map interaction regression failed\n");
        goto cleanup;
    }
#endif
    if (manifest.look_width == 25 && manifest.look_height == 25 &&
        manifest.viewport_width == 2560 && manifest.viewport_height == 1440) {
        for (size_t warmup = 0; warmup < 2; warmup++) {
            if (!gpu_player_view_render(map_widget, manifest.widget_render)) {
                fprintf(stderr,
                        "gpu-player-view: retained-scene warmup failed: %s\n",
                        SDL_GetError());
                goto cleanup;
            }
        }
        gpu_renderer_statistics_t retained_before;
        gpu_renderer_statistics_t retained_after;
        gpu_renderer_statistics_get(&retained_before);
        if (!gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: retained-scene verification failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        gpu_renderer_statistics_get(&retained_after);
        if (retained_after.upload_count != retained_before.upload_count ||
            retained_after.upload_bytes != retained_before.upload_bytes ||
            retained_after.resource_creations != retained_before.resource_creations ||
            retained_after.resource_destructions != retained_before.resource_destructions ||
            retained_after.retained_bytes != retained_before.retained_bytes) {
            fprintf(stderr,
                    "gpu-player-view: unchanged retained scene caused GPU resource churn\n");
            goto cleanup;
        }
    }
    char initial_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char initial_artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE];
    char initial_artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char transition_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char timed_endpoint_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    transition_digest[0] = '\0';
    timed_endpoint_digest[0] = '\0';
    if (!gpu_player_view_checkpoint_named("initial",
                                          initial_digest,
                                          initial_artifact,
                                          initial_artifact_digest)) {
        fprintf(stderr,
                "gpu-player-view: cannot capture initial GPU checkpoint: %s\n",
                SDL_GetError());
        goto cleanup;
    }
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (manifest.smooth_lighting) {
        size_t full_lit_instances = gpu_map_renderer_lit_instance_count(false);
        gpu_renderer_statistics_reset();
        map_benchmark_statistics_reset();
        map_redraw_request(MAP_REDRAW_REASON_ANIMATION);
        char retained_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
        if (full_lit_instances == 0 ||
            !gpu_player_view_render(map_widget, manifest.widget_render) ||
            !gpu_player_view_checkpoint(retained_digest)) {
            fprintf(stderr,
                    "gpu-player-view: retained smooth-lighting checkpoint failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        gpu_renderer_statistics_t retained_lighting;
        gpu_renderer_statistics_get(&retained_lighting);
        map_benchmark_statistics_t retained_map;
        map_benchmark_statistics_get(&retained_map);
        size_t retained_lit_instances = gpu_map_renderer_lit_instance_count(false);
        if (strcmp(retained_digest, initial_digest) != 0 ||
            retained_lit_instances != full_lit_instances ||
            retained_lighting.light_upload_count != 0 ||
            retained_lighting.light_upload_bytes != 0 || retained_map.animation_draws != 1 ||
            retained_map.reused_render_commands == 0 ||
            retained_map.compiled_render_commands != 0) {
            fprintf(stderr,
                    "gpu-player-view: retained smooth lighting changed: instances=%" PRIu64
                    "/%" PRIu64 " "
                    "uploads=%" PRIu64 " bytes=%" PRIu64 " compiled=%" PRIu64 " reused=%" PRIu64
                    "\n",
                    (uint64_t)retained_lit_instances,
                    (uint64_t)full_lit_instances,
                    retained_lighting.light_upload_count,
                    retained_lighting.light_upload_bytes,
                    retained_map.compiled_render_commands,
                    retained_map.reused_render_commands);
            goto cleanup;
        }
    }
#endif
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.visibility_fade_test) {
        char deleted_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
        map_benchmark_statistics_t deleted_map;
        map_benchmark_statistics_t restored_map;
        map_benchmark_statistics_reset();
        if (!widget_map_retained_visibility_test_set(false) ||
            !gpu_player_view_render(map_widget, manifest.widget_render) ||
            !gpu_player_view_checkpoint(deleted_digest)) {
            fprintf(stderr,
                    "gpu-player-view: retained cohort deletion checkpoint failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        map_benchmark_statistics_get(&deleted_map);
        map_benchmark_statistics_reset();
        if (!widget_map_retained_visibility_test_set(true) ||
            !gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: retained cohort insertion checkpoint failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        char restored_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
        if (!gpu_player_view_checkpoint(restored_digest)) {
            fprintf(stderr, "gpu-player-view: retained cohort restore readback failed\n");
            goto cleanup;
        }
        map_benchmark_statistics_get(&restored_map);
        if (strcmp(deleted_digest, initial_digest) == 0 ||
            strcmp(restored_digest, initial_digest) != 0 || deleted_map.animation_draws != 1 ||
            restored_map.animation_draws != 1 || deleted_map.compiled_render_commands == 0 ||
            restored_map.compiled_render_commands != deleted_map.compiled_render_commands ||
            deleted_map.reused_render_commands <= deleted_map.compiled_render_commands ||
            restored_map.reused_render_commands <= restored_map.compiled_render_commands) {
            fprintf(stderr,
                    "gpu-player-view: retained cohort insert/delete mismatch: "
                    "delete-compiled=%" PRIu64 " delete-reused=%" PRIu64
                    " restore-compiled=%" PRIu64 " restore-reused=%" PRIu64 "\n",
                    deleted_map.compiled_render_commands,
                    deleted_map.reused_render_commands,
                    restored_map.compiled_render_commands,
                    restored_map.reused_render_commands);
            goto cleanup;
        }
    }
#endif
    if (movement_lifecycle) {
        int origin_x = MapData.posx;
        int origin_y = MapData.posy;
        for (size_t i = 0; i < arraysize(movement_fixture.packets); i++) {
            LastTick += PLAYER_VIEW_MOVEMENT_TICK_MS;
            socket_command_map(movement_fixture.packets[i].data,
                               movement_fixture.packets[i].size,
                               0);
            if (image_missing_faces_detected() ||
                !gpu_player_view_render(map_widget, manifest.widget_render)) {
                fprintf(stderr,
                        "gpu-player-view: movement production frame failed: %s\n",
                        SDL_GetError());
                goto cleanup;
            }
        }
        if (MapData.posx != origin_x || MapData.posy != origin_y) {
            fprintf(stderr, "gpu-player-view: closed movement stream did not restore its origin\n");
            goto cleanup;
        }
        if (!SDL_SetWindowSize(window,
                               (int)(manifest.viewport_width + manifest.resize_width_delta),
                               (int)(manifest.viewport_height + manifest.resize_height_delta)) ||
            !SDL_SyncWindow(window)) {
            fprintf(stderr, "gpu-player-view: movement resize-grow failed: %s\n", SDL_GetError());
            goto cleanup;
        }
        resize_window_recovery_request();
        if (!gpu_player_view_recover_once(window) ||
            !SDL_SetWindowSize(window,
                               (int)manifest.viewport_width,
                               (int)manifest.viewport_height) ||
            !SDL_SyncWindow(window)) {
            fprintf(stderr,
                    "gpu-player-view: movement resize recovery failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        resize_window_recovery_request();
        if (!gpu_player_view_recover_once(window)) {
            fprintf(stderr,
                    "gpu-player-view: movement resize restore failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        socket_command_map(transition_snapshot, transition_snapshot_size, 0);
        if (image_missing_faces_detected() ||
            !gpu_player_view_render(map_widget, manifest.widget_render) ||
            !gpu_player_view_checkpoint(transition_digest)) {
            fprintf(stderr,
                    "gpu-player-view: transition production frame failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        socket_command_map(snapshot, snapshot_size, 0);
        if (image_missing_faces_detected() ||
            !gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: return production frame failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
    } else if (next_snapshot != NULL) {
        LastTick = 0;
        socket_command_map(next_snapshot, next_snapshot_size, 0);
        LastTick = manifest.clock_ms;
#ifdef ATRINIK_WIDGET_TESTS
        if (manifest.damage_animation || manifest.kill_animation) {
            widget_map_animation_test_begin();
        }
#endif
        if (image_missing_faces_detected() ||
            !gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: updated production frame failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
    }
    if (timed_light_lifecycle) {
        telemetry_game_time_sync(MapData.light_keyframe_end_seconds, UINT32_MAX);
        /* Follow the production frame lifecycle so the temporal bucket
         * advances the retained lighting revision before the endpoint draw. */
        map_animate();
        if (!gpu_player_view_render(map_widget, manifest.widget_render) ||
            !gpu_player_view_checkpoint(timed_endpoint_digest) ||
            strcmp(timed_endpoint_digest, initial_digest) == 0) {
            fprintf(stderr, "gpu-player-view: timed-light endpoint was not distinct\n");
            goto cleanup;
        }
        telemetry_game_time_sync(timed_midpoint_seconds, UINT32_MAX);
        map_animate();
        char timed_restored_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
        if (!gpu_player_view_render(map_widget, manifest.widget_render) ||
            !gpu_player_view_checkpoint(timed_restored_digest) ||
            strcmp(timed_restored_digest, initial_digest) != 0) {
            fprintf(stderr, "gpu-player-view: timed-light midpoint restore failed\n");
            goto cleanup;
        }
    }

    char ui_pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE] = {0};
#ifdef ATRINIK_WIDGET_TESTS
    if (manifest.player_names && manifest.target_ui) {
        if (!widget_map_ui_test_end() || !gpu_player_view_checkpoint(ui_pixels_digest)) {
            fprintf(stderr,
                    "gpu-player-view: name or target UI was not rendered: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
        if (!gpu_player_view_digest_zero(manifest.expected_ui_pixels_digest) &&
            strcmp(ui_pixels_digest, manifest.expected_ui_pixels_digest) != 0) {
            fprintf(stderr,
                    "gpu-player-view: name and target UI pixel mismatch (expected %s, "
                    "got %s)\n",
                    manifest.expected_ui_pixels_digest,
                    ui_pixels_digest);
            result = 7;
            goto cleanup;
        }
        setting_set_int(OPT_CAT_MAP, OPT_PLAYER_NAMES, 0);
        cpl.target_code = 0;
        map_redraw_request(MAP_REDRAW_REASON_UI);
        if (!gpu_player_view_render(map_widget, manifest.widget_render)) {
            fprintf(stderr,
                    "gpu-player-view: UI-disabled production frame failed: %s\n",
                    SDL_GetError());
            goto cleanup;
        }
    }
    if ((manifest.damage_animation || manifest.kill_animation) &&
        !widget_map_animation_test_end(manifest.damage_animation,
                                       manifest.kill_animation,
                                       manifest.animation_elevated,
                                       manifest.animation_layer_content)) {
        fprintf(stderr, "gpu-player-view: damage or kill animation was not rendered\n");
        goto cleanup;
    }
#endif
    if (!player_view_inputs_verify(&manifest)) {
        fprintf(stderr, "gpu-player-view: frozen inputs changed during replay\n");
        goto cleanup;
    }

    char pixels_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    char final_artifact[PLAYER_VIEW_ARTIFACT_PATH_SIZE];
    char final_artifact_digest[PLAYER_VIEW_SHA256_HEX_SIZE];
    if (!gpu_player_view_checkpoint_named("final",
                                          pixels_digest,
                                          final_artifact,
                                          final_artifact_digest)) {
        fprintf(stderr, "gpu-player-view: cannot hash completed GPU frame: %s\n", SDL_GetError());
        goto cleanup;
    }
    bool expected_pending = gpu_player_view_digest_zero(manifest.expected_pixels_digest);
    bool golden_verified = !expected_pending;
    if ((movement_lifecycle && strcmp(pixels_digest, initial_digest) != 0) ||
        (movement_lifecycle &&
         (transition_digest[0] == '\0' || strcmp(transition_digest, initial_digest) == 0)) ||
        (!movement_lifecycle && !expected_pending &&
         strcmp(pixels_digest, manifest.expected_pixels_digest) != 0) ||
        (movement_lifecycle && !gpu_player_view_digest_zero(manifest.expected_pixels_digest))) {
        fprintf(stderr,
                "gpu-player-view: pixel/lifecycle mismatch (expected %s, initial %s, got %s)\n",
                movement_lifecycle ? initial_digest : manifest.expected_pixels_digest,
                initial_digest,
                pixels_digest);
        result = 7;
        goto cleanup;
    }
    gpu_player_view_record(&manifest,
                           fixture_id,
                           pixels_digest,
                           ui_pixels_digest,
                           timed_endpoint_digest,
                           borrowed_temporal_samples,
                           movement_lifecycle,
                           golden_verified,
                           initial_digest,
                           transition_digest,
                           initial_artifact,
                           initial_artifact_digest,
                           final_artifact,
                           final_artifact_digest);
    result = 0;

cleanup:
    OfflineRenderSurface = NULL;
#ifdef ATRINIK_WIDGET_TESTS
    client_ui_test_clock_reset();
#endif
    if (widgets_ready) {
        toolkit_widget_fixture_deinit();
    } else {
        cur_widget[MAP_ID] = NULL;
        cur_widget[MINIMAP_ID] = NULL;
    }
#ifdef ATRINIK_WIDGET_TESTS
    widget_mplayer_test_isolated_set(false);
    widget_render_profiler_test_isolated_set(false);
    region_map_test_fow_persistence_set(true);
#endif
    if (objects_ready) {
        object_deinit();
    }
    if (resources_ready) {
        resources_deinit();
    }
    if (metaserver_ready) {
        metaserver_deinit();
    }
    if (server_settings_ready) {
        server_settings_deinit();
    }
    if (texture_ready) {
        texture_deinit();
    }
    gpu_renderer_destroy();
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
            text_test_mono_font_path_set(NULL);
#endif
        }
        SDL_DestroyWindow(window);
        ScreenWindow = NULL;
        SDL_Quit();
    }
    if (settings_ready) {
        settings_deinit_read_only();
    }
#ifdef ATRINIK_WIDGET_TESTS
    wrapper_test_user_data_isolated_set(false);
#endif
    free(snapshot);
    free(next_snapshot);
    free(transition_snapshot);
    player_view_manifest_free(&manifest);
    return result;
}
