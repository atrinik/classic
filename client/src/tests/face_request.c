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

/** @file Bounded local decoding, priority, retry, and face-backlog regressions. */

#include <global.h>
#include <face_cache.h>
#include <face_loader.h>
#include <image_codec.h>
#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <wrapper.h>

#define TEST_WARM_FACE 1U
#define TEST_PACK_FACE 2U
#define TEST_FACE_FIRST 3U
#define TEST_FACE_COUNT (ASSET_REQUEST_PENDING_MAX + 6U)
#define TEST_BACKPRESSURE_FACE (TEST_FACE_FIRST + TEST_FACE_COUNT)
#define TEST_FAILURE_FACE (TEST_BACKPRESSURE_FACE + 1U)
#define TEST_CLEAR_FACE (TEST_FAILURE_FACE + 1U)
#define TEST_OCCUPIED_FACE_FIRST (TEST_CLEAR_FACE + 1U)
#define TEST_OCCUPIED_FACE_COUNT ASSET_STREAM_ACTIVE_MAX
#define TEST_OCCUPIED_LOCAL_FACE (TEST_OCCUPIED_FACE_FIRST + TEST_OCCUPIED_FACE_COUNT)
#define TEST_BUDGET_FACE_FIRST (TEST_OCCUPIED_LOCAL_FACE + 1U)
#define TEST_BUDGET_FACE_COUNT ASSET_STREAM_ACTIVE_MAX
#define TEST_MALFORMED_FACE (TEST_BUDGET_FACE_FIRST + TEST_BUDGET_FACE_COUNT)
#define TEST_EMPTY_FACE (TEST_MALFORMED_FACE + 1U)
#define TEST_OFFLINE_FOREGROUND_FACE (TEST_EMPTY_FACE + 1U)
#define TEST_OFFLINE_BACKGROUND_FACE (TEST_OFFLINE_FOREGROUND_FACE + 1U)
#define TEST_CURRENT_GFX_FACE (TEST_OFFLINE_BACKGROUND_FACE + 1U)
#define TEST_INSTALLED_GFX_FACE (TEST_CURRENT_GFX_FACE + 1U)
#define TEST_USER_GFX_FACE (TEST_INSTALLED_GFX_FACE + 1U)
#define TEST_BACKGROUND_DEPTH_FIRST (TEST_USER_GFX_FACE + 1U)
#define TEST_BACKGROUND_DEPTH_COUNT 2U
#define TEST_RECENCY_FACE_FIRST (TEST_BACKGROUND_DEPTH_FIRST + TEST_BACKGROUND_DEPTH_COUNT)
#define TEST_RECENCY_FACE_COUNT 6U
#define TEST_FACE_LAST (TEST_RECENCY_FACE_FIRST + TEST_RECENCY_FACE_COUNT - 1U)
#define TEST_ASYNC_TIMEOUT_MS 10000U
#define TEST_PREEMPTION_REPETITIONS 5U

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

struct asset_request {
    uint16_t face;
    asset_request_state_t state;
};

_face_struct FaceList[MAX_FACE_TILES];
Client_Player cpl;
SDL_Surface *OfflineRenderSurface;
int map_redraw_flag;
int minimap_redraw_flag;

void map_redraw_request(map_redraw_reason_t reason) {
    TEST_CHECK(reason != 0);
    map_redraw_flag = 1;
}

static char test_directory[HUGE_BUF];
static char test_original_directory[HUGE_BUF];
static char test_cache_directory[HUGE_BUF];
static char test_data_directory[HUGE_BUF];
static char test_user_gfx_directory[HUGE_BUF];
static char test_current_gfx_directory[HUGE_BUF];
static char test_installed_directory[HUGE_BUF];
static char test_installed_gfx_directory[HUGE_BUF];
static char test_pack_path[HUGE_BUF];
static char test_warm_cache[HUGE_BUF];
static char test_occupied_warm_cache[HUGE_BUF];
static char test_offline_warm_cache[HUGE_BUF];
static char test_current_gfx_face[HUGE_BUF];
static char test_installed_gfx_face[HUGE_BUF];
static char test_user_precedence_faces[3][HUGE_BUF];
static uint8_t *test_png;
static uint8_t *test_user_png;
static uint8_t *test_malformed_png;
static size_t test_png_size;
static size_t test_user_png_size;
static uint32_t test_png_crc;
static uint32_t test_user_png_crc;
static uint32_t test_malformed_png_crc;
static size_t test_file_path_calls;
static size_t test_cache_enqueues;
static size_t test_asset_active;
static size_t test_asset_start_attempts;
static size_t test_asset_start_count;
static size_t test_asset_priority_start_count;
static size_t test_asset_preempt_count;
static size_t test_asset_state_polls;
static uint16_t test_asset_attempt_faces[MAX_FACE_TILES];
static uint16_t test_asset_start_faces[MAX_FACE_TILES];
static asset_request_t *test_assets[MAX_FACE_TILES];
static bool test_asset_admission_blocked;
static bool test_asset_transport_available = true;
static bool test_asset_face_batch_available;
static bool test_asset_preemption_required;
static bool test_asset_replacement_progressed;
static size_t test_book_redraws;
static size_t test_interface_redraws;
static size_t test_widget_redraws;
static size_t test_active_effect_redraws;

static FILE *test_path_fopen(const char *path, const char *mode) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(mode != NULL);
    if (strcmp(path, FILE_ATRINIK_P0) == 0) {
        TEST_CHECK(strcmp(mode, "rb") == 0);
        return fopen(test_pack_path, mode);
    }
    TEST_CHECK(strncmp(path, DIRECTORY_GFX_USER "/", sizeof(DIRECTORY_GFX_USER)) == 0);
    TEST_CHECK(strcmp(mode, "rb") == 0);
    return NULL;
}

char *file_path(const char *path, const char *mode) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(path[0] != '/');
    TEST_CHECK(mode != NULL);
    TEST_CHECK(strcmp(mode, "wb") == 0 || strcmp(mode, "rb") == 0);
    test_file_path_calls++;
    if (strcmp(path, DIRECTORY_GFX_USER "/.face-loader-root") == 0) {
        return path_join(test_user_gfx_directory, ".face-loader-root");
    }
    return path_join(test_directory, path);
}

void get_data_dir_file(char *buffer, size_t size, const char *path) {
    TEST_CHECK(buffer != NULL);
    TEST_CHECK(path != NULL);
    int length = snprintf(buffer, size, "%s/%s", test_installed_directory, path);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < size);
}

