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

/**
 * @file
 * Unit-aware network graph history storage.
 */

#ifndef NETWORK_GRAPH_DATA_H
#define NETWORK_GRAPH_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * A fixed-width history of one or more graph series.
 *
 * Position @c pos is the current, not-yet-drawn bucket. Buckets before it
 * are historical samples; a bucket is valid only when a sample was recorded.
 */
typedef struct network_graph_series {
    uint64_t *values;
    bool *valid;
    size_t width;
    size_t pos;
    size_t series_count;
    uint64_t max;
} network_graph_series_t;

void network_graph_series_init(network_graph_series_t *series, size_t series_count);
void network_graph_series_free(network_graph_series_t *series);
bool network_graph_series_resize(network_graph_series_t *series, size_t width);
void network_graph_series_advance(network_graph_series_t *series);
void network_graph_series_add(network_graph_series_t *series,
                              size_t index,
                              uint64_t value,
                              bool accumulate);
bool network_graph_series_is_valid(const network_graph_series_t *series, size_t position);
uint64_t network_graph_series_value(const network_graph_series_t *series,
                                    size_t position,
                                    size_t index);

#endif
