/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Handles image related code.
 */

#include <global.h>
#include <wrapper.h>
#include <client_socket.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>
#include <toolkit/path.h>
#include <toolkit/datetime.h>
#include <face_asset.h>
#include <face_cache.h>
#include <face_loader.h>

#define FACE_ASSET_ADMISSION_MAX (ASSET_STREAM_ACTIVE_MAX * ASSET_FACE_BATCH_MAX)
#define FACE_TRANSFER_RETRY_MAX 3U
#define FACE_ASSET_SCAN_MAX 64U
#ifndef FACE_TRANSFER_RETRY_BASE_MS
#define FACE_TRANSFER_RETRY_BASE_MS UINT64_C(250)
#endif
#ifndef FACE_COMPLETION_BUDGET_US
#define FACE_COMPLETION_BUDGET_US UINT64_C(2000)
#endif
#ifndef FACE_URGENT_MAX_AGE_MS
#define FACE_URGENT_MAX_AGE_MS UINT64_C(250)
#endif

typedef enum face_loader_state {
    FACE_LOADER_STATE_NONE,
    FACE_LOADER_STATE_LOCAL,
    FACE_LOADER_STATE_NETWORK,
} face_loader_state_t;

typedef struct face_asset_request {
    struct face_asset_request *prev;
    struct face_asset_request *next;
    uint16_t face;
    unsigned int attempts;
    uint64_t retry_at_ms;
    uint64_t urgent_at_ms;
    uint64_t urgent_sequence;
    uint64_t demand_generation;
    asset_request_t *asset;
    uint64_t loader_token;
    bool foreground;
    bool urgent;
    bool transfer_urgent;
    bool local_checked;
    uint8_t admitted_slot;
    face_loader_state_t loader_state;
} face_asset_request_t;

/**
 * Bitmaps loaded from image packs.
 */
static bmap_hash_t *image_bmap_packs = NULL;
/**
 * Bitmaps loaded from the server bmaps file.
 */
static bmap_t *image_bmaps = NULL;
/**
 * Number of entries in ::image_bmaps.
 */
static size_t image_bmaps_size = 0;
/** Tracks incomplete immutable/offline map inputs without changing request APIs. */
static bool image_missing_faces;
static face_asset_request_t *face_asset_requests;
static face_asset_request_t *face_asset_foreground_tail;
static face_asset_request_t *face_asset_requests_tail;
/** Next foreground request whose local sources have not entered the loader. */
static face_asset_request_t *face_asset_foreground_prepare_cursor;
/** Next queued request whose connection-independent sources need inspection. */
static face_asset_request_t *face_asset_prepare_cursor;
/** Rotating admission cursors keep retry-delayed backlogs frame-budgeted. */
static face_asset_request_t *face_asset_foreground_admission_cursor;
static face_asset_request_t *face_asset_urgent_admission_cursor;
static face_asset_request_t *face_asset_background_admission_cursor;
static face_asset_request_t *face_asset_request_index[MAX_FACE_TILES];
static face_asset_request_t *face_asset_admitted[FACE_ASSET_ADMISSION_MAX];
static size_t face_asset_request_count;
static size_t face_asset_admitted_count;
static size_t face_asset_unprepared_count;
static size_t face_asset_foreground_unprepared_count;
static unsigned int face_asset_urgent_admission_streak;
static uint64_t face_asset_next_loader_token;
static uint64_t face_asset_next_urgent_sequence;
static uint64_t face_asset_loader_retry_at_ms;
static uint64_t face_asset_demand_generation = 1;

#ifdef ATRINIK_FACE_REQUEST_TESTING
static uint64_t (*face_completion_test_clock)(void);

void image_face_requests_test_clock_set(uint64_t (*clock_func)(void)) {
    face_completion_test_clock = clock_func;
}

size_t image_face_requests_test_unprepared_count(void) {
    return face_asset_unprepared_count;
}
#endif

static uint64_t face_completion_now_us(void) {
#ifdef ATRINIK_FACE_REQUEST_TESTING
    if (face_completion_test_clock != NULL) {
        return face_completion_test_clock();
    }
#endif
    return datetime_monotonic_us();
}

static bool face_completion_budget_remaining(uint64_t started_us) {
    return face_completion_now_us() - started_us < FACE_COMPLETION_BUDGET_US;
}

static size_t face_asset_admission_limit(void) {
    return asset_face_batch_available() ? FACE_ASSET_ADMISSION_MAX : ASSET_STREAM_ACTIVE_MAX;
}

static bool face_asset_request_is_urgent(face_asset_request_t *request, uint64_t now_ms) {
    if (request->urgent && now_ms >= request->urgent_at_ms &&
        now_ms - request->urgent_at_ms > FACE_URGENT_MAX_AGE_MS) {
        request->urgent = false;
    }
    return request->urgent;
}

static void face_asset_request_release(face_asset_request_t *request) {
    if (request->asset == NULL) {
        HARD_ASSERT(request->admitted_slot == UINT8_MAX);
        return;
    }
    HARD_ASSERT(request->admitted_slot < FACE_ASSET_ADMISSION_MAX);
    HARD_ASSERT(face_asset_admitted[request->admitted_slot] == request);
    asset_request_free(request->asset);
    request->asset = NULL;
    face_asset_admitted[request->admitted_slot] = NULL;
    request->admitted_slot = UINT8_MAX;
    HARD_ASSERT(face_asset_admitted_count != 0);
    face_asset_admitted_count--;
}

static void face_asset_request_detach(face_asset_request_t *request) {
    if (request->prev != NULL) {
        request->prev->next = request->next;
    } else {
        face_asset_requests = request->next;
    }
    if (request->next != NULL) {
        request->next->prev = request->prev;
    } else {
        face_asset_requests_tail = request->prev;
    }
    if (face_asset_foreground_tail == request) {
        face_asset_foreground_tail =
            request->prev != NULL && request->prev->foreground ? request->prev : NULL;
    }
    request->prev = NULL;
    request->next = NULL;
}

