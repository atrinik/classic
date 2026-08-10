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
 * Testable physical keybinding dispatch and movement-stream integration.
 */

#include <global.h>

static bool keybind_event_running(const keybind_event_handler *handler) {
    return handler->running != NULL && handler->running(handler->user_data);
}

static bool keybind_event_firing(const keybind_event_handler *handler) {
    return handler->firing != NULL && handler->firing(handler->user_data);
}

static void keybind_event_flush(const keybind_event_handler *handler) {
    if (handler->flush != NULL) {
        handler->flush(handler->user_data);
    }
}

/** Process one already-selected physical keybinding. */
void keybind_event_process_binding(const keybind_struct *keybind,
                                   const SDL_KeyboardEvent *event,
                                   const keybind_event_handler *handler) {
    char command[MAX_BUF], *cp;
    bool movement_segment_seen = false;
    bool movement_accepted = false, cancel_mode_deferral = false;

    if (keybind == NULL || event == NULL || handler == NULL || handler->movement == NULL ||
        (!keybind->repeat && event->repeat)) {
        return;
    }

    strncpy(command, keybind->command, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';

    cp = strtok(command, ";");
    while (cp != NULL) {
        while (*cp == ' ') {
            cp++;
        }

        uint8_t direction;
        if (keybind_movement_command_direction(cp, &direction)) {
            if (movement_segment_seen) {
                keybind_event_flush(handler);
            }
            movement_segment_seen = true;
            if (event->type == SDL_EVENT_KEY_DOWN) {
                if (handler->movement_intercept_matches != NULL &&
                    handler->movement_intercept_matches(cp, handler->user_data)) {
                    keybind_event_flush(handler);
                    if (handler->movement_intercept != NULL) {
                        handler->movement_intercept(cp, handler->user_data);
                    }
                    cp = strtok(NULL, ";");
                    continue;
                }
                bool accepted = keybind_movement_state_press(handler->movement,
                                                             event->scancode,
                                                             direction,
                                                             event->repeat,
                                                             keybind->repeat);
                if (accepted) {
                    movement_accepted = true;
                    keybind_movement_state_set_modifier(handler->movement,
                                                        event->scancode,
                                                        keybind->mod);
                }
                if (accepted && event->repeat) {
                    keybind_event_flush(handler);
                }
            } else {
                keybind_movement_state_release(handler->movement,
                                               event->scancode,
                                               keybind_event_running(handler),
                                               keybind_event_firing(handler));
            }
        } else {
            bool run = !strcmp(cp, "?RUNON");
            bool fire = !strcmp(cp, "?FIREON");
            bool mode_up_changes = event->type == SDL_EVENT_KEY_UP && (run || fire) &&
                                   keybind_movement_state_mode_release_changes(handler->movement,
                                                                               event->scancode,
                                                                               run);
            if (event->type == SDL_EVENT_KEY_DOWN || mode_up_changes) {
                keybind_event_flush(handler);
            }
            if (event->type == SDL_EVENT_KEY_DOWN) {
                if (handler->command_down != NULL) {
                    bool running = keybind_event_running(handler);
                    bool firing = keybind_event_firing(handler);
                    bool accepted = handler->command_down(cp, handler->user_data);
                    if (accepted && !strcmp(cp, "?RUNON")) {
                        keybind_movement_state_mode_pressed(handler->movement,
                                                            event->scancode,
                                                            keybind->mod,
                                                            true);
                    } else if (accepted && !strcmp(cp, "?FIREON")) {
                        keybind_movement_state_mode_pressed(handler->movement,
                                                            event->scancode,
                                                            keybind->mod,
                                                            false);
                    } else if (accepted && !strcmp(cp, "?RUNON_TOGGLE")) {
                        keybind_movement_state_mode_clear(handler->movement, true);
                    } else if (accepted && !strcmp(cp, "?FIREON_TOGGLE")) {
                        keybind_movement_state_mode_clear(handler->movement, false);
                    }
                    if (running != keybind_event_running(handler) ||
                        firing != keybind_event_firing(handler)) {
                        keybind_movement_state_defer_move(handler->movement);
                        cancel_mode_deferral = movement_accepted;
                    }
                }
            } else if (handler->command_up != NULL) {
                bool released = false;
                if (run || fire) {
                    released = keybind_movement_state_mode_released(handler->movement,
                                                                    event->scancode,
                                                                    run);
                }
                if ((!run && !fire) ||
                    (released && !keybind_movement_state_mode_owned(handler->movement, run))) {
                    handler->command_up(cp, handler->user_data);
                }
            }
        }

        cp = strtok(NULL, ";");
    }
    if (cancel_mode_deferral && handler->movement->deferred_move) {
        keybind_movement_state_cancel_deferred_move(handler->movement);
    }
}

static const keybind_struct *keybind_event_find(keybind_struct *const *bindings,
                                                size_t bindings_num,
                                                const SDL_KeyboardEvent *event) {
    for (size_t i = 0; i < bindings_num; i++) {
        if (event->key == bindings[i]->key && bindings[i]->mod == keybind_adjust_kmod(event->mod)) {
            return bindings[i];
        }
    }

    for (size_t i = 0; i < bindings_num; i++) {
        if (event->key == bindings[i]->key && !bindings[i]->mod) {
            return bindings[i];
        }
    }

    return NULL;
}

static const keybind_struct *keybind_event_find_scancode(keybind_struct *const *bindings,
                                                         size_t bindings_num,
                                                         SDL_Scancode scancode,
                                                         SDL_Keymod mod) {
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    for (size_t i = 0; i < bindings_num; i++) {
        if (SDL_GetScancodeFromKey(bindings[i]->key, NULL) == scancode &&
            bindings[i]->mod == adjusted_mod) {
            return bindings[i];
        }
    }

    for (size_t i = 0; i < bindings_num; i++) {
        if (SDL_GetScancodeFromKey(bindings[i]->key, NULL) == scancode && !bindings[i]->mod) {
            return bindings[i];
        }
    }

    return NULL;
}

static bool keybind_event_simple_movement(const keybind_struct *keybind,
                                          uint8_t *direction,
                                          bool *has_movement,
                                          bool *will_move,
                                          char *movement_command,
                                          size_t movement_command_size,
                                          const keybind_event_handler *handler) {
    char command[MAX_BUF], *cp;
    bool found = false, simple = true;

    strncpy(command, keybind->command, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    cp = strtok(command, ";");
    while (cp != NULL) {
        while (*cp == ' ') {
            cp++;
        }
        uint8_t candidate;
        if (keybind_movement_command_direction(cp, &candidate)) {
            if (found) {
                simple = false;
            } else {
                found = true;
                strncpy(movement_command, cp, movement_command_size - 1);
                movement_command[movement_command_size - 1] = '\0';
            }
            bool intercepted = handler->movement_intercept_matches != NULL &&
                               handler->movement_intercept_matches(cp, handler->user_data);
            if (!intercepted && !*will_move) {
                *direction = candidate;
                *will_move = true;
            }
        } else if (*cp != '\0') {
            simple = false;
        }
        cp = strtok(NULL, ";");
    }

    *has_movement = found;
    return found && simple;
}

static size_t keybind_event_modifier_rebinds(keybind_struct *const *bindings,
                                             size_t bindings_num,
                                             const key_struct *key_states,
                                             const SDL_KeyboardEvent *event,
                                             const keybind_event_handler *handler,
                                             const bool *invalid_mode_scancodes,
                                             keybind_movement_rebind *rebinds,
                                             const keybind_struct **compound_bindings,
                                             SDL_KeyboardEvent *compound_events,
                                             size_t *compound_num,
                                             size_t *compound_moves) {
    if (key_states == NULL) {
        return 0;
    }

    size_t rebinds_num = 0;
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(event->mod);
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        const keybind_movement_key *movement_key = &handler->movement->keys[scancode];
        bool invalid_movement = movement_key->direction != 0 &&
                                movement_key->mod != SDL_KMOD_NONE &&
                                movement_key->mod != adjusted_mod;
        if (!key_states[scancode].pressed) {
            continue;
        }
        if (!invalid_movement && !invalid_mode_scancodes[scancode]) {
            continue;
        }
        const keybind_struct *keybind =
            keybind_event_find_scancode(bindings, bindings_num, scancode, event->mod);
        if (keybind == NULL) {
            continue;
        }
        uint8_t direction;
        bool has_movement, will_move = false;
        char movement_command[MAX_BUF];
        bool simple = keybind_event_simple_movement(keybind,
                                                    &direction,
                                                    &has_movement,
                                                    &will_move,
                                                    movement_command,
                                                    sizeof(movement_command),
                                                    handler);
        if (!has_movement) {
            continue;
        }
        if (simple && !will_move) {
            keybind_event_flush(handler);
            if (handler->movement_intercept != NULL) {
                handler->movement_intercept(movement_command, handler->user_data);
            }
            continue;
        }
        if (will_move) {
            rebinds[rebinds_num++] = (keybind_movement_rebind){
                .scancode = scancode,
                .mod = keybind->mod,
                .direction = direction,
                .repeat = keybind->repeat,
            };
        }
        if (!simple) {
            compound_bindings[*compound_num] = keybind;
            compound_events[*compound_num] = (SDL_KeyboardEvent){
                .type = SDL_EVENT_KEY_DOWN,
                .key = keybind->key,
                .scancode = scancode,
                .mod = event->mod,
            };
            (*compound_num)++;
            if (will_move) {
                (*compound_moves)++;
            }
        }
    }

    return rebinds_num;
}

static bool keybind_event_invalid_mode_rebinds_movement(keybind_struct *const *bindings,
                                                        size_t bindings_num,
                                                        const key_struct *key_states,
                                                        const SDL_KeyboardEvent *event,
                                                        const keybind_event_handler *handler,
                                                        const bool *invalid_mode_scancodes) {
    if (key_states == NULL) {
        return false;
    }
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (!invalid_mode_scancodes[scancode] || !key_states[scancode].pressed) {
            continue;
        }
        const keybind_struct *keybind =
            keybind_event_find_scancode(bindings, bindings_num, scancode, event->mod);
        if (keybind == NULL) {
            continue;
        }
        uint8_t direction;
        bool has_movement, will_move = false;
        char movement_command[MAX_BUF];
        (void)keybind_event_simple_movement(keybind,
                                            &direction,
                                            &has_movement,
                                            &will_move,
                                            movement_command,
                                            sizeof(movement_command),
                                            handler);
        if (will_move) {
            return true;
        }
    }
    return false;
}

