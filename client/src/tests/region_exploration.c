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
#include <toolkit/path.h>
#include <openssl/evp.h>
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
static const char certificate[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const uint8_t reset[] = {0, 'a', 0};
static size_t packet(uint8_t *out, const char *path, unsigned w, unsigned h) {
    out[0] = 1;
    size_t pos = strlen(path) + 2;
    memcpy(out + 1, path, pos - 1);
    out[pos++] = (uint8_t)(w >> 8);
    out[pos++] = (uint8_t)w;
    out[pos++] = (uint8_t)(h >> 8);
    out[pos++] = (uint8_t)h;
    size_t bytes = ((size_t)w * h + 7) / 8;
    memset(out + pos, 0, bytes);
    return pos + bytes;
}
static bool receive(const uint8_t *data, size_t len) {
    return region_exploration_receive(data, len, NULL);
}
static size_t sent;
static bool blocked;
static bool send_request(const uint8_t *data, size_t len, void *user) {
    (void)user;
    if (blocked) {
        return false;
    }
    CHECK(data[0] == 0);
    size_t pos = strlen((const char *)data + 1) + 2;
    unsigned bytes = ((unsigned)data[pos] << 8) | data[pos + 1];
    CHECK(len == pos + 2 + bytes);
    if (strcmp((const char *)data + 1, "/test") == 0) {
        CHECK(bytes == 3 && data[pos + 2] == 0x85 && data[pos + 3] == 2 && data[pos + 4] == 1);
    }
    sent++;
    return true;
}
static void select_account(const char *directory) {
    region_exploration_connect(certificate, directory);
    CHECK(receive(reset, sizeof(reset)));
}
static char *cache_file(const char *directory) {
    uint8_t identity[67], digest[EVP_MAX_MD_SIZE];
    unsigned size;
    memcpy(identity, certificate, 65);
    identity[65] = 'a';
    identity[66] = 0;
    CHECK(EVP_Digest(identity, sizeof(identity), digest, &size, EVP_sha256(), NULL) == 1 &&
          size == 32);
    char name[81] = "exploration-";
    for (unsigned i = 0; i < 32; i++) {
        snprintf(name + 12 + i * 2, 3, "%02x", digest[i]);
    }
    strcat(name, ".bin");
    return path_join(directory, name);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(path_ensure_real_directory(argv[1], 0700) == PATH_DIRECTORY_OK);
    char *filename = cache_file(argv[1]);
    remove(filename);
    uint8_t data[9000];
    unsigned w, h;
    size_t len = packet(data, "/test", 9, 2);
    CHECK(!receive(data, len)); /* No account before authenticated RESET. */
    select_account(NULL);
    data[len - 3] = 0x81;
    data[len - 2] = 2;
    data[len - 1] = 1;
    bool changed;
    CHECK(region_exploration_receive(data, len, &changed) && changed);
    CHECK(region_exploration_receive(data, len, &changed) && !changed);
    const uint8_t *bits = region_exploration_find("/test", &w, &h);
    CHECK(bits && w == 9 && h == 2 && bits[0] == 0x81 && bits[1] == 2 && bits[2] == 1);
    for (size_t n = 0; n < len; n++) {
        CHECK(!receive(data, n));
        CHECK(region_exploration_find("/test", &w, &h) == bits && bits[0] == 0x81);
    }
    data[len] = 0;
    CHECK(!receive(data, len + 1));
    const uint8_t patch[] = {2, '/', 't', 'e', 's', 't', 0, 0, 9, 0, 2, 0, 1, 0, 0, 4};
    for (size_t n = 0; n < sizeof(patch); n++) {
        CHECK(!receive(patch, n));
        CHECK(bits[0] == 0x81);
    }
    CHECK(region_exploration_receive(patch, sizeof(patch), &changed) && changed && bits[0] == 0x85);
    CHECK(region_exploration_receive(patch, sizeof(patch), &changed) && !changed);
    uint8_t invalid_patch[] = {2, '/', 't', 'e', 's', 't', 0, 0, 9, 0, 2, 0, 2, 0, 0, 2, 0, 3, 1};
    CHECK(!receive(invalid_patch, sizeof(invalid_patch)) &&
          bits[0] == 0x85); /* Atomic bad final index. */
    invalid_patch[17] = 0;
    CHECK(!receive(invalid_patch, sizeof(invalid_patch))); /* Duplicate. */
    invalid_patch[17] = 2;
    invalid_patch[18] = 4;
    CHECK(!receive(invalid_patch, sizeof(invalid_patch))); /* Tail padding. */
    const uint8_t empty[] = {3, '/', 't', 'e', 's', 't', 0};
    CHECK(region_exploration_receive(empty, sizeof(empty), &changed) && !changed &&
          bits[0] == 0x85);
    CHECK(region_exploration_request("/test"));
    CHECK(region_exploration_request("/test"));
    blocked = true;
    CHECK(region_exploration_service(send_request, NULL) == 0);
    blocked = false;
    CHECK(region_exploration_service(send_request, NULL) == 1 && sent == 1);
    CHECK(region_exploration_request("/test"));
    CHECK(region_exploration_service(send_request, NULL) == 0);
    region_exploration_disconnect();
    CHECK(!region_exploration_find("/test", &w, &h));
    CHECK(region_exploration_service(send_request, NULL) == 0);
    select_account(NULL);
    CHECK(region_exploration_find("/test", &w, &h) == bits && bits[0] == 0x85);
    len = packet(data, "/test", 1, 1);
    data[len - 1] = 1;
    CHECK(receive(data, len));
    CHECK(region_exploration_find("/test", &w, &h) == bits && w == 9 && h == 2 && bits[0] == 0x85);
    const uint8_t account_b[] = {0, 'b', 0};
    CHECK(receive(account_b, sizeof(account_b)));
    CHECK(!region_exploration_find("/test", &w, &h));
    CHECK(!receive((const uint8_t *)"\0a\0x", 4));
    len = packet(data, "/test", 0, 1);
    CHECK(!receive(data, len));
    len = packet(data, "/test", 257, 1);
    CHECK(!receive(data, len));
    char path[258];
    memset(path, 'a', sizeof(path));
    path[0] = '/';
    path[256] = 0;
    len = packet(data, path, 1, 1);
    CHECK(!receive(data, len));
    path[255] = 0;
    len = packet(data, path, 256, 256);
    data[len - 1] = 0x80;
    CHECK(receive(data, len));
    bits = region_exploration_find(path, &w, &h);
    CHECK(bits && w == 256 && h == 256 && bits[8191] == 0x80);
    const char *invalid[] = {"/", "/a//b", "/a/../b", "/a/./b", "/a\\b", "/a:b", "/a\nb"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        len = packet(data, invalid[i], 1, 1);
        CHECK(!receive(data, len));
    }
    len = packet(data, "/padding", 1, 1);
    data[len - 1] = 2;
    CHECK(!receive(data, len));
    region_exploration_clear();
    select_account(argv[1]);
    sent = 0;
    for (unsigned i = 0; i < 10000; i++) {
        snprintf(path, sizeof(path), "/map/%u", i);
        len = packet(data, path, 1, 1);
        data[len - 1] = 1;
        CHECK(receive(data, len));
        CHECK(region_exploration_request(path));
    }
    CHECK(!region_exploration_request("/overflow"));
    len = packet(data, "/overflow", 1, 1);
    CHECK(!receive(data, len));
    CHECK(region_exploration_service(send_request, NULL) == 32);
    while (region_exploration_service(send_request, NULL)) {}
    CHECK(sent == 10000);
    region_exploration_clear(); /* Flush persistent account cache and drop memory. */
    select_account(argv[1]);
    for (unsigned i = 0; i < 10000; i++) {
        snprintf(path, sizeof(path), "/map/%u", i);
        bits = region_exploration_find(path, &w, &h);
        CHECK(bits && w == 1 && h == 1 && bits[0] == 1);
    }
    region_exploration_connect("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                               argv[1]);
    CHECK(receive(reset, sizeof(reset)) && !region_exploration_find("/map/0", &w, &h));
    region_exploration_clear();
    FILE *file = fopen(filename, "ab");
    CHECK(file != NULL);
    CHECK(fwrite("x", 1, 1, file) == 1);
    CHECK(fclose(file) == 0);
    select_account(argv[1]);
    CHECK(!region_exploration_find("/map/0", &w, &h)); /* Corrupt suffix rolls back whole load. */
    region_exploration_clear();
    CHECK(remove(filename) == 0);
    free(filename);
    return 0;
}
