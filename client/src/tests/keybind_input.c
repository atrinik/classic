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

#define TEST_CHECK(condition)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                        \
        }                                                                   \
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

static void expect_movement(keybind_movement_state *state,
                            keybind_movement_action expected_action,
                            uint8_t expected_direction) {
    uint8_t direction = UINT8_MAX;
    keybind_movement_action action = keybind_movement_state_flush(state, &direction);

    if (action != expected_action) {
        fprintf(stderr,
                "movement action %d, expected %d (direction %u)\n",
                action,
                expected_action,
                direction);
        abort();
    }
    if (expected_action != KEYBIND_MOVEMENT_ACTION_NONE) {
        TEST_CHECK(direction == expected_direction);
    }
}

static void test_movement_commands(void) {
    static const struct {
        const char *command;
        uint8_t direction;
    } cases[] = {
        {"?MOVE_N", 8},
        {"?MOVE_NE", 9},
        {"?MOVE_E", 6},
        {"?MOVE_SE", 3},
        {"?MOVE_S", 2},
        {"?MOVE_SW", 1},
        {"?MOVE_W", 4},
        {"?MOVE_NW", 7},
    };
    uint8_t direction;

    for (size_t i = 0; i < arraysize(cases); i++) {
        TEST_CHECK(keybind_movement_command_direction(cases[i].command, &direction));
        TEST_CHECK(direction == cases[i].direction);
    }
    TEST_CHECK(!keybind_movement_command_direction("?MOVE_STAY", &direction));
    TEST_CHECK(!keybind_movement_command_direction("?MOVE_N;?HELP", &direction));
    TEST_CHECK(!keybind_movement_command_direction(NULL, &direction));
    TEST_CHECK(!keybind_movement_command_direction("?MOVE_N", NULL));

    TEST_CHECK(keybind_command_contains("?RUNON; ?MOVE_NW", "?RUNON"));
    TEST_CHECK(keybind_command_contains("?RUNON; ?MOVE_NW", "?MOVE_NW"));
    TEST_CHECK(!keybind_command_contains("?RUNON_TOGGLE; ?MOVE_NW", "?RUNON"));
    TEST_CHECK(!keybind_command_contains("?MOVE_NORTH", "?MOVE_N"));
    TEST_CHECK(!keybind_command_contains(NULL, "?MOVE_N"));

    keybind_struct compound = {
        .command = "?RUNON; ?MOVE_NW",
        .key = SDLK_A,
    };
    keybind_struct exact = {
        .command = "?RUNON",
        .key = SDLK_B,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct *bindings[] = {&compound, &exact};
    key_struct key_states[SDL_SCANCODE_COUNT] = {0};

    key_states[SDL_SCANCODE_A].pressed = true;
    TEST_CHECK(keybind_command_matches_held(bindings,
                                            arraysize(bindings),
                                            "?RUNON",
                                            key_states,
                                            SDL_KMOD_NONE));
    key_states[SDL_SCANCODE_A].pressed = false;
    key_states[SDL_SCANCODE_B].pressed = true;
    TEST_CHECK(keybind_command_matches_held(bindings,
                                            arraysize(bindings),
                                            "?RUNON",
                                            key_states,
                                            SDL_KMOD_LSHIFT));
    TEST_CHECK(!keybind_command_matches_held(bindings,
                                             arraysize(bindings),
                                             "?RUNON",
                                             key_states,
                                             SDL_KMOD_NONE));
}

static void test_movement_chords(void) {
    static const struct {
        uint8_t first;
        uint8_t second;
        uint8_t result;
    } cases[] = {
        {7, 9, 8},
        {9, 3, 6},
        {3, 1, 2},
        {1, 7, 4},
    };

    for (size_t i = 0; i < arraysize(cases); i++) {
        for (size_t reverse = 0; reverse < 2; reverse++) {
            keybind_movement_state state;
            uint8_t first = reverse ? cases[i].second : cases[i].first;
            uint8_t second = reverse ? cases[i].first : cases[i].second;

            keybind_movement_state_init(&state);
            keybind_movement_state_press(&state, SDL_SCANCODE_A, first, false, true);
            keybind_movement_state_press(&state, SDL_SCANCODE_B, second, false, true);
            expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, cases[i].result);
            expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

            keybind_movement_state_init(&state);
            keybind_movement_state_press(&state, SDL_SCANCODE_A, first, false, true);
            expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, first);
            keybind_movement_state_press(&state, SDL_SCANCODE_B, second, false, true);
            expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, cases[i].result);
        }
    }
}

