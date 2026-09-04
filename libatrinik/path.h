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
 * Path API header file.
 *
 * @author Zoey Rose
 */

#ifndef TOOLKIT_PATH_H
#define TOOLKIT_PATH_H

#include "toolkit.h"

/**
 * Prototype for the ::path_fopen function signature.
 *
 * @param filename
 * Filename.
 * @param modes
 * Modes to open the file in.
 */
typedef FILE *(*path_fopen_t)(const char *filename, const char *modes);

typedef enum path_secret_error {
    PATH_SECRET_OK,
    PATH_SECRET_NOT_FOUND,
    PATH_SECRET_OPEN_ERROR,
    PATH_SECRET_METADATA_ERROR,
    PATH_SECRET_NOT_REGULAR,
    PATH_SECRET_WRONG_OWNER,
    PATH_SECRET_UNSAFE_LINK,
    PATH_SECRET_EMPTY,
    PATH_SECRET_TOO_LONG,
    PATH_SECRET_TRAILING_DATA,
    PATH_SECRET_INVALID_DATA,
    PATH_SECRET_READ_ERROR
} path_secret_error_t;

/** Result of create-only secret publication. */
typedef enum path_secret_create_result {
    PATH_SECRET_CREATE_OK,
    PATH_SECRET_CREATE_EXISTS,
    PATH_SECRET_CREATE_ERROR
} path_secret_create_result_t;

/** Result of preparing one final directory path component. */
typedef enum path_directory_result {
    PATH_DIRECTORY_OK,
    PATH_DIRECTORY_UNSAFE,
    PATH_DIRECTORY_ERROR
} path_directory_result_t;

/* Prototypes */

extern path_fopen_t path_fopen;

TOOLKIT_FUNCS_DECLARE(path);

char *path_join(const char *path, const char *path2);
char *path_dirname(const char *path);
char *path_basename(const char *path);
char *path_normalize(const char *path);
void path_ensure_directories(const char *path);
int path_copy_file(const char *src, FILE *dst, const char *mode);
int path_exists(const char *path);
int path_touch(const char *path);
size_t path_size(const char *path);
char *path_file_contents(const char *path);
int path_rename(const char *old, const char *new);
bool path_write_atomic(const char *path, const void *data, size_t size, unsigned int mode);
/** Atomically replace a file whose parent directories are already present. */
bool path_write_atomic_existing(const char *path, const void *data, size_t size, unsigned int mode);
/**
 * Ensure the final path component is a direct directory, creating it if absent.
 *
 * Existing symbolic links, Windows reparse points, and non-directories are
 * rejected. This function does not validate ancestor components, ownership, or
 * permissions and retains no handle after returning. Concurrent calls are
 * supported; callers must separately control later name-based access.
 */
path_directory_result_t path_ensure_real_directory(const char *path, unsigned int mode);
/**
 * Atomically publish a new owner-only secret without replacing an existing path.
 *
 * The function borrows `path` and `data` for the call and retains no copy. The
 * final name is absent until the complete contents have been flushed and is
 * never overwritten. EXISTS leaves the existing path untouched; ERROR can be
 * returned after an otherwise safe final publication when durability cannot be
 * confirmed, so callers must not delete the final path. Calls for the same path
 * may run concurrently and resolve through OK/EXISTS. The caller owns and must
 * cleanse sensitive input storage.
 */
path_secret_create_result_t
path_secret_create_atomic(const char *path, const void *data, size_t size);
/**
 * Read the first line of a strict secret file through one verified file handle.
 *
 * `secret` is caller-owned and is always cleared before use and again on every
 * error. Only an optional LF or CRLF terminator may follow the secret; later
 * bytes and embedded NUL bytes are rejected. A successful read NUL-terminates it.
 * `permissive_mode`, when non-NULL,
 * reports an otherwise readable file whose POSIX mode or Windows DACL grants
 * broader access; policy-sensitive callers should reject that result. Link,
 * reparse, owner, regular-file, length, and trailing-data failures are distinct.
 * The function retains no state and independent calls are thread-safe.
 */
path_secret_error_t
path_read_secret(const char *path, char *secret, size_t secret_size, bool *permissive_mode);
const char *path_secret_error_string(path_secret_error_t error);
bool path_is_safe_relative(const char *path);

#endif
