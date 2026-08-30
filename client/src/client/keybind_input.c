/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Keybinding persistence compatibility and input matching helpers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <event.h>
#include <keybind.h>
#include <toolkit/toolkit.h>

/**
 * Translate an SDL 1.2 special-key value to its SDL3 keycode.
 *
 * Printable values are already Unicode keycodes in both versions. Values
 * not assigned by SDL 1.2 are retained so custom data is not discarded.
 */
SDL_Keycode keybind_keycode_from_legacy(uint32_t key) {
    static const SDL_Keycode keypad[] = {
        SDLK_KP_0,
        SDLK_KP_1,
        SDLK_KP_2,
        SDLK_KP_3,
        SDLK_KP_4,
        SDLK_KP_5,
        SDLK_KP_6,
        SDLK_KP_7,
        SDLK_KP_8,
        SDLK_KP_9,
    };
    static const SDL_Keycode function_keys[] = {
        SDLK_F1,
        SDLK_F2,
        SDLK_F3,
        SDLK_F4,
        SDLK_F5,
        SDLK_F6,
        SDLK_F7,
        SDLK_F8,
        SDLK_F9,
        SDLK_F10,
        SDLK_F11,
        SDLK_F12,
        SDLK_F13,
        SDLK_F14,
        SDLK_F15,
    };

    if (key >= 256 && key <= 265) {
        return keypad[key - 256];
    }
    if (key >= 282 && key <= 296) {
        return function_keys[key - 282];
    }

    switch (key) {
        case 266:
            return SDLK_KP_PERIOD;
        case 267:
            return SDLK_KP_DIVIDE;
        case 268:
            return SDLK_KP_MULTIPLY;
        case 269:
            return SDLK_KP_MINUS;
        case 270:
            return SDLK_KP_PLUS;
        case 271:
            return SDLK_KP_ENTER;
        case 272:
            return SDLK_KP_EQUALS;
        case 273:
            return SDLK_UP;
        case 274:
            return SDLK_DOWN;
        case 275:
            return SDLK_RIGHT;
        case 276:
            return SDLK_LEFT;
        case 277:
            return SDLK_INSERT;
        case 278:
            return SDLK_HOME;
        case 279:
            return SDLK_END;
        case 280:
            return SDLK_PAGEUP;
        case 281:
            return SDLK_PAGEDOWN;
        case 300:
            return SDLK_NUMLOCKCLEAR;
        case 301:
            return SDLK_CAPSLOCK;
        case 302:
            return SDLK_SCROLLLOCK;
        case 303:
            return SDLK_RSHIFT;
        case 304:
            return SDLK_LSHIFT;
        case 305:
            return SDLK_RCTRL;
        case 306:
            return SDLK_LCTRL;
        case 307:
            return SDLK_RALT;
        case 308:
            return SDLK_LALT;
        case 309:
            return SDLK_RGUI;
        case 310:
            return SDLK_LGUI;
        case 311:
            return SDLK_LGUI;
        case 312:
            return SDLK_RGUI;
        case 313:
            return SDLK_MODE;
        case 315:
            return SDLK_HELP;
        case 316:
            return SDLK_PRINTSCREEN;
        case 317:
            return SDLK_SYSREQ;
        case 318:
            return SDLK_PAUSE;
        case 319:
            return SDLK_MENU;
        case 320:
            return SDLK_POWER;
        case 321:
            return SDLK_CURRENCYUNIT;
        case 322:
            return SDLK_UNDO;
        default:
            return (SDL_Keycode)key;
    }
}

