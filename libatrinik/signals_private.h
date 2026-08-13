/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

#ifndef SIGNALS_PRIVATE_H
#define SIGNALS_PRIVATE_H

#ifdef WIN32
#include <stdbool.h>
#include <windows.h>

typedef struct signals_exception_claim {
    DWORD code;
    bool acquired;
} signals_exception_claim;

/**
 * Atomically claim Windows exception reporting while retaining the first code.
 * @param stored_code
 * Shared first-exception storage.
 * @param candidate_code
 * Exception code observed by this handler invocation.
 * @return
 * Whether this invocation acquired reporting and the code to terminate with.
 */
static inline signals_exception_claim signals_claim_exception(
    volatile LONG *stored_code,
    DWORD candidate_code) {
    LONG previous_code = InterlockedCompareExchange(stored_code,
                                                    (LONG)candidate_code,
                                                    0);
    signals_exception_claim claim = {
        .code = previous_code == 0 ? candidate_code : (DWORD)previous_code,
        .acquired = previous_code == 0,
    };

    return claim;
}
#endif

#endif