static void keybind_event_preserve_fallback_modes(keybind_struct *const *bindings,
                                                  size_t bindings_num,
                                                  const key_struct *key_states,
                                                  const SDL_KeyboardEvent *event,
                                                  const keybind_event_handler *handler,
                                                  const bool *invalid_mode_scancodes) {
    if (key_states == NULL) {
        return;
    }
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (!invalid_mode_scancodes[scancode] || !key_states[scancode].pressed) {
            continue;
        }
        const keybind_struct *keybind =
            keybind_event_find_scancode(bindings, bindings_num, scancode, event->mod);
        if (keybind == NULL) {
            continue;
        }
        bool run = handler->movement->keys[scancode].run_owned &&
                   keybind_command_contains(keybind->command, "?RUNON");
        bool fire = handler->movement->keys[scancode].fire_owned &&
                    keybind_command_contains(keybind->command, "?FIREON");
        if (run && (handler->movement_intercept_matches == NULL ||
                    !handler->movement_intercept_matches("?RUNON", handler->user_data))) {
            keybind_movement_state_mode_rebind(handler->movement, scancode, keybind->mod, true);
        }
        if (fire && (handler->movement_intercept_matches == NULL ||
                     !handler->movement_intercept_matches("?FIREON", handler->user_data))) {
            keybind_movement_state_mode_rebind(handler->movement, scancode, keybind->mod, false);
        }
    }
}

