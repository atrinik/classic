/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
 * Defines various structures and values that are used for the new client
 * server communication method.
 */

#ifndef NEWSERVER_H
#define NEWSERVER_H

#include "map.h"
#include "server_clock.h"

/** Maximum time a connection may remain in the handshake/setup phase. */
#define SOCKET_SETUP_TIMEOUT 30

/** Maximum inactivity while entering credentials or selecting a character. */
#define SOCKET_LOGIN_IDLE_TIMEOUT (10 * 60)

/** Maximum buffered player-command bytes retained by one connection. */
#define SOCKET_COMMAND_QUEUE_MAX (1024U * 1024U)

/** Physical queue storage including canceled movement records awaiting compaction. */
#define SOCKET_COMMAND_QUEUE_STORAGE_MAX (SOCKET_COMMAND_QUEUE_MAX * 2U)

/** Amortize queue compaction against a meaningful amount of canceled input. */
#define SOCKET_COMMAND_QUEUE_COMPACT_MIN (64U * 1024U)

typedef struct asset_stream_state asset_stream_state_t;

/** How many items to show in the below window. Used in esrv_draw_look(). */
#define NUM_LOOK_OBJECTS 15

/**
 * One map cell. Used to hold 'cache' of faces we already sent
 * to the client.
 */
typedef struct MapCell_struct {
    /** Cache of last sent ambient sound. */
    tag_t sound_ambient_count;

    /* Everything below will be cleared by memset() in when the map
     * cell is no longer visible. */

    uint8_t cleared;

    /** Last normalized light levels sent to the client. */
    uint8_t light_level[NUM_SUB_LAYERS];

    /** Whether each normalized light level has been sent at least once. */
    uint8_t light_known[NUM_SUB_LAYERS];

    /** Last resolved RGB light samples sent to the client. */
    uint8_t light_rgb[NUM_SUB_LAYERS][3];

    /** Whether each resolved RGB sample is established in the client cache. */
    uint8_t light_rgb_known[NUM_SUB_LAYERS];

    /** Last base-map structural support height sent to the client. */
    int16_t support_height;

    /** Whether the structural support height has been sent at least once. */
    uint8_t support_height_known;

    /** Last explicit fog-of-war state sent to the client. */
    uint8_t fow;

    /** Whether the fog-of-war state has been sent at least once. */
    uint8_t fow_known;

    /** Faces we sent. */
    int16_t faces[NUM_REAL_LAYERS];

    /** Multi-arch cache. */
    uint8_t quick_pos[NUM_REAL_LAYERS];

    /** Flags cache. */
    uint8_t flags[NUM_REAL_LAYERS];

    /** Whether a wall-layer object is a roof/camera surface. */
    uint8_t roof[NUM_REAL_LAYERS];

    /** Whether each sent object is a door. */
    uint8_t door[NUM_REAL_LAYERS];

    /** Whether each sent object is a visible exit. */
    uint8_t exit[NUM_REAL_LAYERS];

    uint8_t anim_speed[NUM_REAL_LAYERS];

    uint8_t anim_facing[NUM_REAL_LAYERS];

    uint8_t anim_flags[NUM_SUB_LAYERS];

    /**
     * Probe cache. No need for an array, since this only appears
     * for players or monsters, both on layer 6.
     */
    uint8_t probe;

    /**
     * Possible target object UID cache.
     */
    tag_t target_object_count;

    uint8_t ext_flags; ///< Last ext flags.

    uint8_t client_flags[NUM_SUB_LAYERS];

    uint8_t anim_num; ///< Last number of animations sent.

    uint8_t is_friend : NUM_SUB_LAYERS; ///< Friendly state cache.
} MapCell;

/** One map for a player. */
struct Map {
    /** Lazily allocated cells for each linked-map depth. */
    struct MapCell_struct *levels[MAP2_LEVELS];
};

MapCell *map_client_cache_cell(struct Map *cache, int depth, int x, int y, bool create);
void map_client_cache_clear(struct Map *cache);
void map_client_cache_free(struct Map *cache);

/** Possible socket statuses. */
enum {
    ST_AVAILABLE,
    ST_WAITING,
    ST_LOGIN,
    ST_PLAYING,
    ST_DEAD,
    ST_ZOMBIE
};

/** One queued command belonging to the client's current keyboard movement epoch. */
typedef struct socket_movement_queue_entry {
    uint64_t offset;
} socket_movement_queue_entry;

/** Queue index for one replaceable movement command type. */
typedef struct socket_movement_queue_index {
    socket_movement_queue_entry *entries;
    size_t entries_start;
    size_t entries_num;
    size_t entries_size;
} socket_movement_queue_index;

