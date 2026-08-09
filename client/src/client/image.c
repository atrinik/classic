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

#define FACE_REQUEST_MAX 64U
#define FACE_TRANSFER_RETRY_MAX 3U
#define FACE_TRANSFER_RETRY_BASE_MS UINT64_C(250)
#define FACE_ADMISSION_TIMEOUT_MS UINT64_C(10000)
#define FACE_COMPLETION_BUDGET_US UINT64_C(2000)

typedef struct face_asset_request {
    struct face_asset_request *next;
    uint16_t face;
    unsigned int attempts;
    uint64_t queued_ms;
    uint64_t retry_at_ms;
    asset_request_t *asset;
} face_asset_request_t;

typedef struct face_cache_write {
    struct face_cache_write *next;
    char *path;
    uint8_t *data;
    size_t size;
} face_cache_write_t;

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
static size_t face_asset_request_count;
static face_cache_write_t *face_cache_writes;
static face_cache_write_t *face_cache_writes_tail;
static size_t face_cache_write_count;
static size_t face_cache_write_failures;
static SDL_Mutex *face_cache_mutex;
static SDL_Condition *face_cache_condition;
static SDL_Thread *face_cache_thread;
static bool face_cache_stopping;
static char *face_cache_directory;

static void face_asset_request_remove(face_asset_request_t **cursor) {
    face_asset_request_t *request = *cursor;
    *cursor = request->next;
    asset_request_free(request->asset);
    HARD_ASSERT(face_asset_request_count != 0);
    face_asset_request_count--;
    free(request);
}

static void face_asset_requests_clear(void) {
    while (face_asset_requests != NULL) {
        face_asset_request_remove(&face_asset_requests);
    }
}

