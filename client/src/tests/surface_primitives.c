/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2026 The Atrinik Project                              *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>

SDL_Surface *ScreenSurface = NULL;

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

static void test_packed_indexed_conversion(void) {
    SDL_Surface *surface = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_INDEX4MSB);
    TEST_CHECK(surface != NULL);

    SDL_Color colors[16] = {{0}};
    colors[0] = (SDL_Color){255, 0, 255, SDL_ALPHA_TRANSPARENT};
    colors[1] = (SDL_Color){240, 10, 20, SDL_ALPHA_OPAQUE};
    SDL_Palette *palette = SDL_CreatePalette(arraysize(colors));
    TEST_CHECK(palette != NULL);
    TEST_CHECK(SDL_SetPaletteColors(palette, colors, 0, arraysize(colors)));
    TEST_CHECK(SDL_SetSurfacePalette(surface, palette));
    SDL_DestroyPalette(palette);
    ((Uint8 *)surface->pixels)[0] = 0x10;

    TEST_CHECK(surface_ensure_blittable(&surface));
    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    TEST_CHECK(details != NULL);
    TEST_CHECK(details->bits_per_pixel == sizeof(Uint32) * 8);

    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(surface, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 240 && green == 10 && blue == 20 && alpha == SDL_ALPHA_OPAQUE);
    TEST_CHECK(SDL_ReadSurfacePixel(surface, 1, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(alpha == SDL_ALPHA_TRANSPARENT);

    SDL_Surface *destination = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_XRGB8888);
    TEST_CHECK(destination != NULL);
    TEST_CHECK(SDL_FillSurfaceRect(destination, NULL, surface_map_rgb(destination, 1, 2, 3)));
    TEST_CHECK(SDL_BlitSurface(surface, NULL, destination, NULL));
    TEST_CHECK(SDL_ReadSurfacePixel(destination, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 240 && green == 10 && blue == 20);
    TEST_CHECK(SDL_ReadSurfacePixel(destination, 1, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 1 && green == 2 && blue == 3);

    SDL_DestroySurface(destination);
    SDL_DestroySurface(surface);
}

static void test_index8_visible_bounds(void) {
    SDL_Surface *surface = SDL_CreateSurface(5, 4, SDL_PIXELFORMAT_INDEX8);
    TEST_CHECK(surface != NULL);

    SDL_Color colors[256] = {{0}};
    colors[0] = (SDL_Color){255, 0, 255, SDL_ALPHA_TRANSPARENT};
    colors[1] = (SDL_Color){240, 10, 20, SDL_ALPHA_OPAQUE};
    colors[2] = (SDL_Color){10, 20, 30, SPRITE_ALPHA_VISIBLE_MIN - 1};
    colors[3] = (SDL_Color){40, 50, 60, SPRITE_ALPHA_VISIBLE_MIN};
    SDL_Palette *palette = SDL_CreatePalette(arraysize(colors));
    TEST_CHECK(palette != NULL);
    TEST_CHECK(SDL_SetPaletteColors(palette, colors, 0, arraysize(colors)));
    TEST_CHECK(SDL_SetSurfacePalette(surface, palette));
    SDL_DestroyPalette(palette);

    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, 0));
    SDL_Rect visible = {1, 1, 3, 2};
    TEST_CHECK(SDL_FillSurfaceRect(surface, &visible, 1));

    TEST_CHECK(surface_ensure_blittable(&surface));
    TEST_CHECK(surface->format == SDL_PIXELFORMAT_INDEX8);
    TEST_CHECK(!surface_pixel_visible(surface, 0, 0));
    TEST_CHECK(surface_pixel_visible(surface, 1, 1));
    TEST_CHECK(!surface_pixel_visible(surface, -1, 0));
    TEST_CHECK(!surface_pixel_visible(surface, surface->w, 0));

    sprite_struct sprite = {0};
    TEST_CHECK(sprite_borders_get(surface, &sprite));
    TEST_CHECK(sprite.border_up == 1);
    TEST_CHECK(sprite.border_down == 1);
    TEST_CHECK(sprite.border_left == 1);
    TEST_CHECK(sprite.border_right == 1);

    SDL_Rect pixel = {0, 0, 1, 1};
    TEST_CHECK(SDL_FillSurfaceRect(surface, &pixel, 2));
    TEST_CHECK(!surface_pixel_visible(surface, pixel.x, pixel.y));
    pixel.x = surface->w - 1;
    TEST_CHECK(SDL_FillSurfaceRect(surface, &pixel, 3));
    TEST_CHECK(surface_pixel_visible(surface, pixel.x, pixel.y));

    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, 0));
    sprite = (sprite_struct){
        .border_up = 7,
        .border_down = 7,
        .border_left = 7,
        .border_right = 7,
    };
    TEST_CHECK(!sprite_borders_get(surface, &sprite));
    TEST_CHECK(sprite.border_up == 7);
    TEST_CHECK(sprite.border_down == 7);
    TEST_CHECK(sprite.border_left == 7);
    TEST_CHECK(sprite.border_right == 7);

    SDL_DestroySurface(surface);
}

