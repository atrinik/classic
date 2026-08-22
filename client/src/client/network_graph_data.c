/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *************************************************************************/

/** @file Unit-aware network graph history storage. */

#include <network_graph_data.h>
#include <toolkit/memory.h>

#include <string.h>

static void network_graph_series_recalculate_max(network_graph_series_t *series) {
    series->max = 0;

    for (size_t position = 0; position < series->width; position++) {
        if (!series->valid[position]) {
            continue;
        }

        for (size_t index = 0; index < series->series_count; index++) {
            uint64_t value = series->values[position * series->series_count + index];
            if (value > series->max) {
                series->max = value;
            }
        }
    }
}

void network_graph_series_init(network_graph_series_t *series, size_t series_count) {
    memset(series, 0, sizeof(*series));
    series->series_count = series_count;
}

void network_graph_series_free(network_graph_series_t *series) {
    free(series->values);
    free(series->valid);
    memset(series, 0, sizeof(*series));
}

bool network_graph_series_resize(network_graph_series_t *series, size_t width) {
    if (width == series->width) {
        return true;
    }

    if (width != 0 && series->series_count > SIZE_MAX / width) {
        return false;
    }

    size_t cells = width * series->series_count;
    if (cells > SIZE_MAX / sizeof(*series->values)) {
        return false;
    }

    uint64_t *values = NULL;
    bool *valid = NULL;
    if (width != 0) {
        values = xcalloc(cells, sizeof(*values));
        valid = xcalloc(width, sizeof(*valid));
    }

    size_t copy_width = MIN(series->width, width);
    size_t source_start = 0;
    size_t new_pos = 0;

    if (copy_width != 0 && series->width != 0) {
        if (width < series->width) {
            size_t available = MIN(series->pos + 1, series->width);
            copy_width = MIN(copy_width, available);
            source_start = available - copy_width;
            new_pos = series->pos - source_start;
        } else {
            new_pos = MIN(series->pos, width - 1);
        }

        memcpy(values,
               series->values + source_start * series->series_count,
               copy_width * series->series_count * sizeof(*values));
        memcpy(valid, series->valid + source_start, copy_width);
    }

    free(series->values);
    free(series->valid);
    series->values = values;
    series->valid = valid;
    series->width = width;
    series->pos = width == 0 ? 0 : new_pos;
    network_graph_series_recalculate_max(series);
    return true;
}

void network_graph_series_advance(network_graph_series_t *series) {
    if (series->width == 0) {
        return;
    }

    if (series->pos + 1 < series->width) {
        series->pos++;
    } else {
        size_t cells = (series->width - 1) * series->series_count;
        if (cells != 0) {
            memmove(series->values,
                    series->values + series->series_count,
                    cells * sizeof(*series->values));
        }
        if (series->width > 1) {
            memmove(series->valid, series->valid + 1, series->width - 1);
        }
        series->pos = series->width - 1;
    }

    memset(series->values + series->pos * series->series_count,
           0,
           series->series_count * sizeof(*series->values));
    series->valid[series->pos] = false;
    network_graph_series_recalculate_max(series);
}

void network_graph_series_add(network_graph_series_t *series,
                              size_t index,
                              uint64_t value,
                              bool accumulate) {
    HARD_ASSERT(index < series->series_count);
    HARD_ASSERT(series->width > 0);

    uint64_t *destination = &series->values[series->pos * series->series_count + index];
    uint64_t previous = *destination;
    bool was_maximum = previous == series->max;

    if (accumulate) {
        if (UINT64_MAX - *destination < value) {
            *destination = UINT64_MAX;
        } else {
            *destination += value;
        }
    } else {
        *destination = value;
    }

    series->valid[series->pos] = true;
    if (*destination >= series->max) {
        series->max = *destination;
    } else if (was_maximum) {
        network_graph_series_recalculate_max(series);
    }
}

bool network_graph_series_is_valid(const network_graph_series_t *series, size_t position) {
    return position < series->width && series->valid[position];
}

uint64_t
network_graph_series_value(const network_graph_series_t *series, size_t position, size_t index) {
    if (!network_graph_series_is_valid(series, position) || index >= series->series_count) {
        return 0;
    }

    return series->values[position * series->series_count + index];
}
