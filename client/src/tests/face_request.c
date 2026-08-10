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

/** @file Bounded local decoding, priority, retry, and face-backlog regressions. */

#include <global.h>
#include <face_cache.h>
#include <toolkit/datetime.h>
#include <toolkit/path.h>
#include <wrapper.h>

#define TEST_FACE_FIRST 2U
#define TEST_FACE_COUNT (ASSET_REQUEST_PENDING_MAX + 6U)
#define TEST_BACKPRESSURE_FACE (TEST_FACE_FIRST + TEST_FACE_COUNT)
#define TEST_FAILURE_FACE (TEST_BACKPRESSURE_FACE + 1U)
#define TEST_CLEAR_FACE (TEST_FAILURE_FACE + 1U)
#define TEST_OCCUPIED_FACE_FIRST (TEST_CLEAR_FACE + 1U)
#define TEST_OCCUPIED_FACE_COUNT ASSET_STREAM_ACTIVE_MAX
#define TEST_OCCUPIED_LOCAL_FACE (TEST_OCCUPIED_FACE_FIRST + TEST_OCCUPIED_FACE_COUNT)
#define TEST_FACE_LAST TEST_OCCUPIED_LOCAL_FACE

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
int map_redraw_flag;
int minimap_redraw_flag;

static char test_directory[HUGE_BUF];
static char test_cache_directory[HUGE_BUF];
static char test_warm_cache[HUGE_BUF];
static char test_occupied_warm_cache[HUGE_BUF];
static uint8_t *test_png;
static size_t test_png_size;
static uint32_t test_png_crc;
static size_t test_file_path_calls;
static size_t test_cache_enqueues;
static size_t test_asset_active;
static size_t test_asset_start_attempts;
static size_t test_asset_start_count;
static size_t test_file_path_calls_at_first_asset_start;
static size_t test_asset_state_polls;
static uint16_t test_asset_start_faces[TEST_FACE_COUNT];
static asset_request_t *test_assets[MAX_FACE_TILES];
static bool test_asset_admission_blocked;
static bool test_asset_transport_available = true;
static size_t test_book_redraws;
static size_t test_interface_redraws;
static size_t test_widget_redraws;

static FILE *test_path_fopen(const char *path, const char *mode) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(mode != NULL);
    TEST_CHECK(strncmp(path, DIRECTORY_GFX_USER "/", sizeof(DIRECTORY_GFX_USER)) == 0);
    TEST_CHECK(strcmp(mode, "rb") == 0);
    return NULL;
}

char *file_path(const char *path, const char *mode) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(path[0] != '/');
    TEST_CHECK(mode != NULL);
    TEST_CHECK(strcmp(mode, "wb") == 0);
    test_file_path_calls++;
    return path_join(test_directory, path);
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
    TEST_CHECK(data == test_png);
    TEST_CHECK(size == test_png_size);
    test_cache_enqueues++;
}

void face_cache_report_failures(void) {}

void face_cache_stop(void) {}

bool asset_requests_available(void) {
    return test_asset_transport_available;
}

asset_request_t *asset_request_start_bounded(const char *path, size_t max_size) {
    TEST_CHECK(path != NULL);
    TEST_CHECK(max_size == ASSET_FACE_MAX_SIZE);
    test_asset_start_attempts++;
    if (test_asset_admission_blocked) {
        return NULL;
    }
    TEST_CHECK(test_asset_active < ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_count < TEST_FACE_COUNT);

    uint16_t face = 0;
    TEST_CHECK(socket_asset_face_path_parse(path, &face));
    TEST_CHECK(face >= TEST_FACE_FIRST);
    TEST_CHECK(face <= TEST_FACE_LAST);
    TEST_CHECK(test_assets[face] == NULL);

    asset_request_t *request = xcalloc(1, sizeof(*request));
    request->face = face;
    request->state = ASSET_REQUEST_PENDING;
    test_assets[face] = request;
    if (test_asset_start_count == 0) {
        test_file_path_calls_at_first_asset_start = test_file_path_calls;
    }
    test_asset_start_faces[test_asset_start_count++] = face;
    test_asset_active++;
    return request;
}

asset_request_state_t asset_request_get_state(asset_request_t *request) {
    TEST_CHECK(request != NULL);
    test_asset_state_polls++;
    return request->state;
}

