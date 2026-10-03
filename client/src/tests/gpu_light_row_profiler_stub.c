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

/* The GPU integration executable does not link the profiler's widget provider.
 * Production links the real provider; this test only checks renderer output. */
#include <render_profiler.h>

uint64_t render_profiler_begin(void) {
    return 0;
}

void render_profiler_end(render_profile_stage_t stage, uint64_t started_us) {
    (void)stage;
    (void)started_us;
}
