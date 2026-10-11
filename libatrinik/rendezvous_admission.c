/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "rendezvous_admission.h"
#include "datetime.h"
#include "metaserver_url.h"
#include "string.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>

#define ADMISSION_SCHEMA "atrinik-rendezvous-admission-v1"
#define WORK_DOMAIN "atrinik-rendezvous-work-v1"

static bool admission_digest(const void *data, size_t size, unsigned char digest[32]) {
    unsigned int written = 0;
    return EVP_Digest(data, size, digest, &written, EVP_sha256(), NULL) == 1 && written == 32;
}

bool rendezvous_admission_ticket_hash(const char *ticket, char out[65]) {
    if (out == NULL)
        return false;
    memset(out, 0, 65);
    if (!string_is_hex_fixed(ticket, 64, true))
        return false;
    unsigned char binary[32], digest[32];
    for (size_t i = 0; i < sizeof(binary); i++) {
        unsigned int hi = ticket[i * 2] <= '9' ? ticket[i * 2] - '0' : ticket[i * 2] - 'a' + 10;
        unsigned int lo =
            ticket[i * 2 + 1] <= '9' ? ticket[i * 2 + 1] - '0' : ticket[i * 2 + 1] - 'a' + 10;
        binary[i] = (unsigned char)((hi << 4) | lo);
    }
    bool ok = admission_digest(binary, sizeof(binary), digest) &&
              string_tohex(digest, sizeof(digest), out, 65, false) == 64;
    string_tolower(out);
    OPENSSL_cleanse(binary, sizeof(binary));
    OPENSSL_cleanse(digest, sizeof(digest));
    return ok;
}

bool rendezvous_admission_url(const char *websocket_url,
                              const char *server_id,
                              char *out,
                              size_t out_size) {
    if (out == NULL || out_size == 0)
        return false;
    out[0] = 0;
    if (websocket_url == NULL || strlen(websocket_url) >= 1024 ||
        !string_is_hex_fixed(server_id, 64, true))
        return false;
    if (strncmp(websocket_url, "wss://", 6) != 0)
        return false;
    const char *authority = websocket_url + 6;
    const char *path = strchr(authority, '/');
    if (path == NULL || path == authority || (size_t)(path - authority) > 270)
        return false;
    char origin[280], validated[1024], expected[1024], suffix[100];
    int n = snprintf(origin, sizeof(origin), "https://%.*s", (int)(path - authority), authority);
    if (n <= 0 || (size_t)n >= sizeof(origin) ||
        !metaserver_url_access(origin, NULL, false, validated, sizeof(validated)))
        return false;
    bool ok = metaserver_url_access(origin, server_id, true, expected, sizeof(expected)) &&
              strcmp(expected, websocket_url) == 0;
    if (!ok) {
        snprintf(suffix, sizeof(suffix), "/servers/%s?role=client", server_id);
        size_t path_size = strlen(path), suffix_size = strlen(suffix);
        if (path_size < suffix_size || strcmp(path + path_size - suffix_size, suffix) != 0)
            return false;
        n = snprintf(validated,
                     sizeof(validated),
                     "%s%.*s",
                     origin,
                     (int)(path_size - suffix_size),
                     path);
        ok =
            n > 0 && (size_t)n < sizeof(validated) &&
            metaserver_url_rendezvous(validated, server_id, "client", expected, sizeof(expected)) &&
            strcmp(expected, websocket_url) == 0;
    }
    if (!ok)
        return false;
    n = snprintf(out, out_size, "%s/v1/rendezvous/challenge/classic/%s", origin, server_id);
    if (n <= 0 || (size_t)n >= out_size) {
        out[0] = 0;
        return false;
    }
    return true;
}

static bool admission_decimal(const char *start, size_t size, uint64_t *value) {
    if (size == 0 || size > 12 || start[0] < '1' || start[0] > '9')
        return false;
    uint64_t result = 0;
    for (size_t i = 0; i < size; i++) {
        if (start[i] < '0' || start[i] > '9')
            return false;
        result = result * 10U + (unsigned int)(start[i] - '0');
    }
    if (result > UINT64_C(253402300799))
        return false;
    *value = result;
    return true;
}