static void face_asset_request_append_background(face_asset_request_t *request) {
    HARD_ASSERT(!request->local_checked);
    request->prev = face_asset_requests_tail;
    if (face_asset_requests_tail != NULL) {
        face_asset_requests_tail->next = request;
    } else {
        face_asset_requests = request;
    }
    face_asset_requests_tail = request;
    face_asset_unprepared_count++;
    if (face_asset_prepare_cursor == NULL) {
        face_asset_prepare_cursor = request;
    }
    if (face_asset_background_admission_cursor == NULL) {
        face_asset_background_admission_cursor = request;
    }
}

static face_asset_request_t *face_asset_first_in_lane(bool foreground) {
    if (foreground) {
        return face_asset_requests != NULL && face_asset_requests->foreground ? face_asset_requests
                                                                              : NULL;
    }
    return face_asset_foreground_tail != NULL ? face_asset_foreground_tail->next
                                              : face_asset_requests;
}

static face_asset_request_t *face_asset_next_in_lane(face_asset_request_t *request,
                                                     bool foreground) {
    face_asset_request_t *next = request->next;
    if (next == NULL || next->foreground != foreground) {
        next = face_asset_first_in_lane(foreground);
    }
    return next;
}

static void face_asset_request_insert_foreground(face_asset_request_t *request) {
    if (!request->foreground) {
        request->foreground = true;
        if (!request->local_checked) {
            face_asset_foreground_unprepared_count++;
        }
    }
    request->next = face_asset_requests;
    if (face_asset_requests != NULL) {
        face_asset_requests->prev = request;
    } else {
        face_asset_requests_tail = request;
    }
    face_asset_requests = request;
    if (face_asset_foreground_tail == NULL) {
        face_asset_foreground_tail = request;
    }
    if (face_asset_foreground_admission_cursor == NULL) {
        face_asset_foreground_admission_cursor = request;
    }
}

static void face_asset_request_remove(face_asset_request_t *request) {
    HARD_ASSERT(face_asset_request_index[request->face] == request);
    face_asset_request_index[request->face] = NULL;
    if (request->loader_state != FACE_LOADER_STATE_NONE) {
        face_loader_cancel(request->face, request->loader_token);
        request->loader_state = FACE_LOADER_STATE_NONE;
    }
    if (!request->local_checked) {
        HARD_ASSERT(face_asset_unprepared_count != 0);
        face_asset_unprepared_count--;
        if (request->foreground) {
            HARD_ASSERT(face_asset_foreground_unprepared_count != 0);
            face_asset_foreground_unprepared_count--;
        }
    }
    if (face_asset_foreground_prepare_cursor == request) {
        face_asset_foreground_prepare_cursor = face_asset_next_in_lane(request, true);
        if (face_asset_foreground_prepare_cursor == request) {
            face_asset_foreground_prepare_cursor = NULL;
        }
    }
    if (face_asset_prepare_cursor == request) {
        face_asset_prepare_cursor = face_asset_next_in_lane(request, false);
        if (face_asset_prepare_cursor == request) {
            face_asset_prepare_cursor = NULL;
        }
    }
    if (face_asset_foreground_admission_cursor == request) {
        face_asset_foreground_admission_cursor = face_asset_next_in_lane(request, true);
        if (face_asset_foreground_admission_cursor == request) {
            face_asset_foreground_admission_cursor = NULL;
        }
    }
    if (face_asset_urgent_admission_cursor == request) {
        face_asset_urgent_admission_cursor = face_asset_next_in_lane(request, true);
        if (face_asset_urgent_admission_cursor == request) {
            face_asset_urgent_admission_cursor = NULL;
        }
    }
    if (face_asset_background_admission_cursor == request) {
        face_asset_background_admission_cursor = face_asset_next_in_lane(request, false);
        if (face_asset_background_admission_cursor == request) {
            face_asset_background_admission_cursor = NULL;
        }
    }
    face_asset_request_release(request);
    face_asset_request_detach(request);
    if (face_asset_prepare_cursor == NULL && face_asset_unprepared_count != 0) {
        face_asset_prepare_cursor = face_asset_first_in_lane(false);
    }
    HARD_ASSERT(face_asset_request_count != 0);
    face_asset_request_count--;
    free(request);
}

void image_face_requests_clear(void) {
    while (face_asset_requests != NULL) {
        uint16_t face = face_asset_requests->face;
        free(FaceList[face].name);
        FaceList[face].name = NULL;
        FaceList[face].checksum = 0;
        FaceList[face].flags &= ~FACE_REQUESTED;
        face_asset_request_remove(face_asset_requests);
    }
    HARD_ASSERT(face_asset_requests_tail == NULL);
    HARD_ASSERT(face_asset_foreground_tail == NULL);
    HARD_ASSERT(face_asset_foreground_prepare_cursor == NULL);
    HARD_ASSERT(face_asset_prepare_cursor == NULL);
    HARD_ASSERT(face_asset_foreground_admission_cursor == NULL);
    HARD_ASSERT(face_asset_urgent_admission_cursor == NULL);
    HARD_ASSERT(face_asset_background_admission_cursor == NULL);
    HARD_ASSERT(face_asset_request_count == 0);
    HARD_ASSERT(face_asset_admitted_count == 0);
    HARD_ASSERT(face_asset_unprepared_count == 0);
    HARD_ASSERT(face_asset_foreground_unprepared_count == 0);
    face_asset_urgent_admission_streak = 0;
}