FILE *server_file_open_name(const char *name) {
    TEST_CHECK(name != NULL);
    TEST_CHECK(strcmp(name, SERVER_FILE_BMAPS) == 0);

    FILE *fp = tmpfile();
    TEST_CHECK(fp != NULL);
    for (size_t face = 0; face <= TEST_FACE_LAST; face++) {
        TEST_CHECK(fprintf(fp,
                           "%x %x face-%03" PRIu64 "\n",
                           (unsigned int)test_png_size,
                           test_png_crc,
                           (uint64_t)face) > 0);
    }
    rewind(fp);
    return fp;
}

int64_t setting_get_int(int category, int setting) {
    (void)category;
    (void)setting;
    return 0;
}

bool face_cache_start(void) {
    return true;
}

void face_cache_enqueue(const char *name, const uint8_t *data, size_t size) {
    TEST_CHECK(name != NULL);
    TEST_CHECK(data != NULL);
    TEST_CHECK(size == test_png_size);
    TEST_CHECK(memcmp(data, test_png, size) == 0);
    test_cache_enqueues++;
}

void face_cache_report_failures(void) {}

void face_cache_stop(void) {}

bool asset_requests_available(void) {
    return test_asset_transport_available;
}

bool asset_face_batch_available(void) {
    return test_asset_face_batch_available;
}

static asset_request_t *test_asset_request_start(const char *path, size_t max_size, bool priority) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(max_size == ASSET_FACE_MAX_SIZE);
    TEST_CHECK(test_asset_start_attempts < arraysize(test_asset_attempt_faces));
    uint16_t face = 0;
    TEST_CHECK(socket_asset_face_path_parse(path, &face));
    test_asset_attempt_faces[test_asset_start_attempts++] = face;
    if (test_asset_admission_blocked) {
        return NULL;
    }
    size_t admission_limit = test_asset_face_batch_available
                                 ? ASSET_STREAM_ACTIVE_MAX * ASSET_FACE_BATCH_MAX
                                 : ASSET_STREAM_ACTIVE_MAX;
    TEST_CHECK(test_asset_active < admission_limit ||
               (priority && test_asset_active == admission_limit));
    TEST_CHECK(test_asset_start_count < MAX_FACE_TILES);

    TEST_CHECK(face >= TEST_FACE_FIRST);
    TEST_CHECK(face <= TEST_FACE_LAST);
    TEST_CHECK(test_assets[face] == NULL);

    asset_request_t *request = xcalloc(1, sizeof(*request));
    request->face = face;
    request->state = ASSET_REQUEST_PENDING;
    test_assets[face] = request;
    test_asset_start_faces[test_asset_start_count++] = face;
    test_asset_priority_start_count += priority;
    test_asset_active++;
    return request;
}

asset_request_t *asset_request_start_bounded(const char *path, size_t max_size) {
    return test_asset_request_start(path, max_size, false);
}

asset_request_t *asset_request_start_bounded_priority(const char *path, size_t max_size) {
    return test_asset_request_start(path, max_size, true);
}

bool asset_request_preemption_needed(const asset_request_t *replacement) {
    TEST_CHECK(replacement != NULL);
    return test_asset_preemption_required;
}

bool asset_request_preempt(asset_request_t *victim,
                           asset_request_t *replacement,
                           bool *displace_victim) {
    TEST_CHECK(victim != NULL);
    TEST_CHECK(replacement != NULL);
    TEST_CHECK(displace_victim != NULL);
    *displace_victim = false;
    TEST_CHECK(victim != replacement);
    TEST_CHECK(replacement->state == ASSET_REQUEST_PENDING ||
               replacement->state == ASSET_REQUEST_COMPLETE);
    TEST_CHECK(victim->state == ASSET_REQUEST_PENDING || victim->state == ASSET_REQUEST_COMPLETE);
    test_asset_preempt_count++;
    if (test_asset_replacement_progressed) {
        return true;
    }
    if (victim->state == ASSET_REQUEST_COMPLETE) {
        *displace_victim = true;
        return true;
    }

    *displace_victim = true;
    if (!test_asset_face_batch_available) {
        victim->state = ASSET_REQUEST_PREEMPTED;
        return true;
    }
    size_t victim_index = test_asset_start_count;
    for (size_t i = 0; i < test_asset_start_count; i++) {
        if (test_asset_start_faces[i] == victim->face) {
            victim_index = i;
            break;
        }
    }
    TEST_CHECK(victim_index < test_asset_start_count);
    size_t batch_first = victim_index / ASSET_FACE_BATCH_MAX * ASSET_FACE_BATCH_MAX;
    size_t batch_last = MIN(batch_first + ASSET_FACE_BATCH_MAX, test_asset_start_count);
    for (size_t i = batch_first; i < batch_last; i++) {
        asset_request_t *member = test_assets[test_asset_start_faces[i]];
        if (member != NULL && member != replacement && member->state == ASSET_REQUEST_PENDING) {
            member->state = ASSET_REQUEST_PREEMPTED;
        }
    }
    return true;
}

asset_request_state_t asset_request_get_state(asset_request_t *request) {
    TEST_CHECK(request != NULL);
    test_asset_state_polls++;
    return request->state;
}

const uint8_t *asset_request_get_data(const asset_request_t *request, size_t *size) {
    TEST_CHECK(request != NULL);
    TEST_CHECK(request->state == ASSET_REQUEST_COMPLETE);
    if (request->face == TEST_EMPTY_FACE) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = test_png_size;
    }
    return request->face == TEST_MALFORMED_FACE ? test_malformed_png : test_png;
}

void asset_request_free(asset_request_t *request) {
    if (request == NULL) {
        return;
    }
    TEST_CHECK(request->face < MAX_FACE_TILES);
    TEST_CHECK(test_assets[request->face] == request);
    TEST_CHECK(test_asset_active != 0);
    test_assets[request->face] = NULL;
    test_asset_active--;
    free(request);
}

sprite_struct *sprite_tryload_file(char *filename, uint32_t flags, SDL_IOStream *stream) {
    TEST_CHECK(filename == NULL);
    TEST_CHECK(flags == 0);
    TEST_CHECK(stream != NULL);
    SDL_Surface *surface = image_codec_load_png_io(stream);
    if (surface == NULL) {
        return NULL;
    }
    return sprite_from_surface(surface, flags, true);
}

sprite_struct *sprite_from_surface(SDL_Surface *surface, uint32_t flags, bool enable_rle) {
    TEST_CHECK(surface != NULL);
    TEST_CHECK(flags == 0);
    if (enable_rle) {
        TEST_CHECK(SDL_SetSurfaceRLE(surface, true));
    }
    sprite_struct *sprite = xcalloc(1, sizeof(*sprite));
    sprite->bitmap = surface;
    return sprite;
}

void sprite_free_sprite(sprite_struct *sprite) {
    if (sprite == NULL) {
        return;
    }
    SDL_DestroySurface(sprite->bitmap);
    free(sprite);
}

