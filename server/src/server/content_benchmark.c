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

/**
 * @file
 * Offline measurements for the current authored-content loading pipeline.
 */

#include <global.h>

#include <content_benchmark.h>
#include <walking_route.h>
#include <initialization.h>
#include <map.h>
#include <object.h>
#include <server_main.h>
#include <light.h>
#include <swap.h>
#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#ifdef __linux__
#include <sys/resource.h>
#endif

#define CONTENT_BENCHMARK_PREFIX "ATRINIK_CONTENT_BENCHMARK"
#define CONTENT_BENCHMARK_MAX_MAPS 16

static uint64_t startup_started_us;
static uint64_t arch_started_us;
static uint64_t arch_elapsed_us;

void content_benchmark_startup_begin(void) {
    startup_started_us = datetime_monotonic_us();
}

void content_benchmark_arch_begin(void) {
    if (settings.content_benchmark) {
        arch_started_us = datetime_monotonic_us();
    }
}

void content_benchmark_arch_end(void) {
    if (settings.content_benchmark) {
        arch_elapsed_us = datetime_monotonic_us() - arch_started_us;
    }
}

static bool logical_map_id_is_safe(const char *path) {
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

static size_t parse_map_ids(const char *input, char maps[CONTENT_BENCHMARK_MAX_MAPS][MAX_BUF]) {
    size_t length = strlen(input);
    if (length == 0 || length >= sizeof(settings.content_benchmark_maps) || input[0] == ',' ||
        input[length - 1] == ',' || strstr(input, ",,") != NULL) {
        return 0;
    }

    size_t component_length = 0;
    for (size_t i = 0; i <= length; i++) {
        if (input[i] == ',' || input[i] == '\0') {
            if (component_length == 0 || component_length >= MAX_BUF) {
                return 0;
            }
            component_length = 0;
        } else {
            component_length++;
        }
    }

    size_t count = 0;
    size_t position = 0;
    while (count < CONTENT_BENCHMARK_MAX_MAPS &&
           string_get_word(input, &position, ',', maps[count], sizeof(maps[count]), 0) != NULL) {
        if (!logical_map_id_is_safe(maps[count])) {
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

bool content_benchmark_maps_valid(const char *input) {
    char maps[CONTENT_BENCHMARK_MAX_MAPS][MAX_BUF];
    return input != NULL && parse_map_ids(input, maps) != 0;
}

static void report_header(uint64_t startup_elapsed_us) {
    printf(CONTENT_BENCHMARK_PREFIX "\tformat\t1\n");
    printf(CONTENT_BENCHMARK_PREFIX "\tmode\toffline-authored-content\n");
    printf(CONTENT_BENCHMARK_PREFIX "\titerations\t%u\n", settings.content_benchmark_iterations);
    printf(CONTENT_BENCHMARK_PREFIX "\tstartup_us\t%" PRIu64 "\n", startup_elapsed_us);
    printf(CONTENT_BENCHMARK_PREFIX "\tarchetype_init_us\t%" PRIu64 "\n", arch_elapsed_us);

#ifndef __linux__
    printf(CONTENT_BENCHMARK_PREFIX "\tstartup_peak_rss_kib\tunsupported\n");
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        printf(CONTENT_BENCHMARK_PREFIX "\tstartup_peak_rss_kib\t%ld\n", usage.ru_maxrss);
    } else {
        printf(CONTENT_BENCHMARK_PREFIX "\tstartup_peak_rss_kib\tunavailable\n");
    }
#endif
}

/* Task-local opt-in diagnostics. No synthetic node survives a lookup sample.
 * inventory: map, sample, width, height, objects, origins, positive, negative.
 * rebuild: map, sample, wall_us, all resident maps/cells/objects/origins/positive/
 * negative, exact comparison result. Resident totals are not rebuilt-set counts.
 * lookup-miss: original map count, synthetic additions, probes, total wall_us.
 * cohort-load: map, wall_us. Initialization and comparison are outside rebuild timing.
 * mismatch: rebuild map, sample, changed map, x, y, before/after pairs for scalar,
 * positive, red, green, blue, weight. At most 16 cells, plus mismatch-count total.
 * failed-rebuild-repeat: rebuild map, sample, wall_us, mismatch count, stability.
 * This optional failure diagnostic never turns the rejected sample into a pass.
 * opaque-burst: map, x, y, update call count, wall_us, accumulator mismatches,
 * synthetic label. Once per run; ordinary opaque flag updates also invalidate
 * celestial caches/revisions even though source geometry remains unchanged.
 */
#define ISSUE566_PREFIX "ATRINIK_ISSUE566_BENCHMARK"

static bool issue566_enabled(void) {
    const char *value = getenv("ATRINIK_ISSUE566_BENCHMARK");
    return value != NULL && strcmp(value, "1") == 0;
}

typedef struct issue566_light_snapshot {
    MapSpace *space;
    const char *path;
    int x, y;
    int32_t scalar;
    int32_t positive;
    int64_t color[3];
    int64_t weight;
} issue566_light_snapshot;

static bool issue566_snapshot_matches(const issue566_light_snapshot *saved) {
    const MapSpace *space = saved->space;
    return saved->scalar == space->light_source_value &&
           saved->positive == space->light_source_positive_value &&
           memcmp(saved->color, space->light_source_color, sizeof(saved->color)) == 0 &&
           saved->weight == space->light_source_color_weight;
}

static void issue566_snapshot_capture(issue566_light_snapshot *saved) {
    const MapSpace *space = saved->space;
    saved->scalar = space->light_source_value;
    saved->positive = space->light_source_positive_value;
    memcpy(saved->color, space->light_source_color, sizeof(saved->color));
    saved->weight = space->light_source_color_weight;
}

/* Synthetic amplification measurement, executed once per offline run. */
static bool issue566_opaque_burst(mapstruct *map,
                                 const issue566_light_snapshot *before,
                                 size_t cells) {
    static bool attempted;
    const char *enabled = getenv("ATRINIK_ISSUE566_OPAQUE_BURST");
    if (attempted || enabled == NULL || strcmp(enabled, "1") != 0) {
        return true;
    }
    attempted = true;
    object *selected = NULL;
    for (int y = 0; y < MAP_HEIGHT(map) && selected == NULL; y++) {
        for (int x = 0; x < MAP_WIDTH(map) && selected == NULL; x++) {
            for (object *op = GET_MAP_OB(map, x, y); op != NULL; op = op->above) {
                if (op->head == NULL && op->more == NULL &&
                    (QUERY_FLAG(op, FLAG_IS_FLOOR) || QUERY_FLAG(op, FLAG_BLOCKSVIEW))) {
                    selected = op;
                    break;
                }
            }
        }
    }
    if (selected == NULL) {
        printf(ISSUE566_PREFIX "\topaque-burst-skipped\t%s\tno-single-part-opaque-object\n",
               map->path);
        return true;
    }
    static const int counts[] = {1, 1000};
    for (size_t sample = 0; sample < arraysize(counts); sample++) {
        uint64_t started = datetime_monotonic_us();
        for (int i = 0; i < counts[sample]; i++) {
            object_update(selected, UP_OBJ_FLAGS);
        }
        uint64_t elapsed_us = datetime_monotonic_us() - started;
        size_t mismatches = 0;
        for (size_t i = 0; i < cells; i++) {
            mismatches += !issue566_snapshot_matches(&before[i]);
        }
        printf(ISSUE566_PREFIX "\topaque-burst\t%s\t%d\t%d\t%d\t%" PRIu64
                              "\t%" PRIuMAX "\tsynthetic-repeated-no-op-opaque-updates\n",
               map->path, selected->x, selected->y, counts[sample], elapsed_us, (uintmax_t)mismatches);
        if (mismatches != 0) {
            LOG(ERROR, "Issue 566 synthetic opaque updates changed local illumination for %s.",
                map->path);
            return false;
        }
    }
    return true;
}

static bool issue566_rebuild(mapstruct *map, uint16_t sample) {
    size_t cells = 0, resident_maps = 0, objects = 0, origins = 0, positive = 0, negative = 0;
    size_t local_objects = 0, local_origins = 0, local_positive = 0, local_negative = 0;
    for (mapstruct *loaded = first_map; loaded != NULL; loaded = loaded->next) {
        if (loaded->in_memory != MAP_IN_MEMORY || loaded->spaces == NULL) {
            continue;
        }
        resident_maps++;
        cells += (size_t)MAP_WIDTH(loaded) * MAP_HEIGHT(loaded);
        for (MapSpace *origin = loaded->first_light; origin != NULL; origin = origin->next_light) {
            origins++;
            local_origins += loaded == map;
        }
        for (int y = 0; y < MAP_HEIGHT(loaded); y++) {
            for (int x = 0; x < MAP_WIDTH(loaded); x++) {
                for (object *op = GET_MAP_OB(loaded, x, y); op != NULL; op = op->above) {
                    objects++;
                    positive += op->glow_radius > 0;
                    negative += op->glow_radius < 0;
                    if (loaded == map) {
                        local_objects++;
                        local_positive += op->glow_radius > 0;
                        local_negative += op->glow_radius < 0;
                    }
                }
            }
        }
    }
    printf(ISSUE566_PREFIX "\tinventory\t%s\t%u\t%d\t%d\t%" PRIuMAX
                          "\t%" PRIuMAX "\t%" PRIuMAX "\t%" PRIuMAX "\n",
           map->path, sample, MAP_WIDTH(map), MAP_HEIGHT(map), (uintmax_t)local_objects,
           (uintmax_t)local_origins, (uintmax_t)local_positive, (uintmax_t)local_negative);
    issue566_light_snapshot *before = xcalloc(cells, sizeof(*before));
    size_t position = 0;
    for (mapstruct *loaded = first_map; loaded != NULL; loaded = loaded->next) {
        if (loaded->in_memory != MAP_IN_MEMORY || loaded->spaces == NULL) {
            continue;
        }
        for (int y = 0; y < MAP_HEIGHT(loaded); y++) {
            for (int x = 0; x < MAP_WIDTH(loaded); x++) {
                MapSpace *space = GET_MAP_SPACE_PTR(loaded, x, y);
                issue566_light_snapshot *saved = &before[position++];
                saved->space = space;
                saved->path = loaded->path;
                saved->x = x;
                saved->y = y;
                issue566_snapshot_capture(saved);
            }
        }
    }
    uint64_t started = datetime_monotonic_us();
    recalculate_light_sources(map);
    uint64_t elapsed_us = datetime_monotonic_us() - started;
    size_t mismatches = 0;
    for (size_t i = 0; i < cells; i++) {
        const issue566_light_snapshot *saved = &before[i];
        const MapSpace *space = saved->space;
        if (!issue566_snapshot_matches(saved)) {
            if (mismatches < 16) {
                printf(ISSUE566_PREFIX "\tmismatch\t%s\t%u\t%s\t%d\t%d"
                                      "\t%" PRId32 "\t%" PRId32 "\t%" PRId32 "\t%" PRId32
                                      "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64
                                      "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\n",
                       map->path, sample, saved->path, saved->x, saved->y,
                       saved->scalar, space->light_source_value,
                       saved->positive, space->light_source_positive_value,
                       saved->color[0], space->light_source_color[0],
                       saved->color[1], space->light_source_color[1],
                       saved->color[2], space->light_source_color[2],
                       saved->weight, space->light_source_color_weight);
            }
            mismatches++;
        }
    }
    bool identical = mismatches == 0;
    printf(ISSUE566_PREFIX "\tmismatch-count\t%s\t%u\t%" PRIuMAX "\n",
           map->path, sample, (uintmax_t)mismatches);
    printf(ISSUE566_PREFIX "\trebuild\t%s\t%u\t%" PRIu64
                          "\t%" PRIuMAX "\t%" PRIuMAX "\t%" PRIuMAX
                          "\t%" PRIuMAX "\t%" PRIuMAX "\t%" PRIuMAX "\t%s\n",
           map->path, sample, elapsed_us, (uintmax_t)resident_maps, (uintmax_t)cells,
           (uintmax_t)objects, (uintmax_t)origins, (uintmax_t)positive, (uintmax_t)negative,
           identical ? "exact" : "changed");
    if (!identical) {
        LOG(ERROR, "Issue 566 benchmark rebuild changed initialized illumination for %s.", map->path);
        const char *repeat = getenv("ATRINIK_ISSUE566_REPEAT_FAILED");
        if (repeat != NULL && strcmp(repeat, "1") == 0) {
            for (size_t i = 0; i < cells; i++) {
                issue566_snapshot_capture(&before[i]);
            }
            started = datetime_monotonic_us();
            recalculate_light_sources(map);
            uint64_t repeat_us = datetime_monotonic_us() - started;
            size_t repeat_mismatches = 0;
            for (size_t i = 0; i < cells; i++) {
                repeat_mismatches += !issue566_snapshot_matches(&before[i]);
            }
            printf(ISSUE566_PREFIX "\tfailed-rebuild-repeat\t%s\t%u\t%" PRIu64
                                  "\t%" PRIuMAX "\t%s\n",
                   map->path, sample, repeat_us, (uintmax_t)repeat_mismatches,
                   repeat_mismatches == 0 ? "stable" : "changed");
        }
    }
    if (identical && !issue566_opaque_burst(map, before, cells)) {
        identical = false;
    }
    free(before);
    return identical;
}

static bool issue566_lookup(void) {
    static const size_t counts[] = {0, 64, 256, 1024, 4096};
    enum { PROBES = 1000 };
    shstr *missing = add_string("/__issue566_lookup_missing__");
    shstr *dummy_path = add_string("/__issue566_lookup_dummy__");
    mapstruct *original_head = first_map;
    size_t actual = 0;
    for (mapstruct *map = first_map; map != NULL; map = map->next) {
        actual++;
    }
    if (has_been_loaded_sh(missing) != NULL) {
        free_string_shared(dummy_path);
        free_string_shared(missing);
        LOG(ERROR, "Issue 566 lookup probe path unexpectedly exists.");
        return false;
    }
    bool valid = true;
    for (size_t sample = 0; sample < arraysize(counts); sample++) {
        size_t count = counts[sample];
        mapstruct *nodes = xcalloc(count + 1, sizeof(*nodes));
        for (size_t i = 0; i < count; i++) {
            nodes[i].path = dummy_path;
            nodes[i].in_memory = MAP_SWAPPED;
            DL_PREPEND(first_map, &nodes[i]);
        }
        uint64_t started = datetime_monotonic_us();
        for (int probe = 0; probe < PROBES; probe++) {
            if (has_been_loaded_sh(missing) != NULL) {
                valid = false;
            }
        }
        uint64_t elapsed_us = datetime_monotonic_us() - started;
        for (size_t i = count; i > 0; i--) {
            DL_DELETE(first_map, &nodes[i - 1]);
        }
        free(nodes);
        if (first_map != original_head) {
            valid = false;
        }
        printf(ISSUE566_PREFIX "\tlookup-miss\t%" PRIuMAX "\t%" PRIuMAX "\t%d\t%" PRIu64 "\n",
               (uintmax_t)actual, (uintmax_t)count, PROBES, elapsed_us);
    }
    free_string_shared(dummy_path);
    free_string_shared(missing);
    return valid;
}

static bool issue566_cohort(char paths[CONTENT_BENCHMARK_MAX_MAPS][MAX_BUF], size_t count) {
    mapstruct *loaded[CONTENT_BENCHMARK_MAX_MAPS] = {0};
    bool success = true;
    size_t initialized = 0;
    for (; initialized < count; initialized++) {
        uint64_t started = datetime_monotonic_us();
        loaded[initialized] = ready_map_name(paths[initialized], NULL, MAP_FLUSH);
        uint64_t elapsed_us = datetime_monotonic_us() - started;
        if (loaded[initialized] == NULL) {
            success = false;
            break;
        }
        printf(ISSUE566_PREFIX "\tcohort-load\t%s\t%" PRIu64 "\n",
               paths[initialized], elapsed_us);
    }
    if (success) {
        for (uint16_t sample = 0; sample < settings.content_benchmark_iterations && success; sample++) {
            for (size_t i = 0; i < count; i++) {
                if (!issue566_rebuild(loaded[i], sample)) {
                    success = false;
                    break;
                }
            }
        }
    }
    if (success) {
        success = issue566_lookup();
    }
    for (size_t i = initialized; i > 0; i--) {
        clean_tmp_map(loaded[i - 1]);
        delete_map(loaded[i - 1]);
    }
    return success;
}

static bool benchmark_map(const char *path) {
    for (uint16_t sample = 0; sample < settings.content_benchmark_iterations; sample++) {
        uint64_t started = datetime_monotonic_us();
        mapstruct *map = ready_map_name(path, NULL, MAP_FLUSH);
        uint64_t cold_us = datetime_monotonic_us() - started;
        if (map == NULL) {
            LOG(ERROR, "Content benchmark could not load authored map %s.", path);
            return false;
        }

        started = datetime_monotonic_us();
        mapstruct *warm = ready_map_name(path, NULL, 0);
        uint64_t warm_us = datetime_monotonic_us() - started;
        if (warm != map) {
            LOG(ERROR, "Content benchmark warm lookup changed the map identity for %s.", path);
            return false;
        }

        if (issue566_enabled() && !issue566_rebuild(map, sample)) {
            clean_tmp_map(map);
            delete_map(map);
            return false;
        }

        started = datetime_monotonic_us();
        swap_map(map, 1);
        uint64_t swap_us = datetime_monotonic_us() - started;

        shstr *path_shared = add_string(path);
        mapstruct *swapped = has_been_loaded_sh(path_shared);
        free_string_shared(path_shared);
        if (swapped == NULL || swapped->in_memory != MAP_SWAPPED || swapped->tmpname == NULL) {
            LOG(ERROR, "Content benchmark map %s did not produce a temporary swapped map.", path);
            return false;
        }

        started = datetime_monotonic_us();
        mapstruct *reloaded = ready_map_name(path, NULL, 0);
        uint64_t reload_us = datetime_monotonic_us() - started;
        if (reloaded == NULL || reloaded != swapped || reloaded->in_memory != MAP_IN_MEMORY) {
            LOG(ERROR, "Content benchmark could not reload swapped map %s.", path);
            return false;
        }

        printf(CONTENT_BENCHMARK_PREFIX "\tmap\t%s\t%u\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                                        "\t%" PRIu64 "\n",
               path,
               sample,
               cold_us,
               warm_us,
               swap_us,
               reload_us);

        clean_tmp_map(reloaded);
        delete_map(reloaded);
    }

    return true;
}

int content_benchmark_run(void) {
    if (strcmp(settings.content_benchmark_maps, "brynknot-v1") == 0) {
        return walking_route_export();
    }
    char maps[CONTENT_BENCHMARK_MAX_MAPS][MAX_BUF];
    size_t map_count = parse_map_ids(settings.content_benchmark_maps, maps);
    if (map_count == 0) {
        LOG(ERROR,
            "Content benchmark requires 1-%d unique, canonical logical map IDs separated by "
            "commas.",
            CONTENT_BENCHMARK_MAX_MAPS);
        return EXIT_FAILURE;
    }

    uint64_t startup_elapsed_us = datetime_monotonic_us() - startup_started_us;
    report_header(startup_elapsed_us);
    const char *cohort = getenv("ATRINIK_ISSUE566_COHORT");
    if (issue566_enabled() && cohort != NULL && strcmp(cohort, "1") == 0) {
        bool success = issue566_cohort(maps, map_count);
        fflush(stdout);
        return success ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    for (size_t i = 0; i < map_count; i++) {
        if (!benchmark_map(maps[i])) {
            return EXIT_FAILURE;
        }
    }
    if (issue566_enabled() && !issue566_lookup()) {
        return EXIT_FAILURE;
    }
    fflush(stdout);
    return EXIT_SUCCESS;
}
