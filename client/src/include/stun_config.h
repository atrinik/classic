/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#ifndef STUN_CONFIG_H
#define STUN_CONFIG_H

#include <stdbool.h>

#define CLIENT_STUN_DEFAULT_ENDPOINT "stun.cloudflare.com:3478"

typedef enum client_stun_source {
    CLIENT_STUN_SOURCE_DEFAULT,
    CLIENT_STUN_SOURCE_OVERRIDE,
    CLIENT_STUN_SOURCE_DISABLED,
} client_stun_source_t;

typedef struct client_stun_config {
    char *endpoint;
    client_stun_source_t source;
} client_stun_config_t;

void client_stun_config_init(client_stun_config_t *config);
bool client_stun_config_set(client_stun_config_t *config, const char *value, char **errmsg);
void client_stun_config_deinit(client_stun_config_t *config);
const char *client_stun_source_name(client_stun_source_t source);

#endif
