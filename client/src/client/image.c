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

#define FACE_ASSET_ADMISSION_MAX ASSET_STREAM_ACTIVE_MAX
#define FACE_TRANSFER_RETRY_MAX 3U
#ifndef FACE_TRANSFER_RETRY_BASE_MS
#define FACE_TRANSFER_RETRY_BASE_MS UINT64_C(250)
#endif
#ifndef FACE_ADMISSION_RETRY_MS
#define FACE_ADMISSION_RETRY_MS UINT64_C(16)
#endif
#ifndef FACE_COMPLETION_BUDGET_US
#define FACE_COMPLETION_BUDGET_US UINT64_C(2000)
#endif

typedef struct face_asset_request {
    struct face_asset_request *prev;
    struct face_asset_request *next;
    uint16_t face;
    unsigned int attempts;
    uint64_t retry_at_ms;
    asset_request_t *asset;
    bool foreground;
    bool local_checked;
    uint8_t admitted_slot;
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
/** Next queued request whose connection-independent sources need inspection. */
static face_asset_request_t *face_asset_prepare_cursor;
static face_asset_request_t *face_asset_request_index[MAX_FACE_TILES];
static face_asset_request_t *face_asset_admitted[FACE_ASSET_ADMISSION_MAX];
static size_t face_asset_request_count;
static size_t face_asset_admitted_count;
static size_t face_asset_unprepared_count;

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
}

static void face_asset_request_insert_foreground(face_asset_request_t *request) {
    request->foreground = true;
    if (face_asset_foreground_tail != NULL) {
        request->prev = face_asset_foreground_tail;
        request->next = face_asset_foreground_tail->next;
        face_asset_foreground_tail->next = request;
    } else {
        request->next = face_asset_requests;
        face_asset_requests = request;
    }
    if (request->next != NULL) {
        request->next->prev = request;
    } else {
        face_asset_requests_tail = request;
    }
    face_asset_foreground_tail = request;
}

static void face_asset_request_remove(face_asset_request_t *request) {
    HARD_ASSERT(face_asset_request_index[request->face] == request);
    face_asset_request_index[request->face] = NULL;
    if (!request->local_checked) {
        HARD_ASSERT(face_asset_unprepared_count != 0);
        face_asset_unprepared_count--;
    }
    if (face_asset_prepare_cursor == request) {
        face_asset_prepare_cursor = request->next;
    }
    face_asset_request_release(request);
    face_asset_request_detach(request);
    if (face_asset_prepare_cursor == NULL && face_asset_unprepared_count != 0) {
        face_asset_prepare_cursor = face_asset_requests;
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
    HARD_ASSERT(face_asset_prepare_cursor == NULL);
    HARD_ASSERT(face_asset_request_count == 0);
    HARD_ASSERT(face_asset_admitted_count == 0);
    HARD_ASSERT(face_asset_unprepared_count == 0);
}

static void face_asset_request_promote(uint16_t face) {
    face_asset_request_t *request = face_asset_request_index[face];
    if (request != NULL && !request->foreground) {
        face_asset_request_detach(request);
        request->retry_at_ms = request->attempts == 0 ? 0 : request->retry_at_ms;
        face_asset_request_insert_foreground(request);
        if (!request->local_checked &&
            (face_asset_prepare_cursor == NULL || !face_asset_prepare_cursor->foreground ||
             face_asset_prepare_cursor->local_checked)) {
            face_asset_prepare_cursor = request;
        }
    }
}

static void face_asset_request_cancel(uint16_t face) {
    face_asset_request_t *request = face_asset_request_index[face];
    if (request != NULL) {
        face_asset_request_remove(request);
    }
}

static sprite_struct *face_asset_decode(const uint8_t *data, size_t size) {
    SDL_IOStream *stream = SDL_IOFromConstMem(data, size);
    sprite_struct *sprite = stream != NULL ? sprite_tryload_file(NULL, 0, stream) : NULL;
    if (stream != NULL) {
        SDL_CloseIO(stream);
    }
    return sprite;
}

static bool load_picture_from_pack(uint16_t num);
static bool load_gfx_user_face(uint16_t num);

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
    if (!face_cache_start()) {
        LOG(ERROR, "Could not start the face cache writer");
    }
}

