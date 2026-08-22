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
    if (keybind_fixture != NULL && !strcmp(modes, "r")) {
        stream = tmpfile();
        TEST_CHECK(stream != NULL);
        TEST_CHECK(fputs(keybind_fixture, stream) >= 0);
        rewind(stream);
        return stream;
    }

    return fopen(ATRINIK_TEST_BINARY_DIR "/keybind-roundtrip.dat", modes);
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

    keybind_fixture =
        "keycode_format " KEYBIND_KEYCODE_FORMAT "\nbind\nkey 102\ncommand ?FIRE_READY\nend\n"
        "bind\nkey 103\nmod 3\nrepeat 1\ncommand ?FIRE_READY_CUSTOM\nend\n"
        "bind\nkey 104\ncommand ?FIRE_READY;?HELP\nend\n"
        "bind\nkey 105\ncommand /custom\nend\n"
        "bind\nkey 119\ncommand /custom-wasd\nend\n";
    keybind_load();
    TEST_CHECK(keybindings_num == 4);
    TEST_CHECK(!strcmp(keybindings[0]->command, "?FIRE_READY_CUSTOM"));
    TEST_CHECK(keybindings[0]->key == SDLK_G);
    TEST_CHECK(keybindings[0]->mod == SDL_KMOD_SHIFT);
    TEST_CHECK(keybindings[0]->repeat == 1);
    TEST_CHECK(!strcmp(keybindings[1]->command, "?FIRE_READY;?HELP"));
    TEST_CHECK(!strcmp(keybindings[2]->command, "/custom"));
    TEST_CHECK(!strcmp(keybindings[3]->command, "/custom-wasd") && keybindings[3]->key == SDLK_W);

    keybind_fixture = NULL;
    keybind_save();
    keybind_fixture_reset();
    keybind_load();
    TEST_CHECK(keybindings_num == 4);
    TEST_CHECK(!strcmp(keybindings[0]->command, "?FIRE_READY_CUSTOM"));
    TEST_CHECK(keybindings[0]->key == SDLK_G);
    TEST_CHECK(keybindings[0]->mod == SDL_KMOD_SHIFT);
    TEST_CHECK(keybindings[0]->repeat == 1);
    TEST_CHECK(!strcmp(keybindings[1]->command, "?FIRE_READY;?HELP"));
    TEST_CHECK(!strcmp(keybindings[2]->command, "/custom"));
    TEST_CHECK(!strcmp(keybindings[3]->command, "/custom-wasd") && keybindings[3]->key == SDLK_W);
    keybind_save();
    keybind_fixture_reset();
    TEST_CHECK(remove(ATRINIK_TEST_BINARY_DIR "/keybind-roundtrip.dat") == 0);
}

typedef struct bundled_binding {
    const char *command;
    SDL_Keycode key;
    SDL_Keymod mod;
    bool repeat;
} bundled_binding;

typedef struct parsed_bundled_binding {
    char command[128];
    SDL_Keycode key;
    SDL_Keymod mod;
    bool repeat;
} parsed_bundled_binding;

static bool bundled_binding_matches(const parsed_bundled_binding *actual,
                                    const bundled_binding *expected) {
    return !strcmp(actual->command, expected->command) && actual->key == expected->key &&
           actual->mod == expected->mod && actual->repeat == expected->repeat;
}

