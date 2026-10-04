/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "access_resolve.h"
#include "access_code.h"
#include "metaserver_url.h"
#include "string.h"
#include <curl/curl.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

static bool literal(const char **cursor, const char *value) {
    size_t n = strlen(value);
    if (strncmp(*cursor, value, n) != 0) return false;
    *cursor += n;
    return true;
}

/* Canonical publisher JSON only escapes quote/backslash; controls forbidden. */
static bool text_value(const char **cursor, char *out, size_t capacity) {
    size_t n = 0;
    if (!literal(cursor, "\"")) return false;
    while (**cursor != 0 && **cursor != '"') {
        unsigned char c = (unsigned char)*(*cursor)++;
        if (c == '\\') {
            c = (unsigned char)*(*cursor)++;
            if (c != '\\' && c != '"') return false;
        }
        if (c < 0x20 || c == 0x7f || n + 1 >= capacity) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return literal(cursor, "\"");
}

static bool display_name(const char *name) {
    const unsigned char *p = (const unsigned char *)name;
    if (*p == 0) return false;
    while (*p != 0) {
        uint32_t value = *p++;
        unsigned int extra = 0;
        uint32_t minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) { extra = 1; minimum = 0x80; value &= 0x1f; }
        else if (value >= 0xe0 && value <= 0xef) { extra = 2; minimum = 0x800; value &= 0xf; }
        else if (value >= 0xf0 && value <= 0xf4) { extra = 3; minimum = 0x10000; value &= 7; }
        else if (value >= 0x80) return false;
        for (unsigned int i = 0; i < extra; i++) {
            if ((*p & 0xc0) != 0x80) return false;
            value = (value << 6) | (*p++ & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x7f && value <= 0x9f) || (value & 0xffff) >= 0xfffe ||
            (value >= 0xfdd0 && value <= 0xfdef)) return false;
    }
    return true;
}

static bool certificate_valid(const char *encoded, const char *server_id) {
    size_t n = strlen(encoded);
    unsigned char der[2052], hash[32];
    char canonical[2737], actual[65], group[32];
    if (n == 0 || n > 2732 || n % 4 != 0) return false;
    int decoded = EVP_DecodeBlock(der, (const unsigned char *)encoded, (int)n);
    if (decoded <= 0) return false;
    if (encoded[n - 1] == '=') decoded--;
    if (encoded[n - 2] == '=') decoded--;
    if (decoded <= 0 || decoded > 2048 ||
        EVP_EncodeBlock((unsigned char *)canonical, der, decoded) != (int)n ||
        strcmp(encoded, canonical) != 0) return false;
    const unsigned char *cursor = der;
    X509 *certificate = d2i_X509(NULL, &cursor, decoded);
    EVP_PKEY *key = certificate != NULL ? X509_get_pubkey(certificate) : NULL;
    unsigned int hash_size = 0;
    size_t group_size = 0;
    bool ok = certificate != NULL && cursor == der + decoded && key != NULL &&
              EVP_PKEY_is_a(key, "EC") == 1 &&
              EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME,
                                             group, sizeof(group), &group_size) == 1 &&
              strcmp(group, "prime256v1") == 0 &&
              EVP_Digest(der, (size_t)decoded, hash, &hash_size, EVP_sha256(), NULL) == 1 &&
              hash_size == 32 && string_tohex(hash, 32, actual, sizeof(actual), false) == 64;
    if (ok) {
        string_tolower(actual);
        ok = CRYPTO_memcmp(actual, server_id, 64) == 0;
    }
    EVP_PKEY_free(key);
    X509_free(certificate);
    return ok;
}

void access_resolved_clear(access_resolved_t *resolved) {
    access_code_clear(resolved, sizeof(*resolved));
}

