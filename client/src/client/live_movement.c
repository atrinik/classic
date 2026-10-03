/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <live_movement.h>
#include <live_movement_route.h>
#include <client.h>
#include <client_command_queue.h>
#include <client_socket.h>
#include <commands.h>
#include <event.h>
#include <gpu_renderer.h>
#include <gpu_map_renderer.h>
#include <image.h>
#include <main.h>
#include <map.h>
#include <player.h>
#include <render_profiler.h>
#include <settings.h>
#include <toolkit/datetime.h>
#include <cmake.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define LIVE_REPORT_MAX_BYTES (UINT64_C(127) * 1024 * 1024)
#define LIVE_REPORT_MAX_FRAMES UINT64_C(900000)
#define LIVE_FINAL_DRAIN_US UINT64_C(1000000)
#define LIVE_MOVEMENT_EPOCH UINT32_C(0x7fffffff)

static live_movement_route_t *route;
static live_movement_route_state_t *route_state;
static FILE *report;
static bool enabled, ready, finished, succeeded, arrival_waiting;
static uint64_t started_us, previous_frame_us, previous_service_us, service_gap_us;
static uint64_t started_utc_us;
static uint64_t max_service_gap_us, final_drain_started_us;
static uint64_t final_drain_presented_frames;
static uint64_t frames, presented_frames, arrivals, presented_checkpoints;
static uint64_t arrival_map_draws, arrival_gpu_generation;
static uint64_t previous_profile_frames;
static uint64_t step_started_us;
static size_t arrival_index;
static char arrival_map[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1];
static uint8_t arrival_x, arrival_y;

/** JSON strings are escaped even when they originate from driver metadata. */
static void json_string(const char *value) {
    fputc('"', report);
    for (const unsigned char *p = (const unsigned char *)(value != NULL ? value : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', report);
            fputc(*p, report);
        } else if (*p < 32) {
            fprintf(report, "\\u%04x", *p);
        } else {
            fputc(*p, report);
        }
    }
    fputc('"', report);
}

static uint64_t elapsed_us(void) {
    uint64_t now = datetime_monotonic_us();
    return now >= started_us ? now - started_us : 0;
}

static void terminal(bool success, const char *reason) {
    if (!enabled || finished) {
        return;
    }
    finished = true;
    succeeded = success;
    if (ready && client_socket_active()) {
        move_keys_stream_stop(LIVE_MOVEMENT_EPOCH);
    }
    fprintf(report,
            "{\"type\":\"terminal\",\"status\":\"%s\",\"reason\":",
            success ? "success" : "failure");
    json_string(reason);
    fprintf(report,
            ",\"arrivals\":%" PRIu64 ",\"presented_checkpoints\":%" PRIu64
            ",\"expected_checkpoints\":%zu,\"frames\":%" PRIu64 ",\"presented_frames\":%" PRIu64
            ",\"elapsed_us\":%" PRIu64 "}\n",
            arrivals,
            presented_checkpoints,
            live_movement_route_checkpoint_count(route),
            frames,
            presented_frames,
            elapsed_us());
    if (fflush(report) != 0 || ferror(report)) {
        succeeded = false;
    }
}

