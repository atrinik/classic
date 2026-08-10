/**
 * @file
 * Canonical metaserver service URL construction.
 */

#include "metaserver_url.h"

#include <curl/curl.h>
#include <idn2.h>

typedef struct metaserver_parsed_url {
    CURLU *handle;
    char *scheme;
    char *rendered;
    char *path;
    char *host;
} metaserver_parsed_url_t;

static void metaserver_parsed_url_free(metaserver_parsed_url_t *parsed) {
    curl_free(parsed->scheme);
    curl_free(parsed->rendered);
    curl_free(parsed->path);
    curl_free(parsed->host);
    if (parsed->handle != NULL) {
        curl_url_cleanup(parsed->handle);
    }
    memset(parsed, 0, sizeof(*parsed));
}

static bool metaserver_url_ascii_valid(const char *url) {
    if (url == NULL || *url == '\0' || strlen(url) >= MAX_BUF) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)url; *cp != '\0'; cp++) {
        if (*cp <= 0x20U || *cp >= 0x7fU || *cp == '%' || *cp == '?' || *cp == '#' || *cp == '@' ||
            *cp == '\\') {
            return false;
        }
    }
    return true;
}

static bool metaserver_path_valid(const char *path) {
    if (path == NULL || *path != '/' || strstr(path, "//") != NULL) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)path; *cp != '\0'; cp++) {
        if (*cp <= 0x20U || *cp >= 0x7fU || *cp == '%' || *cp == '?' || *cp == '#' || *cp == '\\') {
            return false;
        }
    }
    const char *part = path;
    while (*part != '\0') {
        const char *next = strchr(part + 1, '/');
        size_t size = next != NULL ? (size_t)(next - part - 1) : strlen(part + 1);
        if ((size == 1 && part[1] == '.') || (size == 2 && part[1] == '.' && part[2] == '.')) {
            return false;
        }
        if (next == NULL) {
            break;
        }
        part = next;
    }
    return true;
}

static bool metaserver_url_host_valid(const char *host) {
    if (strcmp(host, "localhost") == 0 || metaserver_hostname_valid(host)) {
        return true;
    }

    struct in_addr address4;
    char rendered[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, host, &address4) == 1) {
        return inet_ntop(AF_INET, &address4, VS(rendered)) != NULL && strcmp(rendered, host) == 0;
    }

    size_t host_size = strlen(host);
    const char *numeric = host;
    char unbracketed[INET6_ADDRSTRLEN];
    if (host_size >= 2 && host[0] == '[' && host[host_size - 1] == ']') {
        if (host_size - 2 >= sizeof(unbracketed)) {
            return false;
        }
        memcpy(unbracketed, host + 1, host_size - 2);
        unbracketed[host_size - 2] = '\0';
        numeric = unbracketed;
    }
    struct in6_addr address6;
    return inet_pton(AF_INET6, numeric, &address6) == 1 &&
           inet_ntop(AF_INET6, &address6, VS(rendered)) != NULL && strcmp(rendered, numeric) == 0;
}

