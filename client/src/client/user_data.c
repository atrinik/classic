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

#include <errno.h>

#include <toolkit/logger.h>
#include <toolkit/path.h>

#include <user_data.h>

#define USER_DATA_DIRECTORY ".atrinik"
#define USER_DATA_MIGRATION_MARKER_PREFIX "."
#define USER_DATA_MIGRATION_MARKER_SUFFIX ".x.migration"
#define USER_DATA_MAX_MIGRATION_DEPTH 128U

typedef enum user_data_path_kind {
    USER_DATA_PATH_MISSING,
    USER_DATA_PATH_DIRECTORY,
    USER_DATA_PATH_REGULAR,
    USER_DATA_PATH_UNSAFE,
    USER_DATA_PATH_OTHER,
    USER_DATA_PATH_ERROR,
} user_data_path_kind_t;

typedef struct user_data_candidate {
    char name[HUGE_BUF];
    uint64_t minor;
    uint64_t patch;
    bool has_patch;
    bool suffixed;
} user_data_candidate_t;

#ifdef WIN32
static wchar_t *user_data_windows_wide(const char *path) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }

    wchar_t *wide = calloc((size_t)length, sizeof(*wide));
    if (wide == NULL ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length) != length) {
        free(wide);
        return NULL;
    }

    for (wchar_t *cp = wide; *cp != L'\0'; cp++) {
        if (*cp == L'/') {
            *cp = L'\\';
        }
    }

    return wide;
}
#endif

static user_data_path_kind_t user_data_inspect_path(const char *path) {
#ifdef WIN32
    wchar_t *wide = user_data_windows_wide(path);
    if (wide == NULL) {
        return USER_DATA_PATH_ERROR;
    }

    HANDLE file = CreateFileW(wide,
                              0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? USER_DATA_PATH_MISSING
                   : USER_DATA_PATH_ERROR;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes;
    FILE_STANDARD_INFO standard;
    user_data_path_kind_t result = USER_DATA_PATH_ERROR;
    if (GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result = USER_DATA_PATH_UNSAFE;
        } else if (standard.Directory) {
            result = USER_DATA_PATH_DIRECTORY;
        } else if (GetFileType(file) == FILE_TYPE_DISK) {
            result = USER_DATA_PATH_REGULAR;
        } else {
            result = USER_DATA_PATH_OTHER;
        }
    }

    CloseHandle(file);
    return result;
#else
    struct stat metadata;
    if (lstat(path, &metadata) != 0) {
        return errno == ENOENT ? USER_DATA_PATH_MISSING : USER_DATA_PATH_ERROR;
    }
    if (S_ISLNK(metadata.st_mode)) {
        return USER_DATA_PATH_UNSAFE;
    }
    if (S_ISDIR(metadata.st_mode)) {
        return USER_DATA_PATH_DIRECTORY;
    }
    if (S_ISREG(metadata.st_mode)) {
        return USER_DATA_PATH_REGULAR;
    }
    return USER_DATA_PATH_OTHER;
#endif
}

static bool
user_data_format_path(char *path, size_t path_size, const char *parent, const char *name) {
    return snprintf(path, path_size, "%s/%s", parent, name) < (int)path_size;
}

static bool user_data_remove_file(const char *path) {
#ifdef WIN32
    wchar_t *wide = user_data_windows_wide(path);
    bool removed = wide != NULL && DeleteFileW(wide) != 0;
    free(wide);
    return removed;
#else
    return unlink(path) == 0;
#endif
}

static bool user_data_remove_directory(const char *path) {
#ifdef WIN32
    wchar_t *wide = user_data_windows_wide(path);
    bool removed = wide != NULL && RemoveDirectoryW(wide) != 0;
    free(wide);
    return removed;
#else
    return rmdir(path) == 0;
#endif
}