static bool
admission_token(const char *token, const char *ticket_hash, uint64_t now, uint64_t *expires_at) {
    size_t size = strlen(token);
    if (size > RENDEZVOUS_ADMISSION_TOKEN_MAX || strncmp(token, "v1.", 3) != 0)
        return false;
    const char *key = token + 3, *key_end = strchr(key, '.');
    if (key_end == NULL || key_end == key || key_end - key > 32)
        return false;
    for (const char *p = key; p < key_end; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-'))
            return false;
    const char *issued = key_end + 1, *issued_end = strchr(issued, '.');
    uint64_t iat;
    if (issued_end == NULL || !admission_decimal(issued, (size_t)(issued_end - issued), &iat) ||
        iat > UINT64_C(253402300799) - 60 || iat > now || now - iat >= 60)
        return false;
    const char *hash = issued_end + 1;
    if (strlen(hash) != 64 + 1 + 32 + 1 + 64)
        return false;
    for (size_t i = 0; i < 162; i++) {
        if (i == 64 || i == 97) {
            if (hash[i] != '.')
                return false;
        } else if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
            return false;
    }
    if (ticket_hash != NULL && CRYPTO_memcmp(hash, ticket_hash, 64) != 0)
        return false;
    *expires_at = iat + 60;
    return true;
}

bool rendezvous_admission_parse(const char *body,
                                size_t size,
                                const char *ticket_hash,
                                uint64_t now,
                                rendezvous_admission_challenge_t *out) {
    if (out == NULL)
        return false;
    OPENSSL_cleanse(out, sizeof(*out));
    static const char prefix[] = "{\"schema\":\"" ADMISSION_SCHEMA "\",\"challenge\":\"";
    static const char middle[] = "\",\"difficulty\":24,\"expiresAt\":\"";
    if (body == NULL || size > 512 || size < sizeof(prefix) + sizeof(middle) ||
        memchr(body, 0, size) != NULL || !string_is_hex_fixed(ticket_hash, 64, true) ||
        memcmp(body, prefix, sizeof(prefix) - 1) != 0)
        return false;
    char copy[513];
    memcpy(copy, body, size);
    copy[size] = 0;
    char *token = copy + sizeof(prefix) - 1, *end = strchr(token, '"');
    bool ok = end != NULL && (size_t)(end - token) <= RENDEZVOUS_ADMISSION_TOKEN_MAX &&
              strncmp(end, middle, sizeof(middle) - 1) == 0;
    rendezvous_admission_challenge_t parsed = {0};
    if (ok) {
        memcpy(parsed.token, token, (size_t)(end - token));
        const char *expiry = end + sizeof(middle) - 1, *expiry_end = strchr(expiry, '"');
        uint64_t declared = 0;
        ok = expiry_end != NULL && strcmp(expiry_end, "\"}") == 0 &&
             admission_decimal(expiry, (size_t)(expiry_end - expiry), &declared) &&
             admission_token(parsed.token, ticket_hash, now, &parsed.expires_at) &&
             declared == parsed.expires_at;
    }
    if (ok)
        *out = parsed;
    OPENSSL_cleanse(&parsed, sizeof(parsed));
    OPENSSL_cleanse(copy, sizeof(copy));
    return ok;
}

/* Private parameterization permits fast deterministic unit tests without a
 * production API capable of negotiating weaker work or increasing the budget. */
static rendezvous_admission_result_t
admission_solve(const rendezvous_admission_challenge_t *challenge,
                uint64_t deadline_ms,
                const curl_cancel_t *cancel,
                char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U],
                unsigned int difficulty,
                uint64_t trials,
                uint64_t (*clock_ms)(void),
                time_t (*clock_wall)(time_t *)) {
    if (proof == NULL)
        return RENDEZVOUS_ADMISSION_PROTOCOL;
    OPENSSL_cleanse(proof, RENDEZVOUS_ADMISSION_PROOF_MAX + 1U);
    uint64_t expiry = 0;
    time_t wall = clock_wall(NULL);
    if (challenge == NULL || memchr(challenge->token, 0, sizeof(challenge->token)) == NULL ||
        wall < 0 || difficulty == 0 || difficulty > 256 ||
        !admission_token(challenge->token, NULL, (uint64_t)wall, &expiry) ||
        expiry != challenge->expires_at)
        return RENDEZVOUS_ADMISSION_PROTOCOL;
    uint64_t start = clock_ms();
    uint64_t budget = MIN(RENDEZVOUS_ADMISSION_BUDGET_MS, (expiry - (uint64_t)wall) * 1000U);
    uint64_t finish = MIN(deadline_ms, start > UINT64_MAX - budget ? UINT64_MAX : start + budget);
    char input[sizeof(WORK_DOMAIN) + RENDEZVOUS_ADMISSION_TOKEN_MAX + 1 + 16];
    size_t token_size = strlen(challenge->token);
    memcpy(input, WORK_DOMAIN, sizeof(WORK_DOMAIN));
    memcpy(input + sizeof(WORK_DOMAIN), challenge->token, token_size + 1);
    size_t prefix_size = sizeof(WORK_DOMAIN) + token_size + 1;
    unsigned char digest[32];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    rendezvous_admission_result_t result = RENDEZVOUS_ADMISSION_UNAVAILABLE;
    for (uint64_t trial = 0; ctx != NULL && trial < MIN(trials, RENDEZVOUS_ADMISSION_TRIALS_MAX);
         trial++) {
        if ((trial & 4095U) == 0) {
            if (curl_cancelled(cancel)) {
                result = RENDEZVOUS_ADMISSION_CANCELLED;
                break;
            }
            if (clock_ms() >= finish) {
                result = RENDEZVOUS_ADMISSION_TIMEOUT;
                break;
            }
        }
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 16; i++)
            input[prefix_size + i] = hex[(trial >> ((15U - i) * 4U)) & 15U];
        unsigned int written = 0;
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
            EVP_DigestUpdate(ctx, input, prefix_size + 16) != 1 ||
            EVP_DigestFinal_ex(ctx, digest, &written) != 1 || written != sizeof(digest))
            break;
        bool match = true;
        for (unsigned int bit = 0; bit < difficulty; bit++)
            match = match && (digest[bit / 8] & (1U << (7U - bit % 8))) == 0;
        if (match) {
            if (curl_cancelled(cancel)) {
                result = RENDEZVOUS_ADMISSION_CANCELLED;
                break;
            }
            if (clock_ms() >= finish || clock_wall(NULL) >= (time_t)expiry) {
                result = RENDEZVOUS_ADMISSION_TIMEOUT;
                break;
            }
            memcpy(proof, challenge->token, token_size);
            proof[token_size] = '.';
            memcpy(proof + token_size + 1, input + prefix_size, 16);
            proof[token_size + 17] = 0;
            result = RENDEZVOUS_ADMISSION_OK;
            break;
        }
    }
    EVP_MD_CTX_free(ctx);
    OPENSSL_cleanse(input, sizeof(input));
    OPENSSL_cleanse(digest, sizeof(digest));
    return result;
}

