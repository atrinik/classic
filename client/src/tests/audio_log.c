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
#include <audio_log.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static char captured[HUGE_BUF];

static void capture_print(const char *text) {
    snprintf(VS(captured), "%s", text);
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_CHECK(fp != NULL);
    TEST_CHECK(fseek(fp, 0, SEEK_END) == 0);
    long length = ftell(fp);
    TEST_CHECK(length >= 0);
    TEST_CHECK(fseek(fp, 0, SEEK_SET) == 0);
    char *contents = calloc((size_t)length + 1, 1);
    TEST_CHECK(contents != NULL);
    TEST_CHECK(fread(contents, 1, (size_t)length, fp) == (size_t)length);
    TEST_CHECK(fclose(fp) == 0);
    return contents;
}

int main(void) {
    toolkit_import(logger);
    logger_set_print_func(capture_print);
    TEST_CHECK(logger_get_level("audio") == LOG_AUDIO);

    char escaped[MAX_BUF];
    audio_log_asset_escape("relative/area/fireside.mid", VS(escaped));
    TEST_CHECK(strcmp(escaped, "relative/area/fireside.mid") == 0);
    audio_log_asset_escape("/private/media/fireside.mid", VS(escaped));
    TEST_CHECK(strcmp(escaped, "fireside.mid") == 0);
    audio_log_asset_escape("C:\\private\\media\\fireside.mid", VS(escaped));
    TEST_CHECK(strcmp(escaped, "fireside.mid") == 0);
    audio_log_asset_escape("\\\\host\\share\\fireside.mid", VS(escaped));
    TEST_CHECK(strcmp(escaped, "fireside.mid") == 0);
    audio_log_asset_escape("ignored", escaped, 0);

    char path[HUGE_BUF];
    snprintf(VS(path), "%s/audio-observability.log", ATRINIK_TEST_BINARY_DIR);
    logger_open_log(path);

    audio_log_effect_started("server-positional",
                             "/private/request/doh_female_4.ogg\nforged",
                             "C:\\private\\effective\\doh_female_4.ogg\"quoted",
                             7,
                             80,
                             0,
                             true,
                             45,
                             72);
    TEST_CHECK(captured[0] == '\0');

    audio_log_music_started("map", "fireside.mid", "fireside.mid", 80, -1);
    audio_log_music_stopped("map", "fireside.mid", "replaced");

    char *contents = read_file(path);
    TEST_CHECK(strstr(contents, "AUDIO") != NULL);
    TEST_CHECK(strstr(contents, "effect started source=server-positional") != NULL);
    TEST_CHECK(strstr(contents, "requested=\"doh_female_4.ogg\\nforged\"") != NULL);
    TEST_CHECK(strstr(contents, "effective=\"doh_female_4.ogg\\\"quoted\"") != NULL);
    TEST_CHECK(strstr(contents, "/private/") == NULL);
    TEST_CHECK(strstr(contents, "C:\\private") == NULL);
    TEST_CHECK(strstr(contents, "channel=7 volume=80 loop=0 angle=45 distance=72") != NULL);
    TEST_CHECK(strstr(contents, "music started source=map") != NULL);
    TEST_CHECK(strstr(contents, "music stopped source=map") != NULL);
    TEST_CHECK(strstr(contents, "reason=replaced") != NULL);
    free(contents);

    logger_set_filter_stdout("audio");
    audio_log_effect_started("ambient", "wind.ogg", "wind.ogg", 3, 50, -1, false, 0, 0);
    TEST_CHECK(strstr(captured, "AUDIO") != NULL);
    TEST_CHECK(strstr(captured, "source=ambient") != NULL);

    toolkit_deinit();
    return 0;
}
