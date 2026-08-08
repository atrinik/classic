/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
#include <initialization.h>
#include <map.h>
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
    for (size_t i = 0; i < map_count; i++) {
        if (!benchmark_map(maps[i])) {
            return EXIT_FAILURE;
        }
    }
    fflush(stdout);
    return EXIT_SUCCESS;
}
