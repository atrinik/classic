/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef RENDER_PROFILER_H
#define RENDER_PROFILER_H

#include <stdbool.h>
#include <stdint.h>

#define RENDER_PROFILER_STATISTICS_VERSION UINT8_C(4)

/** Timed portions of the client frame and map renderer. */
typedef enum render_profile_stage {
    RENDER_PROFILE_FRAME,
    RENDER_PROFILE_EVENTS,
    RENDER_PROFILE_GAME,
    RENDER_PROFILE_WIDGETS,
    RENDER_PROFILE_OVERLAYS,
    RENDER_PROFILE_MAINTENANCE,
    RENDER_PROFILE_PRESENT,
    RENDER_PROFILE_WAIT,
    RENDER_PROFILE_MAP,
    RENDER_PROFILE_MAP_SCRATCH_CLEAR,
    RENDER_PROFILE_MAP_GROUND,
    RENDER_PROFILE_MAP_GROUND_COMPOSITE,
    RENDER_PROFILE_LIGHTING,
    RENDER_PROFILE_MAP_OBJECTS,
    RENDER_PROFILE_MAP_PAINT,
    RENDER_PROFILE_MAP_COMMAND_SORT,
    RENDER_PROFILE_MAP_LIVING_OCCLUSION,
    RENDER_PROFILE_MAP_SPRITE_EFFECTS,
    RENDER_PROFILE_MAP_HINT_REPLAY,
    RENDER_PROFILE_MAP_UI,

    RENDER_PROFILE_STAGE_NUM
} render_profile_stage_t;

/** Aggregation scope for one profiler call count and elapsed-time total. */
typedef enum render_profile_scope {
    RENDER_PROFILE_SCOPE_FRAME,
    RENDER_PROFILE_SCOPE_MAP_DRAW,
    RENDER_PROFILE_SCOPE_MAP_LEVEL,
} render_profile_scope_t;

/** Stable machine-readable label and aggregation scope for one stage. */
typedef struct render_profile_stage_metadata {
    const char *name;
    render_profile_scope_t scope;
} render_profile_stage_metadata_t;

/** One completed sampling interval displayed by the profiler widget. */
typedef struct render_profile_snapshot {
    uint64_t elapsed_us[RENDER_PROFILE_STAGE_NUM];
    uint32_t calls[RENDER_PROFILE_STAGE_NUM];
    uint64_t interval_us;
    uint32_t frames;
    uint32_t drawn_frames;
    uint32_t generation;
} render_profile_snapshot_t;

void render_profiler_set_enabled(bool enabled);
uint64_t render_profiler_begin(void);
void render_profiler_end(render_profile_stage_t stage, uint64_t started_us);
void render_profiler_frame_finished(bool drawn);
const render_profile_snapshot_t *render_profiler_snapshot(void);
/** Reset the non-rotating counters used by deterministic benchmark phases. */
void render_profiler_statistics_reset(void);
/** Copy all counters accumulated since render_profiler_statistics_reset(). */
void render_profiler_statistics_get(render_profile_snapshot_t *statistics);
/** Copy stable metadata for a profiler stage; return false for an invalid stage. */
bool render_profiler_stage_metadata_get(render_profile_stage_t stage,
                                        render_profile_stage_metadata_t *metadata);
/** Return a stable machine-readable aggregation-scope label. */
const char *render_profiler_scope_name(render_profile_scope_t scope);
void widget_render_profiler_init(widgetdata *widget);

#endif
