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
 * Socket server implementation.
 *
 * @author
 * Zoey Rose
 */

#include <global.h>
#include <server_main.h>
#include <initialization.h>
#include <toolkit/path.h>
#include <toolkit/string.h>
#include <toolkit/packet.h>
#include <toolkit/datetime.h>
#include <server.h>
#include <player.h>
#include <object.h>
#include <ban.h>
#include <metaserver_internal.h>
#include <network_metrics.h>

TOOLKIT_API(DEPENDS(socket), IMPORTS(logger));

/**
 * @defgroup SOCKET_COMMAND_xxx Socket command flags
 * Flags for the socket commands.
 *@{*/
/**
 * The command may only be performed by clients that are logged in.
 */
#define SOCKET_COMMAND_PLAYER_ONLY 1
/*@}*/

/** Connection-phase policies enforced before command dispatch. */
typedef enum socket_command_policy {
    SOCKET_COMMAND_POLICY_CONTROL,
    SOCKET_COMMAND_POLICY_ADMITTED,
    SOCKET_COMMAND_POLICY_SETUP,
    SOCKET_COMMAND_POLICY_VERSION,
    SOCKET_COMMAND_POLICY_PLAYING,
    SOCKET_COMMAND_POLICY_LOGIN,
    SOCKET_COMMAND_POLICY_LIVENESS,
} socket_command_policy_t;

/**
 * Maximum number of commands a player is able to issue in a single
 * iteration.
 */
#define SOCKET_SERVER_PLAYER_MAX_COMMANDS 15

/** Maximum connections serviced from each intrusive list per transport wake. */
#define SOCKET_SERVER_CONNECTIONS_PER_WAKEUP 64U

/**
 * Structure to provide link linkage for client socket entries.
 */
typedef struct csocket_entry {
    struct csocket_entry *next; ///< Next entry.
    struct csocket_entry *prev; ///< Previous entry.
    socket_struct *cs; ///< Client's socket.
} csocket_entry_t;

/**
 * Structure that defines a single socket command type.
 */
typedef struct socket_command {
    /**
     * Handler function.
     */
    socket_command_func handle_func;

    /**
     * A combination of ::SOCKET_COMMAND_xxx.
     */
    int flags;

    /** Connection phase in which the command is valid. */
    socket_command_policy_t policy;

    const char *name;
} socket_command_t;

/**
 * File descriptors that have data available.
 */
static fd_set fds_read;
/** Direct UDP/QUIC listeners (IPv4 and IPv6), when enabled. */
static socket_t *quic_server_sockets[2];
static socket_direct_candidate_t quic_candidates[SOCKET_DIRECT_MAX_CANDIDATES];
static size_t quic_candidate_count;
static void socket_server_csocket_drop(csocket_entry_t *entry);
static char quic_public_host[MAX_BUF];
static uint16_t quic_public_port;
static char quic_certificate_sha256[65];
static uint64_t quic_punches_received;
static uint64_t quic_punches_echoed;
static player *socket_server_player_find(socket_struct *cs);
/**
 * List of client sockets that are not yet playing.
 */
static csocket_entry_t *client_sockets;
static size_t client_sockets_count;
/** Round-robin cursor for pending-login transport work. */
static size_t client_service_cursor;
/** Round-robin cursor for logged-in transport work. */
static size_t player_service_cursor;
/** Avoid retrying a permanently blocked application queue in a tight loop. */
static bool application_wakeup_armed = true;
/** Preserve the existing immediate first non-transport pass after startup. */
static bool transport_first_pass = true;

static int socket_server_address_family(const struct sockaddr_storage *address) {
    return ((const struct sockaddr *)address)->sa_family;
}

static bool socket_server_address_loopback(const struct sockaddr_storage *address) {
    if (socket_server_address_family(address) == AF_INET) {
        const struct sockaddr_in *address4 = (const struct sockaddr_in *)address;
        return (ntohl(address4->sin_addr.s_addr) & 0xff000000U) == 0x7f000000U;
    }
#ifdef HAVE_IPV6
    if (socket_server_address_family(address) == AF_INET6) {
        const struct sockaddr_in6 *address6 = (const struct sockaddr_in6 *)address;
        return IN6_IS_ADDR_LOOPBACK(&address6->sin6_addr);
    }
#endif
    return false;
}

static bool socket_server_address_unspecified(const struct sockaddr_storage *address) {
    if (socket_server_address_family(address) == AF_INET) {
        const struct sockaddr_in *address4 = (const struct sockaddr_in *)address;
        return address4->sin_addr.s_addr == htonl(INADDR_ANY);
    }
#ifdef HAVE_IPV6
    if (socket_server_address_family(address) == AF_INET6) {
        const struct sockaddr_in6 *address6 = (const struct sockaddr_in6 *)address;
        return IN6_IS_ADDR_UNSPECIFIED(&address6->sin6_addr);
    }
#endif
    return false;
}

#define SOCKET_PENDING_CONNECTIONS_MAX 128U

/**
 * Defines all the possible socket commands.
 */
