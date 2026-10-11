/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Trusted qualification helper, deliberately outside the distributed AppImage.
 */
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

static size_t discard(char *data, size_t size, size_t count, void *context) {
    (void)data;
    (void)context;
    return size * count;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: runtime-probe --assets APPDIR | --curl URL\n");
        return 2;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return 1;
    }
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (strcmp(version->version, "8.18.0") != 0 || version->ares == NULL ||
        strcmp(version->ares, "1.34.6") != 0 ||
        !(version->features & CURL_VERSION_ASYNCHDNS) ||
        version->ssl_version == NULL || strstr(version->ssl_version, "OpenSSL/3.5.5") == NULL) {
        fprintf(stderr, "wrong curl/c-ares/OpenSSL runtime feature contract\n");
        return 1;
    }
    if (strcmp(argv[1], "--curl") == 0) {
        CURL *handle = curl_easy_init();
        if (handle == NULL) {
            return 1;
        }
        curl_easy_setopt(handle, CURLOPT_URL, argv[2]);
        curl_easy_setopt(handle, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
        CURLcode status = curl_easy_perform(handle);
        if (status != CURLE_OK) {
            fprintf(stderr, "TLS request failed: %s\n", curl_easy_strerror(status));
        }
        curl_easy_cleanup(handle);
        curl_global_cleanup();
        return status == CURLE_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "--assets") != 0 || !SDL_Init(0) || !MIX_Init() || !TTF_Init()) {
        fprintf(stderr, "dependency initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    static const char *const expected[] = {
        "WAV", "STBVORBIS", "OPUS", "VOC", "AIFF", "AU", "DRMP3", "SINEWAVE", "RAW"
    };
    int count = MIX_GetNumAudioDecoders();
    if (count != (int)(sizeof(expected) / sizeof(expected[0]))) {
        fprintf(stderr, "unexpected audio decoder count: %d\n", count);
        return 1;
    }
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        unsigned int matches = 0;
        for (int j = 0; j < count; j++) {
            const char *actual = MIX_GetAudioDecoder(j);
            if (actual != NULL && strcmp(expected[i], actual) == 0) {
                matches++;
            }
        }
        if (matches != 1) {
            fprintf(stderr, "missing or duplicate decoder: %s\n", expected[i]);
            return 1;
        }
    }
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/atrinik.png", argv[2]) >= (int)sizeof(path)) {
        return 1;
    }
    SDL_Surface *image = IMG_Load(path);
    if (image == NULL) {
        fprintf(stderr, "PNG asset decode failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DestroySurface(image);
    if (snprintf(path, sizeof(path), "%s/usr/share/games/atrinik/fonts/arial.ttf", argv[2]) >=
        (int)sizeof(path)) {
        return 1;
    }
    TTF_Font *font = TTF_OpenFont(path, 16.0f);
    if (font == NULL) {
        fprintf(stderr, "font asset decode failed: %s\n", SDL_GetError());
        return 1;
    }
    TTF_CloseFont(font);
    TTF_Quit();
    MIX_Quit();
    SDL_Quit();
    curl_global_cleanup();
    puts("exact mixer decoders, PNG, font, c-ares and OpenSSL runtime features passed");
    return 0;
}
