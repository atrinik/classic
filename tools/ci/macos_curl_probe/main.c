/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <curl/curl.h>
#include <dlfcn.h>
#include <stdio.h>

static void json_string(const char *text)
{
    if (text == NULL) {
        printf("null");
        return;
    }
    putchar('"');
    if (text != NULL) {
        for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
            if (*p == '"' || *p == '\\') {
                putchar('\\');
                putchar(*p);
            } else if (*p < 32) {
                printf("\\u%04x", *p);
            } else {
                putchar(*p);
            }
        }
    }
    putchar('"');
}

static void string_array(const char *const *values)
{
    putchar('[');
    if (values != NULL) {
        for (size_t i = 0; values[i] != NULL; i++) {
            if (i != 0) {
                putchar(',');
            }
            json_string(values[i]);
        }
    }
    putchar(']');
}

static size_t discard(char *data, size_t size, size_t count, void *context)
{
    (void) data;
    (void) context;
    return size * count;
}

/* No curlrc, proxy, netrc, credentials, redirects, or weakened TLS checks. */
static int public_tls(const char *bundle)
{
    CURL *handle = curl_easy_init();
    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    long verify_result = -1;
    char error[CURL_ERROR_SIZE] = {0};
    const char *default_bundle = NULL;
    const char *default_path = NULL;
    if (handle != NULL) {
#define SET(option, value) do { \
    result = curl_easy_setopt(handle, option, value); \
    if (result != CURLE_OK) goto finished; \
} while (0)
        result = curl_easy_getinfo(handle, CURLINFO_CAINFO, &default_bundle);
        if (result != CURLE_OK) goto finished;
        result = curl_easy_getinfo(handle, CURLINFO_CAPATH, &default_path);
        if (result != CURLE_OK) goto finished;
        SET(CURLOPT_URL, "https://curl.se/");
        SET(CURLOPT_PROXY, "");
        SET(CURLOPT_NETRC, CURL_NETRC_IGNORED);
        SET(CURLOPT_FOLLOWLOCATION, 0L);
        SET(CURLOPT_SSL_VERIFYPEER, 1L);
        SET(CURLOPT_SSL_VERIFYHOST, 2L);
        SET(CURLOPT_CONNECTTIMEOUT, 15L);
        SET(CURLOPT_TIMEOUT, 45L);
        SET(CURLOPT_WRITEFUNCTION, discard);
        SET(CURLOPT_ERRORBUFFER, error);
        if (bundle != NULL) {
            SET(CURLOPT_CAINFO, bundle);
        }
        result = curl_easy_perform(handle);
        if (result == CURLE_OK) {
            result = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
            if (result == CURLE_OK) {
                result = curl_easy_getinfo(handle, CURLINFO_SSL_VERIFYRESULT, &verify_result);
            }
        }
finished:
#undef SET
        ;
    }
    printf("{\"code\":%d,\"status\":%ld,\"ssl_verify_result\":%ld,\"error\":",
           (int) result, status, verify_result);
    json_string(error);
    printf(",\"default_ca_bundle\":");
    json_string(default_bundle);
    printf(",\"default_ca_path\":");
    json_string(default_path);
    printf("}");
    curl_easy_cleanup(handle);
    return result == CURLE_OK && status >= 200 && status < 400;
}

int main(int argc, char **argv)
{
    if (argc != 2 || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return 2;
    }
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    Dl_info provider = {0};
    void *symbol = dlsym(RTLD_DEFAULT, "curl_version_info");
    if (info == NULL || symbol == NULL || dladdr(symbol, &provider) == 0 ||
        info->age < CURLVERSION_ELEVENTH) {
        curl_global_cleanup();
        return 2;
    }
    printf("{\"header_version\":");
    json_string(LIBCURL_VERSION);
    printf(",\"header_version_num\":%u,\"version\":", (unsigned) LIBCURL_VERSION_NUM);
    json_string(info->version);
    printf(",\"version_num\":%u,\"loaded_library\":", info->version_num);
    json_string(provider.dli_fname);
    printf(",\"host\":");
    json_string(info->host);
    printf(",\"ssl_version\":");
    json_string(info->ssl_version);
    printf(",\"ares\":");
    json_string(info->ares);
    printf(",\"ares_num\":%d,\"libz_version\":", info->ares_num);
    json_string(info->libz_version);
    printf(",\"libidn\":");
    json_string(info->libidn);
    printf(",\"libssh_version\":");
    json_string(info->libssh_version);
    printf(",\"brotli_version\":");
    json_string(info->brotli_version);
    printf(",\"nghttp2_version\":");
    json_string(info->nghttp2_version);
    printf(",\"quic_version\":");
    json_string(info->quic_version);
    printf(",\"zstd_version\":");
    json_string(info->zstd_version);
    printf(",\"features\":");
    string_array(info->feature_names);
    printf(",\"protocols\":");
    string_array(info->protocols);
    printf(",\"public_tls_default\":");
    int default_ok = public_tls(NULL);
    printf(",\"public_tls_bundle\":");
    int bundle_ok = public_tls(argv[1]);
    printf("}\n");
    curl_global_cleanup();
    return default_ok && bundle_ok ? 0 : 1;
}
