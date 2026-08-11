#ifndef ACTIVE_EFFECTS_MODEL_H
#define ACTIVE_EFFECTS_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <player_status.h>

const player_status_t *active_effects_model_rows(void);
const player_status_t *active_effects_model_find(const char *key);
size_t active_effects_model_count(void);
bool active_effects_model_tick(uint32_t seconds);

#endif