rendezvous_admission_result_t
rendezvous_admission_solve(const rendezvous_admission_challenge_t *challenge,
                           uint64_t deadline_ms,
                           const curl_cancel_t *cancel,
                           char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U]) {
    return admission_solve(challenge,
                           deadline_ms,
                           cancel,
                           proof,
                           RENDEZVOUS_ADMISSION_DIFFICULTY,
                           RENDEZVOUS_ADMISSION_TRIALS_MAX,
                           datetime_monotonic_ms,
                           time);
}

typedef struct admission_cancel {
    uint64_t deadline_ms;
    const curl_cancel_t *caller;
} admission_cancel_t;

static bool admission_cancelled(void *context) {
    const admission_cancel_t *cancel = context;
    return datetime_monotonic_ms() >= cancel->deadline_ms || curl_cancelled(cancel->caller);
}

typedef struct admission_response {
    char body[513];
    size_t used;
    size_t header_bytes;
    bool no_store;
} admission_response_t;

static size_t admission_body(char *data, size_t size, size_t count, void *context) {
    admission_response_t *response = context;
    if (size != 0 && count > SIZE_MAX / size)
        return 0;
    size_t n = size * count;
    if (n > sizeof(response->body) - 1 - response->used)
        return 0;
    memcpy(response->body + response->used, data, n);
    response->used += n;
    return n;
}