static void test_bundled_defaults(void) {
    static const bundled_binding expected[] = {
        {"?MOVE_N", SDLK_KP_9, SDL_KMOD_NONE, true},
        {"?MOVE_NE", SDLK_KP_6, SDL_KMOD_NONE, true},
        {"?MOVE_E", SDLK_KP_3, SDL_KMOD_NONE, true},
        {"?MOVE_SE", SDLK_KP_2, SDL_KMOD_NONE, true},
        {"?MOVE_S", SDLK_KP_1, SDL_KMOD_NONE, true},
        {"?MOVE_SW", SDLK_KP_4, SDL_KMOD_NONE, true},
        {"?MOVE_W", SDLK_KP_7, SDL_KMOD_NONE, true},
        {"?MOVE_NW", SDLK_KP_8, SDL_KMOD_NONE, true},
        {"?MOVE_STAY", SDLK_KP_5, SDL_KMOD_NONE, true},
        {"?MOVE_NW", SDLK_W, SDL_KMOD_NONE, true},
        {"?MOVE_SW", SDLK_A, SDL_KMOD_NONE, true},
        {"?MOVE_SE", SDLK_S, SDL_KMOD_NONE, true},
        {"?MOVE_NE", SDLK_D, SDL_KMOD_NONE, true},
        {"/left", SDLK_LEFTBRACKET, SDL_KMOD_NONE, true},
        {"/right", SDLK_RIGHTBRACKET, SDL_KMOD_NONE, true},
        {"/push", SDLK_K, SDL_KMOD_NONE, true},
        {"?CONSOLE", SDLK_RETURN, SDL_KMOD_NONE, false},
        {"?CONSOLE", SDLK_KP_ENTER, SDL_KMOD_NONE, false},
        {"?APPLY", SDLK_F, SDL_KMOD_NONE, false},
        {"?EXAMINE", SDLK_E, SDL_KMOD_NONE, false},
        {"?LOCK", SDLK_L, SDL_KMOD_NONE, false},
        {"?MARK", SDLK_B, SDL_KMOD_NONE, false},
        {"?DROP", SDLK_Z, SDL_KMOD_NONE, true},
        {"?GET", SDLK_G, SDL_KMOD_NONE, true},
        {"/apply", SDLK_SPACE, SDL_KMOD_NONE, false},
        {"?SPELL_LIST", SDLK_F9, SDL_KMOD_NONE, false},
        {"?SKILL_LIST", SDLK_F10, SDL_KMOD_NONE, false},
        {"?PARTY_LIST", SDLK_F11, SDL_KMOD_NONE, false},
        {"?HELP", SDLK_F12, SDL_KMOD_NONE, false},
        {"?UP", SDLK_UP, SDL_KMOD_NONE, true},
        {"?DOWN", SDLK_DOWN, SDL_KMOD_NONE, true},
        {"?LEFT", SDLK_LEFT, SDL_KMOD_NONE, true},
        {"?RIGHT", SDLK_RIGHT, SDL_KMOD_NONE, true},
        {"?QLIST", SDLK_Q, SDL_KMOD_NONE, false},
        {"/region_map", SDLK_M, SDL_KMOD_NONE, false},
        {"?COMBAT", SDLK_C, SDL_KMOD_NONE, false},
        {"?TARGET_ENEMY", SDLK_X, SDL_KMOD_NONE, false},
        {"?TARGET_FRIEND", SDLK_Y, SDL_KMOD_NONE, false},
        {"/widget_toggle inventory:main", SDLK_TAB, SDL_KMOD_NONE, false},
        {"/widget_focus inventory", SDLK_TAB, SDL_KMOD_SHIFT, false},
        {"?RUNON", SDLK_LALT, SDL_KMOD_NONE, false},
        {"?RUNON", SDLK_RALT, SDL_KMOD_NONE, false},
        {"?FIREON", SDLK_RCTRL, SDL_KMOD_NONE, false},
        {"?FIREON", SDLK_LCTRL, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_GROUP_PREV", SDLK_9, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_GROUP_NEXT", SDLK_0, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_1", SDLK_1, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_2", SDLK_2, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_3", SDLK_3, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_4", SDLK_4, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_5", SDLK_5, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_6", SDLK_6, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_7", SDLK_7, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_8", SDLK_8, SDL_KMOD_NONE, false},
        {"?COPY", SDLK_C, SDL_KMOD_CTRL, false},
        {"?PASTE", SDLK_V, SDL_KMOD_CTRL, false},
        {"?HELLO", SDLK_T, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_GROUP_CYCLE", (SDL_Keycode)96, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_GROUP_NEXT", SDLK_END, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_GROUP_PREV", SDLK_HOME, SDL_KMOD_NONE, false},
        {"?QUICKSLOT_SET_KEY", SDLK_LSHIFT, SDL_KMOD_NONE, false},
        {"?COMBAT_FORCE", SDLK_C, SDL_KMOD_SHIFT, false},
    };
    parsed_bundled_binding actual[128];
    size_t actual_num = 0;
    char line[256];
    char command[sizeof(actual[0].command)] = {0};
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Keymod mod = SDL_KMOD_NONE;
    bool repeat = false;
    bool in_bind = false;
    bool have_key = false;
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
        } else if (!strcmp(text, "bind")) {
            TEST_CHECK(!in_bind);
            in_bind = true;
            command[0] = '\0';
            key = SDLK_UNKNOWN;
            mod = SDL_KMOD_NONE;
            repeat = false;
            have_key = false;
        } else if (!strcmp(text, "end")) {
            TEST_CHECK(in_bind && have_key && command[0] != '\0');
            TEST_CHECK(actual_num < arraysize(actual));
            snprintf(actual[actual_num].command, sizeof(actual[actual_num].command), "%s", command);
            actual[actual_num].key = key;
            actual[actual_num].mod = mod;
            actual[actual_num].repeat = repeat;
            actual_num++;
            in_bind = false;
        } else if (in_bind && !strncmp(text, "key ", 4)) {
            TEST_CHECK(keybind_keycode_parse(text + 4, false, &key));
            have_key = true;
        } else if (in_bind && !strncmp(text, "mod ", 4)) {
            uint32_t value;
            TEST_CHECK(keybind_uint32_parse(text + 4, UINT16_MAX, &value));
            mod = keybind_adjust_kmod((SDL_Keymod)value);
        } else if (in_bind && !strncmp(text, "repeat ", 7)) {
            uint32_t value;
            TEST_CHECK(keybind_uint32_parse(text + 7, 1, &value));
            repeat = value != 0;
        } else if (in_bind && !strncmp(text, "command ", 8)) {
            TEST_CHECK(strlen(text + 8) < sizeof(command));
            snprintf(command, sizeof(command), "%s", text + 8);
        }
    }
    TEST_CHECK(fclose(stream) == 0);
    TEST_CHECK(marker && !in_bind);
    TEST_CHECK(actual_num == arraysize(expected));

    for (size_t i = 0; i < actual_num; i++) {
        for (size_t j = 0; j < i; j++) {
            TEST_CHECK(actual[i].key != actual[j].key || actual[i].mod != actual[j].mod);
        }
        bool found = false;
        for (size_t j = 0; j < arraysize(expected); j++) {
            if (bundled_binding_matches(&actual[i], &expected[j])) {
                found = true;
                break;
            }
        }
        TEST_CHECK(found);
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

static uint32_t expect_movement_at(keybind_movement_state *state,
                                   keybind_movement_action expected_action,
                                   uint8_t expected_direction,
                                   int line) {
    uint8_t direction = UINT8_MAX;
    uint32_t epoch = 0;
    keybind_movement_action action = keybind_movement_state_flush(state, &direction, &epoch);

    if (action != expected_action) {
        fprintf(stderr,
                "line %d: movement action %d, expected %d (direction %u)\n",
                line,
                action,
                expected_action,
                direction);
        abort();
    }
    if (expected_action != KEYBIND_MOVEMENT_ACTION_NONE) {
        TEST_CHECK(direction == expected_direction);
    }
    return epoch;
}

#define expect_movement(state, action, direction) \
    expect_movement_at((state), (action), (direction), __LINE__)

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
        {8, 6, 9},
        {6, 2, 3},
        {2, 4, 1},
        {4, 8, 7},
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
            expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, cases[i].result);
        }
    }
}

