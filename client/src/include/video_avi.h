/*****************************************************************************
 * Copyright 2026 The Atrinik Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *****************************************************************************/

#ifndef VIDEO_AVI_H
#define VIDEO_AVI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct video_avi video_avi_t;

/**
 * Create an MJPEG AVI writer at the stream's current position.
 *
 * The stream must be seekable, binary, writable, and opened without append
 * mode. It remains owned by the caller. The writer exclusively owns the stream
 * position until it is freed. The returned writer is not thread-safe.
 */
video_avi_t *video_avi_open(FILE *stream, uint32_t width, uint32_t height, uint32_t fps);

/**
 * Add a JPEG at a presentation-frame index.
 *
 * The first index must be zero and later indices must increase. Gaps are
 * filled by repeating the previous JPEG. The writer copies the JPEG.
 */
bool video_avi_frame(video_avi_t *avi,
                     const unsigned char *jpeg,
                     size_t size,
                     uint32_t frame_index);

/**
 * Complete the AVI with exactly frame_count frames.
 *
 * If frame_count extends beyond the last supplied index, the last JPEG is
 * repeated. Calling this function more than once returns the original result.
 */
bool video_avi_finish(video_avi_t *avi, uint32_t frame_count);

/** Free writer-owned memory. This does not close or otherwise own the stream. */
void video_avi_free(video_avi_t *avi);

/** Return the first error recorded by the writer, or an empty string. */
const char *video_avi_error(const video_avi_t *avi);

/** Return the number of complete frames written to the AVI. */
uint32_t video_avi_frames(const video_avi_t *avi);

#endif
