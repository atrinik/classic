#ifndef PLAYER_STATUS_H
#define PLAYER_STATUS_H

#include <stdbool.h>

bool player_status_should_publish(const object *op);
bool player_status_set(object *op,
                       const char *key,
                       const char *name,
                       const char *tooltip,
                       New_Face *face);
bool player_status_set_from_source(object *op, const object *source, const char *key_namespace);
void player_status_update(object *op);
void player_status_remove(object *op);
void player_status_update_paralysis(object *pl);
void player_status_send_snapshot(object *pl);

#endif
