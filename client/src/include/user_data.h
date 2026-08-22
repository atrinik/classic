/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                 *
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
 * Stable per-major client user-data paths and legacy migration.
 */

#ifndef USER_DATA_H
#define USER_DATA_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Prepare and return the stable user-data directory for a major release.
 *
 * If the stable directory does not exist, the highest valid same-major legacy
 * directory is migrated into it. Migration is collision-safe and leaves a
 * recovery marker and all conflicting or unsupported source entries in place
 * when it cannot finish.
 *
 * @param config_root
 * Platform-specific configuration root.
 * @param major
 * Client major release number.
 * @param path
 * Destination buffer for the stable directory path.
 * @param path_size
 * Size of @p path.
 * @return
 * True when the stable directory can be used, false when its parent or final
 * component is unsafe or cannot be prepared.
 */
extern bool
user_data_prepare(const char *config_root, unsigned int major, char *path, size_t path_size);

#endif