/** Parse a bounded persisted unsigned decimal value. */
bool keybind_uint32_parse(const char *text, uint32_t maximum, uint32_t *value_out) {
    char *end;
    unsigned long long value;

    if (text == NULL || value_out == NULL || *text == '\0' || *text == '-') {
        return false;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno == ERANGE || value > maximum || end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *value_out = (uint32_t)value;
    return true;
}

/** Parse a persisted decimal keycode. */
bool keybind_keycode_parse(const char *text, bool legacy, SDL_Keycode *key) {
    uint32_t value;

    if (key == NULL || !keybind_uint32_parse(text, UINT32_MAX, &value)) {
        return false;
    }

    *key = legacy ? keybind_keycode_from_legacy(value) : (SDL_Keycode)value;
    return true;
}

/** Normalize modifier sides for persistence and comparisons. */
SDL_Keymod keybind_adjust_kmod(SDL_Keymod mod) {
    mod &= SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI;

    if (mod & SDL_KMOD_SHIFT) {
        mod |= SDL_KMOD_SHIFT;
    }
    if (mod & SDL_KMOD_CTRL) {
        mod |= SDL_KMOD_CTRL;
    }
    if (mod & SDL_KMOD_ALT) {
        mod |= SDL_KMOD_ALT;
    }
    if (mod & SDL_KMOD_GUI) {
        mod |= SDL_KMOD_GUI;
    }

    return mod;
}

/** Check whether a keybinding matches a keyboard event. */
bool keybind_matches_event(const keybind_struct *keybind, const SDL_KeyboardEvent *event) {
    return keybind != NULL && event != NULL && event->key == keybind->key &&
           (!keybind->mod || keybind->mod == keybind_adjust_kmod(event->mod));
}

/** Append text to a fixed-size shortcut buffer. */
static void keybind_shortcut_append(char *buf, size_t len, size_t *used, const char *text) {
    size_t available = len - *used - 1;
    size_t amount = MIN(strlen(text), available);

    memcpy(buf + *used, text, amount);
    *used += amount;
    buf[*used] = '\0';
}

/** Construct a display string for a keybinding shortcut. */
char *keybind_get_key_shortcut(SDL_Keycode key, SDL_Keymod mod, char *buf, size_t len) {
    size_t used = 0;

    if (len == 0) {
        return buf;
    }
    buf[0] = '\0';

    if (mod & SDL_KMOD_SHIFT) {
        keybind_shortcut_append(buf, len, &used, "shift + ");
    }
    if (mod & SDL_KMOD_CTRL) {
        keybind_shortcut_append(buf, len, &used, "ctrl + ");
    }
    if (mod & SDL_KMOD_ALT) {
        keybind_shortcut_append(buf, len, &used, "alt + ");
    }
    if (mod & SDL_KMOD_GUI) {
        keybind_shortcut_append(buf, len, &used, "super + ");
    }
    if (key != SDLK_UNKNOWN) {
        keybind_shortcut_append(buf, len, &used, SDL_GetKeyName(key));
    }

    return buf;
}

/** Check whether a semicolon-separated binding contains an exact command. */
bool keybind_command_contains(const char *commands, const char *command) {
    if (commands == NULL || command == NULL || *command == '\0') {
        return false;
    }

    size_t command_len = strlen(command);
    const char *segment = commands;
    while (*segment != '\0') {
        while (*segment == ' ') {
            segment++;
        }
        const char *end = strchr(segment, ';');
        size_t segment_len = end == NULL ? strlen(segment) : (size_t)(end - segment);
        if (segment_len == command_len && !strncmp(segment, command, command_len)) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        segment = end + 1;
    }
    return false;
}

/** Check whether any physical binding containing a command is currently held. */
bool keybind_command_matches_held(keybind_struct *const *bindings,
                                  size_t bindings_num,
                                  const char *command,
                                  const key_struct *key_states,
                                  SDL_Keymod mod) {
    if (bindings == NULL || command == NULL || key_states == NULL) {
        return false;
    }

    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (size_t i = 0; i < bindings_num; i++) {
        if (!keybind_command_contains(bindings[i]->command, command)) {
            continue;
        }
        SDL_Scancode scancode = SDL_GetScancodeFromKey(bindings[i]->key, NULL);
        if (scancode != SDL_SCANCODE_UNKNOWN && key_states[scancode].pressed &&
            (!bindings[i]->mod || bindings[i]->mod == adjusted_mod)) {
            return true;
        }
    }
    return false;
}

/** Resolve a gameplay movement command to its keypad-style direction. */
bool keybind_movement_command_direction(const char *cmd, uint8_t *direction) {
    static const struct {
        const char *command;
        uint8_t direction;
    } commands[] = {
        {"?MOVE_N", 8},
        {"?MOVE_NE", 9},
        {"?MOVE_E", 6},
        {"?MOVE_SE", 3},
        {"?MOVE_S", 2},
        {"?MOVE_SW", 1},
        {"?MOVE_W", 4},
        {"?MOVE_NW", 7},
    };

    if (cmd == NULL || direction == NULL) {
        return false;
    }
    for (size_t i = 0; i < arraysize(commands); i++) {
        if (!strcmp(cmd, commands[i].command)) {
            *direction = commands[i].direction;
            return true;
        }
    }
    return false;
}

/** Reset a gameplay movement stream. */
void keybind_movement_state_init(keybind_movement_state *state) {
    memset(state, 0, sizeof(*state));
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
}

static uint8_t keybind_movement_direction(const keybind_movement_state *state);

/** Record one physical movement key-down in the logical stream. */
bool keybind_movement_state_press(keybind_movement_state *state,
                                  SDL_Scancode scancode,
                                  uint8_t direction,
                                  bool repeated,
                                  bool repeat) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT ||
        direction < 1 || direction > 9 || direction == 5) {
        return false;
    }

    keybind_movement_key *key = &state->keys[scancode];
    if (repeated) {
        if (!key->owned || !repeat) {
            return false;
        }
        key->repeat = repeat;
        key->preseeded = false;
        if (key->direction == 0) {
            key->order = ++state->next_order;
            key->direction = direction;
        }
        if (state->repeat_scancode == SDL_SCANCODE_UNKNOWN) {
            state->repeat_scancode = scancode;
        } else if (scancode != state->repeat_scancode) {
            return false;
        }
        key->direction = direction;
        state->repeated = true;
        state->pending_move = true;
        state->pending_move_repeated = true;
        state->pending_direction = keybind_movement_direction(state);
        if (!state->pending_stop_ordered) {
            state->pending_stop = false;
        }
        state->deferred_move = false;
        return true;
    }

    bool preseeded = key->preseeded && key->direction == direction;
    if (key->direction == 0) {
        key->order = ++state->next_order;
    }
    key->direction = direction;
    key->mod = SDL_KMOD_NONE;
    key->repeat = repeat;
    key->owned = true;
    key->preseeded = false;
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
    if (!preseeded || state->deferred_move) {
        state->pending_move = true;
        state->pending_move_repeated = false;
        state->pending_direction = keybind_movement_direction(state);
    }
    if (!state->pending_stop_ordered) {
        state->pending_stop = false;
    }
    state->deferred_move = false;
    return true;
}

