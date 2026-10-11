/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* The private prepare seam preserves the production HTTPS/parser path while
 * selecting eight-bit work solely in this independently linked test. */
#include "../rendezvous_admission.c"
#include <toolkit/logger.h>

#define REQUIRE(x)                                                               \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "HTTPS admission assertion at line %d\n", __LINE__); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static const char ticket[] = "2222222222222222222222222222222222222222222222222222222222222222";
static const char server_id[] = "1111111111111111111111111111111111111111111111111111111111111111";
static unsigned int solver_calls;

CURL *__real_curl_easy_init(void);
CURL *__wrap_curl_easy_init(void) {
    CURL *handle = __real_curl_easy_init();
    const char *ca = getenv("ATRINIK_TEST_ADMISSION_CA");
    if (handle != NULL && ca != NULL) {
        if (curl_easy_setopt(handle, CURLOPT_CAINFO, ca) != CURLE_OK) {
            curl_easy_cleanup(handle);
            return NULL;
        }
    }
    return handle;
}

static rendezvous_admission_result_t solve_fast(const rendezvous_admission_challenge_t *challenge,
                                                uint64_t deadline_ms,
                                                const curl_cancel_t *cancel,
                                                char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U]) {
    solver_calls++;
    return admission_solve(challenge,
                           deadline_ms,
                           cancel,
                           proof,
                           8,
                           100000,
                           datetime_monotonic_ms,
                           time);
}

static bool cancel_after(void *context) {
    return datetime_monotonic_ms() >= *(const uint64_t *)context;
}

static bool zeroed(const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0)
            return false;
    }
    return true;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    toolkit_import(logger);
    toolkit_import(datetime);
    toolkit_import(curl);
    const char *mode = argv[2];
    uint64_t start = datetime_monotonic_ms();
    uint64_t cancellation_time = start + 200;
    curl_cancel_t cancel = {.cancelled = cancel_after, .context = &cancellation_time};
    bool deadline_case = strcmp(mode, "deadline") == 0;
    bool cancel_case = strcmp(mode, "cancel") == 0;
    uint64_t deadline = start + (deadline_case ? 250 : 5000);
    char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U];
    memset(proof, 0xa5, sizeof(proof));
    rendezvous_admission_result_t result = admission_prepare(argv[1],
                                                             server_id,
                                                             ticket,
                                                             deadline,
                                                             cancel_case ? &cancel : NULL,
                                                             proof,
                                                             solve_fast);
    uint64_t elapsed = datetime_monotonic_ms() - start;
    if (strcmp(mode, "valid") == 0) {
        REQUIRE(result == RENDEZVOUS_ADMISSION_OK && solver_calls == 1);
        char *separator = strrchr(proof, '.');
        REQUIRE(separator != NULL && string_is_hex_fixed(separator + 1, 16, true));
        size_t token_size = (size_t)(separator - proof);
        char hash[65];
        REQUIRE(rendezvous_admission_ticket_hash(ticket, hash));
        *separator = 0;
        uint64_t expiry = 0;
        REQUIRE(admission_token(proof, hash, (uint64_t)time(NULL), &expiry));
        char input[512];
        size_t prefix_size = sizeof(WORK_DOMAIN) + token_size + 1;
        memcpy(input, WORK_DOMAIN, sizeof(WORK_DOMAIN));
        memcpy(input + sizeof(WORK_DOMAIN), proof, token_size + 1);
        memcpy(input + prefix_size, separator + 1, 16);
        unsigned char digest[32];
        REQUIRE(admission_digest(input, prefix_size + 16, digest) && digest[0] == 0);
        OPENSSL_cleanse(hash, sizeof(hash));
        OPENSSL_cleanse(input, sizeof(input));
        OPENSSL_cleanse(digest, sizeof(digest));
    } else {
        REQUIRE(solver_calls == 0 && zeroed(proof, sizeof(proof)));
        if (deadline_case) {
            REQUIRE(result == RENDEZVOUS_ADMISSION_TIMEOUT && elapsed < 1500);
        } else if (cancel_case) {
            REQUIRE(result == RENDEZVOUS_ADMISSION_CANCELLED && elapsed < 1500);
        } else if (strcmp(mode, "malformed") == 0 || strcmp(mode, "missing-no-store") == 0) {
            REQUIRE(result == RENDEZVOUS_ADMISSION_PROTOCOL);
        } else {
            REQUIRE(result == RENDEZVOUS_ADMISSION_UNAVAILABLE);
        }
    }
    OPENSSL_cleanse(proof, sizeof(proof));
    toolkit_deinit();
    return 0;
}
