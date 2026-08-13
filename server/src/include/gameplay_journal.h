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

/** @file Durable, private gameplay audit journal. */

#ifndef GAMEPLAY_JOURNAL_H
#define GAMEPLAY_JOURNAL_H

#include <decls.h>

#define GAMEPLAY_JOURNAL_SCHEMA_VERSION 1
#define GAMEPLAY_JOURNAL_ID_MAX 255
#define GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE 33

typedef enum gameplay_journal_kind {
    GAMEPLAY_JOURNAL_ITEM,
    GAMEPLAY_JOURNAL_CURRENCY,
    GAMEPLAY_JOURNAL_QUEST,
    GAMEPLAY_JOURNAL_PROGRESSION
} gameplay_journal_kind_t;

typedef struct gameplay_journal_profile {
    const char *id;
    uint32_t schema;
    const char *digest;
    const char *effective_axes;
} gameplay_journal_profile_t;

typedef struct gameplay_journal_subject {
    const char *account_id;
    const char *character_id;
    const char *map_id;
    int32_t x;
    int32_t y;
} gameplay_journal_subject_t;

typedef struct gameplay_journal_change {
    const char *subject_id;
    const char *lineage_id;
    int64_t before;
    int64_t delta;
    int64_t after;
} gameplay_journal_change_t;

/**
 * Start the process journal in an existing private server data directory.
 * The API is single-threaded and must be initialized before gameplay
 * producers can create transactions.
 */
bool gameplay_journal_init(const char *datapath,
                           const char *server_id,
                           const gameplay_journal_profile_t *profile);

/** Close the current journal without deleting retained records. */
void gameplay_journal_deinit(void);

/** Whether journal-backed gameplay mutations may currently start. */
bool gameplay_journal_available(void);

/**
 * Durably record an intent before its gameplay mutation.
 * On success, transaction_id contains a globally unique lowercase hex ID.
 */
bool gameplay_journal_begin(const gameplay_journal_subject_t *subject,
                            gameplay_journal_kind_t kind,
                            const char *reason,
                            const gameplay_journal_change_t *change,
                            char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]);

/** Durably mark a previously written intent as committed. */
bool gameplay_journal_commit(const char *transaction_id);

/** Durably mark a vetoed or rolled-back intent as aborted. */
bool gameplay_journal_abort(const char *transaction_id, const char *reason);

/** Record an immutable profile fork or migration boundary. */
bool gameplay_journal_profile_boundary(const gameplay_journal_profile_t *profile,
                                       const char *reason);

/** Trusted-plugin adapter deriving stable account, character, and map context. */
bool gameplay_journal_player_begin(player *pl,
                                   const char *kind,
                                   const char *reason,
                                   const char *subject_id,
                                   const char *lineage_id,
                                   int64_t before,
                                   int64_t delta,
                                   int64_t after,
                                   char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]);

#ifdef ATRINIK_TESTING
/** Unit-test seam for the fail-stop write policy; unavailable in release builds. */
void gameplay_journal_fail_writes_for_test(bool fail);
void gameplay_journal_file_limit_for_test(size_t limit);
void gameplay_journal_hard_limit_for_test(size_t limit);
#endif

#endif
