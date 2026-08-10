#include <stun_config.h>

#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    client_stun_config_t config = {0};
    client_stun_config_init(&config);
    TEST_CHECK(config.endpoint != NULL);
    TEST_CHECK(strcmp(config.endpoint, CLIENT_STUN_DEFAULT_ENDPOINT) == 0);
    TEST_CHECK(config.source == CLIENT_STUN_SOURCE_DEFAULT);
    TEST_CHECK(strcmp(client_stun_source_name(config.source), "packaged default") == 0);

    char *errmsg = NULL;
    TEST_CHECK(client_stun_config_set(&config, "127.0.0.1:3478", &errmsg));
    TEST_CHECK(errmsg == NULL);
    TEST_CHECK(strcmp(config.endpoint, "127.0.0.1:3478") == 0);
    TEST_CHECK(config.source == CLIENT_STUN_SOURCE_OVERRIDE);

    TEST_CHECK(client_stun_config_set(&config, "off", &errmsg));
    TEST_CHECK(config.endpoint == NULL);
    TEST_CHECK(config.source == CLIENT_STUN_SOURCE_DISABLED);
    TEST_CHECK(strcmp(client_stun_source_name(config.source), "disabled") == 0);

    TEST_CHECK(!client_stun_config_set(&config, "", &errmsg));
    TEST_CHECK(errmsg != NULL && strstr(errmsg, "empty") != NULL);
    TEST_CHECK(config.endpoint == NULL);
    TEST_CHECK(config.source == CLIENT_STUN_SOURCE_DISABLED);
    free(errmsg);

    client_stun_config_deinit(&config);
    return 0;
}