/**
 * Deinitialize the bmaps.
 */
void image_bmaps_deinit(void) {
    image_face_requests_clear();
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
    HARD_ASSERT(face_asset_request_index[request->face] == NULL);
    face_asset_request_index[request->face] = request;
    face_asset_request_append_background(request);
    /* FACE_REQUESTED deduplicates this backlog, so the fixed face table is
     * also its hard memory bound. */
    face_asset_request_count++;
    HARD_ASSERT(face_asset_request_count <= MAX_FACE_TILES);
    FaceList[facenum].flags |= FACE_REQUESTED;
}

static bool face_asset_request_prepare(face_asset_request_t *request) {
    uint16_t face = request->face;
    if (load_gfx_user_face(face)) {
        return true;
    }
    if (image_bmaps[face].pos != -1 && load_picture_from_pack(face)) {
        return true;
    }

    char path[HUGE_BUF];
    snprintf(VS(path), DIRECTORY_CACHE "/%s", FaceList[face].name);
    char *resolved_cache = file_path(path, "wb");
    FILE *fp = fopen(resolved_cache, "rb");
    if (fp == NULL) {
        free(resolved_cache);
        return false;
    }

    struct stat statbuf;
    bool valid_file = fstat(fileno(fp), &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
                      statbuf.st_size > 0 && (uint64_t)statbuf.st_size <= ASSET_FACE_MAX_SIZE;
    size_t size = valid_file ? (size_t)statbuf.st_size : 0;
    uint8_t *data = valid_file ? xmalloc(size) : NULL;
    if (valid_file && fread(data, 1, size, fp) != size) {
        size = 0;
    }
    if (fclose(fp) != 0) {
        size = 0;
    }

    sprite_struct *sprite = NULL;
    if (face_asset_validate(data, size, FaceList[face].checksum)) {
        sprite = face_asset_decode(data, size);
    }
    free(data);
    if (sprite == NULL) {
        unlink(resolved_cache);
    } else {
        sprite_free_sprite(FaceList[face].sprite);
        FaceList[face].sprite = sprite;
    }
    free(resolved_cache);
    return sprite != NULL;
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

static bool face_asset_request_start(face_asset_request_t *request, uint64_t now_ms) {
    char path[32];
    if (!socket_asset_face_path_format(VS(path), request->face)) {
        return false;
    }
    request->asset = asset_request_start_bounded(path, ASSET_FACE_MAX_SIZE);
    if (request->asset == NULL) {
        return false;
    }
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
    return true;
}

static bool face_asset_request_install(face_asset_request_t *request) {
    size_t size = 0;
    const uint8_t *data = asset_request_get_data(request->asset, &size);
    if (!face_asset_validate(data, size, FaceList[request->face].checksum)) {
        return false;
    }

    uint64_t started = datetime_monotonic_us();
    sprite_struct *sprite = face_asset_decode(data, size);
    if (sprite == NULL) {
        return false;
    }

    face_cache_enqueue(FaceList[request->face].name, data, size);

    sprite_free_sprite(FaceList[request->face].sprite);
    FaceList[request->face].sprite = sprite;
    LOG(DEBUG,
        "Installed face %u (%" PRIu64 " bytes) in %.3f ms",
        request->face,
        (uint64_t)size,
        (double)(datetime_monotonic_us() - started) / 1000.0);
    return true;
}

static bool face_asset_request_prepare_local(face_asset_request_t *request, bool *redraw) {
    bool installed = face_asset_request_prepare(request);
    request->local_checked = true;
    HARD_ASSERT(face_asset_unprepared_count != 0);
    face_asset_unprepared_count--;
    if (installed) {
        FaceList[request->face].flags &= ~FACE_REQUESTED;
        face_asset_request_remove(request);
        *redraw = true;
    }
    return installed;
}

static void face_asset_requests_prepare_local(uint64_t started_us, bool *redraw) {
    face_asset_request_t *request = face_asset_prepare_cursor;
    while (request != NULL && face_asset_unprepared_count != 0 &&
           datetime_monotonic_us() - started_us < FACE_COMPLETION_BUDGET_US) {
        face_asset_prepare_cursor = request->next != NULL ? request->next : face_asset_requests;

        if (!request->local_checked) {
            face_asset_request_prepare_local(request, redraw);
        }
        request = face_asset_prepare_cursor;
    }
    if (face_asset_unprepared_count == 0) {
        face_asset_prepare_cursor = NULL;
    }
}

static void
face_asset_requests_prepare_for_admission(uint64_t now_ms, uint64_t started_us, bool *redraw) {
    size_t slots_needed = FACE_ASSET_ADMISSION_MAX - face_asset_admitted_count;
    face_asset_request_t *request = face_asset_requests;
    while (request != NULL && slots_needed != 0 &&
           datetime_monotonic_us() - started_us < FACE_COMPLETION_BUDGET_US) {
        face_asset_request_t *next = request->next;
        if (request->asset == NULL) {
            if (!request->local_checked && face_asset_request_prepare_local(request, redraw)) {
                request = next;
                continue;
            }
            if (now_ms >= request->retry_at_ms) {
                slots_needed--;
            } else if (request->attempts == 0) {
                /* An admission retry applies to the shared scheduler, so do
                 * not perform network-oriented preparation behind it. */
                return;
            }
        }
        request = next;
    }
}

static void face_asset_requests_admit(uint64_t now_ms, uint64_t started_us) {
    while (face_asset_admitted_count < FACE_ASSET_ADMISSION_MAX &&
           datetime_monotonic_us() - started_us < FACE_COMPLETION_BUDGET_US) {
        face_asset_request_t *request = face_asset_requests;
        while (request != NULL) {
            if (datetime_monotonic_us() - started_us >= FACE_COMPLETION_BUDGET_US) {
                return;
            }
            if (request->asset == NULL) {
                if (!request->local_checked) {
                    return;
                }
                if (now_ms >= request->retry_at_ms) {
                    break;
                }
                if (request->attempts == 0) {
                    return;
                }
            }
            request = request->next;
        }
        if (request == NULL) {
            return;
        }

        if (!face_asset_request_start(request, now_ms)) {
            /* Expected shared-scheduler pressure is retried at frame scale. */
            request->retry_at_ms = now_ms + FACE_ADMISSION_RETRY_MS;
            return;
        }
    }
}

static void face_asset_requests_complete(uint64_t now_ms, uint64_t started_us, bool *redraw) {
    for (size_t slot = 0; slot < FACE_ASSET_ADMISSION_MAX; slot++) {
        face_asset_request_t *request = face_asset_admitted[slot];
        if (request == NULL) {
            continue;
        }
        asset_request_state_t state = asset_request_get_state(request->asset);
        if (state == ASSET_REQUEST_PENDING) {
            continue;
        }
        if (state == ASSET_REQUEST_ERROR) {
            face_asset_request_release(request);
            if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                face_asset_request_fail(request, "transfer retry limit reached");
                continue;
            }
            request->retry_at_ms = now_ms + FACE_TRANSFER_RETRY_BASE_MS * request->attempts;
            continue;
        }

        if (!face_asset_request_install(request)) {
            face_asset_request_release(request);
            if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                face_asset_request_fail(request, "integrity or decode retry limit reached");
                continue;
            }
            request->retry_at_ms = now_ms + FACE_TRANSFER_RETRY_BASE_MS * request->attempts;
            continue;
        }
        FaceList[request->face].flags &= ~FACE_REQUESTED;
        face_asset_request_remove(request);
        *redraw = true;

        /* Drain the bounded set of completed streams while there is still
         * room in this frame's decode/upload budget. */
        if (datetime_monotonic_us() - started_us >= FACE_COMPLETION_BUDGET_US) {
            break;
        }
    }
}

