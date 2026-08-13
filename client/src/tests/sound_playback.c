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

Client_Player cpl;
_mapdata MapData;

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

static char audio_path[HUGE_BUF];
static char captured[HUGE_BUF * 8];

char *file_path(const char *path, const char *mode) {
    (void)mode;
    return xstrdup(strstr(path, "missing") == NULL ? audio_path : "/no/such/audio-test.wav");
}

int64_t setting_get_int(int category, int setting) {
    if (category == OPT_CAT_MAP) {
        return 25;
    }
    if (setting == OPT_3D_SOUNDS) {
        return 1;
    }
    return 100;
}

void draw_info_format(const char *color, const char *format, ...) {
    (void)color;
    (void)format;
}

static void capture_print(const char *text) {
    size_t used = strlen(captured);
    snprintf(captured + used, sizeof(captured) - used, "%s", text);
}

static size_t occurrences(const char *needle) {
    size_t count = 0;
    const char *cursor = captured;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += strlen(needle);
    }
    return count;
}

static void write_u16(FILE *fp, uint16_t value) {
    fputc(value & 0xff, fp);
    fputc(value >> 8, fp);
}

static void write_u32(FILE *fp, uint32_t value) {
    write_u16(fp, value & 0xffff);
    write_u16(fp, value >> 16);
}

static void write_test_wav(void) {
    snprintf(VS(audio_path), "%s/audio-playback.wav", ATRINIK_TEST_BINARY_DIR);
    FILE *fp = fopen(audio_path, "wb");
    TEST_CHECK(fp != NULL);
    const uint32_t samples = 80000;
    fwrite("RIFF", 1, 4, fp);
    write_u32(fp, 36 + samples);
    fwrite("WAVEfmt ", 1, 8, fp);
    write_u32(fp, 16);
    write_u16(fp, 1);
    write_u16(fp, 1);
    write_u32(fp, 8000);
    write_u32(fp, 8000);
    write_u16(fp, 1);
    write_u16(fp, 8);
    fwrite("data", 1, 4, fp);
    write_u32(fp, samples);
    for (uint32_t i = 0; i < samples; i++) {
        fputc(128, fp);
    }
    TEST_CHECK(fclose(fp) == 0);
}

static void reset_capture(void) {
    captured[0] = '\0';
}

static void write_effect_packet(packet_struct *packet, const char *filename, int loop) {
    packet_writer_write_uint8(packet, CMD_SOUND_EFFECT);
    packet_writer_write_cstring(packet, filename);
    packet_writer_write_int8(packet, loop);
    packet_writer_write_int8(packet, 0);
    packet_writer_write_uint8(packet, 2);
    packet_writer_write_uint8(packet, -1);
}

static void test_effect_paths(void) {
    reset_capture();
    int channel = sound_play_effect_loop("impact.ogg", 75, -1);
    TEST_CHECK(channel >= 0);
    TEST_CHECK(sound_test_cache_size() == 1);
    TEST_CHECK(strstr(captured,
                      "effect started source=client requested=\"impact.ogg\" "
                      "effective=\"impact.ogg\"") != NULL);
    TEST_CHECK(strstr(captured, "volume=75 loop=-1") != NULL);

    sound_stop_effect(channel);
    reset_capture();
    channel = sound_play_effect_loop("impact.ogg", 75, 0);
    TEST_CHECK(channel >= 0);
    TEST_CHECK(sound_test_cache_size() == 1);
    TEST_CHECK(occurrences("effect started") == 1);

    sound_stop_effect(channel);
    reset_capture();
    sound_test_fail_next_playback();
    TEST_CHECK(sound_play_effect_loop("impact.ogg", 75, 0) == -1);
    TEST_CHECK(strstr(captured, "effect started") == NULL);

    reset_capture();
    TEST_CHECK(sound_play_effect_loop("missing.ogg", 75, 0) == -1);
    TEST_CHECK(strstr(captured, "effect started") == NULL);
    TEST_CHECK(strstr(captured, "/no/such/") == NULL);

    packet_struct *absolute = packet_new(0, 64, 16);
    packet_writer_write_uint8(absolute, CMD_SOUND_ABSOLUTE);
    packet_writer_write_cstring(absolute, "/no/such/private\nforged.wav");
    packet_writer_write_int8(absolute, 0);
    packet_writer_write_int8(absolute, 75);
    reset_capture();
    socket_command_sound(absolute->data, absolute->len, 0);
    TEST_CHECK(strstr(captured, "effect started") == NULL);
    TEST_CHECK(strstr(captured, "/no/such/") == NULL);
    TEST_CHECK(strstr(captured, "private\\nforged.wav") != NULL);
    packet_free(absolute);
}

