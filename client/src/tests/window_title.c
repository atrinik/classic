#include <window_title.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <version.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    TEST_CHECK(!client_window_title_init(NULL));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);

    TEST_CHECK(client_window_title_init("topology review - profile classic"));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME " — topology review - profile classic") ==
               0);
    char preserved[256];
    snprintf(preserved, sizeof(preserved), "%s", client_window_title());
    TEST_CHECK(strcmp(client_window_title(), preserved) == 0);

    TEST_CHECK(!client_window_title_init(""));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);
    TEST_CHECK(!client_window_title_init("profile classic\nspoofed"));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);

    char boundary[CLIENT_LAUNCH_LABEL_MAX_SIZE + 1];
    memset(boundary, 'a', sizeof(boundary) - 1);
    boundary[sizeof(boundary) - 1] = '\0';
    TEST_CHECK(client_window_title_init(boundary));
    TEST_CHECK(strstr(client_window_title(), boundary) != NULL);

    char oversized[CLIENT_LAUNCH_LABEL_MAX_SIZE + 2];
    memset(oversized, 'b', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    TEST_CHECK(!client_window_title_init(oversized));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);
    return 0;
}
