/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 ************************************************************************/

#include <metaserver_options.h>

#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    client_metaserver_options_t options = {0};
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.endpoints == NULL);
    TEST_CHECK(options.count == 0);

    char directory[] = "https://classic.meta.atrinik.org/index.xml";
    char rendezvous[] = "https://rendezvous.meta.atrinik.org/v1/classic";
    client_metaserver_options_add(&options, directory, rendezvous);
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url, directory) == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin, rendezvous) == 0);
    directory[8] = 'X';
    rendezvous[8] = 'X';
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic.meta.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous.meta.atrinik.org/v1/classic") == 0);

    client_metaserver_options_disable(&options);
    TEST_CHECK(!client_metaserver_options_enabled(&options));
    TEST_CHECK(options.endpoints == NULL);
    TEST_CHECK(options.count == 0);

    client_metaserver_options_add(&options,
                                  "https://classic-directory-canary.atrinik.org/index.xml",
                                  "https://rendezvous-canary.meta.atrinik.org/v1/classic");
    TEST_CHECK(client_metaserver_options_enabled(&options));
    TEST_CHECK(options.count == 1);
    TEST_CHECK(strcmp(options.endpoints[0].directory_url,
                      "https://classic-directory-canary.atrinik.org/index.xml") == 0);
    TEST_CHECK(strcmp(options.endpoints[0].rendezvous_origin,
                      "https://rendezvous-canary.meta.atrinik.org/v1/classic") == 0);

    client_metaserver_options_add(&options,
                                  "https://backup.example.org/index.xml",
                                  "https://signal.example.org/v1/classic");
    TEST_CHECK(options.count == 2);

    client_metaserver_options_disable(&options);
    client_metaserver_options_add(&options,
                                  "https://only.example.org/index.xml",
                                  "https://only-signal.example.org/v1/classic");
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
