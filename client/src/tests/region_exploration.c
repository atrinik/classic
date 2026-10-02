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
#include <region_exploration.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                            \
    do {                                                    \
        if (!(c)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #c); \
            abort();                                        \
        }                                                   \
    } while (0)

static size_t packet(uint8_t *out, const char *path, unsigned w, unsigned h) {
    out[0] = 1;
    size_t pos = strlen(path) + 2;
    memcpy(out + 1, path, pos - 1);
    out[pos++] = w >> 8;
    out[pos++] = w;
    out[pos++] = h >> 8;
    out[pos++] = h;
    size_t bytes = ((size_t)w * h + 7) / 8;
    memset(out + pos, 0, bytes);
    return pos + bytes;
}
int main(void) {
    uint8_t data[9000];
    unsigned w, h;
    size_t len = packet(data, "/test", 9, 2);
    data[len - 3] = 0x81;
    data[len - 2] = 0x02;
    data[len - 1] = 0x01;
    CHECK(region_exploration_receive(data, len));
    const uint8_t *bits = region_exploration_find("/test", &w, &h);
    CHECK(bits && w == 9 && h == 2 && bits[0] == 0x81 && bits[1] == 2 && bits[2] == 1);
    for (size_t n = 0; n < len; n++) {
        CHECK(!region_exploration_receive(data, n));
        CHECK(region_exploration_find("/test", &w, &h) == bits);
    }
    data[len] = 0;
    CHECK(!region_exploration_receive(data, len + 1));
    data[0] = 2;
    CHECK(!region_exploration_receive(data, len));
    data[0] = 0;
    CHECK(!region_exploration_receive(data, 2));
    CHECK(region_exploration_find("/test", &w, &h) == bits);
    CHECK(region_exploration_receive(data, 1));
    CHECK(!region_exploration_find("/test", &w, &h));
    len = packet(data, "/test", 0, 1);
    CHECK(!region_exploration_receive(data, len));
    len = packet(data, "/test", 257, 1);
    CHECK(!region_exploration_receive(data, len));
    char path[258];
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[256] = 0;
    len = packet(data, path, 1, 1);
    CHECK(!region_exploration_receive(data, len));
    path[255] = 0;
    len = packet(data, path, 256, 256);
    data[len - 1] = 0x80;
    CHECK(region_exploration_receive(data, len));
    bits = region_exploration_find(path, &w, &h);
    CHECK(bits && w == 256 && h == 256 && bits[8191] == 0x80);
    region_exploration_clear();
    const char *invalid[] = {"/", "/a//b", "/a/../b", "/a/./b", "/a\\b", "/a:b", "/a\nb"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        len = packet(data, invalid[i], 1, 1);
        CHECK(!region_exploration_receive(data, len));
    }
    len = packet(data, "/padding", 1, 1);
    data[len - 1] = 2;
    CHECK(!region_exploration_receive(data, len));
    for (unsigned i = 0; i < 4096; i++) {
        snprintf(path, sizeof(path), "/map/%u", i);
        len = packet(data, path, 1, 1);
        CHECK(region_exploration_receive(data, len));
    }
    len = packet(data, "/overflow", 1, 1);
    CHECK(!region_exploration_receive(data, len));
    len = packet(data, "/map/0", 2, 2);
    data[len - 1] = 8;
    CHECK(region_exploration_receive(data, len));
    bits = region_exploration_find("/map/0", &w, &h);
    CHECK(bits && w == 2 && h == 2 && *bits == 8);
    region_exploration_clear();
    CHECK(!region_exploration_find("/map/0", &w, &h));
    CHECK(region_exploration_receive(data, len));
    region_exploration_clear();
    return 0;
}