/** Return whether the movement stream still has an active physical key. */
static bool keybind_movement_active(const keybind_movement_state *state) {
    for (SDL_Scancode i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (state->keys[i].direction != 0) {
            return true;
        }
    }
    return false;
}

/** Finish a released physical stream without canceling its first running step. */
static void keybind_movement_finish(keybind_movement_state *state, bool running, bool firing) {
    bool prior_stop = state->pending_stop;
    bool stop = prior_stop || ((state->repeated || running) && !firing);
    bool ordered_stop =
        prior_stop ? state->pending_stop_ordered : stop && running && !state->repeated;
    if (state->pending_run_stop) {
        stop = false;
        ordered_stop = false;
        state->pending_move = false;
        state->pending_move_repeated = false;
    } else if (ordered_stop && state->emitted_direction != 0) {
        state->pending_move = false;
        state->pending_move_repeated = false;
    }
    if (stop && !ordered_stop) {
        state->pending_move = false;
        state->pending_move_repeated = false;
    }
    state->pending_stop = stop;
    state->pending_stop_ordered = ordered_stop;
    state->repeated = false;
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
    if (!state->pending_stop && !state->pending_run_stop) {
        state->emitted_direction = 0;
        state->epoch = 0;
    }
}

/** Return whether a physical scancode participates in the movement stream. */
bool keybind_movement_state_has_scancode(const keybind_movement_state *state,
                                         SDL_Scancode scancode) {
    return state != NULL && scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT &&
           state->keys[scancode].direction != 0;
}