static bool user_data_parse_decimal(const char **cursor, uint64_t *value) {
    const char *current = *cursor;
    uint64_t result = 0;

    if (!isdigit((unsigned char)*current)) {
        return false;
    }

    do {
        unsigned int digit = (unsigned int)(*current - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
        current++;
    } while (isdigit((unsigned char)*current));

    *cursor = current;
    *value = result;
    return true;
}

static bool
user_data_parse_candidate(const char *name, unsigned int major, user_data_candidate_t *candidate) {
    size_t length = strlen(name);
    const char *cursor = name;
    uint64_t parsed_major;

    if (length == 0 || length >= sizeof(candidate->name)) {
        return false;
    }

    candidate->suffixed = name[length - 1] == '~';
    if (candidate->suffixed) {
        if (length == 1) {
            return false;
        }
        length--;
    }

    if (!user_data_parse_decimal(&cursor, &parsed_major) || parsed_major != major ||
        *cursor != '.') {
        return false;
    }
    cursor++;
    if (!user_data_parse_decimal(&cursor, &candidate->minor)) {
        return false;
    }

    candidate->has_patch = false;
    candidate->patch = 0;
    if (*cursor == '.') {
        cursor++;
        if (!user_data_parse_decimal(&cursor, &candidate->patch)) {
            return false;
        }
        candidate->has_patch = true;
    }

    if (cursor != name + length) {
        return false;
    }

    memcpy(candidate->name, name, length);
    candidate->name[length] = candidate->suffixed ? '~' : '\0';
    return true;
}

static bool user_data_candidate_is_newer(const user_data_candidate_t *candidate,
                                         const user_data_candidate_t *current) {
    if (candidate->minor != current->minor) {
        return candidate->minor > current->minor;
    }
    if (candidate->patch != current->patch) {
        return candidate->patch > current->patch;
    }
    if (candidate->suffixed != current->suffixed) {
        return !candidate->suffixed;
    }
    if (candidate->has_patch != current->has_patch) {
        return !candidate->has_patch;
    }
    return strcmp(candidate->name, current->name) > 0;
}

static bool user_data_find_candidate(const char *root,
                                     unsigned int major,
                                     user_data_candidate_t *candidate,
                                     bool *found) {
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return false;
    }

    *found = false;
    struct dirent *entry;
    bool read_error = false;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            read_error = errno != 0;
            break;
        }
        user_data_candidate_t current;
        char path[HUGE_BUF];

        if (!user_data_parse_candidate(entry->d_name, major, &current) ||
            !user_data_format_path(path, sizeof(path), root, entry->d_name) ||
            user_data_inspect_path(path) != USER_DATA_PATH_DIRECTORY) {
            continue;
        }

        if (!*found || user_data_candidate_is_newer(&current, candidate)) {
            *candidate = current;
            *found = true;
        }
    }

    bool complete = !read_error;
    if (closedir(directory) != 0) {
        complete = false;
    }
    return complete;
}

static bool user_data_same_file(const char *first, const char *second) {
#ifdef WIN32
    (void)first;
    (void)second;
    return false;
#else
    struct stat first_metadata;
    struct stat second_metadata;
    return lstat(first, &first_metadata) == 0 && lstat(second, &second_metadata) == 0 &&
           S_ISREG(first_metadata.st_mode) && S_ISREG(second_metadata.st_mode) &&
           first_metadata.st_dev == second_metadata.st_dev &&
           first_metadata.st_ino == second_metadata.st_ino;
#endif
}

static bool user_data_move_file(const char *source, const char *target) {
    user_data_path_kind_t target_kind = user_data_inspect_path(target);
    if (target_kind != USER_DATA_PATH_MISSING) {
        if (target_kind == USER_DATA_PATH_REGULAR && user_data_same_file(source, target)) {
            return user_data_remove_file(source) ||
                   user_data_inspect_path(source) == USER_DATA_PATH_MISSING;
        }
        return false;
    }

#ifdef WIN32
    wchar_t *source_wide = user_data_windows_wide(source);
    wchar_t *target_wide = user_data_windows_wide(target);
    bool moved = source_wide != NULL && target_wide != NULL &&
                 MoveFileExW(source_wide, target_wide, MOVEFILE_WRITE_THROUGH) != 0;
    free(source_wide);
    free(target_wide);
    if (moved) {
        return true;
    }

    return user_data_inspect_path(source) == USER_DATA_PATH_MISSING &&
           user_data_inspect_path(target) == USER_DATA_PATH_REGULAR;
#else
    if (link(source, target) != 0) {
        return errno == EEXIST && user_data_same_file(source, target) &&
               (user_data_remove_file(source) ||
                user_data_inspect_path(source) == USER_DATA_PATH_MISSING);
    }

    if (!user_data_same_file(source, target)) {
        return false;
    }

    return user_data_remove_file(source) ||
           user_data_inspect_path(source) == USER_DATA_PATH_MISSING;
#endif
}

