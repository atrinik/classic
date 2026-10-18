/*************************************************************************
 * Atrinik client protocol-1085 celestial inputs.                        *
 *                                                                       *
 * Copyright 2026 The Atrinik Project                                   *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Region celestial inputs received through CELESTIAL, with the sun, moon, sky
 * and starlight sources the shared celestial model derives from them for the
 * current and next hour, and the celestial closure received through
 * CELESTIAL_STRUCTURE, which the client solves with the shared solver
 * (server/doc/CLIENT_CELESTIAL.md). The two payloads are one publication:
 * each is staged when it arrives, and both replace the published pair
 * together after the MAP2 update that follows them applies. State is owned
 * by the main thread and reset with every connection.
 */

#ifndef CLIENT_CELESTIAL_H
#define CLIENT_CELESTIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <toolkit/celestial_closure.h>
#include <toolkit/celestial_model.h>

/** Forget every slot and the last generation; called for every connection. */
void client_celestial_reset(void);

/**
 * Stage a complete CELESTIAL payload for the next publication.
 * @return False for a stale payload, one whose generation does not advance
 * past the published pair or a staged one (a duplicate never replaces what
 * was staged first), keeping every state; and for a malformed payload, which
 * also discards the staged pair.
 */
bool client_celestial_apply(const uint8_t *data, size_t len);

/**
 * Stage a CELESTIAL_STRUCTURE payload for a MAP2 window of the given size.
 * @return False for a stale payload, keeping every state, and for a
 * malformed one, which also discards the staged pair.
 */
bool client_celestial_apply_structure(const uint8_t *data, size_t len, int window_width,
                                      int window_height);

/** Receives one closure cell's solved field for the current and next hour:
 * value and RGB, at a viewer-relative depth and MAP2 window coordinate. */
typedef void (*client_celestial_cell_fn)(void *context, int depth, int x, int y,
                                         const int32_t current[4], const int32_t next[4]);

/**
 * Publish the staged pair after the MAP2 update that follows it applied:
 * only both payloads with the same generation replace the published ones.
 * Anything else staged is discarded, since each half belongs only to the
 * update right after it.
 * Every cell of the new closure is then solved by client_celestial_process()
 * and reported through cell; a newer publication supersedes the rest.
 * @return Whether a pair was published.
 */
bool client_celestial_publish(client_celestial_cell_fn cell, void *context);

/**
 * Solve fragments of the published closure until the budget is spent; a
 * fragment is solved whole, so the first always completes.
 * @return Whether every fragment is solved.
 */
bool client_celestial_process(uint64_t budget_us);

/** Drop staged payloads, as when the MAP2 update that follows fails. */
void client_celestial_discard_staged(void);

/** The published closure. */
const celestial_closure_t *client_celestial_closure(void);

/**
 * Sources of one slot for the current hour, or for the next one.
 * @return False if the slot is not assigned.
 */
bool client_celestial_model(uint8_t slot, bool next, celestial_model_t *model);

/** Start of the current hour in game seconds, as last received. */
uint64_t client_celestial_start_seconds(void);

/** Generation of the published pair; 0 before the first one. */
uint32_t client_celestial_generation(void);

/** Revision that changes whenever the received inputs change. */
uint64_t client_celestial_revision(void);

#endif