static void test_server_and_ambient_sources(void) {
    static const char *const hurt_effects[] = {
        "doh.ogg",
        "doh_female_1.ogg",
        "doh_female_4.ogg",
        "doh_female_7.ogg",
    };
    for (size_t i = 0; i < arraysize(hurt_effects); i++) {
        packet_struct *packet = packet_new(0, 64, 16);
        write_effect_packet(packet, hurt_effects[i], 0);
        reset_capture();
        socket_command_sound(packet->data, packet->len, 0);
        TEST_CHECK(strstr(captured, "source=server-positional") != NULL);
        char expected[MAX_BUF];
        snprintf(VS(expected),
                 "requested=\"%s\" effective=\"%s\"",
                 hurt_effects[i],
                 hurt_effects[i]);
        TEST_CHECK(strstr(captured, expected) != NULL);
        TEST_CHECK(strstr(captured, "angle=") != NULL);
        packet_free(packet);
        sound_stop_effect(0);
    }

    packet_struct *absolute = packet_new(0, 64, 16);
    packet_writer_write_uint8(absolute, CMD_SOUND_ABSOLUTE);
    packet_writer_write_cstring(absolute, audio_path);
    packet_writer_write_int8(absolute, 2);
    packet_writer_write_int8(absolute, 55);
    reset_capture();
    socket_command_sound(absolute->data, absolute->len, 0);
    TEST_CHECK(strstr(captured, "effect started source=server-absolute") != NULL);
    TEST_CHECK(strstr(captured, "volume=55 loop=2") != NULL);
    packet_free(absolute);

    packet_struct *ambient = packet_new(0, 64, 16);
    packet_writer_write_uint8(ambient, 12);
    packet_writer_write_uint8(ambient, 12);
    packet_writer_write_uint32(ambient, 0);
    packet_writer_write_uint32(ambient, 42);
    packet_writer_write_cstring(ambient, "wind.ogg");
    packet_writer_write_uint8(ambient, 60);
    packet_writer_write_uint8(ambient, 8);
    reset_capture();
    socket_command_sound_ambient(ambient->data, ambient->len, 0);
    TEST_CHECK(strstr(captured, "effect started source=ambient") != NULL);
    TEST_CHECK(strstr(captured, "loop=-1") != NULL);
    TEST_CHECK(strstr(captured, "distance=") != NULL);
    packet_free(ambient);

    size_t starts = occurrences("effect started");
    sound_ambient_mapcroll(1, 0);
    TEST_CHECK(occurrences("effect started") == starts);
    sound_ambient_clear();
}

static void test_channel_exhaustion(void) {
    sound_deinit();
    sound_init();
    reset_capture();
    for (int i = 0; i < 32; i++) {
        TEST_CHECK(sound_play_effect_loop("full.ogg", 100, 0) == i);
    }
    TEST_CHECK(occurrences("effect started") == 32);
    TEST_CHECK(sound_play_effect_loop("exhausted.ogg", 100, 0) == -1);
    TEST_CHECK(occurrences("effect started") == 32);
}

