/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mixer-probe VERIFIED_OPUS_FIXTURE\n");
        return 2;
    }
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS) || !MIX_Init()) {
        fprintf(stderr, "mixer initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    int result = 1;
    MIX_AudioDecoder *decoder = NULL;
    MIX_Mixer *mixer = NULL;
    MIX_Audio *audio = NULL;
    static const char *const expected[] =
        {"WAV", "STBVORBIS", "OPUS", "VOC", "AIFF", "AU", "DRMP3", "SINEWAVE", "RAW"};
    int count = MIX_GetNumAudioDecoders();
    printf("SDL3_mixer decoders:");
    for (int i = 0; i < count; i++) {
        const char *name = MIX_GetAudioDecoder(i);
        printf(" %s", name != NULL ? name : "(null)");
    }
    puts("");
    if (count != (int)(sizeof(expected) / sizeof(expected[0]))) {
        fprintf(stderr, "unsupported decoder count: %d\n", count);
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        unsigned int matches = 0;
        for (int j = 0; j < count; j++) {
            const char *actual = MIX_GetAudioDecoder(j);
            matches += actual != NULL && strcmp(actual, expected[i]) == 0;
        }
        if (matches != 1) {
            fprintf(stderr, "missing or duplicate decoder: %s\n", expected[i]);
            goto cleanup;
        }
    }
    decoder = MIX_CreateAudioDecoder(argv[1], 0);
    if (decoder == NULL) {
        fprintf(stderr, "Opus fixture decoder failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 48000};
    unsigned char pcm[4096];
    size_t total = 0;
    bool nonzero = false;
    for (;;) {
        int decoded = MIX_DecodeAudio(decoder, pcm, sizeof(pcm), &spec);
        if (decoded < 0 || (size_t)decoded > 23040 - total) {
            fprintf(stderr, "invalid Opus fixture PCM: %s\n", SDL_GetError());
            goto cleanup;
        }
        if (decoded == 0) {
            break;
        }
        total += (size_t)decoded;
        for (int i = 0; i < decoded; i++) {
            nonzero = nonzero || pcm[i] != 0;
        }
    }
    if (total != 23040 || !nonzero) {
        fprintf(stderr, "expected 23040 nonzero Opus PCM bytes, got %zu\n", total);
        goto cleanup;
    }
    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (mixer == NULL || (audio = MIX_LoadAudio(mixer, argv[1], true)) == NULL) {
        fprintf(stderr, "dummy device/fixture load failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    MIX_Track *track = MIX_CreateTrack(mixer);
    if (track == NULL || !MIX_SetTrackAudio(track, audio) || !MIX_PlayTrack(track, 0)) {
        fprintf(stderr, "dummy playback failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    result = 0;
    puts("exact decoder contract, Opus PCM and dummy playback passed");
cleanup:
    MIX_DestroyMixer(mixer);
    MIX_DestroyAudio(audio);
    MIX_DestroyAudioDecoder(decoder);
    MIX_Quit();
    SDL_Quit();
    return result;
}