void image_face_requests_service(void) {
    face_cache_report_failures();
    if (face_asset_requests == NULL) {
        return;
    }
    uint64_t started = datetime_monotonic_us();
    uint64_t now_ms = SDL_GetTicks();
    bool redraw = false;
    bool transport_available = asset_requests_available();
    if (transport_available) {
        face_asset_requests_complete(now_ms, started, &redraw);
        /* Fill free streams from cold requests whose local sources were
         * already inspected before doing any more synchronous local work. */
        face_asset_requests_admit(now_ms, started);
        if (face_asset_admitted_count < FACE_ASSET_ADMISSION_MAX) {
            face_asset_requests_prepare_for_admission(now_ms, started, &redraw);
            face_asset_requests_admit(now_ms, started);
        }
    }
    /* Local cache/pack discovery remains useful offline and while every
     * network stream is occupied. The persistent cursor bounds repeat work. */
    face_asset_requests_prepare_local(started, &redraw);

    if (redraw) {
        map_redraw_flag = minimap_redraw_flag = 1;
        book_redraw();
        interface_redraw();
        WIDGET_REDRAW_ALL(PDOLL_ID);
        WIDGET_REDRAW_ALL(QUICKSLOT_ID);
        WIDGET_REDRAW_ALL(INVENTORY_ID);
    }
    if (datetime_monotonic_us() - started > FACE_COMPLETION_BUDGET_US) {
        LOG(DEBUG,
            "Face completion exceeded the %.3f ms frame budget",
            (double)FACE_COMPLETION_BUDGET_US / 1000.0);
    }
}