static void test_music_lifecycle(void) {
    TEST_CHECK(strcmp(sound_test_duration_key("relative/area/fireside.mid"), "fireside.mid") == 0);
    TEST_CHECK(strcmp(sound_test_duration_key("relative\\area\\fireside.mid"), "fireside.mid") ==
               0);
    TEST_CHECK(strcmp(sound_test_duration_key("../private/fireside.mid"), "fireside.mid") == 0);
    TEST_CHECK(sound_test_duration_key("../..") == NULL);
    TEST_CHECK(sound_test_duration_key("private\nforged.mid") == NULL);

    sound_deinit();
    sound_init();
    reset_capture();
    sound_start_bg_music("intro.ogg", 80, -1);
    TEST_CHECK(occurrences("music started") == 1);
    TEST_CHECK(strstr(captured, "source=intro-player") != NULL);
    TEST_CHECK(strstr(captured, "volume=80 loop=-1") != NULL);

    sound_start_bg_music("intro.ogg", 70, -1);
    TEST_CHECK(occurrences("music started") == 1);
    TEST_CHECK(strstr(captured, "music stopped") == NULL);

    sound_start_bg_music("relative/area/finite-loop.ogg", 70, 1);
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 1);
    sound_test_finish_music();
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 1);
    sound_test_finish_music();
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 2);
    TEST_CHECK(strstr(captured, "reason=finished") != NULL);
    TEST_CHECK(strstr(captured,
                      "music stopped source=intro-player "
                      "effective=\"relative/area/finite-loop.ogg\" reason=finished") != NULL);

    reset_capture();
    sound_start_bg_music("restart-failure.ogg", 70, -1);
    sound_test_fail_next_playback();
    sound_test_finish_music();
    TEST_CHECK(occurrences("music started") == 1);
    TEST_CHECK(occurrences("music stopped") == 1);
    TEST_CHECK(strstr(captured,
                      "music stopped source=intro-player "
                      "effective=\"restart-failure.ogg\" reason=restart-failed") != NULL);
    TEST_CHECK(!sound_playing_music());

    sound_start_bg_music("replacement.ogg", 60, 0);
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 1);

    sound_test_fail_next_playback();
    sound_start_bg_music("failure.ogg", 50, 0);
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 2);
    TEST_CHECK(!sound_playing_music());

    sound_start_bg_music("missing.ogg", 50, 0);
    TEST_CHECK(occurrences("music started") == 2);
    TEST_CHECK(occurrences("music stopped") == 2);
    TEST_CHECK(!sound_playing_music());

    reset_capture();
    update_map_bg_music("map-track.ogg -1 -20");
    TEST_CHECK(strstr(captured, "music started source=map") != NULL);
    TEST_CHECK(strstr(captured, "requested=\"map-track.ogg\"") != NULL);
    TEST_CHECK(strstr(captured, "volume=80 loop=-1") != NULL);

    reset_capture();
    sound_stop_bg_music();
    TEST_CHECK(occurrences("music stopped") == 1);
    TEST_CHECK(strstr(captured, "source=map") != NULL);
    TEST_CHECK(strstr(captured, "reason=stopped") != NULL);

    reset_capture();
    sound_start_bg_music("relative/area/fireside.mid", 80, 0);
    sound_start_bg_music("next.ogg", 80, 0);
    TEST_CHECK(strstr(captured,
                      "music started source=intro-player "
                      "requested=\"relative/area/fireside.mid\" "
                      "effective=\"relative/area/fireside.mid\"") != NULL);
    TEST_CHECK(strstr(captured,
                      "music stopped source=intro-player "
                      "effective=\"relative/area/fireside.mid\" reason=replaced") != NULL);

    reset_capture();
    sound_test_fail_next_stop();
    sound_start_bg_music("rejected-replacement.ogg", 80, 0);
    TEST_CHECK(strstr(captured, "music stopped") == NULL);
    TEST_CHECK(strstr(captured, "music started") == NULL);
    TEST_CHECK(sound_playing_music());
    sound_stop_bg_music();
    TEST_CHECK(occurrences("music stopped") == 1);

    reset_capture();
    sound_start_bg_music("cache-retained.ogg", 80, 0);
    size_t cache_size = sound_test_cache_size();
    sound_test_fail_next_stop();
    TEST_CHECK(!sound_clear_cache());
    TEST_CHECK(sound_playing_music());
    TEST_CHECK(sound_test_cache_size() == cache_size);
    TEST_CHECK(strstr(captured, "music stopped") == NULL);
    TEST_CHECK(sound_clear_cache());
    TEST_CHECK(!sound_playing_music());
    TEST_CHECK(sound_test_cache_size() == 0);
    TEST_CHECK(strstr(captured, "reason=cache-cleared") != NULL);

    reset_capture();
    sound_start_bg_music("shutdown-forced.ogg", 80, 0);
    sound_test_fail_next_stop();
    sound_deinit();
    TEST_CHECK(strstr(captured,
                      "music stopped source=intro-player "
                      "effective=\"shutdown-forced.ogg\" reason=shutdown") != NULL);

    reset_capture();
    sound_play_effect("disabled.ogg", 100);
    sound_start_bg_music("disabled.ogg", 100, 0);
    TEST_CHECK(strstr(captured, "started") == NULL);
}

int main(void) {
    TEST_CHECK(SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1) == 0);
    TEST_CHECK(SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS));
    toolkit_import(logger);
    toolkit_import(math);
    toolkit_import(packet);
    logger_set_filter_stdout("audio");
    logger_set_print_func(capture_print);
    write_test_wav();

    sound_init();
    test_effect_paths();
    test_server_and_ambient_sources();
    test_channel_exhaustion();
    test_music_lifecycle();

    toolkit_deinit();
    SDL_Quit();
    return 0;
}
