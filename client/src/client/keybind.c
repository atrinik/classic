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
 * Handles keybindings.
 *
 * Whenever a keyboard event occurs and the user is logged into the game,
 * keybindings are checked for a match. First keybindings with modifier
 * keys are checked to see if they match the event key and modifier state,
 * then keybindings without modifier keys are checked and will work
 * regardless of the current key modifier state.
 *
 * This is done to ensure user commands will work correctly, even if they
 * have modifier keys. For example, CTRL+c would not work if it was near
 * the bottom of the keybinding list, because the 'c' keybinding near the
 * top would work first.
 *
 * Also if the keybindings with no modifier keys were not triggered
 * regardless of the current keyboard modifier state, it would not be
 * possible to do actions such as alt+numpad, or ctrl+numpad.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <client_socket.h>
#include <notification.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>

/** Active physical keys contributing to the logical gameplay movement stream. */
static keybind_movement_state movement_state;

/** Whether run/fire mode is owned by a momentary held command rather than a toggle. */
static bool movement_run_held;
static bool movement_fire_held;

static bool keybind_event_running(void *user_data) {
    (void)user_data;
    return cpl.run_on;
}

static bool keybind_event_firing(void *user_data) {
    (void)user_data;
    return cpl.fire_on;
}

static void keybind_event_flush(void *user_data) {
    (void)user_data;
    keybind_movement_flush();
}

static bool keybind_event_movement_intercept_matches(const char *command, void *user_data) {
    (void)user_data;
    return notification_keybind_matches(command);
}

static void keybind_event_movement_intercept(const char *command, void *user_data) {
    (void)user_data;
    (void)notification_keybind_check(command);
}

static bool keybind_event_command_down(const char *command, void *user_data) {
    (void)user_data;
    if (notification_keybind_matches(command)) {
        (void)notification_keybind_check(command);
        return false;
    }
    return keybind_process_command(command) != 0;
}

static void keybind_event_command_up(const char *command, void *user_data) {
    (void)user_data;
    keybind_process_command_up(command);
}

static void keybind_event_reconcile_modes(void *user_data) {
    (void)user_data;
    keybind_state_ensure();
}

static keybind_event_handler keybind_event_handler_create(void) {
    return (keybind_event_handler){
        .movement = &movement_state,
        .running = keybind_event_running,
        .firing = keybind_event_firing,
        .reconcile_modes = keybind_event_reconcile_modes,
        .flush = keybind_event_flush,
        .movement_intercept_matches = keybind_event_movement_intercept_matches,
        .movement_intercept = keybind_event_movement_intercept,
        .command_down = keybind_event_command_down,
        .command_up = keybind_event_command_up,
    };
}

/**
 * Add a keybinding to the ::keybindings array.
 * @param key
 * Key the keybinding uses.
 * @param mod
 * Modifier for the keybinding shortcut. Will be adjusted by
 * keybind_adjust_kmod().
 * @param command
 * Command to execute when the keybinding is activated.
 * @return
 * The added keybinding.
 */
keybind_struct *keybind_add(SDL_Keycode key, SDL_Keymod mod, const char *command) {
    keybind_struct *keybind;

    /* Allocate a new keybinding, and store the values. */
    keybind = xcalloc(1, sizeof(*keybind));
    keybind->key = key;
    keybind->mod = keybind_adjust_kmod(mod);
    keybind->command = xstrdup(command);

    /* Expand the keybindings array, and store the new keybinding. */
    keybindings = xreallocarray(keybindings, (keybindings_num + 1), sizeof(*keybindings));
    keybindings[keybindings_num] = keybind;
    keybindings_num++;

    return keybind;
}

/**
 * Edit the specified keybinding.
 * @param i
 * Index inside the ::keybindings array to edit.
 * @param key
 * Key to change.
 * @param mod
 * Modifier to change.
 * @param command
 * Command to change.
 */
void keybind_edit(size_t i, SDL_Keycode key, SDL_Keymod mod, const char *command) {
    /* Sanity check. */
    if (i >= keybindings_num) {
        return;
    }

    /* Store the values. */
    keybindings[i]->key = key;
    keybindings[i]->mod = keybind_adjust_kmod(mod);
    free(keybindings[i]->command);
    keybindings[i]->command = xstrdup(command);
}