/** Record the modifier requirement of a selected movement binding. */
void keybind_movement_state_set_modifier(keybind_movement_state *state,
                                         SDL_Scancode scancode,
                                         SDL_Keymod mod) {
    if (keybind_movement_state_has_scancode(state, scancode)) {
        state->keys[scancode].mod = keybind_adjust_kmod(mod);
    }
}

/** Require the next accepted movement segment to emit the current batch. */
void keybind_movement_state_defer_move(keybind_movement_state *state) {
    if (state != NULL) {
        state->deferred_move = true;
    }
}

/** Drop a mode-induced deferral that had no later segment in its binding. */
void keybind_movement_state_cancel_deferred_move(keybind_movement_state *state) {
    if (state != NULL) {
        state->deferred_move = false;
    }
}

/** Separate explicitly ordered movement commands from held-stream replacement. */
void keybind_movement_state_ordered_boundary(keybind_movement_state *state) {
    if (state != NULL) {
        state->emitted_direction = 0;
        state->epoch = 0;
    }
}

/** Record a momentary RUN/FIRE owner accepted by gameplay dispatch. */
void keybind_movement_state_mode_pressed(keybind_movement_state *state,
                                         SDL_Scancode scancode,
                                         SDL_Keymod mod,
                                         bool run) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return;
    }
    if (run) {
        state->keys[scancode].run_owned = true;
        state->keys[scancode].run_mod = keybind_adjust_kmod(mod);
    } else {
        state->keys[scancode].fire_owned = true;
        state->keys[scancode].fire_mod = keybind_adjust_kmod(mod);
    }
}

/** Release one accepted momentary RUN/FIRE owner. */
bool keybind_movement_state_mode_released(keybind_movement_state *state,
                                          SDL_Scancode scancode,
                                          bool run) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return false;
    }
    bool *owned = run ? &state->keys[scancode].run_owned : &state->keys[scancode].fire_owned;
    bool released = *owned;
    *owned = false;
    if (run) {
        state->keys[scancode].run_mod = SDL_KMOD_NONE;
    } else {
        state->keys[scancode].fire_mod = SDL_KMOD_NONE;
    }
    return released;
}

/** Return whether gameplay dispatch has another momentary RUN/FIRE owner. */
bool keybind_movement_state_mode_owned(const keybind_movement_state *state, bool run) {
    if (state == NULL) {
        return false;
    }
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (run ? state->keys[scancode].run_owned : state->keys[scancode].fire_owned) {
            return true;
        }
    }
    return false;
}

/** Return whether releasing one key changes a momentary mode. */
bool keybind_movement_state_mode_release_changes(const keybind_movement_state *state,
                                                 SDL_Scancode scancode,
                                                 bool run) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT ||
        !(run ? state->keys[scancode].run_owned : state->keys[scancode].fire_owned)) {
        return false;
    }

    for (SDL_Scancode other = 1; other < SDL_SCANCODE_COUNT; other++) {
        if (other != scancode &&
            (run ? state->keys[other].run_owned : state->keys[other].fire_owned)) {
            return false;
        }
    }
    return true;
}

/** Transfer an accepted mode owner to its newly selected modifier binding. */
void keybind_movement_state_mode_rebind(keybind_movement_state *state,
                                        SDL_Scancode scancode,
                                        SDL_Keymod mod,
                                        bool run) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return;
    }
    keybind_movement_key *key = &state->keys[scancode];
    if (run && key->run_owned) {
        key->run_mod = keybind_adjust_kmod(mod);
    } else if (!run && key->fire_owned) {
        key->fire_mod = keybind_adjust_kmod(mod);
    }
}

