/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Encoding and validation for the in-band QUIC asset protocol.
 */

#include "socket.h"
#include "packet.h"

bool socket_asset_face_path_format(char *path, size_t size, uint16_t face) {
    if (path == NULL || size == 0 || face == 0) {
        return false;
    }

    int length = snprintf(path, size, ASSET_FACE_PATH_PREFIX "%u.png", (unsigned int)face);
    return length > 0 && (size_t)length < size;
}

bool socket_asset_face_path_parse(const char *path, uint16_t *face) {
    if (path == NULL || face == NULL ||
        strncmp(path, ASSET_FACE_PATH_PREFIX, sizeof(ASSET_FACE_PATH_PREFIX) - 1U) != 0) {
        return false;
    }

    const char *cursor = path + sizeof(ASSET_FACE_PATH_PREFIX) - 1U;
    if (*cursor == '0') {
        return false;
    }
    uint32_t parsed = 0;
    size_t digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10U + (uint32_t)(*cursor - '0');
        if (parsed > UINT16_MAX) {
            return false;
        }
        cursor++;
        digits++;
    }
    if (digits == 0 || parsed == 0 || strcmp(cursor, ".png") != 0) {
        return false;
    }

    *face = (uint16_t)parsed;
    return true;
}

void socket_asset_request_append(packet_struct *packet,
                                 const char *path,
                                 uint32_t cached_size,
                                 const uint8_t cached_digest[ASSET_DIGEST_SIZE],
                                 uint8_t flags) {
    packet_writer_write_cstring(packet, path);
    packet_writer_write_uint32(packet, cached_size);
    packet_writer_write_bytes(packet, cached_digest, ASSET_DIGEST_SIZE);
    packet_writer_write_uint8(packet, flags);
}

bool socket_asset_request_parse(const uint8_t *data,
                                size_t len,
                                size_t pos,
                                socket_asset_request_t *request) {
    if (request == NULL) {
        return false;
    }

    socket_asset_request_t parsed = {0};
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    packet_reader_read_string(&reader, VS(parsed.path));
    parsed.cached_size = packet_reader_read_uint32(&reader);
    packet_view_t digest = packet_reader_read_view(&reader, ASSET_DIGEST_SIZE);
    parsed.flags = packet_reader_read_uint8(&reader);
    if (!packet_reader_finish(&reader) || *parsed.path == '\0') {
        return false;
    }
    memcpy(parsed.cached_digest, digest.data, digest.len);

    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    if ((parsed.flags & ~ASSET_REQUEST_METADATA) != 0 ||
        ((parsed.flags & ASSET_REQUEST_METADATA) != 0 &&
         (parsed.cached_size != 0 ||
          memcmp(parsed.cached_digest, empty_digest, ASSET_DIGEST_SIZE) != 0))) {
        return false;
    }

    *request = parsed;
    return true;
}

void socket_asset_response_append_metadata(packet_struct *packet,
                                           uint32_t total_size,
                                           const uint8_t digest[ASSET_DIGEST_SIZE]) {
    socket_asset_response_append_status(packet, ASSET_STATUS_METADATA, total_size, digest);
}

void socket_asset_response_append_status(packet_struct *packet,
                                         uint8_t status,
                                         uint32_t total_size,
                                         const uint8_t digest[ASSET_DIGEST_SIZE]) {
    packet_writer_write_uint8(packet, status);
    packet_writer_write_uint32(packet, total_size);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    packet_writer_write_bytes(packet, digest != NULL ? digest : empty_digest, ASSET_DIGEST_SIZE);
}

void socket_asset_response_append_ok(packet_struct *packet,
                                     uint32_t total_size,
                                     const uint8_t digest[ASSET_DIGEST_SIZE]) {
    socket_asset_response_append_status(packet, ASSET_STATUS_OK, total_size, digest);
}

bool socket_asset_response_parse(const uint8_t *data,
                                 size_t len,
                                 size_t pos,
                                 socket_asset_response_t *response) {
    if (response == NULL) {
        return false;
    }

    socket_asset_response_t parsed = {0};
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    parsed.status = packet_reader_read_uint8(&reader);
    parsed.total_size = packet_reader_read_uint32(&reader);
    packet_view_t digest = packet_reader_read_view(&reader, ASSET_DIGEST_SIZE);
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    if (!packet_reader_finish(&reader) || parsed.status > ASSET_STATUS_METADATA_NOT_FOUND ||
        parsed.total_size > ASSET_MAX_SIZE ||
        ((parsed.status == ASSET_STATUS_OK || parsed.status == ASSET_STATUS_METADATA) &&
         digest.len != ASSET_DIGEST_SIZE) ||
        ((parsed.status == ASSET_STATUS_NOT_FOUND ||
          parsed.status == ASSET_STATUS_METADATA_NOT_FOUND) &&
         (parsed.total_size != 0 || memcmp(digest.data, empty_digest, ASSET_DIGEST_SIZE) != 0))) {
        return false;
    }
    memcpy(parsed.digest, digest.data, digest.len);
    *response = parsed;
    return true;
}

