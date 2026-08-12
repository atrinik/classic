#include <active_effects_model.h>

const player_status_t *active_effects_model_rows(void) {
    return player_status_model.head;
}

const player_status_t *active_effects_model_find(const char *key) {
    return player_status_model_find(&player_status_model, key);
}

size_t active_effects_model_count(void) {
    return player_status_model.count;
}

bool active_effects_model_tick(uint32_t seconds) {
    return player_status_model_tick(&player_status_model, seconds);
}
