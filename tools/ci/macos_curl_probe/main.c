/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <curl/curl.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>
#ifdef ATRINIK_CURL_COHORT
#include <ares.h>
#endif

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

static int symbol_provider(const char *name, const void *symbol, int *first)
{
    Dl_info provider = {0};
    if (symbol == NULL || dladdr(symbol, &provider) == 0 || provider.dli_fname == NULL) {
        return 0;
    }
    if (!*first) putchar(',');
    *first = 0;
    json_string(name);
    putchar(':');
    json_string(provider.dli_fname);
    return 1;
}

/* Read loaded headers, including OS images resident only in dyld's cache. */
static int image_inventory(void)
{
    uint32_t count = _dyld_image_count();
    if (count == 0 || count > 4096) return 0;
    printf(",\"loaded_images\":[");
    for (uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name == NULL) return 0;
        if (i != 0) putchar(',');
        json_string(name);
    }
    printf("],\"dependency_install_names\":{");
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header *header = _dyld_get_image_header(i);
        if (header == NULL || header->magic != MH_MAGIC_64) return 0;
        const struct mach_header_64 *native = (const struct mach_header_64 *) header;
        if (native->ncmds > 16384 || native->sizeofcmds > 16U * 1024U * 1024U) return 0;
        const unsigned char *cursor = (const unsigned char *) (native + 1);
        size_t remaining = native->sizeofcmds;
        if (i != 0) putchar(',');
        json_string(_dyld_get_image_name(i));
        printf(":[");
        int first = 1;
        for (uint32_t command = 0; command < native->ncmds; command++) {
            if (remaining < sizeof(struct load_command)) return 0;
            const struct load_command *load = (const struct load_command *) cursor;
            if (load->cmdsize < sizeof(*load) || load->cmdsize > remaining || load->cmdsize % 8 != 0) return 0;
            if (load->cmd == LC_LOAD_DYLIB || load->cmd == LC_LOAD_WEAK_DYLIB ||
                load->cmd == LC_REEXPORT_DYLIB || load->cmd == LC_LOAD_UPWARD_DYLIB) {
                if (load->cmdsize < sizeof(struct dylib_command)) return 0;
                const struct dylib_command *dylib = (const struct dylib_command *) load;
                uint32_t offset = dylib->dylib.name.offset;
                if (offset < sizeof(*dylib) || offset >= load->cmdsize ||
                    memchr(cursor + offset, '\0', load->cmdsize - offset) == NULL) return 0;
                if (!first) putchar(',');
                first = 0;
                json_string((const char *) cursor + offset);
            }
            remaining -= load->cmdsize;
            cursor += load->cmdsize;
        }
        if (remaining != 0) return 0;
        putchar(']');
    }
    putchar('}');
    return 1;
}

/* No curlrc, proxy, netrc, credentials, redirects, or weakened TLS checks. */
static int tls_request(const char *url, const char *bundle, const char *resolve)
{
    CURL *handle = curl_easy_init();
    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    long verify_result = -1;
    char error[CURL_ERROR_SIZE] = {0};
    const char *default_bundle = NULL;
    const char *default_path = NULL;
    struct curl_slist *addresses = NULL;
    if (handle != NULL) {
#define SET(option, value) do { \
    result = curl_easy_setopt(handle, option, value); \
    if (result != CURLE_OK) goto finished; \
} while (0)
        result = curl_easy_getinfo(handle, CURLINFO_CAINFO, &default_bundle);
        if (result != CURLE_OK) goto finished;
        result = curl_easy_getinfo(handle, CURLINFO_CAPATH, &default_path);
        if (result != CURLE_OK) goto finished;
        SET(CURLOPT_URL, url);
        SET(CURLOPT_PROXY, "");
        SET(CURLOPT_NETRC, CURL_NETRC_IGNORED);
        SET(CURLOPT_FOLLOWLOCATION, 0L);
        SET(CURLOPT_SSL_VERIFYPEER, 1L);
        SET(CURLOPT_SSL_VERIFYHOST, 2L);
        SET(CURLOPT_CONNECTTIMEOUT, 15L);
        SET(CURLOPT_TIMEOUT, 45L);
        SET(CURLOPT_WRITEFUNCTION, discard);
        SET(CURLOPT_ERRORBUFFER, error);
        if (resolve != NULL) {
            addresses = curl_slist_append(NULL, resolve);
            if (addresses == NULL) {
                result = CURLE_OUT_OF_MEMORY;
                goto finished;
            }
            SET(CURLOPT_RESOLVE, addresses);
        }
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
    curl_slist_free_all(addresses);
    return result == CURLE_OK && status >= 200 && status < 400;
}