/** Clear momentary owners when a toggle takes ownership of a mode. */
void keybind_movement_state_mode_clear(keybind_movement_state *state, bool run) {
    if (state == NULL) {
        return;
    }
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (run) {
            state->keys[scancode].run_owned = false;
            state->keys[scancode].run_mod = SDL_KMOD_NONE;
        } else {
            state->keys[scancode].fire_owned = false;
            state->keys[scancode].fire_mod = SDL_KMOD_NONE;
        }
    }
}

/** Return whether one accepted mode owner is invalid under a new modifier. */
bool keybind_movement_state_scancode_has_invalid_mode_modifier(const keybind_movement_state *state,
                                                               SDL_Scancode scancode,
                                                               SDL_Keymod mod) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return false;
    }
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    const keybind_movement_key *key = &state->keys[scancode];
    return (key->run_owned && key->run_mod != SDL_KMOD_NONE && key->run_mod != adjusted_mod) ||
           (key->fire_owned && key->fire_mod != SDL_KMOD_NONE && key->fire_mod != adjusted_mod);
}

/** Return whether modifier invalidation changes a momentary mode. */
bool keybind_movement_state_mode_modifier_changes(const keybind_movement_state *state,
                                                  SDL_Keymod mod) {
    if (state == NULL) {
        return false;
    }
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (int mode = 0; mode < 2; mode++) {
        bool invalid = false, valid = false;
        for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
            const keybind_movement_key *key = &state->keys[scancode];
            bool owned = mode == 0 ? key->run_owned : key->fire_owned;
            SDL_Keymod owner_mod = mode == 0 ? key->run_mod : key->fire_mod;
            if (!owned) {
                continue;
            }
            if (owner_mod != SDL_KMOD_NONE && owner_mod != adjusted_mod) {
                invalid = true;
            } else {
                valid = true;
            }
        }
        if (invalid && !valid) {
            return true;
        }
    }
    return false;
}

/** Release mode owners whose selected modifier binding is no longer valid. */
void keybind_movement_state_release_invalid_mode_modifiers(keybind_movement_state *state,
                                                           SDL_Keymod mod,
                                                           bool *run_released,
                                                           bool *fire_released) {
    if (state == NULL || run_released == NULL || fire_released == NULL) {
        return;
    }
    *run_released = false;
    *fire_released = false;
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (state->keys[scancode].run_owned && state->keys[scancode].run_mod != SDL_KMOD_NONE &&
            state->keys[scancode].run_mod != adjusted_mod) {
            state->keys[scancode].run_owned = false;
            state->keys[scancode].run_mod = SDL_KMOD_NONE;
            *run_released = true;
        }
        if (state->keys[scancode].fire_owned && state->keys[scancode].fire_mod != SDL_KMOD_NONE &&
            state->keys[scancode].fire_mod != adjusted_mod) {
            state->keys[scancode].fire_owned = false;
            state->keys[scancode].fire_mod = SDL_KMOD_NONE;
            *fire_released = true;
        }
    }
}

