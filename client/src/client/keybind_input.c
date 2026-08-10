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

#include <global.h>

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
        if (key->direction == 0 || !key->repeat) {
            return false;
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
        state->pending_stop = false;
        return true;
    }

    if (key->direction == 0) {
        key->order = ++state->next_order;
    }
    key->direction = direction;
    key->mod = SDL_KMOD_NONE;
    key->repeat = repeat;
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
    state->pending_move = true;
    state->pending_move_repeated = false;
    state->pending_direction = keybind_movement_direction(state);
    state->pending_stop = false;
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

/** Release movement entries whose selected modifier binding is no longer valid. */
void keybind_movement_state_release_invalid_modifiers(keybind_movement_state *state,
                                                      SDL_Keymod mod,
                                                      bool running,
                                                      bool firing) {
    if (state == NULL) {
        return;
    }

    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (SDL_Scancode scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (state->keys[scancode].direction != 0 && state->keys[scancode].mod != SDL_KMOD_NONE &&
            state->keys[scancode].mod != adjusted_mod) {
            keybind_movement_state_release(state, scancode, running, firing);
        }
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
    if (state == NULL || scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT ||
        state->keys[scancode].direction == 0) {
        return;
    }

    memset(&state->keys[scancode], 0, sizeof(state->keys[scancode]));
    state->repeat_scancode = SDL_SCANCODE_UNKNOWN;

    if (keybind_movement_active(state)) {
        state->pending_move = true;
        state->pending_move_repeated = false;
        state->pending_direction = keybind_movement_direction(state);
        state->pending_stop = false;
    } else {
        bool stop = (state->repeated || running) && !firing;
        if (stop) {
            state->pending_move = false;
        }
        state->pending_stop = stop;
        state->repeated = false;
        state->repeat_scancode = SDL_SCANCODE_UNKNOWN;
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
    keybind_movement_state_init(state);
    state->next_order = next_order;
    state->pending_stop = stop;
}

/** Schedule a run-stream stop unless movement already stopped it. */
void keybind_movement_state_run_released(keybind_movement_state *state, bool run_stream_active) {
    if (state != NULL && run_stream_active && !state->pending_stop) {
        state->pending_run_stop = true;
    }
}

/** Resolve the active directions, composing only the four supported diagonal pairs. */
static uint8_t keybind_movement_direction(const keybind_movement_state *state) {
    uint16_t directions = 0;
    uint8_t newest_direction = 0;
    uint64_t newest_order = 0;

    for (SDL_Scancode i = 0; i < SDL_SCANCODE_COUNT; i++) {
        const keybind_movement_key *key = &state->keys[i];
        if (key->direction != 0) {
            directions |= (uint16_t)(1U << key->direction);
            if (key->order >= newest_order) {
                newest_order = key->order;
                newest_direction = key->direction;
            }
        }
    }

    if (directions == ((1U << 7) | (1U << 9))) {
        return 8;
    }
    if (directions == ((1U << 9) | (1U << 3))) {
        return 6;
    }
    if (directions == ((1U << 3) | (1U << 1))) {
        return 2;
    }
    if (directions == ((1U << 1) | (1U << 7))) {
        return 4;
    }
    return newest_direction;
}

/** Consume one pending logical movement-stream update. */
keybind_movement_action keybind_movement_state_flush(keybind_movement_state *state,
                                                     uint8_t *direction) {
    if (state == NULL || direction == NULL) {
        return KEYBIND_MOVEMENT_ACTION_NONE;
    }
    if (state->pending_run_stop) {
        state->pending_run_stop = false;
        state->repeated = state->pending_move && state->pending_move_repeated;
        *direction = 0;
        return KEYBIND_MOVEMENT_ACTION_RUN_STOP;
    }
    if (state->pending_move) {
        state->pending_move = false;
        state->pending_move_repeated = false;
        *direction = state->pending_direction;
        if (*direction != 0) {
            return KEYBIND_MOVEMENT_ACTION_MOVE;
        }
    }
    if (state->pending_stop) {
        state->pending_stop = false;
        state->pending_run_stop = false;
        *direction = 5;
        return KEYBIND_MOVEMENT_ACTION_STOP;
    }
    return KEYBIND_MOVEMENT_ACTION_NONE;
}
