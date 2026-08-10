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
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    TEST_CHECK(SDL_Init(SDL_INIT_VIDEO));

    TEST_CHECK(!client_window_title_init(NULL));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);

    TEST_CHECK(client_window_title_init("topology review - profile classic"));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME " — topology review - profile classic") ==
               0);
    SDL_Window *window = client_window_create(640, 480, SDL_WINDOW_RESIZABLE);
    TEST_CHECK(window != NULL);
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);
    TEST_CHECK(SDL_SetWindowTitle(window, PACKAGE_NAME));
    client_window_title_apply(window);
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);
    TEST_CHECK(SDL_SetWindowSize(window, 800, 600));
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);
    SDL_SetWindowFullscreen(window, true);
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);
    SDL_SetWindowFullscreen(window, false);
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);

    TEST_CHECK(client_window_title_init("profile classic (direct run)"));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME " — profile classic (direct run)") == 0);
    client_window_title_apply(window);
    TEST_CHECK(strcmp(SDL_GetWindowTitle(window), client_window_title()) == 0);

    TEST_CHECK(!client_window_title_init(""));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);
    TEST_CHECK(!client_window_title_init("profile classic\nspoofed"));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);
    TEST_CHECK(!client_window_title_init("release build"));
    TEST_CHECK(!client_window_title_init("topology  - profile classic"));
    TEST_CHECK(!client_window_title_init("topology review - profile "));
    TEST_CHECK(!client_window_title_init("topology review - profile Classic"));
    TEST_CHECK(!client_window_title_init("profile classic direct run"));

    char boundary[CLIENT_LAUNCH_LABEL_MAX_SIZE + 1];
    memset(boundary, 'a', sizeof(boundary) - 1);
    boundary[sizeof(boundary) - 1] = '\0';
    TEST_CHECK(!client_window_title_init(boundary));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);

    char oversized[CLIENT_LAUNCH_LABEL_MAX_SIZE + 2];
    memset(oversized, 'b', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    TEST_CHECK(!client_window_title_init(oversized));
    TEST_CHECK(strcmp(client_window_title(), PACKAGE_NAME) == 0);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
