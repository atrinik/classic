/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 Atrinik Development Team                         *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdlib.h>
#include <text.h>
#include <toolkit/toolkit.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void test_colors_parse_opaque(void) {
    static const struct {
        const char *notation;
        SDL_Color expected;
    } cases[] = {
        {COLOR_HGOLD, {0xd4, 0xd5, 0x53, SDL_ALPHA_OPAQUE}},
        {COLOR_RED, {0xff, 0x30, 0x30, SDL_ALPHA_OPAQUE}},
        {COLOR_GREEN, {0x00, 0xff, 0x00, SDL_ALPHA_OPAQUE}},
        {COLOR_WHITE, {0xff, 0xff, 0xff, SDL_ALPHA_OPAQUE}},
    };

    for (size_t i = 0; i < arraysize(cases); i++) {
        SDL_Color actual = {0, 0, 0, SDL_ALPHA_TRANSPARENT};
        TEST_CHECK(text_color_parse(cases[i].notation, &actual));
        TEST_CHECK(actual.r == cases[i].expected.r);
        TEST_CHECK(actual.g == cases[i].expected.g);
        TEST_CHECK(actual.b == cases[i].expected.b);
        TEST_CHECK(actual.a == cases[i].expected.a);
    }
}

static void test_color_parser_boundaries(void) {
    SDL_Color actual = {1, 2, 3, 4};
    TEST_CHECK(text_color_parse("#d4d553", &actual));
    TEST_CHECK(actual.r == 0xd4 && actual.g == 0xd5 && actual.b == 0x53);
    TEST_CHECK(actual.a == SDL_ALPHA_OPAQUE);

    TEST_CHECK(text_color_parse(COLOR_WHITE, NULL));

    actual = (SDL_Color){1, 2, 3, 4};
    TEST_CHECK(!text_color_parse("not-a-color", &actual));
    TEST_CHECK(actual.r == 1 && actual.g == 2 && actual.b == 3 && actual.a == 4);
}

static void test_parsed_color_renders_opaque_glyph(void) {
    SDL_Color color = {0, 0, 0, SDL_ALPHA_TRANSPARENT};
    TEST_CHECK(text_color_parse(COLOR_HGOLD, &color));

    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/fonts/arial.ttf", ATRINIK_TEST_SOURCE_DIR);
    TEST_CHECK(length > 0 && (size_t)length < sizeof(path));

    TEST_CHECK(TTF_Init());
    TTF_Font *font = TTF_OpenFont(path, 24.0f);
    TEST_CHECK(font != NULL);
    SDL_Surface *glyph = TTF_RenderText_Blended(font, "M", 0, color);
    TEST_CHECK(glyph != NULL);

    Uint8 max_alpha = SDL_ALPHA_TRANSPARENT;
    for (int y = 0; y < glyph->h; y++) {
        for (int x = 0; x < glyph->w; x++) {
            Uint8 red, green, blue, alpha;
            TEST_CHECK(SDL_ReadSurfacePixel(glyph, x, y, &red, &green, &blue, &alpha));
            max_alpha = MAX(max_alpha, alpha);
        }
    }
    TEST_CHECK(max_alpha == SDL_ALPHA_OPAQUE);

    SDL_DestroySurface(glyph);
    TTF_CloseFont(font);
    TTF_Quit();
}

int main(void) {
    test_colors_parse_opaque();
    test_color_parser_boundaries();
    test_parsed_color_renders_opaque_glyph();
    return 0;
}
