/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <toolkit/metaserver_publisher.h>
#include <toolkit/string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#define METASERVER_PUBLISH_CLASSIC_TAG "atrinik-classic-publish-v1"
#define METASERVER_PUBLISH_GAME_TAG "atrinik-game-publish-v1"
#define METASERVER_PUBLISH_COMPONENTS                                            \
    "(\"@method\" \"@authority\" \"@path\" \"content-digest\" \"content-type\" " \
    "\"atrinik-server-id\" \"atrinik-publish-sequence\")"

struct metaserver_publisher_identity {
    X509 *certificate;
    EVP_PKEY *key;
    char certificate_base64[METASERVER_PUBLISH_CERTIFICATE_BASE64_MAX + 1U];
};

static void metaserver_publisher_hex_lower(char *value) {
    for (; *value != '\0'; value++) {
        if (*value >= 'A' && *value <= 'F') {
            *value = (char)(*value - 'A' + 'a');
        }
    }
}

static bool metaserver_publisher_utf8(const char *value, size_t maximum, bool allow_empty) {
    if (value == NULL) {
        return false;
    }
    size_t size = strlen(value);
    if ((!allow_empty && size == 0) || size > maximum) {
        return false;
    }
    const unsigned char *cp = (const unsigned char *)value;
    const unsigned char *end = cp + size;
    while (cp < end) {
        if (*cp < 0x80) {
            if (*cp < 0x20 || *cp == 0x7f) {
                return false;
            }
            cp++;
            continue;
        }
        size_t continuation;
        uint32_t codepoint;
        if (*cp >= 0xc2 && *cp <= 0xdf) {
            continuation = 1;
            codepoint = *cp & 0x1fU;
        } else if (*cp >= 0xe0 && *cp <= 0xef) {
            continuation = 2;
            codepoint = *cp & 0x0fU;
        } else if (*cp >= 0xf0 && *cp <= 0xf4) {
            continuation = 3;
            codepoint = *cp & 0x07U;
        } else {
            return false;
        }
        if ((size_t)(end - cp) <= continuation) {
            return false;
        }
        unsigned char leading = *cp++;
        for (size_t i = 0; i < continuation; i++) {
            if ((*cp & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (*cp++ & 0x3fU);
        }
        if ((leading == 0xe0 && codepoint < 0x800U) || (leading == 0xed && codepoint >= 0xd800U) ||
            (leading == 0xf0 && codepoint < 0x10000U) || codepoint > 0x10ffffU) {
            return false;
        }
    }
    return true;
}

static bool metaserver_publisher_base64(const char *value, size_t maximum) {
    if (value == NULL) {
        return false;
    }
    size_t size = strlen(value);
    if (size == 0 || size > maximum || size % 4U != 0) {
        return false;
    }
    size_t padding = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char cp = (unsigned char)value[i];
        if (cp == '=') {
            padding++;
            if (i < size - 2U || padding > 2U) {
                return false;
            }
        } else if (padding != 0 || (!((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                                      (cp >= '0' && cp <= '9')) &&
                                    cp != '+' && cp != '/')) {
            return false;
        }
    }
    size_t decoded_size = size / 4U * 3U - padding;
    if (decoded_size == 0 || decoded_size > METASERVER_PUBLISH_CERTIFICATE_DER_MAX) {
        return false;
    }
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *last_data = strchr(alphabet, value[size - padding - 1U]);
    if (last_data == NULL || (padding == 1U && ((size_t)(last_data - alphabet) & 0x03U) != 0) ||
        (padding == 2U && ((size_t)(last_data - alphabet) & 0x0fU) != 0)) {
        return false;
    }
    return true;
}

static bool
metaserver_publisher_json_string(const char *value, char *escaped, size_t escaped_size) {
    size_t used = 0;
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if ((*cp == '"' || *cp == '\\') && used + 1U >= escaped_size) {
            return false;
        }
        if (*cp == '"' || *cp == '\\') {
            escaped[used++] = '\\';
        }
        if (used + 1U >= escaped_size) {
            return false;
        }
        escaped[used++] = (char)*cp;
    }
    escaped[used] = '\0';
    return true;
}

bool metaserver_publisher_classic_body(const metaserver_publisher_classic_payload_t *payload,
                                       char body[METASERVER_PUBLISH_BODY_MAX + 1U],
                                       size_t *body_size) {
    HARD_ASSERT(body != NULL);
    HARD_ASSERT(body_size != NULL);
    *body = '\0';
    *body_size = 0;
    if (payload == NULL || payload->server_id == NULL ||
        !string_is_hex_fixed(payload->server_id, 64, true) ||
        !metaserver_publisher_base64(payload->certificate,
                                     METASERVER_PUBLISH_CERTIFICATE_BASE64_MAX) ||
        !metaserver_publisher_utf8(payload->name, 80, false) ||
        !metaserver_publisher_utf8(payload->version, 32, false) ||
        !metaserver_publisher_utf8(payload->text_comment, 256, true)) {
        return false;
    }
    char name[161], version[65], comment[513];
    if (!metaserver_publisher_json_string(payload->name, VS(name)) ||
        !metaserver_publisher_json_string(payload->version, VS(version)) ||
        !metaserver_publisher_json_string(payload->text_comment, VS(comment))) {
        return false;
    }
    int length = snprintf(body,
                          METASERVER_PUBLISH_BODY_MAX + 1U,
                          "{\"schema\":\"atrinik-classic-publish-v1\","
                          "\"serverId\":\"%s\",\"certificate\":\"%s\","
                          "\"name\":\"%s\",\"playersCount\":%" PRIu32 ","
                          "\"version\":\"%s\",\"textComment\":\"%s\","
                          "\"public\":%s,\"passwordRequired\":%s}",
                          payload->server_id,
                          payload->certificate,
                          name,
                          payload->players_count,
                          version,
                          comment,
                          payload->is_public ? "true" : "false",
                          payload->password_required ? "true" : "false");
    if (length <= 0 || (size_t)length > METASERVER_PUBLISH_BODY_MAX) {
        *body = '\0';
        return false;
    }
    *body_size = (size_t)length;
    return true;
}

static bool metaserver_publisher_authority_valid(const char *authority) {
    if (authority == NULL) {
        return false;
    }
    size_t length = strlen(authority);
    if (length == 0 || length > 253 || authority[0] == '.' || authority[length - 1U] == '.') {
        return false;
    }
    bool label_start = true;
    size_t label_size = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char cp = (unsigned char)authority[i];
        if (cp == '.') {
            if (label_start || authority[i - 1U] == '-') {
                return false;
            }
            label_start = true;
            label_size = 0;
        } else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
            label_start = false;
            label_size++;
        } else if (cp == '-' && !label_start) {
            label_start = false;
            label_size++;
        } else {
            return false;
        }
        if (label_size > 63) {
            return false;
        }
    }
    return !label_start && authority[length - 1U] != '-';
}

