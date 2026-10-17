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
 * current and next hour (server/doc/CLIENT_CELESTIAL.md). State is owned by
 * the main thread and reset with every connection.
 */

#ifndef CLIENT_CELESTIAL_H
#define CLIENT_CELESTIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <toolkit/celestial_model.h>

/** Forget every slot and the last generation; called for every connection. */
void client_celestial_reset(void);

/**
 * Replace the slot table with a complete CELESTIAL payload.
 * @return False, keeping the previous state, for a malformed payload or one
 * whose generation does not advance past the last applied one.
 */
bool client_celestial_apply(const uint8_t *data, size_t len);

/**
 * Sources of one slot for the current hour, or for the next one.
 * @return False if the slot is not assigned.
 */
bool client_celestial_model(uint8_t slot, bool next, celestial_model_t *model);

/** Start of the current hour in game seconds, as last received. */
uint64_t client_celestial_start_seconds(void);

/** Generation of the last applied payload; 0 before the first one. */
uint32_t client_celestial_generation(void);

/** Revision that changes whenever the received inputs change. */
uint64_t client_celestial_revision(void);

#endif