const uint8_t *asset_request_get_data(const asset_request_t *request, size_t *size) {
    TEST_CHECK(request != NULL);
    TEST_CHECK(request->state == ASSET_REQUEST_COMPLETE);
    if (size != NULL) {
        *size = test_png_size;
    }
    return test_png;
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

    SDL_Surface *surface = IMG_LoadPNG_IO(stream);
    TEST_CHECK(surface != NULL);
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

void sprite_cache_free_all(void) {}

void book_redraw(void) {
    test_book_redraws++;
}

void interface_redraw(void) {
    test_interface_redraws++;
}

void widget_redraw_all(int widget_type_id) {
    (void)widget_type_id;
    test_widget_redraws++;
}

static void test_png_load(void) {
    char source[HUGE_BUF];
    int length = snprintf(VS(source), "%s/textures/magic.png", ATRINIK_TEST_SOURCE_DIR);
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
}

static void test_cache_prepare(void) {
    snprintf(VS(test_directory), "/tmp/atrinik-face-request-XXXXXX");
    TEST_CHECK(mkdtemp(test_directory) != NULL);

    int length = snprintf(VS(test_cache_directory), "%s/%s", test_directory, DIRECTORY_CACHE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_cache_directory));
    TEST_CHECK(mkdir(test_cache_directory, 0700) == 0);

    length = snprintf(VS(test_warm_cache), "%s/face-001.png", test_cache_directory);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_warm_cache));
    FILE *fp = fopen(test_warm_cache, "wb");
    TEST_CHECK(fp != NULL);
    TEST_CHECK(fwrite(test_png, 1, test_png_size, fp) == test_png_size);
    TEST_CHECK(fclose(fp) == 0);

    length = snprintf(VS(test_occupied_warm_cache),
                      "%s/face-%03u.png",
                      test_cache_directory,
                      TEST_OCCUPIED_LOCAL_FACE);
    TEST_CHECK(length > 0);
    TEST_CHECK((size_t)length < sizeof(test_occupied_warm_cache));
    fp = fopen(test_occupied_warm_cache, "wb");
    TEST_CHECK(fp != NULL);
    TEST_CHECK(fwrite(test_png, 1, test_png_size, fp) == test_png_size);
    TEST_CHECK(fclose(fp) == 0);
}

static void test_warm_cache_decode(void) {
    TEST_CHECK(test_asset_start_count == 0);
    image_request_face(1);
    TEST_CHECK(test_file_path_calls == 0);
    TEST_CHECK(FaceList[1].sprite == NULL);
    test_asset_transport_available = false;
    image_face_requests_service();
    test_asset_transport_available = true;
    TEST_CHECK(test_file_path_calls == 1);
    TEST_CHECK(test_asset_start_count == 0);
    TEST_CHECK(FaceList[1].sprite != NULL);
    TEST_CHECK(FaceList[1].sprite->bitmap != NULL);
    TEST_CHECK((FaceList[1].flags & FACE_REQUESTED) == 0);
}

static void test_local_cache_bypasses_occupied_streams(void) {
    size_t starts_before = test_asset_start_count;
    for (uint16_t face = TEST_OCCUPIED_FACE_FIRST;
         face < TEST_OCCUPIED_FACE_FIRST + TEST_OCCUPIED_FACE_COUNT;
         face++) {
        image_request_face(face);
    }
    image_face_requests_service();
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_count == starts_before + ASSET_STREAM_ACTIVE_MAX);

    image_request_face(TEST_OCCUPIED_LOCAL_FACE);
    TEST_CHECK(FaceList[TEST_OCCUPIED_LOCAL_FACE].sprite == NULL);
    image_face_requests_service();
    TEST_CHECK(FaceList[TEST_OCCUPIED_LOCAL_FACE].sprite != NULL);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_count == starts_before + ASSET_STREAM_ACTIVE_MAX);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_admission_backpressure(void) {
    image_request_face(0);
    TEST_CHECK(FaceList[0].name == NULL);
    TEST_CHECK((FaceList[0].flags & FACE_REQUESTED) == 0);

    image_request_face(TEST_BACKPRESSURE_FACE);
    TEST_CHECK(FaceList[TEST_BACKPRESSURE_FACE].name != NULL);
    TEST_CHECK((FaceList[TEST_BACKPRESSURE_FACE].flags & FACE_REQUESTED) != 0);

    test_asset_admission_blocked = true;
    for (size_t i = 0; i < 5; i++) {
        image_face_requests_service();
        SDL_Delay(2);
    }
    TEST_CHECK(test_asset_start_attempts == 5);
    TEST_CHECK(test_asset_start_count == 0);
    TEST_CHECK(FaceList[TEST_BACKPRESSURE_FACE].name != NULL);

    test_asset_admission_blocked = false;
    image_face_requests_service();
    TEST_CHECK(test_asset_start_attempts == 6);
    TEST_CHECK(test_asset_start_count == 1);
    TEST_CHECK(test_assets[TEST_BACKPRESSURE_FACE] != NULL);

    test_assets[TEST_BACKPRESSURE_FACE]->state = ASSET_REQUEST_COMPLETE;
    image_face_requests_service();
    TEST_CHECK(FaceList[TEST_BACKPRESSURE_FACE].sprite != NULL);
    TEST_CHECK(test_asset_active == 0);
}

