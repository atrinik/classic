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

#ifndef CELESTIAL_OVERRIDE_H
#define CELESTIAL_OVERRIDE_H

#include <celestial_lunar.h>

#include <stdbool.h>
#include <stdint.h>

#define CELESTIAL_OVERRIDE_LUNAR_PERIOD_MIN UINT16_C(168)
#define CELESTIAL_OVERRIDE_LUNAR_PERIOD_MAX UINT16_C(8064)

typedef enum celestial_override_mode {
    CELESTIAL_OVERRIDE_MODE_NONE,
    CELESTIAL_OVERRIDE_MODE_AGE,
    CELESTIAL_OVERRIDE_MODE_PHASE,
} celestial_override_mode_t;

typedef struct celestial_override_state {
    bool active;
    celestial_override_mode_t mode;
    /** Age, or a value from celestial_lunar_phase, depending on mode. */
    uint16_t value;
    /** Source profile period for an explicit age; zero for a named phase. */
    uint16_t period;
    /** Monotonic process-local identity for cache invalidation. */
    uint64_t revision;
} celestial_override_state_t;

/** Return the process-local revision of the diagnostic override state. */
extern uint64_t celestial_override_revision(void);

/** Copy the process-local diagnostic override state. */
extern void celestial_override_get(celestial_override_state_t *state);

/** Set an explicit age in the supplied effective profile period. */
extern bool celestial_override_set_age(uint16_t age, uint16_t period);

/** Set one of the eight named lunar phase anchors. */
extern bool celestial_override_set_phase(celestial_lunar_phase phase);

/** Clear the override; return false when it was already clear. */
extern bool celestial_override_clear(void);

/** Resolve the active override into a target profile's lunar period. */
extern bool celestial_override_apply(uint16_t period, uint16_t *age);

/** Parse one non-negative decimal age without truncating or wrapping. */
extern bool celestial_override_parse_age(const char *text, uint16_t *age);

/** Return stable diagnostic names for modes and lunar phases. */
extern const char *celestial_override_mode_name(celestial_override_mode_t mode);
extern const char *celestial_override_phase_name(celestial_lunar_phase phase);

/** Parse a canonical lower-case or case-insensitive phase name. */
extern bool celestial_override_parse_phase(const char *name, celestial_lunar_phase *phase);

#endif
