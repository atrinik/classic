/**
 * @file
 * Transactional classic static-directory protocol 4 parser.
 */

#ifndef CLIENT_METASERVER_DIRECTORY_H
#define CLIENT_METASERVER_DIRECTORY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define METASERVER_DIRECTORY_BODY_MAX (4U * 1024U * 1024U)
#define METASERVER_DIRECTORY_SERVERS_MAX 512U
#define METASERVER_DIRECTORY_CLOCK_SKEW_SECONDS 300U

typedef struct metaserver_directory_entry {
    char server_id[65];
    char name[81];
    uint32_t players_count;
    char version[33];
    char text_comment[257];
    char hostname[254];
    uint16_t port;
    bool has_endpoint;
    bool password_required;
} metaserver_directory_entry_t;

typedef struct metaserver_directory_snapshot {
    uint64_t generation;
    uint64_t generated_at;
    uint64_t expires_at;
    size_t servers_count;
    metaserver_directory_entry_t servers[METASERVER_DIRECTORY_SERVERS_MAX];
} metaserver_directory_snapshot_t;

/** Parse one complete body without changing caller state. */
bool metaserver_directory_parse(const char *body,
                                size_t body_size,
                                metaserver_directory_snapshot_t **snapshot);

/** Determine whether a structurally valid body is currently usable. */
bool metaserver_directory_current(const metaserver_directory_snapshot_t *snapshot, uint64_t now);

/**
 * Enforce monotonic generation replacement.
 *
 * Equal generations identify one exact byte sequence.
 */
bool metaserver_directory_replacement_valid(const metaserver_directory_snapshot_t *candidate,
                                            const char *candidate_body,
                                            size_t candidate_body_size,
                                            const metaserver_directory_snapshot_t *cached,
                                            const char *cached_body,
                                            size_t cached_body_size);

void metaserver_directory_free(metaserver_directory_snapshot_t *snapshot);

#endif
