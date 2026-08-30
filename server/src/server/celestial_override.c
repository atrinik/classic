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

/** @file Process-local lunar override state for authorized diagnostics. */

#include <global.h>

#include <celestial_override.h>

#include <errno.h>
#include <stdlib.h>

static celestial_override_state_t override_state;

static const char *const phase_names[CELESTIAL_LUNAR_PHASE_COUNT] = {
    [CELESTIAL_LUNAR_NEW] = "new",
    [CELESTIAL_LUNAR_WAXING_CRESCENT] = "waxing-crescent",
    [CELESTIAL_LUNAR_FIRST_QUARTER] = "first-quarter",
    [CELESTIAL_LUNAR_WAXING_GIBBOUS] = "waxing-gibbous",
    [CELESTIAL_LUNAR_FULL] = "full",
    [CELESTIAL_LUNAR_WANING_GIBBOUS] = "waning-gibbous",
    [CELESTIAL_LUNAR_LAST_QUARTER] = "last-quarter",
    [CELESTIAL_LUNAR_WANING_CRESCENT] = "waning-crescent",
};

static bool period_valid(uint16_t period) {
    return period >= CELESTIAL_OVERRIDE_LUNAR_PERIOD_MIN &&
           period <= CELESTIAL_OVERRIDE_LUNAR_PERIOD_MAX && period % 24 == 0 && period % 8 == 0;
}

static void revision_bump(void) {
    if (override_state.revision == UINT64_MAX) {
        HARD_ASSERT(false);
    }
    override_state.revision++;
}

uint64_t celestial_override_revision(void) {
    return override_state.revision;
}

void celestial_override_get(celestial_override_state_t *state) {
    HARD_ASSERT(state != NULL);
    *state = override_state;
}

bool celestial_override_set_age(uint16_t age, uint16_t period) {
    if (!period_valid(period) || age >= period) {
        return false;
    }

    override_state.active = true;
    override_state.mode = CELESTIAL_OVERRIDE_MODE_AGE;
    override_state.value = age;
    override_state.period = period;
    revision_bump();
    return true;
}

bool celestial_override_set_phase(celestial_lunar_phase phase) {
    if (phase < CELESTIAL_LUNAR_NEW || phase >= CELESTIAL_LUNAR_PHASE_COUNT) {
        return false;
    }

    override_state.active = true;
    override_state.mode = CELESTIAL_OVERRIDE_MODE_PHASE;
    override_state.value = (uint16_t)phase;
    override_state.period = 0;
    revision_bump();
    return true;
}

bool celestial_override_clear(void) {
    if (!override_state.active) {
        return false;
    }

    override_state.active = false;
    override_state.mode = CELESTIAL_OVERRIDE_MODE_NONE;
    override_state.value = 0;
    override_state.period = 0;
    revision_bump();
    return true;
}

bool celestial_override_apply(uint16_t period, uint16_t *age) {
    if (!override_state.active || age == NULL || !period_valid(period)) {
        return false;
    }

    if (override_state.mode == CELESTIAL_OVERRIDE_MODE_PHASE) {
        *age = (uint16_t)(((uint32_t)override_state.value * period) / 8U);
        return true;
    }

    if (override_state.mode != CELESTIAL_OVERRIDE_MODE_AGE ||
        !period_valid(override_state.period) || override_state.value >= override_state.period) {
        return false;
    }

    *age = (uint16_t)(((uint32_t)override_state.value * period) / override_state.period);
    return *age < period;
}

bool celestial_override_parse_age(const char *text, uint16_t *age) {
    char *end;
    unsigned long long parsed;

    if (text == NULL || age == NULL || *text == '\0') {
        return false;
    }
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *age = (uint16_t)parsed;
    return true;
}

const char *celestial_override_mode_name(celestial_override_mode_t mode) {
    switch (mode) {
        case CELESTIAL_OVERRIDE_MODE_AGE:
            return "age";
        case CELESTIAL_OVERRIDE_MODE_PHASE:
            return "phase";
        default:
            return "none";
    }
}

const char *celestial_override_phase_name(celestial_lunar_phase phase) {
    return phase >= CELESTIAL_LUNAR_NEW && phase < CELESTIAL_LUNAR_PHASE_COUNT
               ? phase_names[phase]
               : "invalid";
}

bool celestial_override_parse_phase(const char *name, celestial_lunar_phase *phase) {
    if (name == NULL || phase == NULL) {
        return false;
    }

    for (celestial_lunar_phase candidate = CELESTIAL_LUNAR_NEW;
         candidate < CELESTIAL_LUNAR_PHASE_COUNT;
         candidate++) {
        if (strcasecmp(name, phase_names[candidate]) == 0) {
            *phase = candidate;
            return true;
        }
    }
    return false;
}
