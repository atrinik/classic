/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Headless, bounded SDL_ttf equivalence evidence; no window or GPU is created.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_output.h"

static const char *fonts[] = {
    "arial.ttf", "courier.otf", "fanwood.otf", "logisoso.ttf",
    "mono.ttf", "pecita.otf", "sans.ttf", "serif.ttf"
};
static const int points[] = {11, 16, 24};
static const int styles[] = {TTF_STYLE_NORMAL, TTF_STYLE_BOLD, TTF_STYLE_ITALIC,
                           TTF_STYLE_BOLD | TTF_STYLE_ITALIC, TTF_STYLE_UNDERLINE};
static const char *style_names[] = {"normal", "bold", "italic", "bold-italic", "underline"};
static const char *samples[] = {"M", "g", "\xc3\xa9", "AV", "ffi",
                                "Caf\xc3\xa9", "Cafe\xcc\x81"};
static const Uint32 glyphs[] = {'M', 'g', 0xe9, 'A', 'V', 'f', 'i', 'e', 0x301};

static void fail(const char *message) {
    fprintf(stderr, "font probe: %s: %s\n", message, SDL_GetError());
    exit(EXIT_FAILURE);
}

static FILE *output(const char *directory, const char *name) {
    FILE *file = font_output(directory, name);
    if (file == NULL) {
        fprintf(stderr, "font probe: cannot exclusively create output: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return file;
}

static void canonical_pixel(SDL_Surface *surface, int x, int y, Uint8 rgba[4]) {
    if (!SDL_ReadSurfacePixel(surface, x, y, &rgba[0], &rgba[1], &rgba[2], &rgba[3])) {
        fail("pixel read failed");
    }
    if (rgba[3] == 0) {
        rgba[0] = rgba[1] = rgba[2] = 0;
    }
}

static void check_color_key_conversion(void) {
    SDL_Surface *indexed = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_INDEX8);
    if (indexed == NULL) {
        fail("color key fixture allocation failed");
    }
    SDL_Palette *palette = SDL_CreateSurfacePalette(indexed);
    SDL_Color colors[] = {{43, 90, 172, 255}, {212, 165, 83, 255}};
    if (palette == NULL || !SDL_SetPaletteColors(palette, colors, 0, 2) ||
        !SDL_SetSurfaceColorKey(indexed, true, 0)) {
        fail("color key fixture setup failed");
    }
    ((Uint8 *)indexed->pixels)[0] = 0;
    ((Uint8 *)indexed->pixels)[1] = 1;
    SDL_Surface *converted = SDL_ConvertSurface(indexed, SDL_PIXELFORMAT_RGBA32);
    if (converted == NULL) {
        fail("color key fixture conversion failed");
    }
    Uint8 rgba[4];
    canonical_pixel(converted, 0, 0, rgba);
    if (rgba[0] || rgba[1] || rgba[2] || rgba[3]) {
        fail("color key fixture lost transparency");
    }
    canonical_pixel(converted, 1, 0, rgba);
    if (rgba[0] != 212 || rgba[1] != 165 || rgba[2] != 83 || rgba[3] != 255) {
        fail("color key fixture changed foreground");
    }
    SDL_DestroySurface(converted);
    SDL_DestroySurface(indexed);
}

static void write_pixels(SDL_Surface *surface, const char *directory, int id) {
    /* SDL_ttf solid surfaces use a palette color key, not palette alpha.
     * Conversion applies that key before serializing actual RGBA coverage. */
    SDL_Surface *rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (rgba_surface == NULL) {
        fail("RGBA conversion failed");
    }
    char name[32];
    snprintf(name, sizeof(name), "case%04d.rgba", id);
    FILE *file = output(directory, name);
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint8 rgba[4];
            canonical_pixel(rgba_surface, x, y, rgba);
            if (fwrite(rgba, 1, sizeof(rgba), file) != sizeof(rgba)) {
                fail("pixel write failed");
            }
        }
    }
    if (fclose(file) != 0) {
        fail("pixel close failed");
    }
    SDL_DestroySurface(rgba_surface);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--test-output") == 0) {
        const unsigned char bytes[] = {0, '\n', '\r', 0x1a, 0xff};
        FILE *file = output(argv[2], "font-output-test.bin");
        if (fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
            fclose(file);
            return EXIT_FAILURE;
        }
        return fclose(file) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s FONT_DIR OUTPUT_DIR\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!SDL_Init(0) || !TTF_Init()) {
        fail("initialization failed");
    }
    check_color_key_conversion();
    FILE *manifest = output(argv[2], "manifest.json");
    fprintf(manifest, "{\"schema\":1,\"cases\":[\n");
    int id = 0;
    size_t total = 0;
    for (size_t f = 0; f < SDL_arraysize(fonts); f++) {
        char path[4096];
        int length = snprintf(path, sizeof(path), "%s/%s", argv[1], fonts[f]);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            fail("font path too long");
        }
        for (size_t p = 0; p < SDL_arraysize(points); p++) {
            TTF_Font *font = TTF_OpenFont(path, (float)points[p]);
            if (font == NULL) {
                fail("cannot open shipped font");
            }
            for (size_t s = 0; s < SDL_arraysize(styles); s++) {
                TTF_SetFontStyle(font, styles[s]);
                for (int mode = 0; mode < 2; mode++) {
                    for (size_t t = 0; t < SDL_arraysize(samples); t++, id++) {
                        int width, height;
                        if (!TTF_GetStringSize(font, samples[t], 0, &width, &height)) {
                            fail("string size failed");
                        }
                        SDL_Color color = {212, 165, 83, 255};
                        SDL_Surface *surface = mode == 0
                            ? TTF_RenderText_Solid(font, samples[t], 0, color)
                            : TTF_RenderText_Blended(font, samples[t], 0, color);
                        if (surface == NULL) {
                            fail("render failed");
                        }
                        if (surface->w <= 0 || surface->h <= 0 ||
                            surface->w > 1024 || surface->h > 1024) {
                            fail("surface dimension bound exceeded");
                        }
                        total += (size_t)surface->w * (size_t)surface->h * 4;
                        if (total > 128 * 1024 * 1024) {
                            fail("total pixel bound exceeded");
                        }
                        write_pixels(surface, argv[2], id);
                        fprintf(manifest,
                            "%s{\"id\":%d,\"font\":\"%s\",\"point\":%d,"
                            "\"style\":\"%s\",\"mode\":\"%s\",\"sample\":%u,"
                            "\"text_size\":[%d,%d],\"surface_size\":[%d,%d],"
                            "\"font_metrics\":[%d,%d,%d,%d],\"glyphs\":[",
                            id == 0 ? "" : ",\n", id, fonts[f], points[p], style_names[s],
                            mode == 0 ? "solid" : "blended", (unsigned)t, width, height,
                            surface->w, surface->h, TTF_GetFontHeight(font),
                            TTF_GetFontAscent(font), TTF_GetFontDescent(font), TTF_GetFontLineSkip(font));
                        for (size_t g = 0; g < SDL_arraysize(glyphs); g++) {
                            int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
                            bool available = TTF_FontHasGlyph(font, glyphs[g]);
                            if (available && !TTF_GetGlyphMetrics(font, glyphs[g],
                                    &minx, &maxx, &miny, &maxy, &advance)) {
                                fail("glyph metrics failed");
                            }
                            fprintf(manifest, "%s[%u,%d,%d,%d,%d,%d,%d]",
                                g == 0 ? "" : ",", (unsigned)glyphs[g], available ? 1 : 0,
                                minx, maxx, miny, maxy, advance);
                        }
                        fprintf(manifest, "]}");
                        SDL_DestroySurface(surface);
                    }
                }
            }
            TTF_CloseFont(font);
        }
    }
    fprintf(manifest, "\n]}\n");
    if (ferror(manifest) || fclose(manifest) != 0) {
        fail("manifest write failed");
    }
    TTF_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}