static void face_asset_request_promote(uint16_t face) {
    face_asset_request_t *request = face_asset_request_index[face];
    if (request != NULL) {
        uint64_t now_ms = SDL_GetTicks();
        request->urgent = true;
        request->urgent_at_ms = now_ms;
        face_asset_next_urgent_sequence++;
        if (face_asset_next_urgent_sequence == 0) {
            face_asset_next_urgent_sequence++;
        }
        request->urgent_sequence = face_asset_next_urgent_sequence;
        request->transfer_urgent = request->transfer_urgent || request->asset != NULL;
        if (request->demand_generation == face_asset_demand_generation) {
            face_asset_urgent_admission_cursor = request;
            return;
        }
        request->demand_generation = face_asset_demand_generation;
        bool was_foreground = request->foreground;
        if (face_asset_prepare_cursor == request) {
            face_asset_prepare_cursor = face_asset_next_in_lane(request, false);
            if (face_asset_prepare_cursor == request) {
                face_asset_prepare_cursor = NULL;
            }
        }
        if (face_asset_background_admission_cursor == request) {
            face_asset_background_admission_cursor = face_asset_next_in_lane(request, false);
            if (face_asset_background_admission_cursor == request) {
                face_asset_background_admission_cursor = NULL;
            }
        }
        if (face_asset_foreground_admission_cursor == request) {
            face_asset_foreground_admission_cursor = face_asset_next_in_lane(request, true);
            if (face_asset_foreground_admission_cursor == request) {
                face_asset_foreground_admission_cursor = NULL;
            }
        }
        if (face_asset_urgent_admission_cursor == request) {
            face_asset_urgent_admission_cursor = face_asset_next_in_lane(request, true);
            if (face_asset_urgent_admission_cursor == request) {
                face_asset_urgent_admission_cursor = NULL;
            }
        }
        face_asset_request_detach(request);
        request->retry_at_ms = request->attempts == 0 ? 0 : request->retry_at_ms;
        face_asset_request_insert_foreground(request);
        if (!request->local_checked && request->loader_state == FACE_LOADER_STATE_NONE &&
            (!was_foreground || face_asset_foreground_prepare_cursor == NULL)) {
            face_asset_foreground_prepare_cursor = request;
        }
        face_asset_urgent_admission_cursor = request;
        if (request->loader_state != FACE_LOADER_STATE_NONE) {
            face_loader_promote(request->face, request->loader_token);
        }
    }
}

static void face_asset_request_cancel(uint16_t face) {
    face_asset_request_t *request = face_asset_request_index[face];
    if (request != NULL) {
        face_asset_request_remove(request);
    }
}

void image_missing_faces_reset(void) {
    image_missing_faces = false;
}

bool image_missing_faces_detected(void) {
    return image_missing_faces;
}

/**
 * Check whether a face ID can be used to index ::FaceList.
 */
bool image_face_valid(int face) {
    return face >= 0 && face < MAX_FACE_TILES;
}

/**
 * Get a loaded sprite without allowing an invalid face array access.
 */
sprite_struct *image_get_sprite(int face) {
    sprite_struct *sprite = image_face_valid(face) ? FaceList[face].sprite : NULL;

    return sprite != NULL && sprite->bitmap != NULL ? sprite : NULL;
}

/**
 * Get a face name without allowing an invalid face array access.
 */
const char *image_get_face_name(int face) {
    return image_face_valid(face) ? FaceList[face].name : NULL;
}
/**
 * Free data associated with a bmap_t structure.
 */
static void bmap_free(bmap_t *bmap) {
    HARD_ASSERT(bmap != NULL);
    free(bmap->name);
}

/**
 * Read bmaps from image packs, calculate checksums, etc.
 */
void image_init(void) {
    FILE *fp = path_fopen(FILE_ATRINIK_P0, "rb");
    if (fp == NULL) {
        return;
    }

    size_t tmp_buf_size = 24 * 1024;
    char *tmp_buf = xmalloc(tmp_buf_size);

    char buf[HUGE_BUF];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strncmp(buf, "IMAGE ", 6)) {
            LOG(ERROR, "The file %s is corrupted.", FILE_ATRINIK_P0);
            exit(1);
        }

        char *cp;
        /* Skip across the image ID data. */
        for (cp = buf + 6; *cp != ' '; cp++) {}

        size_t len = atoi(cp);

        /* Skip across the length data. */
        for (cp = cp + 1; *cp != ' '; cp++) {}

        /* Adjust the buffer if necessary. */
        if (len > tmp_buf_size) {
            tmp_buf_size = len;
            tmp_buf = xrealloc(tmp_buf, tmp_buf_size);
        }

        long pos = ftell(fp);
        if (fread(tmp_buf, 1, len, fp) != len) {
            break;
        }

        string_strip_newline(cp);

        /* Trim left whitespace. */
        while (*cp == ' ') {
            cp++;
        }

        bmap_hash_t *bmap = xcalloc(1, sizeof(*bmap));
        bmap->bmap.name = xstrdup(cp);
        bmap->bmap.crc32 = crc32(1L, (const unsigned char FAR *)tmp_buf, len);
        bmap->bmap.len = len;
        bmap->bmap.pos = pos;
        HASH_ADD_KEYPTR(hh, image_bmap_packs, bmap->bmap.name, strlen(bmap->bmap.name), bmap);
    }

    free(tmp_buf);
    fclose(fp);
}

/*
 * Deinitialize the image packs.
 */
void image_deinit(void) {
    bmap_hash_t *curr, *tmp;

    HASH_ITER(hh, image_bmap_packs, curr, tmp) {
        HASH_DEL(image_bmap_packs, curr);
        bmap_free(&curr->bmap);
        free(curr);
    }
}

