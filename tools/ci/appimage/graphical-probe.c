/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Trusted software qualification helper, never distributed in the AppImage.
 * Production client hardware policy and gameplay are outside this proof.
 */
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

static bool asset_path(char *path, size_t size, const char *appdir, const char *asset) {
    int length = snprintf(path, size, "%s/%s", appdir, asset);
    return length >= 0 && (size_t)length < size;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: graphical-probe APPDIR\n");
        return 2;
    }
    int status = 1;
    SDL_Window *window = NULL;
    SDL_GPUDevice *gpu = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Surface *image = NULL, *text = NULL, *frame = NULL;
    SDL_Texture *image_texture = NULL, *text_texture = NULL;
    TTF_Font *font = NULL;
    MIX_Mixer *mixer = NULL;
    MIX_Audio *audio = NULL;
    MIX_Track *track = NULL;
    char path[4096];
    SDL_PropertiesID properties = 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) || !TTF_Init() || !MIX_Init()) {
        goto finish;
    }
    window = SDL_CreateWindow("Atrinik AppImage dependency qualification", 800, 600, 0);
    properties = SDL_CreateProperties();
    if (window == NULL || properties == 0 ||
        !SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan") ||
        !SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true) ||
        !SDL_SetBooleanProperty(properties,
                               SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
                               false)) {
        goto finish;
    }
    gpu = SDL_CreateGPUDeviceWithProperties(properties);
    if (gpu == NULL || strcmp(SDL_GetGPUDeviceDriver(gpu), "vulkan") != 0) {
        goto finish;
    }
    const char *device = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(gpu),
                                              SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown");
    if (strstr(device, "llvmpipe") == NULL && strstr(device, "lavapipe") == NULL) {
        SDL_SetError("qualification must use the pinned host's software Vulkan device");
        goto finish;
    }
    renderer = SDL_CreateGPURenderer(gpu, window);
    if (renderer == NULL ||
        !asset_path(path, sizeof(path), argv[1], "atrinik.png") ||
        (image = IMG_Load(path)) == NULL ||
        !asset_path(path, sizeof(path), argv[1], "usr/share/games/atrinik/fonts/arial.ttf") ||
        (font = TTF_OpenFont(path, 32.0f)) == NULL ||
        (text = TTF_RenderText_Blended(font, "Atrinik bundled PNG and font", 0,
                                      (SDL_Color){255, 230, 120, 255})) == NULL ||
        (image_texture = SDL_CreateTextureFromSurface(renderer, image)) == NULL ||
        (text_texture = SDL_CreateTextureFromSurface(renderer, text)) == NULL) {
        goto finish;
    }
    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (mixer == NULL ||
        !asset_path(path, sizeof(path), argv[1],
                    "usr/share/games/atrinik/sound/background/intro.ogg") ||
        (audio = MIX_LoadAudio(mixer, path, true)) == NULL ||
        (track = MIX_CreateTrack(mixer)) == NULL || !MIX_SetTrackAudio(track, audio) ||
        !MIX_PlayTrack(track, 0) ||
        strcmp(SDL_GetCurrentAudioDriver(), "pulseaudio") != 0) {
        goto finish;
    }
    SDL_FRect image_area = {32, 32, 256, 256};
    SDL_FRect text_area = {32, 340, (float)text->w, (float)text->h};
    Uint64 started = SDL_GetTicks();
    bool qualified = false;
    bool announced = false;
    while (SDL_GetTicks() - started < 5000) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                SDL_SetError("qualification window closed prematurely");
                goto finish;
            }
        }
        if (!SDL_SetRenderDrawColor(renderer, 15, 30, 60, 255) || !SDL_RenderClear(renderer) ||
            !SDL_RenderTexture(renderer, image_texture, NULL, &image_area) ||
            !SDL_RenderTexture(renderer, text_texture, NULL, &text_area)) {
            goto finish;
        }
        if (!qualified) {
            frame = SDL_RenderReadPixels(renderer, NULL);
            if (frame == NULL) {
                goto finish;
            }
            /* Read the actual GPU-rendered asset area, not the source surface. */
            bool colors[256] = {false};
            unsigned int distinct = 0;
            for (int y = 32; y < 288; y += 4) {
                for (int x = 32; x < 288; x += 4) {
                    Uint8 red, green, blue, alpha;
                    if (!SDL_ReadSurfacePixel(frame, x, y, &red, &green, &blue, &alpha)) {
                        goto finish;
                    }
                    unsigned int color = (red & 0xe0U) | ((green & 0xe0U) >> 3) | (blue >> 6);
                    if (!colors[color]) {
                        colors[color] = true;
                        distinct++;
                    }
                }
            }
            if (distinct < 16) {
                SDL_SetError("GPU readback did not contain the decoded PNG's color variation");
                goto finish;
            }
            unsigned int glyph_pixels = 0;
            for (int y = 340; y < 340 + text->h && y < frame->h; y += 2) {
                for (int x = 32; x < 32 + text->w && x < frame->w; x += 2) {
                    Uint8 red, green, blue, alpha;
                    if (!SDL_ReadSurfacePixel(frame, x, y, &red, &green, &blue, &alpha)) {
                        goto finish;
                    }
                    if (red > 100 && green > 100) {
                        glyph_pixels++;
                    }
                }
            }
            if (glyph_pixels < 20) {
                SDL_SetError("GPU readback did not contain the rendered font glyphs");
                goto finish;
            }
            qualified = true;
        }
        if (!SDL_RenderPresent(renderer)) {
            goto finish;
        }
        if (!announced) {
            printf("graphical-ready backend=vulkan device=%s audio=pulseaudio frame=800x600\n", device);
            fflush(stdout);
            announced = true;
            /* Keep the observable window alive for five seconds even when
             * the first software shader compilation was slow. */
            started = SDL_GetTicks();
        }
        SDL_Delay(40);
    }
    status = 0;
finish:
    if (status != 0) {
        fprintf(stderr, "graphical qualification failed: %s\n", SDL_GetError());
    }
    MIX_DestroyTrack(track);
    MIX_DestroyAudio(audio);
    MIX_DestroyMixer(mixer);
    SDL_DestroySurface(frame);
    SDL_DestroyTexture(text_texture);
    SDL_DestroyTexture(image_texture);
    SDL_DestroySurface(text);
    SDL_DestroySurface(image);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyProperties(properties);
    SDL_DestroyWindow(window);
    MIX_Quit();
    TTF_Quit();
    SDL_Quit();
    if (status == 0) {
        puts("graphical-complete: software Vulkan presentation, GPU readback, bundled PNG/font/OGG, virtual audio");
    }
    return status;
}