static bool metaserver_url_parse(const char *url, metaserver_parsed_url_t *parsed) {
    memset(parsed, 0, sizeof(*parsed));
    if (!metaserver_url_ascii_valid(url)) {
        return false;
    }

    parsed->handle = curl_url();
    char *user = NULL;
    char *password = NULL;
    bool ok = parsed->handle != NULL &&
              curl_url_set(parsed->handle, CURLUPART_URL, url, 0) == CURLUE_OK &&
              curl_url_get(parsed->handle, CURLUPART_SCHEME, &parsed->scheme, 0) == CURLUE_OK &&
              (strcmp(parsed->scheme, "https") == 0 || strcmp(parsed->scheme, "http") == 0) &&
              curl_url_get(parsed->handle, CURLUPART_HOST, &parsed->host, 0) == CURLUE_OK &&
              *parsed->host != '\0' && metaserver_url_host_valid(parsed->host) &&
              curl_url_get(parsed->handle, CURLUPART_PATH, &parsed->path, 0) == CURLUE_OK &&
              metaserver_path_valid(parsed->path) &&
              curl_url_get(parsed->handle, CURLUPART_URL, &parsed->rendered, 0) == CURLUE_OK;
    CURLUcode user_result = parsed->handle != NULL
                                ? curl_url_get(parsed->handle, CURLUPART_USER, &user, 0)
                                : CURLUE_BAD_HANDLE;
    CURLUcode password_result = parsed->handle != NULL
                                    ? curl_url_get(parsed->handle, CURLUPART_PASSWORD, &password, 0)
                                    : CURLUE_BAD_HANDLE;
    ok = ok && user_result == CURLUE_NO_USER && password_result == CURLUE_NO_PASSWORD;
    curl_free(user);
    curl_free(password);

    if (ok) {
        size_t original_size = strlen(url);
        size_t rendered_size = strlen(parsed->rendered);
        ok = strcmp(url, parsed->rendered) == 0 ||
             (rendered_size == original_size + 1 && parsed->rendered[rendered_size - 1] == '/' &&
              strncmp(url, parsed->rendered, original_size) == 0);
    }
    if (!ok) {
        metaserver_parsed_url_free(parsed);
    }
    return ok;
}

