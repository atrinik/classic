/*************************************************************************
 * Atrinik client visibility field and transient fade helpers.
 *
 * Copyright 2026 The Atrinik Project
 *************************************************************************/

#ifndef MAP_VISIBILITY_H
#define MAP_VISIBILITY_H

#include <stdbool.h>
#include <stdint.h>

/** Fixed player-field constants from the remembered-world contract. */
#define MAP_VISIBILITY_FIELD_UNIT UINT16_C(256)
#define MAP_VISIBILITY_INNER_RADIUS_SQUARED UINT16_C(16)
#define MAP_VISIBILITY_OUTER_RADIUS_SQUARED UINT16_C(64)
#define MAP_VISIBILITY_PLAYER_RADIANCE UINT16_C(640)
#define MAP_VISIBILITY_FADE_DURATION_MS UINT32_C(250)
#define MAP_VISIBILITY_STALE_BOUND_MS UINT32_C(500)
#define MAP_VISIBILITY_INTERACTION_CUTOFF UINT8_C(192)

/** One presentation-only alpha transition for a live MAP2 record. */
typedef struct map_visibility_fade {
    uint8_t alpha;
    uint8_t from_alpha;
    uint8_t target_alpha;
    uint32_t transition_started;
    uint32_t last_authoritative_update;
    bool initialized;
    bool authorized;
} map_visibility_fade_t;

/** Return the fixed-point radial field weight for one map-coordinate vector. */
uint16_t map_visibility_field_weight(int dx, int dy);

/** Return the fixed-point radial field weight for an exact squared distance. */
uint16_t map_visibility_field_weight_squared(uint32_t distance_squared);

/** Add the presentation-only player contribution to one radiance sample. */
uint16_t map_visibility_add_player_radiance(uint16_t radiance, uint16_t weight);

/** Convert a field weight into the corresponding presentation alpha. */
uint8_t map_visibility_field_alpha(uint16_t weight);

/** Initialize a presentation transition. */
void map_visibility_fade_init(map_visibility_fade_t *fade);

/** Record one authoritative live MAP2 update and begin/continue fade-in. */
void map_visibility_fade_authorize(map_visibility_fade_t *fade,
                                   uint8_t target_alpha,
                                   uint32_t now);

/** Change the presentation target without fabricating authoritative state. */
void map_visibility_fade_set_target(map_visibility_fade_t *fade,
                                    uint8_t target_alpha,
                                    uint32_t now);

/** Remove authorization and begin fade-out. */
void map_visibility_fade_revoke(map_visibility_fade_t *fade, uint32_t now);

/** Advance one transition; return true when the visible alpha changed. */
bool map_visibility_fade_advance(map_visibility_fade_t *fade, uint32_t now);

/** Return whether annotations and interaction metadata may be exposed. */
bool map_visibility_fade_interactive(const map_visibility_fade_t *fade);

#endif
