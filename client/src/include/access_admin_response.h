/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 ************************************************************************/

#ifndef ACCESS_ADMIN_RESPONSE_H
#define ACCESS_ADMIN_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum client_access_admin_operation {
    CLIENT_ACCESS_ADMIN_ISSUE,
    CLIENT_ACCESS_ADMIN_LIST,
    CLIENT_ACCESS_ADMIN_HISTORY,
    CLIENT_ACCESS_ADMIN_REVOKE,
    CLIENT_ACCESS_ADMIN_REMOVE,
    CLIENT_ACCESS_ADMIN_STATUS,
    CLIENT_ACCESS_ADMIN_RESULT,
    CLIENT_ACCESS_ADMIN_OPERATION_COUNT
} client_access_admin_operation_t;

typedef struct client_access_admin_response {
    client_access_admin_operation_t operation;
    char request_id[33];
    char revision[21];
    bool revision_present;
    bool committed;
    bool terminal;
} client_access_admin_response_t;

/** Parse and validate one complete strict access-administration response. */
bool client_access_admin_response_parse(const uint8_t *data,
                                        size_t size,
                                        client_access_admin_response_t *response);

const char *client_access_admin_operation_name(client_access_admin_operation_t operation);

#endif