int main(int argc, char **argv)
{
#ifdef ATRINIK_CURL_COHORT
    if (argc != 5) {
        return 2;
    }
#else
    if (argc != 2) {
        return 2;
    }
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return 2;
    }
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    Dl_info provider = {0};
    const void *symbol = (const void *) curl_version_info;
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
    int default_ok = tls_request("https://curl.se/", NULL, NULL);
    printf(",\"public_tls_bundle\":");
    int bundle_ok = tls_request("https://curl.se/", argv[1], NULL);
#ifdef ATRINIK_CURL_COHORT
    Dl_info cares_provider = {0};
    const void *cares_symbol = (const void *) ares_version;
    if (cares_symbol == NULL || dladdr(cares_symbol, &cares_provider) == 0) {
        curl_global_cleanup();
        return 2;
    }
    printf(",\"cares_version\":");
    json_string(ares_version(NULL));
    printf(",\"cares_header_version\":");
    json_string(ARES_VERSION_STR);
    printf(",\"loaded_cares_library\":");
    json_string(cares_provider.dli_fname);
    printf(",\"public_tls_unrelated\":");
    tls_request("https://curl.se/", argv[2], NULL);
    char trusted_url[128], wrong_url[128], wrong_resolve[128];
    int trusted_size = snprintf(trusted_url, sizeof(trusted_url), "https://localhost:%s/", argv[4]);
    int wrong_size = snprintf(wrong_url, sizeof(wrong_url), "https://wrong.invalid:%s/", argv[4]);
    int resolve_size = snprintf(wrong_resolve, sizeof(wrong_resolve), "wrong.invalid:%s:127.0.0.1", argv[4]);
    if (trusted_size < 0 || (size_t) trusted_size >= sizeof(trusted_url) ||
        wrong_size < 0 || (size_t) wrong_size >= sizeof(wrong_url) ||
        resolve_size < 0 || (size_t) resolve_size >= sizeof(wrong_resolve)) {
        curl_global_cleanup();
        return 2;
    }
    printf(",\"private_tls_trusted\":");
    int private_ok = tls_request(trusted_url, argv[3], NULL);
    printf(",\"private_tls_untrusted\":");
    tls_request(trusted_url, NULL, NULL);
    printf(",\"private_tls_wrong_hostname\":");
    tls_request(wrong_url, argv[3], wrong_resolve);
#else
    int private_ok = 1;
#endif
    printf(",\"symbol_providers\":{");
    int first = 1;
#define PROVIDER(name) do { \
    if (!symbol_provider(#name, (const void *) name, &first)) return 2; \
} while (0)
    PROVIDER(curl_global_init);
    PROVIDER(curl_global_cleanup);
    PROVIDER(curl_version_info);
    PROVIDER(curl_easy_init);
    PROVIDER(curl_easy_setopt);
    PROVIDER(curl_easy_getinfo);
    PROVIDER(curl_easy_perform);
    PROVIDER(curl_easy_cleanup);
    PROVIDER(curl_slist_append);
    PROVIDER(curl_slist_free_all);
#ifdef ATRINIK_CURL_COHORT
    PROVIDER(ares_version);
#endif
#undef PROVIDER
    putchar('}');
    if (!image_inventory()) return 2;
    printf("}\n");
    curl_global_cleanup();
    return default_ok && bundle_ok && private_ok ? 0 : 1;
}