static bool metaserver_publisher_p256(EVP_PKEY *key) {
    char group[32];
    size_t length = 0;
    return key != NULL && EVP_PKEY_is_a(key, "EC") == 1 &&
           EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, VS(group), &length) ==
               1 &&
           strcmp(group, "prime256v1") == 0;
}

static bool metaserver_publisher_certificate_id(X509 *certificate, const char *server_id) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_size = 0;
    char actual[SHA256_DIGEST_LENGTH * 2U + 1U];
    bool valid = certificate != NULL && server_id != NULL &&
                 string_is_hex_fixed(server_id, SHA256_DIGEST_LENGTH * 2U, true) &&
                 X509_digest(certificate, EVP_sha256(), digest, &digest_size) == 1 &&
                 digest_size == sizeof(digest) &&
                 string_tohex(VS(digest), VS(actual), false) == sizeof(actual) - 1U;
    if (valid) {
        metaserver_publisher_hex_lower(actual);
        valid = CRYPTO_memcmp(actual, server_id, sizeof(actual) - 1U) == 0;
    }
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(actual, sizeof(actual));
    return valid;
}

bool metaserver_publisher_build(metaserver_publisher_profile_t profile,
                                const char *authority,
                                const char *server_id,
                                uint64_t sequence,
                                const unsigned char nonce[METASERVER_PUBLISH_NONCE_SIZE],
                                uint64_t created,
                                const void *body,
                                size_t body_size,
                                metaserver_publisher_components_t *components) {
#ifdef WIN32
    fprintf(stderr, "publisher build trace: enter\n");
    fflush(stderr);
#endif
    HARD_ASSERT(components != NULL);
    memset(components, 0, sizeof(*components));
#ifdef WIN32
    fprintf(stderr, "publisher build trace: output cleared\n");
    fflush(stderr);
#endif

    const char *prefix = NULL;
    const char *tag = NULL;
    if (profile == METASERVER_PUBLISHER_CLASSIC_V1) {
        prefix = "/v1/classic/servers";
        tag = METASERVER_PUBLISH_CLASSIC_TAG;
    } else if (profile == METASERVER_PUBLISHER_GAME_V1) {
        prefix = "/v1/servers";
        tag = METASERVER_PUBLISH_GAME_TAG;
    } else {
        return false;
    }
#ifdef WIN32
    fprintf(stderr, "publisher build trace: profile selected\n");
    fflush(stderr);
#endif
    unsigned char nonce_or = 0;
    for (size_t i = 0; nonce != NULL && i < METASERVER_PUBLISH_NONCE_SIZE; i++) {
        nonce_or |= nonce[i];
    }
    if (!metaserver_publisher_authority_valid(authority) || server_id == NULL ||
        !string_is_hex_fixed(server_id, 64, true) || sequence == 0 || nonce == NULL ||
        nonce_or == 0 || created > 999999999999999ULL - METASERVER_PUBLISH_VALIDITY_SECONDS ||
        body == NULL || body_size == 0 || body_size > METASERVER_PUBLISH_BODY_MAX) {
        return false;
    }
#ifdef WIN32
    fprintf(stderr, "publisher build trace: input validated\n");
    fflush(stderr);
#endif

    unsigned char digest[SHA256_DIGEST_LENGTH];
    char digest_base64[64];
    char nonce_hex[METASERVER_PUBLISH_NONCE_SIZE * 2U + 1U];
    bool digest_ok = SHA256(body, body_size, digest) != NULL;
#ifdef WIN32
    fprintf(stderr, "publisher build trace: digest hashed=%d\n", digest_ok);
    fflush(stderr);
#endif
    bool base64_ok =
        digest_ok && EVP_EncodeBlock((unsigned char *)digest_base64, digest, sizeof(digest)) == 44;
#ifdef WIN32
    fprintf(stderr, "publisher build trace: digest base64=%d\n", base64_ok);
    fflush(stderr);
#endif
    bool nonce_ok =
        base64_ok && string_tohex(nonce, METASERVER_PUBLISH_NONCE_SIZE, VS(nonce_hex), false) ==
                         METASERVER_PUBLISH_NONCE_SIZE * 2U;
#ifdef WIN32
    fprintf(stderr, "publisher build trace: nonce hex=%d\n", nonce_ok);
    fflush(stderr);
#endif
    if (!nonce_ok) {
        OPENSSL_cleanse(digest, sizeof(digest));
        return false;
    }
#ifdef WIN32
    fprintf(stderr, "publisher build trace: digest rendered\n");
    fflush(stderr);
#endif
    metaserver_publisher_hex_lower(nonce_hex);

    int path_length = snprintf(VS(components->path), "%s/%s/publish", prefix, server_id);
    int digest_length = snprintf(VS(components->content_digest), "sha-256=:%s:", digest_base64);
    int input_length =
        snprintf(VS(components->signature_input),
                 "atrinik=" METASERVER_PUBLISH_COMPONENTS ";created=%" PRIu64 ";expires=%" PRIu64
                 ";nonce=\"%s\";alg=\"ecdsa-p256-sha256\";keyid=\"%s\""
                 ";tag=\"%s\"",
                 created,
                 created + METASERVER_PUBLISH_VALIDITY_SECONDS,
                 nonce_hex,
                 server_id,
                 tag);
    int base_length = snprintf(VS(components->signature_base),
                               "\"@method\": POST\n"
                               "\"@authority\": %s\n"
                               "\"@path\": %s\n"
                               "\"content-digest\": %s\n"
                               "\"content-type\": " METASERVER_PUBLISH_CONTENT_TYPE "\n"
                               "\"atrinik-server-id\": %s\n"
                               "\"atrinik-publish-sequence\": %" PRIu64 "\n"
                               "\"@signature-params\": %s",
                               authority,
                               components->path,
                               components->content_digest,
                               server_id,
                               sequence,
                               components->signature_input + strlen("atrinik="));
#ifdef WIN32
    fprintf(stderr, "publisher build trace: components rendered\n");
    fflush(stderr);
#endif
    OPENSSL_cleanse(digest, sizeof(digest));
    return path_length > 0 && (size_t)path_length < sizeof(components->path) && digest_length > 0 &&
           (size_t)digest_length < sizeof(components->content_digest) && input_length > 0 &&
           (size_t)input_length < sizeof(components->signature_input) && base_length > 0 &&
           (size_t)base_length < sizeof(components->signature_base);
}

