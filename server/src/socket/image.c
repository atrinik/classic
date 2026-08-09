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
 * This file deals with the image related communication to the client.
 */

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <initialization.h>
#include <loader.h>
#include <toolkit/packet.h>
#include <toolkit/string.h>
#include "zlib.h"
#include <openssl/evp.h>

/** Maximum different face sets. */
#define MAX_FACE_SETS 1

/** Face info structure. */
typedef struct FaceInfo {
    /** Image data */
    uint8_t *data;

    /** Length of the XPM data */
    uint16_t datalen;

    /** Checksum of face data */
    uint32_t checksum;

    /** SHA-256 digest authenticating typed-stream transfers. */
    uint8_t digest[ASSET_DIGEST_SIZE];
} FaceInfo;

/** Face sets structure. */
typedef struct {
    /** Prefix */
    char *prefix;

    /** Full name */
    char *fullname;

    /** Size */
    char *size;

    /** Extension */
    char *extension;

    /** Comment */
    char *comment;

    /** Faces */
    FaceInfo *faces;
} FaceSets;

/** The face sets. */
static FaceSets facesets[MAX_FACE_SETS];

/**
 * Check if a face set is valid.
 * @param fsn
 * The face set number to check
 * @return
 * 1 if the face set is valid, 0 otherwise
 */
int is_valid_faceset(int fsn) {
    if (fsn >= 0 && fsn < MAX_FACE_SETS && facesets[fsn].prefix) {
        return 1;
    }

    return 0;
}

/**
 * Free all the information in face sets.
 */
void free_socket_images(void) {
    int num, q;

    for (num = 0; num < MAX_FACE_SETS; num++) {
        if (facesets[num].prefix) {
            for (q = 0; q < nrofpixmaps; q++) {
                free(facesets[num].faces[q].data);
            }

            free(facesets[num].prefix);
            free(facesets[num].fullname);
            free(facesets[num].size);
            free(facesets[num].extension);
            free(facesets[num].comment);
            free(facesets[num].faces);
        }
    }
}

/** Maximum possible size of a single image in bytes. */
#define MAX_IMAGE_SIZE ASSET_FACE_MAX_SIZE

/**
 * Loads up all the image types into memory.
 *
 * This way, we can easily send them to the client.
 *
 * This function also generates client_bmaps file here.
 *
 * At the moment, Atrinik only uses one face set file, no files like
 * atrinik.1, atrinik.2, etc.
 */
