/*************************************************************************
 * Atrinik client visibility field and transient fade helpers.
 *
 * Copyright 2026 The Atrinik Project
 *************************************************************************/

#include <global.h>
#include <map_visibility.h>

static uint8_t map_visibility_fade_value(uint8_t from,
                                         uint8_t target,
                                         uint32_t elapsed) {
    if (elapsed >= MAP_VISIBILITY_FADE_DURATION_MS || from == target) {
        return target;
    }

    uint32_t distance = from > target ? from - target : target - from;
    uint32_t step = (distance * elapsed + MAP_VISIBILITY_FADE_DURATION_MS / 2U) /
                    MAP_VISIBILITY_FADE_DURATION_MS;
    if (from < target) {
        return (uint8_t)MIN(UINT8_MAX, (uint32_t)from + step);
    }
    return (uint8_t)(step > from ? 0 : from - step);
}

uint16_t map_visibility_field_weight_squared(uint32_t distance_squared) {
    if (distance_squared <= MAP_VISIBILITY_INNER_RADIUS_SQUARED) {
        return MAP_VISIBILITY_FIELD_UNIT;
    }
    if (distance_squared >= MAP_VISIBILITY_OUTER_RADIUS_SQUARED) {
        return 0;
    }

    int64_t remaining = MAP_VISIBILITY_OUTER_RADIUS_SQUARED - distance_squared;
    return (uint16_t)((2 * remaining * MAP_VISIBILITY_FIELD_UNIT + 48) / (2 * 48));
}

uint16_t map_visibility_field_weight(int dx, int dy) {
    int64_t distance_squared = (int64_t)dx * dx + (int64_t)dy * dy;
    if (distance_squared < 0 || (uint64_t)distance_squared > UINT32_MAX) {
        return 0;
    }
    return map_visibility_field_weight_squared((uint32_t)distance_squared);
}

uint16_t map_visibility_add_player_radiance(uint16_t radiance, uint16_t weight) {
    /* MAP2 samples and lighting vertices are Q5.11.  The contract's raw
     * player contribution is 640, which encodes exactly as 1024. */
    const uint32_t player_radiance_q5_11 =
        (MAP_VISIBILITY_PLAYER_RADIANCE * 8U) / 5U;
    uint32_t addition = (player_radiance_q5_11 * weight + MAP_VISIBILITY_FIELD_UNIT / 2U) /
                        MAP_VISIBILITY_FIELD_UNIT;
    return (uint16_t)MIN(UINT16_MAX, (uint32_t)radiance + addition);
}

uint8_t map_visibility_field_alpha(uint16_t weight) {
    return (uint8_t)MIN(UINT8_MAX,
                        (weight * (uint32_t)UINT8_MAX + MAP_VISIBILITY_FIELD_UNIT / 2U) /
                            MAP_VISIBILITY_FIELD_UNIT);
}

void map_visibility_fade_init(map_visibility_fade_t *fade) {
    HARD_ASSERT(fade != NULL);
    memset(fade, 0, sizeof(*fade));
}

void map_visibility_fade_set_target(map_visibility_fade_t *fade,
                                    uint8_t target_alpha,
                                    uint32_t now) {
    HARD_ASSERT(fade != NULL);
    if (!fade->initialized) {
        map_visibility_fade_init(fade);
        fade->initialized = true;
        fade->transition_started = now;
        fade->from_alpha = fade->alpha;
    }
    if (fade->target_alpha == target_alpha) {
        return;
    }
    fade->from_alpha = fade->alpha;
    fade->target_alpha = target_alpha;
    fade->transition_started = now;
}

void map_visibility_fade_authorize(map_visibility_fade_t *fade,
                                   uint8_t target_alpha,
                                   uint32_t now) {
    HARD_ASSERT(fade != NULL);
    if (!fade->initialized) {
        map_visibility_fade_init(fade);
        fade->initialized = true;
    }
    fade->authorized = true;
    fade->last_authoritative_update = now;
    map_visibility_fade_set_target(fade, target_alpha, now);
}

void map_visibility_fade_revoke(map_visibility_fade_t *fade, uint32_t now) {
    HARD_ASSERT(fade != NULL);
    if (!fade->initialized) {
        return;
    }
    fade->authorized = false;
    map_visibility_fade_set_target(fade, 0, now);
}

bool map_visibility_fade_advance(map_visibility_fade_t *fade, uint32_t now) {
    HARD_ASSERT(fade != NULL);
    if (!fade->initialized) {
        return false;
    }
    if (fade->authorized && now - fade->last_authoritative_update >=
                                MAP_VISIBILITY_STALE_BOUND_MS) {
        map_visibility_fade_revoke(fade, now);
    }

    uint8_t before = fade->alpha;
    fade->alpha = map_visibility_fade_value(fade->from_alpha,
                                            fade->target_alpha,
                                            now - fade->transition_started);
    return before != fade->alpha;
}

bool map_visibility_fade_interactive(const map_visibility_fade_t *fade) {
    HARD_ASSERT(fade != NULL);
    return fade->authorized && fade->alpha >= MAP_VISIBILITY_INTERACTION_CUTOFF;
}