metaserver_publisher_identity_t *
metaserver_publisher_identity_create(X509 *certificate, EVP_PKEY *key, const char *server_id) {
    HARD_ASSERT(certificate != NULL);
    HARD_ASSERT(key != NULL);
    HARD_ASSERT(server_id != NULL);
    EVP_PKEY *public_key = X509_get_pubkey(certificate);
    if (public_key == NULL || !metaserver_publisher_p256(key) ||
        !metaserver_publisher_p256(public_key) || EVP_PKEY_eq(key, public_key) != 1 ||
        !metaserver_publisher_certificate_id(certificate, server_id)) {
        EVP_PKEY_free(public_key);
        return NULL;
    }
    EVP_PKEY_free(public_key);

    int der_size = i2d_X509(certificate, NULL);
    unsigned char *der = der_size > 0 ? OPENSSL_malloc((size_t)der_size) : NULL;
    unsigned char *cursor = der;
    metaserver_publisher_identity_t *identity = NULL;
    if (der != NULL && (size_t)der_size <= METASERVER_PUBLISH_CERTIFICATE_DER_MAX &&
        i2d_X509(certificate, &cursor) == der_size &&
        4U * (((size_t)der_size + 2U) / 3U) <= METASERVER_PUBLISH_CERTIFICATE_BASE64_MAX) {
        identity = calloc(1, sizeof(*identity));
    }
    if (identity != NULL) {
        int encoded = EVP_EncodeBlock((unsigned char *)identity->certificate_base64, der, der_size);
        if (encoded <= 0 || (size_t)encoded >= sizeof(identity->certificate_base64)) {
            free(identity);
            identity = NULL;
        }
    }
    OPENSSL_clear_free(der, der_size > 0 ? (size_t)der_size : 0);
    bool certificate_retained = identity != NULL && X509_up_ref(certificate) == 1;
    bool key_retained = certificate_retained && EVP_PKEY_up_ref(key) == 1;
    if (!key_retained) {
        if (identity != NULL) {
            if (certificate_retained) {
                X509_free(certificate);
            }
            OPENSSL_cleanse(identity->certificate_base64, sizeof(identity->certificate_base64));
            free(identity);
        }
        return NULL;
    }
    identity->certificate = certificate;
    identity->key = key;
    return identity;
}

