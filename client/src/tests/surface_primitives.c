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

static void test_safe_pixel_access(void) {
    SDL_Surface *surface = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, 0, 10, 20, 30, SDL_ALPHA_OPAQUE));

    Uint32 pixel;
    TEST_CHECK(surface_pixel_get(surface, 0, 0, &pixel));
    TEST_CHECK(pixel == surface_map_rgba(surface, 10, 20, 30, SDL_ALPHA_OPAQUE));
    TEST_CHECK(!surface_pixel_get(surface, -1, 0, &pixel));
    TEST_CHECK(!surface_pixel_get(surface, surface->w, 0, &pixel));
    TEST_CHECK(!surface_pixel_get(NULL, 0, 0, &pixel));
    TEST_CHECK(!surface_pixel_get(surface, 0, 0, NULL));

    void *surface_pixels = surface->pixels;
    surface->pixels = NULL;
    TEST_CHECK(!surface_pixel_get(surface, 0, 0, &pixel));
    surface->pixels = surface_pixels;

    /* Model decoder surfaces that require SDL_LockSurface before direct access. */
    surface->flags |= SDL_SURFACE_LOCK_NEEDED;
    TEST_CHECK(SDL_MUSTLOCK(surface));
    TEST_CHECK(surface_pixel_get(surface, 0, 0, &pixel));
    surface->flags &= ~SDL_SURFACE_LOCK_NEEDED;

    SDL_DestroySurface(surface);

    surface = SDL_CreateSurface(3, 3, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    Uint32 color = surface_map_rgba(surface, 0, 0, 0, SDL_ALPHA_OPAQUE);
    Uint32 visible = surface_map_rgba(surface, 255, 255, 255, SDL_ALPHA_OPAQUE);
    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, color));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 1, 1, 255, 255, 255, SDL_ALPHA_OPAQUE));

    int top, bottom, left, right;
    TEST_CHECK(surface_borders_get(surface, &top, &bottom, &left, &right, color) == 1);
    TEST_CHECK(top == 1 && bottom == 1 && left == 1 && right == 1);
    TEST_CHECK(surface_texture_borders_get(surface, &top, &bottom, &left, &right));
    TEST_CHECK(top == 1 && bottom == 1 && left == 1 && right == 1);

    surface->flags |= SDL_SURFACE_LOCK_NEEDED;
    TEST_CHECK(surface_borders_get(surface, &top, &bottom, &left, &right, color) == 1);
    TEST_CHECK(surface_texture_borders_get(surface, &top, &bottom, &left, &right));
    surface->flags &= ~SDL_SURFACE_LOCK_NEEDED;

    TEST_CHECK(SDL_SetSurfaceColorKey(surface, true, color));
    TEST_CHECK(surface_texture_borders_get(surface, &top, &bottom, &left, &right));
    TEST_CHECK(top == 1 && bottom == 1 && left == 1 && right == 1);
    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, visible));
    TEST_CHECK(surface_borders_get(surface, &top, &bottom, &left, &right, visible) == 0);
    TEST_CHECK(top == 0 && bottom == 0 && left == 0 && right == 0);

    void *border_pixels = surface->pixels;
    surface->pixels = NULL;
    TEST_CHECK(surface_borders_get(surface, &top, &bottom, &left, &right, color) < 0);
    surface->pixels = border_pixels;
    TEST_CHECK(surface_borders_get(NULL, &top, &bottom, &left, &right, color) < 0);
    TEST_CHECK(surface_texture_borders_get(NULL, &top, &bottom, &left, &right) == false);

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

