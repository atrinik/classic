/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Include the adapter so the test can reset its process-lifetime state. */
#include "../client/live_movement.c"

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

typedef struct fixture_paths {
    char route[64];
    char report[64];
} fixture_paths_t;

static uint64_t now_us;
static uint64_t publication_generation;
static uint64_t primary_gpu_generation;
static map_benchmark_statistics_t map_statistics;
static render_profile_snapshot_t profile_statistics;
static bool socket_active = true;
static bool shutdown_pending;
static bool map_transaction_active;
static unsigned int move_count, stop_count;
static int last_move;

Client_Player cpl;
_mapdata MapData;
SDL_Window *ScreenWindow;

uint64_t datetime_monotonic_us(void) { return now_us; }

bool client_socket_active(void) { return socket_active; }
bool client_socket_shutdown_pending(void) { return shutdown_pending; }
uint64_t socket_command_map_publication_generation(void) {
    return publication_generation;
}
bool map_state_transaction_active(void) { return map_transaction_active; }

void move_keys_stream(int direction, uint32_t epoch) {
    if (epoch != LIVE_MOVEMENT_EPOCH) abort();
    move_count++;
    last_move = direction;
}

void move_keys_stream_stop(uint32_t epoch) {
    if (epoch != LIVE_MOVEMENT_EPOCH) abort();
    stop_count++;
}

void render_profiler_set_enabled(bool enabled_value) { (void)enabled_value; }
void render_profiler_statistics_reset(void) { profile_statistics.frames = 0; }
void render_profiler_statistics_get(render_profile_snapshot_t *statistics) {
    *statistics = profile_statistics;
}
bool render_profiler_stage_metadata_get(render_profile_stage_t stage,
                                        render_profile_stage_metadata_t *metadata) {
    (void)stage;
    *metadata =
        (render_profile_stage_metadata_t){.name = "test", .scope = RENDER_PROFILE_SCOPE_FRAME};
    return true;
}

const char *gpu_renderer_backend(void) { return "test"; }
const char *gpu_renderer_device_name(void) { return "test-device"; }
const char *gpu_renderer_driver_name(void) { return "test-driver"; }
void gpu_renderer_statistics_get(gpu_renderer_statistics_t *statistics) {
    *statistics = (gpu_renderer_statistics_t){0};
}
uint64_t gpu_map_renderer_primary_publication_generation(void) {
    return primary_gpu_generation;
}
void map_benchmark_statistics_get(map_benchmark_statistics_t *statistics) {
    *statistics = map_statistics;
}
void client_command_queue_statistics_get(uint64_t timestamp,
                                         client_command_queue_statistics_t *statistics) {
    (void)timestamp;
    *statistics = (client_command_queue_statistics_t){0};
}
void image_face_statistics_get(image_face_statistics_t *statistics) {
    *statistics = (image_face_statistics_t){0};
}
bool telemetry_game_time_seconds(uint64_t *game_seconds) {
    *game_seconds = 0;
    return false;
}
int64_t setting_get_int(int category, int setting) {
    (void)category;
    (void)setting;
    return 0;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *width, int *height) {
    (void)window;
    *width = 800;
    *height = 600;
    return true;
}

static const char route_xml[] =
    "<live-movement-route version=\"1\" timeout-ms=\"10000\" step-timeout-ms=\"100\">"
    "<checkpoint map=\"/maps/start\" x=\"10\" y=\"10\" direction=\"0\"/>"
    "<checkpoint map=\"/maps/start\" x=\"11\" y=\"10\" direction=\"6\"/>"
    "</live-movement-route>";

