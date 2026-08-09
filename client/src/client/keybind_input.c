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

/** Parse a persisted decimal keycode. */
bool keybind_keycode_parse(const char *text, bool legacy, SDL_Keycode *key) {
    char *end;
    unsigned long long value;

    if (text == NULL || key == NULL || *text == '\0' || *text == '-') {
        return false;
    }

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno == ERANGE || value > UINT32_MAX || end == text) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *key = legacy ? keybind_keycode_from_legacy((uint32_t)value) : (SDL_Keycode)value;
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

/** Construct a display string for a keybinding shortcut. */
char *keybind_get_key_shortcut(SDL_Keycode key, SDL_Keymod mod, char *buf, size_t len) {
    if (len == 0) {
        return buf;
    }
    buf[0] = '\0';

    if (mod & SDL_KMOD_SHIFT) {
        strncat(buf, "shift + ", len - strlen(buf) - 1);
    }
    if (mod & SDL_KMOD_CTRL) {
        strncat(buf, "ctrl + ", len - strlen(buf) - 1);
    }
    if (mod & SDL_KMOD_ALT) {
        strncat(buf, "alt + ", len - strlen(buf) - 1);
    }
    if (mod & SDL_KMOD_GUI) {
        strncat(buf, "super + ", len - strlen(buf) - 1);
    }
    if (key != SDLK_UNKNOWN) {
        strncat(buf, SDL_GetKeyName(key), len - strlen(buf) - 1);
    }

    return buf;
}
