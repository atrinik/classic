/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <metaserver_options.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition)                                                       \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                \
        }                                                                           \
    } while (0)

static void test_access_origin_validation(void) {
    client_metaserver_options_t options = {0};
    const char *prefix = "https://classic.meta.atrinik.org/index.xml "
                         "https://rendezvous.meta.atrinik.org/v1/classic ";
    const char *valid[] = {
        "https://rendezvous.meta.atrinik.org",
        "https://access.example.org:8443",
        "http://127.0.0.1:8080",
        "http://[::1]:8080",
    };
    const char *invalid[] = {
        "http://access.example.org", "http://localhost:8080",
        "https://access.example.org/path", "https://access.example.org?query=1",
        "https://access.example.org#fragment", "https://user@access.example.org",
    };
    char value[1024];
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        int size = snprintf(value, sizeof(value), "%s%s", prefix, valid[i]);
        TEST_CHECK(size > 0 && (size_t)size < sizeof(value));
        TEST_CHECK(client_metaserver_options_parse(&options, value, NULL));
        TEST_CHECK(strcmp(options.endpoints[i].access_origin, valid[i]) == 0);
    }
    size_t admitted = options.count;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        int size = snprintf(value, sizeof(value), "%s%s", prefix, invalid[i]);
        TEST_CHECK(size > 0 && (size_t)size < sizeof(value));
        TEST_CHECK(!client_metaserver_options_parse(&options, value, NULL));
        TEST_CHECK(options.count == admitted);
    }
    TEST_CHECK(!client_metaserver_options_parse(&options,
        "https://classic.meta.atrinik.org/index.xml "
        "https://rendezvous.meta.atrinik.org/v1/classic", NULL));
    TEST_CHECK(options.count == admitted);
    client_metaserver_options_deinit(&options);
}

