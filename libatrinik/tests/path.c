#include <toolkit/path.h>

#include <openssl/crypto.h>

#ifdef WIN32
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

#ifndef WIN32
static bool fail_parent_directory_fsync;
static bool fail_parent_directory_close;
static bool fail_next_parent_close;
static unsigned int fsync_call_count;

int __real_fsync(int fd);
int __real_close(int fd);

int __wrap_fsync(int fd) {
    if (fail_parent_directory_close && ++fsync_call_count == 2) {
        fail_next_parent_close = true;
    }
    if (fail_parent_directory_fsync && ++fsync_call_count == 2) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_close(int fd) {
    if (fail_next_parent_close) {
        fail_next_parent_close = false;
        int result = __real_close(fd);
        errno = EIO;
        return result == 0 ? -1 : result;
    }
    return __real_close(fd);
}
#endif

static void
require_failed(const char *expression, const char *file, int line, unsigned long system_error) {
    fprintf(stderr,
            "%s:%d: requirement failed: %s (last system error: %lu)\n",
            file,
            line,
            expression,
            system_error);
    fflush(stderr);
    abort();
}

#ifdef WIN32
#define REQUIRE_SYSTEM_ERROR ((unsigned long)GetLastError())
#else
#define REQUIRE_SYSTEM_ERROR ((unsigned long)errno)
#endif

#define require(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            require_failed(#condition, __FILE__, __LINE__, REQUIRE_SYSTEM_ERROR); \
        }                                                                         \
    } while (0)

#ifdef WIN32
static wchar_t *path_to_wide(const char *path) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    require(length > 0);
    wchar_t *wide = calloc((size_t)length, sizeof(*wide));
    require(wide != NULL);
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length) == length);
    return wide;
}

static void unprotect_file_dacl(const char *path) {
    wchar_t *wide = path_to_wide(path);
    HANDLE file = CreateFileW(wide,
                              READ_CONTROL | WRITE_DAC,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              NULL);
    require(file != INVALID_HANDLE_VALUE);

    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    require(GetSecurityInfo(file,
                            SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            &owner,
                            NULL,
                            &dacl,
                            NULL,
                            &descriptor) == ERROR_SUCCESS);
    require(owner != NULL && dacl != NULL && descriptor != NULL);
    require(SetSecurityInfo(file,
                            SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                            NULL,
                            NULL,
                            dacl,
                            NULL) == ERROR_SUCCESS);

    PSID updated_owner = NULL;
    PSECURITY_DESCRIPTOR updated_descriptor = NULL;
    require(GetSecurityInfo(file,
                            SE_FILE_OBJECT,
                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            &updated_owner,
                            NULL,
                            NULL,
                            NULL,
                            &updated_descriptor) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    require(updated_owner != NULL && EqualSid(owner, updated_owner));
    require(GetSecurityDescriptorControl(updated_descriptor, &control, &revision));
    require((control & SE_DACL_PROTECTED) == 0);

    require(LocalFree(updated_descriptor) == NULL);
    require(LocalFree(descriptor) == NULL);
    require(CloseHandle(file));
    free(wide);
}
#endif

static void setup_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    require(fp != NULL);
    require(fwrite(contents, 1, strlen(contents), fp) == strlen(contents));
    require(fclose(fp) == 0);
}

