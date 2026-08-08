/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <join_credentials.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>

bool client_join_password_missing(const char *prompt,
                                  const char *configured,
                                  const char *existing) {
    return (prompt == NULL || *prompt == '\0') && (configured == NULL || *configured == '\0') &&
           (existing == NULL || *existing == '\0');
}

void client_join_credentials_clear(char **selected, char **configured) {
    if (selected != NULL && configured != NULL && selected != configured && *selected != NULL &&
        *selected == *configured) {
        *configured = NULL;
    }
    char **passwords[] = {selected, configured};
    for (size_t i = 0; i < sizeof(passwords) / sizeof(passwords[0]); i++) {
        if (passwords[i] == NULL || *passwords[i] == NULL) {
            continue;
        }
        OPENSSL_cleanse(*passwords[i], strlen(*passwords[i]));
        free(*passwords[i]);
        *passwords[i] = NULL;
    }
}

void client_attempt_secrets_clear(char **selected,
                                  char **configured,
                                  rendezvous_invite_t **invite) {
    client_join_credentials_clear(selected, configured);
    if (invite != NULL && *invite != NULL) {
        rendezvous_invite_cleanse(*invite);
        free(*invite);
        *invite = NULL;
    }
}
