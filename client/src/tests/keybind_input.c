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
#include <toolkit/path.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void test_legacy_keycode_migration(void) {
    static const struct {
        uint32_t legacy;
        SDL_Keycode current;
    } cases[] = {
        {256, SDLK_KP_0},        {257, SDLK_KP_1},         {258, SDLK_KP_2},
        {259, SDLK_KP_3},        {260, SDLK_KP_4},         {261, SDLK_KP_5},
        {262, SDLK_KP_6},        {263, SDLK_KP_7},         {264, SDLK_KP_8},
        {265, SDLK_KP_9},        {266, SDLK_KP_PERIOD},    {267, SDLK_KP_DIVIDE},
        {268, SDLK_KP_MULTIPLY}, {269, SDLK_KP_MINUS},     {270, SDLK_KP_PLUS},
        {271, SDLK_KP_ENTER},    {272, SDLK_KP_EQUALS},    {273, SDLK_UP},
        {274, SDLK_DOWN},        {275, SDLK_RIGHT},        {276, SDLK_LEFT},
        {277, SDLK_INSERT},      {278, SDLK_HOME},         {279, SDLK_END},
        {280, SDLK_PAGEUP},      {281, SDLK_PAGEDOWN},     {282, SDLK_F1},
        {283, SDLK_F2},          {284, SDLK_F3},           {285, SDLK_F4},
        {286, SDLK_F5},          {287, SDLK_F6},           {288, SDLK_F7},
        {289, SDLK_F8},          {290, SDLK_F9},           {291, SDLK_F10},
        {292, SDLK_F11},         {293, SDLK_F12},          {294, SDLK_F13},
        {295, SDLK_F14},         {296, SDLK_F15},          {300, SDLK_NUMLOCKCLEAR},
        {301, SDLK_CAPSLOCK},    {302, SDLK_SCROLLLOCK},   {303, SDLK_RSHIFT},
        {304, SDLK_LSHIFT},      {305, SDLK_RCTRL},        {306, SDLK_LCTRL},
        {307, SDLK_RALT},        {308, SDLK_LALT},         {309, SDLK_RGUI},
        {310, SDLK_LGUI},        {311, SDLK_LGUI},         {312, SDLK_RGUI},
        {313, SDLK_MODE},        {315, SDLK_HELP},         {316, SDLK_PRINTSCREEN},
        {317, SDLK_SYSREQ},      {318, SDLK_PAUSE},        {319, SDLK_MENU},
        {320, SDLK_POWER},       {321, SDLK_CURRENCYUNIT}, {322, SDLK_UNDO},
    };
    SDL_Keycode key;

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        TEST_CHECK(keybind_keycode_from_legacy(cases[i].legacy) == cases[i].current);
    }
    TEST_CHECK(keybind_keycode_from_legacy(233) == 233);
    TEST_CHECK(keybind_keycode_from_legacy(297) == 297);
    TEST_CHECK(keybind_keycode_from_legacy(314) == 314);
    TEST_CHECK(keybind_keycode_from_legacy(323) == 323);

    TEST_CHECK(keybind_keycode_parse("293", true, &key) && key == SDLK_F12);
    TEST_CHECK(keybind_keycode_parse("1073741893", false, &key) && key == SDLK_F12);
    TEST_CHECK(keybind_keycode_parse("233", false, &key) && key == 233);
    TEST_CHECK(!keybind_keycode_parse("-1", true, &key));
    TEST_CHECK(!keybind_keycode_parse("4294967296", false, &key));
    TEST_CHECK(!keybind_keycode_parse("293junk", true, &key));
    TEST_CHECK(keybind_keycode_parse("293 \t", true, &key) && key == SDLK_F12);

    uint32_t value;
    TEST_CHECK(keybind_uint32_parse("65535", UINT16_MAX, &value) && value == UINT16_MAX);
    TEST_CHECK(!keybind_uint32_parse("65536", UINT16_MAX, &value));
    TEST_CHECK(!keybind_uint32_parse("2", 1, &value));
}

static void test_shortcut_names(void) {
    char buf[64];
    char one[1] = {'x'};
    char untouched = 'x';

    TEST_CHECK(!strcmp(keybind_get_key_shortcut(SDLK_F12, SDL_KMOD_NONE, buf, sizeof(buf)), "F12"));
    TEST_CHECK(
        !strcmp(keybind_get_key_shortcut(SDLK_UP, SDL_KMOD_SHIFT, buf, sizeof(buf)), "shift + Up"));
    TEST_CHECK(
        !strcmp(keybind_get_key_shortcut(SDLK_KP_9, SDL_KMOD_NONE, buf, sizeof(buf)), "Keypad 9"));
    TEST_CHECK(keybind_get_key_shortcut(SDLK_F12, SDL_KMOD_NONE, &untouched, 0) == &untouched);
    TEST_CHECK(untouched == 'x');
    TEST_CHECK(keybind_get_key_shortcut(SDLK_F12, SDL_KMOD_NONE, one, sizeof(one)) == one);
    TEST_CHECK(one[0] == '\0');
    TEST_CHECK(!strcmp(keybind_get_key_shortcut(SDLK_F12,
                                                SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI,
                                                buf,
                                                sizeof(buf)),
                       "ctrl + alt + super + F12"));
    TEST_CHECK(keybind_adjust_kmod(SDL_KMOD_LCTRL | SDL_KMOD_LALT | SDL_KMOD_LGUI) ==
               (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI));
}

