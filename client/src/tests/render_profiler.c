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

#include <global.h>

#define TEST_CHECK(condition) \
    do {                       \
        if (!(condition)) {    \
            abort();          \
        }                      \
    } while (0)

char *render_profiler_widget_text_for_test(const render_profile_snapshot_t *snapshot);

static void test_stage_metadata_is_rendered(void) {
    render_profile_snapshot_t snapshot = {
        .interval_us = UINT64_C(2000000),
        .frames = 20,
        .drawn_frames = 18,
    };

    for (render_profile_stage_t stage = 0; stage < RENDER_PROFILE_STAGE_NUM; stage++) {
        snapshot.elapsed_us[stage] = (stage + 1) * UINT64_C(1000);
        snapshot.calls[stage] = stage + 1;
    }

    char *text = render_profiler_widget_text_for_test(&snapshot);
    TEST_CHECK(strstr(text, "[c=#ffd060]Stage breakdown[/c]") != NULL);

    for (render_profile_stage_t stage = 0; stage < RENDER_PROFILE_STAGE_NUM; stage++) {
        render_profile_stage_metadata_t metadata = {0};
        TEST_CHECK(render_profiler_stage_metadata_get(stage, &metadata));
        TEST_CHECK(strstr(text, metadata.name) != NULL);
        TEST_CHECK(strstr(text, render_profiler_scope_name(metadata.scope)) != NULL);
    }

    free(text);
}

static void test_zero_interval_and_calls_are_safe(void) {
    render_profile_snapshot_t snapshot = {0};
    char *text = render_profiler_widget_text_for_test(&snapshot);

    TEST_CHECK(strstr(text, "frame [per_frame] 0.00 ms, 0 calls, 0.0/s") != NULL);
    TEST_CHECK(render_profiler_scope_name((render_profile_scope_t)-1) == NULL);
    TEST_CHECK(!render_profiler_stage_metadata_get(-1, NULL));
    TEST_CHECK(!render_profiler_stage_metadata_get(RENDER_PROFILE_STAGE_NUM, NULL));
    TEST_CHECK(!render_profiler_stage_metadata_get(RENDER_PROFILE_FRAME, NULL));
    free(text);
}

int main(void) {
    test_stage_metadata_is_rendered();
    test_zero_interval_and_calls_are_safe();
    return 0;
}