const char *
metaserver_publisher_identity_certificate(const metaserver_publisher_identity_t *identity) {
    return identity != NULL ? identity->certificate_base64 : NULL;
}

static bool metaserver_publisher_raw_signature(const unsigned char *der,
                                               size_t der_size,
                                               unsigned char raw[64]) {
    const unsigned char *cursor = der;
    ECDSA_SIG *signature = d2i_ECDSA_SIG(NULL, &cursor, (long)der_size);
    if (signature == NULL || cursor != der + der_size) {
        ECDSA_SIG_free(signature);
        return false;
    }
    const BIGNUM *r = NULL, *s = NULL;
    ECDSA_SIG_get0(signature, &r, &s);
    bool valid = r != NULL && s != NULL && BN_num_bytes(r) <= 32 && BN_num_bytes(s) <= 32 &&
                 BN_bn2binpad(r, raw, 32) == 32 && BN_bn2binpad(s, raw + 32, 32) == 32;
    ECDSA_SIG_free(signature);
    return valid;
}

bool metaserver_publisher_identity_sign(
    const metaserver_publisher_identity_t *identity,
    const char *signature_base,
    char signature_header[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX]) {
    HARD_ASSERT(signature_header != NULL);
    memset(signature_header, 0, METASERVER_PUBLISH_SIGNATURE_HEADER_MAX);
    if (identity == NULL || signature_base == NULL || *signature_base == '\0') {
        return false;
    }
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    size_t der_size = 0;
    unsigned char *der = NULL;
    unsigned char raw[64];
    bool valid = context != NULL &&
                 EVP_DigestSignInit(context, NULL, EVP_sha256(), NULL, identity->key) == 1 &&
                 EVP_DigestSignUpdate(context, signature_base, strlen(signature_base)) == 1 &&
                 EVP_DigestSignFinal(context, NULL, &der_size) == 1 && der_size > 0;
    if (valid) {
        der = OPENSSL_malloc(der_size);
        valid = der != NULL && EVP_DigestSignFinal(context, der, &der_size) == 1 &&
                metaserver_publisher_raw_signature(der, der_size, raw);
    }
    char encoded[96];
    if (valid) {
        valid = EVP_EncodeBlock((unsigned char *)encoded, raw, sizeof(raw)) == 88;
    }
    if (valid) {
        int length = snprintf(signature_header,
                              METASERVER_PUBLISH_SIGNATURE_HEADER_MAX,
                              "atrinik=:%s:",
                              encoded);
        valid = length > 0 && (size_t)length < METASERVER_PUBLISH_SIGNATURE_HEADER_MAX;
    }
    EVP_MD_CTX_free(context);
    OPENSSL_clear_free(der, der_size);
    OPENSSL_cleanse(raw, sizeof(raw));
    OPENSSL_cleanse(encoded, sizeof(encoded));
    if (!valid) {
        memset(signature_header, 0, METASERVER_PUBLISH_SIGNATURE_HEADER_MAX);
    }
    return valid;
}