/**
 * Load picture from the image pack file.
 *
 * @param num
 * ID of the picture to load.
 */
static bool load_picture_from_pack(uint16_t num) {
    FILE *fp = path_fopen(FILE_ATRINIK_P0, "rb");
    if (fp == NULL) {
        LOG(ERROR, "Failed to open %s", FILE_ATRINIK_P0);
        return false;
    }

    if (lseek(fileno(fp), image_bmaps[num].pos, SEEK_SET) == -1) {
        LOG(ERROR, "Failed to seek to %ld: %s", image_bmaps[num].pos, strerror(errno));
        fclose(fp);
        return false;
    }

    char *buf = xmalloc(image_bmaps[num].len);
    size_t num_read = fread(buf, 1, image_bmaps[num].len, fp);
    if (num_read != image_bmaps[num].len) {
        LOG(ERROR,
            "Expected %" PRIu64 " bytes but read %" PRIu64 " bytes",
            (uint64_t)image_bmaps[num].len,
            (uint64_t)num_read);
        free(buf);
        fclose(fp);
        return false;
    }

    fclose(fp);

    sprite_struct *sprite = face_asset_decode((const uint8_t *)buf, image_bmaps[num].len);
    if (sprite != NULL) {
        sprite_free_sprite(FaceList[num].sprite);
        FaceList[num].sprite = sprite;
    }

    free(buf);
    return sprite != NULL;
}

/**
 * Load face from user's graphics directory.
 *
 * @param num
 * ID of the face to load.
 * @return
 * True on success, false on failure.
 */
static bool load_gfx_user_face(uint16_t num) {
    /* First check for this image in gfx_user directory. */
    char buf[MAX_BUF];
    snprintf(VS(buf), DIRECTORY_GFX_USER "/%s.png", image_bmaps[num].name);

    FILE *fp = path_fopen(buf, "rb");
    if (fp == NULL) {
        return false;
    }

    struct stat statbuf;
    bool valid_file = fstat(fileno(fp), &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
                      statbuf.st_size > 0 && (uint64_t)statbuf.st_size <= ASSET_FACE_MAX_SIZE;
    size_t len = valid_file ? (size_t)statbuf.st_size : 0;
    uint8_t *data = valid_file ? xmalloc(len) : NULL;
    if (valid_file && fread(data, 1, len, fp) != len) {
        len = 0;
    }
    fclose(fp);

    sprite_struct *sprite = len != 0 ? face_asset_decode(data, len) : NULL;
    if (sprite != NULL) {
        sprite_free_sprite(FaceList[num].sprite);
        FaceList[num].sprite = sprite;
        free(FaceList[num].name);
        FaceList[num].name = xstrdup(buf);
        FaceList[num].checksum = crc32(1L, data, len);
    }
    free(data);
    return sprite != NULL;
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
