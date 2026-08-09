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

#include <global.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void test_legacy_keycode_migration(void) {
    SDL_Keycode key;

    TEST_CHECK(keybind_keycode_parse("293", true, &key) && key == SDLK_F12);
    TEST_CHECK(keybind_keycode_parse("273", true, &key) && key == SDLK_UP);
    TEST_CHECK(keybind_keycode_parse("265", true, &key) && key == SDLK_KP_9);
    TEST_CHECK(keybind_keycode_parse("1073741893", false, &key) && key == SDLK_F12);
    TEST_CHECK(keybind_keycode_parse("233", false, &key) && key == 233);
    TEST_CHECK(!keybind_keycode_parse("-1", true, &key));
    TEST_CHECK(!keybind_keycode_parse("4294967296", false, &key));
    TEST_CHECK(!keybind_keycode_parse("293junk", true, &key));
}

static void test_shortcut_names(void) {
    char buf[64];

    TEST_CHECK(!strcmp(keybind_get_key_shortcut(SDLK_F12, SDL_KMOD_NONE, buf, sizeof(buf)), "F12"));
    TEST_CHECK(
        !strcmp(keybind_get_key_shortcut(SDLK_UP, SDL_KMOD_SHIFT, buf, sizeof(buf)), "shift + Up"));
    TEST_CHECK(
        !strcmp(keybind_get_key_shortcut(SDLK_KP_9, SDL_KMOD_NONE, buf, sizeof(buf)), "Keypad 9"));
}

static void test_event_matching(void) {
    keybind_struct keybind = {.key = SDLK_F12};
    SDL_KeyboardEvent event = {.key = SDLK_F12};

    TEST_CHECK(keybind_matches_event(&keybind, &event));
    event.key = SDLK_UP;
    TEST_CHECK(!keybind_matches_event(&keybind, &event));

    keybind.key = SDLK_UP;
    TEST_CHECK(keybind_matches_event(&keybind, &event));

    keybind.key = SDLK_KP_9;
    event.key = SDLK_KP_9;
    TEST_CHECK(keybind_matches_event(&keybind, &event));

    keybind.mod = SDL_KMOD_SHIFT;
    TEST_CHECK(!keybind_matches_event(&keybind, &event));
    event.mod = SDL_KMOD_LSHIFT;
    TEST_CHECK(keybind_matches_event(&keybind, &event));
}

int main(void) {
    test_legacy_keycode_migration();
    test_shortcut_names();
    test_event_matching();
    return 0;
}
