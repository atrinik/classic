/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "access_code.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdint.h>

static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void access_code_clear(void *data, size_t size) {
    if (data != NULL) {
        OPENSSL_cleanse(data, size);
    }
}

bool access_code_valid(const char *code, size_t len) {
    if (code == NULL || len != ACCESS_CODE_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        bool found = false;
        for (size_t j = 0; j < sizeof(alphabet) - 1; j++) {
            found |= code[i] == alphabet[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool ascii_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

bool access_code_normalize(const char *input, size_t len, char out[ACCESS_CODE_BUFFER_SIZE]) {
    access_code_clear(out, ACCESS_CODE_BUFFER_SIZE);
    if (input == NULL || out == NULL || len > 1024) {
        return false;
    }
    while (len != 0 && ascii_space((unsigned char)*input)) {
        input++;
        len--;
    }
    while (len != 0 && ascii_space((unsigned char)input[len - 1])) {
        len--;
    }
    if (len != ACCESS_CODE_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        out[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    if (!access_code_valid(out, len)) {
        access_code_clear(out, ACCESS_CODE_BUFFER_SIZE);
        return false;
    }
    return true;
}

bool access_code_generate(char out[ACCESS_CODE_BUFFER_SIZE]) {
    unsigned char random[10] = {0};
    access_code_clear(out, ACCESS_CODE_BUFFER_SIZE);
    if (out == NULL || RAND_priv_bytes(random, sizeof(random)) != 1) {
        access_code_clear(random, sizeof(random));
        return false;
    }
    /* Read exactly eighty bits in network bit order; no modulo reduction. */
    for (size_t i = 0; i < ACCESS_CODE_LENGTH; i++) {
        unsigned int value = 0;
        for (size_t bit = i * 5; bit < i * 5 + 5; bit++) {
            value = (value << 1) | ((random[bit / 8] >> (7 - bit % 8)) & 1U);
        }
        out[i] = alphabet[value];
    }
    access_code_clear(random, sizeof(random));
    return true;
}

static bool digest(const char *domain,
                   size_t domain_size,
                   const void *first,
                   size_t first_size,
                   const void *second,
                   size_t second_size,
                   unsigned char out[ACCESS_HASH_SIZE]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int size = 0;
    bool ok = ctx != NULL && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(ctx, domain, domain_size) == 1 &&
              EVP_DigestUpdate(ctx, first, first_size) == 1 &&
              (second_size == 0 || EVP_DigestUpdate(ctx, second, second_size) == 1) &&
              EVP_DigestFinal_ex(ctx, out, &size) == 1 && size == ACCESS_HASH_SIZE;
    EVP_MD_CTX_free(ctx);
    return ok;
}

bool access_code_route(const char code[ACCESS_CODE_LENGTH], unsigned char route[ACCESS_HASH_SIZE]) {
    static const char domain[] = "atrinik-access-route-v1";
    access_code_clear(route, ACCESS_HASH_SIZE);
    bool ok = route != NULL && access_code_valid(code, ACCESS_CODE_LENGTH) &&
              digest(domain, sizeof(domain), code, ACCESS_CODE_LENGTH, NULL, 0, route);
    if (!ok) {
        access_code_clear(route, ACCESS_HASH_SIZE);
    }
    return ok;
}

bool access_code_derive(const char code[ACCESS_CODE_LENGTH],
                        const unsigned char server_identity[ACCESS_HASH_SIZE],
                        unsigned char route[ACCESS_HASH_SIZE],
                        unsigned char index[ACCESS_HASH_SIZE],
                        unsigned char verifier[ACCESS_HASH_SIZE]) {
    static const char index_domain[] = "atrinik-access-index-v1";
    static const char join_domain[] = "atrinik-access-join-v1";
    access_code_clear(route, ACCESS_HASH_SIZE);
    access_code_clear(index, ACCESS_HASH_SIZE);
    access_code_clear(verifier, ACCESS_HASH_SIZE);
    bool ok = server_identity != NULL && route != NULL && index != NULL && verifier != NULL &&
              access_code_valid(code, ACCESS_CODE_LENGTH) && access_code_route(code, route) &&
              digest(index_domain, sizeof(index_domain), route, ACCESS_HASH_SIZE, NULL, 0, index) &&
              digest(join_domain,
                     sizeof(join_domain),
                     server_identity,
                     ACCESS_HASH_SIZE,
                     code,
                     ACCESS_CODE_LENGTH,
                     verifier);
    if (!ok) {
        access_code_clear(route, ACCESS_HASH_SIZE);
        access_code_clear(index, ACCESS_HASH_SIZE);
        access_code_clear(verifier, ACCESS_HASH_SIZE);
    }
    return ok;
}
