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

/** @file Shared fixed-point local light profiles. */

#include "light_profile.h"
#include "toolkit.h"

typedef struct light_profile {
    int center;
    int radial_radius;
    int radial_power;
} light_profile_t;

/* Authored glow_radius remains a bounded profile selector. Center intensity is
 * preserved; radial support gains one cell where the old four-cell cap allows
 * it, giving interpolation enough samples without increasing worst-case work. */
static const light_profile_t light_profiles[LIGHT_PROFILE_COUNT] = {
    {0, 0, 1},
    {40, 2, 1},
    {80, 3, 2},
    {160, 3, 2},
    {160, 4, 2},
    {320, 4, 2},
    {320, 4, 2},
    {320, 4, 2},
    {640, 4, 2},
    {1280, 4, 2},
};

static const int light_mask[LIGHT_PROFILE_SOURCE_MAX + 1] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9};

int light_profile_mask(int glow_radius) {
    if (glow_radius > LIGHT_PROFILE_SOURCE_MAX) {
        return light_mask[LIGHT_PROFILE_SOURCE_MAX];
    }

    if (glow_radius < -LIGHT_PROFILE_SOURCE_MAX) {
        return -light_mask[LIGHT_PROFILE_SOURCE_MAX];
    }

    if (glow_radius < 0) {
        return -light_mask[-glow_radius];
    }

    return light_mask[glow_radius];
}

int light_profile_radius(int mask) {
    HARD_ASSERT(mask >= 0 && mask < LIGHT_PROFILE_COUNT);
    return light_profiles[mask].radial_radius;
}

/** Calculate the deterministic monotonic radial profile at one distance. */
int light_profile_value(int mask, int distance_squared) {
    static const uint16_t distances[LIGHT_PROFILE_RADIUS_MAX * LIGHT_PROFILE_RADIUS_MAX + 1] = {
        0,
        256,
        362,
        443,
        512,
        572,
        627,
        677,
        724,
        768,
        809,
        849,
        886,
        923,
        957,
        991,
        1024,
    };
    HARD_ASSERT(mask >= 0 && mask < LIGHT_PROFILE_COUNT);
    const light_profile_t *profile = &light_profiles[mask];
    uint32_t radius = (uint32_t)profile->radial_radius * LIGHT_PROFILE_DISTANCE_SCALE;
    HARD_ASSERT(distance_squared >= 0 && distance_squared < (int)arraysize(distances));
    uint32_t distance = distances[distance_squared];
    if (distance >= radius) {
        return 0;
    }

    uint64_t numerator = radius - distance;
    uint64_t denominator = radius;
    for (int power = 1; power < profile->radial_power; power++) {
        numerator *= radius - distance;
        denominator *= radius;
    }

    return (int)(((uint64_t)profile->center * numerator + denominator / 2) / denominator);
}

/** Canonical IEC 61966-2-1 sRGB8 to scene-linear Q0.16 lookup. */
static const uint16_t srgb8_to_linear_q16[UINT8_MAX + 1] = {
    0,     20,    40,    60,    80,    99,    119,   139,   159,   179,   199,   219,   241,
    264,   288,   313,   340,   367,   396,   427,   458,   491,   526,   562,   599,   637,
    677,   718,   761,   805,   851,   898,   947,   997,   1048,  1101,  1156,  1212,  1270,
    1330,  1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,  1937,  2013,  2090,  2170,
    2250,  2333,  2418,  2504,  2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,  3360,
    3464,  3570,  3678,  3788,  3900,  4014,  4129,  4247,  4366,  4488,  4611,  4736,  4864,
    4993,  5124,  5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,  6395,  6547,  6700,
    6856,  7014,  7174,  7335,  7500,  7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,
    9072,  9258,  9445,  9635,  9828,  10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446,
    11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387,
    14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727,
    18001, 18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481,
    21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662,
    26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542, 29911, 30282,
    30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143, 34544, 34948, 35355,
    35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903,
    47385, 47871, 48359, 48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401,
    53921, 54445, 54971, 55500, 56032, 56567, 57105, 57646, 58190, 58737, 59287, 59840, 60396,
    60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

uint16_t light_profile_color_component(uint32_t color, size_t channel) {
    HARD_ASSERT(channel < 3);
    uint16_t components[3] = {
        srgb8_to_linear_q16[(color >> 16) & UINT8_MAX],
        srgb8_to_linear_q16[(color >> 8) & UINT8_MAX],
        srgb8_to_linear_q16[color & UINT8_MAX],
    };
    uint16_t peak = MAX(components[0], MAX(components[1], components[2]));
    if (peak == 0) {
        return 0;
    }
    /* Round half up exactly as the server's overflow-free mul-div does. */
    return (uint16_t)(((uint64_t)components[channel] * UINT16_MAX + peak / 2) / peak);
}

uint16_t light_profile_raw_to_radiance(int64_t raw_light) {
    if (raw_light <= 0) {
        return 0;
    }
    if (raw_light >= INT64_C(40959)) {
        return UINT16_MAX;
    }
    return (uint16_t)((raw_light * 8 + 2) / 5);
}
