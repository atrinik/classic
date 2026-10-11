/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Compile the implementation into this test so its private solver can exercise
 * low difficulty and deterministic clocks; production exports only fixed24. */
#include "../rendezvous_admission.c"
#include <toolkit/logger.h>

#define REQUIRE(x)                                  \
    do {                                            \
        if (!(x)) {                                 \
            fprintf(stderr, "line %d\n", __LINE__); \
            return 1;                               \
        }                                           \
    } while (0)

static const char ticket[] = "2222222222222222222222222222222222222222222222222222222222222222";
static const char server_id[] = "1111111111111111111111111111111111111111111111111111111111111111";
static const char ticket_hash[] =
    "9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4";
/* Independent protocol/fixtures/rendezvous-admission-v1.json vector. */
static const char token[] = "v1.synthetic_current.1800000000."
                            "9f72ea0cf49536e3c66c787f705186df9a4378083753ae9536d65b3ad7fcddc4."
                            "33333333333333333333333333333333."
                            "45548449753e9460a1b06d180786d4472060f536cf1c0f667371d3519abc4f8f";

static bool zeroed(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++)
        if (p[i] != 0)
            return false;
    return true;
}

static int test_vectors(void) {
    char hash[65];
    REQUIRE(rendezvous_admission_ticket_hash(ticket, hash));
    REQUIRE(strcmp(hash, ticket_hash) == 0);
    REQUIRE(!rendezvous_admission_ticket_hash("bad", hash));
    REQUIRE(zeroed(hash, sizeof(hash)));
    char work[512];
    memcpy(work, WORK_DOMAIN, sizeof(WORK_DOMAIN));
    memcpy(work + sizeof(WORK_DOMAIN), token, sizeof(token));
    memcpy(work + sizeof(WORK_DOMAIN) + sizeof(token), "000000000129985a", 16);
    unsigned char digest[32];
    REQUIRE(admission_digest(work, sizeof(WORK_DOMAIN) + sizeof(token) + 16, digest));
    REQUIRE(string_tohex(digest, sizeof(digest), hash, sizeof(hash), false) == 64);
    string_tolower(hash);
    REQUIRE(strcmp(hash, "000000145087ed013d9d9cec3075508e9758271c668b6c37f549d9c7d3dba0f5") == 0);
    return 0;
}

static int test_parse(void) {
    char body[513];
    int n = snprintf(body,
                     sizeof(body),
                     "{\"schema\":\"" ADMISSION_SCHEMA
                     "\",\"challenge\":\"%s\",\"difficulty\":24,\"expiresAt\":\"1800000060\"}",
                     token);
    REQUIRE(n > 0 && (size_t)n < sizeof(body));
    rendezvous_admission_challenge_t out;
    REQUIRE(rendezvous_admission_parse(body, (size_t)n, ticket_hash, 1800000000, &out));
    REQUIRE(strcmp(out.token, token) == 0 && out.expires_at == 1800000060);
    REQUIRE(rendezvous_admission_parse(body, (size_t)n, ticket_hash, 1800000059, &out));
    REQUIRE(!rendezvous_admission_parse(body, (size_t)n, ticket_hash, 1800000060, &out));
    REQUIRE(zeroed(&out, sizeof(out)));
    REQUIRE(!rendezvous_admission_parse(body, (size_t)n, ticket_hash, 1799999999, &out));
    REQUIRE(!rendezvous_admission_parse(body, (size_t)n, ticket, 1800000000, &out));
    for (size_t i = 0; i < (size_t)n; i++) {
        REQUIRE(!rendezvous_admission_parse(body, i, ticket_hash, 1800000000, &out));
        REQUIRE(zeroed(&out, sizeof(out)));
    }
    char changed[514];
    memcpy(changed, body, (size_t)n + 1);
    changed[n] = ' ';
    REQUIRE(!rendezvous_admission_parse(changed, (size_t)n + 1, ticket_hash, 1800000000, &out));
    memcpy(changed, body, (size_t)n + 1);
    char *difficulty = strstr(changed, "difficulty\":24");
    REQUIRE(difficulty != NULL);
    difficulty[strlen("difficulty\":2")] = '3';
    REQUIRE(!rendezvous_admission_parse(changed, (size_t)n, ticket_hash, 1800000000, &out));
    difficulty[strlen("difficulty\":2")] = '5';
    REQUIRE(!rendezvous_admission_parse(changed, (size_t)n, ticket_hash, 1800000000, &out));
    memcpy(changed, body, (size_t)n + 1);
    changed[n - 3] = '1';
    REQUIRE(!rendezvous_admission_parse(changed, (size_t)n, ticket_hash, 1800000000, &out));
    memcpy(changed, body, (size_t)n + 1);
    changed[20] = 0;
    REQUIRE(!rendezvous_admission_parse(changed, (size_t)n, ticket_hash, 1800000000, &out));
    uint64_t expires;
    REQUIRE(!admission_token("v1..1800000000.bad", NULL, 1800000000, &expires));
    REQUIRE(!admission_token("v1.key.01800000000.bad", NULL, 1800000000, &expires));
    REQUIRE(!admission_token("v1.key!.1800000000.bad", NULL, 1800000000, &expires));
    char malformed[sizeof(token)];
    memcpy(malformed, token, sizeof(token));
    malformed[sizeof(token) - 2] = 'F';
    REQUIRE(!admission_token(malformed, NULL, 1800000000, &expires));
    uint64_t decimal = 0;
    REQUIRE(admission_decimal("253402300799", 12, &decimal) && decimal == UINT64_C(253402300799));
    REQUIRE(!admission_decimal("253402300800", 12, &decimal));
    REQUIRE(!admission_decimal("0253402300799", 13, &decimal));
    REQUIRE(!admission_decimal("0", 1, &decimal));
    REQUIRE(!admission_decimal("+1800000000", 11, &decimal));
    REQUIRE(!rendezvous_admission_parse(body, 513, ticket_hash, 1800000000, &out));
    return 0;
}