static void test_movement_queue_replacement(void) {
    keybind_movement_state state;

    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 2, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 2);
    for (size_t i = 0; i < 4; i++) {
        TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 2, true, true));
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 2);
    }

    keybind_movement_state_press(&state, SDL_SCANCODE_B, 6, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 3);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_B, 6, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 3);

    keybind_movement_state_release(&state, SDL_SCANCODE_B, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 2);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);

    /* Completed standalone taps begin a new queue epoch instead of replacing. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 6, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 6);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 8, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);

    /* Removing the final modified tap also closes its replacement epoch. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 9, false, true);
    keybind_movement_state_set_modifier(&state, SDL_SCANCODE_A, SDL_KMOD_SHIFT);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 9);
    keybind_movement_state_reconcile_modifiers(&state,
                                               SDL_KMOD_NONE,
                                               NULL,
                                               0,
                                               false,
                                               false,
                                               false,
                                               false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 2, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 2);
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
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 7);
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
        expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 8);
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
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 8);
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

    /* A quick running tap moves once before its ordered direction-zero stop. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Once repeat starts, final release cancels the replaceable backlog. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* Non-perpendicular and three-direction chords use the newest active key. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 8, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 8);
    keybind_movement_state_press(&state, SDL_SCANCODE_C, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 9);

    /* A quick running chord preserves its first step, then stops in order. */
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    keybind_movement_state_release(&state, SDL_SCANCODE_B, true, false);
    keybind_movement_state_release(&state, SDL_SCANCODE_C, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
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
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
    keybind_movement_state_run_released(&state, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A run stop closes the repeat epoch before a later key release. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    uint32_t running_epoch = expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    TEST_CHECK(expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_STOP, 0) == running_epoch);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, false, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);

    /* A queued ordinary move follows the prior ordered quick-run stop. */
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_run_released(&state, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
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

    /* Focus loss is flushed before a same-poll key-down starts a fresh epoch. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    uint32_t focus_epoch = expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    TEST_CHECK(keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, true, true));
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 7);
    keybind_movement_state_clear(&state, true, false);
    TEST_CHECK(expect_movement(&state, KEYBIND_MOVEMENT_ACTION_STOP, 5) == focus_epoch);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    uint32_t refocused_epoch = expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 9);
    TEST_CHECK(refocused_epoch != 0 && refocused_epoch != focus_epoch);

    /* Same-poll chord releases retain the initial composite and each transition. */
    keybind_movement_state_init(&state);
    keybind_movement_state_press(&state, SDL_SCANCODE_A, 7, false, true);
    keybind_movement_state_press(&state, SDL_SCANCODE_B, 9, false, true);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_MOVE, 8);
    keybind_movement_state_release(&state, SDL_SCANCODE_A, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_REPLACE, 9);
    keybind_movement_state_release(&state, SDL_SCANCODE_B, true, false);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_RUN_TAP_STOP, 0);
    expect_movement(&state, KEYBIND_MOVEMENT_ACTION_NONE, 0);
}

