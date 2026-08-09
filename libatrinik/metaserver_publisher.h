/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef TOOLKIT_METASERVER_PUBLISHER_H
#define TOOLKIT_METASERVER_PUBLISHER_H

#include "toolkit.h"
#include <openssl/types.h>

#define METASERVER_PUBLISH_AUTHORITY "publish.meta.atrinik.org"
#define METASERVER_PUBLISH_CONTENT_TYPE "application/json"
#define METASERVER_PUBLISH_BODY_MAX 4096U
#define METASERVER_PUBLISH_CERTIFICATE_DER_MAX 2048U
#define METASERVER_PUBLISH_CERTIFICATE_BASE64_MAX 2736U
#define METASERVER_PUBLISH_SIGNATURE_INPUT_MAX 1024U
#define METASERVER_PUBLISH_SIGNATURE_BASE_MAX 2048U
#define METASERVER_PUBLISH_SIGNATURE_HEADER_MAX 128U
#define METASERVER_PUBLISH_NONCE_SIZE 16U
#define METASERVER_PUBLISH_VALIDITY_SECONDS 300U

typedef enum metaserver_publisher_profile {
    METASERVER_PUBLISHER_CLASSIC_V1,
    METASERVER_PUBLISHER_GAME_V1
} metaserver_publisher_profile_t;

typedef enum metaserver_publish_sequence_result {
    METASERVER_PUBLISH_SEQUENCE_OK,
    METASERVER_PUBLISH_SEQUENCE_EXHAUSTED,
    METASERVER_PUBLISH_SEQUENCE_ERROR
} metaserver_publish_sequence_result_t;

typedef struct metaserver_publisher_components {
    char path[128];
    char content_digest[64];
    char signature_input[METASERVER_PUBLISH_SIGNATURE_INPUT_MAX];
    char signature_base[METASERVER_PUBLISH_SIGNATURE_BASE_MAX];
} metaserver_publisher_components_t;

typedef struct metaserver_publisher_classic_payload {
    const char *server_id;
    const char *certificate;
    const char *name;
    uint32_t players_count;
    const char *version;
    const char *text_comment;
    bool is_public;
    bool password_required;
} metaserver_publisher_classic_payload_t;

typedef struct metaserver_publisher_identity metaserver_publisher_identity_t;

/**
 * Render the exact canonical classic JSON body.
 *
 * All payload strings are borrowed for the call. The caller owns `body`,
 * which is cleared before validation and contains the exact signed bytes on
 * success. The function retains no pointers and is thread-safe.
 */
bool metaserver_publisher_classic_body(const metaserver_publisher_classic_payload_t *payload,
                                       char body[METASERVER_PUBLISH_BODY_MAX + 1U],
                                       size_t *body_size);

/** Construct exact signed components for borrowed body bytes. Output is
 * caller-owned, contains no private material, and is cleared before use. */
bool metaserver_publisher_build(metaserver_publisher_profile_t profile,
                                const char *authority,
                                const char *server_id,
                                uint64_t sequence,
                                const unsigned char nonce[METASERVER_PUBLISH_NONCE_SIZE],
                                uint64_t created,
                                const void *body,
                                size_t body_size,
                                metaserver_publisher_components_t *components);

/** Retain an immutable publisher identity from a borrowed certificate/key pair.
 *
 * The pair must match, use P-256, and bind to the exact certificate-derived
 * `server_id`. The returned identity owns independent OpenSSL references;
 * NULL reports validation or allocation failure. */
metaserver_publisher_identity_t *
metaserver_publisher_identity_create(X509 *certificate, EVP_PKEY *key, const char *server_id);

/** Borrow the identity-owned canonical certificate base64 until free. */
const char *
metaserver_publisher_identity_certificate(const metaserver_publisher_identity_t *identity);

/** Sign a borrowed canonical base; the caller owns the output header. */
bool metaserver_publisher_identity_sign(
    const metaserver_publisher_identity_t *identity,
    const char *signature_base,
    char signature_header[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX]);

/** Free and cleanse an identity. NULL is accepted. */
void metaserver_publisher_identity_free(metaserver_publisher_identity_t *identity);

/** Verify a borrowed certificate/signature tuple without retaining input. */
bool metaserver_publisher_verify(const unsigned char *certificate_der,
                                 size_t certificate_size,
                                 const char *server_id,
                                 const char *signature_base,
                                 const unsigned char signature[64]);

/** Atomically reserve and persist a sequence at least `minimum`.
 *
 * The two owner-only files are stored below `data_path` and are bound to the
 * exact lowercase certificate-derived `server_id`; gaps are valid and a
 * returned sequence is consumed even if no request is sent. Callers must
 * serialize reserve/recover operations for the same server identity. */
metaserver_publish_sequence_result_t metaserver_publish_sequence_reserve(const char *data_path,
                                                                         const char *server_id,
                                                                         uint64_t minimum,
                                                                         uint64_t *sequence);

/** Raise the persisted high-water mark so the next reservation is at least
 * `minimum_next_sequence`. State never moves backwards. */
metaserver_publish_sequence_result_t
metaserver_publish_sequence_recover(const char *data_path,
                                    const char *server_id,
                                    uint64_t minimum_next_sequence);

/** Parse the exact bounded replay response and return its canonical uint64
 * minimum. The response is non-secret and borrowed for the call. */
bool metaserver_publish_replay_parse(const char *body,
                                     size_t body_size,
                                     uint64_t *minimum_next_sequence);

#endif