static uint64_t fake_now;
static unsigned int clock_calls, cancel_calls;
static uint64_t test_clock(void) {
    clock_calls++;
    return fake_now;
}
static uint64_t advancing_clock(void) {
    clock_calls++;
    return fake_now += 10000;
}
static bool cancelled(void *data) {
    unsigned int *limit = data;
    cancel_calls++;
    return cancel_calls >= *limit;
}

static time_t test_wall(time_t *unused) {
    (void)unused;
    return 1800000000;
}

static int test_solver(void) {
    rendezvous_admission_challenge_t challenge = {0};
    memcpy(challenge.token, token, sizeof(token));
    challenge.expires_at = 1800000060;
    char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U];
    fake_now = 1000;
    clock_calls = 0;
    REQUIRE(admission_solve(&challenge, 20000, NULL, proof, 8, 100000, test_clock, test_wall) ==
            RENDEZVOUS_ADMISSION_OK);
    REQUIRE(strncmp(proof, challenge.token, strlen(challenge.token)) == 0);
    REQUIRE(strlen(proof) == strlen(challenge.token) + 17);
    REQUIRE(strcmp(proof + strlen(challenge.token) + 1, "00000000000000a7") == 0);
    const char *solution = proof + strlen(challenge.token) + 1;
    char input[512];
    size_t prefix = sizeof(WORK_DOMAIN) + strlen(challenge.token) + 1;
    memcpy(input, WORK_DOMAIN, sizeof(WORK_DOMAIN));
    memcpy(input + sizeof(WORK_DOMAIN), challenge.token, strlen(challenge.token) + 1);
    memcpy(input + prefix, solution, 16);
    unsigned char digest[32];
    REQUIRE(admission_digest(input, prefix + 16, digest) && digest[0] == 0);
    clock_calls = 0;
    REQUIRE(admission_solve(&challenge, 1000, NULL, proof, 256, 10000, test_clock, test_wall) ==
            RENDEZVOUS_ADMISSION_TIMEOUT);
    /* Read the starting clock and reject at the first pre-work deadline poll. */
    REQUIRE(clock_calls == 2 && zeroed(proof, sizeof(proof)));
    unsigned int cancel_limit = 1;
    curl_cancel_t cancel = {.cancelled = cancelled, .context = &cancel_limit};
    cancel_calls = 0;
    REQUIRE(admission_solve(&challenge, 20000, &cancel, proof, 24, 10000, test_clock, test_wall) ==
            RENDEZVOUS_ADMISSION_CANCELLED);
    REQUIRE(cancel_calls == 1 && zeroed(proof, sizeof(proof)));
    cancel_calls = 0;
    cancel_limit = 2;
    REQUIRE(
        admission_solve(&challenge, 20000, &cancel, proof, 256, 100000, test_clock, test_wall) ==
        RENDEZVOUS_ADMISSION_CANCELLED);
    REQUIRE(cancel_calls == 2 && zeroed(proof, sizeof(proof)));
    fake_now = 0;
    clock_calls = 0;
    REQUIRE(
        admission_solve(&challenge, 999999, NULL, proof, 256, 100000, advancing_clock, test_wall) ==
        RENDEZVOUS_ADMISSION_TIMEOUT);
    /* The solver's own ten-second cap wins over a much later caller deadline. */
    REQUIRE(clock_calls == 2 && fake_now == 20000 && zeroed(proof, sizeof(proof)));
    fake_now = 1000;
    REQUIRE(admission_solve(&challenge, 20000, NULL, proof, 256, 1, test_clock, test_wall) ==
            RENDEZVOUS_ADMISSION_UNAVAILABLE);
    REQUIRE(zeroed(proof, sizeof(proof)));
    cancel_limit = 1;
    cancel_calls = 0;
    REQUIRE(rendezvous_admission_prepare("bad", server_id, ticket, 999999, &cancel, proof) ==
            RENDEZVOUS_ADMISSION_CANCELLED);
    REQUIRE(rendezvous_admission_prepare("bad", server_id, ticket, 0, NULL, proof) ==
            RENDEZVOUS_ADMISSION_TIMEOUT);
    return 0;
}