static const char *keybind_fixture;

static FILE *keybind_fixture_open(const char *filename, const char *modes) {
    FILE *stream;

    TEST_CHECK(!strcmp(filename, FILE_KEYBIND));
    TEST_CHECK(!strcmp(modes, "r"));
    stream = tmpfile();
    TEST_CHECK(stream != NULL);
    TEST_CHECK(fputs(keybind_fixture, stream) >= 0);
    rewind(stream);
    return stream;
}

static void keybind_fixture_reset(void) {
    for (size_t i = 0; i < keybindings_num; i++) {
        keybind_free(keybindings[i]);
    }
    free(keybindings);
    keybindings = NULL;
    keybindings_num = 0;
}

static void test_keybind_loader(void) {
    path_fopen = keybind_fixture_open;
    keybind_fixture = "end\r\n"
                      "bind\r\nkey 293\r\ncommand ?HELP\r\nend\r\n"
                      "bind\nkey 273\ncommand ?UP\nend\n"
                      "bind\nkey invalid\ncommand ?BAD\nend\n"
                      "bind\nkey 265\ncommand ?INCOMPLETE\n"
                      "bind\nkey 293\ncommand ?DISCARDED\n"
                      "bind\nkey 265\ncommand ?OLD\ncommand ?KP\nend\n";

    keybind_load();
    TEST_CHECK(keybindings_num == 3);
    TEST_CHECK(!strcmp(keybindings[0]->command, "?HELP") && keybindings[0]->key == SDLK_F12);
    TEST_CHECK(!strcmp(keybindings[1]->command, "?UP") && keybindings[1]->key == SDLK_UP);
    TEST_CHECK(!strcmp(keybindings[2]->command, "?KP") && keybindings[2]->key == SDLK_KP_9);
    keybind_fixture_reset();

    keybind_fixture = "keycode_format " KEYBIND_KEYCODE_FORMAT
                      "\nbind\nkey 1073741893\nmod 64\nrepeat 1\ncommand ?HELP\nend\n";
    keybind_load();
    TEST_CHECK(keybindings_num == 1);
    TEST_CHECK(keybindings[0]->key == SDLK_F12);
    TEST_CHECK(keybindings[0]->mod == SDL_KMOD_CTRL);
    TEST_CHECK(keybindings[0]->repeat == 1);
    keybind_fixture_reset();
}

static void test_bundled_defaults(void) {
    static const struct {
        const char *command;
        SDL_Keycode key;
    } expected[] = {
        {"?MOVE_N", SDLK_KP_9},
        {"?MOVE_NE", SDLK_KP_6},
        {"?MOVE_E", SDLK_KP_3},
        {"?MOVE_SE", SDLK_KP_2},
        {"?MOVE_S", SDLK_KP_1},
        {"?MOVE_SW", SDLK_KP_4},
        {"?MOVE_W", SDLK_KP_7},
        {"?MOVE_NW", SDLK_KP_8},
        {"?MOVE_STAY", SDLK_KP_5},
        {"?CONSOLE", SDLK_KP_ENTER},
        {"?SPELL_LIST", SDLK_F9},
        {"?SKILL_LIST", SDLK_F10},
        {"?PARTY_LIST", SDLK_F11},
        {"?HELP", SDLK_F12},
        {"?UP", SDLK_UP},
        {"?DOWN", SDLK_DOWN},
        {"?LEFT", SDLK_LEFT},
        {"?RIGHT", SDLK_RIGHT},
        {"?RUNON", SDLK_LALT},
        {"?RUNON", SDLK_RALT},
        {"?FIREON", SDLK_RCTRL},
        {"?FIREON", SDLK_LCTRL},
        {"?QUICKSLOT_GROUP_NEXT", SDLK_END},
        {"?QUICKSLOT_GROUP_PREV", SDLK_HOME},
        {"?QUICKSLOT_SET_KEY", SDLK_LSHIFT},
    };
    bool found[sizeof(expected) / sizeof(*expected)] = {false};
    char line[256];
    SDL_Keycode key = SDLK_UNKNOWN;
    bool marker = false;
    FILE *stream = fopen(ATRINIK_TEST_SOURCE_DIR "/settings/keys.dat", "r");

    TEST_CHECK(stream != NULL);
    while (fgets(line, sizeof(line), stream) != NULL) {
        char *text = line;
        while (isspace((unsigned char)*text)) {
            text++;
        }
        char *newline = strchr(text, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        if (!strcmp(text, "keycode_format " KEYBIND_KEYCODE_FORMAT)) {
            marker = true;
        } else if (!strncmp(text, "key ", 4)) {
            TEST_CHECK(keybind_keycode_parse(text + 4, false, &key));
        } else if (!strncmp(text, "command ", 8)) {
            for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
                if (!found[i] && key == expected[i].key && !strcmp(text + 8, expected[i].command)) {
                    found[i] = true;
                    break;
                }
            }
        }
    }
    TEST_CHECK(fclose(stream) == 0);
    TEST_CHECK(marker);
    for (size_t i = 0; i < sizeof(found) / sizeof(*found); i++) {
        TEST_CHECK(found[i]);
    }
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
    test_keybind_loader();
    test_bundled_defaults();
    test_event_matching();
    return 0;
}