static bool face_asset_loader_start(void) {
    char *pack_path = file_path(FILE_ATRINIK_P0, "rb");
    char *gfx_user_marker = file_path(DIRECTORY_GFX_USER "/.face-loader-root", "wb");
    char *gfx_user_path = path_dirname(gfx_user_marker);
    char gfx_installed_marker[HUGE_BUF];
    get_data_dir_file(VS(gfx_installed_marker), DIRECTORY_GFX_USER "/.face-loader-root");
    char *gfx_installed_path = path_dirname(gfx_installed_marker);
    char *cache_marker = file_path(DIRECTORY_CACHE "/.face-loader-root", "wb");
    char *cache_path = path_dirname(cache_marker);

    bool started =
        face_loader_start(pack_path,
                          gfx_user_path,
                          "./" DIRECTORY_GFX_USER,
                          gfx_installed_path,
                          cache_path,
                          ScreenSurface != NULL ? ScreenSurface->format : SDL_PIXELFORMAT_RGBA32);
    free(cache_path);
    free(cache_marker);
    free(gfx_installed_path);
    free(gfx_user_path);
    free(gfx_user_marker);
    free(pack_path);
    return started;
}

/**
 * Read bmaps server file.
 */
void image_bmaps_init(void) {
    FILE *fp = server_file_open_name(SERVER_FILE_BMAPS);
    if (fp == NULL) {
        return;
    }

    /* Free previously allocated bmaps. */
    image_bmaps_deinit();

    char buf[HUGE_BUF];
    while (fgets(buf, sizeof(buf), fp)) {
        uint32_t len, crc;
        char name[HUGE_BUF];
        if (sscanf(buf, "%x %x %s", &len, &crc, name) != 3) {
            LOG(BUG, "Syntax error in server bmaps file: %s", buf);
            break;
        }

        bmap_hash_t *bmap;
        HASH_FIND_STR(image_bmap_packs, name, bmap);

        /* Expand the array. */
        image_bmaps = xreallocarray(image_bmaps, (image_bmaps_size + 1), sizeof(*image_bmaps));

        /* Does it exist, and the lengths and checksums match? */
        if (bmap != NULL && bmap->bmap.len == len && bmap->bmap.crc32 == crc) {
            image_bmaps[image_bmaps_size].pos = bmap->bmap.pos;
        } else {
            /* It doesn't exist in the atrinik.p0 file. */
            image_bmaps[image_bmaps_size].pos = -1;
        }

        image_bmaps[image_bmaps_size].len = len;
        image_bmaps[image_bmaps_size].crc32 = crc;
        image_bmaps[image_bmaps_size].name = xstrdup(name);

        image_bmaps_size++;
    }

    fclose(fp);
    if (!face_asset_loader_start()) {
        LOG(ERROR, "Could not start the asynchronous face loader");
        face_asset_loader_retry_at_ms = SDL_GetTicks() + 1000U;
    } else {
        face_asset_loader_retry_at_ms = 0;
    }
    if (!face_cache_start()) {
        LOG(ERROR, "Could not start the face cache writer");
    }
}

/**
 * Deinitialize the bmaps.
 */
void image_bmaps_deinit(void) {
    image_face_requests_clear();
    face_loader_stop();
    face_asset_loader_retry_at_ms = 0;
    face_cache_stop();

    if (image_bmaps != NULL) {
        for (size_t i = 0; i < image_bmaps_size; i++) {
            free(image_bmaps[i].name);
        }

        free(image_bmaps);
        image_bmaps = NULL;
        image_bmaps_size = 0;
    }

    for (size_t i = 0; i < MAX_FACE_TILES; i++) {
        if (FaceList[i].name != NULL) {
            free(FaceList[i].name);
            FaceList[i].name = NULL;
            sprite_free_sprite(FaceList[i].sprite);
            FaceList[i].sprite = NULL;
            FaceList[i].checksum = 0;
        }
        FaceList[i].flags = 0;
    }

    sprite_cache_free_all();
}

/**
 * Finish face command.
 *
 * @param pnum
 * ID of the face.
 * @param checksum
 * Face checksum.
 * @param face
 * Face name.
 */
void finish_face_cmd(int facenum, uint32_t checksum, const char *face) {
    HARD_ASSERT(face != NULL);

    if (!image_face_valid(facenum) || (size_t)facenum >= image_bmaps_size) {
        LOG(ERROR,
            "Ignoring invalid face data ID %d (catalog size: %" PRIu64 ")",
            facenum,
            (uint64_t)image_bmaps_size);
        return;
    }
    if (*face == '\0' || strchr(face, '/') != NULL || strchr(face, '\\') != NULL ||
        strlen(face) > MAX_BUF - sizeof(".png")) {
        LOG(ERROR, "Ignoring unsafe face name for ID %d", facenum);
        image_missing_faces = true;
        return;
    }
    char name[MAX_BUF];
    int name_length = snprintf(VS(name), "%s.png", face);
    if (name_length < 0 || (size_t)name_length >= sizeof(name)) {
        image_missing_faces = true;
        return;
    }

    /* Loaded or requested. */
    if (FaceList[facenum].name != NULL) {
        if (strcmp(name, FaceList[facenum].name) == 0 && checksum == FaceList[facenum].checksum &&
            (FaceList[facenum].sprite != NULL || (FaceList[facenum].flags & FACE_REQUESTED) != 0)) {
            return;
        }

        /* Something is different. */
        face_asset_request_cancel((uint16_t)facenum);
        free(FaceList[facenum].name);
        FaceList[facenum].name = NULL;
        sprite_free_sprite(FaceList[facenum].sprite);
        FaceList[facenum].sprite = NULL;
    }

    FaceList[facenum].name = xstrdup(name);
    FaceList[facenum].checksum = checksum;

    face_asset_request_t *request = xcalloc(1, sizeof(*request));
    request->face = (uint16_t)facenum;
    request->admitted_slot = UINT8_MAX;
    face_asset_next_loader_token++;
    if (face_asset_next_loader_token == 0) {
        face_asset_next_loader_token++;
    }
    request->loader_token = face_asset_next_loader_token;
    HARD_ASSERT(face_asset_request_index[request->face] == NULL);
    face_asset_request_index[request->face] = request;
    face_asset_request_append_background(request);
    /* FACE_REQUESTED deduplicates this backlog, so the fixed face table is
     * also its hard memory bound. */
    face_asset_request_count++;
    HARD_ASSERT(face_asset_request_count <= MAX_FACE_TILES);
    FaceList[facenum].flags |= FACE_REQUESTED;
}