void sprite_free_rendered(sprite_struct *sprite) {
    sprite_free_sprite(sprite);
}

void sprite_cache_free_all(void) {}

void book_redraw(void) {
    test_book_redraws++;
}

void interface_redraw(void) {
    test_interface_redraws++;
}

void widget_redraw_all(int widget_type_id) {
    test_widget_redraws++;
    if (widget_type_id == ACTIVE_EFFECTS_ID) {
        test_active_effect_redraws++;
    }
}

static void test_png_load(void) {
    char source[HUGE_BUF];
    int length = snprintf(VS(source), "%s/textures/loading_off.png", ATRINIK_TEST_SOURCE_DIR);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(source));

    FILE *fp = fopen(source, "rb");
    TEST_CHECK(fp != NULL);
    struct stat info;
    TEST_CHECK(fstat(fileno(fp), &info) == 0);
    TEST_CHECK(S_ISREG(info.st_mode));
    TEST_CHECK(info.st_size > 0);
    TEST_CHECK((uint64_t)info.st_size <= ASSET_FACE_MAX_SIZE);

    test_png_size = (size_t)info.st_size;
    test_png = xmalloc(test_png_size);
    TEST_CHECK(fread(test_png, 1, test_png_size, fp) == test_png_size);
    TEST_CHECK(fclose(fp) == 0);
    test_png_crc = (uint32_t)crc32(1L, test_png, test_png_size);

    static const uint8_t text[] = {'s', 'o', 'u', 'r', 'c', 'e', 0, 'u', 's', 'e', 'r'};
    TEST_CHECK(test_png_size >= 12U);
    TEST_CHECK(memcmp(test_png + test_png_size - 8U, "IEND", 4U) == 0);
    test_user_png_size = test_png_size + sizeof(text) + 12U;
    TEST_CHECK(test_user_png_size <= ASSET_FACE_MAX_SIZE);
    test_user_png = xmalloc(test_user_png_size);
    size_t prefix = test_png_size - 12U;
    memcpy(test_user_png, test_png, prefix);
    uint8_t *chunk = test_user_png + prefix;
    chunk[0] = (uint8_t)(sizeof(text) >> 24U);
    chunk[1] = (uint8_t)(sizeof(text) >> 16U);
    chunk[2] = (uint8_t)(sizeof(text) >> 8U);
    chunk[3] = (uint8_t)sizeof(text);
    memcpy(chunk + 4U, "tEXt", 4U);
    memcpy(chunk + 8U, text, sizeof(text));
    uint32_t text_crc = (uint32_t)crc32(0L, chunk + 4U, (uInt)(sizeof(text) + 4U));
    chunk[8U + sizeof(text)] = (uint8_t)(text_crc >> 24U);
    chunk[9U + sizeof(text)] = (uint8_t)(text_crc >> 16U);
    chunk[10U + sizeof(text)] = (uint8_t)(text_crc >> 8U);
    chunk[11U + sizeof(text)] = (uint8_t)text_crc;
    memcpy(chunk + 12U + sizeof(text), test_png + prefix, 12U);
    test_user_png_crc = (uint32_t)crc32(1L, test_user_png, test_user_png_size);

    test_malformed_png = xmalloc(test_png_size);
    memcpy(test_malformed_png, test_png, test_png_size);
    bool corrupted = false;
    for (size_t offset = 8; offset + 12U <= test_png_size;) {
        uint32_t chunk_size = ((uint32_t)test_png[offset] << 24U) |
                              ((uint32_t)test_png[offset + 1U] << 16U) |
                              ((uint32_t)test_png[offset + 2U] << 8U) | test_png[offset + 3U];
        if ((uint64_t)offset + chunk_size + 12U > test_png_size) {
            break;
        }
        if (memcmp(test_png + offset + 4U, "IDAT", 4U) == 0) {
            test_malformed_png[offset + 8U + chunk_size] ^= 0x80U;
            corrupted = true;
            break;
        }
        offset += (size_t)chunk_size + 12U;
    }
    TEST_CHECK(corrupted);
    test_malformed_png_crc = (uint32_t)crc32(1L, test_malformed_png, test_png_size);
}

static void test_png_write_data(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    TEST_CHECK(fp != NULL);
    TEST_CHECK(fwrite(data, 1, size, fp) == size);
    TEST_CHECK(fclose(fp) == 0);
}

static void test_png_write(const char *path) {
    test_png_write_data(path, test_png, test_png_size);
}

static void test_cache_prepare(void) {
    TEST_CHECK(getcwd(VS(test_original_directory)) != NULL);
    snprintf(VS(test_directory), "/tmp/atrinik-face-request-XXXXXX");
    TEST_CHECK(mkdtemp(test_directory) != NULL);

    int length = snprintf(VS(test_cache_directory), "%s/%s", test_directory, DIRECTORY_CACHE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_cache_directory));
    TEST_CHECK(mkdir(test_cache_directory, 0700) == 0);

    length = snprintf(VS(test_data_directory), "%s/data", test_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_data_directory));
    TEST_CHECK(mkdir(test_data_directory, 0700) == 0);

    length = snprintf(VS(test_user_gfx_directory), "%s/user-gfx", test_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_user_gfx_directory));
    TEST_CHECK(mkdir(test_user_gfx_directory, 0700) == 0);

    length = snprintf(VS(test_current_gfx_directory), "%s/%s", test_directory, DIRECTORY_GFX_USER);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_current_gfx_directory));
    TEST_CHECK(mkdir(test_current_gfx_directory, 0700) == 0);

    length = snprintf(VS(test_installed_directory), "%s/installed", test_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_installed_directory));
    TEST_CHECK(mkdir(test_installed_directory, 0700) == 0);
    length = snprintf(VS(test_installed_gfx_directory),
                      "%s/%s",
                      test_installed_directory,
                      DIRECTORY_GFX_USER);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_installed_gfx_directory));
    TEST_CHECK(mkdir(test_installed_gfx_directory, 0700) == 0);

    length = snprintf(VS(test_pack_path), "%s/atrinik.p0", test_data_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_pack_path));
    FILE *fp = fopen(test_pack_path, "wb");
    TEST_CHECK(fp != NULL);
    TEST_CHECK(fprintf(fp,
                       "IMAGE %u %" PRIu64 " face-%03u\n",
                       TEST_PACK_FACE,
                       (uint64_t)test_png_size,
                       TEST_PACK_FACE) > 0);
    TEST_CHECK(fwrite(test_png, 1, test_png_size, fp) == test_png_size);
    TEST_CHECK(fclose(fp) == 0);

    length =
        snprintf(VS(test_warm_cache), "%s/face-%03u.png", test_cache_directory, TEST_WARM_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_warm_cache));
    test_png_write(test_warm_cache);

    length = snprintf(VS(test_occupied_warm_cache),
                      "%s/face-%03u.png",
                      test_cache_directory,
                      TEST_OCCUPIED_LOCAL_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_occupied_warm_cache));
    test_png_write(test_occupied_warm_cache);

    length = snprintf(VS(test_offline_warm_cache),
                      "%s/face-%03u.png",
                      test_cache_directory,
                      TEST_OFFLINE_BACKGROUND_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_offline_warm_cache));
    test_png_write(test_offline_warm_cache);

    length = snprintf(VS(test_current_gfx_face),
                      "%s/face-%03u.png",
                      test_current_gfx_directory,
                      TEST_CURRENT_GFX_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_current_gfx_face));
    test_png_write(test_current_gfx_face);

    length = snprintf(VS(test_installed_gfx_face),
                      "%s/face-%03u.png",
                      test_installed_gfx_directory,
                      TEST_INSTALLED_GFX_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_installed_gfx_face));
    test_png_write(test_installed_gfx_face);

    const char *precedence_directories[] = {
        test_user_gfx_directory,
        test_current_gfx_directory,
        test_installed_gfx_directory,
    };
    for (size_t i = 0; i < arraysize(precedence_directories); i++) {
        length = snprintf(VS(test_user_precedence_faces[i]),
                          "%s/face-%03u.png",
                          precedence_directories[i],
                          TEST_USER_GFX_FACE);
        TEST_CHECK(length > 0);
        TEST_CHECK((size_t)length < sizeof(test_user_precedence_faces[i]));
        test_png_write_data(test_user_precedence_faces[i],
                            i == 0 ? test_user_png : test_png,
                            i == 0 ? test_user_png_size : test_png_size);
    }

    TEST_CHECK(chdir(test_directory) == 0);
}