static size_t admission_header(char *data, size_t size, size_t count, void *context) {
    admission_response_t *response = context;
    if (size != 0 && count > SIZE_MAX / size)
        return 0;
    size_t n = size * count;
    if (n > 8192 - response->header_bytes)
        return 0;
    response->header_bytes += n;
    if (n >= 5 && memcmp(data, "HTTP/", 5) == 0)
        response->no_store = false;
    static const char name[] = "Cache-Control:";
    if (n >= sizeof(name) - 1 && strncasecmp(data, name, sizeof(name) - 1) == 0) {
        const char *p = data + sizeof(name) - 1, *end = data + n;
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
            end--;
        response->no_store = (size_t)(end - p) == 8 && strncasecmp(p, "no-store", 8) == 0;
    }
    return n;
}

/* Tests may inject only a private solver callback into this translation unit;
 * the public entry point below always selects the production fixed24 solver. */
static rendezvous_admission_result_t
admission_prepare(const char *websocket_url,
                  const char *server_id,
                  const char *ticket,
                  uint64_t deadline_ms,
                  const curl_cancel_t *cancel,
                  char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U],
                  rendezvous_admission_result_t (*solver)(const rendezvous_admission_challenge_t *,
                                                          uint64_t,
                                                          const curl_cancel_t *,
                                                          char *)) {
    if (proof == NULL)
        return RENDEZVOUS_ADMISSION_PROTOCOL;
    OPENSSL_cleanse(proof, RENDEZVOUS_ADMISSION_PROOF_MAX + 1U);
    if (curl_cancelled(cancel))
        return RENDEZVOUS_ADMISSION_CANCELLED;
    uint64_t now_ms = datetime_monotonic_ms();
    if (now_ms >= deadline_ms)
        return RENDEZVOUS_ADMISSION_TIMEOUT;
    char url[1024], hash[65] = {0}, request[256] = {0};
    if (!rendezvous_admission_url(websocket_url, server_id, url, sizeof(url)) ||
        !rendezvous_admission_ticket_hash(ticket, hash))
        return RENDEZVOUS_ADMISSION_PROTOCOL;
    int n = snprintf(request,
                     sizeof(request),
                     "{\"schema\":\"" ADMISSION_SCHEMA "\",\"ticketHash\":\"%s\"}",
                     hash);
    admission_response_t response = {0};
    rendezvous_admission_challenge_t challenge = {0};
    rendezvous_admission_result_t result = RENDEZVOUS_ADMISSION_UNAVAILABLE;
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    struct curl_slist *added =
        headers != NULL ? curl_slist_append(headers, "Cache-Control: no-store") : NULL;
    if (added != NULL)
        headers = added;
    if (curl != NULL && added != NULL && n > 0 && (size_t)n < sizeof(request)) {
        long timeout = (long)MIN(deadline_ms - now_ms, (uint64_t)LONG_MAX);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)n);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
        curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, admission_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, admission_header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
#ifdef WIN32
        curl_easy_setopt(curl, CURLOPT_CAINFO, "ca-bundle.crt");
