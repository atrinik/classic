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
 * Various defines.
 */

#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <client_command_queue.h>
#include <connection_failure.h>
#include <metaserver_options.h>
#include <stun_config.h>
#include <toolkit/socket.h>

typedef struct Player_Struct Client_Player;

/* How many skill types server supports/client will get sent to it.
 * If more skills are added to server, this needs to get increased. */
#define MAX_SKILL 6

#define INPUT_MODE_NO 0
#define INPUT_MODE_CONSOLE 1
#define INPUT_MODE_NUMBER 2

#define NUM_MODE_GET 1
#define NUM_MODE_DROP 2

typedef struct Animations {
    /* 0 = all fields are invalid, 1 = anim is loaded */
    int loaded;

    /* Length of one a animation frame (num_anim / facings) */
    size_t frame;
    uint16_t *faces;

    /* Number of frames */
    uint8_t facings;

    /* Number of animations. Value of 2 means
     * only faces[0], [1] have meaningful values. */
    size_t num_animations;
    uint8_t flags;
} Animations;

typedef struct _anim_table {
    /* Length of anim_cmd data */
    size_t len;

    /* Faked animation command */
    uint8_t *anim_cmd;
} _anim_table;

/**
 * One command buffer.
 */
typedef struct command_buffer {
    /** Next command in queue. */
    struct command_buffer *next;

    /** Previous command in queue. */
    struct command_buffer *prev;

    /** Monotonic arrival timestamp, in microseconds. */
    uint64_t enqueued_us;

    /** Length of the data. */
    size_t len;

    /** The data. */
    uint8_t data[1];
} command_buffer;

/* ClientSocket could probably hold more of the global values - it could
 * probably hold most all socket/communication related values instead
 * of globals. */
typedef struct client_socket {
    socket_t *sc;
    /** Bounded, non-sensitive reason for the most recent failed open. */
    socket_connect_failure_t failure;
} client_socket_t;

/** Copies information from one color structure into another. */
#define SDL_color_copy(_color, _color2) \
    {                                   \
        (_color)->r = (_color2)->r;     \
        (_color)->g = (_color2)->g;     \
        (_color)->b = (_color2)->b;     \
    }

typedef struct socket_command_struct {
    void (*handle_func)(uint8_t *data, size_t len, size_t pos);
    const char *name;
} socket_command_struct;

/**
 * @defgroup SPELL_DESC_xxx Spell flags
 * Spell flags.
 *@{*/
/** Spell is safe to cast in town. */
#define SPELL_DESC_TOWN 0x01
/** Spell is fired in a direction (bullet, bolt, ...). */
#define SPELL_DESC_DIRECTION 0x02
/** Spell can be cast on self. */
#define SPELL_DESC_SELF 0x04
/** Spell can be cast on friendly creature. */
#define SPELL_DESC_FRIENDLY 0x08
/** Spell can be cast on enemy creature. */
#define SPELL_DESC_ENEMY 0x10

/*@}*/

typedef struct clioption_settings_struct {
    char **servers;

    size_t servers_num;

    client_metaserver_options_t metaservers;

    char *connect[4];

    char *game_news_url;

    char *join_password;

    /** Path to a protected rendezvous invite file; never the capability. */
    char *rendezvous_invite_file;

    /** Direct-rendezvous STUN endpoint and its configured provenance. */
    client_stun_config_t stun;

    uint8_t reconnect;
} clioption_settings_struct;

/** Public API implemented in src/client/client.c. */

extern Client_Player cpl;

extern void DoClient(void);

/** Retain the command currently being dispatched for one post-recovery replay. */
extern bool client_command_retry_current(void);

/** Replay a command retained after a recoverable UI resource failure. */
extern bool client_command_retry_deferred(void);

/** Discard any command retained for recovery, such as during disconnect. */
extern void client_command_retry_clear(void);

#ifdef ATRINIK_WIDGET_TESTS
/** Dispatch one complete envelope through the production table. */
extern bool client_command_dispatch_test(uint8_t *data, size_t len);

/** Whether an exact command envelope is retained for post-recovery replay. */
extern bool client_command_retry_test_pending(void);

/** Exercise the production recovery republish callback. */
extern bool gpu_renderer_recovery_republish_test(void);
#endif

/** Drain inbound envelopes through the production dispatcher using an injected clock. */
extern void client_commands_drain_with_clock(uint64_t budget_us,
                                             client_command_queue_clock_func clock_func,
                                             void *clock_data,
                                             client_command_queue_drain_result_t *result);

extern bool check_animation_status(int anum);

#endif
