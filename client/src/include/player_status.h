#ifndef PLAYER_STATUS_H
#define PLAYER_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <atrinik/protocol/game_commands.h>

typedef struct player_status {
    struct player_status *next;
    char key[ATRINIK_PLAYER_STATUS_KEY_SIZE + 1U];
    uint16_t face;
    char name[ATRINIK_PLAYER_STATUS_NAME_SIZE + 1U];
    char tooltip[ATRINIK_PLAYER_STATUS_TOOLTIP_SIZE + 1U];
    int32_t seconds;
} player_status_t;

typedef struct player_status_model {
    player_status_t *head;
    size_t count;
} player_status_model_t;

extern player_status_model_t player_status_model;

void player_status_model_clear(player_status_model_t *model);
const player_status_t *player_status_model_find(const player_status_model_t *model,
                                                const char *key);
bool player_status_model_tick(player_status_model_t *model, uint32_t seconds);
bool player_status_parse_command(player_status_model_t *model,
                                 const uint8_t *data,
                                 size_t len,
                                 size_t pos);

#endif