int main(int argc, char **argv) {
    toolkit_import(path);

#ifndef WIN32
    (void)argc;
    (void)argv;
#endif

    char directory[HUGE_BUF];
#ifdef WIN32
    char temporary_root[HUGE_BUF];
    DWORD length = GetTempPathA(sizeof(temporary_root), temporary_root);
    require(length > 0 && length < sizeof(temporary_root));
    require(snprintf(VS(directory),
                     "%satrinik-path-%lu",
                     temporary_root,
                     (unsigned long)GetCurrentProcessId()) < (int)sizeof(directory));
    require(CreateDirectoryA(directory, NULL));
#else
    snprintf(VS(directory), "/tmp/atrinik-path-XXXXXX");
    require(mkdtemp(directory) != NULL);
#endif

    char prepared[HUGE_BUF];
    require(snprintf(VS(prepared), "%s/prepared", directory) < (int)sizeof(prepared));
    require(path_ensure_real_directory(prepared, 0700) == PATH_DIRECTORY_OK);
    require(path_ensure_real_directory(prepared, 0700) == PATH_DIRECTORY_OK);
    struct stat prepared_metadata;
    require(stat(prepared, &prepared_metadata) == 0 && S_ISDIR(prepared_metadata.st_mode));

    char not_directory[HUGE_BUF];
    require(snprintf(VS(not_directory), "%s/not-directory", directory) <
            (int)sizeof(not_directory));
    setup_file(not_directory, "not a directory\n");
    require(path_ensure_real_directory(not_directory, 0700) == PATH_DIRECTORY_UNSAFE);

    char missing_directory_parent[HUGE_BUF];
    require(snprintf(VS(missing_directory_parent), "%s/missing-parent/child", directory) <
            (int)sizeof(missing_directory_parent));
    require(path_ensure_real_directory(missing_directory_parent, 0700) == PATH_DIRECTORY_ERROR);

    char directory_link[HUGE_BUF];
    require(snprintf(VS(directory_link), "%s/directory-link", directory) <
            (int)sizeof(directory_link));
#ifdef WIN32
    wchar_t *prepared_wide = path_to_wide(prepared);
    wchar_t *directory_link_wide = path_to_wide(directory_link);
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
    bool directory_link_created =
        CreateSymbolicLinkW(directory_link_wide,
                            prepared_wide,
                            SYMBOLIC_LINK_FLAG_DIRECTORY |
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    DWORD directory_link_error = directory_link_created ? ERROR_SUCCESS : GetLastError();
    if (!directory_link_created && directory_link_error == ERROR_INVALID_PARAMETER) {
        directory_link_created =
            CreateSymbolicLinkW(directory_link_wide, prepared_wide, SYMBOLIC_LINK_FLAG_DIRECTORY) !=
            0;
        directory_link_error = directory_link_created ? ERROR_SUCCESS : GetLastError();
    }
    require(directory_link_created || directory_link_error == ERROR_PRIVILEGE_NOT_HELD);
    if (directory_link_created) {
        require(path_ensure_real_directory(directory_link, 0700) == PATH_DIRECTORY_UNSAFE);
        require(RemoveDirectoryW(directory_link_wide));
    }
    free(directory_link_wide);
    free(prepared_wide);

    static const char invalid_utf8[] = {(char)0xc3, (char)0x28, '\0'};
    require(path_ensure_real_directory(invalid_utf8, 0700) == PATH_DIRECTORY_ERROR);
    require(argc == 1 || argc == 2);
    if (argc == 2) {
        require(path_ensure_real_directory(argv[1], 0700) == PATH_DIRECTORY_UNSAFE);
    }
#else
    require(symlink(prepared, directory_link) == 0);
    require(path_ensure_real_directory(directory_link, 0700) == PATH_DIRECTORY_UNSAFE);
    require(unlink(directory_link) == 0);

    char dangling_link[HUGE_BUF];
    require(snprintf(VS(dangling_link), "%s/dangling-link", directory) <
            (int)sizeof(dangling_link));
    require(symlink("missing-target", dangling_link) == 0);
    require(path_ensure_real_directory(dangling_link, 0700) == PATH_DIRECTORY_UNSAFE);
    require(unlink(dangling_link) == 0);
#endif

    char path[HUGE_BUF];
    require(snprintf(VS(path), "%s/invite", directory) < (int)sizeof(path));
    static const char secret_data[] = "secret-value\n";
    require(path_secret_create_atomic(path, secret_data, sizeof(secret_data) - 1U) ==
            PATH_SECRET_CREATE_OK);

    char secret[64];
    bool permissive = true;
    require(path_read_secret(path, VS(secret), &permissive) == PATH_SECRET_OK);
    require(strcmp(secret, "secret-value") == 0 && !permissive);
    struct stat metadata;
    require(stat(path, &metadata) == 0 && S_ISREG(metadata.st_mode));
#ifndef WIN32
    require(metadata.st_uid == geteuid() && (metadata.st_mode & 0777) == 0600);
#endif

    static const char replacement[] = "must-not-replace\n";
    require(path_secret_create_atomic(path, replacement, sizeof(replacement) - 1U) ==
            PATH_SECRET_CREATE_EXISTS);
    require(path_read_secret(path, VS(secret), &permissive) == PATH_SECRET_OK);
    require(strcmp(secret, "secret-value") == 0);

#ifdef WIN32
    char broad[HUGE_BUF];
    require(snprintf(VS(broad), "%s/broad", directory) < (int)sizeof(broad));
    static const char broad_secret[] = "broad-secret\n";
    require(path_secret_create_atomic(broad, broad_secret, sizeof(broad_secret) - 1U) ==
            PATH_SECRET_CREATE_OK);
    unprotect_file_dacl(broad);
    permissive = false;
    require(path_read_secret(broad, VS(secret), &permissive) == PATH_SECRET_OK);
    require(strcmp(secret, "broad-secret") == 0 && permissive);

    char link_path[HUGE_BUF];
    require(snprintf(VS(link_path), "%s/link", directory) < (int)sizeof(link_path));
    wchar_t *target_wide = path_to_wide(path);
    wchar_t *link_wide = path_to_wide(link_path);
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
    bool link_created =
        CreateSymbolicLinkW(link_wide, target_wide, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) !=
        0;
    DWORD link_error = link_created ? ERROR_SUCCESS : GetLastError();
    if (!link_created && link_error == ERROR_INVALID_PARAMETER) {
        link_created = CreateSymbolicLinkW(link_wide, target_wide, 0) != 0;
        link_error = link_created ? ERROR_SUCCESS : GetLastError();
    }
    require(link_created || link_error == ERROR_PRIVILEGE_NOT_HELD);
    if (link_created) {
        memset(secret, 'x', sizeof(secret));
        require(path_read_secret(link_path, VS(secret), NULL) == PATH_SECRET_UNSAFE_LINK);
        static const char windows_cleared[sizeof(secret)];
        require(CRYPTO_memcmp(secret, windows_cleared, sizeof(secret)) == 0);
        require(DeleteFileW(link_wide));
    }
    free(link_wide);
    free(target_wide);
#endif

    char missing[HUGE_BUF];
    require(snprintf(VS(missing), "%s/missing", directory) < (int)sizeof(missing));
    memset(secret, 'x', sizeof(secret));
    require(path_read_secret(missing, VS(secret), NULL) == PATH_SECRET_NOT_FOUND);
    static const char cleared[sizeof(secret)];
    require(CRYPTO_memcmp(secret, cleared, sizeof(secret)) == 0);

    char too_long[HUGE_BUF];
    require(snprintf(VS(too_long), "%s/too-long", directory) < (int)sizeof(too_long));
    static const char too_long_secret[] = "12345678";
    require(path_secret_create_atomic(too_long, too_long_secret, sizeof(too_long_secret) - 1U) ==
            PATH_SECRET_CREATE_OK);
#ifndef WIN32
    require(chmod(too_long, 0600) == 0);
#endif
    char small[8];
    memset(small, 'x', sizeof(small));
    require(path_read_secret(too_long, VS(small), NULL) == PATH_SECRET_TOO_LONG);
    static const char small_cleared[sizeof(small)];
    require(CRYPTO_memcmp(small, small_cleared, sizeof(small)) == 0);

    char trailing[HUGE_BUF];
    require(snprintf(VS(trailing), "%s/trailing", directory) < (int)sizeof(trailing));
    static const char trailing_secret[] = "secret\nother\n";
    require(path_secret_create_atomic(trailing, trailing_secret, sizeof(trailing_secret) - 1U) ==
            PATH_SECRET_CREATE_OK);
#ifndef WIN32
    require(chmod(trailing, 0600) == 0);
#endif
    memset(secret, 'x', sizeof(secret));
    require(path_read_secret(trailing, VS(secret), NULL) == PATH_SECRET_TRAILING_DATA);
    require(CRYPTO_memcmp(secret, cleared, sizeof(secret)) == 0);

    char atomic[HUGE_BUF];
    require(snprintf(VS(atomic), "%s/atomic", directory) < (int)sizeof(atomic));
    static const char atomic_data[] = "atomic-data";
    require(path_write_atomic_existing(atomic, atomic_data, sizeof(atomic_data) - 1U, 0600));
    char *atomic_contents = path_file_contents(atomic);
    require(atomic_contents != NULL && strcmp(atomic_contents, atomic_data) == 0);
    free(atomic_contents);

#ifndef WIN32
    char unsynced[HUGE_BUF];
    require(snprintf(VS(unsynced), "%s/unsynced", directory) < (int)sizeof(unsynced));
    fail_parent_directory_fsync = true;
    fsync_call_count = 0;
    require(!path_write_atomic_existing(unsynced, atomic_data, sizeof(atomic_data) - 1U, 0600));
    fail_parent_directory_fsync = false;
    require(path_exists(unsynced));
    unlink(unsynced);

    char unclosed[HUGE_BUF];
    require(snprintf(VS(unclosed), "%s/unclosed", directory) < (int)sizeof(unclosed));
    fail_parent_directory_close = true;
    fsync_call_count = 0;
    require(!path_write_atomic_existing(unclosed, atomic_data, sizeof(atomic_data) - 1U, 0600));
    fail_parent_directory_close = false;
    require(path_exists(unclosed));
    unlink(unclosed);
#endif

    char missing_parent[HUGE_BUF];
    require(snprintf(VS(missing_parent), "%s/missing/atomic", directory) <
            (int)sizeof(missing_parent));
    require(
        !path_write_atomic_existing(missing_parent, atomic_data, sizeof(atomic_data) - 1U, 0600));
    require(path_exists(missing_parent) == 0);

#ifndef WIN32
    require(chmod(path, 0640) == 0);
    permissive = false;
    require(path_read_secret(path, VS(secret), &permissive) == PATH_SECRET_OK && permissive);
    require(chmod(path, 0600) == 0);

    char link_path[HUGE_BUF];
    require(snprintf(VS(link_path), "%s/link", directory) < (int)sizeof(link_path));
    require(symlink(path, link_path) == 0);
    memset(secret, 'x', sizeof(secret));
    require(path_read_secret(link_path, VS(secret), NULL) == PATH_SECRET_UNSAFE_LINK);
    require(CRYPTO_memcmp(secret, cleared, sizeof(secret)) == 0);

    char fifo_path[HUGE_BUF];
    require(snprintf(VS(fifo_path), "%s/fifo", directory) < (int)sizeof(fifo_path));
    require(mkfifo(fifo_path, 0600) == 0);
    require(path_read_secret(fifo_path, VS(secret), NULL) == PATH_SECRET_NOT_REGULAR);

    char wrong_owner[HUGE_BUF];
    require(snprintf(VS(wrong_owner), "%s/wrong-owner", directory) < (int)sizeof(wrong_owner));
    setup_file(wrong_owner, "secret\n");
    require(chmod(wrong_owner, 0600) == 0);
    bool changed_owner = geteuid() == 0 && chown(wrong_owner, 1, (gid_t)-1) == 0;
    if (changed_owner) {
        require(path_read_secret(wrong_owner, VS(secret), NULL) == PATH_SECRET_WRONG_OWNER);
    }

    char orphan[HUGE_BUF];
    require(snprintf(VS(orphan), "%s/.atrinik-secret-orphan", directory) < (int)sizeof(orphan));
    setup_file(orphan, "orphan");
    char independent[HUGE_BUF];
    require(snprintf(VS(independent), "%s/independent", directory) < (int)sizeof(independent));
    require(path_secret_create_atomic(independent, secret_data, sizeof(secret_data) - 1U) ==
            PATH_SECRET_CREATE_OK);

    unlink(independent);
    unlink(orphan);
    unlink(wrong_owner);
    unlink(fifo_path);
    unlink(link_path);
#endif
    unlink(atomic);
    unlink(trailing);
    unlink(too_long);
    unlink(path);
#ifdef WIN32
    unlink(broad);
    unlink(not_directory);
    require(RemoveDirectoryA(prepared));
    require(RemoveDirectoryA(directory));
#else
    unlink(not_directory);
    require(rmdir(prepared) == 0);
    require(rmdir(directory) == 0);
#endif
    OPENSSL_cleanse(secret, sizeof(secret));
    toolkit_deinit();
    return 0;
}
