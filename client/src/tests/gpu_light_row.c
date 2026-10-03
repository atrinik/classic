/*************************************************************************
 * Atrinik client GPU light-row regression tests.                        *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                    *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <gpu_light_row.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                            \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "gpu light-row assertion failed at line %d\n", __LINE__);    \
            abort();                                                                     \
        }                                                                                \
    } while (0)

static int64_t reference_edge(const int32_t xs[4],
                              const int32_t ys[4],
                              size_t a,
                              size_t b,
                              int x,
                              int y) {
    const int64_t point_x_twice = (int64_t)x * 2 + 1;
    const int64_t point_y_twice = (int64_t)y * 2 + 1;
    const int64_t a_x_twice = (int64_t)xs[a] * 2;
    const int64_t a_y_twice = (int64_t)ys[a] * 2;

    return (point_x_twice - a_x_twice) * ((int64_t)ys[b] - ys[a]) -
           (point_y_twice - a_y_twice) * ((int64_t)xs[b] - xs[a]);
}

/* This is the former scalar barycentric coverage predicate. */
static bool reference_triangle_contains(const int32_t xs[4],
                                        const int32_t ys[4],
                                        size_t a,
                                        size_t b,
                                        size_t c,
                                        int x,
                                        int y) {
    const int64_t area =
        ((int64_t)xs[c] * 2 - (int64_t)xs[a] * 2) * ((int64_t)ys[b] - ys[a]) -
        ((int64_t)ys[c] * 2 - (int64_t)ys[a] * 2) * ((int64_t)xs[b] - xs[a]);
    if (area == 0) {
        return false;
    }

    const int64_t orientation = area < 0 ? -1 : 1;
    const int64_t weight_b = orientation * reference_edge(xs, ys, c, a, x, y);
    const int64_t weight_c = orientation * reference_edge(xs, ys, a, b, x, y);
    const uint64_t absolute_area = area < 0 ? (uint64_t)-area : (uint64_t)area;

    return weight_b >= 0 && weight_c >= 0 &&
           (uint64_t)(weight_b + weight_c) <= absolute_area;
}

static bool reference_quad_contains(const int32_t xs[4],
                                    const int32_t ys[4],
                                    int x,
                                    int y) {
    return reference_triangle_contains(xs, ys, 0, 1, 2, x, y) ||
           reference_triangle_contains(xs, ys, 0, 2, 3, x, y);
}

static uint64_t reference_row_mask(const int32_t xs[4],
                                   const int32_t ys[4],
                                   int sample_y,
                                   int first_x,
                                   unsigned int width) {
    uint64_t mask = 0;

    for (unsigned int column = 0; column < width; column++) {
        if (reference_quad_contains(xs, ys, first_x + (int)column, sample_y)) {
            mask |= UINT64_C(1) << column;
        }
    }

    return mask;
}

static void check_row(const int32_t xs[4],
                      const int32_t ys[4],
                      int sample_y,
                      int first_x,
                      unsigned int width) {
    uint64_t mask = 0;
    gpu_light_row_work_t work = {0};

    TEST_CHECK(gpu_light_row_quad_mask(xs, ys, sample_y, first_x, width, &mask, &work));
    TEST_CHECK(mask == reference_row_mask(xs, ys, sample_y, first_x, width));
    /* Three half-planes per triangle, two endpoints plus at most six bisections. */
    TEST_CHECK(work.halfplane_evaluations <= 2U * 3U * 8U);
}

static void reverse_vertices(const int32_t input_xs[4],
                             const int32_t input_ys[4],
                             int32_t output_xs[4],
                             int32_t output_ys[4]) {
    for (size_t index = 0; index < 4; index++) {
        output_xs[index] = input_xs[3 - index];
        output_ys[index] = input_ys[3 - index];
    }
}

static uint32_t random_state = UINT32_C(0x7639b5a1);