static void test_truecolor_pixel_visibility(void) {
    SDL_Surface *surface = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, 0, 10, 20, 30, SPRITE_ALPHA_VISIBLE_MIN - 1));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 1, 0, 10, 20, 30, SPRITE_ALPHA_VISIBLE_MIN));
    TEST_CHECK(!surface_pixel_visible(surface, 0, 0));
    TEST_CHECK(surface_pixel_visible(surface, 1, 0));
    SDL_DestroySurface(surface);

    surface = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_XRGB8888);
    TEST_CHECK(surface != NULL);
    Uint32 color_key = surface_map_rgb(surface, 10, 20, 30);
    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, color_key));
    TEST_CHECK(SDL_SetSurfaceColorKey(surface, true, color_key));
    TEST_CHECK(!surface_pixel_visible(surface, 0, 0));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 1, 0, 40, 50, 60, SDL_ALPHA_OPAQUE));
    TEST_CHECK(surface_pixel_visible(surface, 1, 0));
    SDL_DestroySurface(surface);
}

static void test_legacy_texture_transparency(void) {
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/textures/invslot.png", ATRINIK_TEST_SOURCE_DIR);
    TEST_CHECK(length > 0 && (size_t)length < sizeof(path));
    SDL_Surface *surface = IMG_Load(path);
    TEST_CHECK(surface != NULL);

    Uint32 color_key;
    TEST_CHECK(!SDL_GetSurfaceColorKey(surface, &color_key));
    TEST_CHECK(surface_set_transparent_black(surface));

    SDL_Surface *converted = surface_to_display_alpha(surface);
    TEST_CHECK(converted != NULL);
    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(converted,
                                    converted->w / 2,
                                    converted->h / 2,
                                    &red,
                                    &green,
                                    &blue,
                                    &alpha));
    TEST_CHECK(alpha == SDL_ALPHA_TRANSPARENT);
    TEST_CHECK(SDL_ReadSurfacePixel(converted, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(alpha == SDL_ALPHA_OPAQUE);

    SDL_DestroySurface(converted);
    SDL_DestroySurface(surface);

    length = snprintf(path, sizeof(path), "%s/textures/apply.png", ATRINIK_TEST_SOURCE_DIR);
    TEST_CHECK(length > 0 && (size_t)length < sizeof(path));
    surface = IMG_Load(path);
    TEST_CHECK(surface != NULL);
    TEST_CHECK(surface_set_transparent_black(surface));
    converted = surface_to_display_alpha(surface);
    TEST_CHECK(converted != NULL);
    TEST_CHECK(SDL_ReadSurfacePixel(converted,
                                    converted->w / 2,
                                    converted->h / 2,
                                    &red,
                                    &green,
                                    &blue,
                                    &alpha));
    TEST_CHECK(alpha == SDL_ALPHA_OPAQUE);
    SDL_DestroySurface(converted);
    SDL_DestroySurface(surface);
}

static SDL_Surface *create_indexed_alpha_surface(void) {
    SDL_Surface *surface = SDL_CreateSurface(5, 5, SDL_PIXELFORMAT_INDEX8);
    TEST_CHECK(surface != NULL);

    SDL_Color colors[256];
    for (size_t i = 0; i < arraysize(colors); i++) {
        colors[i] = (SDL_Color){0, 0, 0, SDL_ALPHA_OPAQUE};
    }
    colors[0] = (SDL_Color){20, 80, 180, SDL_ALPHA_TRANSPARENT};
    colors[1] = (SDL_Color){30, 120, 230, SDL_ALPHA_OPAQUE};
    colors[2] = (SDL_Color){245, 245, 230, SDL_ALPHA_OPAQUE};
    SDL_Palette *palette = SDL_CreatePalette(arraysize(colors));
    TEST_CHECK(palette != NULL);
    TEST_CHECK(SDL_SetPaletteColors(palette, colors, 0, arraysize(colors)));
    TEST_CHECK(SDL_SetSurfacePalette(surface, palette));
    SDL_DestroyPalette(palette);

    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, 0));
    SDL_Rect opaque = {1, 1, 3, 3};
    TEST_CHECK(SDL_FillSurfaceRect(surface, &opaque, 1));
    return surface;
}

