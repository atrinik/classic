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
/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EXPLORATION_H
#define EXPLORATION_H

#include <decls.h>

/* Main simulation thread only. Store keys come from authenticated sockets;
 * only draw_client_map2 may grant discoveries. Client cache reconciliation never
 * mutates the authoritative account bitfields. */
void exploration_begin(socket_struct *ns);
bool exploration_end_checked(socket_struct *ns);
void exploration_end(socket_struct *ns);
bool exploration_shutdown_checked(void);
void exploration_shutdown(void);
bool exploration_mark(socket_struct *ns,
                      const char *path,
                      unsigned width,
                      unsigned height,
                      unsigned x,
                      unsigned y);
void exploration_flush(socket_struct *ns, bool force);
#ifdef ATRINIK_TESTING
typedef struct exploration_test_stats {
    uint64_t account_lookups, map_lookups, broadcast_sessions, pending_visits;
    uint64_t sent_records, save_records, save_bytes, load_records;
} exploration_test_stats;
void exploration_stats_reset(void);
exploration_test_stats exploration_stats_get(void);
bool exploration_visited(socket_struct *ns, const char *path, unsigned x, unsigned y);
#endif
#endif