static void test_terminal_failure_requeues_on_demand(void) {
    image_request_face(TEST_FAILURE_FACE);
    image_face_requests_service();

    for (unsigned int attempt = 1; attempt <= 3; attempt++) {
        TEST_CHECK(test_assets[TEST_FAILURE_FACE] != NULL);
        test_assets[TEST_FAILURE_FACE]->state = ASSET_REQUEST_ERROR;
        image_face_requests_service();
        if (attempt < 3) {
            SDL_Delay(attempt + 1U);
            image_face_requests_service();
        }
    }

    TEST_CHECK(test_assets[TEST_FAILURE_FACE] == NULL);
    TEST_CHECK(FaceList[TEST_FAILURE_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_FAILURE_FACE].flags & FACE_REQUESTED) == 0);

    image_request_face(TEST_FAILURE_FACE);
    image_face_requests_service();
    TEST_CHECK(test_assets[TEST_FAILURE_FACE] != NULL);
    test_assets[TEST_FAILURE_FACE]->state = ASSET_REQUEST_COMPLETE;
    image_face_requests_service();
    TEST_CHECK(FaceList[TEST_FAILURE_FACE].sprite != NULL);
}

static void test_connection_clear_discards_stale_completion(void) {
    image_request_face(TEST_CLEAR_FACE);
    image_face_requests_service();
    TEST_CHECK(test_assets[TEST_CLEAR_FACE] != NULL);
    test_assets[TEST_CLEAR_FACE]->state = ASSET_REQUEST_COMPLETE;

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
    TEST_CHECK(test_assets[TEST_CLEAR_FACE] == NULL);
    TEST_CHECK(FaceList[TEST_CLEAR_FACE].name == NULL);
    TEST_CHECK((FaceList[TEST_CLEAR_FACE].flags & FACE_REQUESTED) == 0);

    image_request_face(TEST_CLEAR_FACE);
    image_face_requests_service();
    TEST_CHECK(test_assets[TEST_CLEAR_FACE] != NULL);
    test_assets[TEST_CLEAR_FACE]->state = ASSET_REQUEST_COMPLETE;
    image_face_requests_service();
    TEST_CHECK(FaceList[TEST_CLEAR_FACE].sprite != NULL);
}

static void test_counters_reset(void) {
    TEST_CHECK(test_asset_active == 0);
    memset(test_asset_start_faces, 0, sizeof(test_asset_start_faces));
    test_file_path_calls = 0;
    test_cache_enqueues = 0;
    test_asset_start_attempts = 0;
    test_asset_start_count = 0;
    test_file_path_calls_at_first_asset_start = 0;
    test_asset_state_polls = 0;
    test_book_redraws = 0;
    test_interface_redraws = 0;
    test_widget_redraws = 0;
}

static void test_foreground_promotes_a_prefetch(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + 4U; face++) {
        image_prefetch_face(face);
    }
    image_request_face(TEST_FACE_FIRST + 3U);
    image_face_requests_service();

    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_start_faces[0] == TEST_FACE_FIRST + 3U);
    TEST_CHECK(test_asset_start_faces[1] == TEST_FACE_FIRST);
    TEST_CHECK(test_asset_start_faces[2] == TEST_FACE_FIRST + 1U);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_admitted_polling_is_constant_after_promotions(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + TEST_FACE_COUNT; face++) {
        image_prefetch_face(face);
    }
    image_face_requests_service();
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);

    for (uint16_t face = TEST_FACE_FIRST + ASSET_STREAM_ACTIVE_MAX;
         face < TEST_FACE_FIRST + TEST_FACE_COUNT;
         face++) {
        image_request_face(face);
    }
    size_t polls_before = test_asset_state_polls;
    image_face_requests_service();
    TEST_CHECK(test_asset_state_polls - polls_before == ASSET_STREAM_ACTIVE_MAX);

    image_face_requests_clear();
    TEST_CHECK(test_asset_active == 0);
}