static void assert_rotation_transparency(SDL_Surface *source, int smooth) {
    SDL_Surface *rotated = rotozoomSurface(source, -135.0, 1.0, smooth);
    TEST_CHECK(rotated != NULL);
    TEST_CHECK(rotated->w > source->w || rotated->h > source->h);

    SDL_Surface *destination = SDL_CreateSurface(rotated->w, rotated->h, SDL_PIXELFORMAT_XRGB8888);
    TEST_CHECK(destination != NULL);
    TEST_CHECK(SDL_FillSurfaceRect(destination, NULL, surface_map_rgb(destination, 1, 2, 3)));
    TEST_CHECK(SDL_BlitSurface(rotated, NULL, destination, NULL));

    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(destination, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 1 && green == 2 && blue == 3);
    TEST_CHECK(SDL_ReadSurfacePixel(destination,
                                    destination->w / 2,
                                    destination->h / 2,
                                    &red,
                                    &green,
                                    &blue,
                                    &alpha));
    TEST_CHECK(red == 30 && green == 120 && blue == 230);

    SDL_DestroySurface(destination);
    SDL_DestroySurface(rotated);
}

static void test_indexed_alpha_rotation(int smooth) {
    SDL_Surface *source = create_indexed_alpha_surface();
    assert_rotation_transparency(source, smooth);
    TEST_CHECK(source->format == SDL_PIXELFORMAT_INDEX8);
    SDL_DestroySurface(source);
}

static void test_color_key_rotation(void) {
    SDL_Surface *source = create_indexed_alpha_surface();
    SDL_Palette *palette = SDL_GetSurfacePalette(source);
    TEST_CHECK(palette != NULL);
    SDL_Color transparent = palette->colors[0];
    transparent.a = SDL_ALPHA_OPAQUE;
    TEST_CHECK(SDL_SetPaletteColors(palette, &transparent, 0, 1));
    TEST_CHECK(SDL_SetSurfaceColorKey(source, true, 0));

    assert_rotation_transparency(source, 1);
    SDL_DestroySurface(source);
}

