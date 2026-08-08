/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#ifndef JOIN_CREDENTIALS_H
#define JOIN_CREDENTIALS_H

#include <stdbool.h>
#include <toolkit/rendezvous.h>

/** True only when no prompt, command-line, or existing per-server password exists. */
bool client_join_password_missing(const char *prompt, const char *configured, const char *existing);
/** Idempotently cleanse and free selected/configured session passwords. */
void client_join_credentials_clear(char **selected, char **configured);
/** Also cleanse/free the selected server's one-attempt rendezvous invite. */
void client_attempt_secrets_clear(char **selected, char **configured, rendezvous_invite_t **invite);

#endif
