#include <toolkit/curl.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct http_fixture {
    int listener;
    const char *response;
    useconds_t stall_us;
    bool accepted;
} http_fixture_t;

static void *http_fixture_run(void *user_data) {
    http_fixture_t *fixture = user_data;
    int client = accept(fixture->listener, NULL, NULL);
    if (client < 0) {
        return NULL;
    }
    fixture->accepted = true;

    char request[1024];
    (void)recv(client, request, sizeof(request), 0);
    if (fixture->stall_us != 0) {
        usleep(fixture->stall_us);
    }
    if (fixture->response != NULL) {
        (void)send(client, fixture->response, strlen(fixture->response), 0);
    }
    close(client);
    return NULL;
}

static int http_fixture_start(http_fixture_t *fixture, char *url, size_t url_size) {
    fixture->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (fixture->listener < 0) {
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    if (bind(fixture->listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fixture->listener, 1) != 0) {
        close(fixture->listener);
        return -1;
    }

    socklen_t address_size = sizeof(address);
    if (getsockname(fixture->listener, (struct sockaddr *)&address, &address_size) != 0) {
        close(fixture->listener);
        return -1;
    }
    int written = snprintf(url, url_size, "http://127.0.0.1:%u/", ntohs(address.sin_port));
    return written > 0 && (size_t)written < url_size ? 0 : -1;
}

static int test_response_code_survives_body_limit(void) {
    static const char response[] = "HTTP/1.1 401 Unauthorized\r\n"
                                   "Content-Length: 32\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "0123456789abcdefghijklmnopqrstuv";
    http_fixture_t fixture = {.listener = -1, .response = response};
    char url[128];
    if (http_fixture_start(&fixture, url, sizeof(url)) != 0) {
        return 1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, http_fixture_run, &fixture) != 0) {
        close(fixture.listener);
        return 1;
    }

    curl_request_t *request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_max_body(request, 4);
    curl_request_do_get(request);
    curl_state_t state = curl_request_get_state(request);
    int http_code = curl_request_get_http_code(request);
    curl_request_free(request);
    pthread_join(thread, NULL);
    close(fixture.listener);
    return !fixture.accepted || state != CURL_STATE_ERROR || http_code != 401;
}

static int test_response_code_survives_partial_body(void) {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 64\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "partial";
    http_fixture_t fixture = {.listener = -1, .response = response};
    char url[128];
    if (http_fixture_start(&fixture, url, sizeof(url)) != 0) {
        return 1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, http_fixture_run, &fixture) != 0) {
        close(fixture.listener);
        return 1;
    }

    curl_request_t *request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_do_get(request);
    curl_state_t state = curl_request_get_state(request);
    int http_code = curl_request_get_http_code(request);
    curl_request_free(request);
    pthread_join(thread, NULL);
    close(fixture.listener);
    return !fixture.accepted || state != CURL_STATE_ERROR || http_code != 200;
}

static int test_total_timeout(void) {
    http_fixture_t fixture = {.listener = -1, .stall_us = 250000};
    char url[128];
    if (http_fixture_start(&fixture, url, sizeof(url)) != 0) {
        return 1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, http_fixture_run, &fixture) != 0) {
        close(fixture.listener);
        return 1;
    }

    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    curl_request_t *request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_timeout(request, 50);
    curl_request_do_get(request);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    uint64_t elapsed_ms = (uint64_t)(finished.tv_sec - started.tv_sec) * 1000U;
    if (finished.tv_nsec >= started.tv_nsec) {
        elapsed_ms += (uint64_t)(finished.tv_nsec - started.tv_nsec) / 1000000U;
    } else {
        elapsed_ms -= 1000U;
        elapsed_ms += (uint64_t)(1000000000L + finished.tv_nsec - started.tv_nsec) / 1000000U;
    }

    curl_state_t state = curl_request_get_state(request);
    curl_request_free(request);
    pthread_join(thread, NULL);
    close(fixture.listener);
    return !fixture.accepted || state != CURL_STATE_ERROR || elapsed_ms < 25U || elapsed_ms > 1000U;
}

int main(void) {
    toolkit_import(curl);
    int failed = test_response_code_survives_body_limit() ||
                 test_response_code_survives_partial_body() || test_total_timeout();
    toolkit_deinit();
    return failed;
}