static int test_urls_headers(void) {
    char url[1024], expected[1024], source[1024];
    snprintf(expected,
             sizeof(expected),
             "https://rendezvous.example.invalid/v1/rendezvous/challenge/classic/%s",
             server_id);
    snprintf(source,
             sizeof(source),
             "wss://rendezvous.example.invalid/servers/%s?role=client",
             server_id);
    REQUIRE(rendezvous_admission_url(source, server_id, url, sizeof(url)) &&
            strcmp(url, expected) == 0);
    snprintf(source,
             sizeof(source),
             "wss://rendezvous.example.invalid/v1/access/rendezvous/classic/%s",
             server_id);
    REQUIRE(rendezvous_admission_url(source, server_id, url, sizeof(url)) &&
            strcmp(url, expected) == 0);
    snprintf(source,
             sizeof(source),
             "wss://rendezvous.example.invalid/prefix/servers/%s?role=client",
             server_id);
    REQUIRE(rendezvous_admission_url(source, server_id, url, sizeof(url)) &&
            strcmp(url, expected) == 0);
    REQUIRE(!rendezvous_admission_url(source, ticket, url, sizeof(url)) && url[0] == 0);
    char insecure[1024];
    snprintf(insecure, sizeof(insecure), "ws://127.0.0.1/servers/%s?role=client", server_id);
    REQUIRE(!rendezvous_admission_url(insecure, server_id, url, sizeof(url)) && url[0] == 0);
    REQUIRE(!rendezvous_admission_url(source, server_id, url, 10) && url[0] == 0);
    const char *invalid[] = {"https://rendezvous.example.invalid",
                             "ws://remote.example.invalid/",
                             "wss://user:pass@remote.example.invalid/",
                             "wss://remote.example.invalid:443/#fragment"};
    for (size_t i = 0; i < arraysize(invalid); i++)
        REQUIRE(!rendezvous_admission_url(invalid[i], server_id, url, sizeof(url)));
    char proof[257];
    snprintf(proof, sizeof(proof), "%s.000000000129985a", token);
    struct curl_slist *headers = rendezvous_admission_headers(proof, ticket, NULL);
    REQUIRE(headers != NULL &&
            strstr(headers->data, "X-Atrinik-Rendezvous-Admission: ") == headers->data);
    REQUIRE(headers->next != NULL && strstr(headers->next->data, ticket) != NULL &&
            headers->next->next == NULL);
    rendezvous_admission_headers_free(headers);
    rendezvous_access_grant_t grant = {0};
    memcpy(grant.grant, ticket, sizeof(grant.grant));
    memcpy(grant.client_nonce, server_id, sizeof(grant.client_nonce));
    headers = rendezvous_admission_headers(proof, ticket, &grant);
    REQUIRE(headers != NULL &&
            strstr(headers->next->data, "Authorization: Bearer ") == headers->next->data);
    REQUIRE(strstr(headers->next->next->data, "X-Atrinik-Access-Client-Nonce: ") ==
            headers->next->next->data);
    REQUIRE(strcmp(headers->next->next->next->data,
                   "Sec-WebSocket-Protocol: " RENDEZVOUS_ACCESS_SUBPROTOCOL) == 0);
    REQUIRE(headers->next->next->next->next == NULL);
    rendezvous_admission_headers_free(headers);
    REQUIRE(rendezvous_admission_headers(proof, server_id, &grant) == NULL);
    REQUIRE(rendezvous_admission_headers("injection\r\nInjected: x", ticket, NULL) == NULL);
    return 0;
}

int main(int argc, char **argv) {
    toolkit_import(logger);
    toolkit_import(datetime);
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
        rendezvous_admission_challenge_t challenge = {.expires_at = 1800000060};
        memcpy(challenge.token, token, sizeof(token));
        char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U];
        uint64_t start = datetime_monotonic_ms();
        rendezvous_admission_result_t result = admission_solve(&challenge,
                                                               start + 15000,
                                                               NULL,
                                                               proof,
                                                               RENDEZVOUS_ADMISSION_DIFFICULTY,
                                                               RENDEZVOUS_ADMISSION_TRIALS_MAX,
                                                               datetime_monotonic_ms,
                                                               test_wall);
        printf("fixed24 result=%u elapsed_ms=%llu known_trials=19503195\n",
               (unsigned int)result,
               (unsigned long long)(datetime_monotonic_ms() - start));
        if (result == RENDEZVOUS_ADMISSION_OK)
            REQUIRE(strcmp(proof + strlen(token) + 1, "000000000129985a") == 0);
        OPENSSL_cleanse(proof, sizeof(proof));
        toolkit_deinit();
        return result == RENDEZVOUS_ADMISSION_OK ? 0 : 2;
    }
    REQUIRE(test_vectors() == 0);
    REQUIRE(test_parse() == 0);
    REQUIRE(test_solver() == 0);
    REQUIRE(test_urls_headers() == 0);
    toolkit_deinit();
    return 0;
}
