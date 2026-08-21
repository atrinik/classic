#include <toolkit/socket.h>

#include <curl/curl.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CHECK(condition)                                                               \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                        \
        }                                                                                   \
    } while (0)

#if LIBCURL_VERSION_NUM >= 0x075600
typedef struct fake_websocket_frame {
    CURLcode result;
    unsigned int flags;
    const unsigned char *payload;
    size_t payload_size;
    bool yield_again_after_chunk;
} fake_websocket_frame_t;

static const fake_websocket_frame_t *fake_frames;
static size_t fake_frame_count;
static size_t fake_frame_index;
static size_t fake_frame_offset;
static bool fake_yielded_again;
static struct curl_ws_frame fake_metadata;

static void fake_websocket_frames_set(const fake_websocket_frame_t *frames, size_t count) {
    fake_frames = frames;
    fake_frame_count = count;
    fake_frame_index = 0;
    fake_frame_offset = 0;
    fake_yielded_again = false;
}

CURLcode __wrap_curl_ws_recv(CURL *curl,
                             void *buffer,
                             size_t buflen,
                             size_t *recv,
                             const struct curl_ws_frame **meta) {
    (void)curl;
    *recv = 0;
    *meta = NULL;
    if (fake_frame_index >= fake_frame_count) {
        return CURLE_AGAIN;
    }

    const fake_websocket_frame_t *frame = &fake_frames[fake_frame_index];
    if (frame->result != CURLE_OK) {
        fake_frame_index++;
        fake_frame_offset = 0;
        return frame->result;
    }

    if (frame->yield_again_after_chunk && fake_frame_offset != 0 && !fake_yielded_again) {
        fake_yielded_again = true;
        return CURLE_AGAIN;
    }
    size_t remaining = frame->payload_size - fake_frame_offset;
    size_t copied = remaining < buflen ? remaining : buflen;
    if (copied != 0) {
        memcpy(buffer, frame->payload + fake_frame_offset, copied);
    }
    fake_frame_offset += copied;
    *recv = copied;
    fake_metadata = (struct curl_ws_frame){0};
    fake_metadata.flags = (int)frame->flags;
    fake_metadata.offset = (curl_off_t)(fake_frame_offset - copied);
    fake_metadata.bytesleft = (curl_off_t)(remaining - copied);
    fake_metadata.len = frame->payload_size;
    *meta = &fake_metadata;
    if (fake_frame_offset == frame->payload_size) {
        fake_frame_index++;
        fake_frame_offset = 0;
        fake_yielded_again = false;
    }
    return CURLE_OK;
}

static void test_control_frames_are_ignored(void) {
    static const unsigned char ping[] = {'p', 'i', 'n', 'g'};
    static const unsigned char pong[] = {'p', 'o', 'n', 'g'};
    static const unsigned char message[] = "hello";
    const fake_websocket_frame_t frames[] = {
        {CURLE_OK, CURLWS_PING, ping, sizeof(ping), false},
        {CURLE_OK, CURLWS_PONG, pong, sizeof(pong), false},
        {CURLE_OK, CURLWS_TEXT, message, sizeof(message) - 1U, false},
    };
    fake_websocket_frames_set(frames, arraysize(frames));

    char buffer[32] = {0};
    size_t used = 0;
    socket_websocket_receive_info_t info;
    TEST_CHECK(socket_websocket_receive_ex((void *)1, VS(buffer), &used, &info) ==
               SOCKET_WEBSOCKET_MESSAGE);
    TEST_CHECK(used == sizeof(message) - 1U);
    TEST_CHECK(strcmp(buffer, "hello") == 0);
    TEST_CHECK(info.frame_present);
    TEST_CHECK(info.frame_flags == CURLWS_TEXT);
}