static bool metaserver_identity_valid(const char *server_id) {
    if (server_id == NULL || strlen(server_id) != 64) {
        return false;
    }
    for (const char *cp = server_id; *cp != '\0'; cp++) {
        if (!(*cp >= '0' && *cp <= '9') && !(*cp >= 'a' && *cp <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool metaserver_hostname_alabel_valid(const char *label, size_t label_size) {
    if (label_size < 5 || strncmp(label, "xn--", 4) != 0) {
        return true;
    }
    char hostname[70];
    memcpy(hostname, label, label_size);
    hostname[label_size] = '\0';
    char *roundtrip = NULL;
    bool ok = idn2_to_ascii_8z(hostname,
                               &roundtrip,
                               IDN2_NFC_INPUT | IDN2_ALABEL_ROUNDTRIP | IDN2_NONTRANSITIONAL |
                                   IDN2_USE_STD3_ASCII_RULES) == IDN2_OK &&
              strcmp(roundtrip, hostname) == 0;
    idn2_free(roundtrip);
    return ok;
}

bool metaserver_hostname_valid(const char *hostname) {
    if (hostname == NULL) {
        return false;
    }
    size_t hostname_size = strlen(hostname);
    if (hostname_size < 3 || hostname_size > 253 || hostname[hostname_size - 1] == '.') {
        return false;
    }
    bool has_letter = false;
    bool has_non_numeric_label = false;
    size_t labels = 0;
    const char *label = hostname;
    while (*label != '\0') {
        const char *end = strchr(label, '.');
        size_t size = end != NULL ? (size_t)(end - label) : strlen(label);
        if (size == 0 || size > 63 || label[0] == '-' || label[size - 1] == '-' ||
            !metaserver_hostname_alabel_valid(label, size)) {
            return false;
        }
        bool decimal = true;
        bool prefixed_hex = size > 2 && label[0] == '0' && label[1] == 'x';
        for (size_t i = 0; i < size; i++) {
            char cp = label[i];
            if (cp >= 'a' && cp <= 'z') {
                has_letter = true;
            } else if (!(cp >= '0' && cp <= '9') && cp != '-') {
                return false;
            }
            decimal = decimal && cp >= '0' && cp <= '9';
            if (i >= 2 && prefixed_hex) {
                prefixed_hex = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f');
            }
        }
        has_non_numeric_label = has_non_numeric_label || (!decimal && !prefixed_hex);
        labels++;
        if (end == NULL) {
            break;
        }
        label = end + 1;
    }
    return labels >= 2 && has_letter && has_non_numeric_label;
}

bool metaserver_url_directory_valid(const char *url) {
    metaserver_parsed_url_t parsed;
    if (!metaserver_url_parse(url, &parsed)) {
        return false;
    }
    bool valid = strcmp(parsed.path, "/") != 0 && parsed.path[strlen(parsed.path) - 1] != '/' &&
                 strcmp(parsed.host, "publish.meta.atrinik.org") != 0 &&
                 strcmp(parsed.host, "rendezvous.meta.atrinik.org") != 0;
    metaserver_parsed_url_free(&parsed);
    return valid;
}

bool metaserver_url_publish(const char *origin,
                            const char *path,
                            char *url,
                            size_t url_size,
                            char *authority,
                            size_t authority_size) {
    HARD_ASSERT(url != NULL);
    HARD_ASSERT(authority != NULL);
    *url = '\0';
    *authority = '\0';
    if (path == NULL || *path != '/' || !metaserver_path_valid(path)) {
        return false;
    }

    metaserver_parsed_url_t parsed;
    if (!metaserver_url_parse(origin, &parsed)) {
        return false;
    }
    if (strcmp(parsed.path, "/") != 0 || strcmp(parsed.host, "classic.meta.atrinik.org") == 0 ||
        strcmp(parsed.host, "rendezvous.meta.atrinik.org") == 0) {
        metaserver_parsed_url_free(&parsed);
        return false;
    }

    const char *authority_start = strstr(parsed.rendered, "://");
    authority_start = authority_start != NULL ? authority_start + 3 : NULL;
    const char *authority_end = authority_start != NULL ? strchr(authority_start, '/') : NULL;
    size_t authority_length = authority_end != NULL ? (size_t)(authority_end - authority_start) : 0;
    size_t base_length = authority_end != NULL ? (size_t)(authority_end - parsed.rendered) : 0;
    bool ok = authority_length > 0 && authority_length < authority_size &&
              base_length + strlen(path) < url_size;
    if (ok) {
        memcpy(authority, authority_start, authority_length);
        authority[authority_length] = '\0';
        memcpy(url, parsed.rendered, base_length);
        snprintf(url + base_length, url_size - base_length, "%s", path);
    }
    metaserver_parsed_url_free(&parsed);
    return ok;
}

bool metaserver_url_rendezvous(const char *origin,
                               const char *server_id,
                               const char *role,
                               char *url,
                               size_t url_size) {
    HARD_ASSERT(url != NULL);
    *url = '\0';
    if (!metaserver_identity_valid(server_id) || role == NULL ||
        (strcmp(role, "client") != 0 && strcmp(role, "server") != 0)) {
        return false;
    }

    metaserver_parsed_url_t parsed;
    if (!metaserver_url_parse(origin, &parsed)) {
        return false;
    }
    if (strcmp(parsed.host, "classic.meta.atrinik.org") == 0 ||
        strcmp(parsed.host, "publish.meta.atrinik.org") == 0) {
        metaserver_parsed_url_free(&parsed);
        return false;
    }
    size_t path_length = strlen(parsed.path);
    char path[MAX_BUF];
    bool root = strcmp(parsed.path, "/") == 0;
    int path_result = root ? snprintf(VS(path), "/servers/%s", server_id)
                           : snprintf(VS(path),
                                      "%s%sservers/%s",
                                      parsed.path,
                                      parsed.path[path_length - 1] == '/' ? "" : "/",
                                      server_id);
    bool ok = path_result > 0 && path_result < (int)sizeof(path);
    char query[32];
    ok = ok && snprintf(VS(query), "role=%s", role) < (int)sizeof(query);
    const char *websocket_scheme = strcmp(parsed.scheme, "https") == 0 ? "wss" : "ws";
    char *rendered = NULL;
    ok = ok && curl_url_set(parsed.handle, CURLUPART_PATH, path, 0) == CURLUE_OK &&
         curl_url_set(parsed.handle, CURLUPART_QUERY, query, 0) == CURLUE_OK &&
         curl_url_get(parsed.handle, CURLUPART_URL, &rendered, 0) == CURLUE_OK;
    if (ok) {
        const char *scheme_end = strstr(rendered, "://");
        int rendered_result =
            scheme_end != NULL ? snprintf(url, url_size, "%s%s", websocket_scheme, scheme_end) : -1;
        ok = rendered_result > 0 && (size_t)rendered_result < url_size;
    }
    curl_free(rendered);
    metaserver_parsed_url_free(&parsed);
    if (!ok) {
        *url = '\0';
    }
    return ok;
}
