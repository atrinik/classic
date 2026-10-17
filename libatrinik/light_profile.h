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

/**
 * @file
 * Local light profiles shared by the server's authoritative light masks and
 * the client's protocol-1085 emitter evaluation, so both use identical
 * fixed-point falloff.
 */

#ifndef TOOLKIT_LIGHT_PROFILE_H
#define TOOLKIT_LIGHT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Number of profiles a grouped glow radius maps to. */
#define LIGHT_PROFILE_COUNT 10
/** Largest grouped glow radius; stronger sums use this one's profile. */
#define LIGHT_PROFILE_SOURCE_MAX 13
/** Largest support radius of any profile, in cells. */
#define LIGHT_PROFILE_RADIUS_MAX 4
/** Fixed-point scale of one cell of distance. */
#define LIGHT_PROFILE_DISTANCE_SCALE 256

/** Map a grouped, signed glow radius to a signed profile index. */
int light_profile_mask(int glow_radius);

/** Support radius in cells of an unsigned profile index. */
int light_profile_radius(int mask);

/** Raw light of an unsigned profile at a squared cell distance, before sign. */
int light_profile_value(int mask, int distance_squared);

/**
 * One channel of an authored sRGB color in scene-linear Q0.16, normalized so
 * its strongest channel is 65535.
 */
uint16_t light_profile_color_component(uint32_t color, size_t channel);

/** Convert raw light to pre-tone Q5.11 radiance. */
uint16_t light_profile_raw_to_radiance(int64_t raw_light);

#endif
