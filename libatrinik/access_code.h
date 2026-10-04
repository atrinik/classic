/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TOOLKIT_ACCESS_CODE_H
#define TOOLKIT_ACCESS_CODE_H
#include <stdbool.h>
#include <stddef.h>
#define ACCESS_CODE_LENGTH 16U
#define ACCESS_CODE_BUFFER_SIZE 17U
#define ACCESS_HASH_SIZE 32U

/* Stateless and thread-safe. Inputs are borrowed; all output storage is caller
 * owned. No pointers are retained and no allocation escapes these calls.
 * Outputs must not overlap each other or inputs. Failure clears supplied output
 * buffers. Callers must cleanse all live secret copies after their attempt.
 * Canonical wire codes are exactly 16 bytes (no NUL required by valid/derive).
 * Only UI normalization accepts outer ASCII whitespace and lowercase letters. */
bool access_code_valid(const char *code, size_t len);
bool access_code_normalize(const char *input, size_t len, char out[ACCESS_CODE_BUFFER_SIZE]);
bool access_code_generate(char out[ACCESS_CODE_BUFFER_SIZE]);
bool access_code_derive(const char code[ACCESS_CODE_LENGTH],
                        const unsigned char server_identity[ACCESS_HASH_SIZE],
                        unsigned char route[ACCESS_HASH_SIZE],
                        unsigned char index[ACCESS_HASH_SIZE],
                        unsigned char verifier[ACCESS_HASH_SIZE]);
void access_code_clear(void *data, size_t size);
#endif