#endif
        admission_cancel_t cancellation = {.deadline_ms = deadline_ms, .caller = cancel};
        curl_cancel_t bounded = {.cancelled = admission_cancelled, .context = &cancellation};
        CURLcode transferred = curl_perform_cancellable(curl, &bounded);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        time_t now = time(NULL);
        if (transferred == CURLE_OPERATION_TIMEDOUT || datetime_monotonic_ms() >= deadline_ms)
            result = RENDEZVOUS_ADMISSION_TIMEOUT;
        else if (transferred == CURLE_ABORTED_BY_CALLBACK)
            result = RENDEZVOUS_ADMISSION_CANCELLED;
        else if (transferred == CURLE_OK && status == 200) {
            result = RENDEZVOUS_ADMISSION_PROTOCOL;
            if (response.no_store && now >= 0 &&
                rendezvous_admission_parse(response.body,
                                           response.used,
                                           hash,
                                           (uint64_t)now,
                                           &challenge))
                result = solver(&challenge, deadline_ms, cancel, proof);
        }
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    OPENSSL_cleanse(hash, sizeof(hash));
    OPENSSL_cleanse(request, sizeof(request));
    OPENSSL_cleanse(&response, sizeof(response));
    OPENSSL_cleanse(&challenge, sizeof(challenge));
    return result;
}

rendezvous_admission_result_t
rendezvous_admission_prepare(const char *websocket_url,
                             const char *server_id,
                             const char *ticket,
                             uint64_t deadline_ms,
                             const curl_cancel_t *cancel,
                             char proof[RENDEZVOUS_ADMISSION_PROOF_MAX + 1U]) {
    return admission_prepare(websocket_url,
                             server_id,
                             ticket,
                             deadline_ms,
                             cancel,
                             proof,
                             rendezvous_admission_solve);
}

void rendezvous_admission_headers_free(struct curl_slist *headers) {
    for (struct curl_slist *item = headers; item != NULL; item = item->next)
        if (item->data != NULL)
            OPENSSL_cleanse(item->data, strlen(item->data));
    curl_slist_free_all(headers);
}

static bool admission_append(struct curl_slist **headers, const char *name, const char *value) {
    char line[320];
    int n = snprintf(line, sizeof(line), "%s: %s", name, value);
    struct curl_slist *added =
        n > 0 && (size_t)n < sizeof(line) ? curl_slist_append(*headers, line) : NULL;
    OPENSSL_cleanse(line, sizeof(line));
    if (added == NULL)
        return false;
    *headers = added;
    return true;
}

struct curl_slist *rendezvous_admission_headers(const char *proof,
                                                const char *ticket,
                                                const rendezvous_access_grant_t *grant) {
    if (proof == NULL || strlen(proof) > RENDEZVOUS_ADMISSION_PROOF_MAX ||
        !string_is_hex_fixed(ticket, 64, true))
        return NULL;
    for (const char *p = proof; *p != 0; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            return NULL;
    struct curl_slist *headers = NULL;
    bool ok = *proof != 0 && admission_append(&headers, "X-Atrinik-Rendezvous-Admission", proof);
    if (grant != NULL) {
        char bearer[72];
        ok = ok && string_is_hex_fixed(grant->grant, 64, true) &&
             string_is_hex_fixed(grant->client_nonce, 64, true) &&
             strcmp(ticket, grant->grant) == 0;
        if (ok) {
            snprintf(bearer, sizeof(bearer), "Bearer %s", grant->grant);
            ok =
                admission_append(&headers, "Authorization", bearer) &&
                admission_append(&headers, "X-Atrinik-Access-Client-Nonce", grant->client_nonce) &&
                admission_append(&headers, "Sec-WebSocket-Protocol", RENDEZVOUS_ACCESS_SUBPROTOCOL);
        }
        OPENSSL_cleanse(bearer, sizeof(bearer));
    } else
        ok = ok && admission_append(&headers, "X-Atrinik-Rendezvous-Ticket", ticket);
    if (!ok) {
        rendezvous_admission_headers_free(headers);
        return NULL;
    }
    return headers;
}
