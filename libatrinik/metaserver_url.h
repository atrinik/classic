/**
 * @file
 * Canonical metaserver service URL construction.
 */

#ifndef TOOLKIT_METASERVER_URL_H
#define TOOLKIT_METASERVER_URL_H

#include "toolkit.h"

/** Validate a canonical lowercase DNS hostname and reject every numeric form. */
bool metaserver_hostname_valid(const char *hostname);

/**
 * Validate one complete static classic-directory HTTPS/HTTP URL.
 *
 * The path must name an object rather than an origin. Userinfo, query,
 * fragment, percent encoding, path traversal, and noncanonical serialization
 * are rejected.
 */
bool metaserver_url_directory_valid(const char *url);

/**
 * Combine a canonical publisher origin with a signed request path.
 *
 * The origin has no path beyond an optional slash. The authority is returned
 * exactly as it must appear in the HTTP signature. Both outputs are cleared
 * on failure.
 */
bool metaserver_url_publish(const char *origin,
                            const char *path,
                            char *url,
                            size_t url_size,
                            char *authority,
                            size_t authority_size);

/**
 * Build the exact ticket-scoped rendezvous WebSocket URL.
 *
 * An intentional path prefix and optional trailing slash are preserved before
 * appending /servers/{server_id}. Only lowercase 64-hex identities and the
 * closed client/server role set are accepted. Userinfo, query, fragment,
 * encoded or traversal paths, unsupported schemes, and output overflow fail
 * with an empty output. The caller owns all buffers.
 */
bool metaserver_url_rendezvous(const char *origin,
                               const char *server_id,
                               const char *role,
                               char *url,
                               size_t url_size);

#endif
