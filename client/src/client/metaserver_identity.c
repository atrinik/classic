/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <metaserver_direct.h>

#include <string.h>
#include <toolkit/rendezvous.h>
#include <toolkit/string.h>

bool metaserver_direct_identity_valid(const char *server_id, const char *certificate_sha256) {
    return string_is_hex_fixed(server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) &&
           string_is_hex_fixed(certificate_sha256, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) &&
           strcmp(server_id, certificate_sha256) == 0;
}
