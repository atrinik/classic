/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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

#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <decls.h>

/**
 * @file
 * Public declarations for the corresponding server module.
 */

/** Public API implemented in src/server/account.c. */

extern void account_init(void);

extern void account_deinit(void);

extern char *account_make_path(const char *name);

/**
 * Provision one account and first-login character in a stopped local state.
 * Existing account or player files are never replaced. Newly reserved files
 * are rolled back on failure.
 *
 * @param name Account name.
 * @param password Plaintext password, consumed synchronously and not retained.
 * @param character Character name.
 * @param archname Player archetype name.
 * @param error Output buffer for a non-secret failure description.
 * @param error_size Size of error.
 * @return True on success, false on failure.
 */
extern bool account_provision(const char *name,
                              const char *password,
                              const char *character,
                              const char *archname,
                              char *error,
                              size_t error_size);

/**
 * Read a plaintext password from a protected file and call
 * account_provision(). The file must be regular, non-symlink, owned by the
 * current user, and mode 0600. It may have no terminator or one LF/CRLF.
 *
 * @see account_provision
 */
extern bool account_provision_from_file(const char *name,
                                        const char *password_file,
                                        const char *character,
                                        const char *archname,
                                        char *error,
                                        size_t error_size);

extern void account_login(socket_struct *ns, char *name, char *password);

extern void account_register(socket_struct *ns, char *name, char *password, char *password2);

extern void account_new_char(socket_struct *ns, char *name, char *archname);

extern void account_login_char(socket_struct *ns, char *name);

extern void account_logout_char(socket_struct *ns, player *pl);

extern void
account_password_change(socket_struct *ns, char *password, char *password_new, char *password_new2);

extern void account_password_force(object *op, char *name, const char *password);

#endif
