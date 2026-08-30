/**
 * @file
 * Classic static-directory protocol 4 adapter.
 */

#include "metaserver_directory.h"
#include "metaserver_private.h"

#include <toolkit/memory.h>

bool metaserver_direct_parse(const char *body,
                             size_t body_size,
                             const char *rendezvous_origin,
                             uint64_t now,
                             uint64_t minimum_generation,
                             uint64_t *accepted_generation) {
    metaserver_directory_snapshot_t *snapshot = NULL;
    if (!metaserver_directory_parse(body, body_size, &snapshot) ||
        !metaserver_directory_current(snapshot, now) || snapshot->generation < minimum_generation) {
        metaserver_directory_free(snapshot);
        return false;
    }

    server_struct *servers[METASERVER_DIRECTORY_SERVERS_MAX] = {0};
    for (size_t i = 0; i < snapshot->servers_count; i++) {
        const metaserver_directory_entry_t *entry = &snapshot->servers[i];
        server_struct *server = xcalloc(1, sizeof(*server));
        servers[i] = server;
        server->is_meta = true;
        server->direct = true;
        server->player_known = true;
        server->player = entry->players_count;
        server->server_id = xstrdup(entry->server_id);
        server->quic_certificate_sha256 = xstrdup(entry->server_id);
        server->rendezvous_origin = xstrdup(rendezvous_origin);
        server->name = xstrdup(entry->name);
        server->version = xstrdup(entry->version);
        server->desc = xstrdup(entry->text_comment);
        server->password_required = entry->password_required;
        if (entry->has_endpoint) {
            server->hostname = xstrdup(entry->hostname);
            server->port = entry->port;
        }
    }

    for (size_t i = snapshot->servers_count; i > 0; i--) {
        metaserver_server_add(servers[i - 1]);
    }
    if (accepted_generation != NULL) {
        *accepted_generation = snapshot->generation;
    }
    metaserver_directory_free(snapshot);
    return true;
}
