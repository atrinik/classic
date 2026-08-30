/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Client interface main routine.
 *
 * This file sets up a few global variables, connects to the server,
 * tells it what kind of pictures it wants, adds the client and enters
 * the main event loop (event_loop()) checks the game socket for input and
 * then polls for x events. This should be fixed since you can just block
 * on both filedescriptors.
 *
 * The DoClient function receives a message (an ArgList), unpacks it, and
 * in a slow for loop dispatches the command to the right function
 * through the commands table. ArgLists are essentially like RPC things,
 * only they don't require going through RPCgen, and it's easy to get
 * variable length lists. They are just lists of longs, strings,
 * characters, and byte arrays that can be converted to a machine
 * independent format.
 */

#include <global.h>
#include <client_command_queue.h>
#include <client_socket.h>
#include <notification.h>
#include <resources.h>
#include <toolkit/datetime.h>
#include <toolkit/packet.h>

/** Client player structure with things like stats, damage, etc */
Client_Player cpl;

/** Structure of all the socket commands */
static socket_command_struct commands[CLIENT_CMD_NROF] = {
#define ATRINIK_CLIENT_COMMAND_HANDLER(_symbol, _handler) \
    [CLIENT_CMD_##_symbol] = {.handle_func = (_handler), .name = CLIENT_CMD_NAME_##_symbol},
#include "command_handlers.def"
#undef ATRINIK_CLIENT_COMMAND_HANDLER
};
CASSERT_ARRAY(commands, CLIENT_CMD_NROF);

static const uint8_t *current_command_data;
static size_t current_command_len;
static command_buffer *deferred_command;

bool client_command_retry_current(void) {
    if (current_command_data == NULL || current_command_len == 0 || deferred_command != NULL) {
        return false;
    }
    deferred_command = command_buffer_new(current_command_len, (uint8_t *)current_command_data);
    return true;
}

void client_command_retry_clear(void) {
    command_buffer_free(deferred_command);
    deferred_command = NULL;
}

/** Dispatch one complete server command envelope through the production table. */
static bool client_command_dispatch(uint8_t *data, size_t len, void *user_data) {
    (void)user_data;

    HARD_ASSERT(current_command_data == NULL);
    current_command_data = data;
    current_command_len = len;

    size_t pos = 0;
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type = packet_reader_read_uint8(&reader);

    if (packet_reader_error(&reader) != PACKET_ERROR_NONE) {
        LOG(ERROR, "Rejected command envelope: %s", packet_error_string(reader.error));
    } else if (type >= CLIENT_CMD_NROF || commands[type].handle_func == NULL) {
        LOG(ERROR, "Bad command from server (%d)", type);
    } else {
        packet_reader_scope_t scope;
        packet_reader_scope_begin(&scope);
        packet_reader_init_at(&reader, data, len, pos);
        commands[type].handle_func(data, len, pos);
        if (deferred_command != NULL) {
            /* The exact envelope is retained for replay, so this dispatch
             * intentionally owns the unread suffix without classifying it as
             * malformed input. */
            (void)packet_reader_skip(&reader, packet_reader_remaining(&reader));
        }
        packet_error_t error = packet_reader_scope_finish(&scope);
        if (error != PACKET_ERROR_NONE) {
            LOG(ERROR,
                "Rejected malformed %s command: %s",
                commands[type].name,
                packet_error_string(error));
        }
    }

    current_command_data = NULL;
    current_command_len = 0;
    return deferred_command == NULL;
}

#ifdef ATRINIK_WIDGET_TESTS
bool client_command_dispatch_test(uint8_t *data, size_t len) {
    return client_command_dispatch(data, len, NULL);
}

bool client_command_retry_test_pending(void) {
    return deferred_command != NULL;
}
#endif

bool client_command_retry_deferred(void) {
    if (deferred_command == NULL) {
        return true;
    }

    command_buffer *command = deferred_command;
    deferred_command = NULL;
    bool complete = client_command_dispatch(command->data, command->len, NULL);
    command_buffer_free(command);
    return complete && deferred_command == NULL;
}

void client_commands_drain_with_clock(uint64_t budget_us,
                                      client_command_queue_clock_func clock_func,
                                      void *clock_data,
                                      client_command_queue_drain_result_t *result) {
    client_command_queue_drain(budget_us,
                               clock_func,
                               clock_data,
                               client_command_dispatch,
                               NULL,
                               result);
}

static uint64_t client_command_clock(void *user_data) {
    (void)user_data;
    return datetime_monotonic_us();
}

/**
 * Do client. The main loop for commands. From this, the data and
 * commands from server are received.
 */
void DoClient(void) {
    /* A sustained stream of multi-level map updates must not starve the
     * main loop's render/present phases. Packet order is retained; the
     * remaining queue resumes on the next frame. Always finish at least
     * the command already dequeued, even when it exceeds this budget. */
    client_commands_drain_with_clock(CLIENT_COMMAND_QUEUE_BUDGET_US,
                                     client_command_clock,
                                     NULL,
                                     NULL);
}

/**
 * Check animation status.
 * @param anum
 * Animation ID.
 */
bool check_animation_status(int anum) {
    if (anum < 0 || (size_t)anum >= animations_num || animations == NULL || anim_table == NULL) {
        LOG(ERROR,
            "Ignoring invalid animation ID %d (count: %" PRIu64 ")",
            anum,
            (uint64_t)animations_num);
        return false;
    }

    if (animations[anum].loaded) {
        return true;
    }

    if (anim_table[anum].anim_cmd == NULL || anim_table[anum].len < 4) {
        LOG(ERROR, "Animation %d has no valid command data", anum);
        return false;
    }

    /* Same as server sends it. */
    socket_command_anim(anim_table[anum].anim_cmd, anim_table[anum].len, 0);

    return animations[anum].loaded;
}
