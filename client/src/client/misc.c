/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Miscellaneous functions.
 */

#include <image_codec.h>
#include <gpu_renderer.h>
#include <main.h>
#include <misc.h>
#include <textwin.h>
#include <wrapper.h>
#include <toolkit/logger.h>
#include <toolkit/toolkit.h>

#ifdef ATRINIK_WIDGET_TESTS
static bool ui_test_clock_enabled;
static uint32_t ui_test_clock_ticks;
#endif

uint32_t client_ui_ticks(void) {
#ifdef ATRINIK_WIDGET_TESTS
    if (ui_test_clock_enabled) {
        return ui_test_clock_ticks;
    }
#endif
    return SDL_GetTicks();
}

#ifdef ATRINIK_WIDGET_TESTS
void client_ui_test_clock_set(uint32_t ticks) {
    ui_test_clock_enabled = true;
    ui_test_clock_ticks = ticks;
}

void client_ui_test_clock_reset(void) {
    ui_test_clock_enabled = false;
    ui_test_clock_ticks = 0;
}
#endif

/**
 * Opens an url in the system's default browser.
 * @param url
 * URL to open.
 */
void browser_open(const char *url) {
#if defined(WIN32)
    ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOWDEFAULT);
#elif defined(__GNUC__)
    char buf[HUGE_BUF];

    snprintf(buf, sizeof(buf), "xdg-open \"%s\"", url);

    if (system(buf) != 0) {
        snprintf(buf, sizeof(buf), "x-www-browser \"%s\"", url);

        if (system(buf) != 0) {
            LOG(BUG, "Could not open '%s'.", url);
        }
    }
#else
    LOG(DEBUG, "Unknown platform, cannot open '%s'.", url);
#endif
}

/**
 * Get the full package version as string.
 *
 * If patch version is 0, it will not be appended to the version string.
 * @param dst
 * Where to store the version.
 * @param dstlen
 * Size of dst.
 * @return
 * 'dst'.
 */
char *package_get_version_full(char *dst, size_t dstlen) {
#if PACKAGE_VERSION_PATCH == 0
    package_get_version_partial(dst, dstlen);
#else
    snprintf(dst,
             dstlen,
             "%d.%d.%d",
             PACKAGE_VERSION_MAJOR,
             PACKAGE_VERSION_MINOR,
             PACKAGE_VERSION_PATCH);
#endif
    return dst;
}

/**
 * Get the partial package version. This means that the patch version
 * will not be included, even if it's not 0.
 * @param dst
 * Where to store the version.
 * @param dstlen
 * Size of dst.
 * @return
 * 'dst'
 */
char *package_get_version_partial(char *dst, size_t dstlen) {
    snprintf(dst, dstlen, "%d.%d", PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR);
    return dst;
}

typedef struct screenshot_job {
    char path[HUGE_BUF];
} screenshot_job_t;

#ifdef ATRINIK_WIDGET_TESTS
static bool screenshot_test_active;
static SDL_Surface *screenshot_test_surface;

void screenshot_test_begin(void) {
    SDL_DestroySurface(screenshot_test_surface);
    screenshot_test_surface = NULL;
    screenshot_test_active = true;
}

SDL_Surface *screenshot_test_take(void) {
    SDL_Surface *surface = screenshot_test_surface;
    screenshot_test_surface = NULL;
    return surface;
}
#endif

static void screenshot_complete(SDL_Surface *surface, void *userdata) {
    screenshot_job_t *job = userdata;
#ifdef ATRINIK_WIDGET_TESTS
    if (screenshot_test_active) {
        screenshot_test_active = false;
        screenshot_test_surface = surface;
        free(job);
        return;
    }
#endif
    if (surface == NULL) {
        draw_info_format(COLOR_RED, "Failed to read back GPU screenshot: %s", SDL_GetError());
    } else if (image_codec_save_png(surface, job->path)) {
        draw_info_format(COLOR_GREEN, "Saved screenshot as %s successfully.", job->path);
    } else {
        draw_info_format(COLOR_RED, "Failed to write screenshot data (path: %s).", job->path);
    }
    SDL_DestroySurface(surface);
    free(job);
}

static void screenshot_cancel(void *userdata) {
    free(userdata);
}

/**
 * Enqueue a screenshot readback of the completed GPU frame.
 * @param rect
 * Optional completed-frame rectangle. NULL captures the complete window.
 */
void screenshot_create(const SDL_Rect *rect) {
    char timebuf[64];
    struct timeval tv;
    struct tm *tm;
    time_t seconds;
    screenshot_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        draw_info(COLOR_RED, "Could not allocate GPU screenshot request.");
        return;
    }

#ifdef ATRINIK_WIDGET_TESTS
    /* Test interception still exercises the production asynchronous GPU
     * readback and callbacks, but it must not inspect time or create host
     * directories before that command is enqueued. */
    if (screenshot_test_active) {
        if (!gpu_renderer_readback_async(rect, screenshot_complete, screenshot_cancel, job)) {
            draw_info_format(COLOR_RED, "Failed to enqueue GPU screenshot: %s", SDL_GetError());
            free(job);
        }
        return;
    }
#endif

    gettimeofday(&tv, NULL);
    seconds = tv.tv_sec;
    tm = localtime(&seconds);

    if (tm) {
        char timebuf2[32];

        strftime(timebuf2, sizeof(timebuf2), "%Y-%m-%d-%H-%M-%S", tm);
        snprintf(timebuf, sizeof(timebuf), "%s-%06" PRIu64, timebuf2, (uint64_t)tv.tv_usec);
    } else {
        draw_info(COLOR_RED, "Could not get time information.");
        free(job);
        return;
    }

    snprintf(job->path,
             sizeof(job->path),
             "%s/.atrinik/screenshots/Atrinik-%s.png",
             get_config_dir(),
             timebuf);
    mkdir_ensure(job->path);
    if (!gpu_renderer_readback_async(rect, screenshot_complete, screenshot_cancel, job)) {
        draw_info_format(COLOR_RED, "Failed to enqueue GPU screenshot: %s", SDL_GetError());
        free(job);
    }
}