/** Return whether a physical keyboard event belongs to a binding modifier key. */
bool keybind_event_is_modifier(const SDL_KeyboardEvent *event) {
    switch (event->scancode) {
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI:
        case SDL_SCANCODE_MODE:
            return true;

        default:
            return false;
    }
}

static void keybind_event_release_invalid_mode_modifiers(SDL_Keymod mod,
                                                         const keybind_event_handler *handler) {
    bool run_released, fire_released;
    keybind_movement_state_release_invalid_mode_modifiers(handler->movement,
                                                          mod,
                                                          &run_released,
                                                          &fire_released);
    if (handler->command_up == NULL) {
        return;
    }
    if (run_released && !keybind_movement_state_mode_owned(handler->movement, true)) {
        handler->command_up("?RUNON", handler->user_data);
    }
    if (fire_released && !keybind_movement_state_mode_owned(handler->movement, false)) {
        handler->command_up("?FIREON", handler->user_data);
    }
}

static void keybind_event_release_scancode_modes(SDL_Scancode scancode,
                                                 const keybind_event_handler *handler) {
    if (handler->command_up == NULL) {
        return;
    }
    if (keybind_movement_state_mode_released(handler->movement, scancode, true) &&
        !keybind_movement_state_mode_owned(handler->movement, true)) {
        handler->command_up("?RUNON", handler->user_data);
    }
    if (keybind_movement_state_mode_released(handler->movement, scancode, false) &&
        !keybind_movement_state_mode_owned(handler->movement, false)) {
        handler->command_up("?FIREON", handler->user_data);
    }
}