int main(void) {
    test_access_origin_validation();
    client_metaserver_options_t options = {0};
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.endpoints == NULL);
    TEST_CHECK(options.count == 0);

    client_metaserver_options_replace_provider(&options, METASERVER_PROVIDER_DEFAULT);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic.metaserver.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous.meta.atrinik.org/v1/classic") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].access_origin,
                      "https://rendezvous.meta.atrinik.org") == 0);
    TEST_CHECK(client_metaserver_options_provider(&options) == METASERVER_PROVIDER_DEFAULT);

    client_metaserver_options_replace_provider(&options, METASERVER_PROVIDER_DEV);
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic.dev.metaserver.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous.dev.meta.atrinik.org/v1/classic") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].access_origin,
                      "https://rendezvous.dev.meta.atrinik.org") == 0);
    TEST_CHECK(client_metaserver_options_provider(&options) == METASERVER_PROVIDER_DEV);

    client_metaserver_options_t copied = {0};
    client_metaserver_options_copy(&copied, &options);
    TEST_CHECK(copied.count == 1);
    TEST_CHECK(copied.endpoints != options.endpoints);
    TEST_CHECK(copied.endpoints[0].directory_url != options.endpoints[0].directory_url);
    TEST_CHECK(copied.endpoints[0].rendezvous_origin != options.endpoints[0].rendezvous_origin);
    TEST_CHECK(copied.endpoints[0].access_origin != options.endpoints[0].access_origin);
    options.endpoints[0].access_origin[8] = 'X';
    options.endpoints[0].directory_url[8] = 'X';
    options.endpoints[0].rendezvous_origin[8] = 'X';
    TEST_CHECK(strcmp(copied.endpoints[0].directory_url,
                      "https://classic.dev.metaserver.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(copied.endpoints[0].rendezvous_origin,
                      "https://rendezvous.dev.meta.atrinik.org/v1/classic") == 0);
    TEST_CHECK(strcmp(copied.endpoints[0].access_origin,
                      "https://rendezvous.dev.meta.atrinik.org") == 0);
    TEST_CHECK(client_metaserver_options_provider(&options) == METASERVER_PROVIDER_DEFAULT);
    client_metaserver_options_deinit(&copied);
    client_metaserver_options_deinit(&options);

    char directory[] = "https://classic.meta.atrinik.org/index.xml";
    char rendezvous[] = "https://rendezvous.meta.atrinik.org/v1/classic";
    char access[] = "https://rendezvous.meta.atrinik.org";
    client_metaserver_options_add(&options, directory, rendezvous, access);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url, directory) == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin, rendezvous) == 0);
    TEST_CHECK(strcmp(options.endpoints[0].access_origin, access) == 0);
    directory[8] = 'X';
    rendezvous[8] = 'X';
    access[8] = 'X';
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic.meta.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous.meta.atrinik.org/v1/classic") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].access_origin, "https://rendezvous.meta.atrinik.org") ==
               0);

    char *errmsg = NULL;
    TEST_CHECK(!client_metaserver_options_parse(&options,
                                                "https://classic.meta.atrinik.org/index.xml "
                                                "https://rendezvous.meta.atrinik.org/v1/classic "
                                                "https://rendezvous.meta.atrinik.org extra",
                                                &errmsg));
    TEST_CHECK(errmsg != NULL);
    free(errmsg);
    errmsg = NULL;
    TEST_CHECK(options.count == 1);
    TEST_CHECK(!client_metaserver_options_parse(&options, "", NULL));
    TEST_CHECK(!client_metaserver_options_parse(&options,
                                                "https://classic.meta.atrinik.org/index.xml "
                                                "https://only-signal.example.org/v1/classic"
                                                "?query=forbidden https://access.example.org",
                                                &errmsg));
    TEST_CHECK(errmsg != NULL);
    free(errmsg);
    errmsg = NULL;
    TEST_CHECK(options.count == 1);

    client_metaserver_options_disable(&options);
    TEST_CHECK(!client_metaserver_options_enabled(&options));
    TEST_CHECK(options.endpoints == NULL);
    TEST_CHECK(options.count == 0);
    TEST_CHECK(client_metaserver_options_provider(&options) == METASERVER_PROVIDER_DEFAULT);

    TEST_CHECK(
        client_metaserver_options_parse(&options,
                                        "https://classic-directory-canary.atrinik.org/index.xml "
                                        "https://rendezvous-canary.meta.atrinik.org/v1/classic "
                                        "https://access-canary.meta.atrinik.org",
                                        &errmsg));
    TEST_CHECK(errmsg == NULL);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic-directory-canary.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous-canary.meta.atrinik.org/v1/classic") == 0);
    TEST_CHECK(
        strcmp(options.endpoints[0].access_origin, "https://access-canary.meta.atrinik.org") == 0);

    TEST_CHECK(client_metaserver_options_parse(
        &options,
        "https://backup.example.org/index.xml https://signal.example.org/v1/classic "
        "https://access.example.org",
        NULL));
    TEST_CHECK(options.count == 2);

    client_metaserver_options_disable(&options);
    TEST_CHECK(
        !client_metaserver_options_parse(&options,
                                         "https://only.example.org/index.xml?query=forbidden "
                                         "https://only-signal.example.org/v1/classic "
                                         "https://access.example.org",
                                         &errmsg));
    TEST_CHECK(errmsg != NULL);
    free(errmsg);
    errmsg = NULL;
    TEST_CHECK(!client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 0);

    TEST_CHECK(client_metaserver_options_parse(
        &options,
        "https://only.example.org/index.xml https://only-signal.example.org/v1/classic "
        "https://access.example.org",
        &errmsg));
    TEST_CHECK(errmsg == NULL);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url, "https://only.example.org/index.xml") ==
               0);

    client_metaserver_options_deinit(&options);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.endpoints == NULL);
    TEST_CHECK(options.count == 0);
    client_metaserver_options_deinit(&options);
    return 0;
}