/**
 * Remove a keybinding from ::keybindings.
 * @param i
 * Index in the ::keybindings array to remove.
 */
void keybind_remove(size_t i) {
    size_t j;

    /* Sanity check. */
    if (i >= keybindings_num) {
        return;
    }

    /* Free the entry. */
    keybind_free(keybindings[i]);

    /* Shift entries below the removed keybinding, if any. */
    for (j = i + 1; j < keybindings_num; j++) {
        keybindings[j - 1] = keybindings[j];
    }

    /* Shrink the array. */
    keybindings_num--;
    keybindings = xreallocarray(keybindings, keybindings_num, sizeof(*keybindings));
}

/**
 * Toggle the repeat state of a keybinding.
 * @param i
 * Index in the ::keybindings array to toggle the repeat state
 * of.
 */
void keybind_repeat_toggle(size_t i) {
    /* Sanity check. */
    if (i >= keybindings_num) {
        return;
    }

    keybindings[i]->repeat = !keybindings[i]->repeat;
}

/**
 * Finds keybinding structure by command name.
 * @param cmd
 * The command to find.
 * @return
 * Keybinding if found, NULL otherwise.
 */
keybind_struct *keybind_find_by_command(const char *cmd) {
    size_t i;

    for (i = 0; i < keybindings_num; i++) {
        if (!strcmp(cmd, keybindings[i]->command)) {
            return keybindings[i];
        }
    }

    return NULL;
}

/**
 * Check if the specified keybinding command matches a keyboard event.
 * @param cmd
 * The keybinding command.
 * @return
 * 1 if it matches, 0 otherwise.
 */
int keybind_command_matches_event(const char *cmd, SDL_KeyboardEvent *event) {
    keybind_struct *keybind = keybind_find_by_command(cmd);

    if (!keybind) {
        return 0;
    }

    if (keybind_matches_event(keybind, event)) {
        return 1;
    }

    return 0;
}

/**
 * Check if the specified keybinding command matches the current keyboard
 * state.
 * @param cmd
 * The keybinding command.
 * @return
 * 1 if it matches, 0 otherwise.
 */
int keybind_command_matches_state(const char *cmd) {
    return keybind_command_matches_held(keybindings, keybindings_num, cmd, keys, SDL_GetModState());
}

/**
 * Attempt to process a keyboard event.
 * @param event
 * The event to process.
 * @return
 * 1 if the event was handled, 0 otherwise.
 */
int keybind_process_event(SDL_KeyboardEvent *event) {
    keybind_event_handler handler = keybind_event_handler_create();

    return keybind_event_process(keybindings, keybindings_num, event, &handler);
}

/**
 * Process a keybinding.
 * @param keybind
 * The keybinding to process.
 * @param event
 * Physical keyboard event to process.
 */
void keybind_process(keybind_struct *keybind, const SDL_KeyboardEvent *event) {
    keybind_event_handler handler = keybind_event_handler_create();

    keybind_event_process_binding(keybind, event, &handler);
}

/**
 * Handle keybinding 'key up' event.
 * @param cmd
 * Keybinding command to handle.
 * @return
 * 1 if the command was handled, 0 otherwise.
 */
int keybind_process_command_up(const char *cmd) {
    const char *cmd_orig = cmd;

    if (*cmd == '?') {
        cmd++;

        if (!strcmp(cmd, "RUNON")) {
            movement_run_held = false;
            cpl.run_on = 0;
            keybind_movement_state_run_released(&movement_state, move_keys_run_stream_active());
        } else if (!strcmp(cmd, "FIREON")) {
            movement_fire_held = false;
            cpl.fire_on = 0;
        } else if (!strncmp(cmd, "MOVE_", 5)) {
            keybind_struct *keybind;

            cmd += 5;

            if (strcmp(cmd, "STAY") != 0 && !cpl.fire_on &&
                (keybind = keybind_find_by_command(cmd_orig))) {
                SDL_Scancode scancode = SDL_GetScancodeFromKey(keybind->key, NULL);
                if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode].repeated) {
                    move_keys(5);
                }
            }
        }

        return 1;
    }

    return 0;
}

