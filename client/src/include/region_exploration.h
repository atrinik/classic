/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/
#ifndef REGION_EXPLORATION_H
#define REGION_EXPLORATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Main-thread session cache. Returned snapshots are borrowed until the next
 * successful packet or clear. Invalid packets leave all snapshots unchanged. */
bool region_exploration_receive(const uint8_t *data, size_t len);
void region_exploration_clear(void);
const uint8_t *region_exploration_find(const char *path, unsigned *width, unsigned *height);
#endif
