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

/**
 * @file
 * Keybinding persistence and ownership.
 */

#include <global.h>
#include <toolkit/path.h>

/** The keybindings. */
keybind_struct **keybindings = NULL;
/** Number of keybindings. */
size_t keybindings_num = 0;

/** Load keybindings. */
void keybind_load(void) {
    FILE *fp;
    char buf[HUGE_BUF], *cp;
    keybind_struct *keybind = NULL;
    bool legacy_keycodes = true;
    bool keycode_valid = false;
    bool record_valid = true;
    bool bindings_started = false;

    fp = path_fopen(FILE_KEYBIND, "r");
    if (fp == NULL) {
        LOG(ERROR, "Failed to open file: %s", FILE_KEYBIND);
        return;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        size_t length = strlen(buf);
        while (length > 0 && (buf[length - 1] == '\n' || buf[length - 1] == '\r')) {
            buf[--length] = '\0';
        }

        cp = buf;
        while (isspace((unsigned char)*cp)) {
            cp++;
        }

        if (*cp == '#' || *cp == '\0') {
            continue;
        }

        if (!bindings_started && !strcmp(cp, "keycode_format " KEYBIND_KEYCODE_FORMAT)) {
            legacy_keycodes = false;
        } else if (!bindings_started && !strncmp(cp, "keycode_format ", 15)) {
            LOG(ERROR, "Unknown keybinding keycode format, preserving numeric values: %s", cp + 15);
            legacy_keycodes = false;
        } else if (!strcmp(cp, "end")) {
            if (keybind != NULL && keybind->command != NULL && keycode_valid && record_valid &&
                strcmp(keybind->command, "?FIRE_READY")) {
                keybindings =
                    xreallocarray(keybindings, (keybindings_num + 1), sizeof(*keybindings));
                keybindings[keybindings_num++] = keybind;
            } else if (keybind != NULL) {
                keybind_free(keybind);
            }
            keybind = NULL;
            keycode_valid = false;
            record_valid = true;
        } else if (!strcmp(cp, "bind")) {
            if (keybind != NULL) {
                keybind_free(keybind);
            }
            bindings_started = true;
            keybind = xcalloc(1, sizeof(*keybind));
            keycode_valid = false;
            record_valid = true;
        } else if (keybind != NULL) {
            if (!strncmp(cp, "command ", 8)) {
                free(keybind->command);
                keybind->command = xstrdup(cp + 8);
                record_valid &= cp[8] != '\0';
            } else if (!strncmp(cp, "key ", 4)) {
                keycode_valid = keybind_keycode_parse(cp + 4, legacy_keycodes, &keybind->key);
                record_valid &= keycode_valid;
            } else if (!strncmp(cp, "mod ", 4)) {
                uint32_t value;
                if (keybind_uint32_parse(cp + 4, UINT16_MAX, &value)) {
                    keybind->mod = keybind_adjust_kmod((SDL_Keymod)value);
                } else {
                    record_valid = false;
                }
            } else if (!strncmp(cp, "repeat ", 7)) {
                uint32_t value;
                if (keybind_uint32_parse(cp + 7, 1, &value)) {
                    keybind->repeat = (uint8_t)value;
                } else {
                    record_valid = false;
                }
            }
        }
    }

    if (keybind != NULL) {
        keybind_free(keybind);
    }
    fclose(fp);
}

/** Migrate keybindings from the 2.0 line-oriented macro format. */
void keybind_upgrade_legacy(FILE *stream) {
    char buf[HUGE_BUF];

    while (fgets(buf, sizeof(buf) - 1, stream)) {
        int keycode, repeat;
        char keyname[MAX_BUF], command[HUGE_BUF];

        if (sscanf(buf,
                   "%d %d \"%200[^\"]\" \"%2000[^\"]\"",
                   &keycode,
                   &repeat,
                   keyname,
                   command) != 4 ||
            keycode < 0) {
            continue;
        }

        SDL_Keycode migrated_keycode = keybind_keycode_from_legacy((uint32_t)keycode);
        keybind_struct *keybind;

        if (*command == '/') {
            keybind = keybind_find_by_command(command);
            if (keybind == NULL) {
                keybind = keybind_add(migrated_keycode, 0, command);
            } else {
                keybind->key = migrated_keycode;
            }
            keybind->repeat = repeat;
        } else if (!strncmp(command, "?M_MCON", 7)) {
            char mcon_buf[HUGE_BUF];

            snprintf(mcon_buf, sizeof(mcon_buf), "?MCON %s", command + 7);
            if (!keybind_find_by_command(mcon_buf)) {
                keybind = keybind_add(migrated_keycode, 0, mcon_buf);
                keybind->repeat = repeat;
            }
        } else if (*command == '?') {
            const char *new_cmd = keybind_command_from_legacy(command);

            if (new_cmd == NULL) {
                continue;
            }

            keybind = keybind_find_by_command(new_cmd);
            if (keybind != NULL) {
                keybind->key = migrated_keycode;
            }
        }
    }
}

/** Save the keybindings. */
void keybind_save(void) {
    bool write_failed;
    int write_error = 0;
    FILE *fp = path_fopen(FILE_KEYBIND, "w");
    if (fp == NULL) {
        LOG(ERROR, "Could not open %s for writing: %s (%d)", FILE_KEYBIND, strerror(errno), errno);
        return;
    }

    fprintf(fp, "keycode_format %s\n", KEYBIND_KEYCODE_FORMAT);
    for (size_t i = 0; i < keybindings_num; i++) {
        fprintf(fp, "bind\n");
        fprintf(fp,
                "\t# %s\n\tkey %" PRIu32 "\n",
                SDL_GetKeyName(keybindings[i]->key),
                keybindings[i]->key);
        if (keybindings[i]->mod != 0) {
            fprintf(fp, "\tmod %d\n", keybindings[i]->mod);
        }
        if (keybindings[i]->repeat) {
            fprintf(fp, "\trepeat %d\n", keybindings[i]->repeat);
        }
        if (keybindings[i]->command != NULL) {
            fprintf(fp, "\tcommand %s\n", keybindings[i]->command);
        }
        fprintf(fp, "end\n");
    }

    write_failed = ferror(fp) != 0;
    if (write_failed) {
        write_error = errno;
    }
    if (fclose(fp) != 0) {
        write_failed = true;
        write_error = errno;
    }
    if (write_failed) {
        if (write_error != 0) {
            LOG(ERROR,
                "Could not completely write %s: %s (%d)",
                FILE_KEYBIND,
                strerror(write_error),
                write_error);
        } else {
            LOG(ERROR, "Could not completely write %s", FILE_KEYBIND);
        }
    }
}

/** Free a single keybinding entry. */
void keybind_free(keybind_struct *keybind) {
    free(keybind->command);
    free(keybind);
}

/** Save and deinitialize all keybindings. */
void keybind_deinit(void) {
    keybind_save();
    for (size_t i = 0; i < keybindings_num; i++) {
        keybind_free(keybindings[i]);
    }
    free(keybindings);
    keybindings = NULL;
    keybindings_num = 0;
}