bool live_movement_initialize(const char *route_path, const char *report_path) {
    if (enabled) {
        fprintf(stderr, "live movement is already initialized\n");
        return false;
    }
    if (route_path == NULL && report_path == NULL) {
        return true;
    }
    char error[256];
    if (route_path == NULL || report_path == NULL || route_path[0] != '/' ||
        report_path[0] != '/') {
        fprintf(stderr, "live movement requires absolute route and report paths\n");
        return false;
    }
    started_us = datetime_monotonic_us();
    struct timespec utc;
    if (timespec_get(&utc, TIME_UTC) != TIME_UTC || utc.tv_sec < 0) {
        fprintf(stderr, "live movement cannot establish its UTC time anchor\n");
        return false;
    }
    started_utc_us = (uint64_t)utc.tv_sec * UINT64_C(1000000) + (uint64_t)utc.tv_nsec / 1000;
    if (!live_movement_route_load(route_path, &route, error, sizeof(error))) {
        fprintf(stderr, "live movement: %s\n", error);
        return false;
    }
    int output_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef WIN32
    output_flags |= O_BINARY;
#endif
    int output_fd = open(report_path, output_flags, 0600);
    report = output_fd >= 0 ? fdopen(output_fd, "wb") : NULL;
    if (report == NULL) {
        if (output_fd >= 0)
            close(output_fd);
        fprintf(stderr, "live movement cannot exclusively create its report\n");
        live_movement_route_free(route);
        route = NULL;
        return false;
    }
    route_state = live_movement_route_state_create(route, started_us / 1000);
    if (route_state == NULL) {
        fclose(report);
        report = NULL;
        live_movement_route_free(route);
        route = NULL;
        return false;
    }
    enabled = true;
    return true;
}

void live_movement_ready(void) {
    if (!enabled) {
        return;
    }
    render_profiler_set_enabled(true);
    render_profiler_statistics_reset();
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(ScreenWindow, &width, &height);
    fprintf(report,
            "{\"type\":\"identity\",\"schema_version\":1,\"started_utc_us\":%" PRIu64
            ",\"route_sha256\":",
            started_utc_us);
    json_string(live_movement_route_sha256(route));
    fprintf(report,
            ",\"route_checkpoints\":%zu,\"source_revision\":",
            live_movement_route_checkpoint_count(route));
    json_string(ATRINIK_BENCHMARK_REVISION);
    fprintf(report,
            ",\"source_dirty\":%s,\"gpu_backend\":",
            strcmp(ATRINIK_BENCHMARK_DIRTY, "false") == 0 ? "false" : "true");
    json_string(gpu_renderer_backend());
    fputs(",\"gpu_device\":", report);
    json_string(gpu_renderer_device_name());
    fputs(",\"gpu_driver\":", report);
    json_string(gpu_renderer_driver_name());
    static const int fps_limits[] = {30, 60, 120, 0};
    int64_t fps_index = setting_get_int(OPT_CAT_CLIENT, OPT_FPS_LIMIT);
    int fps_limit =
        fps_index >= 0 && fps_index < (int64_t)arraysize(fps_limits) ? fps_limits[fps_index] : -1;
    fprintf(report,
            ",\"viewport_width\":%d,\"viewport_height\":%d,\"look_width\":%" PRId64
            ",\"look_height\":%" PRId64 ",\"fps_limit\":%d"
            ",\"smooth_lighting\":%s"
            ",\"gpu_hardware_timing_available\":false,\"cpu_stage_names\":[",
            width,
            height,
            setting_get_int(OPT_CAT_MAP, OPT_MAP_WIDTH),
            setting_get_int(OPT_CAT_MAP, OPT_MAP_HEIGHT),
            fps_limit,
            setting_get_int(OPT_CAT_MAP, OPT_SMOOTH_LIGHTING) ? "true" : "false");
    for (size_t i = 0; i < RENDER_PROFILE_STAGE_NUM; i++) {
        render_profile_stage_metadata_t metadata;
        render_profiler_stage_metadata_get((render_profile_stage_t)i, &metadata);
        if (i != 0)
            fputc(',', report);
        json_string(metadata.name);
    }
    fputs("],\"gpu_host_stage_names\":[\"command_build\",\"albedo_owner\",\"light_tone\","
          "\"ui\",\"submission\",\"completion\",\"present_wait\"]}\n",
          report);
    ready = true;
    previous_frame_us = datetime_monotonic_us();
}

bool live_movement_enabled(void) {
    return enabled;
}
bool live_movement_finished(void) {
    return enabled && finished;
}
int live_movement_exit_status(void) {
    return enabled && !succeeded ? 8 : 0;
}
void live_movement_abort(const char *reason) {
    terminal(false, reason);
}