static void test_mutable_color_key_surface_reuse(void) {
    SDL_Surface *surface = SDL_CreateSurface(4, 1, SDL_PIXELFORMAT_XRGB8888);
    SDL_Surface *destination = SDL_CreateSurface(4, 1, SDL_PIXELFORMAT_XRGB8888);
    TEST_CHECK(surface != NULL && destination != NULL);
    TEST_CHECK(surface_set_transparent_black_mutable(surface));
    Uint8 red, green, blue, alpha;

    Uint32 black = surface_map_rgb(surface, 0, 0, 0);
    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, black));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, 0, 240, 20, 10, SDL_ALPHA_OPAQUE));
    TEST_CHECK(SDL_BlitSurface(surface, NULL, destination, NULL));

    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, black));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 2, 0, 10, 20, 240, SDL_ALPHA_OPAQUE));
    TEST_CHECK(SDL_FillSurfaceRect(destination, NULL, surface_map_rgb(destination, 1, 2, 3)));
    TEST_CHECK(SDL_BlitSurface(surface, NULL, destination, NULL));

    TEST_CHECK(surface_clear_transparent_black(surface));
    TEST_CHECK(SDL_ReadSurfacePixel(surface, 2, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 0 && green == 0 && blue == 0);
    TEST_CHECK(SDL_SetSurfaceRLE(surface, true));
    TEST_CHECK(SDL_BlitSurface(surface, NULL, destination, NULL));
    TEST_CHECK(!surface_clear_transparent_black(surface));
    TEST_CHECK(SDL_SetSurfaceRLE(surface, false));

    TEST_CHECK(SDL_ReadSurfacePixel(destination, 0, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 1 && green == 2 && blue == 3);
    TEST_CHECK(SDL_ReadSurfacePixel(destination, 2, 0, &red, &green, &blue, &alpha));
    TEST_CHECK(red == 10 && green == 20 && blue == 240);

    SDL_DestroySurface(destination);
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

static void assert_rotation_transparency(SDL_Surface *source, int zoom_filter) {
    SDL_Surface *rotated = rotozoomSurface(source, -135.0, 1.0, zoom_filter);
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

static SDL_Surface *create_directional_surface(int width, int height) {
    SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);
    TEST_CHECK(SDL_FillSurfaceRect(surface, NULL, surface_map_rgba(surface, 0, 0, 0, 0)));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, 0, 255, 0, 0, SDL_ALPHA_OPAQUE));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, width - 1, 0, 0, 255, 0, SDL_ALPHA_OPAQUE));
    TEST_CHECK(SDL_WriteSurfacePixel(surface, 0, height - 1, 0, 0, 255, SDL_ALPHA_OPAQUE));
    TEST_CHECK(
        SDL_WriteSurfacePixel(surface, width - 1, height - 1, 255, 255, 0, SDL_ALPHA_OPAQUE));
    return surface;
}

static void assert_surfaces_equal(SDL_Surface *actual, SDL_Surface *expected) {
    TEST_CHECK(actual != NULL && expected != NULL);
    TEST_CHECK(actual->w == expected->w && actual->h == expected->h);

    for (int y = 0; y < actual->h; y++) {
        for (int x = 0; x < actual->w; x++) {
            Uint8 actual_red, actual_green, actual_blue, actual_alpha;
            Uint8 expected_red, expected_green, expected_blue, expected_alpha;
            TEST_CHECK(SDL_ReadSurfacePixel(actual,
                                            x,
                                            y,
                                            &actual_red,
                                            &actual_green,
                                            &actual_blue,
                                            &actual_alpha));
            TEST_CHECK(SDL_ReadSurfacePixel(expected,
                                            x,
                                            y,
                                            &expected_red,
                                            &expected_green,
                                            &expected_blue,
                                            &expected_alpha));
            TEST_CHECK(actual_red == expected_red && actual_green == expected_green &&
                       actual_blue == expected_blue && actual_alpha == expected_alpha);
        }
    }
}

static void assert_surface_pixel(SDL_Surface *surface,
                                 int x,
                                 int y,
                                 Uint8 expected_red,
                                 Uint8 expected_green,
                                 Uint8 expected_blue,
                                 Uint8 expected_alpha) {
    Uint8 red, green, blue, alpha;
    TEST_CHECK(SDL_ReadSurfacePixel(surface, x, y, &red, &green, &blue, &alpha));
    TEST_CHECK(red == expected_red && green == expected_green && blue == expected_blue &&
               alpha == expected_alpha);
}

