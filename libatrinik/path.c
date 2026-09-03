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
 * OS path API.
 */

#include "path.h"
#include "string.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>

#ifdef WIN32
#include <aclapi.h>
#endif

TOOLKIT_API(DEPENDS(logger), DEPENDS(string), DEPENDS(stringbuffer));

/**
 * Function to use for path_fopen(). Can be overriden.
 */
path_fopen_t path_fopen;

/**
 * Simple wrapper for fopen(); ensures directories leading up to path 'path'
 * exist and are accessible.
 *
 * @param path
 * Path.
 * @param modes
 * Modes.
 * @return
 * Opened file, NULL on failure.
 */
static FILE *fopen_wrapper(const char *path, const char *modes) {
    path_ensure_directories(path);
    return fopen(path, modes);
}

TOOLKIT_INIT_FUNC(path) {
    path_fopen = fopen_wrapper;
}
TOOLKIT_INIT_FUNC_FINISH

TOOLKIT_DEINIT_FUNC(path) {}
TOOLKIT_DEINIT_FUNC_FINISH

/**
 * Joins two path components, eg, '/usr' and 'bin' -> '/usr/bin'.
 * @param path
 * First path component.
 * @param path2
 * Second path component.
 * @return
 * The joined path; should be freed when no longer needed.
 */
char *path_join(const char *path, const char *path2) {
    StringBuffer *sb;
    size_t len;
    char *cp;

    TOOLKIT_PROTECT();

    sb = stringbuffer_new();
    stringbuffer_append_string(sb, path);

    len = strlen(path);

    if (len && path[len - 1] != '/') {
        stringbuffer_append_string(sb, "/");
    }

    stringbuffer_append_string(sb, path2);
    cp = stringbuffer_finish(sb);

    return cp;
}

/**
 * Extracts the directory component of a path.
 *
 * Example:
 * @code
 * path_dirname("/usr/local/foobar"); --> "/usr/local"
 * @endcode
 * @param path
 * A path.
 * @return
 * A directory name. This string should be freed when no longer
 * needed.
 * @author Hongli Lai (public domain)
 */
char *path_dirname(const char *path) {
    const char *end;
    char *result;

    TOOLKIT_PROTECT();

    if (!path) {
        return NULL;
    }

    end = strrchr(path, '/');

    if (!end) {
        return xstrdup(".");
    }

    while (end > path && *end == '/') {
        end--;
    }

    result = xstrndup(path, end - path + 1);

    if (result[0] == '\0') {
        free(result);
        return xstrdup("/");
    }

    return result;
}

/**
 * Extracts the basename from path.
 *
 * Example:
 * @code
 * path_basename("/usr/bin/kate"); --> "kate"
 * @endcode
 * @param path
 * A path.
 * @return
 * The basename of the path. Should be freed when no longer
 * needed.
 */
char *path_basename(const char *path) {
    const char *slash;

    TOOLKIT_PROTECT();

    if (!path) {
        return NULL;
    }

    while ((slash = strrchr(path, '/'))) {
        if (*(slash + 1) != '\0') {
            return xstrdup(slash + 1);
        }
    }

    return xstrdup(path);
}

/**
 * Normalize a path, eg, foo//bar, foo/foo2/../bar, foo/./bar all become
 * foo/bar.
 *
 * If the path begins with either a forward slash or a dot *and* a forward
 * slash, they will be preserved.
 * @param path
 * Path to normalize.
 * @return
 * The normalized path; never NULL. Must be freed.
 */