static bool
user_data_migrate_directory(const char *source, const char *target, unsigned int depth) {
    user_data_path_kind_t source_kind = user_data_inspect_path(source);
    if (source_kind == USER_DATA_PATH_MISSING) {
        return true;
    }
    if (source_kind != USER_DATA_PATH_DIRECTORY || depth > USER_DATA_MAX_MIGRATION_DEPTH) {
        return false;
    }
    if (path_ensure_real_directory(target, 0700) != PATH_DIRECTORY_OK) {
        return false;
    }

    DIR *directory = opendir(source);
    if (directory == NULL) {
        return false;
    }

    bool complete = true;
    struct dirent *entry;
    bool read_error = false;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            read_error = errno != 0;
            break;
        }
        char source_path[HUGE_BUF];
        char target_path[HUGE_BUF];
        user_data_path_kind_t entry_kind;
        user_data_path_kind_t target_kind;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!user_data_format_path(source_path, sizeof(source_path), source, entry->d_name) ||
            !user_data_format_path(target_path, sizeof(target_path), target, entry->d_name)) {
            complete = false;
            continue;
        }

        entry_kind = user_data_inspect_path(source_path);
        if (entry_kind == USER_DATA_PATH_MISSING) {
            continue;
        }
        target_kind = user_data_inspect_path(target_path);

        if (entry_kind == USER_DATA_PATH_DIRECTORY) {
            if (target_kind != USER_DATA_PATH_MISSING && target_kind != USER_DATA_PATH_DIRECTORY) {
                complete = false;
                continue;
            }
            if (!user_data_migrate_directory(source_path, target_path, depth + 1U)) {
                complete = false;
            }
        } else if (entry_kind == USER_DATA_PATH_REGULAR) {
            if (!user_data_move_file(source_path, target_path)) {
                complete = false;
            }
        } else {
            complete = false;
        }
    }

    if (read_error || closedir(directory) != 0) {
        complete = false;
    }
    if (!complete) {
        return false;
    }

    source_kind = user_data_inspect_path(source);
    if (source_kind == USER_DATA_PATH_MISSING) {
        return true;
    }
    return source_kind == USER_DATA_PATH_DIRECTORY &&
           (user_data_remove_directory(source) ||
            user_data_inspect_path(source) == USER_DATA_PATH_MISSING);
}

static bool user_data_read_marker(const char *marker,
                                  unsigned int major,
                                  char *source_name,
                                  size_t source_name_size) {
    bool permissive = false;
    path_secret_error_t result =
        path_read_secret(marker, source_name, source_name_size, &permissive);
    user_data_candidate_t candidate;
    if (result != PATH_SECRET_OK || !user_data_parse_candidate(source_name, major, &candidate)) {
        LOG(ERROR, "Ignoring invalid user-data migration marker '%s'.", marker);
        return false;
    }
    return true;
}

static bool user_data_publish_marker(const char *marker,
                                     const char *source_name,
                                     unsigned int major,
                                     char *selected_name,
                                     size_t selected_name_size) {
    path_secret_create_result_t result =
        path_secret_create_atomic(marker, source_name, strlen(source_name));
    if (result == PATH_SECRET_CREATE_OK) {
        snprintf(selected_name, selected_name_size, "%s", source_name);
        return true;
    }

    if (user_data_inspect_path(marker) != USER_DATA_PATH_REGULAR) {
        return false;
    }
    return user_data_read_marker(marker, major, selected_name, selected_name_size);
}