static SDL_Surface *create_zoom_filter_surface(void) {
    SDL_Surface *surface = SDL_CreateSurface(4, 3, SDL_PIXELFORMAT_RGBA32);
    TEST_CHECK(surface != NULL);

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            TEST_CHECK(SDL_WriteSurfacePixel(surface,
                                             x,
                                             y,
                                             (Uint8)(32 + x * 48),
                                             (Uint8)(24 + y * 64),
                                             (Uint8)(16 + (x + y) * 32),
                                             SDL_ALPHA_OPAQUE));
        }
    }

    return surface;
}

static void test_zoom_filter_modes(void) {
    const struct {
        int zoom_filter;
        SDL_ScaleMode scale_mode;
    } cases[] = {
        {ZOOM_FILTER_OFF, SDL_SCALEMODE_NEAREST},
        {ZOOM_FILTER_PIXELART, SDL_SCALEMODE_PIXELART},
        {ZOOM_FILTER_LINEAR, SDL_SCALEMODE_LINEAR},
    };
    SDL_Surface *source = create_zoom_filter_surface();
    int width, height;
    zoomSurfaceSize(source->w, source->h, 1.5, 1.25, &width, &height);
    TEST_CHECK(width > source->w && height > source->h);

    for (size_t i = 0; i < arraysize(cases); i++) {
        TEST_CHECK(zoom_filter_to_scale_mode(cases[i].zoom_filter) == cases[i].scale_mode);

        /* Exercise the same non-100% scaling boundary used by the map surface. */
        SDL_Surface *scaled = zoomSurface(source, 1.5, 1.25, cases[i].zoom_filter);
        SDL_Surface *expected = SDL_ScaleSurface(source, width, height, cases[i].scale_mode);
        SDL_Surface *repeat = zoomSurface(source, 1.5, 1.25, cases[i].zoom_filter);
        TEST_CHECK(scaled != NULL && expected != NULL && repeat != NULL);
        assert_surfaces_equal(scaled, expected);
        assert_surfaces_equal(scaled, repeat);

        /* A 100% map is not resampled, regardless of the selected filter. */
        SDL_Surface *unchanged = zoomSurface(source, 1.0, 1.0, cases[i].zoom_filter);
        TEST_CHECK(unchanged != NULL);
        assert_surfaces_equal(unchanged, source);

        SDL_DestroySurface(unchanged);
        SDL_DestroySurface(repeat);
        SDL_DestroySurface(expected);
        SDL_DestroySurface(scaled);
    }

    TEST_CHECK(zoom_filter_to_scale_mode(-1) == SDL_SCALEMODE_NEAREST);
    TEST_CHECK(zoom_filter_to_scale_mode(ZOOM_FILTER_NUM) == SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(source);
}

static void test_authored_rotation_direction(void) {
    SDL_Surface *source = create_directional_surface(3, 2);
    SDL_Surface *positive = rotozoomSurface(source, 90.0, 1.0, 0);
    SDL_Surface *negative = rotozoomSurface(source, -90.0, 1.0, 0);
    SDL_Surface *positive_reference = SDL_RotateSurface(source, -90.0f);
    SDL_Surface *negative_reference = SDL_RotateSurface(source, 90.0f);

    TEST_CHECK(positive != NULL && negative != NULL);
    TEST_CHECK(positive->w == 2 && positive->h == 3);
    TEST_CHECK(negative->w == 2 && negative->h == 3);
    assert_surfaces_equal(positive, positive_reference);
    assert_surfaces_equal(negative, negative_reference);

    /* The two signs must remain distinguishable at the compatibility edge. */
    assert_surface_pixel(positive, 0, 2, 255, 0, 0, SDL_ALPHA_OPAQUE);
    assert_surface_pixel(negative, 1, 0, 255, 0, 0, SDL_ALPHA_OPAQUE);

    SDL_DestroySurface(negative_reference);
    SDL_DestroySurface(positive_reference);
    SDL_DestroySurface(negative);
    SDL_DestroySurface(positive);
    SDL_DestroySurface(source);
}

static void test_authored_rotation_geometry(void) {
    const struct {
        int width;
        int height;
        double angle;
    } cases[] = {
        {28, 19, -10.0}, /* flagwall_short_brick1.111 rail */
        {48, 23, 128.0}, /* floor_wood2.101 bridge */
    };

    for (size_t i = 0; i < arraysize(cases); i++) {
        SDL_Surface *source = create_directional_surface(cases[i].width, cases[i].height);
        int expected_width, expected_height;
        rotozoomSurfaceSizeXY(cases[i].width,
                              cases[i].height,
                              cases[i].angle,
                              1.0,
                              1.0,
                              &expected_width,
                              &expected_height);

        SDL_Surface *rotated = rotozoomSurface(source, cases[i].angle, 1.0, 0);
        SDL_Surface *reference = SDL_RotateSurface(source, (float)-cases[i].angle);
        TEST_CHECK(rotated != NULL && reference != NULL);
        TEST_CHECK(rotated->w == expected_width && rotated->h == expected_height);
        assert_surfaces_equal(rotated, reference);

        int scaled_width, scaled_height;
        rotozoomSurfaceSizeXY(cases[i].width,
                              cases[i].height,
                              cases[i].angle,
                              1.5,
                              0.75,
                              &scaled_width,
                              &scaled_height);
        SDL_Surface *scaled_rotated = rotozoomSurfaceXY(source, cases[i].angle, 1.5, 0.75, 0);
        int source_scaled_width, source_scaled_height;
        zoomSurfaceSize(cases[i].width,
                        cases[i].height,
                        1.5,
                        0.75,
                        &source_scaled_width,
                        &source_scaled_height);
        SDL_Surface *scaled = SDL_ScaleSurface(source,
                                               source_scaled_width,
                                               source_scaled_height,
                                               SDL_SCALEMODE_NEAREST);
        SDL_Surface *scaled_reference = SDL_RotateSurface(scaled, (float)-cases[i].angle);
        TEST_CHECK(scaled_rotated != NULL && scaled_reference != NULL);
        TEST_CHECK(scaled_rotated->w == scaled_width && scaled_rotated->h == scaled_height);
        assert_surfaces_equal(scaled_rotated, scaled_reference);

        SDL_DestroySurface(scaled_reference);
        SDL_DestroySurface(scaled);
        SDL_DestroySurface(scaled_rotated);
        SDL_DestroySurface(reference);
        SDL_DestroySurface(rotated);
        SDL_DestroySurface(source);
    }
}

static void test_indexed_alpha_rotation(int zoom_filter) {
    SDL_Surface *source = create_indexed_alpha_surface();
    assert_rotation_transparency(source, zoom_filter);
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

    assert_rotation_transparency(source, ZOOM_FILTER_PIXELART);
    SDL_DestroySurface(source);
}

static void test_opaque_indexed_rotation(void) {
    SDL_Surface *source = create_indexed_alpha_surface();
    SDL_Palette *palette = SDL_GetSurfacePalette(source);
    TEST_CHECK(palette != NULL);
    SDL_Color transparent = palette->colors[0];
    transparent.a = SDL_ALPHA_OPAQUE;
    TEST_CHECK(SDL_SetPaletteColors(palette, &transparent, 0, 1));

    SDL_Surface *rotated =
        rotozoomSurface(source, -135.0, 1.0, ZOOM_FILTER_PIXELART);
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
    SDL_Surface *scaled =
        rotozoomSurface(source, 0.0, 1.0, ZOOM_FILTER_PIXELART);
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
            /* region_map_render_marker() passes the already-clockwise
             * direction through the compatibility inverse. */
            SDL_Surface *transformed = rotozoomSurface(converted,
                                                       -(direction * 45.0),
                                                       zooms[zoom],
                                                       ZOOM_FILTER_PIXELART);
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
    test_safe_pixel_access();
    test_legacy_texture_transparency();
    test_mutable_color_key_surface_reuse();
    test_indexed_alpha_rotation(ZOOM_FILTER_OFF);
    test_indexed_alpha_rotation(ZOOM_FILTER_PIXELART);
    test_authored_rotation_direction();
    test_authored_rotation_geometry();
    test_color_key_rotation();
    test_opaque_indexed_rotation();
    test_indexed_alpha_without_rotation();
    test_transform_invalid_input();
    test_true_color_alpha_rotation();
    test_darken_preserves_alpha();
    test_zoom_filter_modes();
    test_map_marker_rotation_contract();
    return 0;
}