static bool socket_face_batch_faces_valid(const uint16_t *faces, size_t count) {
    if (faces == NULL || count == 0 || count > ASSET_FACE_BATCH_MAX) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (faces[i] == 0) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (faces[i] == faces[j]) {
                return false;
            }
        }
    }
    return true;
}

bool socket_face_batch_request_append(packet_struct *packet, const uint16_t *faces, size_t count) {
    if (packet == NULL || !socket_face_batch_faces_valid(faces, count) ||
        packet_writer_error(packet) != PACKET_ERROR_NONE) {
        return false;
    }

    packet_writer_mark_t mark;
    packet_writer_mark(packet, &mark);
    packet_writer_write_uint8(packet, (uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        packet_writer_write_uint16(packet, faces[i]);
    }
    if (!packet_writer_finish(packet)) {
        packet_writer_rollback(packet, &mark);
        return false;
    }
    return true;
}

bool socket_face_batch_request_parse(const uint8_t *data,
                                     size_t len,
                                     size_t pos,
                                     socket_face_batch_request_t *request) {
    if (request == NULL) {
        return false;
    }

    socket_face_batch_request_t parsed = {0};
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    size_t count = 0;
    if (!packet_reader_read_count8(&reader, ASSET_FACE_BATCH_MAX, &count) || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        parsed.faces[i] = packet_reader_read_uint16(&reader);
    }
    if (!packet_reader_finish(&reader) || !socket_face_batch_faces_valid(parsed.faces, count)) {
        return false;
    }

    parsed.count = (uint8_t)count;
    *request = parsed;
    return true;
}

static bool socket_face_batch_response_valid(uint8_t status,
                                             uint32_t body_size,
                                             const uint8_t digest[ASSET_DIGEST_SIZE]) {
    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    if (status == ASSET_STATUS_OK) {
        return body_size != 0 && body_size <= ASSET_FACE_MAX_SIZE && digest != NULL;
    }
    if (status == ASSET_STATUS_NOT_FOUND) {
        return body_size == 0 &&
               (digest == NULL || memcmp(digest, empty_digest, sizeof(empty_digest)) == 0);
    }
    return false;
}

bool socket_face_batch_response_append(packet_struct *packet,
                                       uint16_t face,
                                       uint8_t status,
                                       uint32_t body_size,
                                       const uint8_t digest[ASSET_DIGEST_SIZE]) {
    if (packet == NULL || face == 0 ||
        !socket_face_batch_response_valid(status, body_size, digest) ||
        packet_writer_error(packet) != PACKET_ERROR_NONE) {
        return false;
    }

    static const uint8_t empty_digest[ASSET_DIGEST_SIZE];
    packet_writer_mark_t mark;
    packet_writer_mark(packet, &mark);
    packet_writer_write_uint16(packet, face);
    packet_writer_write_uint8(packet, status);
    packet_writer_write_uint32(packet, body_size);
    packet_writer_write_bytes(packet, digest != NULL ? digest : empty_digest, ASSET_DIGEST_SIZE);
    if (!packet_writer_finish(packet)) {
        packet_writer_rollback(packet, &mark);
        return false;
    }
    return true;
}

bool socket_face_batch_response_parse(const uint8_t *data,
                                      size_t len,
                                      size_t pos,
                                      socket_face_batch_response_t *response) {
    if (response == NULL) {
        return false;
    }

    socket_face_batch_response_t parsed = {0};
    packet_reader_t reader;
    packet_reader_init_at(&reader, data, len, pos);
    parsed.face = packet_reader_read_uint16(&reader);
    parsed.status = packet_reader_read_uint8(&reader);
    parsed.body_size = packet_reader_read_uint32(&reader);
    packet_view_t digest = packet_reader_read_view(&reader, ASSET_DIGEST_SIZE);
    if (!packet_reader_finish(&reader) || parsed.face == 0 ||
        !socket_face_batch_response_valid(parsed.status, parsed.body_size, digest.data)) {
        return false;
    }

    memcpy(parsed.digest, digest.data, sizeof(parsed.digest));
    *response = parsed;
    return true;
}
