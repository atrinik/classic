/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#ifndef METASERVER_OPTIONS_H
#define METASERVER_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

/** One trusted static-directory and dynamic-rendezvous endpoint pair. */
typedef struct client_metaserver_endpoint {
    char *directory_url;
    char *rendezvous_origin;
} client_metaserver_endpoint_t;

/** Ordered metaserver endpoints assembled from configuration and CLI options. */
typedef struct client_metaserver_options {
    client_metaserver_endpoint_t *endpoints;
    size_t count;
    bool disabled;
} client_metaserver_options_t;

/**
 * Append one copied endpoint pair, re-enabling a previously disabled list.
 *
 * A zero-initialized options structure is ready for use. The caller retains
 * ownership of both input strings.
 */
void client_metaserver_options_add(client_metaserver_options_t *options,
                                   const char *directory_url,
                                   const char *rendezvous_origin);

/** Disable metaserver access and discard every previously configured pair. */
void client_metaserver_options_disable(client_metaserver_options_t *options);

/** Return whether metaserver access is enabled. */
bool client_metaserver_options_enabled(const client_metaserver_options_t *options);

/** Free all copied endpoint pairs and restore the zero-initialized state. */
void client_metaserver_options_deinit(client_metaserver_options_t *options);

#endif
