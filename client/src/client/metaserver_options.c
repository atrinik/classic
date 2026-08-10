/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <metaserver_options.h>

#include <toolkit/memory.h>
#include <toolkit/toolkit.h>

#include <stdlib.h>

static void client_metaserver_options_clear(client_metaserver_options_t *options) {
    for (size_t i = 0; i < options->count; i++) {
        free(options->endpoints[i].directory_url);
        free(options->endpoints[i].rendezvous_origin);
    }
    free(options->endpoints);
    options->endpoints = NULL;
    options->count = 0;
}

void client_metaserver_options_add(client_metaserver_options_t *options,
                                   const char *directory_url,
                                   const char *rendezvous_origin) {
    HARD_ASSERT(options != NULL);
    HARD_ASSERT(directory_url != NULL);
    HARD_ASSERT(rendezvous_origin != NULL);

    options->endpoints =
        xreallocarray(options->endpoints, options->count + 1, sizeof(*options->endpoints));
    client_metaserver_endpoint_t *endpoint = &options->endpoints[options->count];
    endpoint->directory_url = xstrdup(directory_url);
    endpoint->rendezvous_origin = xstrdup(rendezvous_origin);
    options->count++;
    options->disabled = false;
}

void client_metaserver_options_disable(client_metaserver_options_t *options) {
    HARD_ASSERT(options != NULL);

    client_metaserver_options_clear(options);
    options->disabled = true;
}

bool client_metaserver_options_enabled(const client_metaserver_options_t *options) {
    HARD_ASSERT(options != NULL);
    return !options->disabled;
}

void client_metaserver_options_deinit(client_metaserver_options_t *options) {
    if (options == NULL) {
        return;
    }
    client_metaserver_options_clear(options);
    options->disabled = false;
}