/**
 * Ensure that keybindings which should trigger on 'key up' event have
 * done so, even if the 'key up' event was handled by something else.
 */
void keybind_state_ensure(void) {
    for (SDL_Scancode i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (!keys[i].pressed) {
            keybind_movement_state_release(&movement_state, i, cpl.run_on, cpl.fire_on);
        }
    }

    if (movement_run_held && !keybind_movement_state_mode_owned(&movement_state, true)) {
        if (cpl.run_on) {
            keybind_process_command_up("?RUNON");
        } else {
            movement_run_held = false;
        }
    }

    if (movement_fire_held && !keybind_movement_state_mode_owned(&movement_state, false)) {
        if (cpl.fire_on) {
            keybind_process_command_up("?FIREON");
        } else {
            movement_fire_held = false;
        }
    }
}

/** Emit the next pending logical movement update. */
void keybind_movement_flush(void) {
    uint8_t direction;
    keybind_movement_action action;

    while ((action = keybind_movement_state_flush(&movement_state, &direction)) !=
           KEYBIND_MOVEMENT_ACTION_NONE) {
        if (action == KEYBIND_MOVEMENT_ACTION_MOVE) {
            move_keys(direction);
        } else if (action == KEYBIND_MOVEMENT_ACTION_STOP) {
            move_keys_clear();
        } else if (action == KEYBIND_MOVEMENT_ACTION_RUN_STOP) {
            move_keys_run_stop();
        }
    }
}

/** Reconcile a physical key-up even when a focused UI element consumes it. */
void keybind_movement_key_released(const SDL_KeyboardEvent *event) {
    keybind_event_handler handler = keybind_event_handler_create();

    keybind_event_reconcile_release(keybindings, keybindings_num, event, keys, &handler);
    if (keybind_event_is_modifier(event)) {
        keybind_state_ensure();
    }
}

/** Cancel un-emitted movement and stop an established stream on focus loss. */
void keybind_movement_focus_lost(void) {
    keybind_movement_state_clear(&movement_state, cpl.run_on, cpl.fire_on);
}

/**
 * Handle keybinding 'key down' event.
 * @param cmd
 * Keybinding command to handle.
 * @return
 * 1 if the command was handled, 0 otherwise.
 */