static void face_asset_request_fail(face_asset_request_t *request, const char *reason) {
    LOG(ERROR,
        "Face %u download failed after %u attempt%s: %s",
        request->face,
        request->attempts,
        request->attempts == 1 ? "" : "s",
        reason);
    image_missing_faces = true;
    free(FaceList[request->face].name);
    FaceList[request->face].name = NULL;
    FaceList[request->face].checksum = 0;
    FaceList[request->face].flags &= ~FACE_REQUESTED;
    face_asset_request_remove(request);
}

static void face_asset_request_retry(face_asset_request_t *request, uint64_t now_ms, bool urgent) {
    if (urgent) {
        face_asset_request_promote(request->face);
    }
    request->retry_at_ms =
        urgent ? now_ms : now_ms + FACE_TRANSFER_RETRY_BASE_MS * request->attempts;
}

static void face_asset_request_mark_local_checked(face_asset_request_t *request) {
    HARD_ASSERT(!request->local_checked);
    request->local_checked = true;
    HARD_ASSERT(face_asset_unprepared_count != 0);
    face_asset_unprepared_count--;
    if (request->foreground) {
        HARD_ASSERT(face_asset_foreground_unprepared_count != 0);
        face_asset_foreground_unprepared_count--;
    }
    if (face_asset_foreground_prepare_cursor == request) {
        face_asset_foreground_prepare_cursor = face_asset_next_in_lane(request, true);
        if (face_asset_foreground_prepare_cursor == request) {
            face_asset_foreground_prepare_cursor = NULL;
        }
    }
    if (face_asset_prepare_cursor == request) {
        face_asset_prepare_cursor = face_asset_next_in_lane(request, false);
        if (face_asset_prepare_cursor == request) {
            face_asset_prepare_cursor = NULL;
        }
    }
    if (face_asset_foreground_unprepared_count == 0) {
        face_asset_foreground_prepare_cursor = NULL;
    }
    if (face_asset_unprepared_count == face_asset_foreground_unprepared_count) {
        face_asset_prepare_cursor = NULL;
    }
}

static asset_request_t *face_asset_request_open(face_asset_request_t *request, bool priority) {
    char path[32];
    if (!socket_asset_face_path_format(VS(path), request->face)) {
        return NULL;
    }
    return priority ? asset_request_start_bounded_priority(path, ASSET_FACE_MAX_SIZE)
                    : asset_request_start_bounded(path, ASSET_FACE_MAX_SIZE);
}

static void
face_asset_request_attach(face_asset_request_t *request, asset_request_t *asset, uint64_t now_ms) {
    HARD_ASSERT(request->asset == NULL);
    HARD_ASSERT(asset != NULL);
    request->asset = asset;
    request->transfer_urgent = request->transfer_urgent || request->urgent;
    request->urgent = false;
    request->attempts++;
    request->retry_at_ms = now_ms;
    size_t slot = 0;
    while (slot < FACE_ASSET_ADMISSION_MAX && face_asset_admitted[slot] != NULL) {
        slot++;
    }
    HARD_ASSERT(slot < FACE_ASSET_ADMISSION_MAX);
    request->admitted_slot = (uint8_t)slot;
    face_asset_admitted[slot] = request;
    face_asset_admitted_count++;
    HARD_ASSERT(face_asset_admitted_count <= FACE_ASSET_ADMISSION_MAX);
}

static bool face_asset_request_start(face_asset_request_t *request, uint64_t now_ms) {
    asset_request_t *asset = face_asset_request_open(request, false);
    if (asset == NULL) {
        return false;
    }
    face_asset_request_attach(request, asset, now_ms);
    return true;
}

static bool face_asset_request_submit_local(face_asset_request_t *request) {
    HARD_ASSERT(!request->local_checked);
    HARD_ASSERT(request->loader_state == FACE_LOADER_STATE_NONE);

    if (!face_loader_available()) {
        return false;
    }

    face_loader_request_t load = {
        .face = request->face,
        .token = request->loader_token,
        .kind = FACE_LOADER_LOCAL,
        .foreground = request->foreground,
        .urgent = face_asset_request_is_urgent(request, SDL_GetTicks()),
        .name = FaceList[request->face].name,
        .checksum = (uint32_t)FaceList[request->face].checksum,
        .pack_offset = image_bmaps[request->face].pos,
        .pack_size = image_bmaps[request->face].len,
    };
    if (!face_loader_submit(&load)) {
        return false;
    }
    request->loader_state = FACE_LOADER_STATE_LOCAL;
    return true;
}

static void face_asset_requests_schedule_local_lane(bool foreground, uint64_t started_us) {
    face_asset_request_t **cursor =
        foreground ? &face_asset_foreground_prepare_cursor : &face_asset_prepare_cursor;
    if (*cursor == NULL) {
        *cursor = face_asset_first_in_lane(foreground);
    }
    face_asset_request_t *request = *cursor;
    face_asset_request_t *first = request;
    size_t inspected = 0;
    while (request != NULL && inspected < FACE_ASSET_SCAN_MAX &&
           face_completion_budget_remaining(started_us) && face_loader_can_submit(foreground)) {
        face_asset_request_t *next = face_asset_next_in_lane(request, foreground);
        *cursor = next;
        inspected++;
        if (!request->local_checked && request->loader_state == FACE_LOADER_STATE_NONE &&
            !face_asset_request_submit_local(request)) {
            *cursor = request;
            return;
        }
        request = next;
        if (request == first) {
            return;
        }
    }
}