static void test_wait_for_asset(uint16_t face) {
    uint64_t deadline = SDL_GetTicks() + TEST_ASYNC_TIMEOUT_MS;
    while (test_assets[face] == NULL && SDL_GetTicks() < deadline) {
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(test_assets[face] != NULL);
}

static void test_wait_for_active(size_t count) {
    uint64_t deadline = SDL_GetTicks() + TEST_ASYNC_TIMEOUT_MS;
    while (test_asset_active < count && SDL_GetTicks() < deadline) {
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(test_asset_active == count);
}

static void test_wait_for_repeated_demand_local_checks(void) {
    uint64_t deadline = SDL_GetTicks() + TEST_ASYNC_TIMEOUT_MS;
    while (image_face_requests_test_unprepared_count() != 0 && SDL_GetTicks() < deadline) {
        for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + TEST_FACE_COUNT; face++) {
            image_request_face(face);
        }
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(image_face_requests_test_unprepared_count() == 0);
}

static void test_wait_for_all_local_checks(void) {
    uint64_t deadline = SDL_GetTicks() + TEST_ASYNC_TIMEOUT_MS;
    while (image_face_requests_test_unprepared_count() != 0 && SDL_GetTicks() < deadline) {
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(image_face_requests_test_unprepared_count() == 0);
}

static void test_wait_for_face(uint16_t face) {
    uint64_t deadline = SDL_GetTicks() + TEST_ASYNC_TIMEOUT_MS;
    while (FaceList[face].sprite == NULL && SDL_GetTicks() < deadline) {
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(FaceList[face].sprite != NULL);
    TEST_CHECK(FaceList[face].sprite->bitmap != NULL);
}

static void test_complete_face(uint16_t face) {
    TEST_CHECK(test_assets[face] != NULL);
    test_assets[face]->state = ASSET_REQUEST_COMPLETE;
    image_face_requests_service();
    TEST_CHECK(test_assets[face] == NULL);
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(face);
}

static void test_loader_submit(uint16_t face, uint64_t token, bool foreground, bool urgent) {
    face_loader_request_t request = {
        .face = face,
        .token = token,
        .kind = FACE_LOADER_NETWORK,
        .foreground = foreground,
        .urgent = urgent,
        .checksum = test_png_crc,
        .data = test_png,
        .size = test_png_size,
    };
    TEST_CHECK(face_loader_submit(&request));
}

static void test_loader_promotion_states(void) {
    enum {
        ACTIVE_TOKEN = 1001,
        QUEUED_TOKEN,
        RESULT_TOKEN,
    };
    TEST_CHECK(image_codec_test_enter(false));
    test_loader_submit(TEST_FACE_FIRST, ACTIVE_TOKEN, false, false);
    image_codec_test_wait_for_waiters(0, 1);
    face_loader_promote(TEST_FACE_FIRST, ACTIVE_TOKEN);

    test_loader_submit(TEST_FACE_FIRST + 1U, QUEUED_TOKEN, false, false);
    face_loader_promote(TEST_FACE_FIRST + 1U, QUEUED_TOKEN);
    test_loader_submit(TEST_FACE_FIRST + 2U, RESULT_TOKEN, false, false);
    image_codec_test_leave(false);

    TEST_CHECK(face_loader_test_wait_results(3, TEST_ASYNC_TIMEOUT_MS));
    face_loader_promote(TEST_FACE_FIRST + 2U, RESULT_TOKEN);
    const uint64_t expected[] = {RESULT_TOKEN, ACTIVE_TOKEN, QUEUED_TOKEN};
    for (size_t i = 0; i < arraysize(expected); i++) {
        face_loader_result_t *result = face_loader_result_pop(true);
        TEST_CHECK(result != NULL);
        TEST_CHECK(result->token == expected[i]);
        TEST_CHECK(result->foreground);
        TEST_CHECK(result->urgent);
        TEST_CHECK(result->sprite != NULL);
        TEST_CHECK(SDL_MUSTLOCK(result->sprite->bitmap));
        face_loader_result_free(result);
    }
}

static void test_loader_urgent_fairness(void) {
    enum {
        BLOCKER_TOKEN = 2001,
        OLDEST_TOKEN,
        NEWER_TOKEN,
        URGENT_FIRST_TOKEN,
        URGENT_SECOND_TOKEN,
        URGENT_THIRD_TOKEN,
    };
    TEST_CHECK(image_codec_test_enter(false));
    test_loader_submit(TEST_FACE_FIRST, BLOCKER_TOKEN, true, false);
    image_codec_test_wait_for_waiters(0, 1);
    test_loader_submit(TEST_FACE_FIRST + 1U, OLDEST_TOKEN, true, false);
    test_loader_submit(TEST_FACE_FIRST + 2U, NEWER_TOKEN, true, false);
    test_loader_submit(TEST_FACE_FIRST + 3U, URGENT_FIRST_TOKEN, true, true);
    test_loader_submit(TEST_FACE_FIRST + 4U, URGENT_SECOND_TOKEN, true, true);
    test_loader_submit(TEST_FACE_FIRST + 5U, URGENT_THIRD_TOKEN, true, true);
    image_codec_test_leave(false);

    TEST_CHECK(face_loader_test_wait_results(6, TEST_ASYNC_TIMEOUT_MS));
    const uint64_t expected[] = {
        URGENT_FIRST_TOKEN,
        URGENT_SECOND_TOKEN,
        BLOCKER_TOKEN,
        URGENT_THIRD_TOKEN,
        OLDEST_TOKEN,
        NEWER_TOKEN,
    };
    for (size_t i = 0; i < arraysize(expected); i++) {
        face_loader_result_t *result = face_loader_result_pop(true);
        TEST_CHECK(result != NULL);
        TEST_CHECK(result->token == expected[i]);
        TEST_CHECK(result->sprite != NULL);
        TEST_CHECK(SDL_MUSTLOCK(result->sprite->bitmap));
        face_loader_result_free(result);
    }
}

static void test_offline_local_sources(void) {
    size_t paths_before = test_file_path_calls;
    test_asset_transport_available = false;

    image_request_face(TEST_WARM_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_WARM_FACE);
    TEST_CHECK(test_asset_start_count == 0);
    TEST_CHECK(test_file_path_calls == paths_before);

    image_request_face(TEST_PACK_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_PACK_FACE);
    TEST_CHECK(test_asset_start_count == 0);
    TEST_CHECK(test_file_path_calls == paths_before);

    test_asset_transport_available = true;
}

static void test_loader_restart_and_gfx_roots(void) {
    test_asset_transport_available = false;
    face_loader_stop();

    image_request_face(TEST_CURRENT_GFX_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_available());
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_CURRENT_GFX_FACE);

    image_request_face(TEST_INSTALLED_GFX_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_INSTALLED_GFX_FACE);

    image_request_face(TEST_USER_GFX_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_USER_GFX_FACE);
    TEST_CHECK(FaceList[TEST_USER_GFX_FACE].checksum == test_user_png_crc);
    char expected[MAX_BUF];
    int length = snprintf(VS(expected), DIRECTORY_GFX_USER "/face-%03u.png", TEST_USER_GFX_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(expected));
    TEST_CHECK(strcmp(FaceList[TEST_USER_GFX_FACE].name, expected) == 0);
    TEST_CHECK(test_asset_start_count == 0);

    test_asset_transport_available = true;
}

static void test_background_local_survives_offline_foreground(void) {
    test_asset_transport_available = false;
    image_request_face(TEST_OFFLINE_FOREGROUND_FACE);
    image_prefetch_face(TEST_OFFLINE_BACKGROUND_FACE);

    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_OFFLINE_BACKGROUND_FACE);

    TEST_CHECK(FaceList[TEST_OFFLINE_FOREGROUND_FACE].sprite == NULL);
    TEST_CHECK((FaceList[TEST_OFFLINE_FOREGROUND_FACE].flags & FACE_REQUESTED) != 0);
    image_face_requests_clear();
    test_asset_transport_available = true;
}

static void test_background_loader_depth(void) {
    test_asset_transport_available = false;
    for (uint16_t face = TEST_BACKGROUND_DEPTH_FIRST;
         face < TEST_BACKGROUND_DEPTH_FIRST + TEST_BACKGROUND_DEPTH_COUNT;
         face++) {
        image_prefetch_face(face);
    }
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(TEST_BACKGROUND_DEPTH_COUNT, TEST_ASYNC_TIMEOUT_MS));
    image_face_requests_clear();
    test_asset_transport_available = true;
}

static void test_admission_backpressure(void) {
    image_request_face(0);
    TEST_CHECK(FaceList[0].name == NULL);
    TEST_CHECK((FaceList[0].flags & FACE_REQUESTED) == 0);

    test_asset_admission_blocked = true;
    image_request_face(TEST_BACKPRESSURE_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    for (size_t i = 0; i < 5; i++) {
        image_face_requests_service();
        SDL_Delay(2);
    }
    TEST_CHECK(test_asset_start_attempts == 5);
    TEST_CHECK(test_asset_start_count == 0);

    test_asset_admission_blocked = false;
    image_face_requests_service();
    TEST_CHECK(test_asset_start_attempts == 6);
    TEST_CHECK(test_asset_start_count == 1);
    test_complete_face(TEST_BACKPRESSURE_FACE);
    TEST_CHECK(test_asset_active == 0);
}

static void test_terminal_failure_requeues_on_demand(void) {
    image_request_face(TEST_FAILURE_FACE);
    for (unsigned int attempt = 1; attempt <= 3; attempt++) {
        test_wait_for_asset(TEST_FAILURE_FACE);
        test_assets[TEST_FAILURE_FACE]->state = ASSET_REQUEST_ERROR;
        image_face_requests_service();
    }

    TEST_CHECK(test_assets[TEST_FAILURE_FACE] == NULL);
    TEST_CHECK(FaceList[TEST_FAILURE_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_FAILURE_FACE].flags & FACE_REQUESTED) == 0);

    image_request_face(TEST_FAILURE_FACE);
    test_wait_for_asset(TEST_FAILURE_FACE);
    test_complete_face(TEST_FAILURE_FACE);
}

static void test_connection_clear_discards_stale_completion(void) {
    image_request_face(TEST_CLEAR_FACE);
    test_wait_for_asset(TEST_CLEAR_FACE);
    test_assets[TEST_CLEAR_FACE]->state = ASSET_REQUEST_COMPLETE;
    image_face_requests_service();
    TEST_CHECK(test_assets[TEST_CLEAR_FACE] == NULL);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    TEST_CHECK(FaceList[TEST_CLEAR_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_CLEAR_FACE].flags & FACE_REQUESTED) == 0);

    image_request_face(TEST_CLEAR_FACE);
    test_wait_for_asset(TEST_CLEAR_FACE);
    test_complete_face(TEST_CLEAR_FACE);
}

