#include <toolkit/curl.h>
#include <toolkit/logger.h>
#include <toolkit/path.h>

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
static char captured_log[HUGE_BUF];

static void capture_curl_log(const char *message) {
    if (strstr(message, "HTTP request origin=") != NULL) {
        snprintf(captured_log, sizeof(captured_log), "%s", message);
    }
}

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

static int test_validated_cache_commit(void) {
    char directory[] = "/tmp/atrinik-curl-cache-XXXXXX";
    if (mkdtemp(directory) == NULL) {
        return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/directory.xml", directory);
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/xml; charset=utf-8\r\n"
                                   "ETag: \"directory-1\"\r\n"
                                   "Content-Length: 9\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "validated";
    http_fixture_t fixture = {.listener = -1, .response = response};
    char url[128];
    if (http_fixture_start(&fixture, url, sizeof(url)) != 0) {
        rmdir(directory);
        return 1;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, http_fixture_run, &fixture) != 0) {
        close(fixture.listener);
        rmdir(directory);
        return 1;
    }
    curl_request_t *request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_path(request, path);
    curl_request_do_get(request);
    bool absent_before_commit = access(path, F_OK) != 0;
    bool committed = curl_request_cache_commit(request);
    curl_request_free(request);
    pthread_join(thread, NULL);
    close(fixture.listener);

    char *body = NULL;
    size_t body_size = 0;
    bool loaded = curl_cache_read(path, 32, &body, &body_size);
    bool body_valid = loaded && body_size == 9 && memcmp(body, "validated", 9) == 0;
    free(body);

    static const char not_modified[] = "HTTP/1.1 304 Not Modified\r\n"
                                       "ETag: \"directory-1\"\r\n"
                                       "Connection: close\r\n"
                                       "\r\n";
    http_fixture_t cached_fixture = {.listener = -1, .response = not_modified};
    bool not_modified_valid = http_fixture_start(&cached_fixture, url, sizeof(url)) == 0;
    pthread_t cached_thread;
    if (not_modified_valid) {
        not_modified_valid =
            pthread_create(&cached_thread, NULL, http_fixture_run, &cached_fixture) == 0;
    }
    if (not_modified_valid) {
        request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
        curl_request_set_path(request, path);
        curl_request_do_get(request);
        body = curl_request_get_body(request, &body_size);
        not_modified_valid = curl_request_get_state(request) == CURL_STATE_OK &&
                             curl_request_get_http_code(request) == 304 && body != NULL &&
                             body_size == 9 && memcmp(body, "validated", 9) == 0;
        curl_request_free(request);
        pthread_join(cached_thread, NULL);
        close(cached_fixture.listener);
    } else if (cached_fixture.listener >= 0) {
        close(cached_fixture.listener);
    }

    char etag_path[272];
    snprintf(etag_path, sizeof(etag_path), "%s.etag", path);
    unlink(etag_path);
    unlink(path);
    rmdir(directory);
    return !fixture.accepted || !absent_before_commit || !committed || !body_valid ||
           !cached_fixture.accepted || !not_modified_valid;
}

static int test_bounded_request_diagnostic(void) {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 7\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "bounded";
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

    captured_log[0] = '\0';
    logger_set_print_func(capture_curl_log);
    curl_request_t *request =
        curl_request_create_with_origin(url, CURL_PKEY_TRUST_SYSTEM, "credential/value");
    curl_request_do_get(request);
    curl_state_t state = curl_request_get_state(request);
    int http_code = curl_request_get_http_code(request);
    curl_request_free(request);
    char loopback_log[HUGE_BUF];
    snprintf(loopback_log, sizeof(loopback_log), "%s", captured_log);

    pthread_join(thread, NULL);
    close(fixture.listener);

    request = curl_request_create_with_origin(url, CURL_PKEY_TRUST_SYSTEM, NULL);
    curl_request_free(request);
    request = curl_request_create_with_origin(url, CURL_PKEY_TRUST_SYSTEM, "");
    curl_request_free(request);
    char long_origin[66];
    memset(long_origin, 'x', sizeof(long_origin) - 1);
    long_origin[sizeof(long_origin) - 1] = '\0';
    request = curl_request_create_with_origin(url, CURL_PKEY_TRUST_SYSTEM, long_origin);
    curl_request_free(request);

    captured_log[0] = '\0';
    request = curl_request_create_with_origin("not-a-url", CURL_PKEY_TRUST_SYSTEM, "client.asset");
    curl_request_do_get(request);
    bool other_endpoint =
        strstr(captured_log, "HTTP request origin=client.asset endpoint=other") != NULL;
    curl_request_free(request);
    logger_set_print_func(logger_do_print);

    return !fixture.accepted || state != CURL_STATE_OK || http_code != 200 ||
           strstr(loopback_log, "HTTP request origin=unknown endpoint=http-loopback") == NULL ||
           strstr(loopback_log, url) != NULL || !other_endpoint;
}

int main(void) {
    toolkit_import(path);
    toolkit_import(curl);
    int failed = test_response_code_survives_body_limit() ||
                 test_response_code_survives_partial_body() || test_total_timeout() ||
                 test_validated_cache_commit() || test_bounded_request_diagnostic();
    toolkit_deinit();
    return failed;
}