static void face_asset_requests_schedule_local(uint64_t started_us) {
    if (face_asset_foreground_unprepared_count != 0) {
        face_asset_requests_schedule_local_lane(true, started_us);
    }
    if (face_asset_foreground_unprepared_count != 0 ||
        face_asset_unprepared_count == face_asset_foreground_unprepared_count ||
        !face_completion_budget_remaining(started_us)) {
        return;
    }
    face_asset_requests_schedule_local_lane(false, started_us);
}

static bool face_asset_requests_have_background(void) {
    for (size_t slot = 0; slot < FACE_ASSET_ADMISSION_MAX; slot++) {
        if (face_asset_admitted[slot] != NULL && !face_asset_admitted[slot]->foreground) {
            return true;
        }
    }
    return false;
}

static bool face_asset_requests_preempt_background(uint64_t now_ms,
                                                   asset_request_t *replacement,
                                                   bool logical_slot_required) {
    HARD_ASSERT(replacement != NULL);
    face_asset_request_t *completed = NULL;
    for (size_t slot = 0; slot < FACE_ASSET_ADMISSION_MAX; slot++) {
        face_asset_request_t *request = face_asset_admitted[slot];
        if (request == NULL || request->foreground) {
            continue;
        }
        asset_request_state_t state = asset_request_get_state(request->asset);
        if (state == ASSET_REQUEST_COMPLETE && completed == NULL) {
            completed = request;
            continue;
        }
        if (state != ASSET_REQUEST_PENDING) {
            continue;
        }
        bool displace_victim = false;
        if (!asset_request_preempt(request->asset, replacement, &displace_victim)) {
            continue;
        }
        if (!displace_victim && !logical_slot_required) {
            /* The replacement won the I/O race after the capacity query. No
             * logical slot is needed below the cap, so retain the background. */
            return true;
        }
        face_asset_request_release(request);
        HARD_ASSERT(request->attempts != 0);
        request->attempts--;
        request->retry_at_ms = 0;
        return true;
    }
    if (!logical_slot_required || completed == NULL) {
        return false;
    }
    bool displace_victim = false;
    if (!asset_request_preempt(completed->asset, replacement, &displace_victim)) {
        return false;
    }
    /* A completed prefetch may be waiting behind the bounded background
     * decoder queue even though its QUIC stream is already free. Discard and
     * retry that speculative body so visible work can use the logical slot. */
    face_asset_request_release(completed);
    HARD_ASSERT(completed->attempts != 0);
    completed->attempts--;
    completed->retry_at_ms = now_ms + FACE_TRANSFER_RETRY_BASE_MS;
    return true;
}

typedef enum face_asset_admission_priority {
    FACE_ASSET_ADMISSION_URGENT,
    FACE_ASSET_ADMISSION_FAIR,
    FACE_ASSET_ADMISSION_BACKGROUND,
} face_asset_admission_priority_t;

static face_asset_request_t *
face_asset_requests_ready_next(face_asset_admission_priority_t priority,
                               uint64_t now_ms,
                               uint64_t started_us) {
    bool foreground = priority != FACE_ASSET_ADMISSION_BACKGROUND;
    face_asset_request_t **cursor = priority == FACE_ASSET_ADMISSION_URGENT
                                        ? &face_asset_urgent_admission_cursor
                                        : (foreground ? &face_asset_foreground_admission_cursor
                                                      : &face_asset_background_admission_cursor);
    if (*cursor == NULL) {
        *cursor = face_asset_first_in_lane(foreground);
    }
    face_asset_request_t *request = *cursor;
    face_asset_request_t *first = request;
    size_t inspected = 0;
    while (request != NULL && inspected < FACE_ASSET_SCAN_MAX &&
           face_completion_budget_remaining(started_us)) {
        *cursor = face_asset_next_in_lane(request, foreground);
        inspected++;
        bool urgent = foreground && face_asset_request_is_urgent(request, now_ms);
        bool priority_matches = priority != FACE_ASSET_ADMISSION_URGENT || urgent;
        if (priority_matches && request->asset == NULL &&
            request->loader_state == FACE_LOADER_STATE_NONE && request->local_checked &&
            now_ms >= request->retry_at_ms) {
            return request;
        }
        request = *cursor;
        if (request == first) {
            return NULL;
        }
    }
    return NULL;
}

static void face_asset_requests_admit(uint64_t now_ms, uint64_t started_us) {
    size_t admission_limit = face_asset_admission_limit();
    while (face_completion_budget_remaining(started_us)) {
        face_asset_request_t *request = NULL;
        bool selected_urgent = false;
        if (face_asset_urgent_admission_streak < 2U) {
            request =
                face_asset_requests_ready_next(FACE_ASSET_ADMISSION_URGENT, now_ms, started_us);
            if (request != NULL) {
                selected_urgent = true;
            }
        }
        if (request == NULL) {
            request = face_asset_requests_ready_next(FACE_ASSET_ADMISSION_FAIR, now_ms, started_us);
        }
        if (request == NULL) {
            request =
                face_asset_requests_ready_next(FACE_ASSET_ADMISSION_URGENT, now_ms, started_us);
            if (request != NULL) {
                selected_urgent = true;
            }
        }
        bool foreground = request != NULL;
        asset_request_t *prestarted = NULL;
        if (foreground) {
            bool logical_slot_required = face_asset_admitted_count >= admission_limit;
            if (logical_slot_required && face_asset_admitted_count != admission_limit) {
                return;
            }
            if (logical_slot_required && !face_asset_requests_have_background()) {
                request->retry_at_ms = now_ms;
                if (selected_urgent) {
                    face_asset_urgent_admission_cursor = request;
                }
                face_asset_foreground_admission_cursor = request;
                return;
            }
            /* Queue the visible replacement before releasing physical
             * capacity. The asset scheduler then performs one locked priority
             * handoff, so speculative work cannot reclaim the stream in the
             * gap between cancellation and admission. */
            prestarted = face_asset_request_open(request, true);
            if (prestarted == NULL) {
                request->retry_at_ms = now_ms;
                if (selected_urgent) {
                    face_asset_urgent_admission_cursor = request;
                }
                face_asset_foreground_admission_cursor = request;
                return;
            }
            bool physical_slot_required = asset_request_preemption_needed(prestarted);
            bool handoff_ready = false;
            if (logical_slot_required || physical_slot_required) {
                handoff_ready = face_asset_requests_preempt_background(now_ms,
                                                                       prestarted,
                                                                       logical_slot_required);
            }
            if (logical_slot_required && !handoff_ready) {
                asset_request_free(prestarted);
                request->retry_at_ms = now_ms;
                if (selected_urgent) {
                    face_asset_urgent_admission_cursor = request;
                }
                face_asset_foreground_admission_cursor = request;
                return;
            }
        }
        if (!foreground) {
            if (face_asset_admitted_count >= admission_limit) {
                return;
            }
            request =
                face_asset_requests_ready_next(FACE_ASSET_ADMISSION_BACKGROUND, now_ms, started_us);
            if (request == NULL) {
                return;
            }
        }

        if (prestarted != NULL) {
            face_asset_request_attach(request, prestarted, now_ms);
        } else if (!face_asset_request_start(request, now_ms)) {
            /* Expected shared-scheduler pressure is retried at frame scale. */
            request->retry_at_ms = now_ms;
            if (!foreground) {
                face_asset_background_admission_cursor = request;
            } else if (selected_urgent) {
                face_asset_urgent_admission_cursor = request;
                face_asset_foreground_admission_cursor = request;
            } else {
                face_asset_foreground_admission_cursor = request;
            }
            return;
        }
        face_asset_urgent_admission_streak =
            foreground && selected_urgent ? face_asset_urgent_admission_streak + 1U : 0U;
    }
}

