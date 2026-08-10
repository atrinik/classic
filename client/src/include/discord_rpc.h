#ifndef DISCORD_RPC_H
#define DISCORD_RPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISCORD_RPC_TEXT_BYTES 513

typedef struct discord_rpc discord_rpc_t;

typedef struct discord_rpc_activity {
    char details[DISCORD_RPC_TEXT_BYTES];
    char state[DISCORD_RPC_TEXT_BYTES];
    uint64_t started_at;
} discord_rpc_activity_t;

typedef struct discord_rpc_io {
    int (*connect)(void *context);
    ptrdiff_t (*read)(void *context, void *buffer, size_t size);
    ptrdiff_t (*write)(void *context, const void *buffer, size_t size);
    void (*close)(void *context);
} discord_rpc_io_t;

discord_rpc_t *discord_rpc_create(const char *application_id);
discord_rpc_t *discord_rpc_create_with_io(const char *application_id,
                                          const discord_rpc_io_t *io,
                                          void *io_context);
void discord_rpc_destroy(discord_rpc_t *rpc, uint64_t now_ms);
void discord_rpc_pump(discord_rpc_t *rpc, uint64_t now_ms);
void discord_rpc_set_activity(discord_rpc_t *rpc, const discord_rpc_activity_t *activity);
void discord_rpc_clear_activity(discord_rpc_t *rpc);
bool discord_rpc_ready(const discord_rpc_t *rpc);

#endif