void metaserver_publisher_identity_free(metaserver_publisher_identity_t *identity) {
    if (identity == NULL) {
        return;
    }
    X509_free(identity->certificate);
    EVP_PKEY_free(identity->key);
    OPENSSL_cleanse(identity->certificate_base64, sizeof(identity->certificate_base64));
    OPENSSL_clear_free(identity, sizeof(*identity));
}

bool metaserver_publisher_verify(const unsigned char *certificate_der,
                                 size_t certificate_size,
                                 const char *server_id,
                                 const char *signature_base,
                                 const unsigned char signature[64]) {
    if (certificate_der == NULL || certificate_size == 0 ||
        certificate_size > METASERVER_PUBLISH_CERTIFICATE_DER_MAX || certificate_size > LONG_MAX ||
        server_id == NULL || signature_base == NULL || signature == NULL) {
        return false;
    }
    const unsigned char *cursor = certificate_der;
    X509 *certificate = d2i_X509(NULL, &cursor, (long)certificate_size);
    EVP_PKEY *key = certificate != NULL ? X509_get_pubkey(certificate) : NULL;
    bool valid = certificate != NULL && cursor == certificate_der + certificate_size &&
                 metaserver_publisher_certificate_id(certificate, server_id) &&
                 metaserver_publisher_p256(key);

    BIGNUM *r = valid ? BN_bin2bn(signature, 32, NULL) : NULL;
    BIGNUM *s = valid ? BN_bin2bn(signature + 32, 32, NULL) : NULL;
    ECDSA_SIG *ecdsa = valid ? ECDSA_SIG_new() : NULL;
    valid = valid && r != NULL && s != NULL && ecdsa != NULL && ECDSA_SIG_set0(ecdsa, r, s) == 1;
    if (valid) {
        r = NULL;
        s = NULL;
    }
    int der_size = valid ? i2d_ECDSA_SIG(ecdsa, NULL) : 0;
    unsigned char *der = der_size > 0 ? OPENSSL_malloc((size_t)der_size) : NULL;
    unsigned char *der_cursor = der;
    valid = valid && der != NULL && i2d_ECDSA_SIG(ecdsa, &der_cursor) == der_size;
    EVP_MD_CTX *context = valid ? EVP_MD_CTX_new() : NULL;
    valid = valid && context != NULL &&
            EVP_DigestVerifyInit(context, NULL, EVP_sha256(), NULL, key) == 1 &&
            EVP_DigestVerifyUpdate(context, signature_base, strlen(signature_base)) == 1 &&
            EVP_DigestVerifyFinal(context, der, (size_t)der_size) == 1;
    EVP_MD_CTX_free(context);
    OPENSSL_clear_free(der, der_size > 0 ? (size_t)der_size : 0);
    ECDSA_SIG_free(ecdsa);
    BN_free(r);
    BN_free(s);
    EVP_PKEY_free(key);
    X509_free(certificate);
    return valid;
}
