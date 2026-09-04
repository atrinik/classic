/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
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

/**
 * @file
 * Windows path security helpers.
 */

#include "path.h"

#ifdef WIN32
#include <aclapi.h>

/**
 * Opens the current-process token for path secret security checks.
 *
 * This is a separate translation unit so Windows tests can wrap token
 * acquisition without relying on import-library symbol names.
 */
TOKEN_USER *path_windows_token_user(HANDLE *token) {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token)) {
        return NULL;
    }
    DWORD size = 0;
    GetTokenInformation(*token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = size != 0 ? malloc(size) : NULL;
    if (user == NULL || !GetTokenInformation(*token, TokenUser, user, size, &size)) {
        free(user);
        CloseHandle(*token);
        *token = NULL;
        return NULL;
    }
    return user;
}
#endif
