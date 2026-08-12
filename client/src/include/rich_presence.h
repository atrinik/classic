#ifndef RICH_PRESENCE_H
#define RICH_PRESENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <discord_rpc.h>

enum {
    RICH_PRESENCE_OFF,
    RICH_PRESENCE_GAME,
    RICH_PRESENCE_SERVER,
    RICH_PRESENCE_SERVER_ZONE,
    RICH_PRESENCE_SERVER_ZONE_CHARACTER
};

typedef struct rich_presence_input {
    bool playing;
    int privacy;
    bool public_server;
    const char *server;
    const char *zone;
    const char *character;
    uint32_t level;
    bool character_available;
    uint64_t now_ms;
    uint64_t now_unix;
} rich_presence_input_t;

typedef struct rich_presence_backend {
    void *context;
    void (*publish)(void *context, const discord_rpc_activity_t *activity);
    void (*clear)(void *context);
} rich_presence_backend_t;

typedef struct rich_presence_controller {
    bool was_playing;
    bool owns_activity;
    bool pending;
    int last_privacy;
    uint64_t session_started_at;
    uint64_t next_publish_ms;
    uint64_t command_times[5];
    size_t command_start;
    size_t command_count;
    discord_rpc_activity_t published;
    discord_rpc_activity_t pending_activity;
} rich_presence_controller_t;

bool rich_presence_application_id_valid(const char *value);
bool rich_presence_normalize(const char *value, char output[DISCORD_RPC_TEXT_BYTES]);
void rich_presence_controller_init(rich_presence_controller_t *controller);
void rich_presence_controller_tick(rich_presence_controller_t *controller,
                                   const rich_presence_input_t *input,
                                   const rich_presence_backend_t *backend);
void rich_presence_controller_begin_session(rich_presence_controller_t *controller,
                                            const rich_presence_backend_t *backend,
                                            uint64_t now_ms);
void rich_presence_controller_stop(rich_presence_controller_t *controller,
                                   const rich_presence_backend_t *backend,
                                   uint64_t now_ms);

void rich_presence_init(void);
void rich_presence_tick(void);
void rich_presence_deinit(void);
void rich_presence_session_start(void);
void rich_presence_zone_changed(void);

#endif