void live_movement_tick(void) {
    if (!enabled || !ready || finished)
        return;
    uint64_t now = datetime_monotonic_us();
    if (now < started_us || now - started_us >= live_movement_route_timeout_ms(route) * 1000) {
        terminal(false, "global route deadline exceeded");
        return;
    }
    if (step_started_us != 0 &&
        presented_checkpoints < live_movement_route_checkpoint_count(route) &&
        now - step_started_us >= live_movement_route_step_timeout_ms(route) * 1000) {
        terminal(false, "movement or presentation deadline exceeded");
        return;
    }
    bool map_ready = socket_command_map_publication_generation() != 0 &&
                     !map_state_transaction_active() && !MapData.continuation.pending;
    if (final_drain_started_us != 0 &&
        (!client_socket_active() || client_socket_shutdown_pending() || cpl.state != ST_PLAY ||
         (map_ready && (strcmp(MapData.map_path, arrival_map) != 0 || MapData.posx != arrival_x ||
                        MapData.posy != arrival_y)))) {
        terminal(false, "final observation changed or disconnected");
        return;
    }
    service_gap_us = previous_service_us != 0 ? now - previous_service_us : 0;
    if (service_gap_us > max_service_gap_us)
        max_service_gap_us = service_gap_us;
    previous_service_us = now;
    live_movement_route_observation_t observation = {
        .now_ms = now / 1000,
        .connected = client_socket_active() && !client_socket_shutdown_pending(),
        .play = cpl.state == ST_PLAY,
        .published_ready = map_ready,
        .map = MapData.map_path,
        .x = MapData.posx,
        .y = MapData.posy,
        .publication_generation = socket_command_map_publication_generation(),
        .run_on = cpl.run_on != 0,
        .fire_on = cpl.fire_on != 0,
    };
    live_movement_route_action_t action = live_movement_route_tick(route_state, &observation);
    if (action.type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED) {
        terminal(false, live_movement_route_failure(route_state));
    } else if (action.type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE) {
        move_keys_stream(action.direction, LIVE_MOVEMENT_EPOCH);
    } else if (action.type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL) {
        map_benchmark_statistics_t map_statistics;
        map_benchmark_statistics_get(&map_statistics);
        arrival_waiting = true;
        arrival_index = action.checkpoint_index;
        if (arrival_index == 0)
            step_started_us = now;
        arrival_map_draws = map_statistics.primary_map_draws;
        arrival_gpu_generation = gpu_map_renderer_primary_publication_generation();
        snprintf(arrival_map, sizeof(arrival_map), "%s", observation.map);
        arrival_x = observation.x;
        arrival_y = observation.y;
        arrivals++;
        fprintf(report, "{\"type\":\"arrival\",\"index\":%zu,\"map_path\":", arrival_index);
        json_string(observation.map);
        fprintf(report,
                ",\"x\":%u,\"y\":%u,\"publication_generation\":%" PRIu64 ",\"elapsed_us\":%" PRIu64
                "}\n",
                observation.x,
                observation.y,
                observation.publication_generation,
                elapsed_us());
    } else if (action.type == LIVE_MOVEMENT_ROUTE_ACTION_DONE) {
        if (final_drain_started_us == 0) {
            final_drain_started_us = now;
            final_drain_presented_frames = presented_frames;
            /* A static world still needs the independently observed final frame. */
            map_redraw_request(MAP_REDRAW_REASON_EXTERNAL);
        }
        if (now - final_drain_started_us >= LIVE_FINAL_DRAIN_US &&
            presented_frames > final_drain_presented_frames && observation.published_ready) {
            terminal(true, "route completed");
        }
    }
}