static void face_asset_request_cancel(uint16_t face) {
    face_asset_request_t **cursor = &face_asset_requests;
    while (*cursor != NULL) {
        if ((*cursor)->face == face) {
            face_asset_request_remove(cursor);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static int SDLCALL face_cache_worker(void *unused) {
    while (true) {
        SDL_LockMutex(face_cache_mutex);
        while (face_cache_writes == NULL && !face_cache_stopping) {
            SDL_WaitCondition(face_cache_condition, face_cache_mutex);
        }
        if (face_cache_stopping) {
            SDL_UnlockMutex(face_cache_mutex);
            break;
        }
        face_cache_write_t *write = face_cache_writes;
        face_cache_writes = write->next;
        if (face_cache_writes == NULL) {
            face_cache_writes_tail = NULL;
        }
        HARD_ASSERT(face_cache_write_count != 0);
        face_cache_write_count--;
        SDL_UnlockMutex(face_cache_mutex);

        if (!path_write_atomic_existing(write->path, write->data, write->size, 0600)) {
            SDL_LockMutex(face_cache_mutex);
            face_cache_write_failures++;
            SDL_UnlockMutex(face_cache_mutex);
        }
        free(write->data);
        free(write->path);
        free(write);
    }
    return 0;
}

static bool face_cache_start(void) {
    if (face_cache_thread != NULL) {
        return true;
    }
    if (face_cache_mutex == NULL) {
        face_cache_mutex = SDL_CreateMutex();
    }
    if (face_cache_condition == NULL) {
        face_cache_condition = SDL_CreateCondition();
    }
    if (face_cache_mutex == NULL || face_cache_condition == NULL) {
        return false;
    }
    if (face_cache_directory == NULL) {
        char *marker = file_path(DIRECTORY_CACHE "/.face-cache-root", "wb");
        face_cache_directory = path_dirname(marker);
        free(marker);
    }
    face_cache_stopping = false;
    face_cache_thread = SDL_CreateThread(face_cache_worker, "face-cache", NULL);
    return face_cache_thread != NULL;
}

static void face_cache_enqueue(const char *name, const uint8_t *data, size_t size) {
    if (!face_cache_start()) {
        LOG(ERROR, "Could not start the face cache writer");
        return;
    }

    SDL_LockMutex(face_cache_mutex);
    if (face_cache_write_count >= FACE_REQUEST_MAX) {
        SDL_UnlockMutex(face_cache_mutex);
        LOG(ERROR, "Skipping face cache write: pending write limit reached");
        return;
    }
    face_cache_write_t *write = xcalloc(1, sizeof(*write));
    write->path = path_join(face_cache_directory, name);
    write->data = xmalloc(size);
    memcpy(write->data, data, size);
    write->size = size;
    if (face_cache_writes_tail != NULL) {
        face_cache_writes_tail->next = write;
    } else {
        face_cache_writes = write;
    }
    face_cache_writes_tail = write;
    face_cache_write_count++;
    SDL_SignalCondition(face_cache_condition);
    SDL_UnlockMutex(face_cache_mutex);
}

static void face_cache_report_failures(void) {
    if (face_cache_mutex == NULL) {
        return;
    }
    SDL_LockMutex(face_cache_mutex);
    size_t failures = face_cache_write_failures;
    face_cache_write_failures = 0;
    SDL_UnlockMutex(face_cache_mutex);
    if (failures != 0) {
        LOG(ERROR,
            "Could not write %" PRIu64 " face cache file%s",
            (uint64_t)failures,
            failures == 1 ? "" : "s");
    }
}

static void face_cache_stop(void) {
    if (face_cache_thread != NULL) {
        SDL_LockMutex(face_cache_mutex);
        face_cache_stopping = true;
        SDL_SignalCondition(face_cache_condition);
        SDL_UnlockMutex(face_cache_mutex);
        SDL_WaitThread(face_cache_thread, NULL);
        face_cache_thread = NULL;
    }
    while (face_cache_writes != NULL) {
        face_cache_write_t *write = face_cache_writes;
        face_cache_writes = write->next;
        free(write->data);
        free(write->path);
        free(write);
    }
    face_cache_writes_tail = NULL;
    face_cache_write_count = 0;
    face_cache_report_failures();
    if (face_cache_condition != NULL) {
        SDL_DestroyCondition(face_cache_condition);
    }
    if (face_cache_mutex != NULL) {
        SDL_DestroyMutex(face_cache_mutex);
    }
    face_cache_condition = NULL;
    face_cache_mutex = NULL;
    face_cache_stopping = false;
    free(face_cache_directory);
    face_cache_directory = NULL;
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
    face_asset_requests_clear();
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

    /* Check private cache first */
    char buf[HUGE_BUF];
    snprintf(VS(buf), DIRECTORY_CACHE "/%s", FaceList[facenum].name);

    char *resolved_cache = file_path(buf, "wb");
    FILE *fp = fopen(resolved_cache, "rb");
    if (fp != NULL) {
        struct stat statbuf;
        bool valid_file = fstat(fileno(fp), &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
                          statbuf.st_size > 0 && (uint64_t)statbuf.st_size <= ASSET_FACE_MAX_SIZE;
        size_t len = valid_file ? (size_t)statbuf.st_size : 0;
        unsigned char *data = valid_file ? xmalloc(len) : NULL;
        if (valid_file && fread(data, 1, len, fp) != len) {
            len = 0;
        }
        if (fclose(fp) != 0) {
            len = 0;
        }
        if (face_asset_validate(data, len, checksum)) {
            FaceList[facenum].sprite = sprite_tryload_file(resolved_cache, 0, NULL);
            if (FaceList[facenum].sprite != NULL) {
                free(data);
                free(resolved_cache);
                return;
            }
        }
        free(data);
        unlink(resolved_cache);
    }
    free(resolved_cache);

    if (face_asset_request_count >= FACE_REQUEST_MAX) {
        LOG(ERROR, "Deferring face %d: pending face request limit reached", facenum);
        free(FaceList[facenum].name);
        FaceList[facenum].name = NULL;
        FaceList[facenum].flags &= ~FACE_REQUESTED;
        return;
    }

    face_asset_request_t *request = xcalloc(1, sizeof(*request));
    request->face = (uint16_t)facenum;
    request->queued_ms = SDL_GetTicks();
    request->next = face_asset_requests;
    face_asset_requests = request;
    face_asset_request_count++;
    FaceList[facenum].flags |= FACE_REQUESTED;
}

static void face_asset_request_fail(face_asset_request_t **cursor, const char *reason) {
    face_asset_request_t *request = *cursor;
    LOG(ERROR,
        "Face %u download failed after %u attempt%s: %s",
        request->face,
        request->attempts,
        request->attempts == 1 ? "" : "s",
        reason);
    image_missing_faces = true;
    face_asset_request_remove(cursor);
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
    return true;
}

static bool face_asset_request_install(face_asset_request_t *request) {
    size_t size = 0;
    const uint8_t *data = asset_request_get_data(request->asset, &size);
    if (!face_asset_validate(data, size, FaceList[request->face].checksum)) {
        return false;
    }

    uint64_t started = datetime_monotonic_us();
    SDL_IOStream *stream = SDL_IOFromConstMem(data, size);
    sprite_struct *sprite = stream != NULL ? sprite_tryload_file(NULL, 0, stream) : NULL;
    if (stream != NULL) {
        SDL_CloseIO(stream);
    }
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

void image_face_requests_service(void) {
    face_cache_report_failures();
    uint64_t started = datetime_monotonic_us();
    uint64_t now_ms = SDL_GetTicks();
    face_asset_request_t **cursor = &face_asset_requests;
    while (*cursor != NULL) {
        face_asset_request_t *request = *cursor;
        if (request->asset == NULL) {
            if (now_ms < request->retry_at_ms) {
                cursor = &request->next;
                continue;
            }
            if (!face_asset_request_start(request, now_ms)) {
                if (now_ms - request->queued_ms >= FACE_ADMISSION_TIMEOUT_MS) {
                    face_asset_request_fail(cursor, "asset-stream admission timed out");
                    continue;
                }
                cursor = &request->next;
                continue;
            }
        }

        asset_request_state_t state = asset_request_get_state(request->asset);
        if (state == ASSET_REQUEST_PENDING) {
            cursor = &request->next;
            continue;
        }
        if (state == ASSET_REQUEST_ERROR) {
            asset_request_free(request->asset);
            request->asset = NULL;
            if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                face_asset_request_fail(cursor, "transfer retry limit reached");
                continue;
            }
            request->retry_at_ms = now_ms + FACE_TRANSFER_RETRY_BASE_MS * request->attempts;
            cursor = &request->next;
            continue;
        }

        if (!face_asset_request_install(request)) {
            asset_request_free(request->asset);
            request->asset = NULL;
            if (request->attempts >= FACE_TRANSFER_RETRY_MAX) {
                face_asset_request_fail(cursor, "integrity or decode retry limit reached");
                continue;
            }
            request->retry_at_ms = now_ms + FACE_TRANSFER_RETRY_BASE_MS * request->attempts;
            cursor = &request->next;
            continue;
        }
        face_asset_request_remove(cursor);
        map_redraw_flag = minimap_redraw_flag = 1;
        book_redraw();
        interface_redraw();
        WIDGET_REDRAW_ALL(PDOLL_ID);
        WIDGET_REDRAW_ALL(QUICKSLOT_ID);
        WIDGET_REDRAW_ALL(INVENTORY_ID);

        /* Only one bounded face is decoded/uploaded and cached per frame. */
        break;
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
static void load_picture_from_pack(int num) {
    FILE *fp = path_fopen(FILE_ATRINIK_P0, "rb");
    if (fp == NULL) {
        LOG(ERROR, "Failed to open %s", FILE_ATRINIK_P0);
        return;
    }

    if (lseek(fileno(fp), image_bmaps[num].pos, SEEK_SET) == -1) {
        LOG(ERROR, "Failed to seek to %ld: %s", image_bmaps[num].pos, strerror(errno));
        fclose(fp);
        return;
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
        return;
    }

    fclose(fp);

    SDL_IOStream *rwop = SDL_IOFromMem(buf, image_bmaps[num].len);
    if (rwop == NULL) {
        LOG(ERROR, "Failed to load image from pack using SDL_IOFromMem(): %s", SDL_GetError());
    } else {
        FaceList[num].sprite = sprite_tryload_file(NULL, 0, rwop);
        SDL_CloseIO(rwop);
    }

    free(buf);
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
    fstat(fileno(fp), &statbuf);
    size_t len = statbuf.st_size;
    unsigned char *data = xmalloc(len);
    len = fread(data, 1, len, fp);

    bool ret = false;
    if (len == 0) {
        goto out;
    }

    if (FaceList[num].sprite != NULL) {
        sprite_free_sprite(FaceList[num].sprite);
    }

    free(FaceList[num].name);
    FaceList[num].name = NULL;

    /* Try to load it. */
    FaceList[num].sprite = sprite_tryload_file(buf, 0, NULL);
    if (FaceList[num].sprite == NULL) {
        goto out;
    }

    FaceList[num].name = xstrdup(buf);
    FaceList[num].checksum = crc32(1L, data, len);
    ret = true;

out:
    free(data);
    fclose(fp);

    return ret;
}

/**
 * We got a face - test if we have it loaded. If not, ask the server to
 * send us face command.
 *
 * @param pnum
 * Face ID.
 */
void image_request_face(int pnum) {
    char buf[MAX_BUF];
    uint16_t num = (uint16_t)(pnum & FACE_ID_MASK);

    if (!image_face_valid(num)) {
        image_missing_faces = true;
        LOG(ERROR, "Ignoring invalid face ID %d (normalized: %u)", pnum, num);
        return;
    }

    /* Immutable offline fixtures may preload a verified face without a
     * mutable server bitmap catalog. A loaded face never needs a request. */
    if (FaceList[num].name != NULL) {
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

    if (setting_get_int(OPT_CAT_DEVEL, OPT_RELOAD_GFX) && load_gfx_user_face(num)) {
        return;
    }

    /* Loaded or requested */
    if (FaceList[num].flags & FACE_REQUESTED) {
        return;
    }

    if (load_gfx_user_face(num)) {
        return;
    }

    if (image_bmaps[num].pos != -1) {
        snprintf(VS(buf), "%s.png", image_bmaps[num].name);
        FaceList[num].name = xstrdup(buf);
        FaceList[num].checksum = image_bmaps[num].crc32;
        load_picture_from_pack(num);
    } else {
        FaceList[num].flags |= FACE_REQUESTED;
        finish_face_cmd(num, image_bmaps[num].crc32, image_bmaps[num].name);
    }
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