typedef struct movement_sink {
    keybind_movement_state state;
    keybind_movement_action actions[32];
    uint8_t directions[32];
    uint32_t epochs[32];
    bool running_at_emit[32];
    bool firing_at_emit[32];
    size_t actions_num;
    bool running;
    bool firing;
    bool intercept_movement;
    bool intercept_modes;
    size_t intercepted_num;
    size_t command_ups;
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
    uint32_t epoch;

    while ((action = keybind_movement_state_flush(&sink->state, &direction, &epoch)) !=
           KEYBIND_MOVEMENT_ACTION_NONE) {
        TEST_CHECK(sink->actions_num < arraysize(sink->actions));
        size_t i = sink->actions_num++;
        sink->actions[i] = action;
        sink->directions[i] = direction;
        sink->epochs[i] = epoch;
        sink->running_at_emit[i] = sink->running;
        sink->firing_at_emit[i] = sink->firing;
    }
}

static bool movement_sink_command_down(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    if (sink->intercept_modes && (!strcmp(command, "?RUNON") || !strcmp(command, "?FIREON"))) {
        return false;
    }
    if (!strcmp(command, "?RUNON")) {
        sink->running = true;
    } else if (!strcmp(command, "?FIREON")) {
        sink->firing = true;
    } else if (!strcmp(command, "?RUNON_TOGGLE")) {
        sink->running = !sink->running;
    } else if (!strcmp(command, "?FIREON_TOGGLE")) {
        sink->firing = !sink->firing;
    }
    return true;
}

static bool movement_sink_intercept_matches(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    return (sink->intercept_movement && !strcmp(command, "?MOVE_NW")) ||
           (sink->intercept_modes && (!strcmp(command, "?RUNON") || !strcmp(command, "?FIREON")));
}

static void movement_sink_intercept(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    TEST_CHECK(!strcmp(command, "?MOVE_NW"));
    sink->intercepted_num++;
}

static void movement_sink_command_up(const char *command, void *user_data) {
    movement_sink *sink = user_data;

    if (!strcmp(command, "?RUNON")) {
        sink->command_ups++;
        sink->running = false;
    } else if (!strcmp(command, "?FIREON")) {
        sink->command_ups++;
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

static void test_keybind_event_cardinal_chords(void) {
    static const struct {
        char *first_command;
        char *second_command;
        uint8_t first_direction;
        uint8_t second_direction;
        uint8_t chord_direction;
        SDL_Keycode first_key;
        SDL_Scancode first_scancode;
        SDL_Keycode second_key;
        SDL_Scancode second_scancode;
    } cases[] = {
        {"?MOVE_NW", "?MOVE_NE", 7, 9, 8, SDLK_W, SDL_SCANCODE_W, SDLK_D, SDL_SCANCODE_D},
        {"?MOVE_NE", "?MOVE_SE", 9, 3, 6, SDLK_D, SDL_SCANCODE_D, SDLK_S, SDL_SCANCODE_S},
        {"?MOVE_SE", "?MOVE_SW", 3, 1, 2, SDLK_S, SDL_SCANCODE_S, SDLK_A, SDL_SCANCODE_A},
        {"?MOVE_SW", "?MOVE_NW", 1, 7, 4, SDLK_A, SDL_SCANCODE_A, SDLK_W, SDL_SCANCODE_W},
    };

    for (size_t i = 0; i < arraysize(cases); i++) {
        for (size_t reverse = 0; reverse < 2; reverse++) {
            SDL_Keycode first_key = reverse ? cases[i].second_key : cases[i].first_key;
            SDL_Scancode first_scancode =
                reverse ? cases[i].second_scancode : cases[i].first_scancode;
            SDL_Keycode second_key = reverse ? cases[i].first_key : cases[i].second_key;
            SDL_Scancode second_scancode =
                reverse ? cases[i].first_scancode : cases[i].second_scancode;
            keybind_struct first = {
                .command = reverse ? cases[i].second_command : cases[i].first_command,
                .key = first_key,
                .repeat = true,
            };
            keybind_struct second = {
                .command = reverse ? cases[i].first_command : cases[i].second_command,
                .key = second_key,
                .repeat = true,
            };
            uint8_t first_direction =
                reverse ? cases[i].second_direction : cases[i].first_direction;
            uint8_t second_direction =
                reverse ? cases[i].first_direction : cases[i].second_direction;
            keybind_struct *bindings[] = {&first, &second};
            key_struct key_states[SDL_SCANCODE_COUNT] = {0};
            movement_sink sink;
            movement_sink_reset(&sink);
            keybind_event_handler handler = movement_sink_handler(&sink);
            SDL_KeyboardEvent event = {
                .type = SDL_EVENT_KEY_DOWN,
                .key = first_key,
                .scancode = first_scancode,
            };

            /* Two downs in one poll produce only the composed direction. */
            key_states[first_scancode].pressed = true;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            event.key = second_key;
            event.scancode = second_scancode;
            key_states[second_scancode].pressed = true;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 1);
            TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_MOVE);
            TEST_CHECK(sink.directions[0] == cases[i].chord_direction);

            /* Either physical repeat retains one logical chord stream. */
            event.repeat = true;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            event.key = first_key;
            event.scancode = first_scancode;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            TEST_CHECK(sink.actions_num == 2);
            TEST_CHECK(sink.actions[1] == KEYBIND_MOVEMENT_ACTION_MOVE);
            TEST_CHECK(sink.directions[1] == cases[i].chord_direction);

            /* Partial release resumes the other cardinal; final release stops once. */
            event.type = SDL_EVENT_KEY_UP;
            event.repeat = false;
            event.key = first_key;
            event.scancode = first_scancode;
            key_states[first_scancode].pressed = false;
            keybind_event_reconcile_release(bindings,
                                            arraysize(bindings),
                                            &event,
                                            key_states,
                                            &handler);
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 3);
            TEST_CHECK(sink.actions[2] == KEYBIND_MOVEMENT_ACTION_REPLACE);
            TEST_CHECK(sink.directions[2] == second_direction);

            event.key = second_key;
            event.scancode = second_scancode;
            key_states[second_scancode].pressed = false;
            keybind_event_reconcile_release(bindings,
                                            arraysize(bindings),
                                            &event,
                                            key_states,
                                            &handler);
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 4);
            TEST_CHECK(sink.actions[3] == KEYBIND_MOVEMENT_ACTION_STOP);
            TEST_CHECK(sink.directions[3] == 5);
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 4);

            /* A later second down replaces the already-emitted cardinal. */
            movement_sink_reset(&sink);
            handler = movement_sink_handler(&sink);
            event.type = SDL_EVENT_KEY_DOWN;
            event.key = first_key;
            event.scancode = first_scancode;
            key_states[first_scancode].pressed = true;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 1);
            TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_MOVE);
            TEST_CHECK(sink.directions[0] == first_direction);

            event.key = second_key;
            event.scancode = second_scancode;
            key_states[second_scancode].pressed = true;
            TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
            movement_sink_flush(&sink);
            TEST_CHECK(sink.actions_num == 2);
            TEST_CHECK(sink.actions[1] == KEYBIND_MOVEMENT_ACTION_REPLACE);
            TEST_CHECK(sink.directions[1] == cases[i].chord_direction);
        }
    }
}

