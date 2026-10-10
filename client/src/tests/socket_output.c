/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Exercise the production enqueuers and dequeue accounting without a network
 * connection. Function sections discard unrelated transport/lifecycle paths. */
#define socket_is_quic socket_output_test_is_quic
#include "../client/socket.c"
#undef socket_is_quic

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                      \
        }                                                                 \
    } while (0)

client_socket_t csocket;
server_struct *selected_server;
static unsigned int transport_marker;

bool socket_output_test_is_quic(socket_t *socket) {
    return socket == (socket_t *)&transport_marker;
}

static void drain_output(void) {
    SDL_LockMutex(output_buffer_mutex);
    while (output_queue_start != NULL) {
        command_buffer *buffer = command_buffer_dequeue(&output_queue_start, &output_queue_end);
        command_buffer_free(buffer);
    }
    REQUIRE(output_queue_end == NULL);
    REQUIRE(output_queue_bytes == 0);
    SDL_UnlockMutex(output_buffer_mutex);
}

static void check_access_queue(size_t payload_size) {
    SDL_LockMutex(output_buffer_mutex);
    REQUIRE(output_queue_start != NULL && output_queue_start->len == 3);
    REQUIRE(!output_queue_start->sensitive);
    REQUIRE(output_queue_end != output_queue_start);
    REQUIRE(output_queue_end->len == payload_size && output_queue_end->sensitive);
    REQUIRE(output_queue_bytes == 3 + payload_size);
    SDL_UnlockMutex(output_buffer_mutex);
}

static packet_struct *ordinary_packet(void) {
    packet_struct *packet = packet_new(SERVER_CMD_KEEPALIVE, 1, 0);
    packet_writer_write_uint8(packet, 1);
    return packet;
}

static void check_followup_sends(void) {
    socket_send_packet(ordinary_packet());
    REQUIRE(output_queue_bytes == 4);
    /* A full bounded queue must reject another packet without changing bytes. */
    REQUIRE(!socket_send_packet_bounded(ordinary_packet(), 4));
    REQUIRE(output_queue_bytes == 4);
    drain_output();
    REQUIRE(socket_send_packet_bounded(ordinary_packet(), 4));
    REQUIRE(output_queue_bytes == 4);
    drain_output();
}

int main(void) {
    static const char code[] = "0123456789ABCDEF";
    static const char admin[] = "{\"operation\":\"list\"}";
    toolkit_import(packet);
    socket_mutex = SDL_CreateMutex();
    output_buffer_mutex = SDL_CreateMutex();
    REQUIRE(socket_mutex != NULL && output_buffer_mutex != NULL);
    csocket.sc = (socket_t *)&transport_marker;

    REQUIRE(client_socket_send_access_auth(code));
    check_access_queue(ACCESS_CODE_LENGTH + 1U);
    drain_output();
    check_followup_sends();

    REQUIRE(client_socket_send_access_admin(admin, sizeof(admin) - 1));
    check_access_queue(sizeof(admin));
    drain_output();
    check_followup_sends();

    /* Both specialized producers can coexist with an ordinary queued command. */
    REQUIRE(client_socket_send_access_auth(code));
    REQUIRE(client_socket_send_access_admin(admin, sizeof(admin) - 1));
    socket_send_packet(ordinary_packet());
    REQUIRE(output_queue_bytes == 3 + ACCESS_CODE_LENGTH + 1U + 3 + sizeof(admin) + 4);
    drain_output();
    check_followup_sends();

    csocket.sc = NULL;
    SDL_DestroyMutex(output_buffer_mutex);
    SDL_DestroyMutex(socket_mutex);
    toolkit_deinit();
    puts("socket output accounting tests passed");
    return 0;
}
