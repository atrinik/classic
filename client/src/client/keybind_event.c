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
                    handler->command_down(cp, handler->user_data);
                }
            } else if (handler->command_up != NULL) {
                handler->command_up(cp, handler->user_data);
            }
        }

        cp = strtok(NULL, ";");
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

/** Reconcile a key-up, flushing only movement-relevant ordering boundaries. */
void keybind_event_reconcile_release(keybind_struct *const *bindings,
                                     size_t bindings_num,
                                     const SDL_KeyboardEvent *event,
                                     const keybind_event_handler *handler) {
    if (bindings == NULL || event == NULL || handler == NULL || handler->movement == NULL) {
        return;
    }

    const keybind_struct *keybind = keybind_event_find(bindings, bindings_num, event);
    if (keybind_movement_state_has_scancode(handler->movement, event->scancode) ||
        keybind_event_is_modifier(event) ||
        (keybind != NULL && (keybind_command_contains(keybind->command, "?RUNON") ||
                             keybind_command_contains(keybind->command, "?FIREON")))) {
        keybind_event_flush(handler);
    }
    keybind_movement_state_release(handler->movement,
                                   event->scancode,
                                   keybind_event_running(handler),
                                   keybind_event_firing(handler));
    if (keybind_event_is_modifier(event)) {
        keybind_movement_state_release_invalid_modifiers(handler->movement,
                                                         event->mod,
                                                         keybind_event_running(handler),
                                                         keybind_event_firing(handler));
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