char *path_normalize(const char *path) {
    StringBuffer *sb;
    size_t pos, startsbpos;
    char component[MAX_BUF];
    ssize_t last_slash;

    TOOLKIT_PROTECT();

    if (string_isempty(path)) {
        return xstrdup(".");
    }

    sb = stringbuffer_new();
    pos = 0;

    if (string_startswith(path, "/")) {
        stringbuffer_append_string(sb, "/");
    } else if (string_startswith(path, "./")) {
        stringbuffer_append_string(sb, "./");
    }

    startsbpos = stringbuffer_length(sb);

    while (string_get_word(path, &pos, '/', component, sizeof(component), 0)) {
        if (strcmp(component, ".") == 0) {
            continue;
        }

        if (strcmp(component, "..") == 0) {
            if (stringbuffer_length(sb) > startsbpos) {
                last_slash = stringbuffer_rindex(sb, '/');

                if (last_slash == -1) {
                    LOG(BUG, "Should have found a forward slash, but didn't: %s", path);
                    continue;
                }

                stringbuffer_seek(sb, last_slash);
            }
        } else {
            size_t len = stringbuffer_length(sb);
            if (len == 0 || stringbuffer_data(sb)[len - 1] != '/') {
                stringbuffer_append_string(sb, "/");
            }

            stringbuffer_append_string(sb, component);
        }
    }

    if (stringbuffer_length(sb) == 0) {
        stringbuffer_append_string(sb, ".");
    }

    return stringbuffer_finish(sb);
}

/**
 * Checks whether any directories in the given path don't exist, and
 * creates them if necessary.
 * @param path
 * The path to check.
 */
void path_ensure_directories(const char *path) {
    char buf[MAXPATHLEN], *cp;
    struct stat statbuf;

    TOOLKIT_PROTECT();

    if (path == NULL || *path == '\0') {
        return;
    }

    snprintf(VS(buf), "%s", path);
    cp = buf;

    while ((cp = strchr(cp + 1, '/')) != NULL) {
        *cp = '\0';

        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            LOG(BUG, "Cannot mkdir %s (path: %s): %s", buf, path, strerror(errno));
            return;
        }

        if (stat(buf, &statbuf) != 0) {
            LOG(BUG, "Cannot stat %s (path: %s): %s", buf, path, strerror(errno));
            return;
        }

        if (!S_ISDIR(statbuf.st_mode)) {
            LOG(BUG, "Not a directory: %s (path: %s)", buf, path);
            return;
        }

        *cp = '/';
    }
}

/**
 * Copy the contents of file 'src' into 'dst'.
 * @param src
 * Path of the file to copy contents from.
 * @param dst
 * Where to put the contents of 'src'.
 * @param mode
 * Mode to open 'src' in.
 * @return
 * 1 on success, 0 on failure.
 */
int path_copy_file(const char *src, FILE *dst, const char *mode) {
    FILE *fp;
    char buf[HUGE_BUF];

    TOOLKIT_PROTECT();

    if (!src || !dst || !mode) {
        return 0;
    }

    fp = fopen(src, mode);

    if (!fp) {
        return 0;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        fputs(buf, dst);
    }

    fclose(fp);

    return 1;
}

/**
 * Check if the specified path exists.
 * @param path
 * Path to check.
 * @return
 * 1 if 'path' exists, 0 otherwise.
 */
int path_exists(const char *path) {
    struct stat statbuf;

    TOOLKIT_PROTECT();

    if (stat(path, &statbuf) != 0) {
        return 0;
    }

    return 1;
}

/**
 * Create a new blank file.
 * @param path
 * Path to the file.
 * @return
 * 1 on success, 0 on failure.
 */
int path_touch(const char *path) {
    FILE *fp;

    TOOLKIT_PROTECT();

    path_ensure_directories(path);
    fp = fopen(path, "w");

    if (!fp) {
        return 0;
    }

    if (fclose(fp) == EOF) {
        return 0;
    }

    return 1;
}

/**
 * Get size of the specified file, in bytes.
 * @param path
 * Path to the file.
 * @return
 * Size of the file.
 */
size_t path_size(const char *path) {
    struct stat statbuf;

    TOOLKIT_PROTECT();

    if (stat(path, &statbuf) != 0) {
        return 0;
    }

    return statbuf.st_size;
}

/**
 * Load the entire contents of file 'path' into a StringBuffer instance,
 * then return the created string.
 * @param path
 * File to load contents of.
 * @return
 * The loaded contents. Must be freed.
 */
char *path_file_contents(const char *path) {
    FILE *fp;
    StringBuffer *sb;
    char buf[MAX_BUF];

    TOOLKIT_PROTECT();

    fp = fopen(path, "rb");

    if (!fp) {
        return NULL;
    }

    sb = stringbuffer_new();

    while (fgets(buf, sizeof(buf), fp)) {
        stringbuffer_append_string(sb, buf);
    }

    fclose(fp);

    return stringbuffer_finish(sb);
}

