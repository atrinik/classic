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

#ifndef IMAGE_CODEC_H
#define IMAGE_CODEC_H

#include <stddef.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

/**
 * Enable serialization while image decoding may run on the face worker.
 * Start/stop are main-thread lifecycle operations; stop follows worker join.
 */
bool image_codec_parallel_start(void);
void image_codec_parallel_stop(void);

SDL_Surface *image_codec_load(const char *path);
SDL_Surface *image_codec_load_io(SDL_IOStream *stream, bool close_stream);
SDL_Surface *image_codec_load_png_io(SDL_IOStream *stream);
/** Decode a bounded face while yielding the codec to any waiting main-thread caller. */
SDL_Surface *image_codec_load_png_io_background(SDL_IOStream *stream);
bool image_codec_save_png(SDL_Surface *surface, const char *path);
bool image_codec_save_png_io(SDL_Surface *surface, SDL_IOStream *stream, bool close_stream);

#ifdef ATRINIK_IMAGE_CODEC_TESTING
bool image_codec_test_enter(bool background);
void image_codec_test_leave(bool background);
void image_codec_test_wait_for_waiters(size_t main_waiters, size_t background_waiters);
#endif

#endif