static void test_local_cache_bypasses_occupied_streams(void) {
    for (uint16_t face = TEST_OCCUPIED_FACE_FIRST;
         face < TEST_OCCUPIED_FACE_FIRST + TEST_OCCUPIED_FACE_COUNT;
         face++) {
        image_request_face(face);
    }
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);
    size_t starts_before = test_asset_start_count;

    image_request_face(TEST_OCCUPIED_LOCAL_FACE);
    image_face_requests_service();
    TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
    test_wait_for_face(TEST_OCCUPIED_LOCAL_FACE);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_count == starts_before);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_counters_reset(void) {
    TEST_CHECK(test_asset_active == 0);
    memset(test_asset_attempt_faces, 0, sizeof(test_asset_attempt_faces));
    memset(test_asset_start_faces, 0, sizeof(test_asset_start_faces));
    test_file_path_calls = 0;
    test_cache_enqueues = 0;
    test_asset_start_attempts = 0;
    test_asset_start_count = 0;
    test_asset_priority_start_count = 0;
    test_asset_preempt_count = 0;
    test_asset_state_polls = 0;
    test_asset_preemption_required = false;
    test_asset_replacement_progressed = false;
    test_book_redraws = 0;
    test_interface_redraws = 0;
    test_widget_redraws = 0;
    test_active_effect_redraws = 0;
}

