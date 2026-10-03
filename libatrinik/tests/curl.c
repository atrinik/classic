#include <toolkit/curl.h>
#include <toolkit/logger.h>
#include <toolkit/path.h>

#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct http_fixture {
    int listener;
    const char *response;
    useconds_t stall_us;
    bool accepted;
    bool stall_until_close;
    bool peer_closed;
    atomic_bool received;
} http_fixture_t;
static char captured_log[HUGE_BUF];

static void capture_curl_log(const char *message) {
    if (strstr(message, "HTTP request origin=") != NULL) {
        snprintf(captured_log, sizeof(captured_log), "%s", message);
    }
}

static void *http_fixture_run(void *user_data) {
    http_fixture_t *fixture = user_data;
    struct pollfd listener = {.fd = fixture->listener, .events = POLLIN};
    if (poll(&listener, 1, 3000) <= 0) {
        return NULL;
    }
    int client = accept(fixture->listener, NULL, NULL);
    if (client < 0) {
        return NULL;
    }
    fixture->accepted = true;

    char request[1024];
    struct pollfd connection = {.fd = client, .events = POLLIN};
    if (poll(&connection, 1, 3000) <= 0 || recv(client, request, sizeof(request), 0) <= 0) {
        close(client);
        return NULL;
    }
    atomic_store(&fixture->received, true);
    if (fixture->stall_until_close) {
        /* Drain any remaining POST bytes, then wait for cancellation's EOF.
         * Bound the fixture independently of both libcurl and CTest. */
        for (unsigned i = 0; i < 100; i++) {
            int ready = poll(&connection, 1, 100);
            if (ready < 0) {
                break;
            }
            if (ready > 0 && recv(client, request, sizeof(request), 0) <= 0) {
                fixture->peer_closed = true;
                break;
            }
        }
        close(client);
        return NULL;
    }
    if (fixture->stall_us != 0) {
        usleep(fixture->stall_us);
    }
    if (fixture->response != NULL) {
        (void)send(client, fixture->response, strlen(fixture->response), MSG_NOSIGNAL);
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

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static bool wait_for_flag(atomic_bool *flag, unsigned timeout_ms) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (!atomic_load(flag)) {
        if (monotonic_ms() >= deadline) {
            return false;
        }
        usleep(1000);
    }
    return true;
}

typedef struct completion_fixture {
    atomic_bool entered;
    atomic_bool release;
    atomic_bool finished;
    bool block;
    bool valid;
} completion_fixture_t;

static void record_completion(curl_request_t *request, void *user_data) {
    completion_fixture_t *completion = user_data;
    if (completion->block) {
        /* Misuse must be rejected even in NDEBUG builds; the owner still
         * joins below and the callback can continue using its request. */
        curl_request_free(request);
    }
    atomic_store(&completion->entered, true);
    bool released = !completion->block || wait_for_flag(&completion->release, 3000);
    size_t size = 0;
    char *body = curl_request_get_body(request, &size);
    /* Read the request after any blocking interval to exercise join lifetime. */
    completion->valid = released && curl_request_get_state(request) == CURL_STATE_OK &&
                        curl_request_get_http_code(request) == 200 && body != NULL && size == 7 &&
                        memcmp(body, "success", 7) == 0;
    atomic_store(&completion->finished, true);
}

typedef struct free_fixture {
    curl_request_t *request;
    atomic_bool started;
    atomic_bool finished;
} free_fixture_t;

static void *free_request(void *user_data) {
    free_fixture_t *fixture = user_data;
    atomic_store(&fixture->started, true);
    curl_request_free(fixture->request);
    atomic_store(&fixture->finished, true);
    return NULL;
}

static int test_async_request(bool post, bool cancel, bool block_callback) {
    static const char response[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 7\r\n"
                                   "Connection: close\r\n\r\n"
                                   "success";
    http_fixture_t fixture = {.listener = -1, .response = response, .stall_until_close = cancel};
    char url[128];
    if (http_fixture_start(&fixture, url, sizeof(url)) != 0) {
        return 1;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, http_fixture_run, &fixture) != 0) {
        close(fixture.listener);
        return 1;
    }
    completion_fixture_t completion = {.block = block_callback};
    curl_request_t *request = curl_request_create(url, CURL_PKEY_TRUST_SYSTEM);
    curl_request_set_timeout(request, 8000);
    curl_request_set_cb(request, record_completion, &completion);
    if (post) {
        (void)curl_request_set_post_body(request, "body", 4);
        curl_request_start_post(request);
    } else {
        curl_request_start_get(request);
    }

    bool valid = wait_for_flag(&fixture.received, 3000);
    if (cancel) {
        uint64_t started = monotonic_ms();
        curl_request_free(request);
        uint64_t elapsed = monotonic_ms() - started;
        valid = valid && elapsed < 2500 && !atomic_load(&completion.entered);
    } else if (block_callback) {
        valid = valid && wait_for_flag(&completion.entered, 3000);
        free_fixture_t freeing = {.request = request};
        pthread_t free_thread;
        if (pthread_create(&free_thread, NULL, free_request, &freeing) == 0) {
            valid = valid && wait_for_flag(&freeing.started, 3000);
            /* Free must join an already admitted callback, even after state OK. */
            bool returned_early = wait_for_flag(&freeing.finished, 100);
            atomic_store(&completion.release, true);
            pthread_join(free_thread, NULL);
            valid = valid && !returned_early && atomic_load(&freeing.finished);
        } else {
            atomic_store(&completion.release, true);
            curl_request_free(request);
            valid = false;
        }
        valid = valid && completion.valid && atomic_load(&completion.finished);
    } else {
        valid = valid && wait_for_flag(&completion.finished, 3000);
        curl_request_free(request);
        valid = valid && completion.valid;
    }
    pthread_join(thread, NULL);
    close(fixture.listener);
    valid = valid && fixture.accepted && (!cancel || fixture.peer_closed);
    if (!valid) {
        fprintf(stderr,
                "async HTTP fixture failed: post=%d cancel=%d callback=%d\n",
                post,
                cancel,
                block_callback);
    }
    return !valid;
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

    static const char not_modified_response[] = "HTTP/1.1 304 Not Modified\r\n"
                                                "Content-Length: 0\r\n"
                                                "Connection: close\r\n"
                                                "\r\n";
    http_fixture_t cache_fixture = {.listener = -1, .response = not_modified_response};
    char cache_url[128];
    if (http_fixture_start(&cache_fixture, cache_url, sizeof(cache_url)) != 0) {
        return 1;
    }
    pthread_t cache_thread;
    if (pthread_create(&cache_thread, NULL, http_fixture_run, &cache_fixture) != 0) {
        close(cache_fixture.listener);
        return 1;
    }
    request = curl_request_create_with_origin(cache_url, CURL_PKEY_TRUST_SYSTEM, "cache-test");
    curl_request_do_get(request);
    curl_state_t cache_state = curl_request_get_state(request);
    int cache_http_code = curl_request_get_http_code(request);
    curl_request_free(request);
    pthread_join(cache_thread, NULL);
    close(cache_fixture.listener);
    bool cache_without_path =
        cache_fixture.accepted && cache_state == CURL_STATE_ERROR && cache_http_code == 304;

    captured_log[0] = '\0';
    request = curl_request_create_with_origin("https://127.0.0.1:9",
                                              CURL_PKEY_TRUST_SYSTEM,
                                              "client.asset");
    curl_request_do_get(request);
    bool https_loopback =
        strstr(captured_log, "HTTP request origin=client.asset endpoint=https-loopback") != NULL;
    curl_request_free(request);

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
           strstr(loopback_log, url) != NULL || !other_endpoint || !cache_without_path ||
           !https_loopback;
}

static int test_endpoint_alias_diagnostics(void) {
    static const char *const urls[] = {
        "http://localhost:9/",
        "http://[::1]:9/",
    };
    bool all_logged = true;
    logger_set_print_func(capture_curl_log);
    for (size_t i = 0; i < arraysize(urls); i++) {
        captured_log[0] = '\0';
        curl_request_t *request =
            curl_request_create_with_origin(urls[i], CURL_PKEY_TRUST_SYSTEM, "client.asset");
        curl_request_set_timeout(request, 50);
        curl_request_do_get(request);
        all_logged =
            all_logged &&
            strstr(captured_log, "HTTP request origin=client.asset endpoint=http-loopback") != NULL;
        curl_request_free(request);
    }
    logger_set_print_func(logger_do_print);
    return !all_logged;
}

int main(void) {
    toolkit_import(path);
    toolkit_import(curl);
    int failed = test_response_code_survives_body_limit() ||
                 test_response_code_survives_partial_body() || test_total_timeout() ||
                 test_validated_cache_commit() || test_bounded_request_diagnostic() ||
                 test_endpoint_alias_diagnostics();
    for (unsigned post = 0; post < 2; post++) {
        failed |= test_async_request(post != 0, false, false);
        failed |= test_async_request(post != 0, true, false);
        failed |= test_async_request(post != 0, false, true);
    }
    toolkit_deinit();
    return failed;
}
