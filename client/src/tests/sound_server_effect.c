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

#include <stdio.h>
#include <stdlib.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

Client_Player cpl;

static const char *played_filename;
static int played_volume;
static int played_loop;

int sound_play_effect_loop(const char *filename, int volume, int loop) {
    played_filename = filename;
    played_volume = volume;
    played_loop = loop;
    return 17;
}

static void test_server_effect_filename_is_opaque(void) {
    static const char *const filenames[] = {
        "doh.ogg",
        "doh_female_1.ogg",
        "doh_female_2.ogg",
        "doh_female_3.ogg",
        "doh_female_4.ogg",
        "doh_female_5.ogg",
        "doh_female_6.ogg",
        "doh_female_7.ogg",
        "player_hurt.ogg",
    };

    for (int gender = GENDER_NEUTER; gender < GENDER_MAX; gender++) {
        cpl.gender = gender;
        for (size_t i = 0; i < arraysize(filenames); i++) {
            played_filename = NULL;
            TEST_CHECK(sound_play_server_effect(filenames[i], 83, -1) == 17);
            TEST_CHECK(played_filename == filenames[i]);
            TEST_CHECK(strcmp(played_filename, filenames[i]) == 0);
            TEST_CHECK(played_volume == 83);
            TEST_CHECK(played_loop == -1);
        }
    }
}

int main(void) {
    test_server_effect_filename_is_opaque();
    return 0;
}