void live_movement_frame_finished(bool presented, const client_keepalive_statistics_t *keepalive) {
    if (!enabled || !ready || finished)
        return;
    uint64_t now = datetime_monotonic_us();
    if (step_started_us != 0 &&
        presented_checkpoints < live_movement_route_checkpoint_count(route) &&
        now - step_started_us >= live_movement_route_step_timeout_ms(route) * 1000) {
        terminal(false, "movement or presentation deadline exceeded");
        return;
    }
    render_profile_snapshot_t cpu;
    map_benchmark_statistics_t map_statistics;
    gpu_renderer_statistics_t gpu;
    client_command_queue_statistics_t queue;
    image_face_statistics_t assets;
    render_profiler_statistics_get(&cpu);
    map_benchmark_statistics_get(&map_statistics);
    gpu_renderer_statistics_get(&gpu);
    client_command_queue_statistics_get(now, &queue);
    image_face_statistics_get(&assets);
    if (cpu.frames <= previous_profile_frames || now < previous_frame_us) {
        terminal(false, "profiler reset or clock regression");
        return;
    }
    previous_profile_frames = cpu.frames;
    bool map_ready = socket_command_map_publication_generation() != 0 &&
                     !map_state_transaction_active() && !MapData.continuation.pending;
    uint64_t primary_gpu_generation = gpu_map_renderer_primary_publication_generation();
    frames++;
    presented_frames += presented;
    fprintf(report,
            "{\"type\":\"frame\",\"sequence\":%" PRIu64 ",\"elapsed_us\":%" PRIu64
            ",\"frame_us\":%" PRIu64 ",\"presented\":%s,\"map_path\":",
            frames,
            now - started_us,
            now - previous_frame_us,
            presented ? "true" : "false");
    previous_frame_us = now;
    json_string(MapData.map_path);
    fprintf(report,
            ",\"x\":%d,\"y\":%d,\"map_publication_generation\":%" PRIu64
            ",\"map_ready\":%s,\"cpu_totals_us\":[",
            MapData.posx,
            MapData.posy,
            socket_command_map_publication_generation(),
            map_ready ? "true" : "false");
    for (size_t i = 0; i < RENDER_PROFILE_STAGE_NUM; i++)
        fprintf(report, "%s%" PRIu64, i != 0 ? "," : "", cpu.elapsed_us[i]);
    fputs("],\"gpu_host_totals_ns\":[", report);
    for (size_t i = 0; i < GPU_RENDERER_TIMING_NUM; i++)
        fprintf(report, "%s%" PRIu64, i != 0 ? "," : "", gpu.timings[i].elapsed_ns);
    fputs("],\"map_totals\":{", report);
#define MAP_TOTAL(field) fprintf(report, "\"" #field "\":%" PRIu64 ",", map_statistics.field)
    MAP_TOTAL(primary_map_draws);
    MAP_TOTAL(compiled_render_commands);
    MAP_TOTAL(reused_render_commands);
    MAP_TOTAL(living_commands);
    MAP_TOTAL(animation_draws);
    MAP_TOTAL(level_draws);
#undef MAP_TOTAL
    fprintf(report,
            "\"render_commands\":%" PRIu64 "},\"gpu_totals\":{",
            map_statistics.render_commands);
#define GPU_TOTAL(field) fprintf(report, "\"" #field "\":%" PRIu64 ",", gpu.field)
    GPU_TOTAL(map_submissions);
    GPU_TOTAL(map_completions);
    GPU_TOTAL(map_dropped_updates);
    GPU_TOTAL(map_merged_updates);
    GPU_TOTAL(map_full_redraws);
    GPU_TOTAL(map_retained_frames);
    GPU_TOTAL(source_upload_count);
    GPU_TOTAL(source_upload_bytes);
    GPU_TOTAL(instance_upload_count);
    GPU_TOTAL(instance_upload_bytes);
    GPU_TOTAL(map_queue_age_total_ns);
    GPU_TOTAL(map_frame_latency_total_ns);
#undef GPU_TOTAL
    fprintf(report,
            "\"resource_creations\":%" PRIu64 "},"
            "\"gpu_primary_publication_generation\":%" PRIu64 ",\"network\":{"
            "\"connected\":%s,\"shutdown_pending\":%s,\"main_service_gap_us\":%" PRIu64
            ",\"main_service_gap_max_us\":%" PRIu64 ",\"queue_depth\":%" PRIu64
            ",\"queue_oldest_age_us\":%" PRIu64 ",\"queue_processing_total_us\":%" PRIu64
            ",\"queue_budget_yields_total\":%" PRIu64 ",\"keepalive_tx_total\":%" PRIu64
            ",\"keepalive_rx_total\":%" PRIu64 ",\"keepalive_timeout_total\":%" PRIu64
            ",\"keepalive_last_rtt_us\":%" PRIu64 "},\"assets\":{"
            "\"installed_total\":%" PRIu64 ",\"pending\":%zu,\"admitted\":%zu,"
            "\"unprepared\":%zu},\"gpu_invalidation_totals\":[",
            gpu.resource_creations,
            primary_gpu_generation,
            client_socket_active() ? "true" : "false",
            client_socket_shutdown_pending() ? "true" : "false",
            service_gap_us,
            max_service_gap_us,
            queue.depth,
            queue.current_oldest_age_us,
            queue.processing_us,
            queue.budget_yields,
            keepalive->tx,
            keepalive->rx,
            keepalive->timed_out,
            keepalive->last_rtt_us,
            assets.installed_total,
            assets.pending,
            assets.admitted,
            assets.unprepared);
    for (size_t i = 0; i < GPU_RENDERER_MAP_INVALIDATION_REASON_NUM; i++)
        fprintf(report, "%s%" PRIu64, i != 0 ? "," : "", gpu.map_invalidation_counts[i]);
    uint64_t game_seconds = 0;
    bool time_valid = telemetry_game_time_seconds(&game_seconds);
    fprintf(report,
            "],\"world_time\":{\"valid\":%s,\"game_seconds\":%" PRIu64
            ",\"light_keyframe_valid\":%s,\"light_keyframe_generation\":%" PRIu64 "}}\n",
            time_valid ? "true" : "false",
            game_seconds,
            MapData.light_keyframe_valid ? "true" : "false",
            MapData.light_keyframe_generation);
    if (ferror(report) || ftell(report) < 0 || (uint64_t)ftell(report) > LIVE_REPORT_MAX_BYTES ||
        frames >= LIVE_REPORT_MAX_FRAMES) {
        terminal(false, "report write failed or report limit exceeded");
        return;
    }
    if (arrival_waiting && presented && map_ready &&
        map_statistics.primary_map_draws > arrival_map_draws &&
        primary_gpu_generation > arrival_gpu_generation &&
        strcmp(MapData.map_path, arrival_map) == 0 && MapData.posx == arrival_x &&
        MapData.posy == arrival_y) {
        if (!live_movement_route_arrival_presented(route_state)) {
            terminal(false, "arrival presentation state mismatch");
            return;
        }
        arrival_waiting = false;
        presented_checkpoints++;
        step_started_us = now;
        fprintf(report,
                "{\"type\":\"checkpoint_presented\",\"index\":%zu,"
                "\"map_publication_generation\":%" PRIu64 ",\"gpu_published_generation\":%" PRIu64
                ",\"elapsed_us\":%" PRIu64 "}\n",
                arrival_index,
                socket_command_map_publication_generation(),
                primary_gpu_generation,
                elapsed_us());
    }
}

void live_movement_close(void) {
    if (!enabled || report == NULL)
        return;
    if (!finished)
        terminal(false, "client exited before completing route");
    if (fclose(report) != 0)
        succeeded = false;
    report = NULL;
    live_movement_route_state_free(route_state);
    route_state = NULL;
    live_movement_route_free(route);
    route = NULL;
}