static const socket_command_t socket_commands[] = {
#define ATRINIK_SERVER_COMMAND_HANDLER(_symbol, _handler, _policy, _player_only)        \
    [SERVER_CMD_##_symbol] = {.handle_func = (_handler),                                \
                              .flags = (_player_only) ? SOCKET_COMMAND_PLAYER_ONLY : 0, \
                              .policy = SOCKET_COMMAND_POLICY_##_policy,                \
                              .name = SERVER_CMD_NAME_##_symbol},
#include "command_handlers.def"
#undef ATRINIK_SERVER_COMMAND_HANDLER
};
CASSERT_ARRAY(socket_commands, SERVER_CMD_NROF);

bool socket_connection_admitted(const socket_struct *cs) {
    HARD_ASSERT(cs != NULL);

    return cs->socket_version == SOCKET_VERSION && cs->setup_completed &&
           (*settings.join_password == '\0' || cs->join_authenticated);
}

bool socket_server_command_phase_allowed(const socket_struct *cs, uint8_t type) {
    HARD_ASSERT(cs != NULL);

    if (type >= SERVER_CMD_NROF || socket_commands[type].handle_func == NULL) {
        return false;
    }

    switch (socket_commands[type].policy) {
        case SOCKET_COMMAND_POLICY_CONTROL:
            /* The central dispatcher separately applies the control
             * protocol's source-IP authentication before invoking it. */
            return cs->state == ST_LOGIN;

        case SOCKET_COMMAND_POLICY_LIVENESS:
            return cs->state == ST_LOGIN || cs->state == ST_PLAYING;

        case SOCKET_COMMAND_POLICY_VERSION:
            return cs->state == ST_LOGIN && cs->socket_version == 0 && !cs->setup_completed;

        case SOCKET_COMMAND_POLICY_SETUP:
            if (cs->state == ST_LOGIN) {
                return cs->socket_version == SOCKET_VERSION && !cs->setup_completed;
            }
            return cs->state == ST_PLAYING && socket_connection_admitted(cs);

        case SOCKET_COMMAND_POLICY_LOGIN:
            return cs->state == ST_LOGIN && socket_connection_admitted(cs);

        case SOCKET_COMMAND_POLICY_ADMITTED:
            return (cs->state == ST_LOGIN || cs->state == ST_PLAYING) &&
                   socket_connection_admitted(cs);

        case SOCKET_COMMAND_POLICY_PLAYING:
            return cs->state == ST_PLAYING && socket_connection_admitted(cs);
    }

    return false;
}

/**
 * Initialize the socket server API.
 */
TOOLKIT_INIT_FUNC(socket_server) {
    /* Used to store the parsed network stack setting. */
    struct {
        /* Type of the network stack; some of these can be combined. */
        enum {
            STACK_IPV4,
            STACK_IPV6,
            STACK_DUAL,
        } type;

        /* IP addresses to bind to in non-dual-stack configurations. */
        struct sockaddr_storage v4;
        struct sockaddr_storage v6;
        char v4_host[INET_ADDRSTRLEN];
        char v6_host[65];
        bool v4_explicit;
        bool v6_explicit;
        bool v4_seen;
        bool v6_seen;
    } stack_setting;
    memset(&stack_setting, 0, sizeof(stack_setting));
    snprintf(VS(stack_setting.v4_host), "%s", "0.0.0.0");
    snprintf(VS(stack_setting.v6_host), "%s", "::");

    char word[MAX_BUF];
    size_t pos = 0;
    while (string_get_word(settings.network_stack, &pos, ',', VS(word), 0)) {
        string_whitespace_trim(word);

        char *cps[2];
        if (string_split(word, cps, arraysize(cps), '=') < 1) {
            LOG(ERROR, "Failed to split string: %s", word);
            exit(1);
        }

        if (strcasecmp(cps[0], "dual") == 0) {
            if (strcasecmp(settings.network_stack, "dual") != 0) {
                LOG(ERROR, "The dual network stack setting cannot be combined with others");
                exit(1);
            }
            stack_setting.type = 0;
            BIT_SET(stack_setting.type, STACK_DUAL);
            break;
        }

        BIT_CLEAR(stack_setting.type, STACK_DUAL);

        struct sockaddr_storage *addr;
        int family;
        if (strcasecmp(cps[0], "ipv4") == 0 || strcasecmp(cps[0], "v4") == 0) {
            if (stack_setting.v4_seen) {
                LOG(ERROR, "Duplicate IPv4 network stack setting");
                exit(1);
            }
            stack_setting.v4_seen = true;
            BIT_SET(stack_setting.type, STACK_IPV4);
            addr = &stack_setting.v4;
            family = AF_INET;
            struct sockaddr_in *saddr = (struct sockaddr_in *)addr;
            saddr->sin_family = AF_INET;
        } else if (strcasecmp(cps[0], "ipv6") == 0 || strcasecmp(cps[0], "v6") == 0) {
#ifdef HAVE_IPV6
            if (stack_setting.v6_seen) {
                LOG(ERROR, "Duplicate IPv6 network stack setting");
                exit(1);
            }
            stack_setting.v6_seen = true;
            BIT_SET(stack_setting.type, STACK_IPV6);
            addr = &stack_setting.v6;
            family = AF_INET6;
            struct sockaddr_in6 *saddr = (struct sockaddr_in6 *)addr;
            saddr->sin6_family = AF_INET6;
#else
            LOG(ERROR, "IPv6 network stack setting is not supported by this build");
            exit(1);
#endif
        } else {
            LOG(ERROR, "Invalid value in network stack setting: %s", cps[0]);
            exit(1);
        }

        if (cps[1] != NULL && !socket_host2addr(cps[1], addr)) {
            LOG(ERROR, "Invalid IP address in network stack configuration: %s", cps[1]);
            exit(1);
        }
        if (cps[1] != NULL) {
            if (socket_server_address_family(addr) != family) {
                LOG(ERROR, "Address family does not match network stack setting: %s", word);
                exit(1);
            }
            char *host = addr == &stack_setting.v4 ? stack_setting.v4_host : stack_setting.v6_host;
            size_t host_size = addr == &stack_setting.v4 ? sizeof(stack_setting.v4_host)
                                                         : sizeof(stack_setting.v6_host);
            socklen_t addr_size = sizeof(struct sockaddr_in);
#ifdef HAVE_IPV6
            if (family == AF_INET6) {
                addr_size = sizeof(struct sockaddr_in6);
            }
#endif
            if (getnameinfo((const struct sockaddr *)addr,
                            addr_size,
                            host,
                            (socklen_t)host_size,
                            NULL,
                            0,
                            NI_NUMERICHOST) != 0) {
                LOG(ERROR, "Failed to format network stack address: %s", cps[1]);
                exit(1);
            }
            if (family == AF_INET) {
                stack_setting.v4_explicit = true;
            } else {
                stack_setting.v6_explicit = true;
            }
        }
    }

    if (stack_setting.type == 0) {
        LOG(ERROR, "No network stack configuration selected");
        exit(1);
    }

    {
        if (settings.port_quic == 0) {
            LOG(ERROR, "No QUIC UDP port configured");
            exit(1);
        }
        char identity_path[HUGE_BUF];
        snprintf(VS(identity_path), "%s/quic-identity.pem", settings.datapath);
        bool dual = BIT_QUERY(stack_setting.type, STACK_DUAL);
        bool bind_v4 = dual || BIT_QUERY(stack_setting.type, STACK_IPV4);
        if (bind_v4) {
            quic_server_sockets[0] = socket_quic_server_create(stack_setting.v4_host,
                                                               settings.port_quic,
                                                               false,
                                                               identity_path);
        }
#ifdef HAVE_IPV6
        if (dual || BIT_QUERY(stack_setting.type, STACK_IPV6)) {
            quic_server_sockets[1] = socket_quic_server_create(stack_setting.v6_host,
                                                               settings.port_quic,
                                                               false,
                                                               identity_path);
        }
#endif
        socket_t *identity_socket =
            quic_server_sockets[0] != NULL ? quic_server_sockets[0] : quic_server_sockets[1];
        if (identity_socket == NULL ||
            !socket_certificate_sha256(identity_socket, quic_certificate_sha256)) {
            LOG(ERROR, "Failed to initialize the QUIC listener");
            exit(1);
        }
        LOG(SYSTEM, "QUIC certificate SHA-256: %s", quic_certificate_sha256);

        bool listen_v4 = quic_server_sockets[0] != NULL;
        bool listen_v6 = quic_server_sockets[1] != NULL;
        bool restrict_v4 =
            stack_setting.v4_explicit && !socket_server_address_unspecified(&stack_setting.v4);
        bool restrict_v6 =
            stack_setting.v6_explicit && !socket_server_address_unspecified(&stack_setting.v6);
        bool v4_loopback =
            listen_v4 && restrict_v4 && socket_server_address_loopback(&stack_setting.v4);
        bool v6_loopback =
            listen_v6 && restrict_v6 && socket_server_address_loopback(&stack_setting.v6);
        bool loopback_only =
            (listen_v4 || listen_v6) && (!listen_v4 || v4_loopback) && (!listen_v6 || v6_loopback);

        quic_candidate_count = 0;
        metaserver_public_endpoint_from_config(settings.server_host,
                                               settings.port_quic,
                                               VS(quic_public_host),
                                               &quic_public_port);
        if (*settings.server_host != '\0') {
            LOG(INFO,
                "The legacy server_host IP is not published; this server will use an "
                "addressless directory listing");
        }
        char mapped_host[65];
        uint16_t mapped_port;
        const char *mapping_local_host = restrict_v4 ? stack_setting.v4_host : NULL;
        if (listen_v4 && !v4_loopback &&
            socket_port_mapping_init(settings.port_quic,
                                     mapping_local_host,
                                     VS(mapped_host),
                                     &mapped_port)) {
            if (!socket_host_is_global(mapped_host)) {
                LOG(INFO,
                    "Router mapping is not globally routable; retaining it as an intermediate "
                    "candidate");
            }
            snprintf(VS(quic_candidates[quic_candidate_count].host), "%s", mapped_host);
            quic_candidates[quic_candidate_count].port = mapped_port;
            quic_candidates[quic_candidate_count].kind = SOCKET_CANDIDATE_MAPPED;
            quic_candidate_count++;
        }

        char stun_host[65];
        uint16_t stun_port = settings.port_quic;
        if (loopback_only) {
            LOG(INFO, "Direct candidate discovery is disabled for loopback-only listeners");
        } else if (!v4_loopback && listen_v4 && *settings.stun_server != '\0' &&
                   strcmp(settings.stun_server, "off") != 0 &&
                   socket_stun_discover(quic_server_sockets[0],
                                        settings.stun_server,
                                        VS(stun_host),
                                        &stun_port)) {
            bool duplicate = quic_candidate_count != 0 && quic_candidates[0].port == stun_port &&
                             strcmp(quic_candidates[0].host, stun_host) == 0;
            if (!duplicate && quic_candidate_count < arraysize(quic_candidates)) {
                snprintf(VS(quic_candidates[quic_candidate_count].host), "%s", stun_host);
                quic_candidates[quic_candidate_count].port = stun_port;
                quic_candidates[quic_candidate_count].kind = SOCKET_CANDIDATE_SRFLX;
                quic_candidate_count++;
            }
        } else if (!v6_loopback && listen_v6 && *settings.stun_server != '\0' &&
                   strcmp(settings.stun_server, "off") != 0 &&
                   socket_stun_discover(quic_server_sockets[1],
                                        settings.stun_server,
                                        VS(stun_host),
                                        &stun_port)) {
            snprintf(VS(quic_candidates[quic_candidate_count].host), "%s", stun_host);
            quic_candidates[quic_candidate_count].port = stun_port;
            quic_candidates[quic_candidate_count].kind = SOCKET_CANDIDATE_IPV6;
            quic_candidate_count++;
        } else if (*quic_public_host == '\0') {
            if (strcmp(settings.port_mapping, "off") == 0 &&
                strcmp(settings.stun_server, "off") == 0) {
                LOG(INFO,
                    "Public UDP candidate discovery is disabled; only "
                    "LAN/IPv6 direct routes are available");
            } else {
                LOG(ERROR,
                    "No mapped or STUN public UDP candidate is available; "
                    "only LAN/IPv6 direct routes may work");
            }
        }

        socket_direct_candidate_t local_candidates[SOCKET_DIRECT_MAX_CANDIDATES];
        size_t local_count = loopback_only ? 0
                                           : socket_local_candidates(settings.port_quic,
                                                                     local_candidates,
                                                                     arraysize(local_candidates));
        for (size_t i = 0; i < local_count && quic_candidate_count < arraysize(quic_candidates);
             i++) {
            struct sockaddr_storage address;
            if (!socket_host2addr(local_candidates[i].host, &address)) {
                continue;
            }
            bool allowed =
                socket_server_address_family(&address) == AF_INET && listen_v4 &&
                (!restrict_v4 || strcmp(local_candidates[i].host, stack_setting.v4_host) == 0);
#ifdef HAVE_IPV6
            allowed =
                allowed ||
                (socket_server_address_family(&address) == AF_INET6 && listen_v6 &&
                 (!restrict_v6 || strcmp(local_candidates[i].host, stack_setting.v6_host) == 0));
#endif
            if (allowed) {
                quic_candidates[quic_candidate_count++] = local_candidates[i];
            }
        }
        for (size_t i = 0; i < quic_candidate_count; i++) {
            LOG(INFO,
                "Discovered a direct %s QUIC candidate",
                socket_candidate_kind_name(quic_candidates[i].kind));
        }
    }

    client_sockets = NULL;
    client_sockets_count = 0;
    client_service_cursor = 0;
    player_service_cursor = 0;
    application_wakeup_armed = true;
    transport_first_pass = true;
}
TOOLKIT_INIT_FUNC_FINISH

/**
 * Deinitialize the socket server API.
 */
TOOLKIT_DEINIT_FUNC(socket_server) {
    csocket_entry_t *entry, *tmp;
    DL_FOREACH_SAFE(client_sockets, entry, tmp) {
        socket_server_csocket_drop(entry);
    }
    socket_port_mapping_deinit();
    for (size_t i = 0; i < arraysize(quic_server_sockets); i++) {
        if (quic_server_sockets[i] != NULL) {
            socket_destroy(quic_server_sockets[i]);
            quic_server_sockets[i] = NULL;
        }
    }
}
TOOLKIT_DEINIT_FUNC_FINISH
bool socket_server_quic_identity(char certificate_sha256[65]) {
    HARD_ASSERT(certificate_sha256 != NULL);

    if (quic_server_sockets[0] == NULL && quic_server_sockets[1] == NULL) {
        return false;
    }
    memcpy(certificate_sha256, quic_certificate_sha256, sizeof(quic_certificate_sha256));
    return true;
}

struct metaserver_publisher_identity *socket_server_quic_publisher_identity(void) {
    socket_t *identity_socket =
        quic_server_sockets[0] != NULL ? quic_server_sockets[0] : quic_server_sockets[1];
    return identity_socket != NULL
               ? socket_quic_publisher_identity(identity_socket, quic_certificate_sha256)
               : NULL;
}

bool socket_server_quic_info(char *host,
                             size_t host_size,
                             uint16_t *port,
                             char certificate_sha256[65]) {
    HARD_ASSERT(host != NULL);
    HARD_ASSERT(port != NULL);
    HARD_ASSERT(certificate_sha256 != NULL);

    if (!socket_server_quic_identity(certificate_sha256)) {
        return false;
    }

    snprintf(host, host_size, "%s", quic_public_host);
    *port = *quic_public_host != '\0' ? quic_public_port : settings.port_quic;
    return true;
}

size_t socket_server_quic_candidates(socket_direct_candidate_t *candidates, size_t capacity) {
    size_t count = MIN(quic_candidate_count, capacity);
    if (count != 0) {
        memcpy(candidates, quic_candidates, count * sizeof(*candidates));
    }
    return count;
}

bool socket_server_quic_punch(const char *host, uint16_t port) {
    size_t index = strchr(host, ':') != NULL ? 1 : 0;
    if (quic_server_sockets[index] == NULL) {
        return false;
    }

    return socket_udp_punch(quic_server_sockets[index], host, port);
}

static bool socket_server_quic_punch_receive(socket_t *server_socket) {
    char host[65];
    uint16_t port;
    if (!socket_udp_punch_receive(server_socket, VS(host), &port)) {
        return false;
    }

    quic_punches_received++;
    bool echoed = socket_udp_punch(server_socket, host, port);
    if (echoed) {
        quic_punches_echoed++;
    }
    LOG(DEBUG,
        "Received direct UDP punch; echo %s (received=%" PRIu64 ", echoed=%" PRIu64 ")",
        echoed ? "sent" : "failed",
        quic_punches_received,
        quic_punches_echoed);
    return true;
}

/** Check whether the control protocol accepts the connection's source IP. */
static bool socket_server_control_authorized(socket_struct *cs) {
    if (strcasecmp(settings.control_allowed_ips, "none") == 0) {
        LOG(PACKET, "Control command received but no IPs are allowed.");
        return false;
    }

    char word[MAX_BUF];
    size_t pos = 0;
    while (string_get_word(settings.control_allowed_ips, &pos, ',', VS(word), 0)) {
        char *split[2];
        if (string_split(word, split, arraysize(split), '/') < 1) {
            continue;
        }

        struct sockaddr_storage addr;
        if (!socket_host2addr(split[0], &addr)) {
            continue;
        }

        unsigned short plen = socket_addr_plen(&addr);
        if (split[1] != NULL) {
            uint64_t value;
            if (!string_parse_uint64(split[1], 10, 0, plen, &value)) {
                LOG(ERROR, "Ignoring invalid control CIDR prefix: %s", word);
                continue;
            }
            plen = (unsigned short)value;
        }

        if (socket_cmp_addr(cs->sc, &addr, plen) == 0) {
            return true;
        }
    }

    LOG(PACKET, "Received control command from unauthorized IP: %s", socket_get_id(cs->sc));
    return false;
}

/**
 * Attempt to handle a command from the client.
 *
 * @return
 * True if the command was handled or rejected. False only when a valid
 * playing-only command must be queued for its player.
 */
bool socket_server_handle_command(socket_struct *cs, player *pl, uint8_t *data, size_t len) {
    size_t pos = 0;
    packet_reader_t reader;
    packet_reader_init_cursor(&reader, data, len, &pos);
    uint8_t type = packet_reader_read_uint8(&reader);

#ifndef DEBUG
    char *cp;

    LOG(DUMPRX, "Received packet with command type %d (%" PRIu64 " bytes):", type, (uint64_t)len);
    cp = xmalloc(sizeof(*cp) * (len * 3 + 1));
    string_tohex(data, len, cp, len * 3 + 1, true);
    LOG(DUMPRX, "  Hexadecimal: %s", cp);
    free(cp);
#endif

    if (packet_reader_error(&reader) != PACKET_ERROR_NONE) {
        LOG(DEVEL, "Malformed command envelope: %s", packet_error_string(reader.error));
        return true;
    }

    if (type >= SERVER_CMD_NROF || socket_commands[type].handle_func == NULL) {
        LOG(DEVEL, "Unknown command type: %" PRIu8, type);
        return true;
    }

    if (!socket_server_command_phase_allowed(cs, type)) {
        LOG(DEVEL,
            "Rejected out-of-order %s command in connection state %d",
            socket_commands[type].name,
            cs->state);
        return true;
    }

    if (socket_commands[type].policy == SOCKET_COMMAND_POLICY_CONTROL &&
        !socket_server_control_authorized(cs)) {
        return true;
    }

    /* Playing-only commands read directly from the transport are queued for
     * the associated player. The phase check above drops them before login. */
    if (socket_commands[type].flags & SOCKET_COMMAND_PLAYER_ONLY && pl == NULL) {
        return false;
    }

    if (pl == NULL && socket_commands[type].policy == SOCKET_COMMAND_POLICY_PLAYING) {
        pl = socket_server_player_find(cs);
    }

    packet_reader_scope_t scope;
    packet_reader_scope_begin(&scope);
    packet_reader_init_at(&reader, data + pos, len - pos, 0);
    socket_commands[type].handle_func(cs, pl, data + pos, len - pos, 0);
    packet_error_t error = packet_reader_scope_finish(&scope);
    if (error != PACKET_ERROR_NONE) {
        LOG(DEVEL,
            "Rejected malformed %s command: %s",
            socket_commands[type].name,
            packet_error_string(error));
    } else if (cs->state == ST_LOGIN && cs->setup_completed) {
        socket_login_deadline_refresh(cs);
    }

    return true;
}

static void socket_server_csocket_create(socket_t *server_socket) {
    socket_t *accepted = socket_accept(server_socket);
    if (accepted == NULL) {
        return;
    }
    if (client_sockets_count >= SOCKET_PENDING_CONNECTIONS_MAX) {
        LOG(ERROR,
            "Rejecting connection: pending login limit (%u) reached",
            SOCKET_PENDING_CONNECTIONS_MAX);
        server_metrics_connection_rejected(client_sockets_count);
        socket_destroy(accepted);
        return;
    }
    csocket_entry_t *entry = xcalloc(1, sizeof(*entry));
    entry->cs = xcalloc(1, sizeof(*entry->cs));
    entry->cs->sc = accepted;

    init_connection(entry->cs);
    DL_APPEND(client_sockets, entry);
    client_sockets_count++;
    server_metrics_connection_accepted(client_sockets_count);
}

/**
 * Frees the specified client socket entry.
 *
 * @param entry
 * Entry to free.
 */
static void socket_server_csocket_free(csocket_entry_t *entry) {
    HARD_ASSERT(entry != NULL);
    free_newsocket(entry->cs);
    DL_DELETE(client_sockets, entry);
    HARD_ASSERT(client_sockets_count != 0);
    client_sockets_count--;
    server_metrics_pending_changed(client_sockets_count);
    free(entry);
}

/**
 * Drops the specified client socket entry connection.
 *
 * Essentially the same as socket_server_csocket_free(), but logs a message.
 *
 * @param entry
 * Entry to drop.
 */
static void socket_server_csocket_drop(csocket_entry_t *entry) {
    HARD_ASSERT(entry != NULL);
    LOG(SYSTEM, "Connection %s: dropping connection", socket_get_id(entry->cs->sc));
    socket_server_csocket_free(entry);
}

static csocket_entry_t *socket_server_csocket_find(socket_struct *cs) {
    csocket_entry_t *entry;
    DL_FOREACH(client_sockets, entry) {
        if (entry->cs == cs) {
            return entry;
        }
    }
    return NULL;
}

static player *socket_server_player_find(socket_struct *cs) {
    player *pl;
    DL_FOREACH(first_player, pl) {
        if (pl->cs == cs) {
            return pl;
        }
    }
    return NULL;
}

static bool socket_server_quic_network_ready(socket_t *sc) {
    for (size_t i = 0; i < arraysize(quic_server_sockets); i++) {
        if (quic_server_sockets[i] != NULL && socket_fd(quic_server_sockets[i]) == socket_fd(sc) &&
            FD_ISSET(socket_fd(sc), &fds_read)) {
            return true;
        }
    }
    return false;
}

#define SOCKET_COMMAND_QUEUE_TOMBSTONE UINT8_MAX

static uint32_t socket_server_command_epoch(const uint8_t *data, size_t len) {
    size_t epoch_pos;

    if (len == 7 && data[0] == SERVER_CMD_MOVE) {
        epoch_pos = 3;
    } else if (len == 10 && data[0] == SERVER_CMD_FIRE && data[2] == 0 && data[3] == 0 &&
               data[4] == 0 && data[5] == 0) {
        epoch_pos = 6;
    } else {
        return 0;
    }
    return ((uint32_t)data[epoch_pos] << 24) | ((uint32_t)data[epoch_pos + 1] << 16) |
           ((uint32_t)data[epoch_pos + 2] << 8) | data[epoch_pos + 3];
}

static socket_movement_queue_index *socket_server_command_queue_index(socket_struct *cs,
                                                                      uint8_t command) {
    if (command == SERVER_CMD_MOVE) {
        return &cs->movement_stream_move;
    }
    if (command == SERVER_CMD_FIRE) {
        return &cs->movement_stream_fire;
    }
    return NULL;
}

static void socket_server_command_queue_prune_index(socket_struct *cs,
                                                    socket_movement_queue_index *index) {
    while (index->entries_start < index->entries_num &&
           index->entries[index->entries_start].offset < cs->packet_recv_cmd_base) {
        index->entries_start++;
    }
    if (index->entries_start == index->entries_num) {
        index->entries_start = 0;
        index->entries_num = 0;
    }
}

static void socket_server_command_queue_prune(socket_struct *cs) {
    socket_server_command_queue_prune_index(cs, &cs->movement_stream_move);
    socket_server_command_queue_prune_index(cs, &cs->movement_stream_fire);
}

static bool
socket_server_command_queue_frame(const packet_struct *queue, size_t offset, size_t *frame_len) {
    if (offset > queue->len || queue->len - offset < 3) {
        return false;
    }
    size_t payload_len = ((size_t)queue->data[offset] << 8) | queue->data[offset + 1];
    if (payload_len == 0 || payload_len > queue->len - offset - 2) {
        return false;
    }
    *frame_len = payload_len + 2;
    return true;
}

/** Compact canceled movement records once while preserving every live frame's order. */
static bool socket_server_command_queue_compact(socket_struct *cs) {
    if (cs->movement_stream_tombstone_bytes == 0) {
        return true;
    }

    socket_server_command_queue_prune(cs);
    socket_movement_queue_index *indexes[] = {
        &cs->movement_stream_move,
        &cs->movement_stream_fire,
    };
    const uint8_t commands[] = {SERVER_CMD_MOVE, SERVER_CMD_FIRE};
    size_t entry_indexes[] = {
        indexes[0]->entries_start,
        indexes[1]->entries_start,
    };
    size_t tombstone_bytes = 0;
    for (size_t offset = 0; offset < cs->packet_recv_cmd->len;) {
        size_t frame_len;
        if (!socket_server_command_queue_frame(cs->packet_recv_cmd, offset, &frame_len)) {
            return false;
        }
        uint64_t type_offset = cs->packet_recv_cmd_base + offset + 2;
        for (size_t i = 0; i < arraysize(indexes); i++) {
            if (entry_indexes[i] < indexes[i]->entries_num &&
                indexes[i]->entries[entry_indexes[i]].offset < type_offset) {
                return false;
            }
            if (entry_indexes[i] < indexes[i]->entries_num &&
                indexes[i]->entries[entry_indexes[i]].offset == type_offset) {
                if (cs->packet_recv_cmd->data[offset + 2] != commands[i]) {
                    return false;
                }
                entry_indexes[i]++;
            }
        }
        if (cs->packet_recv_cmd->data[offset + 2] == SOCKET_COMMAND_QUEUE_TOMBSTONE) {
            tombstone_bytes += frame_len;
        }
        offset += frame_len;
    }
    for (size_t i = 0; i < arraysize(indexes); i++) {
        if (entry_indexes[i] != indexes[i]->entries_num) {
            return false;
        }
        entry_indexes[i] = indexes[i]->entries_start;
    }
    if (tombstone_bytes != cs->movement_stream_tombstone_bytes) {
        return false;
    }

    size_t write_offset = 0;
    for (size_t read_offset = 0; read_offset < cs->packet_recv_cmd->len;) {
        size_t frame_len;
        if (!socket_server_command_queue_frame(cs->packet_recv_cmd, read_offset, &frame_len)) {
            return false;
        }
        uint64_t old_type_offset = cs->packet_recv_cmd_base + read_offset + 2;
        if (cs->packet_recv_cmd->data[read_offset + 2] != SOCKET_COMMAND_QUEUE_TOMBSTONE) {
            for (size_t i = 0; i < arraysize(indexes); i++) {
                if (entry_indexes[i] < indexes[i]->entries_num &&
                    indexes[i]->entries[entry_indexes[i]].offset == old_type_offset) {
                    indexes[i]->entries[entry_indexes[i]].offset =
                        cs->packet_recv_cmd_base + write_offset + 2;
                    entry_indexes[i]++;
                }
            }
            if (write_offset != read_offset) {
                memmove(cs->packet_recv_cmd->data + write_offset,
                        cs->packet_recv_cmd->data + read_offset,
                        frame_len);
            }
            write_offset += frame_len;
        }
        read_offset += frame_len;
    }
    cs->packet_recv_cmd->len = write_offset;
    for (size_t i = 0; i < arraysize(indexes); i++) {
        if (indexes[i]->entries_start == 0) {
            continue;
        }
        size_t live = indexes[i]->entries_num - indexes[i]->entries_start;
        memmove(indexes[i]->entries,
                indexes[i]->entries + indexes[i]->entries_start,
                live * sizeof(*indexes[i]->entries));
        indexes[i]->entries_num = live;
        indexes[i]->entries_start = 0;
    }
    cs->movement_stream_tombstone_bytes = 0;
    return true;
}

/** Append one framed player command and index its replaceable movement epoch. */
bool socket_server_command_queue_append(socket_struct *cs, const uint8_t *data, size_t len) {
    if (cs->packet_recv_cmd->len == 0) {
        socket_server_command_queue_reset(cs);
    }
    if (len == 0 || len > UINT16_MAX || len > SIZE_MAX - 2) {
        cs->packet_recv_cmd->error = PACKET_ERROR_SIZE_OVERFLOW;
        return false;
    }
    size_t frame_len = len + 2;
    if (cs->movement_stream_tombstone_bytes > cs->packet_recv_cmd->len ||
        frame_len > SOCKET_COMMAND_QUEUE_MAX ||
        cs->packet_recv_cmd->len - cs->movement_stream_tombstone_bytes >
            SOCKET_COMMAND_QUEUE_MAX - frame_len) {
        cs->packet_recv_cmd->error = PACKET_ERROR_LIMIT_EXCEEDED;
        return false;
    }
    if (cs->movement_stream_tombstone_bytes >= SOCKET_COMMAND_QUEUE_COMPACT_MIN ||
        cs->packet_recv_cmd->len > SOCKET_COMMAND_QUEUE_STORAGE_MAX - frame_len) {
        if (!socket_server_command_queue_compact(cs)) {
            cs->packet_recv_cmd->error = PACKET_ERROR_INVALID_ENCODING;
            return false;
        }
    }

    size_t type_offset = cs->packet_recv_cmd->len + 2;
    packet_writer_write_uint16(cs->packet_recv_cmd, len);
    packet_writer_write_bytes(cs->packet_recv_cmd, data, len);
    if (!packet_writer_finish(cs->packet_recv_cmd)) {
        return false;
    }

    uint32_t epoch = socket_server_command_epoch(data, len);
    if (epoch == 0) {
        return true;
    }
    if (epoch != cs->movement_stream_epoch) {
        cs->movement_stream_epoch = epoch;
        cs->movement_stream_move.entries_start = 0;
        cs->movement_stream_move.entries_num = 0;
        cs->movement_stream_fire.entries_start = 0;
        cs->movement_stream_fire.entries_num = 0;
    } else {
        socket_server_command_queue_prune(cs);
    }
    socket_movement_queue_index *index = socket_server_command_queue_index(cs, data[0]);
    HARD_ASSERT(index != NULL);
    if (index->entries_num == index->entries_size) {
        if (index->entries_start != 0) {
            size_t live = index->entries_num - index->entries_start;
            memmove(index->entries,
                    index->entries + index->entries_start,
                    live * sizeof(*index->entries));
            index->entries_start = 0;
            index->entries_num = live;
        } else {
            size_t new_size = index->entries_size == 0 ? 8 : index->entries_size * 2;
            index->entries = xrealloc(index->entries, new_size * sizeof(*index->entries));
            index->entries_size = new_size;
        }
    }
    socket_movement_queue_entry *entry = &index->entries[index->entries_num++];
    entry->offset = cs->packet_recv_cmd_base + type_offset;
    return true;
}

/** Tombstone queued records from the current keyboard movement epoch in bounded work. */
bool socket_server_command_queue_clear_stream(socket_struct *cs, uint8_t command, uint32_t epoch) {
    if (epoch == 0 || epoch != cs->movement_stream_epoch) {
        return true;
    }
    socket_movement_queue_index *index = socket_server_command_queue_index(cs, command);
    if (index == NULL) {
        return false;
    }
    socket_server_command_queue_prune_index(cs, index);
    for (size_t i = index->entries_start; i < index->entries_num; i++) {
        socket_movement_queue_entry *entry = &index->entries[i];
        uint64_t relative = entry->offset - cs->packet_recv_cmd_base;
        size_t frame_len;
        if (relative < 2 || relative >= cs->packet_recv_cmd->len ||
            cs->packet_recv_cmd->data[relative] != command ||
            !socket_server_command_queue_frame(cs->packet_recv_cmd,
                                               (size_t)relative - 2,
                                               &frame_len)) {
            return false;
        }
    }

    for (size_t i = index->entries_start; i < index->entries_num; i++) {
        size_t relative = (size_t)(index->entries[i].offset - cs->packet_recv_cmd_base);
        size_t frame_offset = relative - 2;
        size_t frame_len;
        if (!socket_server_command_queue_frame(cs->packet_recv_cmd, frame_offset, &frame_len)) {
            return false;
        }
        cs->packet_recv_cmd->data[relative] = SOCKET_COMMAND_QUEUE_TOMBSTONE;
        cs->movement_stream_tombstone_bytes += frame_len;
    }
    index->entries_start = 0;
    index->entries_num = 0;
    return true;
}

/** Discard the buffered command queue and all of its movement-stream metadata. */
void socket_server_command_queue_reset(socket_struct *cs) {
    cs->packet_recv_cmd->len = 0;
    cs->packet_recv_cmd->error = PACKET_ERROR_NONE;
    cs->packet_recv_cmd_base = 0;
    cs->movement_stream_epoch = 0;
    cs->movement_stream_move.entries_start = 0;
    cs->movement_stream_move.entries_num = 0;
    cs->movement_stream_fire.entries_start = 0;
    cs->movement_stream_fire.entries_num = 0;
    cs->movement_stream_tombstone_bytes = 0;
}

/**
 * Handle client commands.
 *
 * We only get here once there is input, and only do basic connection
 * checking.
 *
 * @param pl
 * Player to handle commands for.
 */
void socket_server_handle_client(player *pl) {
    HARD_ASSERT(pl != NULL);

    if (!socket_server_command_queue_compact(pl->cs)) {
        LOG(ERROR, "Discarding an inconsistent buffered player-command queue");
        socket_server_command_queue_reset(pl->cs);
        return;
    }

    for (int num_cmds = 0; num_cmds < SOCKET_SERVER_PLAYER_MAX_COMMANDS; num_cmds++) {
        if (pl->cs->packet_recv_cmd->len == 0) {
            break;
        }

        size_t len = 2 + (pl->cs->packet_recv_cmd->data[0] << 8) + pl->cs->packet_recv_cmd->data[1];

        /* Ensure the player is in a state capable of issue commands, and
         * has enough speed left to do so. */
        if (pl->cs->state == ST_ZOMBIE || pl->cs->state == ST_DEAD ||
            (pl->cs->state == ST_PLAYING && pl->ob != NULL && pl->ob->speed_left < 0)) {
            break;
        }

        /* Reset idle counter. */
        if (pl->cs->state == ST_PLAYING) {
            pl->cs->login_count = 0;
            pl->cs->keepalive = 0;
        }

        socket_server_handle_command(pl->cs, pl, pl->cs->packet_recv_cmd->data + 2, len - 2);
        packet_delete(pl->cs->packet_recv_cmd, 0, len);
        pl->cs->packet_recv_cmd_base += len;
        socket_server_command_queue_prune(pl->cs);
    }
}

/**
 * Removes the specified client socket from the server's managed list
 * of clients that haven't logged in yet. The client socket remains valid
 * afterwards.
 *
 * This is used from the login routine, because as soon as the client logs
 * in, they go to the player list, which is also walked through in the server
 * socket code, thus, it needs to be removed from the other list.
 *
 * @param cs
 * Client socket to remove.
 * @return
 * True on success, false on failure (no such client socket).
 */
bool socket_server_remove(socket_struct *cs) {
    csocket_entry_t *entry, *tmp;
    DL_FOREACH_SAFE(client_sockets, entry, tmp) {
        if (entry->cs == cs) {
            DL_DELETE(client_sockets, entry);
            HARD_ASSERT(client_sockets_count != 0);
            client_sockets_count--;
            server_metrics_pending_changed(client_sockets_count);
            free(entry);
            return true;
        }
    }

    return false;
}

/** Check whether the specified client socket is in zombie state. */
static inline bool server_socket_csocket_is_zombie(socket_struct *cs) {
    HARD_ASSERT(cs != NULL);
    return cs->state == ST_ZOMBIE;
}

/**
 * Read data from the specified client socket and handle complete commands.
 *
 * @param cs
 * Client socket.
 */
static inline void socket_server_csocket_read(socket_struct *cs) {
    HARD_ASSERT(cs != NULL);

    if (cs->state == ST_DEAD) {
        return;
    }

    size_t amt;
    if (!socket_read(cs->sc,
                     (void *)(cs->packet_recv->data + cs->packet_recv->len),
                     cs->packet_recv->size - cs->packet_recv->len,
                     &amt)) {
        cs->state = ST_DEAD;
        return;
    }

    cs->packet_recv->len += amt;

    while (cs->packet_recv->len >= 2) {
        size_t size = 2 + (cs->packet_recv->data[0] << 8) + cs->packet_recv->data[1];
        if (size > cs->packet_recv->len) {
            break;
        }

        uint8_t *data = cs->packet_recv->data;
        size_t len = size;

        uint8_t *decrypted_data = data + 2;
        size_t decrypted_len = len - 2;

        /* Try to handle the command. */
        if (!socket_server_handle_command(cs, NULL, decrypted_data, decrypted_len)) {
            /* Couldn't handle it immediately, add it to the commands
             * packet. */
            if (!socket_server_command_queue_append(cs, decrypted_data, decrypted_len)) {
                LOG(ERROR,
                    "Connection %s exceeded the buffered command limit: %s",
                    socket_get_id(cs->sc),
                    packet_error_string(packet_writer_error(cs->packet_recv_cmd)));
                cs->state = ST_DEAD;
                return;
            }
        }

        packet_delete(cs->packet_recv, 0, size);
    }
}

typedef struct socket_server_transport_stats {
    size_t ready_connections;
    size_t quic_timer_services;
    bool work_limited;
} socket_server_transport_stats_t;

static size_t socket_server_player_count(void) {
    size_t count = 0;
    player *pl;
    DL_FOREACH(first_player, pl) {
        count++;
    }
    return count;
}

static bool socket_server_pending_application(void) {
    csocket_entry_t *entry;
    DL_FOREACH(client_sockets, entry) {
        if (!server_socket_csocket_is_zombie(entry->cs) &&
            (entry->cs->packets != NULL || socket_assets_pending(entry->cs))) {
            return true;
        }
    }

    player *pl;
    DL_FOREACH(first_player, pl) {
        if (!server_socket_csocket_is_zombie(pl->cs) &&
            (pl->cs->packets != NULL || socket_assets_pending(pl->cs))) {
            return true;
        }
    }
    return false;
}

static uint64_t socket_server_pending_queue_age_us(void) {
    uint64_t now = datetime_monotonic_us();
    uint64_t oldest = 0;
    csocket_entry_t *entry;
    DL_FOREACH(client_sockets, entry) {
        uint64_t started = entry->cs->packet_queue_started_us;
        if (started != 0 && (oldest == 0 || started < oldest)) {
            oldest = started;
        }
    }

    player *pl;
    DL_FOREACH(first_player, pl) {
        uint64_t started = pl->cs->packet_queue_started_us;
        if (started != 0 && (oldest == 0 || started < oldest)) {
            oldest = started;
        }
    }

    return oldest != 0 && now >= oldest ? now - oldest : 0;
}

static void socket_server_deadline_min(uint64_t *timeout_us, socket_t *sc) {
    unsigned int timeout_ms = socket_quic_timeout(sc, UINT_MAX);
    uint64_t quic_timeout_us = (uint64_t)timeout_ms * UINT64_C(1000);
    if (quic_timeout_us < *timeout_us) {
        *timeout_us = quic_timeout_us;
    }
}

static uint64_t socket_server_transport_timeout_us(void) {
    uint64_t timeout_us = sleep_delta_timeout_us();
    if (application_wakeup_armed && socket_server_pending_application()) {
        return 0;
    }

    csocket_entry_t *entry;
    DL_FOREACH(client_sockets, entry) {
        if (!server_socket_csocket_is_zombie(entry->cs)) {
            socket_server_deadline_min(&timeout_us, entry->cs->sc);
        }
    }

    player *pl;
    DL_FOREACH(first_player, pl) {
        if (!server_socket_csocket_is_zombie(pl->cs)) {
            socket_server_deadline_min(&timeout_us, pl->cs->sc);
        }
    }
    return timeout_us;
}

static bool socket_server_service_connection(socket_struct *cs,
                                             bool network_ready,
                                             socket_server_transport_stats_t *stats) {
    bool transport_ready = socket_quic_timeout(cs->sc, 1U) == 0;
    bool timer_due = socket_quic_timer_due(cs->sc);
    bool application_pending = cs->packets != NULL || socket_assets_pending(cs);
    bool application_ready = application_wakeup_armed && application_pending;
    if (!network_ready && !transport_ready && !application_ready) {
        return false;
    }

    if (network_ready) {
        stats->ready_connections++;
    }
    if (timer_due) {
        stats->quic_timer_services++;
    }
    server_metrics_quic_service(network_ready);
    if (!socket_quic_service(cs->sc, network_ready, application_pending)) {
        return false;
    }

    socket_server_csocket_read(cs);
    return true;
}

static void socket_server_service_client_connections(socket_server_transport_stats_t *stats) {
    size_t count = client_sockets_count;
    if (count == 0) {
        client_service_cursor = 0;
        return;
    }

    size_t start = client_service_cursor % count;
    csocket_entry_t *entry = client_sockets;
    for (size_t skipped = 0; skipped < start && entry != NULL; skipped++) {
        entry = entry->next;
    }

    size_t limit = MIN(count, (size_t)SOCKET_SERVER_CONNECTIONS_PER_WAKEUP);
    stats->work_limited |= count > limit;
    size_t visited = 0;
    while (entry != NULL && visited < limit) {
        csocket_entry_t *next = entry->next;
        socket_struct *cs = entry->cs;
        if (!server_socket_csocket_is_zombie(cs) &&
            socket_server_service_connection(cs, socket_server_quic_network_ready(cs->sc), stats)) {
            csocket_entry_t *live = socket_server_csocket_find(cs);
            if (live == NULL) {
                /* Login promoted the connection to the player list. */
                entry = next;
                visited++;
                continue;
            }
            if (cs->state == ST_DEAD) {
                socket_server_csocket_drop(live);
            } else {
                socket_buffer_write(live->cs);
            }
        }
        entry = next;
        visited++;
    }

    client_service_cursor =
        client_sockets_count == 0 ? 0 : (start + visited) % client_sockets_count;
}

static void socket_server_service_player_connections(socket_server_transport_stats_t *stats) {
    size_t count = socket_server_player_count();
    if (count == 0) {
        player_service_cursor = 0;
        return;
    }

    size_t start = player_service_cursor % count;
    player *pl = first_player;
    for (size_t skipped = 0; skipped < start && pl != NULL; skipped++) {
        pl = pl->next;
    }

    size_t limit = MIN(count, (size_t)SOCKET_SERVER_CONNECTIONS_PER_WAKEUP);
    stats->work_limited |= count > limit;
    size_t visited = 0;
    while (pl != NULL && visited < limit) {
        player *next = pl->next;
        socket_struct *cs = pl->cs;
        if (!server_socket_csocket_is_zombie(cs) &&
            socket_server_service_connection(cs, socket_server_quic_network_ready(cs->sc), stats)) {
            player *live = socket_server_player_find(cs);
            if (live == NULL) {
                pl = next;
                visited++;
                continue;
            }
            if (cs->state == ST_DEAD) {
                player_logout(live);
            } else {
                socket_buffer_write(cs);
            }
        }
        pl = next;
        visited++;
    }

    size_t remaining = socket_server_player_count();
    player_service_cursor = remaining == 0 ? 0 : (start + visited) % remaining;
}

/**
 * Wait for and service transport work. The non-transport server pass is
 * scheduled independently by sleep_delta_timeout_us(), so QUIC readiness and
 * timer events do not inherit the simulation tick cadence.
 */
bool socket_server_process(void) {
    static time_t heartbeat_last;
    time_t now = time(NULL);
    if (heartbeat_last == 0 || now - heartbeat_last >= 5) {
        char path[HUGE_BUF];
        char heartbeat[64];
        snprintf(VS(path), "%s/tmp/server-heartbeat", settings.datapath);
        int length = snprintf(VS(heartbeat), "%" PRIu64 "\n", (uint64_t)now);
        if (length > 0 && (size_t)length < sizeof(heartbeat) &&
            path_write_atomic(path, heartbeat, (size_t)length, 0600)) {
            heartbeat_last = now;
        }
    }
    socket_port_mapping_process();
    FD_ZERO(&fds_read);

    int nfds = 0;

    for (size_t i = 0; i < arraysize(quic_server_sockets); i++) {
        if (quic_server_sockets[i] == NULL) {
            continue;
        }
        int fd = socket_fd(quic_server_sockets[i]);
        if (nfds < fd) {
            nfds = fd;
        }
        FD_SET(fd, &fds_read);
    }

    csocket_entry_t *entry, *entry_tmp;
    DL_FOREACH_SAFE(client_sockets, entry, entry_tmp) {
        if (socket_login_expired(entry->cs)) {
            LOG(SYSTEM,
                "Connection %s exceeded the login-phase deadline",
                socket_get_id(entry->cs->sc));
            entry->cs->state = ST_DEAD;
        }
        if (unlikely(!socket_is_fd_valid(entry->cs->sc))) {
            LOG(ERROR, "Invalid waiting socket: %s", socket_get_id(entry->cs->sc));
            entry->cs->state = ST_DEAD;
        }

        if (entry->cs->state == ST_DEAD) {
            socket_server_csocket_drop(entry);
            continue;
        }

        if (server_socket_csocket_is_zombie(entry->cs)) {
            continue;
        }

        /* Accepted OpenSSL QUIC connections share the listener's UDP handle.
         * Poll each SSL object explicitly after servicing that handle. */
    }

    player *pl, *pl_tmp;
    DL_FOREACH_SAFE(first_player, pl, pl_tmp) {
        if (pl->cs->state == ST_DEAD) {
            player_logout(pl);
            continue;
        }

        if (unlikely(!socket_is_fd_valid(pl->cs->sc))) {
            LOG(ERROR, "Invalid waiting socket: %s", socket_get_id(pl->cs->sc));
            pl->cs->state = ST_DEAD;
        }

        if (pl->cs->state == ST_DEAD) {
            player_logout(pl);
            continue;
        }

        if (server_socket_csocket_is_zombie(pl->cs)) {
            continue;
        }
    }

    uint64_t pending_queue_age_us = socket_server_pending_queue_age_us();
    uint64_t wait_timeout_us = transport_first_pass ? 0 : socket_server_transport_timeout_us();
    bool application_wake_requested =
        application_wakeup_armed && socket_server_pending_application();
    uint64_t wait_started_us = datetime_monotonic_us();
    struct timeval timeout = {
        .tv_sec = (long)(wait_timeout_us / UINT64_C(1000000)),
        .tv_usec = (long)(wait_timeout_us % UINT64_C(1000000)),
    };
    int ready;
#ifdef HAVE_PSELECT
    struct timespec pselect_timeout = {
        .tv_sec = timeout.tv_sec,
        .tv_nsec = timeout.tv_usec * 1000L,
    };
    ready = pselect(nfds + 1, &fds_read, NULL, NULL, &pselect_timeout, NULL);
#else
    ready = select(nfds + 1, &fds_read, NULL, NULL, &timeout);
#endif
    bool wait_error = ready == -1;
    if (unlikely(ready == -1)) {
        LOG(ERROR, "pselect/select() returned an error: %s (%d)", strerror(errno), errno);
        FD_ZERO(&fds_read);
    }

    bool listener_ready = false;
    for (size_t i = 0; i < arraysize(quic_server_sockets); i++) {
        if (quic_server_sockets[i] != NULL && ready >= 0 &&
            FD_ISSET(socket_fd(quic_server_sockets[i]), &fds_read)) {
            listener_ready = true;
            if (socket_server_quic_punch_receive(quic_server_sockets[i])) {
                continue;
            }
            socket_server_csocket_create(quic_server_sockets[i]);
        }
    }

    socket_server_transport_stats_t stats = {0};
    socket_server_service_client_connections(&stats);
    socket_server_service_player_connections(&stats);

    bool simulation_due = transport_first_pass || sleep_delta_timeout_us() == 0;
    transport_first_pass = false;
    bool pending_application = socket_server_pending_application();
    if (!pending_application) {
        application_wakeup_armed = true;
    } else if (application_wake_requested) {
        application_wakeup_armed = false;
    }

    server_transport_wake_reason_t reason;
    if (wait_error) {
        reason = SERVER_TRANSPORT_WAKE_ERROR;
    } else if (listener_ready) {
        reason = SERVER_TRANSPORT_WAKE_LISTENER;
    } else if (stats.quic_timer_services != 0) {
        reason = SERVER_TRANSPORT_WAKE_QUIC_TIMER;
    } else if (stats.ready_connections != 0) {
        reason = SERVER_TRANSPORT_WAKE_CONNECTION;
    } else if (application_wake_requested) {
        reason = SERVER_TRANSPORT_WAKE_APPLICATION;
    } else if (simulation_due) {
        reason = SERVER_TRANSPORT_WAKE_SIMULATION;
    } else {
        reason = SERVER_TRANSPORT_WAKE_CONNECTION;
    }
    uint64_t waited_us = datetime_monotonic_us() - wait_started_us;
    pending_queue_age_us = MAX(pending_queue_age_us, socket_server_pending_queue_age_us());
    server_metrics_transport_wait(waited_us,
                                  reason,
                                  stats.ready_connections,
                                  stats.quic_timer_services,
                                  pending_queue_age_us,
                                  stats.work_limited);
    return simulation_due;
}

/**
 * Update player socket-related data, render the map for them, etc.
 * Afterwards, attempt to write to the players' clients.
 */
void socket_server_post_process(void) {
    csocket_entry_t *entry, *entry_tmp;
    DL_FOREACH_SAFE(client_sockets, entry, entry_tmp) {
        if (server_socket_csocket_is_zombie(entry->cs) &&
            entry->cs->login_count++ >= MAX_TICKS_MULTIPLIER) {
            entry->cs->state = ST_DEAD;
        }
        if (entry->cs->state == ST_DEAD) {
            socket_server_csocket_drop(entry);
        }
    }

    player *pl, *pl_tmp;
    DL_FOREACH_SAFE(first_player, pl, pl_tmp) {
        if (server_socket_csocket_is_zombie(pl->cs) &&
            pl->cs->login_count++ >= MAX_TICKS_MULTIPLIER) {
            pl->cs->state = ST_DEAD;
        }
        if (pl->cs->state == ST_DEAD) {
            player_logout(pl);
            continue;
        }

        if (pl->cs->keepalive++ >= SOCKET_KEEPALIVE_TIMEOUT) {
            LOG(SYSTEM,
                "Keepalive: disconnecting %s [%s]: %d",
                object_get_str(pl->ob),
                socket_get_id(pl->cs->sc),
                socket_fd(pl->cs->sc));
            pl->cs->state = ST_DEAD;
            player_logout(pl);
            continue;
        }

        /* The removal of ext_title_flag is done in two steps because we might
         * be somewhere in the middle of the loop right now, which would mean
         * that the previous players in the list would not get the update. */
        if (pl->cs->ext_title_flag == 1) {
            generate_quick_name(pl);
            pl->cs->ext_title_flag = 2;
        } else if (pl->cs->ext_title_flag == 2) {
            pl->cs->ext_title_flag = 0;
        }

        esrv_update_stats(pl);
        party_update_who(pl);

        if (pl->ob->map != NULL) {
            draw_client_map(pl->ob);

            uint32_t update_tile = GET_MAP_UPDATE_COUNTER(pl->ob->map, pl->ob->x, pl->ob->y);
            if (update_tile != pl->cs->update_tile) {
                esrv_draw_look(pl->ob);
                pl->cs->update_tile = update_tile;
            }
        }

        socket_buffer_write(pl->cs);
    }

    /* The pass above can enqueue map/asset responses. Permit one immediate
     * transport retry, but let the transport poller back off if a socket
     * remains unable to accept application data. */
    application_wakeup_armed = true;
}