static void test_foreground_promotes_a_prefetch(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 4U; face++) {
        image_prefetch_face(face);
    }
    image_request_face(TEST_FACE_FIRST + 3U);
    image_face_requests_service();
    test_wait_for_asset(TEST_FACE_FIRST + 3U);

    TEST_CHECK(test_asset_start_count == 1);
    TEST_CHECK(test_asset_start_faces[0] == TEST_FACE_FIRST + 3U);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_foreground_preempts_admitted_prefetches(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 4U; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + ASSET_STREAM_ACTIVE_MAX;
         face++) {
        TEST_CHECK(test_assets[face] != NULL);
    }

    uint16_t foreground = TEST_FACE_FIRST + 3U;
    image_request_face(foreground);
    test_wait_for_asset(foreground);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_faces[test_asset_start_count - 1U] == foreground);
    TEST_CHECK(test_asset_priority_start_count == 1U);
    TEST_CHECK(test_asset_preempt_count == 1U);
    size_t retained = 0;
    size_t preempted = 0;
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + ASSET_STREAM_ACTIVE_MAX;
         face++) {
        if (test_assets[face] != NULL) {
            retained++;
        } else {
            preempted++;
        }
    }
    TEST_CHECK(retained == ASSET_STREAM_ACTIVE_MAX - 1U);
    TEST_CHECK(preempted == 1U);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_progressed_priority_still_releases_a_full_logical_slot(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 4U; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);

    test_asset_replacement_progressed = true;
    uint16_t foreground = TEST_FACE_FIRST + 3U;
    image_request_face(foreground);
    test_wait_for_asset(foreground);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX + 1U);
    TEST_CHECK(test_asset_priority_start_count == 1U);
    TEST_CHECK(test_asset_preempt_count == 1U);

    size_t retained = 0;
    for (uint16_t face = TEST_FACE_FIRST; face < foreground; face++) {
        retained += test_assets[face] != NULL;
    }
    TEST_CHECK(retained == ASSET_STREAM_ACTIVE_MAX - 1U);
    TEST_CHECK(test_assets[foreground] != NULL);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    test_asset_replacement_progressed = false;
}

static void test_admitted_polling_is_constant_after_promotions(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + TEST_FACE_COUNT; face++) {
        image_request_face(face);
    }
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);
    size_t polls_before = test_asset_state_polls;
    image_face_requests_service();
    TEST_CHECK(test_asset_state_polls - polls_before == ASSET_STREAM_ACTIVE_MAX);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_batch_admits_bounded_logical_faces(void) {
    size_t admission_limit = ASSET_STREAM_ACTIVE_MAX * ASSET_FACE_BATCH_MAX;
    test_asset_face_batch_available = true;
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + admission_limit; face++) {
        image_request_face(face);
    }
    test_wait_for_active(admission_limit);
    TEST_CHECK(test_asset_start_count == admission_limit);

    size_t polls_before = test_asset_state_polls;
    image_face_requests_service();
    TEST_CHECK(test_asset_state_polls - polls_before == admission_limit);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    test_asset_face_batch_available = false;
}

