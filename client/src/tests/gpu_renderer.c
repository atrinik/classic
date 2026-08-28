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

#include <global.h>

int main(void) {
    gpu_renderer_statistics_t statistics;

    HARD_ASSERT(!gpu_renderer_ready());
    HARD_ASSERT(strcmp(gpu_renderer_backend(), "") == 0);

    gpu_renderer_statistics_reset();
    gpu_renderer_statistics_commands(17, 5, 7);
    gpu_renderer_statistics_recovery(true);
    gpu_renderer_statistics_recovery(false);
    uint64_t started = gpu_renderer_timing_begin();
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMMAND_BUILD, started);
    gpu_renderer_statistics_get(&statistics);

    HARD_ASSERT(statistics.commands == 17);
    HARD_ASSERT(statistics.batches == 5);
    HARD_ASSERT(statistics.draws == 7);
    HARD_ASSERT(statistics.device_recoveries == 2);
    HARD_ASSERT(statistics.recovery_failures == 1);
    HARD_ASSERT(statistics.fallbacks == 0);
    HARD_ASSERT(statistics.timings[GPU_RENDERER_TIMING_COMMAND_BUILD].calls == 1);

    gpu_renderer_destroy();
    return 0;
}
