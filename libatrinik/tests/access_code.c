/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../access_code.h"
#include <stdio.h>
#include <string.h>
#include <openssl/rand.h>
#define REQUIRE(x)                                  \
    do {                                            \
        if (!(x)) {                                 \
            fprintf(stderr, "line %d\n", __LINE__); \
            return 1;                               \
        }                                           \
    } while (0)
#ifdef ACCESS_CODE_TEST_ENTROPY
static int entropy_mode;
int __wrap_RAND_priv_bytes(unsigned char *out, int size) {
    memset(out, 0xa5, (size_t)size);
    if (entropy_mode == 0)
        return 0;
    for (int i = 0; i < size; i++)
        out[i] = (unsigned char)i;
    return 1;
}
#endif
static bool matches(const unsigned char *digest, const char *hex) {
    char actual[65];
    for (size_t i = 0; i < 32; i++)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    return strcmp(actual, hex) == 0;
}
static bool zero(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++)
        if (p[i] != 0)
            return false;
    return true;
}
int main(void) {
    char code[17];
    unsigned char identity[32], route[32], index[32], verifier[32];
    for (size_t i = 0; i < 32; i++)
        identity[i] = (unsigned char)i;
    REQUIRE(access_code_normalize(" \t000g40r40m30e209\r\n", 20, code));
    REQUIRE(strcmp(code, "000G40R40M30E209") == 0);
    REQUIRE(access_code_route(code, route));
    REQUIRE(matches(route, "b0bb0cb469e574fc840ada88796841765251c2647adfe54a01672f125555140c"));
    REQUIRE(access_code_derive(code, identity, route, index, verifier));
    /* Independently computed hashlib vectors; domains include exactly one NUL. */
    REQUIRE(matches(route, "b0bb0cb469e574fc840ada88796841765251c2647adfe54a01672f125555140c"));
    REQUIRE(matches(index, "b254254cf95cd21e0732fef3fca4233b12a69f04c9a300da581a2583fbccc1bf"));
    REQUIRE(matches(verifier, "048d5e561f7554819084f7b67d36f65a2138ac4debc752299dd4328892e38bc1"));
    identity[0] ^= 1;
    REQUIRE(access_code_derive(code, identity, route, index, verifier));
    REQUIRE(!matches(verifier, "048d5e561f7554819084f7b67d36f65a2138ac4debc752299dd4328892e38bc1"));
    REQUIRE(!access_code_valid("000g40r40m30e209", 16));
    REQUIRE(!access_code_valid("000G40R40M30E209", 15));
    const char *bad[] = {"",
                         "000G40R40M30E2090",
                         "000G40R40M30E20I",
                         "000G40R40M30E20L",
                         "000G40R40M30E20O",
                         "000G40R40M30E20U",
                         "000G40R4 M30E209",
                         "000G40R4-M30E209",
                         "000G40R40M30E20\xff"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        memset(code, 'x', sizeof(code));
        REQUIRE(!access_code_normalize(bad[i], strlen(bad[i]), code));
        REQUIRE(zero(code, sizeof(code)));
    }
    REQUIRE(!access_code_derive("000G40R40M30E20I", identity, route, index, verifier));
    REQUIRE(zero(route, sizeof(route)) && zero(index, sizeof(index)) &&
            zero(verifier, sizeof(verifier)));
    REQUIRE(!access_code_derive(NULL, identity, route, index, verifier));
#ifdef ACCESS_CODE_TEST_ENTROPY
    memset(code, 'x', sizeof(code));
    REQUIRE(!access_code_generate(code));
    REQUIRE(zero(code, sizeof(code)));
    entropy_mode = 1;
    REQUIRE(access_code_generate(code));
    REQUIRE(strcmp(code, "000G40R40M30E209") == 0);
#else
    REQUIRE(access_code_generate(code));
    REQUIRE(access_code_valid(code, 16) && code[16] == 0);
#endif
    access_code_clear(code, sizeof(code));
    REQUIRE(zero(code, sizeof(code)));
    return 0;
}
