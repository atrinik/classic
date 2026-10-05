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

#ifndef GPU_LIGHT_ROW_H
#define GPU_LIGHT_ROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Optional deterministic work counter; never used for elapsed-time assertions. */
typedef struct gpu_light_row_work {
    size_t halfplane_evaluations;
} gpu_light_row_work_t;

/* With every coordinate bounded by M < 2^28, doubled-coordinate edge
 * products are below 2^59, and their difference below 2^60. This also bounds
 * the doubled triangle area and leaves ample signed int64_t headroom. */
#define GPU_LIGHT_ROW_COORDINATE_LIMIT INT32_C(268435455)

static inline int64_t gpu_light_row_edge(const int32_t xs[4],
                                        const int32_t ys[4],
                                        unsigned a,
                                        unsigned b,
                                        int x,
                                        int y,
                                        gpu_light_row_work_t *work) {
    if (work != NULL) {
        work->halfplane_evaluations++;
    }
    return ((int64_t)x * 2 + 1 - (int64_t)xs[a] * 2) * ((int64_t)ys[b] - ys[a]) -
           ((int64_t)y * 2 + 1 - (int64_t)ys[a] * 2) * ((int64_t)xs[b] - xs[a]);
}

/** Intersect one inclusive integer interval with an affine half-plane. */
static inline bool gpu_light_row_clip(const int32_t xs[4],
                                      const int32_t ys[4],
                                      unsigned a,
                                      unsigned b,
                                      int64_t orientation,
                                      int y,
                                      int *first,
                                      int *last,
                                      gpu_light_row_work_t *work) {
    int64_t left = orientation * gpu_light_row_edge(xs, ys, a, b, *first, y, work);
    int64_t right = orientation * gpu_light_row_edge(xs, ys, a, b, *last, y, work);
    if (left >= 0 && right >= 0) {
        return true;
    }
    if (left < 0 && right < 0) {
        return false;
    }
    /* The edge is affine in x. Opposite endpoint signs imply one transition;
     * binary search avoids signed division and its negative rounding rules. */
    int low = *first;
    int high = *last;
    while (high - low > 1) {
        int middle = low + (high - low) / 2;
        int64_t value = orientation * gpu_light_row_edge(xs, ys, a, b, middle, y, work);
        if ((value < 0) == (left < 0)) {
            low = middle;
        } else {
            high = middle;
        }
    }
    if (left < 0) {
        *first = high;
    } else {
        *last = low;
    }
    return true;
}

static inline uint64_t gpu_light_row_triangle_mask(const int32_t xs[4],
                                                    const int32_t ys[4],
                                                    unsigned a,
                                                    unsigned b,
                                                    unsigned c,
                                                    int y,
                                                    int first_x,
                                                    unsigned width,
                                                    gpu_light_row_work_t *work) {
    int64_t area = ((int64_t)xs[c] * 2 - (int64_t)xs[a] * 2) *
                       ((int64_t)ys[b] - ys[a]) -
                   ((int64_t)ys[c] * 2 - (int64_t)ys[a] * 2) *
                       ((int64_t)xs[b] - xs[a]);
    if (area == 0) {
        return 0;
    }
    int64_t orientation = area < 0 ? -1 : 1;
    int first = first_x;
    int last = first_x + (int)width - 1;
    /* edge(b,c) == area - edge(c,a) - edge(a,b), exactly reproducing
     * the original inclusive barycentric weight checks at pixel centers. */
    if (!gpu_light_row_clip(xs, ys, c, a, orientation, y, &first, &last, work) ||
        !gpu_light_row_clip(xs, ys, a, b, orientation, y, &first, &last, work) ||
        !gpu_light_row_clip(xs, ys, b, c, orientation, y, &first, &last, work)) {
        return 0;
    }
    unsigned length = (unsigned)(last - first) + 1U;
    return (UINT64_MAX >> (64U - length)) << (unsigned)(first - first_x);
}

/**
 * Return the exact pixel-center coverage of triangles (0,1,2) and (0,2,3).
 * No convexity or winding assumption is made. On false, use the caller's
 * existing predicate: inputs are outside the proved-safe fast-path range.
 * Scratch and geometry work are bounded independently of viewport dimensions.
 */
static inline bool gpu_light_row_quad_mask(const int32_t xs[4],
                                           const int32_t ys[4],
                                           int sample_y,
                                           int first_x,
                                           unsigned width,
                                           uint64_t *mask,
                                           gpu_light_row_work_t *work) {
    if (width == 0 || width > 64U || first_x < 0 ||
        (int64_t)first_x + width - 1 > GPU_LIGHT_ROW_COORDINATE_LIMIT ||
        sample_y < -GPU_LIGHT_ROW_COORDINATE_LIMIT ||
        sample_y > GPU_LIGHT_ROW_COORDINATE_LIMIT) {
        return false;
    }
    for (unsigned corner = 0; corner < 4; corner++) {
        if (xs[corner] < -GPU_LIGHT_ROW_COORDINATE_LIMIT ||
            xs[corner] > GPU_LIGHT_ROW_COORDINATE_LIMIT ||
            ys[corner] < -GPU_LIGHT_ROW_COORDINATE_LIMIT ||
            ys[corner] > GPU_LIGHT_ROW_COORDINATE_LIMIT) {
            return false;
        }
    }
    *mask = gpu_light_row_triangle_mask(xs, ys, 0, 1, 2, sample_y, first_x, width, work) |
            gpu_light_row_triangle_mask(xs, ys, 0, 2, 3, sample_y, first_x, width, work);
    return true;
}

#endif