static uint32_t random_u32(void) {
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static int random_coordinate(void) {
    return (int)(random_u32() % 161U) - 48;
}

static void test_scalar_oracle_against_deterministic_quads(void) {
    static const struct {
        int32_t xs[4];
        int32_t ys[4];
        int sample_y;
        int first_x;
        unsigned int width;
    } cases[] = {
        {{-GPU_LIGHT_ROW_COORDINATE_LIMIT, GPU_LIGHT_ROW_COORDINATE_LIMIT,
          GPU_LIGHT_ROW_COORDINATE_LIMIT, -GPU_LIGHT_ROW_COORDINATE_LIMIT},
         {-GPU_LIGHT_ROW_COORDINATE_LIMIT, -GPU_LIGHT_ROW_COORDINATE_LIMIT,
          GPU_LIGHT_ROW_COORDINATE_LIMIT, GPU_LIGHT_ROW_COORDINATE_LIMIT},
         0, GPU_LIGHT_ROW_COORDINATE_LIMIT - 63, 64},
        {{2, 25, 25, 2}, {2, 2, 25, 25}, 2, 0, 64},
        {{2, 25, 25, 2}, {2, 2, 25, 25}, 24, 0, 63},
        {{2, 25, 25, 2}, {2, 2, 25, 25}, 12, 12, 1},
        {{-31, 9, 9, -31}, {-20, -20, 20, 20}, 0, 0, 64},
        {{-80, -50, -50, -80}, {3, 3, 30, 30}, 10, 0, 64},
        {{31, 65, 65, 31}, {31, 31, 65, 65}, 63, 0, 64},
        {{2, 20, 7, 2}, {2, 2, 8, 20}, 8, 0, 64},
        {{2, 20, 2, 20}, {2, 20, 20, 2}, 11, 0, 64},
        {{1, 3, 5, 7}, {1, 3, 5, 7}, 3, 0, 64},
        {{2, 10, 18, 30}, {2, 2, 2, 20}, 7, 0, 64},
        {{GPU_LIGHT_ROW_COORDINATE_LIMIT - 1, GPU_LIGHT_ROW_COORDINATE_LIMIT,
          GPU_LIGHT_ROW_COORDINATE_LIMIT, GPU_LIGHT_ROW_COORDINATE_LIMIT - 1},
         {GPU_LIGHT_ROW_COORDINATE_LIMIT - 1, GPU_LIGHT_ROW_COORDINATE_LIMIT - 1,
          GPU_LIGHT_ROW_COORDINATE_LIMIT, GPU_LIGHT_ROW_COORDINATE_LIMIT},
         GPU_LIGHT_ROW_COORDINATE_LIMIT - 1, GPU_LIGHT_ROW_COORDINATE_LIMIT - 1, 1},
        {{-GPU_LIGHT_ROW_COORDINATE_LIMIT, -GPU_LIGHT_ROW_COORDINATE_LIMIT + 1,
          -GPU_LIGHT_ROW_COORDINATE_LIMIT + 1, -GPU_LIGHT_ROW_COORDINATE_LIMIT},
         {-GPU_LIGHT_ROW_COORDINATE_LIMIT, -GPU_LIGHT_ROW_COORDINATE_LIMIT,
          -GPU_LIGHT_ROW_COORDINATE_LIMIT + 1, -GPU_LIGHT_ROW_COORDINATE_LIMIT + 1},
         -GPU_LIGHT_ROW_COORDINATE_LIMIT, 0, 64},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        int32_t reversed_xs[4];
        int32_t reversed_ys[4];

        check_row(cases[index].xs, cases[index].ys, cases[index].sample_y,
                  cases[index].first_x, cases[index].width);
        reverse_vertices(cases[index].xs, cases[index].ys, reversed_xs, reversed_ys);
        check_row(reversed_xs, reversed_ys, cases[index].sample_y, cases[index].first_x,
                  cases[index].width);
    }

    for (unsigned int index = 0; index < 512; index++) {
        int32_t xs[4];
        int32_t ys[4];
        int32_t reversed_xs[4];
        int32_t reversed_ys[4];
        const int sample_y = random_coordinate();
        const int first_x = (int)(random_u32() % 48U);
        const unsigned int width = 1U + random_u32() % 64U;

        for (size_t corner = 0; corner < 4; corner++) {
            xs[corner] = random_coordinate();
            ys[corner] = random_coordinate();
        }
        check_row(xs, ys, sample_y, first_x, width);
        reverse_vertices(xs, ys, reversed_xs, reversed_ys);
        check_row(reversed_xs, reversed_ys, sample_y, first_x, width);
    }
}

static void test_invalid_coordinates_defer_to_the_caller(void) {
    const int32_t valid_xs[4] = {1, 5, 5, 1};
    const int32_t valid_ys[4] = {1, 1, 5, 5};
    int32_t xs[4];
    int32_t ys[4];
    uint64_t mask = UINT64_MAX;

    for (size_t corner = 0; corner < 4; corner++) {
        xs[corner] = valid_xs[corner];
        ys[corner] = valid_ys[corner];
    }
    xs[2] = GPU_LIGHT_ROW_COORDINATE_LIMIT + 1;
    TEST_CHECK(!gpu_light_row_quad_mask(xs, ys, 2, 0, 64, &mask, NULL));

    ys[2] = -GPU_LIGHT_ROW_COORDINATE_LIMIT - 1;
    TEST_CHECK(!gpu_light_row_quad_mask(valid_xs, ys, 2, 0, 64, &mask, NULL));

    TEST_CHECK(!gpu_light_row_quad_mask(valid_xs, valid_ys,
                                        GPU_LIGHT_ROW_COORDINATE_LIMIT + 1, 0, 64, &mask,
                                        NULL));
}

static int reverse_owner(const uint64_t masks[], size_t count, unsigned int column) {
    for (size_t candidate = count; candidate > 0; candidate--) {
        if (masks[candidate - 1] & (UINT64_C(1) << column)) {
            return (int)candidate - 1;
        }
    }
    return -1;
}

static void test_reverse_candidate_ownership_uses_the_highest_index(void) {
    static const int32_t xs[][4] = {
        {2, 30, 30, 2},
        {8, 40, 40, 8},
        {18, 50, 50, 18},
    };
    static const int32_t ys[][4] = {
        {2, 2, 12, 12},
        {2, 2, 12, 12},
        {2, 2, 12, 12},
    };
    uint64_t masks[3];

    for (size_t candidate = 0; candidate < 3; candidate++) {
        TEST_CHECK(gpu_light_row_quad_mask(xs[candidate], ys[candidate], 6, 0, 64,
                                           &masks[candidate], NULL));
    }

    for (unsigned int column = 0; column < 64; column++) {
        int expected = -1;
        for (size_t candidate = 0; candidate < 3; candidate++) {
            if (reference_quad_contains(xs[candidate], ys[candidate], (int)column, 6)) {
                expected = (int)candidate;
            }
        }
        TEST_CHECK(reverse_owner(masks, 3, column) == expected);
    }
}

static void test_large_bucket_avoids_scalar_pixel_work(void) {
    const int32_t xs[4] = {-128, 192, 192, -128};
    const int32_t ys[4] = {-128, -128, 192, 192};
    gpu_light_row_work_t work = {0};
    const size_t before = work.halfplane_evaluations;
    uint64_t mask;

    TEST_CHECK(gpu_light_row_quad_mask(xs, ys, 12, 0, 64, &mask, &work));
    TEST_CHECK(mask == UINT64_MAX);
    /* The scalar predicate evaluates three half-planes for each triangle and pixel. */
    TEST_CHECK(work.halfplane_evaluations - before < 64U * 6U);
}

int main(void) {
    test_scalar_oracle_against_deterministic_quads();
    test_invalid_coordinates_defer_to_the_caller();
    test_reverse_candidate_ownership_uses_the_highest_index();
    test_large_bucket_avoids_scalar_pixel_work();
    return EXIT_SUCCESS;
}