/**
 * Changes name of the specified file in an atomic manner.
 *
 * On POSIX systems, rename() is used; on Windows, the MoveFileEx() API is
 * used.
 *
 * @param old
 * File that is to be renamed.
 * @param new
 * New path for the file.
 * @return
 * 0 on success, an error number otherwise.
 */
int path_rename(const char *old, const char *new) {
#ifdef WIN32
    if (!MoveFileEx(old, new, MOVEFILE_REPLACE_EXISTING)) {
        return GetLastError();
    }

    return 0;
#else
    return rename(old, new);
#endif
}

bool path_write_atomic(const char *path, const void *data, size_t size, unsigned int mode) {
    HARD_ASSERT(path != NULL);
    HARD_ASSERT(data != NULL || size == 0);

    path_ensure_directories(path);
    return path_write_atomic_existing(path, data, size, mode);
}

bool path_write_atomic_existing(const char *path,
                                const void *data,
                                size_t size,
                                unsigned int mode) {
    HARD_ASSERT(path != NULL);
    HARD_ASSERT(data != NULL || size == 0);

    char temporary[HUGE_BUF];
    if (snprintf(VS(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) {
        return false;
    }

    int fd = mkstemp(temporary);
    if (fd == -1) {
        return false;
    }
#ifndef WIN32
    if (fchmod(fd, (mode_t)mode) != 0) {
        close(fd);
        unlink(temporary);
        return false;
    }
#else
    (void)mode;
#endif

    FILE *fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        unlink(temporary);
        return false;
    }
    bool ok = fwrite(data, 1, size, fp) == size && fflush(fp) == 0;
#ifndef WIN32
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
#endif
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(temporary);
        return false;
    }
    if (path_rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
#ifndef WIN32
    char *directory = path_dirname(path);
    int directory_fd = directory != NULL ? open(directory, O_RDONLY | O_DIRECTORY) : -1;
    bool directory_synced = directory_fd >= 0 && fsync(directory_fd) == 0;
    if (directory_fd >= 0 && close(directory_fd) != 0) {
        directory_synced = false;
    }
    free(directory);
    if (!directory_synced) {
        return false;
    }
#endif
    return true;
}

#ifdef WIN32
static wchar_t *path_windows_wide(const char *path) {
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

static path_directory_result_t path_directory_inspect_windows(const wchar_t *path, bool *missing) {
    *missing = false;
    HANDLE directory = CreateFileW(path,
                                   FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL,
                                   OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                   NULL);
    if (directory == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        *missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return PATH_DIRECTORY_ERROR;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes;
    FILE_STANDARD_INFO standard;
    bool inspected =
        GetFileInformationByHandleEx(directory,
                                     FileAttributeTagInfo,
                                     &attributes,
                                     sizeof(attributes)) != 0 &&
        GetFileInformationByHandleEx(directory, FileStandardInfo, &standard, sizeof(standard)) !=
            0 &&
        GetFileType(directory) == FILE_TYPE_DISK;
    bool closed = CloseHandle(directory) != 0;
    if (!inspected || !closed) {
        return PATH_DIRECTORY_ERROR;
    }
    if (!standard.Directory || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return PATH_DIRECTORY_UNSAFE;
    }
    return PATH_DIRECTORY_OK;
}

static TOKEN_USER *path_secret_token_user(HANDLE *token) {
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

static bool path_secret_windows_security(HANDLE file,
                                         const TOKEN_USER *user,
                                         bool *permissive_mode,
                                         path_secret_error_t *error) {
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD status = GetSecurityInfo(file,
                                   SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                   &owner,
                                   NULL,
                                   &dacl,
                                   NULL,
                                   &descriptor);
    if (status != ERROR_SUCCESS) {
        *error = PATH_SECRET_METADATA_ERROR;
        return false;
    }
    if (owner == NULL || !EqualSid(owner, user->User.Sid)) {
        *error = PATH_SECRET_WRONG_OWNER;
        LocalFree(descriptor);
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool unsafe = dacl == NULL || !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
                  (control & SE_DACL_PROTECTED) == 0;
    if (!unsafe) {
        ACL_SIZE_INFORMATION information;
        if (!GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) {
            *error = PATH_SECRET_METADATA_ERROR;
            LocalFree(descriptor);
            return false;
        }
        for (DWORD i = 0; i < information.AceCount; i++) {
            void *entry = NULL;
            if (!GetAce(dacl, i, &entry)) {
                *error = PATH_SECRET_METADATA_ERROR;
                LocalFree(descriptor);
                return false;
            }
            ACE_HEADER *header = entry;
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                ACCESS_ALLOWED_ACE *ace = entry;
                PSID trustee = &ace->SidStart;
                if (ace->Mask != 0 && !EqualSid(trustee, user->User.Sid)) {
                    unsafe = true;
                }
            } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                       header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                       header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
                unsafe = true;
            }
        }
    }
    if (permissive_mode != NULL) {
        *permissive_mode = unsafe;
    }
    LocalFree(descriptor);
    return true;
}
#endif

path_directory_result_t path_ensure_real_directory(const char *path, unsigned int mode) {
    HARD_ASSERT(path != NULL);

#ifdef WIN32
    (void)mode;
    wchar_t *wide = path_windows_wide(path);
    if (wide == NULL) {
        return PATH_DIRECTORY_ERROR;
    }

    bool missing = false;
    path_directory_result_t result = path_directory_inspect_windows(wide, &missing);
    if (missing) {
        if (!CreateDirectoryW(wide, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
                free(wide);
                return PATH_DIRECTORY_ERROR;
            }
        }
        result = path_directory_inspect_windows(wide, &missing);
        if (missing) {
            result = PATH_DIRECTORY_ERROR;
        }
    }
    free(wide);
    return result;
#else
    struct stat metadata;
    if (lstat(path, &metadata) == 0) {
        return S_ISDIR(metadata.st_mode) && !S_ISLNK(metadata.st_mode) ? PATH_DIRECTORY_OK
                                                                       : PATH_DIRECTORY_UNSAFE;
    }
    if (errno != ENOENT) {
        return PATH_DIRECTORY_ERROR;
    }
    if (mkdir(path, (mode_t)mode) != 0 && errno != EEXIST) {
        return PATH_DIRECTORY_ERROR;
    }
    if (lstat(path, &metadata) != 0) {
        return PATH_DIRECTORY_ERROR;
    }
    return S_ISDIR(metadata.st_mode) && !S_ISLNK(metadata.st_mode) ? PATH_DIRECTORY_OK
                                                                   : PATH_DIRECTORY_UNSAFE;
#endif
}

path_secret_create_result_t
path_secret_create_atomic(const char *path, const void *data, size_t size) {
    HARD_ASSERT(path != NULL);
    HARD_ASSERT(data != NULL || size == 0);

    unsigned char random[8];
    char suffix[sizeof(random) * 2U + 1U];
    if (RAND_bytes(random, sizeof(random)) != 1 ||
        string_tohex(random, sizeof(random), VS(suffix), false) != sizeof(random) * 2U) {
        OPENSSL_cleanse(random, sizeof(random));
        return PATH_SECRET_CREATE_ERROR;
    }
    OPENSSL_cleanse(random, sizeof(random));
    string_tolower(suffix);

#ifdef WIN32
    char temporary[HUGE_BUF];
    if (snprintf(VS(temporary), "%s.tmp.%s", path, suffix) >= (int)sizeof(temporary)) {
        return PATH_SECRET_CREATE_ERROR;
    }
    wchar_t *destination_wide = path_windows_wide(path);
    wchar_t *temporary_wide = path_windows_wide(temporary);
    HANDLE token = NULL;
    TOKEN_USER *user = path_secret_token_user(&token);
    DWORD sid_size = user != NULL ? GetLengthSid(user->User.Sid) : 0;
    DWORD acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_size;
    PACL dacl = user != NULL ? malloc(acl_size) : NULL;
    SECURITY_DESCRIPTOR descriptor;
    bool security_ok =
        dacl != NULL && InitializeAcl(dacl, acl_size, ACL_REVISION) &&
        AddAccessAllowedAce(dacl, ACL_REVISION, FILE_ALL_ACCESS, user->User.Sid) &&
        InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE) &&
        SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) &&
        SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
    SECURITY_ATTRIBUTES attributes = {
        .nLength = sizeof(attributes),
        .lpSecurityDescriptor = security_ok ? &descriptor : NULL,
        .bInheritHandle = FALSE,
    };
    HANDLE file = INVALID_HANDLE_VALUE;
    if (destination_wide != NULL && temporary_wide != NULL && security_ok) {
        file = CreateFileW(temporary_wide,
                           GENERIC_WRITE | DELETE,
                           0,
                           &attributes,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                           NULL);
    }
    bool temporary_created = file != INVALID_HANDLE_VALUE;
    bool ok = temporary_created;
    size_t offset = 0;
    while (ok && offset < size) {
        DWORD chunk = (DWORD)MIN(size - offset, (size_t)UINT32_MAX);
        DWORD written = 0;
        ok = WriteFile(file, (const unsigned char *)data + offset, chunk, &written, NULL) &&
             written == chunk;
        offset += written;
    }
    path_secret_create_result_t result = PATH_SECRET_CREATE_ERROR;
    bool published = false;
    DWORD publish_error = ERROR_SUCCESS;
    if (ok) {
        ok = FlushFileBuffers(file) != 0;
    }
    bool closed = file == INVALID_HANDLE_VALUE || CloseHandle(file) != 0;
    if (ok && closed) {
        published = MoveFileExW(temporary_wide, destination_wide, MOVEFILE_WRITE_THROUGH) != 0;
        if (!published) {
            publish_error = GetLastError();
        }
    }
    if (published) {
        result = PATH_SECRET_CREATE_OK;
    } else if (!published &&
               (publish_error == ERROR_FILE_EXISTS || publish_error == ERROR_ALREADY_EXISTS)) {
        result = PATH_SECRET_CREATE_EXISTS;
    }
    if (!published && temporary_created && temporary_wide != NULL) {
        DeleteFileW(temporary_wide);
    }
    free(dacl);
    free(user);
    if (token != NULL) {
        CloseHandle(token);
    }
    free(temporary_wide);
    free(destination_wide);
    OPENSSL_cleanse(temporary, sizeof(temporary));
    OPENSSL_cleanse(suffix, sizeof(suffix));
    return result;
#else
    char *directory = path_dirname(path);
    char *basename = path_basename(path);
    if (directory == NULL || basename == NULL || *basename == '\0' ||
        strchr(basename, '/') != NULL) {
        free(directory);
        free(basename);
        OPENSSL_cleanse(suffix, sizeof(suffix));
        return PATH_SECRET_CREATE_ERROR;
    }
    int directory_flags = O_RDONLY;
#ifdef O_DIRECTORY
    directory_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
    int directory_fd = open(directory, directory_flags);
    char temporary[64];
    int temporary_fd = -1;
    if (directory_fd != -1 &&
        snprintf(VS(temporary), ".atrinik-secret-%s", suffix) < (int)sizeof(temporary)) {
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        temporary_fd = openat(directory_fd, temporary, flags, 0600);
    }
    bool ok = temporary_fd != -1 && fchmod(temporary_fd, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < size) {
        ssize_t written = write(temporary_fd, (const unsigned char *)data + offset, size - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += (size_t)written;
    }
    if (ok && fsync(temporary_fd) != 0) {
        ok = false;
    }

    path_secret_create_result_t result = PATH_SECRET_CREATE_ERROR;
    struct stat opened, named, published;
    int stat_flags = 0;
#ifdef AT_SYMLINK_NOFOLLOW
    stat_flags = AT_SYMLINK_NOFOLLOW;
#endif
    bool same_named_inode = ok && fstat(temporary_fd, &opened) == 0 &&
                            fstatat(directory_fd, temporary, &named, stat_flags) == 0 &&
                            opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
    if (same_named_inode && linkat(directory_fd, temporary, directory_fd, basename, 0) == 0) {
        bool same_published_inode = fstatat(directory_fd, basename, &published, stat_flags) == 0 &&
                                    opened.st_dev == published.st_dev &&
                                    opened.st_ino == published.st_ino;
        if (same_published_inode && unlinkat(directory_fd, temporary, 0) == 0 &&
            fsync(directory_fd) == 0) {
            result = PATH_SECRET_CREATE_OK;
        }
    } else if (same_named_inode && errno == EEXIST) {
        result = PATH_SECRET_CREATE_EXISTS;
    }
    if (result != PATH_SECRET_CREATE_OK && temporary_fd != -1) {
        unlinkat(directory_fd, temporary, 0);
    }
    if (temporary_fd != -1 && close(temporary_fd) != 0 && result == PATH_SECRET_CREATE_OK) {
        result = PATH_SECRET_CREATE_ERROR;
    }
    if (directory_fd != -1) {
        close(directory_fd);
    }
    free(directory);
    free(basename);
    OPENSSL_cleanse(temporary, sizeof(temporary));
    OPENSSL_cleanse(suffix, sizeof(suffix));
    return result;
#endif
}

path_secret_error_t
path_read_secret(const char *path, char *secret, size_t secret_size, bool *permissive_mode) {
    HARD_ASSERT(path != NULL);
    HARD_ASSERT(secret != NULL);
    HARD_ASSERT(secret_size >= 2);

    OPENSSL_cleanse(secret, secret_size);
    if (permissive_mode != NULL) {
        *permissive_mode = false;
    }

    path_secret_error_t result = PATH_SECRET_OK;
    unsigned char trailing[256] = {0};
    size_t length = 0;

#ifdef WIN32
    wchar_t *wide = path_windows_wide(path);
    HANDLE file = wide != NULL ? CreateFileW(wide,
                                             GENERIC_READ | READ_CONTROL,
                                             FILE_SHARE_READ,
                                             NULL,
                                             OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                                 FILE_FLAG_SEQUENTIAL_SCAN,
                                             NULL)
                               : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? PATH_SECRET_NOT_FOUND
                   : PATH_SECRET_OPEN_ERROR;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes;
    FILE_STANDARD_INFO standard;
    if (!GetFileInformationByHandleEx(file,
                                      FileAttributeTagInfo,
                                      &attributes,
                                      sizeof(attributes)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
        result = PATH_SECRET_METADATA_ERROR;
        goto out;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result = PATH_SECRET_UNSAFE_LINK;
        goto out;
    }
    if (standard.Directory || GetFileType(file) != FILE_TYPE_DISK) {
        result = PATH_SECRET_NOT_REGULAR;
        goto out;
    }
    HANDLE token = NULL;
    TOKEN_USER *user = path_secret_token_user(&token);
    if (user == NULL || !path_secret_windows_security(file, user, permissive_mode, &result)) {
        free(user);
        if (token != NULL) {
            CloseHandle(token);
        }
        goto out;
    }
    free(user);
    CloseHandle(token);
    while (length < secret_size) {
        DWORD received = 0;
        if (!ReadFile(file, secret + length, (DWORD)(secret_size - length), &received, NULL)) {
            result = PATH_SECRET_READ_ERROR;
            goto out;
        }
        if (received == 0) {
            break;
        }
        length += received;
    }
#else
    struct stat before;
    bool have_before = lstat(path, &before) == 0;
    if (have_before && S_ISLNK(before.st_mode)) {
        return PATH_SECRET_UNSAFE_LINK;
    }
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int file = open(path, flags);
    if (file == -1) {
        return errno == ENOENT  ? PATH_SECRET_NOT_FOUND
               : errno == ELOOP ? PATH_SECRET_UNSAFE_LINK
                                : PATH_SECRET_OPEN_ERROR;
    }
    struct stat metadata;
    if (fstat(file, &metadata) != 0) {
        result = PATH_SECRET_METADATA_ERROR;
        goto out;
    }
    if ((have_before && (before.st_dev != metadata.st_dev || before.st_ino != metadata.st_ino)) ||
        S_ISLNK(metadata.st_mode)) {
        result = PATH_SECRET_UNSAFE_LINK;
        goto out;
    }
    struct stat after;
    if (lstat(path, &after) != 0 || S_ISLNK(after.st_mode) || after.st_dev != metadata.st_dev ||
        after.st_ino != metadata.st_ino) {
        result = PATH_SECRET_UNSAFE_LINK;
        goto out;
    }
    if (!S_ISREG(metadata.st_mode)) {
        result = PATH_SECRET_NOT_REGULAR;
        goto out;
    }
    if (metadata.st_uid != geteuid()) {
        result = PATH_SECRET_WRONG_OWNER;
        goto out;
    }
    if (permissive_mode != NULL && (metadata.st_mode & 0777) != 0600) {
        *permissive_mode = true;
    }
    while (length < secret_size) {
        ssize_t received = read(file, secret + length, secret_size - length);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            result = PATH_SECRET_READ_ERROR;
            goto out;
        }
        if (received == 0) {
            break;
        }
        length += (size_t)received;
    }
#endif

    char *newline = memchr(secret, '\n', length);
    if (newline != NULL) {
        if ((size_t)(newline - secret) + 1U < length) {
            result = PATH_SECRET_TRAILING_DATA;
            goto out;
        }

        for (;;) {
#ifdef WIN32
            DWORD trailing_length = 0;
            if (!ReadFile(file, trailing, sizeof(trailing), &trailing_length, NULL)) {
                result = PATH_SECRET_READ_ERROR;
                goto out;
            }
#else
            ssize_t trailing_read = read(file, trailing, sizeof(trailing));
            if (trailing_read < 0 && errno == EINTR) {
                continue;
            }
            if (trailing_read < 0) {
                result = PATH_SECRET_READ_ERROR;
                goto out;
            }
            size_t trailing_length = (size_t)trailing_read;
#endif
            if (trailing_length == 0) {
                break;
            }
            result = PATH_SECRET_TRAILING_DATA;
            goto out;
        }

        size_t used = (size_t)(newline - secret);
        if (used > 0 && secret[used - 1] == '\r') {
            used--;
        }
        secret[used] = '\0';
    } else {
        if (length == secret_size) {
            result = PATH_SECRET_TOO_LONG;
            goto out;
        }
        secret[length] = '\0';
    }

    if (*secret == '\0') {
        result = PATH_SECRET_EMPTY;
    }

out:
    OPENSSL_cleanse(trailing, sizeof(trailing));
#ifdef WIN32
    if (!CloseHandle(file) && result == PATH_SECRET_OK) {
#else
    if (close(file) != 0 && result == PATH_SECRET_OK) {
#endif
        result = PATH_SECRET_READ_ERROR;
    }
    if (result != PATH_SECRET_OK) {
        OPENSSL_cleanse(secret, secret_size);
    }
    return result;
}

const char *path_secret_error_string(path_secret_error_t error) {
    switch (error) {
        case PATH_SECRET_OK:
            return "success";
        case PATH_SECRET_NOT_FOUND:
            return "the file does not exist";
        case PATH_SECRET_OPEN_ERROR:
            return "cannot open the file";
        case PATH_SECRET_METADATA_ERROR:
            return "cannot inspect the file";
        case PATH_SECRET_NOT_REGULAR:
            return "the path is not a regular file";
        case PATH_SECRET_WRONG_OWNER:
            return "the file is owned by another user";
        case PATH_SECRET_UNSAFE_LINK:
            return "the path is a symbolic link or reparse point";
        case PATH_SECRET_EMPTY:
            return "the file is empty";
        case PATH_SECRET_TOO_LONG:
            return "the first line is too long";
        case PATH_SECRET_TRAILING_DATA:
            return "the file contains data after the first line";
        case PATH_SECRET_READ_ERROR:
            return "cannot read the file";
    }

    return "unknown error";
}

bool path_is_safe_relative(const char *path) {
    if (path == NULL || *path == '\0' || *path == '/' || *path == '\\') {
        return false;
    }

    const char *component = path;
    for (const char *cp = path;; cp++) {
        if (*cp == '\\' || *cp == ':') {
            return false;
        }
        if (*cp != '/' && *cp != '\0') {
            continue;
        }

        size_t length = (size_t)(cp - component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (*cp == '\0') {
            return true;
        }
        component = cp + 1;
    }
}
