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

#include <global.h>
#include <image_codec.h>
#include <wrapper.h>

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

/**
 * Create a screenshot of the specified surface, or explicitly read back the
 * completed GPU frame when surface is NULL.
 * @param surface
 * The surface to take a screenshot of.
 */
void screenshot_create(SDL_Surface *surface) {
    char path[HUGE_BUF], timebuf[64];
    struct timeval tv;
    struct tm *tm;
    time_t seconds;

    bool gpu_readback = surface == NULL;
    if (gpu_readback) {
        surface = gpu_renderer_readback(NULL);
        if (surface == NULL) {
            draw_info_format(COLOR_RED, "Failed to read back GPU screenshot: %s", SDL_GetError());
            return;
        }
    }

    gettimeofday(&tv, NULL);
    seconds = tv.tv_sec;
    tm = localtime(&seconds);

    if (tm) {
        char timebuf2[32];

        strftime(timebuf2, sizeof(timebuf2), "%Y-%m-%d-%H-%M-%S", tm);
        snprintf(timebuf, sizeof(timebuf), "%s-%06" PRIu64, timebuf2, (uint64_t)tv.tv_usec);
    } else {
        draw_info(COLOR_RED, "Could not get time information.");
        goto cleanup;
    }

    snprintf(path,
             sizeof(path),
             "%s/.atrinik/screenshots/Atrinik-%s.png",
             get_config_dir(),
             timebuf);
    mkdir_ensure(path);

    if (image_codec_save_png(surface, path)) {
        draw_info_format(COLOR_GREEN, "Saved screenshot as %s successfully.", path);
    } else {
        draw_info_format(COLOR_RED, "Failed to write screenshot data (path: %s).", path);
    }
cleanup:
    if (gpu_readback) {
        SDL_DestroySurface(surface);
    }
}