bool access_resolve_parse(const char *body, size_t size, const char nonce[65],
                          uint64_t now, access_resolved_t *out) {
    access_resolved_t value = {0};
    char copy[8193], cert[2737], expiry[21];
    access_resolved_clear(out);
    if (out == NULL || body == NULL || size == 0 || size > 8192 ||
        memchr(body, 0, size) != NULL || !string_is_hex_fixed(nonce, 64, true)) return false;
    memcpy(copy, body, size);
    copy[size] = 0;
    const char *p = copy;
    bool ok = literal(&p, "{\"schema\":\"atrinik-access-resolved-v1\",\"profile\":\"classic\",\"serverId\":") &&
              text_value(&p, value.server_id, sizeof(value.server_id)) &&
              string_is_hex_fixed(value.server_id, 64, true) &&
              literal(&p, ",\"certificate\":") && text_value(&p, cert, sizeof(cert)) &&
              literal(&p, ",\"name\":") && text_value(&p, value.name, sizeof(value.name)) &&
              display_name(value.name) &&
              literal(&p, ",\"accessRequired\":true,\"generation\":") &&
              text_value(&p, value.grant.generation, sizeof(value.grant.generation)) &&
              literal(&p, ",\"clientNonce\":") &&
              text_value(&p, value.grant.client_nonce, sizeof(value.grant.client_nonce)) &&
              literal(&p, ",\"grant\":") &&
              text_value(&p, value.grant.grant, sizeof(value.grant.grant)) &&
              literal(&p, ",\"expiresAt\":") && text_value(&p, expiry, sizeof(expiry)) &&
              expiry[0] >= '1' && expiry[0] <= '9' &&
              string_parse_uint64(expiry, 10, 1, INT64_MAX, &value.grant.expiry);
    if (ok && *p == ',') {
        char port[6];
        size_t n = 0;
        ok = literal(&p, ",\"endpoint\":{\"hostname\":") &&
             text_value(&p, value.hostname, sizeof(value.hostname)) &&
             metaserver_hostname_valid(value.hostname) && literal(&p, ",\"port\":");
        while (ok && *p >= '0' && *p <= '9' && n < sizeof(port) - 1) port[n++] = *p++;
        port[n] = 0;
        uint64_t parsed = 0;
        ok = ok && n != 0 && port[0] != '0' &&
             string_parse_uint64(port, 10, 1, 65535, &parsed) && literal(&p, "}");
        value.port = (uint16_t)parsed;
    }
    memcpy(value.grant.server_id, value.server_id, sizeof(value.server_id));
    ok = ok && literal(&p, "}") && *p == 0 &&
         rendezvous_access_grant_valid(&value.grant, value.server_id, now) &&
         CRYPTO_memcmp(nonce, value.grant.client_nonce, 64) == 0 &&
         certificate_valid(cert, value.server_id);
    if (ok) *out = value;
    access_resolved_clear(&value);
    access_code_clear(copy, sizeof(copy));
    return ok;
}

typedef struct resolve_response {
    char body[8193];
    size_t used;
    size_t header_bytes;
    bool no_store;
} resolve_response_t;

static size_t response_body(char *data, size_t size, size_t count, void *context) {
    resolve_response_t *r = context;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > sizeof(r->body) - 1 - r->used) return 0;
    memcpy(r->body + r->used, data, n);
    r->used += n;
    r->body[r->used] = 0;
    return n;
}

static size_t response_header(char *data, size_t size, size_t count, void *context) {
    resolve_response_t *r = context;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > 8192 - r->header_bytes) return 0;
    r->header_bytes += n;
    if (n >= 5 && memcmp(data, "HTTP/", 5) == 0) r->no_store = false;
    static const char prefix[] = "Cache-Control:";
    if (n >= sizeof(prefix) - 1 && strncasecmp(data, prefix, sizeof(prefix) - 1) == 0) {
        const char *p = data + sizeof(prefix) - 1, *end = data + n;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) end--;
        r->no_store = (size_t)(end - p) == 8 && strncasecmp(p, "no-store", 8) == 0;
    }
    return n;
}

bool access_resolve(const char *origin, const char code[ACCESS_CODE_LENGTH], access_resolved_t *out) {
    access_resolved_clear(out);
    if (out == NULL) return false;
    unsigned char route[32] = {0}, nonce_bytes[32] = {0};
    char route_hex[65], nonce[65], request[256], url[MAX_BUF];
    resolve_response_t response = {0};
    bool ok = access_code_route(code, route) && RAND_priv_bytes(nonce_bytes, 32) == 1 &&
              string_tohex(route, 32, route_hex, sizeof(route_hex), false) == 64 &&
              string_tohex(nonce_bytes, 32, nonce, sizeof(nonce), false) == 64 &&
              metaserver_url_access(origin, NULL, false, url, sizeof(url));
    CURL *curl = ok ? curl_easy_init() : NULL;
    struct curl_slist *headers = NULL;
    if (curl != NULL) {
        string_tolower(route_hex);
        string_tolower(nonce);
        int n = snprintf(request, sizeof(request),
            "{\"schema\":\"atrinik-access-resolve-v1\",\"routeCapability\":\"%s\",\"clientNonce\":\"%s\"}", route_hex, nonce);
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        struct curl_slist *added = headers != NULL ? curl_slist_append(headers, "Cache-Control: no-store") : NULL;
        ok = n > 0 && (size_t)n < sizeof(request) && added != NULL;
        if (added != NULL) headers = added;
        if (ok) {
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)n);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
            curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_body);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
#ifdef WIN32
            curl_easy_setopt(curl, CURLOPT_CAINFO, "ca-bundle.crt");
#endif
            CURLcode result = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            time_t now = time(NULL);
            ok = result == CURLE_OK && status == 200 && response.no_store && now >= 0 &&
                 access_resolve_parse(response.body, response.used, nonce, (uint64_t)now, out);
        }
    } else ok = false;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    access_code_clear(route, sizeof(route));
    access_code_clear(route_hex, sizeof(route_hex));
    access_code_clear(nonce_bytes, sizeof(nonce_bytes));
    access_code_clear(nonce, sizeof(nonce));
    access_code_clear(request, sizeof(request));
    access_code_clear(&response, sizeof(response));
    return ok;
}
