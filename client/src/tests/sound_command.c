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

#include <stdio.h>
#include <stdlib.h>
#include <client.h>
#include <player.h>
#include <sound.h>
#include <toolkit/socket.h>
#include <toolkit/toolkit.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

Client_Player cpl;

static char played_filename[MAX_BUF];
static int played_volume;
static int played_loop;
static int8_t played_x;
static int8_t played_y;
static uint8_t played_type;

void sound_command_play_effect(const char *filename, int volume, int loop, int8_t x, int8_t y) {
    snprintf(VS(played_filename), "%s", filename);
    played_volume = volume;
    played_loop = loop;
    played_x = x;
    played_y = y;
    played_type = CMD_SOUND_EFFECT;
}

void sound_command_play_background(const char *filename, int volume, int loop) {
    snprintf(VS(played_filename), "%s", filename);
    played_volume = volume;
    played_loop = loop;
    played_type = CMD_SOUND_BACKGROUND;
}

void sound_command_play_absolute(const char *filename, int volume, int loop) {
    snprintf(VS(played_filename), "%s", filename);
    played_volume = volume;
    played_loop = loop;
    played_type = CMD_SOUND_ABSOLUTE;
}

static void test_received_effect_filename_is_opaque(void) {
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
            uint8_t packet[MAX_BUF];
            size_t filename_size = strlen(filenames[i]) + 1;
            size_t packet_size = 1 + filename_size + 4;
            TEST_CHECK(packet_size <= sizeof(packet));
            packet[0] = CMD_SOUND_EFFECT;
            memcpy(&packet[1], filenames[i], filename_size);
            packet[1 + filename_size] = (uint8_t)-1;
            packet[2 + filename_size] = (uint8_t)-17;
            packet[3 + filename_size] = (uint8_t)-3;
            packet[4 + filename_size] = 4;

            played_filename[0] = '\0';
            played_type = 0xff;
            socket_command_sound(packet, packet_size, 0);
            TEST_CHECK(played_filename[0] != '\0');
            TEST_CHECK(played_type == CMD_SOUND_EFFECT);
            TEST_CHECK(strcmp(played_filename, filenames[i]) == 0);
            TEST_CHECK(played_volume == -17);
            TEST_CHECK(played_loop == -1);
            TEST_CHECK(played_x == -3);
            TEST_CHECK(played_y == 4);
        }
    }
}

static void test_received_non_effect_dispatch(void) {
    static const uint8_t types[] = {CMD_SOUND_BACKGROUND, CMD_SOUND_ABSOLUTE};

    for (size_t i = 0; i < arraysize(types); i++) {
        static const char filename[] = "server-selected.ogg";
        uint8_t packet[64];
        size_t filename_size = sizeof(filename);
        size_t packet_size = 1 + filename_size + 2;
        packet[0] = types[i];
        memcpy(&packet[1], filename, filename_size);
        packet[1 + filename_size] = 3;
        packet[2 + filename_size] = (uint8_t)-9;

        played_filename[0] = '\0';
        played_type = 0xff;
        socket_command_sound(packet, packet_size, 0);
        TEST_CHECK(played_type == types[i]);
        TEST_CHECK(strcmp(played_filename, filename) == 0);
        TEST_CHECK(played_volume == -9);
        TEST_CHECK(played_loop == 3);
    }
}

int main(void) {
    test_received_effect_filename_is_opaque();
    test_received_non_effect_dispatch();
    return 0;
}
