#ifndef PLAYER_STATUS_H
#define PLAYER_STATUS_H

#include <stdbool.h>

bool player_status_should_publish(const object *op);
void player_status_update(object *op);
void player_status_remove(object *op);
void player_status_send_snapshot(object *pl);

#endif