static void test_fragmented_text_survives_control_frame(void) {
    static const unsigned char first[] = "hel";
    static const unsigned char ping[] = {'x'};
    static const unsigned char last[] = "lo";
    const fake_websocket_frame_t frames[] = {
        {CURLE_OK, CURLWS_TEXT | CURLWS_CONT, first, sizeof(first) - 1U, false},
        {CURLE_OK, CURLWS_PING, ping, sizeof(ping), false},
        {CURLE_OK, CURLWS_TEXT, last, sizeof(last) - 1U, false},
    };
    fake_websocket_frames_set(frames, arraysize(frames));

    char buffer[32] = {0};
    size_t used = 0;
    TEST_CHECK(socket_websocket_receive((void *)1, VS(buffer), &used) ==
               SOCKET_WEBSOCKET_PARTIAL);
    TEST_CHECK(used == sizeof(first) - 1U);
    TEST_CHECK(socket_websocket_receive((void *)1, VS(buffer), &used) ==
               SOCKET_WEBSOCKET_MESSAGE);
    TEST_CHECK(used == sizeof("hello") - 1U);
    TEST_CHECK(strcmp(buffer, "hello") == 0);
}

static void test_partial_control_frame_survives_again(void) {
    static const unsigned char ping[] = "12345678";
    static const unsigned char message[] = "ok";
    const fake_websocket_frame_t frames[] = {
        {CURLE_OK, CURLWS_PING, ping, sizeof(ping) - 1U, true},
        {CURLE_OK, CURLWS_TEXT, message, sizeof(message) - 1U, false},
    };
    fake_websocket_frames_set(frames, arraysize(frames));

    char buffer[8] = {0};
    size_t used = 0;
    TEST_CHECK(socket_websocket_receive((void *)1, VS(buffer), &used) == SOCKET_WEBSOCKET_EMPTY);
    TEST_CHECK(used == 0);
    TEST_CHECK(socket_websocket_receive((void *)1, VS(buffer), &used) ==
               SOCKET_WEBSOCKET_MESSAGE);
    TEST_CHECK(used == sizeof(message) - 1U);
    TEST_CHECK(strcmp(buffer, "ok") == 0);
}

static void test_close_and_transport_diagnostics(void) {
    static const fake_websocket_frame_t binary_frame = {
        CURLE_OK, CURLWS_BINARY, (const unsigned char *)"binary", sizeof("binary") - 1U, false,
    };
    fake_websocket_frames_set(&binary_frame, 1);
    char buffer[16] = {0};
    size_t used = 0;
    socket_websocket_receive_info_t info;
    TEST_CHECK(socket_websocket_receive_ex((void *)1, VS(buffer), &used, &info) ==
               SOCKET_WEBSOCKET_PROTOCOL);
    TEST_CHECK(info.frame_present);
    TEST_CHECK(info.frame_flags == CURLWS_BINARY);
    TEST_CHECK(used == 0);

    static const unsigned char close_payload[] = {0x03, 0xe8};
    const fake_websocket_frame_t close_frame = {
        CURLE_OK, CURLWS_CLOSE, close_payload, sizeof(close_payload), false,
    };
    fake_websocket_frames_set(&close_frame, 1);
    memset(buffer, 0, sizeof(buffer));
    used = 0;
    TEST_CHECK(socket_websocket_receive_ex((void *)1, VS(buffer), &used, &info) ==
               SOCKET_WEBSOCKET_CLOSED);
    TEST_CHECK(info.frame_present);
    TEST_CHECK(info.frame_flags == CURLWS_CLOSE);
    TEST_CHECK(info.has_close_code);
    TEST_CHECK(info.close_code == 1000);

    const fake_websocket_frame_t transport_failure = {
        CURLE_GOT_NOTHING, 0, NULL, 0, false,
    };
    fake_websocket_frames_set(&transport_failure, 1);
    used = 0;
    memset(&info, 0, sizeof(info));
    TEST_CHECK(socket_websocket_receive_ex((void *)1, VS(buffer), &used, &info) ==
               SOCKET_WEBSOCKET_CLOSED);
    TEST_CHECK(info.curl_result == CURLE_GOT_NOTHING);
    TEST_CHECK(!info.frame_present);
}
#endif

int main(void) {
#if LIBCURL_VERSION_NUM >= 0x075600
    test_control_frames_are_ignored();
    test_fragmented_text_survives_control_frame();
    test_partial_control_frame_survives_again();
    test_close_and_transport_diagnostics();
#endif
    return 0;
}
