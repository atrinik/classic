/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#ifndef METASERVER_DIRECT_H
#define METASERVER_DIRECT_H

#include <stdbool.h>

/** Validate one certificate-pinned direct identity. */
bool metaserver_direct_identity_valid(const char *server_id, const char *certificate_sha256);

#endif