static void test_batch_preempts_below_logical_limit_when_physical_slots_are_full(void) {
    const uint16_t visible = TEST_FACE_FIRST + 4U;
    test_asset_face_batch_available = true;
    for (uint16_t face = TEST_FACE_FIRST; face < visible; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_active(4U);

    /* Model three undersized/interleaved physical batches even though only
     * four logical faces are admitted. Visible work must still reclaim an
     * active speculative stream instead of waiting for the 24-handle cap. */
    test_asset_preemption_required = true;
    image_request_face(visible);
    test_wait_for_asset(visible);
    TEST_CHECK(test_asset_active == 5U);
    TEST_CHECK(test_asset_priority_start_count == 1U);
    TEST_CHECK(test_asset_preempt_count == 1U);
    TEST_CHECK(test_assets[visible] != NULL);
    TEST_CHECK(test_asset_start_count == 6U);
    TEST_CHECK(test_asset_start_faces[4] == visible);
    TEST_CHECK(test_asset_start_faces[5] != visible);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    test_asset_preemption_required = false;
    test_asset_face_batch_available = false;
}

static void test_progressed_priority_avoids_below_limit_background_churn(void) {
    const uint16_t visible = TEST_FACE_FIRST + 4U;
    test_asset_face_batch_available = true;
    for (uint16_t face = TEST_FACE_FIRST; face < visible; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_active(4U);

    /* The I/O thread may open the priority replacement between the physical-
     * capacity query and handoff call. Below the logical cap that success must
     * retain every speculative handle instead of cancelling one pointlessly. */
    test_asset_preemption_required = true;
    test_asset_replacement_progressed = true;
    image_request_face(visible);
    test_wait_for_asset(visible);
    TEST_CHECK(test_asset_active == 5U);
    TEST_CHECK(test_asset_start_count == 5U);
    TEST_CHECK(test_asset_priority_start_count == 1U);
    TEST_CHECK(test_asset_preempt_count == 1U);
    for (uint16_t face = TEST_FACE_FIRST; face < visible; face++) {
        TEST_CHECK(test_assets[face] != NULL);
        TEST_CHECK(test_assets[face]->state == ASSET_REQUEST_PENDING);
    }

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    test_asset_replacement_progressed = false;
    test_asset_preemption_required = false;
    test_asset_face_batch_available = false;
}

static void test_batch_preemption_refunds_foreground_collateral(void) {
    const size_t admission_limit = ASSET_STREAM_ACTIVE_MAX * ASSET_FACE_BATCH_MAX;
    const uint16_t collateral = TEST_FACE_FIRST + 1U;
    const uint16_t visible = TEST_FACE_FIRST + (uint16_t)admission_limit;
    test_asset_face_batch_available = true;
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + admission_limit; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_active(admission_limit);

    /* Promote a member of the first speculative physical batch, then force a
     * newer visible face to reclaim that batch. The promoted collateral must
     * be refunded and retried as priority work. */
    image_request_face(collateral);
    image_request_face(visible);
    test_wait_for_asset(visible);
    TEST_CHECK(test_asset_active == admission_limit);
    TEST_CHECK(test_asset_priority_start_count == 1U);
    TEST_CHECK(test_asset_preempt_count == 1U);
    TEST_CHECK(test_assets[collateral] != NULL);
    TEST_CHECK(test_assets[collateral]->state == ASSET_REQUEST_PREEMPTED);

    image_face_requests_service();
    TEST_CHECK(test_assets[collateral] != NULL);
    TEST_CHECK(test_assets[collateral]->state == ASSET_REQUEST_PENDING);
    TEST_CHECK(test_asset_priority_start_count == 2U);
    TEST_CHECK(FaceList[collateral].name != NULL);
    TEST_CHECK((FaceList[collateral].flags & FACE_REQUESTED) != 0);

    /* PREEMPTED is a local displacement, not a transfer failure. Repeated
     * collateral churn beyond the ordinary retry cap must never consume the
     * foreground face's attempt budget or clear its catalog entry. */
    for (size_t iteration = 0; iteration < TEST_PREEMPTION_REPETITIONS; iteration++) {
        test_assets[collateral]->state = ASSET_REQUEST_PREEMPTED;
        image_face_requests_service();
        TEST_CHECK(test_assets[collateral] != NULL);
        TEST_CHECK(test_assets[collateral]->state == ASSET_REQUEST_PENDING);
        TEST_CHECK(FaceList[collateral].name != NULL);
        TEST_CHECK((FaceList[collateral].flags & FACE_REQUESTED) != 0);
    }
    TEST_CHECK(test_asset_priority_start_count == TEST_PREEMPTION_REPETITIONS + 2U);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    test_asset_face_batch_available = false;
}

static void test_recent_demand_leads_without_starving_old_work(void) {
    test_asset_transport_available = false;
    for (uint16_t face = TEST_RECENCY_FACE_FIRST;
         face < TEST_RECENCY_FACE_FIRST + TEST_RECENCY_FACE_COUNT;
         face++) {
        image_request_face(face);
    }
    for (size_t pass = 0; pass < 20U; pass++) {
        image_face_requests_service();
        SDL_Delay(1);
    }
    TEST_CHECK(test_asset_start_count == 0);
    SDL_Delay(6);

    uint16_t recent_first = TEST_RECENCY_FACE_FIRST + TEST_RECENCY_FACE_COUNT - 1U;
    uint16_t recent_second = recent_first - 1U;
    image_request_face(recent_first);
    image_request_face(recent_second);
    test_asset_transport_available = true;
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);

    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_faces[0] == recent_second);
    TEST_CHECK(test_asset_start_faces[1] == recent_first);
    TEST_CHECK(test_asset_start_faces[2] != recent_first);
    TEST_CHECK(test_asset_start_faces[2] != recent_second);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_blocked_recent_demand_retries_first(void) {
    test_asset_transport_available = false;
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + TEST_FACE_COUNT; face++) {
        image_request_face(face);
    }
    test_wait_for_repeated_demand_local_checks();

    uint16_t recent = TEST_FACE_FIRST + TEST_FACE_COUNT / 2U;
    image_request_face(recent);
    test_asset_admission_blocked = true;
    test_asset_transport_available = true;
    size_t attempts_before = test_asset_start_attempts;
    for (size_t pass = 0; pass < 3U; pass++) {
        image_face_requests_service();
        TEST_CHECK(test_asset_start_attempts == attempts_before + pass + 1U);
        TEST_CHECK(test_asset_attempt_faces[test_asset_start_attempts - 1U] == recent);
    }

    test_asset_admission_blocked = false;
    image_face_requests_service();
    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_faces[0] == recent);
    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_completed_transfer_handoff_prefers_urgent(void) {
    uint16_t ordinary = TEST_FACE_FIRST;
    uint16_t urgent = TEST_FACE_FIRST + 1U;
    test_asset_transport_available = false;
    image_request_face(ordinary);
    test_wait_for_all_local_checks();
    SDL_Delay(6);
    test_asset_transport_available = true;
    test_wait_for_asset(ordinary);

    test_asset_transport_available = false;
    image_request_face(urgent);
    test_wait_for_all_local_checks();
    test_asset_transport_available = true;
    test_wait_for_asset(urgent);
    TEST_CHECK(test_asset_active == 2U);

    TEST_CHECK(image_codec_test_enter(false));
    for (size_t i = 0; i < 7U; i++) {
        test_loader_submit(TEST_FACE_FIRST + 2U + (uint16_t)i, 3001U + i, true, false);
    }
    image_codec_test_wait_for_waiters(0, 1);
    test_assets[ordinary]->state = ASSET_REQUEST_COMPLETE;
    test_assets[urgent]->state = ASSET_REQUEST_COMPLETE;
    image_request_face(urgent);
    image_face_requests_service();

    TEST_CHECK(test_assets[urgent] == NULL);
    TEST_CHECK(test_assets[ordinary] != NULL);
    image_codec_test_leave(false);
    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_completed_background_does_not_block_visible(void) {
    test_asset_transport_available = false;
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 4U; face++) {
        image_prefetch_face(face);
    }
    test_wait_for_all_local_checks();
    test_asset_transport_available = true;
    test_wait_for_active(ASSET_STREAM_ACTIVE_MAX);

    TEST_CHECK(image_codec_test_enter(false));
    test_loader_submit(TEST_FACE_FIRST + 10U, 4001U, false, false);
    image_codec_test_wait_for_waiters(0, 1);
    test_loader_submit(TEST_FACE_FIRST + 11U, 4002U, false, false);
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + ASSET_STREAM_ACTIVE_MAX;
         face++) {
        TEST_CHECK(test_assets[face] != NULL);
        test_assets[face]->state = ASSET_REQUEST_COMPLETE;
    }
    image_face_requests_service();
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);

    uint16_t visible = TEST_FACE_FIRST + 3U;
    image_request_face(visible);
    image_face_requests_service();
    TEST_CHECK(test_assets[visible] != NULL);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);

    image_codec_test_leave(false);
    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static size_t test_loaded_budget_faces(void) {
    size_t loaded = 0;
    for (uint16_t face = TEST_BUDGET_FACE_FIRST;
         face < TEST_BUDGET_FACE_FIRST + TEST_BUDGET_FACE_COUNT;
         face++) {
        if (FaceList[face].sprite != NULL) {
            loaded++;
        }
    }
    return loaded;
}

static uint64_t test_unlimited_clock(void) {
    return 0;
}

static size_t test_budget_clock_calls;

static uint64_t test_budget_clock(void) {
    return test_budget_clock_calls++ < 2 ? UINT64_C(1000) : UINT64_C(4001);
}

