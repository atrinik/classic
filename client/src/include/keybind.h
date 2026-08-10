/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Keybindings header file.
 */

#ifndef KEYBIND_H
#define KEYBIND_H

/** Where keybindings are saved. */
#define FILE_KEYBIND "settings/keys.dat"

/** Current on-disk keycode format. */
#define KEYBIND_KEYCODE_FORMAT "SDL3"

/**
 * One keybind.
 */
typedef struct keybind_struct {
    /** Command to execute. */
    char *command;

    /** Key bound. */
    SDL_Keycode key;

    /** Ctrl/shift/etc modifiers. */
    SDL_Keymod mod;

    /** Whether to trigger repeat. */
    uint8_t repeat;
} keybind_struct;

/** Result emitted by a pending gameplay movement stream update. */
typedef enum keybind_movement_action {
    KEYBIND_MOVEMENT_ACTION_NONE,
    KEYBIND_MOVEMENT_ACTION_MOVE,
    KEYBIND_MOVEMENT_ACTION_STOP,
    KEYBIND_MOVEMENT_ACTION_RUN_STOP
} keybind_movement_action;

/** One physical key participating in the gameplay movement stream. */
typedef struct keybind_movement_key {
    uint64_t order;
    SDL_Keymod mod;
    uint8_t direction;
    bool repeat;
    bool owned;
    bool preseeded;
} keybind_movement_key;

/** Replacement binding for a held movement key after modifiers change. */
typedef struct keybind_movement_rebind {
    SDL_Scancode scancode;
    SDL_Keymod mod;
    uint8_t direction;
    bool repeat;
} keybind_movement_rebind;

/** Physical-key state used to resolve one logical gameplay movement stream. */
typedef struct keybind_movement_state {
    keybind_movement_key keys[SDL_SCANCODE_COUNT];
    uint64_t next_order;
    SDL_Scancode repeat_scancode;
    uint8_t pending_direction;
    bool repeated;
    bool pending_move;
    bool pending_move_repeated;
    bool pending_stop;
    bool pending_run_stop;
    bool deferred_move;
} keybind_movement_state;

/** Callbacks used by the testable physical keybinding dispatcher. */
typedef struct keybind_event_handler {
    keybind_movement_state *movement;
    void *user_data;
    bool (*running)(void *user_data);
    bool (*firing)(void *user_data);
    void (*reconcile_modes)(void *user_data);
    void (*flush)(void *user_data);
    bool (*movement_intercept_matches)(const char *command, void *user_data);
    void (*movement_intercept)(const char *command, void *user_data);
    void (*command_down)(const char *command, void *user_data);
    void (*command_up)(const char *command, void *user_data);
} keybind_event_handler;

/** State and persistence API implemented in src/client/keybind_storage.c. */

extern keybind_struct **keybindings;

extern size_t keybindings_num;

extern void keybind_load(void);

extern void keybind_save(void);

extern void keybind_free(keybind_struct *keybind);

extern void keybind_deinit(void);

/** Mutation and command API implemented in src/client/keybind.c. */

extern keybind_struct *keybind_add(SDL_Keycode key, SDL_Keymod mod, const char *command);

extern void keybind_edit(size_t i, SDL_Keycode key, SDL_Keymod mod, const char *command);

extern void keybind_remove(size_t i);

extern void keybind_repeat_toggle(size_t i);

/** Input compatibility API implemented in src/client/keybind_input.c. */

extern SDL_Keycode keybind_keycode_from_legacy(uint32_t key);

extern bool keybind_uint32_parse(const char *text, uint32_t maximum, uint32_t *value);

extern bool keybind_keycode_parse(const char *text, bool legacy, SDL_Keycode *key);

extern SDL_Keymod keybind_adjust_kmod(SDL_Keymod mod);

extern bool keybind_matches_event(const keybind_struct *keybind, const SDL_KeyboardEvent *event);

extern char *keybind_get_key_shortcut(SDL_Keycode key, SDL_Keymod mod, char *buf, size_t len);

extern bool keybind_command_contains(const char *commands, const char *command);

extern bool keybind_command_matches_held(keybind_struct *const *bindings,
                                         size_t bindings_num,
                                         const char *command,
                                         const key_struct *key_states,
                                         SDL_Keymod mod);

extern bool keybind_event_process(keybind_struct *const *bindings,
                                  size_t bindings_num,
                                  const SDL_KeyboardEvent *event,
                                  const keybind_event_handler *handler);

extern void keybind_event_process_binding(const keybind_struct *keybind,
                                          const SDL_KeyboardEvent *event,
                                          const keybind_event_handler *handler);

extern void keybind_event_reconcile_release(keybind_struct *const *bindings,
                                            size_t bindings_num,
                                            const SDL_KeyboardEvent *event,
                                            const key_struct *key_states,
                                            const keybind_event_handler *handler);

extern bool keybind_event_is_modifier(const SDL_KeyboardEvent *event);

extern bool keybind_movement_command_direction(const char *cmd, uint8_t *direction);

extern void keybind_movement_state_init(keybind_movement_state *state);

extern bool keybind_movement_state_has_scancode(const keybind_movement_state *state,
                                                SDL_Scancode scancode);

extern void keybind_movement_state_set_modifier(keybind_movement_state *state,
                                                SDL_Scancode scancode,
                                                SDL_Keymod mod);

extern void keybind_movement_state_defer_move(keybind_movement_state *state);

extern void keybind_movement_state_cancel_deferred_move(keybind_movement_state *state);

extern void keybind_movement_state_reconcile_modifiers(keybind_movement_state *state,
                                                       SDL_Keymod mod,
                                                       const keybind_movement_rebind *rebinds,
                                                       size_t rebinds_num,
                                                       bool force_move,
                                                       bool defer_move,
                                                       bool running,
                                                       bool firing);

extern bool keybind_movement_state_has_invalid_modifier(const keybind_movement_state *state,
                                                        SDL_Keymod mod);

extern bool keybind_movement_state_press(keybind_movement_state *state,
                                         SDL_Scancode scancode,
                                         uint8_t direction,
                                         bool repeated,
                                         bool repeat);

extern void keybind_movement_state_release(keybind_movement_state *state,
                                           SDL_Scancode scancode,
                                           bool running,
                                           bool firing);

extern void keybind_movement_state_clear(keybind_movement_state *state, bool running, bool firing);

extern void keybind_movement_state_run_released(keybind_movement_state *state,
                                                bool run_stream_active);

extern keybind_movement_action keybind_movement_state_flush(keybind_movement_state *state,
                                                            uint8_t *direction);

extern keybind_struct *keybind_find_by_command(const char *cmd);

extern int keybind_command_matches_event(const char *cmd, SDL_KeyboardEvent *event);

extern int keybind_command_matches_state(const char *cmd);

extern int keybind_process_event(SDL_KeyboardEvent *event);

extern void keybind_process(keybind_struct *keybind, const SDL_KeyboardEvent *event);

extern int keybind_process_command_up(const char *cmd);

extern void keybind_state_ensure(void);

extern void keybind_movement_flush(void);

extern void keybind_movement_key_released(const SDL_KeyboardEvent *event);

extern void keybind_movement_focus_lost(void);

extern int keybind_process_command(const char *cmd);

/** Public API implemented in src/gui/popups/settings_keybinding.c. */

extern void settings_keybinding_open(void);

#endif