static void test_movement_repeat_and_release(void) {
    keybind_movement_state state;

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);

    /* The first repeat after a membership change owns the logical stream. */
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    TEST_CHECK(!keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    TEST_CHECK(!keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Releasing the repeat owner continues immediately with the remaining key. */
    keybind_movement_state_release(&state, SDL_SCANCODE_B, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Adding a chord key after repeats begin re-elects either platform source. */
    for (size_t reverse = 0; reverse < 2; reverse++) {
        SDL_Scancode first_scancode = reverse ? SDL_SCANCODE_B : SDL_SCANCODE_A;
        SDL_Scancode second_scancode = reverse ? SDL_SCANCODE_A : SDL_SCANCODE_B;
        uint8_t first_direction = reverse ? 9 : 7;
        uint8_t second_direction = reverse ? 7 : 9;

        keybind_movement_state_init(&state);
        keybind_movement_state_press(&state, first_scancode, first_direction, false, true);
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, first_direction);
        TEST_CHECK(
            keybind_movement_state_press(&state, first_scancode, first_direction, true, true));
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, first_direction);
        keybind_movement_state_press(&state, second_scancode, second_direction, false, true);
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
        TEST_CHECK(
            keybind_movement_state_press(&state, second_scancode, second_direction, true, true));
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
        TEST_CHECK(
            !keybind_movement_state_press(&state, first_scancode, first_direction, true, true));
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
    }

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    TEST_CHECK(!keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
}

static void test_movement_duplicates_and_repeat_selection(void) {
    keybind_movement_state state;

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, false);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, true, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
}