/** Atomically rebind or release movement entries invalidated by a modifier change. */
void keybind_movement_state_reconcile_modifiers(keybind_movement_state *state,
                                                SDL_Keymod mod,
                                                const keybind_movement_rebind *rebinds,
                                                size_t rebinds_num,
                                                bool force_move,
                                                bool defer_move,
                                                bool running,
                                                bool firing) {
    if (state == NULL) {
        return;
    }

    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    uint8_t old_direction = keybind_movement_direction(state);
    bool removed = false;
    for (SDL_Scancode scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (state->keys[scancode].direction != 0 && state->keys[scancode].mod != SDL_KMOD_NONE &&
            state->keys[scancode].mod != adjusted_mod) {
            const keybind_movement_rebind *rebind = NULL;
            for (size_t i = 0; i < rebinds_num; i++) {
                if (rebinds[i].scancode == scancode) {
                    rebind = &rebinds[i];
                    break;
                }
            }

            if (rebind != NULL) {
                state->keys[scancode].direction = rebind->direction;
                state->keys[scancode].mod = keybind_adjust_kmod(rebind->mod);
                state->keys[scancode].repeat = rebind->repeat;
                state->keys[scancode].preseeded = true;
            } else {
                state->keys[scancode].direction = 0;
                state->keys[scancode].order = 0;
                state->keys[scancode].mod = SDL_KMOD_NONE;
            }
            removed = true;
        }
    }
    for (size_t i = 0; i < rebinds_num; i++) {
        keybind_movement_key *key = &state->keys[rebinds[i].scancode];
        if (key->direction != 0) {
            continue;
        }
        key->order = ++state->next_order;
        key->direction = rebinds[i].direction;
        key->mod = keybind_adjust_kmod(rebinds[i].mod);
        key->repeat = rebinds[i].repeat;
        key->owned = true;
        key->preseeded = true;
        removed = true;
    }
    if (!removed) {
        return;
    }
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
    if (keybind_movement_active(state)) {
        uint8_t direction = keybind_movement_direction(state);
        state->pending_move = !defer_move && (force_move || direction != old_direction);
        state->deferred_move = defer_move;
        state->pending_move_repeated = false;
        state->pending_direction = direction;
        if (!state->pending_stop_ordered) {
            state->pending_stop = false;
        }
    } else {
        keybind_movement_finish(state, running, firing);
        state->deferred_move = false;
    }
}

/** Return whether modifier state invalidates an active movement binding. */
bool keybind_movement_state_has_invalid_modifier(const keybind_movement_state *state,
                                                 SDL_Keymod mod) {
    if (state == NULL) {
        return false;
    }

    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (SDL_Scancode scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (state->keys[scancode].direction != 0 && state->keys[scancode].mod != SDL_KMOD_NONE &&
            state->keys[scancode].mod != adjusted_mod) {
            return true;
        }
    }
    return false;
}

/** Release one physical movement key and schedule the resulting stream update. */
void keybind_movement_state_release(keybind_movement_state *state,
                                    SDL_Scancode scancode,
                                    bool running,
                                    bool firing) {
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return;
    }

    if (state->keys[scancode].direction == 0) {
        memset(&state->keys[scancode], 0, sizeof(state->keys[scancode]));
        return;
    }

    memset(&state->keys[scancode], 0, sizeof(state->keys[scancode]));
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;

    if (keybind_movement_active(state)) {
        state->pending_move = true;
        state->pending_move_repeated = false;
        state->pending_direction = keybind_movement_direction(state);
        if (!state->pending_stop_ordered) {
            state->pending_stop = false;
        }
    } else {
        keybind_movement_finish(state, running, firing);
    }
}

/** Release every physical movement key as one logical transition. */
void keybind_movement_state_clear(keybind_movement_state *state, bool running, bool firing) {
    if (state == NULL) {
        return;
    }

    bool stop = state->pending_stop || state->pending_run_stop ||
                (keybind_movement_active(state) && (state->repeated || running) && !firing);
    uint64_t next_order = state->next_order;
    uint32_t next_epoch = state->next_epoch;
    uint32_t epoch = state->epoch;
    keybind_movement_state_init(state);
    state->next_order = next_order;
    state->next_epoch = next_epoch;
    state->epoch = stop ? epoch : 0;
    state->pending_stop = stop;
    state->pending_stop_ordered = false;
}

/** Schedule a run-stream stop unless movement already stopped it. */
void keybind_movement_state_run_released(keybind_movement_state *state, bool run_stream_active) {
    if (state != NULL && run_stream_active && !state->pending_stop) {
        if (state->emitted_direction != 0 && !state->repeated) {
            state->pending_stop = true;
            state->pending_stop_ordered = true;
        } else {
            state->pending_run_stop = true;
        }
    }
}

