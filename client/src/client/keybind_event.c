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
            keybind_event_flush(handler);
            if (event->type == SDL_EVENT_KEY_DOWN) {
                if (handler->command_down != NULL) {
                    bool running = keybind_event_running(handler);
                    bool firing = keybind_event_firing(handler);
                    handler->command_down(cp, handler->user_data);
                    if (running != keybind_event_running(handler) ||
                        firing != keybind_event_firing(handler)) {
                        keybind_movement_state_defer_move(handler->movement);
                        cancel_mode_deferral = movement_accepted;
                    }
                }
            } else if (handler->command_up != NULL) {
                handler->command_up(cp, handler->user_data);
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

static const keybind_struct *keybind_event_find_scancode_exact(keybind_struct *const *bindings,
                                                               size_t bindings_num,
                                                               SDL_Scancode scancode,
                                                               SDL_Keymod mod) {
    for (size_t i = 0; i < bindings_num; i++) {
        if (SDL_GetScancodeFromKey(bindings[i]->key, NULL) == scancode && bindings[i]->mod == mod) {
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
        if (!key_states[scancode].pressed || movement_key->direction == 0 ||
            movement_key->mod == SDL_KMOD_NONE || movement_key->mod == adjusted_mod) {
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

static bool keybind_event_modifier_invalidates_mode(keybind_struct *const *bindings,
                                                    size_t bindings_num,
                                                    const SDL_KeyboardEvent *event,
                                                    const key_struct *key_states) {
    if (!keybind_event_is_modifier(event) || key_states == NULL) {
        return false;
    }

    SDL_Keymod adjusted_mod = keybind_adjust_kmod(event->mod);
    for (size_t i = 0; i < bindings_num; i++) {
        if (bindings[i]->mod == SDL_KMOD_NONE || bindings[i]->mod == adjusted_mod ||
            (!keybind_command_contains(bindings[i]->command, "?RUNON") &&
             !keybind_command_contains(bindings[i]->command, "?FIREON"))) {
            continue;
        }
        SDL_Scancode scancode = SDL_GetScancodeFromKey(bindings[i]->key, NULL);
        if (scancode != SDL_SCANCODE_UNKNOWN && key_states[scancode].pressed) {
            return true;
        }
    }
    return false;
}

static void keybind_event_release_invalid_movement_modes(keybind_struct *const *bindings,
                                                         size_t bindings_num,
                                                         SDL_Keymod mod,
                                                         const key_struct *key_states,
                                                         const keybind_event_handler *handler) {
    SDL_Keymod adjusted_mod = keybind_adjust_kmod(mod);
    bool other_run_owner = false, other_fire_owner = false;
    if (key_states != NULL) {
        for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
            const keybind_movement_key *movement_key = &handler->movement->keys[scancode];
            bool invalid_movement = movement_key->direction != 0 &&
                                    movement_key->mod != SDL_KMOD_NONE &&
                                    movement_key->mod != adjusted_mod;
            if (!key_states[scancode].pressed || invalid_movement) {
                continue;
            }
            const keybind_struct *keybind =
                keybind_event_find_scancode(bindings, bindings_num, scancode, mod);
            if (keybind != NULL) {
                other_run_owner |= keybind_command_contains(keybind->command, "?RUNON");
                other_fire_owner |= keybind_command_contains(keybind->command, "?FIREON");
            }
        }
    }
    for (SDL_Scancode scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        const keybind_movement_key *movement_key = &handler->movement->keys[scancode];
        if (movement_key->direction == 0 || movement_key->mod == SDL_KMOD_NONE ||
            movement_key->mod == adjusted_mod) {
            continue;
        }
        const keybind_struct *keybind =
            keybind_event_find_scancode_exact(bindings, bindings_num, scancode, movement_key->mod);
        if (keybind == NULL || handler->command_up == NULL) {
            continue;
        }
        if (!other_run_owner && keybind_command_contains(keybind->command, "?RUNON")) {
            handler->command_up("?RUNON", handler->user_data);
        }
        if (!other_fire_owner && keybind_command_contains(keybind->command, "?FIREON")) {
            handler->command_up("?FIREON", handler->user_data);
        }
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

    const keybind_struct *keybind = keybind_event_find(bindings, bindings_num, event);
    bool modifier_invalidates_movement =
        keybind_event_is_modifier(event) &&
        keybind_movement_state_has_invalid_modifier(handler->movement, event->mod);
    bool modifier_invalidates_mode =
        keybind_event_modifier_invalidates_mode(bindings, bindings_num, event, key_states);
    if (keybind_movement_state_has_scancode(handler->movement, event->scancode) ||
        modifier_invalidates_movement || modifier_invalidates_mode ||
        (keybind != NULL && (keybind_command_contains(keybind->command, "?RUNON") ||
                             keybind_command_contains(keybind->command, "?FIREON")))) {
        keybind_event_flush(handler);
    }
    keybind_movement_state_release(handler->movement,
                                   event->scancode,
                                   keybind_event_running(handler),
                                   keybind_event_firing(handler));
    if (keybind_event_is_modifier(event)) {
        keybind_movement_rebind rebinds[SDL_SCANCODE_COUNT];
        const keybind_struct *compound_bindings[SDL_SCANCODE_COUNT];
        SDL_KeyboardEvent compound_events[SDL_SCANCODE_COUNT];
        size_t compound_num = 0, compound_moves = 0;
        keybind_event_release_invalid_movement_modes(bindings,
                                                     bindings_num,
                                                     event->mod,
                                                     key_states,
                                                     handler);
        if (modifier_invalidates_mode && handler->reconcile_modes != NULL) {
            handler->reconcile_modes(handler->user_data);
        }
        size_t rebinds_num = keybind_event_modifier_rebinds(bindings,
                                                            bindings_num,
                                                            key_states,
                                                            event,
                                                            handler,
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