static void test_opaque_indexed_rotation(void) {
    SDL_Surface *source = create_indexed_alpha_surface();
    SDL_Palette *palette = SDL_GetSurfacePalette(source);
    TEST_CHECK(palette != NULL);
    SDL_Color transparent = palette->colors[0];
    transparent.a = SDL_ALPHA_OPAQUE;
    TEST_CHECK(SDL_SetPaletteColors(palette, &transparent, 0, 1));

    SDL_Surface *rotated = rotozoomSurface(source, -135.0, 1.0, 1);
    TEST_CHECK(rotated != NULL);
    Uint8 red, green, blue, alpha;
    TEST_CHECK(
        SDL_ReadSurfacePixel(rotated, rotated->w / 2, rotated->h / 2, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 30 && green == 120 && blue == 230 && alpha == SDL_ALPHA_OPAQUE);
    SDL_DestroySurface(rotated);
    SDL_DestroySurface(source);
}

static void test_indexed_alpha_without_rotation(void) {
    SDL_Surface *source = create_indexed_alpha_surface();
    SDL_Surface *scaled = rotozoomSurface(source, 0.0, 1.0, 1);
    TEST_CHECK(scaled != NULL);
    TEST_CHECK(scaled->format == SDL_PIXELFORMAT_INDEX8);

    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(scaled, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(alpha == SDL_ALPHA_TRANSPARENT);

    SDL_DestroySurface(scaled);
    SDL_DestroySurface(source);
}

static void test_transform_invalid_input(void) {
    TEST_CHECK(rotozoomSurface(NULL, -135.0, 1.0, 1) == NULL);

    SDL_Surface *source = create_indexed_alpha_surface();
    TEST_CHECK(rotozoomSurface(source, -135.0, 0.0, 1) == NULL);
    SDL_DestroySurface(source);
}

static void test_true_color_alpha_rotation(void) {
    SDL_Surface *source = SDL_CreateSurface(5, 5, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(source != NULL);
    TEST_CHECK(SDL_FillSurfaceRect(source, NULL, surface_map_rgba(source, 20, 80, 180, 0)));
    SDL_Rect opaque = {1, 1, 3, 3};
    TEST_CHECK(SDL_FillSurfaceRect(source, &opaque, surface_map_rgba(source, 30, 120, 230, 255)));

    assert_rotation_transparency(source, 1);
    SDL_DestroySurface(source);
}

static void test_darken_preserves_alpha(void) {
    SDL_Surface *surface = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, 0, 100, 150, 200, SDL_ALPHA_TRANSPARENT));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 1, 0, 100, 150, 200, 200));

    TEST_CHECK(surface_darken_preserve_alpha(surface, 128));

    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(surface, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 50 && green == 75 && blue == 100 && alpha == SDL_ALPHA_TRANSPARENT);
    TEST_CHECK(SDL_ReadSurfacePixel(surface, 1, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 50 && green == 75 && blue == 100 && alpha == 200);

    SDL_DestroySurface(surface);
}

static void assert_map_marker_palette(SDL_Surface *surface) {
    int visible = 0;
    int bright = 0;

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            Uint8 red, green, blue, alpha;
            TEST_CHECK(SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha));
            if (alpha == SDL_ALPHA_TRANSPARENT) {
                continue;
            }

            visible++;
            if (alpha >= SPRITE_ALPHA_VISIBLE_MIN) {
                TEST_CHECK(red != 0 || green != 0 || blue != 0);
                TEST_CHECK(red >= green && green >= blue);
            }
            if (red >= 240 && green >= 180 && blue >= 50) {
                bright++;
            }
        }
    }

    TEST_CHECK(visible > 0);
    TEST_CHECK(bright > 0);
}

static void test_map_marker_rotation_contract(void) {
    char path[1024];
    int length =
        snprintf(path, sizeof(path), "%s/textures/map_marker.png", ATRINIK_TEST_SOURCE_DIR);
    TEST_CHECK(length > 0 && (size_t)length < sizeof(path));

    SDL_Surface *source = IMG_Load(path);
    TEST_CHECK(source != NULL);
    TEST_CHECK(source->w == 19 && source->h == 28);
    TEST_CHECK(surface_set_transparent_black(source));

    SDL_Surface *converted = surface_to_display_alpha(source);
    SDL_DestroySurface(source);
    TEST_CHECK(converted != NULL);
    assert_map_marker_palette(converted);

    const double zooms[] = {0.5, 1.0, 2.0};
    for (size_t zoom = 0; zoom < arraysize(zooms); zoom++) {
        /* Region-map facings advance clockwise in 45-degree steps. */
        for (int direction = 0; direction < 8; direction++) {
            SDL_Surface *transformed =
                rotozoomSurface(converted, direction * 45.0, zooms[zoom], 1);
            TEST_CHECK(transformed != NULL);
            assert_map_marker_palette(transformed);
            SDL_DestroySurface(transformed);
        }
    }

    SDL_DestroySurface(converted);
}

int main(void) {
    test_packed_indexed_conversion();
    test_index8_visible_bounds();
    test_truecolor_pixel_visibility();
    test_legacy_texture_transparency();
    test_indexed_alpha_rotation(0);
    test_indexed_alpha_rotation(1);
    test_color_key_rotation();
    test_opaque_indexed_rotation();
    test_indexed_alpha_without_rotation();
    test_transform_invalid_input();
    test_true_color_alpha_rotation();
    test_darken_preserves_alpha();
    test_map_marker_rotation_contract();
    return 0;
}