static void test_fifo_backlog(void) {
    for (uint16_t face = TEST_FACE_FIRST; face < TEST_FACE_FIRST + TEST_FACE_COUNT; face++) {
        image_request_face(face);
        TEST_CHECK(FaceList[face].name != NULL);
        TEST_CHECK((FaceList[face].flags & FACE_REQUESTED) != 0);
    }
    TEST_CHECK(test_asset_start_count == 0);
    TEST_CHECK(test_file_path_calls == 0);

    image_face_requests_service();
    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    /* Network admission must not wait for connection-independent discovery
     * across the entire cold backlog. */
    TEST_CHECK(test_file_path_calls_at_first_asset_start == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_file_path_calls == TEST_FACE_COUNT);
    for (size_t i = 0; i < test_asset_start_count; i++) {
        TEST_CHECK(test_asset_start_faces[i] == TEST_FACE_FIRST + i);
    }

    for (size_t completed = 0; completed < ASSET_STREAM_ACTIVE_MAX; completed++) {
        uint16_t face = (uint16_t)(TEST_FACE_FIRST + completed);
        TEST_CHECK(test_assets[face] != NULL);
        test_assets[face]->state = ASSET_REQUEST_COMPLETE;
    }
    image_face_requests_service();
    for (size_t completed = 0; completed < ASSET_STREAM_ACTIVE_MAX; completed++) {
        uint16_t face = (uint16_t)(TEST_FACE_FIRST + completed);
        TEST_CHECK(FaceList[face].sprite != NULL);
    }
    TEST_CHECK(test_asset_start_count == ASSET_STREAM_ACTIVE_MAX * 2U);
    TEST_CHECK(test_asset_active == ASSET_STREAM_ACTIVE_MAX);
    TEST_CHECK(test_book_redraws == 1);
    TEST_CHECK(test_interface_redraws == 1);
    TEST_CHECK(test_widget_redraws == 3);

    for (size_t completed = ASSET_STREAM_ACTIVE_MAX; completed < TEST_FACE_COUNT; completed++) {
        uint16_t face = (uint16_t)(TEST_FACE_FIRST + completed);
        TEST_CHECK(test_assets[face] != NULL);
        test_assets[face]->state = ASSET_REQUEST_COMPLETE;
        image_face_requests_service();
        TEST_CHECK(FaceList[face].sprite != NULL);

        size_t expected_starts = MIN(TEST_FACE_COUNT, ASSET_STREAM_ACTIVE_MAX + completed + 1U);
        TEST_CHECK(test_asset_start_count == expected_starts);
        TEST_CHECK(test_asset_active == expected_starts - completed - 1U);
        for (size_t i = 0; i < test_asset_start_count; i++) {
            TEST_CHECK(test_asset_start_faces[i] == TEST_FACE_FIRST + i);
        }
    }

    TEST_CHECK(test_asset_start_count == TEST_FACE_COUNT);
    TEST_CHECK(test_asset_active == 0);
    TEST_CHECK(test_cache_enqueues == TEST_FACE_COUNT);
    TEST_CHECK(test_file_path_calls == TEST_FACE_COUNT);
}

int main(void) {
    toolkit_import(logger);
    toolkit_import(datetime);
    toolkit_import(path);
    path_fopen = test_path_fopen;
    TEST_CHECK(SDL_Init(0));

    test_png_load();
    test_cache_prepare();
    image_bmaps_init();
    test_warm_cache_decode();
    test_admission_backpressure();
    test_terminal_failure_requeues_on_demand();
    test_connection_clear_discards_stale_completion();
    test_local_cache_bypasses_occupied_streams();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_foreground_promotes_a_prefetch();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_admitted_polling_is_constant_after_promotions();
    image_bmaps_deinit();

    test_counters_reset();
    image_bmaps_init();
    test_fifo_backlog();
    image_bmaps_deinit();

    TEST_CHECK(unlink(test_warm_cache) == 0);
    TEST_CHECK(unlink(test_occupied_warm_cache) == 0);
    TEST_CHECK(rmdir(test_cache_directory) == 0);
    TEST_CHECK(rmdir(test_directory) == 0);
    free(test_png);
    SDL_Quit();
    toolkit_deinit();
    return 0;
}
