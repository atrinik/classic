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

/** @file Implements the operator-only /celestial lighting diagnostic. */

#include <global.h>

#include <celestial_override.h>
#include <initialization.h>
#include <light.h>
#include <object.h>
#include <player.h>
#include <region.h>
#include <server.h>
#include <server_main.h>
#include <tod.h>

#include <stdint.h>

#define CELESTIAL_COMMAND_USAGE \
    "Usage: /celestial status | phase <new|waxing-crescent|first-quarter|waxing-gibbous|full|waning-gibbous|last-quarter|waning-crescent> | age <hours> | clear"

static bool next_word(const char **cursor, char *word, size_t word_size) {
    const char *start;
    const char *finish;
    size_t length;

    HARD_ASSERT(cursor != NULL);
    HARD_ASSERT(word != NULL);
    HARD_ASSERT(word_size != 0);

    start = *cursor != NULL ? *cursor : "";
    while (*start != '\0' && (unsigned char)*start <= 0x7fU && isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '\0') {
        *cursor = start;
        *word = '\0';
        return false;
    }

    finish = start;
    while (*finish != '\0' &&
           !((unsigned char)*finish <= 0x7fU && isspace((unsigned char)*finish))) {
        finish++;
    }
    length = (size_t)(finish - start);
    if (length >= word_size) {
        *cursor = finish;
        *word = '\0';
        return false;
    }

    memcpy(word, start, length);
    word[length] = '\0';
    *cursor = finish;
    return true;
}

static bool no_more_words(const char **cursor) {
    const char *position = *cursor != NULL ? *cursor : "";
    while (*position != '\0' && (unsigned char)*position <= 0x7fU &&
           isspace((unsigned char)*position)) {
        position++;
    }
    return *position == '\0';
}

static void refresh_celestial_clients(void) {
    for (player *pl = first_player; pl != NULL; pl = pl->next) {
        if (pl->ob == NULL || pl->ob->map == NULL || pl->cs == NULL ||
            pl->cs->state != ST_PLAYING) {
            continue;
        }
        pl->cs->lastmap_light_generation = 0;
        draw_client_map2(pl->ob);
    }
}

static void draw_status(object *op) {
    celestial_override_state_t state;
    const region_celestial_profile_t *profile;
    celestial_lunar_input input;
    celestial_lunar_sample sample;
    uint16_t clock_age;

    celestial_override_get(&state);
    if (!state.active) {
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial override: inactive; calendar lunar state is authoritative "
                         "(revision=%" PRIu64 ").",
                         state.revision);
        return;
    }

    profile = op != NULL && op->map != NULL ? region_celestial_for_map(op->map) : NULL;
    if (profile == NULL) {
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial override: active mode=%s revision=%" PRIu64
                         "; no effective map profile is available.",
                         celestial_override_mode_name(state.mode),
                         state.revision);
        return;
    }

    region_celestial_lunar_input(profile, (uint64_t)todtick, &input);
    clock_age = input.lunar_age;
    if (!celestial_override_apply(profile->lunar_period, &input.lunar_age) ||
        !celestial_lunar_evaluate(&input, &sample)) {
        draw_info(COLOR_WHITE, op, "Celestial override has no valid effective sample.");
        return;
    }

    if (state.mode == CELESTIAL_OVERRIDE_MODE_AGE) {
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial override: active mode=age requested=%u/%u "
                         "clock_age=%u effective_age=%u/%u phase=%s illumination=%u "
                         "moon_hour=%u elevation=%" PRId32 " visible=%s moon_strength=%u "
                         "starlight_strength=%u revision=%" PRIu64 ".",
                         state.value,
                         state.period,
                         clock_age,
                         sample.revision.lunar_age,
                         profile->lunar_period,
                         celestial_override_phase_name(sample.phase),
                         sample.illumination,
                         sample.moon_hour,
                         sample.elevation,
                         sample.visible ? "yes" : "no",
                         sample.moon_strength,
                         sample.starlight_strength,
                         state.revision);
    } else {
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial override: active mode=phase requested=%s "
                         "clock_age=%u effective_age=%u/%u phase=%s illumination=%u "
                         "moon_hour=%u elevation=%" PRId32 " visible=%s moon_strength=%u "
                         "starlight_strength=%u revision=%" PRIu64 ".",
                         celestial_override_phase_name((celestial_lunar_phase)state.value),
                         clock_age,
                         sample.revision.lunar_age,
                         profile->lunar_period,
                         celestial_override_phase_name(sample.phase),
                         sample.illumination,
                         sample.moon_hour,
                         sample.elevation,
                         sample.visible ? "yes" : "no",
                         sample.moon_strength,
                         sample.starlight_strength,
                         state.revision);
    }
}

/** @copydoc command_func */
void command_celestial(object *op, const char *command, char *params) {
    const char *cursor = params != NULL ? params : "";
    char action[MAX_BUF];
    char value[MAX_BUF];

    if (!next_word(&cursor, action, sizeof(action))) {
        draw_status(op);
        return;
    }

    if (strcasecmp(action, "status") == 0) {
        if (!no_more_words(&cursor)) {
            draw_info(COLOR_WHITE, op, CELESTIAL_COMMAND_USAGE);
            return;
        }
        draw_status(op);
        return;
    }

    if (strcasecmp(action, "clear") == 0) {
        if (!no_more_words(&cursor)) {
            draw_info(COLOR_WHITE, op, CELESTIAL_COMMAND_USAGE);
            return;
        }
        if (!celestial_override_clear()) {
            draw_info(COLOR_WHITE, op, "Celestial override is already clear.");
            return;
        }
        celestial_light_invalidate_all();
        refresh_celestial_clients();
        draw_info(COLOR_WHITE, op, "Celestial override cleared; calendar lunar state restored.");
        return;
    }

    if (strcasecmp(action, "phase") == 0) {
        celestial_lunar_phase phase;
        if (!next_word(&cursor, value, sizeof(value)) || !no_more_words(&cursor) ||
            !celestial_override_parse_phase(value, &phase) ||
            !celestial_override_set_phase(phase)) {
            draw_info(COLOR_WHITE, op, CELESTIAL_COMMAND_USAGE);
            return;
        }
        celestial_light_invalidate_all();
        refresh_celestial_clients();
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial lunar override set to phase %s; use /celestial status "
                         "to inspect effective lighting.",
                         celestial_override_phase_name(phase));
        return;
    }

    if (strcasecmp(action, "age") == 0) {
        const region_celestial_profile_t *profile;
        uint16_t age;

        if (!next_word(&cursor, value, sizeof(value)) || !no_more_words(&cursor) ||
            !celestial_override_parse_age(value, &age)) {
            draw_info(COLOR_WHITE, op, CELESTIAL_COMMAND_USAGE);
            return;
        }
        profile = op != NULL && op->map != NULL ? region_celestial_for_map(op->map) : NULL;
        if (profile == NULL || age >= profile->lunar_period ||
            !celestial_override_set_age(age, profile->lunar_period)) {
            draw_info_format(COLOR_WHITE,
                             op,
                             "Age must be in the effective profile range 0..%u.",
                             profile != NULL ? profile->lunar_period - 1 : 0);
            return;
        }
        celestial_light_invalidate_all();
        refresh_celestial_clients();
        draw_info_format(COLOR_WHITE,
                         op,
                         "Celestial lunar override set to age %u/%u; use /celestial status "
                         "to inspect effective lighting.",
                         age,
                         profile->lunar_period);
        return;
    }

    draw_info(COLOR_WHITE, op, CELESTIAL_COMMAND_USAGE);
}
