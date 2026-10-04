/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <toolkit/access_resolve.h>
#include <toolkit/metaserver_url.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d\n", __LINE__); return 1; } } while (0)
static const char body[] = "{\"schema\":\"atrinik-access-resolved-v1\",\"profile\":\"classic\",\"serverId\":\"51f341b3c529d7186d2981afd2d8519c6c15eaced3f0291acdfb408f5cba746b\",\"certificate\":\"MIIBRzCB76ADAgECAgEBMAoGCCqGSM49BAMCMCQxIjAgBgNVBAMTGUF0cmluaWsgcHVibGlzaGVyIGZpeHR1cmUwHhcNMjYwMTAxMDAwMDAwWhcNMzYwMTAxMDAwMDAwWjAkMSIwIAYDVQQDExlBdHJpbmlrIHB1Ymxpc2hlciBmaXh0dXJlMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEPQd9J89i9RHBKxJbWY3LVTOACf0pjFZgJTnCMy6BlQ27hfl0xwZf5LkuREpxVbe0xh6ydj5V00OY5Sh5myB4pqMSMBAwDgYDVR0PAQH/BAQDAgeAMAoGCCqGSM49BAMCA0cAMEQCIAbaCO4YENW0H67c1Gj292fvPwTwvwsQ8F5ObWMOv0wyAiArIt0mJkn917HvWQw8HrBVOCyXfGfocde6yjbjwSUcEA==\",\"name\":\"Fixture\",\"accessRequired\":true,\"generation\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"clientNonce\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"grant\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\",\"expiresAt\":\"1800000015\",\"endpoint\":{\"hostname\":\"game.example.net\",\"port\":13327}}";
int main(void) {
    access_resolved_t out;
    char nonce[65]; memset(nonce, 'c', 64); nonce[64] = 0;
    REQUIRE(access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000000, &out));
    REQUIRE(strcmp(out.name, "Fixture") == 0 && out.port == 13327);
    for (size_t n = 0; n < sizeof(body) - 1; n++) {
        memset(&out, 0xa5, sizeof(out));
        REQUIRE(!access_resolve_parse(body, n, nonce, 1800000000, &out));
        REQUIRE(out.server_id[0] == 0 && out.grant.grant[0] == 0);
    }
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000015, &out));
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1799999999, &out));
    nonce[0] = 'a';
    REQUIRE(!access_resolve_parse(body, sizeof(body) - 1, nonce, 1800000000, &out));
    nonce[0] = 'c';
    char changed[sizeof(body) + 5];
    memcpy(changed, body, sizeof(body));
    char *identity = strstr(changed, "\"serverId\":\"");
    identity[12] = identity[12] == 'a' ? 'b' : 'a';
    REQUIRE(!access_resolve_parse(changed, strlen(changed), nonce, 1800000000, &out));
    memcpy(changed, body, sizeof(body));
    changed[sizeof(body) - 1] = ' '; changed[sizeof(body)] = 0;
    REQUIRE(!access_resolve_parse(changed, strlen(changed), nonce, 1800000000, &out));
    char url[1024];
    REQUIRE(metaserver_url_access("https://rendezvous.meta.atrinik.org", NULL, false, url, sizeof(url)));
    REQUIRE(strcmp(url, "https://rendezvous.meta.atrinik.org/v1/access/resolve") == 0);
    REQUIRE(!metaserver_url_access("http://example.net", NULL, false, url, sizeof(url)));
    REQUIRE(!metaserver_url_access("https://example.net/?secret=x", NULL, false, url, sizeof(url)));
    REQUIRE(!metaserver_url_access("https://example.net/base", NULL, false, url, sizeof(url)));
    REQUIRE(metaserver_url_access("http://127.0.0.1:8787", NULL, false, url, sizeof(url)));
    return 0;
}