static void test_keybind_event_direction_transitions(void) {
    keybind_struct east = {
        .command = "?MOVE_E",
        .key = SDLK_A,
        .repeat = true,
    };
    keybind_struct north = {
        .command = "?MOVE_N",
        .key = SDLK_B,
        .repeat = true,
    };
    keybind_struct south = {
        .command = "?MOVE_S",
        .key = SDLK_C,
        .repeat = true,
    };
    keybind_struct west = {
        .command = "?MOVE_W",
        .key = SDLK_D,
        .repeat = true,
    };
    keybind_struct *bindings[] = {&east, &north, &south, &west};
    key_struct key_states[SDL_SCANCODE_COUNT] = {0};
    movement_sink sink;
    movement_sink_reset(&sink);
    keybind_event_handler handler = movement_sink_handler(&sink);
    SDL_KeyboardEvent event = {
        .type = SDL_EVENT_KEY_DOWN,
        .key = SDLK_A,
        .scancode = SDL_SCANCODE_A,
    };

    key_states[event.scancode].pressed = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    key_states[event.scancode].pressed = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    event.type = SDL_EVENT_KEY_UP;
    key_states[event.scancode].pressed = false;
    keybind_event_reconcile_release(bindings, arraysize(bindings), &event, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    event.type = SDL_EVENT_KEY_DOWN;
    event.key = SDLK_C;
    event.scancode = SDL_SCANCODE_C;
    key_states[event.scancode].pressed = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    event.type = SDL_EVENT_KEY_UP;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    key_states[event.scancode].pressed = false;
    keybind_event_reconcile_release(bindings, arraysize(bindings), &event, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    event.type = SDL_EVENT_KEY_DOWN;
    event.key = SDLK_D;
    event.scancode = SDL_SCANCODE_D;
    key_states[event.scancode].pressed = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);

    static const uint8_t expected_directions[] = {6, 9, 6, 3, 2, 1};
    static const keybind_movement_action expected_actions[] = {
        KEYBIND_MOVEMENT_ACTION_MOVE,
        KEYBIND_MOVEMENT_ACTION_REPLACE,
        KEYBIND_MOVEMENT_ACTION_REPLACE,
        KEYBIND_MOVEMENT_ACTION_REPLACE,
        KEYBIND_MOVEMENT_ACTION_REPLACE,
        KEYBIND_MOVEMENT_ACTION_REPLACE,
    };
    TEST_CHECK(sink.actions_num == arraysize(expected_directions));
    for (size_t i = 0; i < arraysize(expected_directions); i++) {
        TEST_CHECK(sink.actions[i] == expected_actions[i]);
        TEST_CHECK(sink.directions[i] == expected_directions[i]);
    }
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
        .command = "?MOVE_NW;?FIREON",
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
        .command = "?MOVE_NE;?FIREON",
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
    keybind_struct fire_owner = {
        .command = "?FIREON",
        .key = SDLK_O,
    };
    keybind_struct run_fallback = {
        .command = "?MOVE_NW",
        .key = SDLK_P,
        .repeat = true,
    };
    keybind_struct shifted_run_move = {
        .command = "?RUNON;?MOVE_NE",
        .key = SDLK_P,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct run_owner = {
        .command = "?RUNON",
        .key = SDLK_Q,
    };
    keybind_struct same_direction = {
        .command = "?MOVE_NW",
        .key = SDLK_R,
        .repeat = true,
    };
    keybind_struct shifted_same_direction = {
        .command = "?MOVE_NW",
        .key = SDLK_R,
        .mod = SDL_KMOD_SHIFT,
        .repeat = true,
    };
    keybind_struct shifted_consumed_run = {
        .command = "?RUNON",
        .key = SDLK_S,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct mode_movement_fallback = {
        .command = "?MOVE_N",
        .key = SDLK_T,
        .repeat = true,
    };
    keybind_struct shifted_mode_only = {
        .command = "?RUNON",
        .key = SDLK_T,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct second_run_owner = {
        .command = "?RUNON",
        .key = SDLK_U,
    };
    keybind_struct shifted_run_only = {
        .command = "?RUNON",
        .key = SDLK_V,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct help = {
        .command = "?HELP",
        .key = SDLK_W,
    };
    keybind_struct run_same_fallback = {
        .command = "?RUNON",
        .key = SDLK_X,
    };
    keybind_struct shifted_run_same = {
        .command = "?RUNON",
        .key = SDLK_X,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct fire_same_fallback = {
        .command = "?FIREON",
        .key = SDLK_Y,
    };
    keybind_struct shifted_fire_same = {
        .command = "?FIREON",
        .key = SDLK_Y,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct run_movement_fallback = {
        .command = "?RUNON;?MOVE_N",
        .key = SDLK_Z,
        .repeat = true,
    };
    keybind_struct shifted_run_same_compound = {
        .command = "?RUNON",
        .key = SDLK_Z,
        .mod = SDL_KMOD_SHIFT,
    };
    keybind_struct run_toggle = {
        .command = "?RUNON_TOGGLE",
        .key = SDLK_1,
    };
    keybind_struct fire_toggle = {
        .command = "?FIREON_TOGGLE",
        .key = SDLK_2,
    };
    keybind_struct *bindings[] = {&northwest,
                                  &shifted,
                                  &shifted_b,
                                  &northeast,
                                  &move_then_fire,
                                  &fire_then_move,
                                  &shifted_d,
                                  &movement_sequence,
                                  &shifted_e,
                                  &padded_northwest,
                                  &shifted_f,
                                  &southeast,
                                  &shifted_modes,
                                  &move_then_fire_again,
                                  &shifted_fire_move,
                                  &compound_k,
                                  &shifted_k,
                                  &compound_l,
                                  &shifted_l,
                                  &intercepted_compound,
                                  &shifted_m,
                                  &fire_owner,
                                  &run_fallback,
                                  &shifted_run_move,
                                  &run_owner,
                                  &same_direction,
                                  &shifted_same_direction,
                                  &shifted_consumed_run,
                                  &mode_movement_fallback,
                                  &shifted_mode_only,
                                  &second_run_owner,
                                  &shifted_run_only,
                                  &help,
                                  &run_same_fallback,
                                  &shifted_run_same,
                                  &fire_same_fallback,
                                  &shifted_fire_same,
                                  &run_movement_fallback,
                                  &shifted_run_same_compound,
                                  &run_toggle,
                                  &fire_toggle};
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
    TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_MOVE &&
               sink.actions[1] == KEYBIND_MOVEMENT_ACTION_MOVE);
    TEST_CHECK(sink.epochs[0] != 0 && sink.epochs[1] != 0 && sink.epochs[0] != sink.epochs[1]);
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 4 && sink.directions[2] == 7 && sink.directions[3] == 9);
    TEST_CHECK(sink.actions[2] == KEYBIND_MOVEMENT_ACTION_MOVE &&
               sink.actions[3] == KEYBIND_MOVEMENT_ACTION_MOVE);
    TEST_CHECK(sink.epochs[2] != sink.epochs[3]);

    /* A multi-movement macro first replaces an existing held stream. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    event.key = SDLK_E;
    event.scancode = SDL_SCANCODE_E;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 3);
    TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_MOVE && sink.directions[0] == 9);
    TEST_CHECK(sink.actions[1] == KEYBIND_MOVEMENT_ACTION_REPLACE && sink.directions[1] == 8);
    TEST_CHECK(sink.actions[2] == KEYBIND_MOVEMENT_ACTION_MOVE && sink.directions[2] == 9);
    TEST_CHECK(sink.epochs[0] == sink.epochs[1] && sink.epochs[1] != sink.epochs[2]);

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
        .key = SDLK_W,
        .scancode = SDL_SCANCODE_W,
    };
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &unrelated,
                                    key_states,
                                    &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &unrelated, &handler));
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
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_H;
    event.scancode = SDL_SCANCODE_H;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.mod = SDL_KMOD_NONE;
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
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_H;
    event.scancode = SDL_SCANCODE_H;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
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

    /* A compound fallback preserves an identical mode without an off/on cycle. */
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
    TEST_CHECK(sink.firing_at_emit[1] && sink.firing);

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

    /* Invalid compound owners preserve independent held RUN/FIRE producers. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.key = SDLK_O;
    event.scancode = SDL_SCANCODE_O;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_I;
    event.scancode = SDL_SCANCODE_I;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    key_states[SDL_SCANCODE_I].pressed = true;
    key_states[SDL_SCANCODE_O].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.firing_at_emit[1]);

    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.key = SDLK_Q;
    event.scancode = SDL_SCANCODE_Q;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_P;
    event.scancode = SDL_SCANCODE_P;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    key_states[SDL_SCANCODE_P].pressed = true;
    key_states[SDL_SCANCODE_Q].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && sink.running_at_emit[1]);

    /* A physically pressed but gameplay-unowned key cannot preserve RUN. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.key = SDLK_P;
    event.scancode = SDL_SCANCODE_P;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    key_states[SDL_SCANCODE_P].pressed = true;
    key_states[SDL_SCANCODE_Q].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 2 && !sink.running_at_emit[1]);

    /* Modifier-qualified mode-only owners release with their modifier. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_H;
    event.scancode = SDL_SCANCODE_H;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.running && sink.firing);
    key_states[SDL_SCANCODE_H].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(!sink.running && !sink.firing && sink.command_ups == 2);

    /* Consumed mode downs never become owners or disable latched modes on key-up. */
    movement_sink_reset(&sink);
    sink.intercept_modes = true;
    sink.firing = true;
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_O;
    event.scancode = SDL_SCANCODE_O;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(!keybind_movement_state_mode_owned(&sink.state, false));
    SDL_KeyboardEvent owner_up = event;
    owner_up.type = SDL_EVENT_KEY_UP;
    keybind_event_reconcile_release(bindings, arraysize(bindings), &owner_up, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &owner_up, &handler));
    TEST_CHECK(sink.firing && sink.command_ups == 0);

    /* A normally accepted last owner invokes command-up exactly once. */
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.type = SDL_EVENT_KEY_DOWN;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.firing);
    keybind_event_reconcile_release(bindings, arraysize(bindings), &owner_up, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &owner_up, &handler));
    TEST_CHECK(!sink.firing && sink.command_ups == 1);

    /* A widget-consumed modified mode key cannot force movement reconciliation. */
    movement_sink_reset(&sink);
    sink.running = true;
    handler = movement_sink_handler(&sink);
    event.type = SDL_EVENT_KEY_DOWN;
    event.repeat = false;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_R;
    event.scancode = SDL_SCANCODE_R;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 7);
    key_states[SDL_SCANCODE_R].pressed = true;
    key_states[SDL_SCANCODE_S].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1);
    TEST_CHECK(sink.running && sink.command_ups == 0);

    /* A held modified mode-only key enters its unmodified movement fallback. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_T;
    event.scancode = SDL_SCANCODE_T;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.running);
    key_states[SDL_SCANCODE_T].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(!sink.running && sink.command_ups == 1);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);
    event.mod = SDL_KMOD_NONE;
    event.repeat = true;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.actions_num == 2 && sink.directions[1] == 8);

    /* Orphan and non-final mode releases do not split chord composition. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.repeat = false;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent run_up = {
        .type = SDL_EVENT_KEY_UP,
        .key = SDLK_Q,
        .scancode = SDL_SCANCODE_Q,
    };
    keybind_event_reconcile_release(bindings, arraysize(bindings), &run_up, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &run_up, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8 && sink.command_ups == 0);

    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_Q;
    event.scancode = SDL_SCANCODE_Q;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_U;
    event.scancode = SDL_SCANCODE_U;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    keybind_event_reconcile_release(bindings, arraysize(bindings), &run_up, key_states, &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &run_up, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);
    TEST_CHECK(sink.running && sink.command_ups == 0);

    /* Invalidating one of two mode owners does not force a movement boundary. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_Q;
    event.scancode = SDL_SCANCODE_Q;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_V;
    event.scancode = SDL_SCANCODE_V;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_V].pressed = true;
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);
    TEST_CHECK(sink.running && sink.command_ups == 0);

    /* Focus loss schedules one stop for an external-only running stream. */
    movement_sink_reset(&sink);
    keybind_movement_state_clear(&sink.state, true, false);
    keybind_movement_state_run_released(&sink.state, true);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1);
    TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_RUN_STOP);

    /* Identical unmodified mode fallbacks transfer accepted ownership. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.type = SDL_EVENT_KEY_DOWN;
    event.repeat = false;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_X;
    event.scancode = SDL_SCANCODE_X;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_X].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.running && sink.command_ups == 0 && sink.actions_num == 0);
    TEST_CHECK(keybind_movement_state_mode_owned(&sink.state, true));

    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.key = SDLK_X;
    event.scancode = SDL_SCANCODE_X;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_X].pressed = true;
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_Y;
    event.scancode = SDL_SCANCODE_Y;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_Y].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.firing && sink.command_ups == 0 && sink.actions_num == 0);
    TEST_CHECK(keybind_movement_state_mode_owned(&sink.state, false));

    /* A same-mode compound fallback never queues a transient run stop. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    handler.reconcile_modes = NULL;
    event.mod = SDL_KMOD_LSHIFT;
    event.key = SDLK_Z;
    event.scancode = SDL_SCANCODE_Z;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    key_states[SDL_SCANCODE_Z].pressed = true;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &modifier_up,
                                    key_states,
                                    &handler);
    movement_sink_flush(&sink);
    TEST_CHECK(sink.running && sink.command_ups == 0 && sink.actions_num == 1);
    TEST_CHECK(sink.actions[0] == KEYBIND_MOVEMENT_ACTION_MOVE && sink.directions[0] == 8);

    /* Toggle ownership revokes stale momentary owners without unlatching later. */
    memset(key_states, 0, sizeof(key_states));
    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.mod = SDL_KMOD_NONE;
    event.key = SDLK_Q;
    event.scancode = SDL_SCANCODE_Q;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_1;
    event.scancode = SDL_SCANCODE_1;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent toggle_up = event;
    toggle_up.type = SDL_EVENT_KEY_UP;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &toggle_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &toggle_up, &handler));
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.running && !keybind_movement_state_mode_owned(&sink.state, true));
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent stale_run_up = {
        .type = SDL_EVENT_KEY_UP,
        .key = SDLK_Q,
        .scancode = SDL_SCANCODE_Q,
    };
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &stale_run_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &stale_run_up, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.running && sink.command_ups == 0);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8);

    movement_sink_reset(&sink);
    handler = movement_sink_handler(&sink);
    event.key = SDLK_O;
    event.scancode = SDL_SCANCODE_O;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    event.key = SDLK_2;
    event.scancode = SDL_SCANCODE_2;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    toggle_up = event;
    toggle_up.type = SDL_EVENT_KEY_UP;
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &toggle_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &toggle_up, &handler));
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    TEST_CHECK(sink.firing && !keybind_movement_state_mode_owned(&sink.state, false));
    event.key = SDLK_A;
    event.scancode = SDL_SCANCODE_A;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    SDL_KeyboardEvent stale_fire_up = {
        .type = SDL_EVENT_KEY_UP,
        .key = SDLK_O,
        .scancode = SDL_SCANCODE_O,
    };
    keybind_event_reconcile_release(bindings,
                                    arraysize(bindings),
                                    &stale_fire_up,
                                    key_states,
                                    &handler);
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &stale_fire_up, &handler));
    event.key = SDLK_B;
    event.scancode = SDL_SCANCODE_B;
    TEST_CHECK(keybind_event_process(bindings, arraysize(bindings), &event, &handler));
    movement_sink_flush(&sink);
    TEST_CHECK(sink.firing && sink.command_ups == 0);
    TEST_CHECK(sink.actions_num == 1 && sink.directions[0] == 8 && sink.firing_at_emit[0]);
}

int main(void) {
    test_legacy_keycode_migration();
    test_shortcut_names();
    test_keybind_loader();
    test_bundled_defaults();
    test_event_matching();
    test_movement_commands();
    test_movement_chords();
    test_movement_queue_replacement();
    test_movement_repeat_and_release();
    test_movement_duplicates_and_repeat_selection();
    test_movement_boundaries_and_modifiers();
    test_keybind_event_cardinal_chords();
    test_keybind_event_direction_transitions();
    test_keybind_event_integration();
    return 0;
}