static void test_movement_boundaries_and_modifiers(void) {
    keybind_movement_state state;

    /* A repeat whose initial down was owned by a focused widget is ignored. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A consumed release prevents a later focused repeat from reclaiming the key. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    TEST_CHECK(!keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Focus loss discards movement that has not yet been emitted. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_clear(&state, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A quick key-down/key-up received in one poll still moves once. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Unsupported cardinal and three-direction chords use the newest active key. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 8, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    keybind_movement_state_press(&state, SDL_SCANCODE_C, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 9);

    /* Running stops once on final release even before SDL repeat begins. */
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    keybind_movement_state_release(&state, SDL_SCANCODE_B, true, false);
    keybind_movement_state_release(&state, SDL_SCANCODE_C, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Fire streams never emit the movement clear command on release/focus loss. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_clear(&state, true, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_clear(&state, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Movement and run releases coalesce to one stop in either order. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    keybind_movement_state_run_released(&state, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A run stop closes the repeat epoch before a later key release. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A queued ordinary move follows the prior run-stream stop. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* An external running producer still receives the legacy run stop. */
    keybind_movement_state_init(&state);
    keybind_movement_state_run_released(&state, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A fire-routed repeat cannot discard a preceding run stop. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A newer non-fire repeat epoch still clears on its later release. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Focus loss plus Run reconciliation coalesces to one movement stop. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_clear(&state, true, false);
    keybind_movement_state_run_released(&state, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Same-poll chord releases retain the initial composite and each transition. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 9);
    keybind_movement_state_release(&state, SDL_SCANCODE_B, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
}

typedef struct movement_sink {
    keybind_movement_state state;
    keybind_movement_action actions[32];
    uint8_t directions[32];
    bool running_at_emit[32];
    bool firing_at_emit[32];
    size_t actions_num;
    bool running;
    bool firing;
    bool intercept_movement;
    size_t intercepted_num;
} movement_sink;

static bool movement_sink_running(void *user_data) {
    return ((movement_sink *)user_data)->running;
}

static bool movement_sink_firing(void *user_data) {
    return ((movement_sink *)user_data)->firing;
}

static void movement_sink_flush(void *user_data) {
    movement_sink *sink = user_data;
    keybind_movement_action action;
    uint8_t direction;

    while ((action = keybind_movement_state_flush(&sink->state, &direction)) !=
           KEYBIND_MOVEMENT_ACTION_NONE) {
        TEST_CHECK(sink->actions_num < arraysize(sink->actions));
        size_t i = sink->actions_num++;
        sink->actions[i] = action;
        sink->directions[i] = direction;
        sink->running_at_emit[i] = sink->running;
        sink->firing_at_emit[i] = sink->firing;
    }
}

static void movement_sink_command_down(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    if (!strcmp(command, "?RUNON")) {
        sink->running = true;
    } else if (!strcmp(command, "?FIREON")) {
        sink->firing = true;
    }
}

static bool movement_sink_intercept_matches(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    return sink->intercept_movement && !strcmp(command, "?MOVE_NW");
}

static void movement_sink_intercept(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    TEST_CHECK(!strcmp(command, "?MOVE_NW"));
    sink->intercepted_num++;
}

static void movement_sink_command_up(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    if (!strcmp(command, "?RUNON")) {
        sink->running = false;
    } else if (!strcmp(command, "?FIREON")) {
        sink->firing = false;
    }
}

static void movement_sink_reconcile_modes(void *user_data) {
    movement_sink *sink = user_data;

    sink->running = false;
    sink->firing = false;
}

static keybind_event_handler movement_sink_handler(movement_sink *sink) {
    return (keybind_event_handler){
        .movement = &sink->state,
        .user_data = sink,
        .running = movement_sink_running,
        .firing = movement_sink_firing,
        .reconcile_modes = movement_sink_reconcile_modes,
        .flush = movement_sink_flush,
        .movement_intercept_matches = movement_sink_intercept_matches,
        .movement_intercept = movement_sink_intercept,
        .command_down = movement_sink_command_down,
        .command_up = movement_sink_command_up,
    };
}

static void movement_sink_reset(movement_sink *sink) {
    memset(sink, 0, sizeof(*sink));
    keybind_movement_state_init(&sink->state);
}

static void test_keybind_event_integration(void) {
    keybind_struct northwest = {
        .command = "?MOVE_NW",
        .key = SDLK_A,
        .repeat = true,
    };
    keybind_struct northeast = {
        .command = "?MOVE_NE",
        .key = SDLK_B,
        .repeat = true,
    };
    keybind_struct shifted = {
        .command = "?MOVE_NE",
        .key = SDLK_A,
        .mod = SDL_KMOD_SHIFT,
        .repeat = false,
    };
    keybind_struct shifted_b = {
        .command = "?MOVE_NW",
        .key = SDLK_B,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct move_then_fire = {
        .command = "?MOVE_N;?FIREON",
        .key = SDLK_C,
        .repeat = true,
    };
    keybind_struct fire_then_move = {
        .command = "?FIREON;?MOVE_N",
        .key = SDLK_D,
        .repeat = true,
    };
    keybind_struct shifted_d = {
        .command = "?MOVE_NE",
        .key = SDLK_D,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct movement_sequence = {
        .command = "?MOVE_NW;?MOVE_NE",
        .key = SDLK_E,
        .repeat = true,
    };
    keybind_struct shifted_e = {
        .command = "?MOVE_NE",
        .key = SDLK_E,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct padded_northwest = {
        .command = " ?MOVE_NW;",
        .key = SDLK_F,
        .repeat = true,
    };
    keybind_struct shifted_f = {
        .command = "?MOVE_NE",
        .key = SDLK_F,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct southeast = {
        .command = "?MOVE_SE",
        .key = SDLK_G,
        .repeat = true,
    };
    keybind_struct shifted_modes = {
        .command = "?RUNON;?FIREON",
        .key = SDLK_H,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct move_then_fire_again = {
        .command = "?MOVE_NW;?FIREON",
        .key = SDLK_I,
        .repeat = true,
    };
    keybind_struct shifted_fire_move = {
        .command = "?FIREON;?MOVE_NE",
        .key = SDLK_I,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct compound_k = {
        .command = "?FIREON;?MOVE_NW",
        .key = SDLK_K,
        .repeat = true,
    };
    keybind_struct shifted_k = {
        .command = "?MOVE_NE",
        .key = SDLK_K,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct compound_l = {
        .command = "?FIREON;?MOVE_NE",
        .key = SDLK_L,
        .repeat = true,
    };
    keybind_struct shifted_l = {
        .command = "?MOVE_NE",
        .key = SDLK_L,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct intercepted_compound = {
        .command = "?MOVE_NW;?HELP",
        .key = SDLK_M,
        .repeat = true,
    };
    keybind_struct shifted_m = {
        .command = "?MOVE_NW",
        .key = SDLK_M,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct *bindings[] = {&northwest,         &shifted,
                                  &shifted_b,         &northeast,
                                  &move_then_fire,    &fire_then_move,
                                  &shifted_d,         &movement_sequence,
                                  &shifted_e,         &padded_northwest,
                                  &shifted_f,         &southeast,
                                  &shifted_modes,     &move_then_fire_again,
                                  &shifted_fire_move, &compound_k,
                                  &shifted_k,         &compound_l,
                                  &shifted_l,         &intercepted_compound,
                                  &shifted_m};
    movement_sink sink;
    key_struct key_states[SDL_SCANCODE_COUNT] = {0};
    SDL_KeyboardEvent event = {.type = SDL_EVENT_KEY_DOWN};

    movement_sink_reset(&sink);
    keybind_event_handler handler = movement_sink_handler(&sink);

    /* Exact modifier bindings retain precedence over an unmodified binding. */
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    event.mod = SDL_KMOD_LSHIFT;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 9);

    /* Custom physical keys coalesce into one composite at end of poll. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_NONE;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    /* Each accepted owner repeat is emitted; the other stream is ignored. */
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 4);
    for (size_t i = 0; i < sink.actions_num; i++) {
        TEST_CHECK(sink.actions[i] == KEYBIND_MOVEMENT_ACTION_MOVE);
        TEST_CHECK(sink.directions[i] == 8);
    }

    /* Semicolon ordering snapshots movement before or after fire transitions. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.key = SDLK_C;
    event.scancode = SDL_SCANCODE_C;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8 && !sink.firing_at_emit[0]);
    TEST_CHECK(sink.firing);

    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_D;
    event.scancode = SDL_SCANCODE_D;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8 && sink.firing_at_emit[0]);

    /* A notification shortcut consumes movement before it enters gameplay state. */
    movement_sink_reset(&sink);
    sink.intercept_movement = true;
    handler = movement_sink_handler(&sink);
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 0);
    TEST_CHECK(sink.intercepted_num == 1);

    /* Multiple movement tokens retain semicolon order on down and repeat. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_E;
    event.scancode = SDL_SCANCODE_E;
    event.repeat = false;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[0] == 7 && sink.directions[1] == 9);
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 4 && sink.directions[2] == 7 && sink.directions[3] == 9);

    /* An unrelated key-up does not split same-poll chord composition. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.type = SDL_EVENT_KEY_DOWN;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent unrelated = {
        .type = SDL_EVENT_KEY_UP,
        .key = SDLK_F,
        .scancode = SDL_SCANCODE_F,
    };
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &unrelated,
                                    key_states,
                                    &handler);
    TEST_CHECK(sink.actions_num == 0);
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    /* An unrelated modifier key-up also leaves chord coalescing intact. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent modifier_up = {
        .type = SDL_EVENT_KEY_UP,
        .key = SDLK_LSHIFT,
        .scancode = SDL_SCANCODE_LSHIFT,
    };
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(sink.actions_num == 0);
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    /* Modifier release flushes movement before modified mode reconciliation. */
    movement_sink_reset(&sink);
    sink.firing = true;
    handler = movement_sink_handler(&sink);
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_H].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 7 && sink.firing_at_emit[0]);
    sink.firing = false;
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 2 && sink.directions[1] == 7 && !sink.firing_at_emit[1]);

    movement_sink_reset(&sink);
    sink.running = true;
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(sink.actions_num == 1 && sink.running_at_emit[0]);
    sink.running = false;
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 2 && !sink.running_at_emit[1]);

    /* Modifier-qualified movement cannot remain stale after modifier release. */
    key_states[SDL_SCANCODE_H].pressed = false;
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_A].pressed = true;
    modifier_up.mod = SDL_KMOD_NONE;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 9);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[1] == 7);
    event.mod = SDL_KMOD_NONE;
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 3 && sink.directions[2] == 7);
    event.repeat = false;
    event.key = SDLK_G;
    event.scancode = SDL_SCANCODE_G;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 4 && sink.directions[3] == 3);

    /* Multiple invalid modified keys are removed as one logical transition. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_B].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    /* Modifier rebinds reset repeat ownership even when the old owner survives. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_NONE;
    event.repeat = false;
    event.key = SDLK_G;
    event.scancode = SDL_SCANCODE_G;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.mod = SDL_KMOD_LSHIFT;
    event.repeat = false;
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.mod = SDL_KMOD_NONE;
    event.repeat = true;
    event.key = SDLK_G;
    event.scancode = SDL_SCANCODE_G;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_G].pressed = true;
    key_states[SDL_SCANCODE_B].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    size_t actions_before_repeat = sink.actions_num;
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == actions_before_repeat + 1);

    /* Compound fallback bindings retain command and movement segment ordering. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_LSHIFT;
    event.repeat = false;
    event.key = SDLK_D;
    event.scancode = SDL_SCANCODE_D;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_D].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[0] == 9 && sink.directions[1] == 8);
    TEST_CHECK(sink.firing && sink.firing_at_emit[1]);

    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_E;
    event.scancode = SDL_SCANCODE_E;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_E].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 3 && sink.directions[0] == 9 && sink.directions[1] == 7 &&
               sink.directions[2] == 9);

    /* Notification-owned fallback movement never enters gameplay state. */
    movement_sink_reset(&sink);
    sink.intercept_movement = true;
    handler = movement_sink_handler(&sink);
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    key_states[SDL_SCANCODE_A].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 9);
    TEST_CHECK(sink.intercepted_num == 1);
    TEST_CHECK(!keybind_movement_state_has_scancode(&sink.state, SDL_SCANCODE_A));

    /* Interception receives the normalized segment from padded custom macros. */
    movement_sink_reset(&sink);
    sink.intercept_movement = true;
    handler = movement_sink_handler(&sink);
    event.key = SDLK_F;
    event.scancode = SDL_SCANCODE_F;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    key_states[SDL_SCANCODE_F].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.intercepted_num == 1);

    /* Simple and compound fallbacks share one pre-transition movement batch. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_D;
    event.scancode = SDL_SCANCODE_D;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_A].pressed = true;
    key_states[SDL_SCANCODE_D].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[0] == 9 && sink.directions[1] == 8);
    TEST_CHECK(sink.firing_at_emit[1]);

    /* Invalid old modes are reconciled before the same mode is replayed later. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_I;
    event.scancode = SDL_SCANCODE_I;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.firing);
    key_states[SDL_SCANCODE_I].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[1] == 7);
    TEST_CHECK(!sink.firing_at_emit[1] && sink.firing);

    /* Two compound fallbacks emit their pre-seeded composite only once. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_K;
    event.scancode = SDL_SCANCODE_K;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_L;
    event.scancode = SDL_SCANCODE_L;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_K].pressed = true;
    key_states[SDL_SCANCODE_L].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[0] == 9 && sink.directions[1] == 8);

    /* An intercepted compound cannot suppress a simple sibling fallback. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_M;
    event.scancode = SDL_SCANCODE_M;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    sink.intercept_movement = true;
    key_states[SDL_SCANCODE_B].pressed = true;
    key_states[SDL_SCANCODE_M].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.directions[0] == 7 && sink.directions[1] == 9);
    TEST_CHECK(sink.intercepted_num == 1);

    /* A queued RUN_STOP coalesces the final invalid movement release. */
    movement_sink_reset(&sink);
    TEST_CHECK(keybind_movement_state_press(&sink.state, SDL_SCANCODE_J, 9, false, true));
    keybind_movement_state_set_modifier(&sink.state, SDL_SCANCODE_J, SDL_KMOD_SHIFT);
    movement_sink_flush(&sink);
    TEST_CHECK(keybind_movement_state_press(&sink.state, SDL_SCANCODE_J, 9, true, true));
    movement_sink_flush(&sink);
    keybind_movement_state_run_released(&sink.state, true);
    keybind_movement_state_reconcile_modifiers(&sink.state,
                                               SDL_KMOD_NONE,
                                               NULL,
                                               0,
                                               false,
                                               false,
                                               false,
                                               false);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 3);
    TEST_CHECK(sink.actions[2] == KEYBIND_MOVEMENT_ACTION_RUN_STOP);
}

int main(void) {
    test_legacy_keycode_migration();
    test_shortcut_names();
    test_keybind_loader();
    test_bundled_defaults();
    test_event_matching();
    test_movement_commands();
    test_movement_chords();
    test_movement_repeat_and_release();
    test_movement_duplicates_and_repeat_selection();
    test_movement_boundaries_and_modifiers();
    test_keybind_event_integration();
    return 0;
}