/** Resolve two perpendicular active directions as one normalized movement vector. */
static uint8_t keybind_movement_direction(const keybind_movement_state *state) {
    static const int8_t direction_x[] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int8_t direction_y[] = {0, 1, 1, 1, 0, 0, 0, -1, -1, -1};
    uint16_t directions = 0;
    uint8_t newest_direction = 0;
    uint64_t newest_order = 0;
    size_t directions_num = 0;

    for (SDL_Scancode i = 0; i < SDL_SCANCODE_COUNT; i++) {
        const keybind_movement_key *key = &state->keys[i];
        if (key->direction != 0) {
            uint16_t direction_bit = (uint16_t)(1U << key->direction);
            if ((directions & direction_bit) == 0) {
                directions |= direction_bit;
                directions_num++;
            }
            if (key->order >= newest_order) {
                newest_order = key->order;
                newest_direction = key->direction;
            }
        }
    }

    if (directions_num == 2) {
        int x = 0, y = 0;
        uint8_t first = 0, second = 0;
        for (uint8_t direction = 1; direction <= 9; direction++) {
            if ((directions & (1U << direction)) != 0) {
                if (first == 0) {
                    first = direction;
                } else {
                    second = direction;
                }
                x += direction_x[direction];
                y += direction_y[direction];
            }
        }
        int dot =
            direction_x[first] * direction_x[second] + direction_y[first] * direction_y[second];
        if (dot == 0) {
            if (y > 0) {
                return x < 0 ? 1 : x > 0 ? 3 : 2;
            }
            if (y < 0) {
                return x < 0 ? 7 : x > 0 ? 9 : 8;
            }
            return x < 0 ? 4 : 6;
        }
    }
    return newest_direction;
}

/** Consume a pending final movement stop. */
static keybind_movement_action keybind_movement_state_flush_stop(keybind_movement_state *state,
                                                                 uint8_t *direction,
                                                                 uint32_t *epoch) {
    bool ordered = state->pending_stop_ordered;
    state->pending_stop = false;
    state->pending_stop_ordered = false;
    state->pending_run_stop = false;
    state->emitted_direction = 0;
    *direction = ordered ? 0 : 5;
    *epoch = state->epoch;
    state->epoch = 0;
    return ordered ? KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP : KEYBIND_MOVEMENT_ACTION_STOP;
}

/** Consume one pending logical movement-stream update. */
keybind_movement_action
keybind_movement_state_flush(keybind_movement_state *state, uint8_t *direction, uint32_t *epoch) {
    if (state == NULL || direction == NULL || epoch == NULL) {
        return KEYBIND_MOVEMENT_ACTION_NONE;
    }
    *epoch = state->epoch;
    if (state->pending_run_stop) {
        state->pending_run_stop = false;
        state->repeated = state->pending_move && state->pending_move_repeated;
        state->emitted_direction = 0;
        *direction = 0;
        return KEYBIND_MOVEMENT_ACTION_RUN_STOP;
    }
    if (state->pending_stop && state->pending_stop_ordered && state->emitted_direction != 0) {
        return keybind_movement_state_flush_stop(state, direction, epoch);
    }
    if (state->pending_move) {
        state->pending_move = false;
        state->pending_move_repeated = false;
        *direction = state->pending_direction;
        if (*direction != 0) {
            bool replace = state->emitted_direction != 0 && state->emitted_direction != *direction;
            if (state->emitted_direction == 0) {
                state->next_epoch++;
                if (state->next_epoch == 0) {
                    state->next_epoch++;
                }
                state->epoch = state->next_epoch;
            }
            state->emitted_direction = *direction;
            *epoch = state->epoch;
            return replace ? KEYBIND_MOVEMENT_ACTION_REPLACE : KEYBIND_MOVEMENT_ACTION_MOVE;
        }
    }
    if (state->pending_stop) {
        return keybind_movement_state_flush_stop(state, direction, epoch);
    }
    return KEYBIND_MOVEMENT_ACTION_NONE;
}