static bool write_all(int fd, const char *value) {
    size_t offset = 0;
    size_t length = strlen(value);
    while (offset < length) {
        ssize_t written = write(fd, value + offset, length - offset);
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

static bool fixture_create(fixture_paths_t *paths, const char *xml) {
    strcpy(paths->route, "/tmp/atrinik-live-adapter-route-XXXXXX");
    strcpy(paths->report, "/tmp/atrinik-live-adapter-report-XXXXXX");
    int route_fd = mkstemp(paths->route);
    int report_fd = mkstemp(paths->report);
    if (route_fd < 0 || report_fd < 0) {
        if (route_fd >= 0) close(route_fd);
        if (report_fd >= 0) close(report_fd);
        unlink(paths->route);
        unlink(paths->report);
        return false;
    }
    bool valid = write_all(route_fd, xml);
    if (close(route_fd) != 0 || close(report_fd) != 0 || unlink(paths->report) != 0) {
        valid = false;
    }
    if (!valid) {
        unlink(paths->route);
        unlink(paths->report);
    }
    return valid;
}

static void fixture_destroy(const fixture_paths_t *paths) {
    unlink(paths->route);
    unlink(paths->report);
}

static void adapter_reset(void) {
    live_movement_close();
    route = NULL;
    route_state = NULL;
    report = NULL;
    enabled = ready = finished = succeeded = arrival_waiting = false;
    started_us = previous_frame_us = previous_service_us = service_gap_us = 0;
    max_service_gap_us = final_drain_started_us = final_drain_presented_frames = 0;
    frames = presented_frames = arrivals = presented_checkpoints = 0;
    arrival_map_draws = arrival_gpu_generation = previous_profile_frames = step_started_us = 0;
    arrival_index = 0;
    memset(arrival_map, 0, sizeof(arrival_map));
    arrival_x = arrival_y = 0;
    now_us = 1000;
    publication_generation = primary_gpu_generation = 0;
    map_statistics = (map_benchmark_statistics_t){0};
    profile_statistics = (render_profile_snapshot_t){0};
    socket_active = true;
    shutdown_pending = map_transaction_active = false;
    move_count = stop_count = 0;
    last_move = 0;
    cpl = (Client_Player){0};
    MapData = (_mapdata){0};
    cpl.state = ST_PLAY;
    strcpy(MapData.map_path, "/maps/start");
    MapData.posx = 10;
    MapData.posy = 10;
}

static bool fixture_ready(const fixture_paths_t *paths) {
    if (!live_movement_initialize(paths->route, paths->report)) return false;
    live_movement_ready();
    return true;
}

static void frame(bool presented) {
    profile_statistics.frames++;
    live_movement_frame_finished(presented, &(client_keepalive_statistics_t){0});
}

static void present_arrival(void) {
    map_statistics.primary_map_draws++;
    primary_gpu_generation++;
    frame(true);
}

static int test_initialize_and_abort(void) {
    fixture_paths_t paths;
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(live_movement_initialize(NULL, NULL));
    CHECK(!live_movement_enabled());
    CHECK(!live_movement_initialize(paths.route, NULL));
    CHECK(!live_movement_initialize("relative", paths.report));
#ifndef WIN32
    mode_t previous_umask = umask(0002);
    bool initialized = live_movement_initialize(paths.route, paths.report);
    umask(previous_umask);
    CHECK(initialized);
    struct stat report_status;
    CHECK(stat(paths.report, &report_status) == 0);
    CHECK((report_status.st_mode & 0777) == 0600);
#else
    CHECK(live_movement_initialize(paths.route, paths.report));
#endif
    CHECK(live_movement_enabled());
    CHECK(!live_movement_initialize(paths.route, paths.report));
    CHECK(live_movement_enabled());
    live_movement_ready();
    publication_generation = 1;
    live_movement_tick();
    CHECK(!live_movement_finished());
    live_movement_abort("renderer recreation or recovery interrupted the route");
    CHECK(live_movement_finished());
    CHECK(live_movement_exit_status() == 8);
    CHECK(stop_count == 1U);
    live_movement_close();
    live_movement_close();
    fixture_destroy(&paths);

    adapter_reset();
    CHECK(fixture_create(&paths, "<broken/>"));
    CHECK(!live_movement_initialize(paths.route, paths.report));
    CHECK(!live_movement_enabled());
    fixture_destroy(&paths);

    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    FILE *occupied = fopen(paths.report, "w");
    CHECK(occupied != NULL);
    CHECK(fclose(occupied) == 0);
    CHECK(!live_movement_initialize(paths.route, paths.report));
    fixture_destroy(&paths);

    return 0;
}

static int test_publication_and_movement(void) {
    fixture_paths_t paths;
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(fixture_ready(&paths));
    live_movement_tick();
    CHECK(!live_movement_finished() && move_count == 0U);
    publication_generation = 1;
    map_transaction_active = true;
    live_movement_tick();
    CHECK(!live_movement_finished() && move_count == 0U);
    map_transaction_active = false;
    MapData.continuation.pending = true;
    live_movement_tick();
    CHECK(!live_movement_finished() && move_count == 0U);
    MapData.continuation.pending = false;
    live_movement_tick();
    CHECK(!live_movement_finished() && move_count == 0U);

    frame(true);
    CHECK(presented_checkpoints == 0U);
    map_statistics.auxiliary_map_draws++;
    frame(true);
    CHECK(presented_checkpoints == 0U);
    map_statistics.primary_map_draws++;
    frame(false);
    CHECK(presented_checkpoints == 0U);
    primary_gpu_generation++;
    frame(true);
    CHECK(presented_checkpoints == 1U);

    live_movement_tick();
    CHECK(move_count == 1U && last_move == 6);
    live_movement_tick();
    CHECK(move_count == 1U);
    live_movement_close();
    fixture_destroy(&paths);
    return 0;
}

static int test_deadline_and_report_failure(void) {
    fixture_paths_t paths;
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(fixture_ready(&paths));
    publication_generation = 1;
    live_movement_tick();
    now_us = 101000;
    frame(true);
    CHECK(live_movement_finished() && live_movement_exit_status() == 8);
    live_movement_close();
    fixture_destroy(&paths);

#ifndef WIN32
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(fixture_ready(&paths));
    CHECK(fclose(report) == 0);
    report = fopen("/dev/full", "w");
    CHECK(report != NULL && setvbuf(report, NULL, _IONBF, 0) == 0);
    publication_generation = 1;
    profile_statistics.frames = 1;
    live_movement_frame_finished(true, &(client_keepalive_statistics_t){0});
    CHECK(live_movement_finished() && live_movement_exit_status() == 8);
    live_movement_close();
    fixture_destroy(&paths);
#endif
    return 0;
}

static int test_final_drain(void) {
    fixture_paths_t paths;
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(fixture_ready(&paths));
    publication_generation = 1;
    live_movement_tick();
    present_arrival();
    live_movement_tick();
    CHECK(move_count == 1U);
    MapData.posx = 11;
    publication_generation++;
    live_movement_tick();
    present_arrival();
    live_movement_tick();
    CHECK(final_drain_started_us == now_us && !live_movement_finished());

    strcpy(MapData.map_path, "/maps/staged");
    MapData.posx = 99;
    MapData.posy = 99;
    map_transaction_active = true;
    now_us += 999999;
    frame(true);
    live_movement_tick();
    CHECK(!live_movement_finished());
    map_transaction_active = false;
    strcpy(MapData.map_path, "/maps/start");
    MapData.posx = 11;
    MapData.posy = 10;
    now_us++;
    live_movement_tick();
    CHECK(live_movement_finished() && live_movement_exit_status() == 0);
    live_movement_close();
    fixture_destroy(&paths);
    return 0;
}

static int test_final_drain_disconnect(void) {
    fixture_paths_t paths;
    adapter_reset();
    CHECK(fixture_create(&paths, route_xml));
    CHECK(fixture_ready(&paths));
    publication_generation = 1;
    live_movement_tick();
    present_arrival();
    live_movement_tick();
    MapData.posx = 11;
    publication_generation++;
    live_movement_tick();
    present_arrival();
    live_movement_tick();
    socket_active = false;
    now_us += 1000000;
    frame(true);
    live_movement_tick();
    CHECK(live_movement_finished() && live_movement_exit_status() == 8);
    live_movement_close();
    fixture_destroy(&paths);
    return 0;
}

int main(void) {
    if (test_initialize_and_abort() != 0) return 1;
    if (test_publication_and_movement() != 0) return 1;
    if (test_deadline_and_report_failure() != 0) return 1;
    if (test_final_drain() != 0) return 1;
    if (test_final_drain_disconnect() != 0) return 1;
    adapter_reset();
    return 0;
}
