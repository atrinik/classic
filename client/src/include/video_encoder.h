/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

/**
 * Run the blocking stdin-to-MJPEG-AVI helper protocol. This process-private
 * entry point owns stdin/stdout until it returns and must run before client
 * toolkit, configuration, logging, or networking initialization. On POSIX it
 * closes inherited descriptors above stderr before doing other work. It is
 * neither thread-safe nor reentrant. Returns zero only after the exclusive
 * output file has been finalized and closed successfully.
 */
int video_encoder_main(const char *output_path);

#endif
