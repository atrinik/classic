/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <metaserver_options.h>

#include <toolkit/metaserver_url.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>

#include <stdlib.h>
#include <string.h>

static const char client_metaserver_identity[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

static bool client_metaserver_options_word(const char **cursor, char *word, size_t word_size) {
    const char *start = *cursor;
    while (*start == ' ') {
        start++;
    }
    const char *end = strchr(start, ' ');
    size_t size = end != NULL ? (size_t)(end - start) : strlen(start);
    if (size == 0 || size >= word_size) {
        return false;
    }
    memcpy(word, start, size);
    word[size] = '\0';
    *cursor = end != NULL ? end : start + size;
    return true;
}

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

bool client_metaserver_options_parse(client_metaserver_options_t *options,
                                     const char *value,
                                     char **errmsg) {
    HARD_ASSERT(options != NULL);
    HARD_ASSERT(value != NULL);

    char directory_url[MAX_BUF];
    char rendezvous_origin[MAX_BUF];
    char rendered[MAX_BUF];
    const char *cursor = value;
    if (!client_metaserver_options_word(&cursor, VS(directory_url)) ||
        !client_metaserver_options_word(&cursor, VS(rendezvous_origin)) || *cursor != '\0' ||
        !metaserver_url_directory_valid(directory_url) ||
        !metaserver_url_rendezvous(rendezvous_origin,
                                   client_metaserver_identity,
                                   "client",
                                   VS(rendered))) {
        if (errmsg != NULL) {
            *errmsg = xstrdup("metaserver requires one canonical directory URL and one canonical "
                              "rendezvous origin");
        }
        return false;
    }

    client_metaserver_options_add(options, directory_url, rendezvous_origin);
    return true;
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