/** Reconcile a key-up, flushing only movement-relevant ordering boundaries. */
void keybind_event_reconcile_release(keybind_struct *const *bindings,
                                     size_t bindings_num,
                                     const SDL_KeyboardEvent *event,
                                     const key_struct *key_states,
                                     const keybind_event_handler *handler) {
    if (bindings == NULL || event == NULL || handler == NULL || handler->movement == NULL) {
        return;
    }

    bool invalid_mode_scancodes[SDL_SCANCODE_COUNT] = {0};
    if (keybind_event_is_modifier(event)) {
        for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
            invalid_mode_scancodes[scancode] =
                keybind_movement_state_scancode_has_invalid_mode_modifier(handler->movement,
                                                                          scancode,
                                                                          event->mod);
        }
        keybind_event_preserve_fallback_modes(bindings,
                                              bindings_num,
                                              key_states,
                                              event,
                                              handler,
                                              invalid_mode_scancodes);
    }
    bool modifier_invalidates_movement =
        keybind_event_is_modifier(event) &&
        keybind_movement_state_has_invalid_modifier(handler->movement, event->mod);
    bool modifier_invalidates_mode =
        keybind_event_is_modifier(event) &&
        keybind_movement_state_mode_modifier_changes(handler->movement, event->mod);
    bool modifier_adds_movement =
        keybind_event_is_modifier(event) &&
        keybind_event_invalid_mode_rebinds_movement(bindings,
                                                    bindings_num,
                                                    key_states,
                                                    event,
                                                    handler,
                                                    invalid_mode_scancodes);
    bool scancode_changes_mode =
        keybind_movement_state_mode_release_changes(handler->movement, event->scancode, true) ||
        keybind_movement_state_mode_release_changes(handler->movement, event->scancode, false);
    if (keybind_movement_state_has_scancode(handler->movement, event->scancode) ||
        modifier_invalidates_movement || modifier_invalidates_mode || modifier_adds_movement ||
        scancode_changes_mode) {
        keybind_event_flush(handler);
    }
    keybind_event_release_scancode_modes(event->scancode, handler);
    keybind_movement_state_release(handler->movement,
                                   event->scancode,
                                   keybind_event_running(handler),
                                   keybind_event_firing(handler));
    if (keybind_event_is_modifier(event)) {
        keybind_movement_rebind rebinds[SDL_SCANCODE_COUNT];
        const keybind_struct *compound_bindings[SDL_SCANCODE_COUNT];
        SDL_KeyboardEvent compound_events[SDL_SCANCODE_COUNT];
        size_t compound_num = 0, compound_moves = 0;
        keybind_event_release_invalid_mode_modifiers(event->mod, handler);
        if (modifier_invalidates_mode && handler->reconcile_modes != NULL) {
            handler->reconcile_modes(handler->user_data);
        }
        size_t rebinds_num = keybind_event_modifier_rebinds(bindings,
                                                            bindings_num,
                                                            key_states,
                                                            event,
                                                            handler,
                                                            invalid_mode_scancodes,
                                                            rebinds,
                                                            compound_bindings,
                                                            compound_events,
                                                            &compound_num,
                                                            &compound_moves);
        keybind_movement_state_reconcile_modifiers(handler->movement,
                                                   event->mod,
                                                   rebinds,
                                                   rebinds_num,
                                                   modifier_invalidates_mode,
                                                   compound_moves != 0,
                                                   keybind_event_running(handler),
                                                   keybind_event_firing(handler));
        for (size_t i = 0; i < compound_num; i++) {
            keybind_event_process_binding(compound_bindings[i], &compound_events[i], handler);
        }
    }
}

/** Match and process one physical keyboard event with normal modifier precedence. */
bool keybind_event_process(keybind_struct *const *bindings,
                           size_t bindings_num,
                           const SDL_KeyboardEvent *event,
                           const keybind_event_handler *handler) {
    if (bindings == NULL || event == NULL || handler == NULL) {
        return false;
    }

    const keybind_struct *keybind = keybind_event_find(bindings, bindings_num, event);
    if (keybind != NULL) {
        keybind_event_process_binding(keybind, event, handler);
        return true;
    }

    return false;
}
