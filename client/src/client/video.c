/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Atrinik Development Team                    *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * SDL3 window and mandatory GPU presentation.
 */

#include <global.h>
#include <video.h>
#include <window_title.h>

void video_init(void) {
    list_vid_modes();

    if (!video_set_size()) {
        LOG(ERROR, "Couldn't create the game window: %s", SDL_GetError());
        exit(1);
    }
}

void video_set_icon(SDL_Surface *icon) {
    HARD_ASSERT(icon != NULL);

    if (!SDL_SetWindowIcon(ScreenWindow, icon)) {
        /* Some compositors intentionally do not implement window icons. */
        LOG(DEBUG, "Could not set the window icon: %s", SDL_GetError());
    }

    SDL_DestroySurface(icon);
}

int video_get_bpp(void) {
    return 32;
}

static void video_get_output_size(int *width, int *height) {
    HARD_ASSERT(width != NULL);
    HARD_ASSERT(height != NULL);
    if (ScreenSurface != NULL) {
        *width = ScreenSurface->w;
        *height = ScreenSurface->h;
        return;
    }
    if (!gpu_renderer_output_size(width, height)) {
        *width = setting_get_int(OPT_CAT_CLIENT, OPT_RESOLUTION_X);
        *height = setting_get_int(OPT_CAT_CLIENT, OPT_RESOLUTION_Y);
    }
}

int video_get_width(void) {
    int width, height;
    video_get_output_size(&width, &height);
    return width;
}

int video_get_height(void) {
    int width, height;
    video_get_output_size(&width, &height);
    return height;
}

int video_set_size(void) {
    int width = setting_get_int(OPT_CAT_CLIENT, OPT_RESOLUTION_X);
    int height = setting_get_int(OPT_CAT_CLIENT, OPT_RESOLUTION_Y);

    if (ScreenWindow == NULL) {
        ScreenWindow = client_window_create(width, height, get_video_flags());
        if (ScreenWindow == NULL) {
            return 0;
        }
    } else {
        if (!SDL_SetWindowSize(ScreenWindow, width, height) ||
            !SDL_SetWindowFullscreen(ScreenWindow,
                                     setting_get_int(OPT_CAT_CLIENT, OPT_FULLSCREEN) != 0)) {
            return 0;
        }
    }

    if (!gpu_renderer_ready() && !gpu_renderer_create(ScreenWindow)) {
        LOG(ERROR,
            "No supported hardware GPU renderer is available: %s",
            SDL_GetError());
        SDL_DestroyWindow(ScreenWindow);
        ScreenWindow = NULL;
        return 0;
    }
    ScreenSurface = NULL;

    return 1;
}

uint32_t get_video_flags(void) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;

    if (setting_get_int(OPT_CAT_CLIENT, OPT_FULLSCREEN)) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    return (uint32_t)flags;
}

int video_fullscreen_toggle(SDL_Surface **surface, uint32_t *flags) {
    HARD_ASSERT(surface != NULL);

    bool fullscreen = (SDL_GetWindowFlags(ScreenWindow) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!SDL_SetWindowFullscreen(ScreenWindow, !fullscreen)) {
        return 0;
    }

    *surface = NULL;

    if (flags != NULL) {
        *flags = SDL_WINDOW_RESIZABLE;
        if (SDL_GetWindowFlags(ScreenWindow) & SDL_WINDOW_FULLSCREEN) {
            *flags |= SDL_WINDOW_FULLSCREEN;
        }
    }

    return 1;
}