static void face_asset_requests_complete_transfers(uint64_t now_ms, uint64_t started_us) {
    size_t slot_order[FACE_ASSET_ADMISSION_MAX];
    for (size_t i = 0; i < FACE_ASSET_ADMISSION_MAX; i++) {
        slot_order[i] = i;
    }
    for (size_t i = 0; i + 1U < FACE_ASSET_ADMISSION_MAX; i++) {
        size_t best = i;
        for (size_t j = i + 1U; j < FACE_ASSET_ADMISSION_MAX; j++) {
            face_asset_request_t *candidate = face_asset_admitted[slot_order[j]];
            face_asset_request_t *selected = face_asset_admitted[slot_order[best]];
            if (candidate != NULL && candidate->foreground && candidate->transfer_urgent &&
                (selected == NULL || !selected->foreground || !selected->transfer_urgent ||
                 candidate->urgent_sequence > selected->urgent_sequence)) {
                best = j;
            }
        }
        size_t slot = slot_order[i];
        slot_order[i] = slot_order[best];
        slot_order[best] = slot;
    }

    for (unsigned int pass = 0; pass < 3U; pass++) {
        bool foreground = pass != 2U;
        for (size_t i = 0; i < FACE_ASSET_ADMISSION_MAX; i++) {
            if (!face_completion_budget_remaining(started_us)) {
                return;
            }
            size_t slot = slot_order[i];
            face_asset_request_t *request = face_asset_admitted[slot];
            if (request == NULL || request->foreground != foreground ||
                (foreground && request->transfer_urgent != (pass == 0))) {
                continue;
            }
            asset_request_state_t state = asset_request_get_state(request->asset);
            if (state == ASSET_REQUEST_PENDING) {
                continue;
            }
            if (state == ASSET_REQUEST_PREEMPTED) {
                bool urgent = request->foreground || request->transfer_urgent;
                face_asset_request_release(request);
                request->transfer_urgent = false;
                HARD_ASSERT(request->attempts != 0);
                request->attempts--;
                face_asset_request_retry(request, now_ms, urgent);
                continue;
            }
            if (state == ASSET_REQUEST_ERROR) {
                bool urgent = request->foreground || request->transfer_urgent;
                face_asset_request_release(request);
                request->transfer_urgent = false;
                if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                    face_asset_request_fail(request, "transfer retry limit reached");
                    continue;
                }
                face_asset_request_retry(request, now_ms, urgent);
                continue;
            }

            if (!face_loader_available()) {
                /* Keep the completed bounded body attached to its logical slot.
                 * Loader startup has its own backoff and is not a transfer
                 * failure; redownloading here could exhaust retries before that
                 * backoff expires. */
                continue;
            }

            size_t size = 0;
            const uint8_t *data = asset_request_get_data(request->asset, &size);
            if (data == NULL || size == 0 || size > ASSET_FACE_MAX_SIZE) {
                bool urgent = request->foreground || request->transfer_urgent;
                face_asset_request_release(request);
                request->transfer_urgent = false;
                if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                    face_asset_request_fail(request, "invalid face asset payload");
                } else {
                    face_asset_request_retry(request, now_ms, urgent);
                }
                continue;
            }
            face_loader_request_t load = {
                .face = request->face,
                .token = request->loader_token,
                .kind = FACE_LOADER_NETWORK,
                .foreground = request->foreground,
                .urgent = request->transfer_urgent,
                .checksum = (uint32_t)FaceList[request->face].checksum,
                .data = data,
                .size = size,
            };
            if (face_loader_submit(&load)) {
                request->loader_state = FACE_LOADER_STATE_NETWORK;
                face_asset_request_release(request);
                request->transfer_urgent = false;
            }
        }
    }
}

