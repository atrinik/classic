/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <stun_config.h>

#include <toolkit/memory.h>
#include <toolkit/toolkit.h>

#include <stdlib.h>
#include <string.h>

void client_stun_config_init(client_stun_config_t *config) {
    HARD_ASSERT(config != NULL);

    config->endpoint = xstrdup(CLIENT_STUN_DEFAULT_ENDPOINT);
    config->source = CLIENT_STUN_SOURCE_DEFAULT;
}

bool client_stun_config_set(client_stun_config_t *config, const char *value, char **errmsg) {
    HARD_ASSERT(config != NULL);
    HARD_ASSERT(value != NULL);

    if (value[0] == '\0' || strlen(value) >= MAX_BUF) {
        if (errmsg != NULL) {
            *errmsg =
                xstrdup(value[0] == '\0' ? "STUN endpoint is empty" : "STUN endpoint is too long");
        }
        return false;
    }

    char *endpoint = strcmp(value, "off") == 0 ? NULL : xstrdup(value);
    free(config->endpoint);
    config->endpoint = endpoint;
    config->source = endpoint == NULL ? CLIENT_STUN_SOURCE_DISABLED : CLIENT_STUN_SOURCE_OVERRIDE;
    return true;
}

void client_stun_config_deinit(client_stun_config_t *config) {
    if (config == NULL) {
        return;
    }
    free(config->endpoint);
    config->endpoint = NULL;
    config->source = CLIENT_STUN_SOURCE_DISABLED;
}

const char *client_stun_source_name(client_stun_source_t source) {
    switch (source) {
        case CLIENT_STUN_SOURCE_DEFAULT:
            return "packaged default";
        case CLIENT_STUN_SOURCE_OVERRIDE:
            return "operator override";
        case CLIENT_STUN_SOURCE_DISABLED:
            return "disabled";
    }
    return "unknown";
}
