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
 *
 * Every function is pure: it reads only immutable built-in tables, allocates
 * nothing, retains no state and is safe to call from any thread. Arguments
 * outside a documented range are programming errors checked with HARD_ASSERT;
 * they abort assertion-enabled builds and are undefined otherwise.
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

/**
 * Map a grouped, signed glow radius to a signed profile index.
 * @param glow_radius Any value; magnitudes above LIGHT_PROFILE_SOURCE_MAX use
 * the strongest profile.
 * @return Profile index in -(LIGHT_PROFILE_COUNT - 1)..LIGHT_PROFILE_COUNT - 1,
 * negative for darkness and 0 for no light.
 */
int light_profile_mask(int glow_radius);

/**
 * Support radius of a profile.
 * @param mask Unsigned profile index, 0..LIGHT_PROFILE_COUNT - 1.
 * @return Radius in cells, 0..LIGHT_PROFILE_RADIUS_MAX.
 */
int light_profile_radius(int mask);

/**
 * Raw light of a profile at a distance, before applying the source's sign.
 * @param mask Unsigned profile index, 0..LIGHT_PROFILE_COUNT - 1.
 * @param distance_squared Squared cell distance,
 * 0..LIGHT_PROFILE_RADIUS_MAX * LIGHT_PROFILE_RADIUS_MAX.
 * @return Raw light, 0 at or beyond the profile's support radius.
 */
int light_profile_value(int mask, int distance_squared);

/**
 * One channel of an authored sRGB color in scene-linear Q0.16, normalized so
 * its strongest channel is 65535.
 * @param color Authored 0xRRGGBB; higher bits are ignored.
 * @param channel 0 red, 1 green, 2 blue.
 * @return Normalized channel; 0 for every channel of color 000000, which
 * positive sources must not use.
 */
uint16_t light_profile_color_component(uint32_t color, size_t channel);

/**
 * Convert raw light to pre-tone Q5.11 radiance.
 * @param raw_light Any value; non-positive light maps to 0 and raw 40959 or
 * more saturates to 65535.
 */
uint16_t light_profile_raw_to_radiance(int64_t raw_light);

#endif