static bool user_data_prepare_root(const char *config_root, char *root, size_t root_size) {
    char probe[HUGE_BUF];

    if (!user_data_format_path(root, root_size, config_root, USER_DATA_DIRECTORY) ||
        !user_data_format_path(probe, sizeof(probe), root, ".prepare")) {
        return false;
    }
    path_ensure_directories(probe);
    return user_data_inspect_path(root) == USER_DATA_PATH_DIRECTORY;
}

bool user_data_prepare(const char *config_root, unsigned int major, char *path, size_t path_size) {
    char root[HUGE_BUF];
    char stable[HUGE_BUF];
    char marker[HUGE_BUF];
    char source[HUGE_BUF];
    user_data_candidate_t candidate;
    bool found;

    if (config_root == NULL || *config_root == '\0' || path == NULL || path_size == 0) {
        return false;
    }
    path[0] = '\0';

    if (!user_data_prepare_root(config_root, root, sizeof(root)) ||
        snprintf(stable, sizeof(stable), "%s/%u.x", root, major) >= (int)sizeof(stable) ||
        snprintf(marker,
                 sizeof(marker),
                 "%s/%s%u%s",
                 root,
                 USER_DATA_MIGRATION_MARKER_PREFIX,
                 major,
                 USER_DATA_MIGRATION_MARKER_SUFFIX) >= (int)sizeof(marker)) {
        return false;
    }
    if (snprintf(path, path_size, "%s", stable) >= (int)path_size) {
        return false;
    }

    user_data_path_kind_t stable_kind = user_data_inspect_path(stable);
    user_data_path_kind_t marker_kind = user_data_inspect_path(marker);
    if ((stable_kind != USER_DATA_PATH_MISSING && stable_kind != USER_DATA_PATH_DIRECTORY) ||
        (marker_kind != USER_DATA_PATH_MISSING && marker_kind != USER_DATA_PATH_REGULAR)) {
        return false;
    }

    if (marker_kind == USER_DATA_PATH_REGULAR) {
        if (!user_data_read_marker(marker, major, source, sizeof(source))) {
            return stable_kind == USER_DATA_PATH_DIRECTORY &&
                   snprintf(path, path_size, "%s", stable) < (int)path_size;
        }
        if (stable_kind == USER_DATA_PATH_MISSING) {
            if (path_ensure_real_directory(stable, 0700) != PATH_DIRECTORY_OK) {
                return false;
            }
            stable_kind = USER_DATA_PATH_DIRECTORY;
            marker_kind = USER_DATA_PATH_REGULAR;
        }
    } else if (stable_kind == USER_DATA_PATH_MISSING) {
        if (!user_data_find_candidate(root, major, &candidate, &found)) {
            return false;
        }
        if (found) {
            if (!user_data_publish_marker(marker, candidate.name, major, source, sizeof(source))) {
                return false;
            }
            if (path_ensure_real_directory(stable, 0700) != PATH_DIRECTORY_OK) {
                return false;
            }
            stable_kind = USER_DATA_PATH_DIRECTORY;
            marker_kind = USER_DATA_PATH_REGULAR;
        } else if (path_ensure_real_directory(stable, 0700) != PATH_DIRECTORY_OK) {
            return false;
        }
    }

    if (marker_kind == USER_DATA_PATH_REGULAR && stable_kind == USER_DATA_PATH_DIRECTORY) {
        char source_path[HUGE_BUF];
        char target_path[HUGE_BUF];
        if (user_data_format_path(source_path, sizeof(source_path), root, source) &&
            snprintf(target_path, sizeof(target_path), "%s", stable) < (int)sizeof(target_path) &&
            user_data_migrate_directory(source_path, target_path, 0)) {
            if (!user_data_remove_file(marker) &&
                user_data_inspect_path(marker) != USER_DATA_PATH_MISSING) {
                LOG(ERROR, "Could not remove user-data migration marker '%s'.", marker);
            }
        }
    }

    return true;
}
