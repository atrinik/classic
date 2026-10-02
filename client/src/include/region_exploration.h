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

/* Main-thread-only, bounded account cache. Invalid packets never mutate it.
 * Connection identity must be the certificate pinned by the actual connection.
 * Optional cache_directory is an existing private directory; NULL disables disk.
 * Only server-received bits enter the cache. Disconnect flushes dirty data once
 * and hides retained memory until RESET authenticates the matching account. */
void region_exploration_connect(const char *certificate, const char *cache_directory);
void region_exploration_disconnect(void);
void region_exploration_clear(void);
bool region_exploration_receive(const uint8_t *data, size_t len, bool *changed);
/* Account bitmap allocations total at most 1 MiB (10,000 normal 24x24 maps
 * use 720,000 bytes). Over-limit new records are rejected transactionally.
 * RESET is idempotent while active; disconnect permits a new account. */
uint64_t region_exploration_revision(void);
typedef void (*region_exploration_visit_fn)(unsigned x, unsigned y, void *user);
/* Resume a record's set bits using a caller-owned byte cursor and shared work
 * budget. True means complete; unchanged records cost no byte/cell work.
 * The caller charges map lookup work and resets the cursor for the next map. */
bool region_exploration_replay(const char *path, uint64_t since, size_t *cursor,
                               size_t *budget, region_exploration_visit_fn visit, void *user);
/* Borrowed storage, valid until clear or an account change. */
const uint8_t *region_exploration_find(const char *path, unsigned *width, unsigned *height);
/* Deduplicates requests for the session; retry queue is bounded to 10000 paths. */
bool region_exploration_request(const char *path);
typedef bool (*region_exploration_send_fn)(const uint8_t *data, size_t len, void *user);
/* At most 32 requests; a rejected send retains the pending request. */
size_t region_exploration_service(region_exploration_send_fn send, void *user);
#endif