static void test_production_completion_budget(void) {
    image_face_requests_test_clock_set(test_unlimited_clock);
    for (uint16_t face = TEST_BUDGET_FACE_FIRST;
         face < TEST_BUDGET_FACE_FIRST + TEST_BUDGET_FACE_COUNT;
         face++) {
        image_request_face(face);
    }
    test_wait_for_active(TEST_BUDGET_FACE_COUNT);
    for (uint16_t face = TEST_BUDGET_FACE_FIRST;
         face < TEST_BUDGET_FACE_FIRST + TEST_BUDGET_FACE_COUNT;
         face++) {
        TEST_CHECK(test_assets[face] != NULL);
        test_assets[face]->state = ASSET_REQUEST_COMPLETE;
    }
    image_face_requests_service();
    TEST_CHECK(test_asset_active == 0);
    TEST_CHECK(face_loader_test_wait_results(TEST_BUDGET_FACE_COUNT, TEST_ASYNC_TIMEOUT_MS));

    size_t redraws_before = test_book_redraws;
    size_t active_effect_redraws_before = test_active_effect_redraws;
    test_budget_clock_calls = 0;
    image_face_requests_test_clock_set(test_budget_clock);
    image_face_requests_service();
    TEST_CHECK(test_loaded_budget_faces() == 1);
    TEST_CHECK(test_book_redraws == redraws_before + 1);
    TEST_CHECK(test_active_effect_redraws == active_effect_redraws_before + 1);

    image_face_requests_test_clock_set(NULL);
    for (uint16_t face = TEST_BUDGET_FACE_FIRST;
         face < TEST_BUDGET_FACE_FIRST + TEST_BUDGET_FACE_COUNT;
         face++) {
        test_wait_for_face(face);
    }
    TEST_CHECK(test_loaded_budget_faces() == TEST_BUDGET_FACE_COUNT);
}

static void test_malformed_network_png_is_rejected(void) {
    char name[MAX_BUF];
    int length = snprintf(VS(name), "face-%03u", TEST_MALFORMED_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(name));
    finish_face_cmd(TEST_MALFORMED_FACE, test_malformed_png_crc, name);
    image_request_face(TEST_MALFORMED_FACE);

    for (size_t attempt = 0; attempt < 3; attempt++) {
        test_wait_for_asset(TEST_MALFORMED_FACE);
        test_assets[TEST_MALFORMED_FACE]->state = ASSET_REQUEST_COMPLETE;
        image_face_requests_service();
        TEST_CHECK(test_assets[TEST_MALFORMED_FACE] == NULL);
        TEST_CHECK(face_loader_test_wait_results(1, TEST_ASYNC_TIMEOUT_MS));
        image_face_requests_service();
    }
    TEST_CHECK(FaceList[TEST_MALFORMED_FACE].sprite == NULL);
    TEST_CHECK(FaceList[TEST_MALFORMED_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_MALFORMED_FACE].flags & FACE_REQUESTED) == 0);
}

static void test_empty_network_payload_retries_then_fails(void) {
    image_request_face(TEST_EMPTY_FACE);
    for (size_t attempt = 0; attempt < 3; attempt++) {
        test_wait_for_asset(TEST_EMPTY_FACE);
        test_assets[TEST_EMPTY_FACE]->state = ASSET_REQUEST_COMPLETE;
        image_face_requests_service();
        TEST_CHECK((attempt < 2) == (test_assets[TEST_EMPTY_FACE] != NULL));
    }
    TEST_CHECK(FaceList[TEST_EMPTY_FACE].sprite == NULL);
    TEST_CHECK(FaceList[TEST_EMPTY_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_EMPTY_FACE].flags & FACE_REQUESTED) == 0);
}

static void test_deinit_joins_active_loader(void) {
    for (size_t iteration = 0; iteration < 25; iteration++) {
        image_bmaps_init();
        for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 16U; face++) {
            image_request_face(face);
        }
        image_face_requests_service();
        image_bmaps_deinit();
        TEST_CHECK(test_asset_active == 0);
    }
}

int main(void) {
    toolkit_import(logger);
    toolkit_import(datetime);
    toolkit_import(path);
    path_fopen = test_path_fopen;
    TEST_CHECK(SDL_Init(0));

    test_png_load();
    test_cache_prepare();
    image_init();
    image_bmaps_init();
    test_loader_promotion_states();
    test_loader_urgent_fairness();
    image_bmaps_deinit();

    image_bmaps_init();
    test_offline_local_sources();
    test_loader_restart_and_gfx_roots();
    test_background_local_survives_offline_foreground();
    test_background_loader_depth();
    test_admission_backpressure();
    test_terminal_failure_requeues_on_demand();
    test_connection_clear_discards_stale_completion();
    test_local_cache_bypasses_occupied_streams();
    test_production_completion_budget();
    test_malformed_network_png_is_rejected();
    test_empty_network_payload_retries_then_fails();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_foreground_promotes_a_prefetch();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_foreground_preempts_admitted_prefetches();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_progressed_priority_still_releases_a_full_logical_slot();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_admitted_polling_is_constant_after_promotions();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_batch_admits_bounded_logical_faces();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_batch_preempts_below_logical_limit_when_physical_slots_are_full();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_progressed_priority_avoids_below_limit_background_churn();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_batch_preemption_refunds_foreground_collateral();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_recent_demand_leads_without_starving_old_work();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_blocked_recent_demand_retries_first();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_completed_transfer_handoff_prefers_urgent();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_completed_background_does_not_block_visible();
    image_bmaps_deinit();

    test_deinit_joins_active_loader();

    TEST_CHECK(chdir(test_original_directory) == 0);
    TEST_CHECK(unlink(test_warm_cache) == 0);
    TEST_CHECK(unlink(test_occupied_warm_cache) == 0);
    TEST_CHECK(unlink(test_offline_warm_cache) == 0);
    TEST_CHECK(unlink(test_current_gfx_face) == 0);
    TEST_CHECK(unlink(test_installed_gfx_face) == 0);
    for (size_t i = 0; i < arraysize(test_user_precedence_faces); i++) {
        TEST_CHECK(unlink(test_user_precedence_faces[i]) == 0);
    }
    TEST_CHECK(unlink(test_pack_path) == 0);
    TEST_CHECK(rmdir(test_user_gfx_directory) == 0);
    TEST_CHECK(rmdir(test_current_gfx_directory) == 0);
    TEST_CHECK(rmdir(test_installed_gfx_directory) == 0);
    TEST_CHECK(rmdir(test_installed_directory) == 0);
    TEST_CHECK(rmdir(test_data_directory) == 0);
    TEST_CHECK(rmdir(test_cache_directory) == 0);
    TEST_CHECK(rmdir(test_directory) == 0);
    image_deinit();
    free(test_malformed_png);
    free(test_user_png);
    free(test_png);
    SDL_Quit();
    toolkit_deinit();
    return 0;
}