int keybind_process_command(const char *cmd) {
    const char *cmd_orig = cmd;

    if (notification_keybind_check(cmd)) {
        return 1;
    }

    if (*cmd == '?') {
        cmd++;

        uint8_t direction;
        if (keybind_movement_command_direction(cmd_orig, &direction)) {
            move_keys(direction);
        } else if (!strncmp(cmd, "MOVE_", 5)) {
            cmd += 5;
            if (!strcmp(cmd, "STAY")) {
                move_keys(5);
            }
        } else if (!strcmp(cmd, "CONSOLE")) {
            widget_textwin_handle_console(NULL);
        } else if (!strcmp(cmd, "APPLY")) {
            widget_inventory_handle_apply(cpl.inventory_focus);
        } else if (!strcmp(cmd, "EXAMINE")) {
            menu_inventory_examine(cpl.inventory_focus, NULL, NULL);
        } else if (!strcmp(cmd, "MARK")) {
            menu_inventory_mark(cpl.inventory_focus, NULL, NULL);
        } else if (!strcmp(cmd, "LOCK")) {
            menu_inventory_lock(cpl.inventory_focus, NULL, NULL);
        } else if (!strcmp(cmd, "GET")) {
            menu_inventory_get(cpl.inventory_focus, NULL, NULL);
        } else if (!strcmp(cmd, "DROP")) {
            menu_inventory_drop(cpl.inventory_focus, NULL, NULL);
        } else if (!strcmp(cmd, "HELP")) {
            help_show("main");
        } else if (!strcmp(cmd, "QLIST")) {
            packet_struct *packet;

            packet = packet_new(SERVER_CMD_QUESTLIST, 0, 0);
            socket_send_packet(packet);
        } else if (!strcmp(cmd, "TARGET_ENEMY")) {
            map_target_handle(0);
        } else if (!strcmp(cmd, "TARGET_FRIEND")) {
            map_target_handle(1);
        } else if (!strcmp(cmd, "SPELL_LIST")) {
            cur_widget[SPELLS_ID]->show = !cur_widget[SPELLS_ID]->show;
            SetPriorityWidget(cur_widget[SPELLS_ID]);
        } else if (!strcmp(cmd, "SKILL_LIST")) {
            cur_widget[SKILLS_ID]->show = !cur_widget[SKILLS_ID]->show;
            SetPriorityWidget(cur_widget[SKILLS_ID]);
        } else if (!strcmp(cmd, "PARTY_LIST")) {
            if (cur_widget[PARTY_ID]->show) {
                cur_widget[PARTY_ID]->show = 0;
            } else {
                send_command("/party list");
            }
        } else if (!strncmp(cmd, "MCON", 4)) {
            cmd += 4;

            while (*cmd == ' ') {
                cmd++;
            }

            widget_textwin_handle_console(cmd);
        } else if (!strcmp(cmd, "UP")) {
            widget_inventory_handle_arrow_key(cpl.inventory_focus, SDLK_UP);
        } else if (!strcmp(cmd, "DOWN")) {
            widget_inventory_handle_arrow_key(cpl.inventory_focus, SDLK_DOWN);
        } else if (!strcmp(cmd, "LEFT")) {
            widget_inventory_handle_arrow_key(cpl.inventory_focus, SDLK_LEFT);
        } else if (!strcmp(cmd, "RIGHT")) {
            widget_inventory_handle_arrow_key(cpl.inventory_focus, SDLK_RIGHT);
        } else if (!strncmp(cmd, "RUNON", 5)) {
            if (!strcmp(cmd + 5, "_TOGGLE")) {
                movement_run_held = false;
                if (cpl.run_on) {
                    move_keys(5);
                }

                cpl.run_on = !cpl.run_on;
            } else {
                movement_run_held = true;
                cpl.run_on = 1;
            }
        } else if (!strncmp(cmd, "FIREON", 6)) {
            if (!strcmp(cmd + 6, "_TOGGLE")) {
                movement_fire_held = false;
                cpl.fire_on = !cpl.fire_on;
            } else {
                movement_fire_held = true;
                cpl.fire_on = 1;
            }
        } else if (!strncmp(cmd, "QUICKSLOT_", 10)) {
            cmd += 10;

            if (string_startswith(cmd, "GROUP_")) {
                widgetdata *widget;

                cmd += 6;
                widget = widget_find(NULL, QUICKSLOT_ID, NULL, NULL);

                if (strcmp(cmd, "NEXT") == 0) {
                    quickslots_scroll(widget, 0, 1);
                } else if (strcmp(cmd, "PREV") == 0) {
                    quickslots_scroll(widget, 1, 1);
                } else if (strcmp(cmd, "CYCLE") == 0) {
                    quickslots_cycle(widget);
                }
            } else if (string_isdigit(cmd)) {
                quickslots_handle_key(MAX(1, MIN(8, atoi(cmd))) - 1);
            }
        } else if (!strcmp(cmd, "COPY")) {
            textwin_handle_copy(NULL);
        } else if (!strcmp(cmd, "HELLO")) {
            send_command_check("/talk 1 hello");
        } else if (strcmp(cmd, "COMBAT") == 0 || strcmp(cmd, "COMBAT_FORCE") == 0) {
            uint8_t combat = cpl.combat, combat_force = cpl.combat_force;

            if (strcmp(cmd, "COMBAT") == 0) {
                combat = !combat;
            } else {
                combat_force = !combat_force;
            }

            WIDGET_REDRAW_ALL(TARGET_ID);

            packet_struct *packet = packet_new(SERVER_CMD_COMBAT, 8, 0);
            packet_writer_write_uint8(packet, combat);
            packet_writer_write_uint8(packet, combat_force);
            socket_send_packet(packet);
        }

        return 1;
    } else {
        if (send_command_check(cmd)) {
            draw_info(COLOR_DGOLD, cmd);
        }
    }

    return 0;
}