void read_client_images(void) {
    char filename[400], buf[HUGE_BUF], *cp, *cps[7 + 1];
    FILE *infile, *fbmap;
    int num, len, file_num, i;

    memset(facesets, 0, sizeof(facesets));

    snprintf(filename, sizeof(filename), "%s/image_info", settings.libpath);
    infile = fopen(filename, "rb");

    if (!infile) {
        LOG(ERROR, "Unable to open %s", filename);
        exit(1);
    }

    while (fgets(buf, HUGE_BUF - 1, infile) != NULL) {
        if (buf[0] == '#') {
            continue;
        }

        if (string_split(buf, cps, sizeof(cps) / sizeof(*cps), ':') != 7) {
            LOG(ERROR, "Bad line in image_info file: %s", buf);
            exit(1);
        } else {
            len = atoi(cps[0]);

            if (len >= MAX_FACE_SETS) {
                LOG(ERROR, "Too high a setnum in image_info file: %d > %d", len, MAX_FACE_SETS);
                exit(1);
            }

            facesets[len].prefix = xstrdup(cps[1]);
            facesets[len].fullname = xstrdup(cps[2]);
            facesets[len].size = xstrdup(cps[4]);
            facesets[len].extension = xstrdup(cps[5]);
            facesets[len].comment = xstrdup(cps[6]);
        }
    }

    fclose(infile);

    /* Loaded the faceset information - now need to load up the
     * actual faces. */
    for (file_num = 0; file_num < MAX_FACE_SETS; file_num++) {
        /* If prefix is not set, this is not used */
        if (!facesets[file_num].prefix) {
            continue;
        }

        facesets[file_num].faces = xcalloc(nrofpixmaps, sizeof(FaceInfo));

        snprintf(filename, sizeof(filename), "%s/atrinik.%d", settings.libpath, file_num);
        snprintf(buf, sizeof(buf), "%s/bmaps", settings.datapath);

        if ((fbmap = fopen(buf, "wb")) == NULL) {
            LOG(ERROR, "Unable to open %s", buf);
            exit(1);
        }

        infile = fopen(filename, "rb");

        if (!infile) {
            LOG(ERROR, "Unable to open %s", filename);
            exit(1);
        }

        while (fgets(buf, HUGE_BUF - 1, infile) != NULL) {
            if (strncmp(buf, "IMAGE ", 6) != 0) {
                LOG(ERROR, "Bad image line - not IMAGE, instead: %s", buf);
                exit(1);
            }

            num = atoi(buf + 6);

            if (num < 0 || num >= nrofpixmaps) {
                LOG(ERROR, "Image num %d not in 0..%d: %s", num, nrofpixmaps, buf);
                exit(1);
            }

            /* Skip across the number data */
            for (cp = buf + 6; *cp != ' '; cp++) {}

            len = atoi(cp);

            if (len <= 0 || (uint32_t)len > MAX_IMAGE_SIZE) {
                LOG(ERROR, "Length not valid: %d > %d: %s", len, MAX_IMAGE_SIZE, buf);
                exit(1);
            }

            /* We don't actually care about the name if the image that
             * is embedded in the image file, so just ignore it. */
            facesets[file_num].faces[num].datalen = len;
            facesets[file_num].faces[num].data = xmalloc(len);

            if ((i = fread(facesets[file_num].faces[num].data, len, 1, infile)) != 1) {
                LOG(ERROR,
                    "Did not read desired amount of data, wanted %d, got %d: %s",
                    len,
                    i,
                    buf);
                exit(1);
            }

            facesets[file_num].faces[num].checksum =
                (uint32_t)crc32(1L, facesets[file_num].faces[num].data, len);
            unsigned int digest_size = 0;
            if (EVP_Digest(facesets[file_num].faces[num].data,
                           (size_t)len,
                           facesets[file_num].faces[num].digest,
                           &digest_size,
                           EVP_sha256(),
                           NULL) != 1 ||
                digest_size != ASSET_DIGEST_SIZE) {
                LOG(ERROR, "Could not hash face %d", num);
                exit(1);
            }
            snprintf(buf,
                     sizeof(buf),
                     "%x %x %s\n",
                     len,
                     facesets[file_num].faces[num].checksum,
                     new_faces[num].name);
            fputs(buf, fbmap);
        }

        fclose(infile);
        fclose(fbmap);
    }
}

void socket_command_ask_face(socket_struct *ns, player *pl, uint8_t *data, size_t len, size_t pos) {
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    (void)packet_reader_read_uint16(&reader);

    /* Retain the command slot so an older peer cannot desynchronize command
     * parsing, but never put an image body on the ordered gameplay stream.
     * Current clients request faces through bounded typed asset streams. */
    (void)ns;
    (void)pl;
}

bool face_get_asset(uint16_t face, const uint8_t **data, uint32_t *size, const uint8_t **digest) {
    if (face == 0 || face >= nrofpixmaps || facesets[0].faces == NULL ||
        facesets[0].faces[face].data == NULL) {
        return false;
    }

    if (data != NULL) {
        *data = facesets[0].faces[face].data;
    }
    if (size != NULL) {
        *size = facesets[0].faces[face].datalen;
    }
    if (digest != NULL) {
        *digest = facesets[0].faces[face].digest;
    }
    return true;
}

/**
 * Get face's data.
 * @param face
 * The face.
 * @param[out] ptr Pointer that will contain the image data, can be NULL.
 * @param[out] len Pointer that will contain the image data length, can
 * be NULL.
 */
void face_get_data(int face, uint8_t **ptr, uint16_t *len) {
    if (ptr) {
        *ptr = facesets[0].faces[face].data;
    }

    if (len) {
        *len = facesets[0].faces[face].datalen;
    }
}
