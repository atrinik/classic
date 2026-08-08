#include <toolkit/path.h>

#include <openssl/crypto.h>

#ifndef WIN32
#include <sys/stat.h>
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

static void setup_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "wb");
    require(fp != NULL);
    require(fwrite(contents, 1, strlen(contents), fp) == strlen(contents));
    require(fclose(fp) == 0);
}

int main(void) {
    toolkit_import(path);

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
    setup_file(broad, "broad-secret\n");
    permissive = false;
    require(path_read_secret(broad, VS(secret), &permissive) == PATH_SECRET_OK);
    require(strcmp(secret, "broad-secret") == 0 && permissive);

    char link_path[HUGE_BUF];
    require(snprintf(VS(link_path), "%s/link", directory) < (int)sizeof(link_path));
    int target_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    int link_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link_path, -1, NULL, 0);
    wchar_t *target_wide =
        target_length > 0 ? calloc((size_t)target_length, sizeof(wchar_t)) : NULL;
    wchar_t *link_wide = link_length > 0 ? calloc((size_t)link_length, sizeof(wchar_t)) : NULL;
    require(target_wide != NULL && link_wide != NULL);
    require(
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, target_wide, target_length) ==
        target_length);
    require(
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link_path, -1, link_wide, link_length) ==
        link_length);
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
    setup_file(too_long, "12345678");
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
    setup_file(trailing, "secret\nother\n");
#ifndef WIN32
    require(chmod(trailing, 0600) == 0);
#endif
    memset(secret, 'x', sizeof(secret));
    require(path_read_secret(trailing, VS(secret), NULL) == PATH_SECRET_TRAILING_DATA);
    require(CRYPTO_memcmp(secret, cleared, sizeof(secret)) == 0);

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
    unlink(trailing);
    unlink(too_long);
    unlink(path);
#ifdef WIN32
    unlink(broad);
    require(RemoveDirectoryA(directory));
#else
    require(rmdir(directory) == 0);
#endif
    OPENSSL_cleanse(secret, sizeof(secret));
    toolkit_deinit();
    return 0;
}