static void
face_asset_requests_complete_loader(uint64_t now_ms, uint64_t started_us, bool *redraw) {
    while (face_completion_budget_remaining(started_us)) {
        face_loader_result_t *result = face_loader_result_pop(true);
        if (result == NULL) {
            return;
        }

        face_asset_request_t *request = face_asset_request_index[result->face];
        face_loader_state_t expected =
            result->kind == FACE_LOADER_LOCAL ? FACE_LOADER_STATE_LOCAL : FACE_LOADER_STATE_NETWORK;
        if (request == NULL || request->loader_token != result->token ||
            request->loader_state != expected) {
            face_loader_result_free(result);
            continue;
        }
        request->loader_state = FACE_LOADER_STATE_NONE;

        if (result->kind == FACE_LOADER_LOCAL) {
            face_asset_request_mark_local_checked(request);
            if (result->sprite != NULL) {
                if (result->override_name != NULL) {
                    free(FaceList[request->face].name);
                    FaceList[request->face].name = result->override_name;
                    result->override_name = NULL;
                    FaceList[request->face].checksum = result->override_checksum;
                }
                sprite_free_sprite(FaceList[request->face].sprite);
                FaceList[request->face].sprite = result->sprite;
                result->sprite = NULL;
                FaceList[request->face].flags &= ~FACE_REQUESTED;
                face_asset_request_remove(request);
                *redraw = true;
            }
        } else if (result->sprite != NULL) {
            face_cache_enqueue(FaceList[request->face].name, result->data, result->size);
            sprite_free_sprite(FaceList[request->face].sprite);
            FaceList[request->face].sprite = result->sprite;
            result->sprite = NULL;
            LOG(DEBUG,
                "Installed face %u (%" PRIu64 " bytes; worker %.3f ms)",
                request->face,
                (uint64_t)result->size,
                (double)result->load_us / 1000.0);
            FaceList[request->face].flags &= ~FACE_REQUESTED;
            face_asset_request_remove(request);
            *redraw = true;
        } else if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
            face_asset_request_fail(request, "integrity or decode retry limit reached");
        } else {
            face_asset_request_retry(request, now_ms, request->foreground || result->urgent);
        }
        face_loader_result_free(result);
    }
}

void image_face_requests_service(void) {
    face_cache_report_failures();
    if (face_asset_requests == NULL) {
        return;
    }
    uint64_t started = face_completion_now_us();
    uint64_t now_ms = SDL_GetTicks();
    bool redraw = false;
    if (!face_loader_available() && now_ms >= face_asset_loader_retry_at_ms) {
        if (face_asset_loader_start()) {
            face_asset_loader_retry_at_ms = 0;
        } else {
            face_asset_loader_retry_at_ms = now_ms + 1000U;
        }
    }
    face_asset_requests_complete_loader(now_ms, started, &redraw);
    bool loader_available = face_loader_available();
    bool transport_available = asset_requests_available();
    if (transport_available) {
        face_asset_requests_complete_transfers(now_ms, started);
        if (loader_available) {
            face_asset_requests_admit(now_ms, started);
        }
    }
    face_asset_requests_schedule_local(started);

    if (redraw) {
        map_redraw_flag = minimap_redraw_flag = 1;
        book_redraw();
        interface_redraw();
        WIDGET_REDRAW_ALL(PDOLL_ID);
        WIDGET_REDRAW_ALL(QUICKSLOT_ID);
        WIDGET_REDRAW_ALL(INVENTORY_ID);
        WIDGET_REDRAW_ALL(ACTIVE_EFFECTS_ID);
    }
    if (face_completion_now_us() - started > FACE_COMPLETION_BUDGET_US) {
        LOG(DEBUG,
            "Face completion exceeded the %.3f ms frame budget",
            (double)FACE_COMPLETION_BUDGET_US / 1000.0);
    }
    face_asset_demand_generation++;
    if (face_asset_demand_generation == 0) {
        face_asset_demand_generation++;
    }
}

/**
 * We got a face - test if we have it loaded. If not, ask the server to
 * send us face command.
 *
 * @param pnum
 * Face ID.
 */
static void image_request_face_internal(int pnum, bool prefetch) {
    uint16_t num = (uint16_t)(pnum & FACE_ID_MASK);

    if (!image_face_valid(num)) {
        image_missing_faces = true;
        LOG(ERROR, "Ignoring invalid face ID %d (normalized: %u)", pnum, num);
        return;
    }

    /* Face zero is the map protocol's empty-layer sentinel, not an asset. */
    if (num == 0) {
        return;
    }

    /* Immutable offline fixtures may preload a verified face without a
     * mutable server bitmap catalog. A loaded face never needs a request. */
    if (FaceList[num].name != NULL) {
        if (!prefetch && (FaceList[num].flags & FACE_REQUESTED) != 0) {
            face_asset_request_promote(num);
        }
        return;
    }

    if (num >= image_bmaps_size) {
        image_missing_faces = true;
        LOG(ERROR,
            "Ignoring unavailable face ID %d (normalized: %u, catalog size: %" PRIu64 ")",
            pnum,
            num,
            (uint64_t)image_bmaps_size);
        return;
    }

    /* Loaded or requested */
    if (FaceList[num].flags & FACE_REQUESTED) {
        if (!prefetch) {
            face_asset_request_promote(num);
        }
        return;
    }

    finish_face_cmd(num, image_bmaps[num].crc32, image_bmaps[num].name);
    if (!prefetch) {
        face_asset_request_promote(num);
    }
}

void image_request_face(int pnum) {
    image_request_face_internal(pnum, false);
}

void image_prefetch_face(int pnum) {
    image_request_face_internal(pnum, true);
}

/**
 * Find a face ID by name. Request the face by finding it, loading it or
 * requesting it.
 *
 * @param name
 * Face name to find.
 * @return
 * Face ID if found, -1 otherwise.
 */
int image_get_id(const char *name) {
    int l = 0, r = image_bmaps_size - 1;

    /* All the faces in ::image_bmaps are already sorted, so we can use a
     * binary search here. */
    while (r >= l) {
        int x = (l + r) / 2;
        int cmp = strcmp(name, image_bmaps[x].name);
        if (cmp < 0) {
            r = x - 1;
        } else if (cmp > 0) {
            l = x + 1;
        } else {
            image_request_face(x);
            return x;
        }
    }

    return -1;
}