/** This contains basic information on the socket structure. */
typedef struct socket_struct {
    /** The real socket. */
    socket_t *sc;

    /**
     * If someone is too long idle in the login, he will get
     * disconnected.
     */
    int login_count;

    /** Monotonic deadline for the current login phase. Never persisted or sent. */
    server_monotonic_t login_deadline;

    /** X size of the map the client wants. */
    int mapx;

    /** Y size of the map the client wants. */
    int mapy;

    /** X size of the map the client wants / 2. */
    int mapx_2;

    /** Y size of the map the client wants / 2. */
    int mapy_2;

    /** Version of the client. */
    uint32_t socket_version;

    /** Marker to see we must update the below windows of the tile the player
     * is. */
    uint32_t update_tile;

    /** Marker to map draw/draw below. */
    uint32_t below_clear : 1;

    /** When set, a "connect" was initialized as "player". */
    uint32_t addme : 1;

    /** Does the client want sound? */
    uint32_t sound : 1;

    /** Whether the configured server join password was accepted. */
    bool join_authenticated;

    /** Whether the initial client setup exchange completed successfully. */
    bool setup_completed;

    /** Abuse-rate window and paced in-band asset response token bucket. */
    uint64_t asset_request_window_ms;
    unsigned int asset_window_requests;
    uint64_t asset_token_updated_ms;
    size_t asset_tokens;

    /** Active explicit asset streams, serviced round-robin after gameplay. */
    asset_stream_state_t *asset_streams;
    size_t asset_stream_count;
    /** Asset transport capabilities advertised during client setup. */
    uint8_t asset_transport_capabilities;
    /** Next typed asset stream kind preferred by the fair accept loop. */
    bool asset_accept_batch_next;

    /** Intrusive links for the server-wide fair asset scheduler. */
    struct socket_struct *asset_service_next;
    struct socket_struct *asset_service_prev;
    bool asset_service_registered;
    uint64_t asset_service_generation;
    /** Productive fair-quantum visits in the current scheduler tick. */
    unsigned int asset_service_productive_passes;

    /** Transport route selected for this connection. */
    socket_connection_mode_t connection_mode;

    /** Start of drawing of look window. */
    uint32_t look_position;

    /** Faceset the client is using, default 0. */
    uint8_t faceset;

    /**
     * How many times the player has failed to provide the right
     * password.
     */
    uint8_t password_fails;

    /** Send ext title to client. */
    uint8_t ext_title_flag;

    /** Current state of the socket. */
    int state;

    /** Last map. */
    struct Map lastmap;

    /** Player level used to calculate cached non-player name colors. */
    uint8_t lastmap_player_level;

    /** Whether ::lastmap_player_level has been initialized. */
    bool lastmap_player_level_known;

    /** Outgoing packets. */
    struct packet_struct *packets;

    /** Current and peak bytes held by the bounded outbound queue. */
    size_t packet_queue_bytes;
    size_t packet_queue_peak_bytes;
    size_t packet_queue_count;
    uint64_t packet_queue_rejected;

    /**
     * Buffer for how many ticks have passed since the last keep alive
     * command. When this reaches @ref SOCKET_KEEPALIVE_TIMEOUT, the
     * socket is disconnected.
     */
    uint32_t keepalive;

    char *account;

    struct packet_struct *packet_recv;
    struct packet_struct *packet_recv_cmd;
    uint64_t packet_recv_cmd_base;
    uint32_t movement_stream_epoch;
    socket_movement_queue_index movement_stream_move;
    socket_movement_queue_index movement_stream_fire;
    size_t movement_stream_tombstone_bytes;
} socket_struct;

/**
 * How many seconds must pass since the last keep alive command for the
 * socket to be disconnected.
 */
#define SOCKET_KEEPALIVE_TIMEOUT (uint32_t)(SOCKET_LOGIN_IDLE_TIMEOUT * MAX_TICKS_MULTIPLIER)

/** Holds some system related information. */
typedef struct Socket_Info_struct {
    /** Timeout for select. */
    struct timeval timeout;

    /** Max filedescriptor on the system. */
    int max_filedescriptor;

    /** Number of connections. */
    int nconns;

    /** Number of allocated in init_sockets. */
    int allocated_sockets;
} Socket_Info;

/**
 * A single file loaded from the updates directory that the client can
 * request.
 */
typedef struct update_file_struct {
    /** File's CRC32. */
    unsigned long checksum;

    /** Length of the file. */
    size_t len;

    /** Uncompressed length of the file. */
    size_t ucomp_len;

    /** Name of the file. */
    char *filename;

    /** Compressed contents of the file. */
    uint8_t *contents;

    /** Packet to use for sending the file. */
    struct packet_struct *packet;
} update_file_struct;

/** Filename used to store information about the updated files. */
#define UPDATES_FILE_NAME "updates"
/**
 * Directory to recursively traverse, looking for files that the client
 * can request for an update.
 */
#define UPDATES_DIR_NAME "updates"

/**
 * Maximum password failures allowed before the server kills the
 * socket.
 */
#define MAX_PASSWORD_FAILURES 3

#endif
