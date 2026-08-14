/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/** @file Durable, private gameplay audit journal implementation. */

#include <global.h>
#include <gameplay_journal.h>
#include <player.h>
#include <object.h>
#include <toolkit/path.h>
#include <toolkit/string.h>
#include <toolkit/stringbuffer.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef WIN32
#include <io.h>
#include <windows.h>
#endif

#define JOURNAL_DIRECTORY "gameplay-journal"
#define JOURNAL_FILE_LIMIT (8U * 1024U * 1024U)
#define JOURNAL_FILE_HARD_LIMIT (9U * 1024U * 1024U)
#define JOURNAL_RETENTION_FILES 16U
#define JOURNAL_PENDING_LIMIT 64U

typedef struct pending_transaction {
    char id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    char reason[GAMEPLAY_JOURNAL_ID_MAX + 1];
    char player_ids[GAMEPLAY_JOURNAL_DOMAIN_LIMIT][GAMEPLAY_JOURNAL_ID_MAX + 1];
    object *players[GAMEPLAY_JOURNAL_DOMAIN_LIMIT];
    tag_t player_counts[GAMEPLAY_JOURNAL_DOMAIN_LIMIT];
    size_t player_count;
    char map_ids[GAMEPLAY_JOURNAL_DOMAIN_LIMIT][GAMEPLAY_JOURNAL_ID_MAX + 1];
    mapstruct *maps[GAMEPLAY_JOURNAL_DOMAIN_LIMIT];
    tag_t map_counts[GAMEPLAY_JOURNAL_DOMAIN_LIMIT];
    bool map_unique[GAMEPLAY_JOURNAL_DOMAIN_LIMIT];
    size_t map_count;
} pending_transaction_t;

typedef struct journal_state {
    FILE *fp;
    int lock_fd;
    char directory[HUGE_BUF];
    char server_id[GAMEPLAY_JOURNAL_ID_MAX + 1];
    char run_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE];
    char previous_hash[EVP_MAX_MD_SIZE * 2U + 1U];
    gameplay_journal_profile_t profile;
    char profile_id[GAMEPLAY_JOURNAL_ID_MAX + 1];
    char profile_digest[GAMEPLAY_JOURNAL_ID_MAX + 1];
    char profile_axes[GAMEPLAY_JOURNAL_ID_MAX + 1];
    uint64_t sequence;
    uint32_t file_index;
    size_t file_size;
    pending_transaction_t pending[JOURNAL_PENDING_LIMIT];
    size_t pending_count;
    bool failed;
    bool required;
} journal_state_t;

static journal_state_t journal = {.lock_fd = -1};

#ifdef ATRINIK_TESTING
static bool journal_test_fail_writes;
static size_t journal_test_writes_before_failure = SIZE_MAX;
static size_t journal_test_file_limit = JOURNAL_FILE_LIMIT;
static size_t journal_test_hard_limit = JOURNAL_FILE_HARD_LIMIT;

typedef struct journal_test_count {
    char reason[GAMEPLAY_JOURNAL_ID_MAX + 1];
    uint64_t count;
} journal_test_count_t;

static journal_test_count_t journal_test_counts[JOURNAL_PENDING_LIMIT];

void gameplay_journal_fail_writes_for_test(bool fail) {
    journal_test_fail_writes = fail;
}

void gameplay_journal_fail_after_writes_for_test(size_t writes) {
    journal_test_writes_before_failure = writes;
}

uint64_t gameplay_journal_committed_count_for_test(const char *reason) {
    for (size_t i = 0; i < JOURNAL_PENDING_LIMIT; i++) {
        if (strcmp(journal_test_counts[i].reason, reason) == 0) {
            return journal_test_counts[i].count;
        }
    }
    return 0;
}

void gameplay_journal_counts_reset_for_test(void) {
    memset(journal_test_counts, 0, sizeof(journal_test_counts));
}

void gameplay_journal_file_limit_for_test(size_t limit) {
    journal_test_file_limit = limit;
}

void gameplay_journal_hard_limit_for_test(size_t limit) {
    journal_test_hard_limit = limit;
}
#endif

static size_t journal_file_limit(void) {
#ifdef ATRINIK_TESTING
    return journal_test_file_limit;
#else
    return JOURNAL_FILE_LIMIT;
#endif
}

static size_t journal_hard_limit(void) {
#ifdef ATRINIK_TESTING
    return journal_test_hard_limit;
#else
    return JOURNAL_FILE_HARD_LIMIT;
#endif
}

typedef struct retained_file {
    char path[HUGE_BUF];
    time_t modified;
} retained_file_t;

static bool journal_token_valid(const char *value, bool allow_empty) {
    if (value == NULL) {
        return allow_empty;
    }
    size_t length = strlen(value);
    if (length == 0 || length > GAMEPLAY_JOURNAL_ID_MAX) {
        return allow_empty && length == 0;
    }
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        bool alphanumeric =
            (*cp >= 'a' && *cp <= 'z') || (*cp >= 'A' && *cp <= 'Z') || (*cp >= '0' && *cp <= '9');
        if (!alphanumeric && strchr("_.:/@+-", *cp) == NULL) {
            return false;
        }
    }
    return true;
}

static bool journal_map_id_valid(const char *value) {
    if (value == NULL || strlen(value) > GAMEPLAY_JOURNAL_ID_MAX) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        bool alphanumeric =
            (*cp >= 'a' && *cp <= 'z') || (*cp >= 'A' && *cp <= 'Z') || (*cp >= '0' && *cp <= '9');
        if (!alphanumeric && strchr("_.:/@+$%-", *cp) == NULL) {
            return false;
        }
    }
    return true;
}

bool gameplay_journal_map_path_identity(const char *path,
                                        char output[GAMEPLAY_JOURNAL_ID_MAX + 1]) {
    if (path == NULL || path[0] == '\0') {
        output[0] = '\0';
        return true;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *cp = (const unsigned char *)path; *cp != '\0'; cp++) {
        bool alphanumeric =
            (*cp >= 'a' && *cp <= 'z') || (*cp >= 'A' && *cp <= 'Z') || (*cp >= '0' && *cp <= '9');
        if (alphanumeric || strchr("_.:/@+$-", *cp) != NULL) {
            if (used == GAMEPLAY_JOURNAL_ID_MAX) {
                return false;
            }
            output[used++] = (char)*cp;
        } else {
            if (used > GAMEPLAY_JOURNAL_ID_MAX - 3) {
                return false;
            }
            output[used++] = '%';
            output[used++] = hex[*cp >> 4];
            output[used++] = hex[*cp & 0x0f];
        }
    }
    output[used] = '\0';
    return used != 0;
}

bool gameplay_journal_map_identity(const mapstruct *map, char output[GAMEPLAY_JOURNAL_ID_MAX + 1]) {
    if (map == NULL) {
        output[0] = '\0';
        return true;
    }
    if (map->path == NULL || map->path[0] == '\0') {
        return snprintf(output, GAMEPLAY_JOURNAL_ID_MAX + 1, "runtime:%" PRIu32, map->count) > 0;
    }
    return gameplay_journal_map_path_identity(map->path, output);
}

static bool journal_identity_valid(const char *value) {
    if (value == NULL) {
        return false;
    }
    size_t length = strlen(value);
    if (length == 0 || length > GAMEPLAY_JOURNAL_ID_MAX) {
        return false;
    }
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if (*cp < 0x20 || *cp > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool journal_identity_valid_optional(const char *value) {
    return value == NULL || value[0] == '\0' || journal_identity_valid(value);
}

static bool journal_item_snapshot_valid(const gameplay_journal_change_t *change) {
    if (change->snapshot == NULL || change->archetype == NULL || change->object_type < 0) {
        return false;
    }
    char prefix[GAMEPLAY_JOURNAL_ID_MAX + 1];
    int prefix_length = snprintf(VS(prefix),
                                 "arch=%s;type=%" PRId32 ";nrof=",
                                 change->archetype,
                                 change->object_type);
    if (prefix_length < 0 || prefix_length >= (int)sizeof(prefix) ||
        strncmp(change->snapshot, prefix, (size_t)prefix_length) != 0) {
        return false;
    }
    const char *cp = change->snapshot + prefix_length;
    char *end;
    errno = 0;
    uintmax_t nrof = strtoumax(cp, &end, 10);
    if (errno != 0 || end == cp || *end != ';' || nrof == 0 || nrof > UINT32_MAX) {
        return false;
    }
    cp = end;
    if (strncmp(cp, ";value=", 7) != 0) {
        return false;
    }
    cp += 7;
    errno = 0;
    intmax_t value = strtoimax(cp, &end, 10);
    if (errno != 0 || end == cp || strncmp(end, ";weight=", 8) != 0) {
        return false;
    }
    cp = end + 8;
    errno = 0;
    uintmax_t weight = strtoumax(cp, &end, 10);
    if (errno != 0 || end == cp || *end != '\0' || weight > UINT32_MAX) {
        return false;
    }
    char expected[GAMEPLAY_JOURNAL_ID_MAX + 1];
    int expected_length = snprintf(VS(expected),
                                   "%s%" PRIuMAX ";value=%" PRIdMAX ";weight=%" PRIuMAX,
                                   prefix,
                                   nrof,
                                   value,
                                   weight);
    return expected_length >= 0 && expected_length < (int)sizeof(expected) &&
           strcmp(change->snapshot, expected) == 0;
}

static bool journal_item_provenance_valid(const char *value) {
    static const char prefix[] = "first=";
    static const char separator[] = ";last=";
    if (value == NULL || strncmp(value, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *first = value + sizeof(prefix) - 1;
    const char *last = strstr(first, separator);
    if (last == NULL || strstr(last + sizeof(separator) - 1, separator) != NULL) {
        return false;
    }
    size_t first_length = (size_t)(last - first);
    const char *last_value = last + sizeof(separator) - 1;
    size_t last_length = strlen(last_value);
    if (first_length > 112 || last_length > 112 || memchr(first, ';', first_length) != NULL ||
        strchr(last_value, ';') != NULL) {
        return false;
    }
    char first_identity[113], last_identity[113];
    memcpy(first_identity, first, first_length);
    first_identity[first_length] = '\0';
    memcpy(last_identity, last_value, last_length + 1);
    return journal_identity_valid_optional(first_identity) &&
           journal_identity_valid_optional(last_identity);
}

static void journal_append_json_string(StringBuffer *record, const char *value) {
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if (*cp == '"' || *cp == '\\') {
            stringbuffer_append_char(record, '\\');
        }
        stringbuffer_append_char(record, (char)*cp);
    }
}

static bool journal_change_valid(gameplay_journal_kind_t kind,
                                 const gameplay_journal_change_t *change) {
    if (change->delta > 0 && change->before > INT64_MAX - change->delta) {
        return false;
    }
    if (change->delta < 0 && change->before < INT64_MIN - change->delta) {
        return false;
    }
    if (change->before + change->delta != change->after || change->price < 0 ||
        !journal_token_valid(change->archetype, true) ||
        !journal_identity_valid_optional(change->snapshot) ||
        !journal_token_valid(change->source, true) ||
        !journal_token_valid(change->destination, true) ||
        !journal_identity_valid_optional(change->actor) ||
        !journal_identity_valid_optional(change->counterparty) ||
        !journal_identity_valid_optional(change->provenance_before) ||
        !journal_identity_valid_optional(change->provenance_after) ||
        !journal_token_valid(change->currency, true) ||
        !journal_token_valid(change->funding, true)) {
        return false;
    }
    if (kind == GAMEPLAY_JOURNAL_ITEM &&
        (!journal_token_valid(change->lineage_id, false) ||
         !journal_token_valid(change->archetype, false) || !journal_item_snapshot_valid(change) ||
         change->quantity == 0 || !journal_token_valid(change->source, false) ||
         !journal_token_valid(change->destination, false) || change->actor == NULL ||
         change->actor[0] == '\0' || change->provenance_before == NULL ||
         !journal_item_provenance_valid(change->provenance_before) ||
         change->provenance_after == NULL ||
         !journal_item_provenance_valid(change->provenance_after))) {
        return false;
    }
    if (kind == GAMEPLAY_JOURNAL_CURRENCY &&
        (!journal_token_valid(change->source, false) ||
         !journal_token_valid(change->destination, false) || change->actor == NULL ||
         change->actor[0] == '\0' || !journal_token_valid(change->currency, false) ||
         !journal_token_valid(change->funding, false))) {
        return false;
    }
    return true;
}

static bool journal_random_id(char output[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1 ||
        string_tohex(random, sizeof(random), output, GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE, false) !=
            sizeof(random) * 2U) {
        OPENSSL_cleanse(random, sizeof(random));
        return false;
    }
    OPENSSL_cleanse(random, sizeof(random));
    string_tolower(output);
    return true;
}

static int journal_retained_compare(const void *left, const void *right) {
    const retained_file_t *a = left;
    const retained_file_t *b = right;
    if (a->modified < b->modified) {
        return -1;
    }
    if (a->modified > b->modified) {
        return 1;
    }
    return strcmp(a->path, b->path);
}

static bool journal_filename_valid(const char *name) {
    static const char prefix[] = "journal-";
    size_t length = strlen(name);
    if (length < sizeof(prefix) - 1 + 32 + 1 + 4 + 6 ||
        strncmp(name, prefix, sizeof(prefix) - 1) != 0 ||
        strcmp(name + length - 6, ".jsonl") != 0) {
        return false;
    }
    const char *run = name + sizeof(prefix) - 1;
    for (size_t i = 0; i < 32; i++) {
        if (!((run[i] >= '0' && run[i] <= '9') || (run[i] >= 'a' && run[i] <= 'f'))) {
            return false;
        }
    }
    if (run[32] != '-') {
        return false;
    }
    size_t digits = length - (sizeof(prefix) - 1 + 32 + 1 + 6);
    if (digits < 4) {
        return false;
    }
    for (size_t i = 0; i < digits; i++) {
        if (run[33 + i] < '0' || run[33 + i] > '9') {
            return false;
        }
    }
    return true;
}

static bool journal_sync_directory(void) {
#ifdef WIN32
    return true;
#else
    int fd = open(journal.directory, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
#endif
}

static bool journal_lock(void) {
    char path[HUGE_BUF];
    if (snprintf(VS(path), "%s/journal.lock", journal.directory) >= (int)sizeof(path)) {
        return false;
    }
#ifdef WIN32
    HANDLE handle = CreateFileA(path,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    BY_HANDLE_FILE_INFORMATION metadata;
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, &metadata) ||
        (metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        return false;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR);
    if (fd < 0) {
        CloseHandle(handle);
        return false;
    }
#else
    int flags = O_RDWR | O_CREAT;
    flags |= O_NOFOLLOW;
    int fd = open(path, flags, SAVE_MODE);
    if (fd < 0) {
        return false;
    }
    struct stat metadata;
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        (metadata.st_mode & 0777) != SAVE_MODE) {
        close(fd);
        return false;
    }
#endif
#ifdef WIN32
    OVERLAPPED overlapped = {0};
    bool ok = LockFileEx(handle,
                         LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                         0,
                         1,
                         0,
                         &overlapped) != 0;
#else
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    bool ok = fcntl(fd, F_SETLK, &lock) == 0;
#endif
    if (!ok) {
        close(fd);
        return false;
    }
    journal.lock_fd = fd;
    return journal_sync_directory();
}

static bool journal_counter_seek_start(void) {
#ifdef WIN32
    return _lseeki64(journal.lock_fd, 0, SEEK_SET) == 0;
#else
    return lseek(journal.lock_fd, 0, SEEK_SET) == 0;
#endif
}

static bool journal_counter_bootstrap(uint64_t *sequence) {
    DIR *dir = opendir(journal.directory);
    if (dir == NULL) {
        return false;
    }
    *sequence = 0;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!journal_filename_valid(entry->d_name)) {
            continue;
        }
        char path[HUGE_BUF];
        if (snprintf(VS(path), "%s/%s", journal.directory, entry->d_name) >= (int)sizeof(path)) {
            ok = false;
            break;
        }
        struct stat metadata;
#ifdef WIN32
        DWORD attributes = GetFileAttributesA(path);
        int status = stat(path, &metadata);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ok = false;
            break;
        }
#else
        int status = lstat(path, &metadata);
#endif
        if (status != 0 || !S_ISREG(metadata.st_mode)
#ifndef WIN32
            || (metadata.st_mode & 0777) != SAVE_MODE
#endif
        ) {
            ok = false;
            break;
        }
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
            ok = false;
            break;
        }
        char line[HUGE_BUF * 4];
        while (fgets(line, sizeof(line), fp) != NULL) {
            size_t length = strlen(line);
            if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
                ok = false;
                break;
            }
            const char *field = strstr(line, "\"sequence\":");
            if (field == NULL) {
                ok = false;
                break;
            }
            field += strlen("\"sequence\":");
            errno = 0;
            char *end = NULL;
            uintmax_t parsed = strtoumax(field, &end, 10);
            if (errno != 0 || parsed > UINT64_MAX || end == field || (*end != ',' && *end != '}')) {
                ok = false;
                break;
            }
            if (parsed > *sequence) {
                *sequence = (uint64_t)parsed;
            }
        }
        if (ferror(fp) || fclose(fp) != 0) {
            ok = false;
        }
        if (!ok) {
            break;
        }
    }
    if (closedir(dir) != 0) {
        ok = false;
    }
    return ok;
}

static bool journal_counter_load(void) {
    char value[32] = {0};
    if (!journal_counter_seek_start()) {
        return false;
    }
#ifdef WIN32
    int length = _read(journal.lock_fd, value, sizeof(value) - 1);
#else
    ssize_t length = read(journal.lock_fd, value, sizeof(value) - 1);
#endif
    if (length < 0) {
        return false;
    }
    if (length == 0) {
        return journal_counter_bootstrap(&journal.sequence);
    }
    value[length] = '\0';
    errno = 0;
    char *end = NULL;
    uintmax_t sequence = strtoumax(value, &end, 10);
    if (errno != 0 || sequence > UINT64_MAX || end == value || (*end != '\n' && *end != '\0') ||
        (*end == '\n' && end[1] != '\0')) {
        return false;
    }
    journal.sequence = (uint64_t)sequence;
    return true;
}

static bool journal_counter_reserve(void) {
    if (journal.sequence == UINT64_MAX) {
        return false;
    }
    uint64_t sequence = journal.sequence + 1;
    char value[32];
    int length = snprintf(VS(value), "%" PRIu64 "\n", sequence);
    if (length <= 0 || length >= (int)sizeof(value) || !journal_counter_seek_start()) {
        return false;
    }
#ifdef WIN32
    bool ok = _write(journal.lock_fd, value, length) == length &&
              _chsize_s(journal.lock_fd, (size_t)length) == 0 && _commit(journal.lock_fd) == 0;
#else
    bool ok = write(journal.lock_fd, value, (size_t)length) == length &&
              ftruncate(journal.lock_fd, length) == 0 && fsync(journal.lock_fd) == 0;
#endif
    if (ok) {
        journal.sequence = sequence;
    }
    return ok;
}

static bool journal_enforce_retention(void) {
    DIR *dir = opendir(journal.directory);
    if (dir == NULL) {
        return false;
    }
    retained_file_t *files = NULL;
    size_t count = 0;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!journal_filename_valid(entry->d_name)) {
            continue;
        }
        retained_file_t file;
        if (snprintf(VS(file.path), "%s/%s", journal.directory, entry->d_name) >=
            (int)sizeof(file.path)) {
            ok = false;
            break;
        }
        struct stat metadata;
#ifdef WIN32
        DWORD attributes = GetFileAttributesA(file.path);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ok = false;
            break;
        }
        int status = stat(file.path, &metadata);
#else
        int status = lstat(file.path, &metadata);
#endif
        if (status != 0 || !S_ISREG(metadata.st_mode)
#ifndef WIN32
            || (metadata.st_mode & 0777) != SAVE_MODE
#endif
        ) {
            ok = false;
            break;
        }
        file.modified = metadata.st_mtime;
        retained_file_t *grown = realloc(files, (count + 1) * sizeof(*files));
        if (grown == NULL) {
            ok = false;
            break;
        }
        files = grown;
        files[count++] = file;
    }
    if (closedir(dir) != 0) {
        ok = false;
    }
    bool removed = false;
    if (ok && count >= JOURNAL_RETENTION_FILES) {
        qsort(files, count, sizeof(*files), journal_retained_compare);
        size_t remove_count = count - JOURNAL_RETENTION_FILES + 1;
        for (size_t i = 0; i < remove_count; i++) {
            if (unlink(files[i].path) != 0) {
                ok = false;
                break;
            }
            removed = true;
        }
    }
    if (ok && removed) {
        ok = journal_sync_directory();
    }
    free(files);
    return ok;
}

static bool journal_utc(char output[32]) {
    time_t now = time(NULL);
    struct tm utc;
#ifdef WIN32
    if (gmtime_s(&utc, &now) != 0) {
        return false;
    }
#else
    if (gmtime_r(&now, &utc) == NULL) {
        return false;
    }
#endif
    return strftime(output, 32, "%Y-%m-%dT%H:%M:%SZ", &utc) != 0;
}

static bool journal_digest(const void *data, size_t length, char output[65]) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (EVP_Digest(data, length, digest, &digest_length, EVP_sha256(), NULL) != 1 ||
        digest_length != 32 ||
        string_tohex(digest, digest_length, output, 65, false) != digest_length * 2U) {
        OPENSSL_cleanse(digest, sizeof(digest));
        return false;
    }
    OPENSSL_cleanse(digest, sizeof(digest));
    string_tolower(output);
    return true;
}

static bool journal_sync(void) {
    if (fflush(journal.fp) != 0) {
        return false;
    }
#ifdef WIN32
    return _commit(_fileno(journal.fp)) == 0;
#else
    return fsync(fileno(journal.fp)) == 0;
#endif
}

static bool journal_open_file(void) {
    if (!journal_enforce_retention()) {
        return false;
    }
    char path[HUGE_BUF];
    if (snprintf(VS(path),
                 "%s/journal-%s-%04" PRIu32 ".jsonl",
                 journal.directory,
                 journal.run_id,
                 journal.file_index) >= (int)sizeof(path)) {
        return false;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef WIN32
    flags |= O_BINARY;
#endif
    int fd = open(path, flags, SAVE_MODE);
    if (fd < 0) {
        return false;
    }
    journal.fp = fdopen(fd, "wb");
    if (journal.fp == NULL) {
        close(fd);
        unlink(path);
        return false;
    }
    if (!journal_sync_directory()) {
        fclose(journal.fp);
        journal.fp = NULL;
        return false;
    }
    journal.file_size = 0;
    return true;
}

static bool journal_rotate(size_t next_size) {
    if (journal.fp != NULL && journal.file_size <= journal_file_limit() &&
        next_size <= journal_file_limit() - journal.file_size) {
        return true;
    }
    if (journal.fp != NULL && journal.pending_count != 0) {
        return journal.file_size <= journal_hard_limit() &&
               next_size <= journal_hard_limit() - journal.file_size;
    }
    if (journal.fp != NULL) {
        if (!journal_sync() || fclose(journal.fp) != 0) {
            journal.fp = NULL;
            return false;
        }
        journal.fp = NULL;
        journal.file_index++;
    }
    return journal_open_file();
}

static const char *journal_kind_name(gameplay_journal_kind_t kind) {
    switch (kind) {
        case GAMEPLAY_JOURNAL_ITEM:
            return "item";
        case GAMEPLAY_JOURNAL_CURRENCY:
            return "currency";
        case GAMEPLAY_JOURNAL_QUEST:
            return "quest";
        case GAMEPLAY_JOURNAL_PROGRESSION:
            return "progression";
        default:
            return NULL;
    }
}

static bool journal_append(StringBuffer *record) {
    if (journal.failed || journal.fp == NULL) {
        return false;
    }
    char hash[65];
    if (!journal_digest(stringbuffer_data(record), stringbuffer_length(record), hash)) {
        journal.failed = true;
        return false;
    }
    stringbuffer_append_printf(record, ",\"record_hash\":\"%s\"}\n", hash);
    size_t length = stringbuffer_length(record);
    bool forced_failure = false;
#ifdef ATRINIK_TESTING
    forced_failure = journal_test_fail_writes || journal_test_writes_before_failure == 0;
    if (!forced_failure && journal_test_writes_before_failure != SIZE_MAX) {
        journal_test_writes_before_failure--;
    }
#endif
    if (forced_failure || !journal_rotate(length) ||
        fwrite(stringbuffer_data(record), 1, length, journal.fp) != length || !journal_sync()) {
        journal.failed = true;
        LOG(ERROR,
            "Gameplay journal write failed; refusing further journal-backed gameplay "
            "transactions.");
        return false;
    }
    journal.file_size += length;
    memcpy(journal.previous_hash, hash, sizeof(hash));
    return true;
}

static StringBuffer *journal_record(const char *phase,
                                    const char *transaction_id,
                                    const char *kind,
                                    const char *reason) {
    char utc[32];
    if (!journal_utc(utc)) {
        journal.failed = true;
        LOG(ERROR, "Gameplay journal could not obtain a UTC record timestamp.");
        return NULL;
    }
    if (!journal_counter_reserve()) {
        journal.failed = true;
        LOG(ERROR, "Gameplay journal could not reserve its global sequence.");
        return NULL;
    }
    StringBuffer *record = stringbuffer_new();
    stringbuffer_append_printf(record,
                               "{\"version\":%d,\"event_id\":\"%s:%s:%" PRIu64
                               "\",\"transaction_id\":\"%s\",\"sequence\":%" PRIu64
                               ",\"utc\":\"%s\",\"server_id\":\"%s\",\"run_id\":\"%s\""
                               ",\"phase\":\"%s\",\"kind\":\"%s\",\"reason\":\"%s\""
                               ",\"profile\":{\"id\":\"%s\",\"schema\":%" PRIu32
                               ",\"digest\":\"%s\",\"effective_axes\":\"%s\"}"
                               ",\"prev_hash\":\"%s\"",
                               GAMEPLAY_JOURNAL_SCHEMA_VERSION,
                               journal.server_id,
                               journal.run_id,
                               journal.sequence,
                               transaction_id != NULL ? transaction_id : "",
                               journal.sequence,
                               utc,
                               journal.server_id,
                               journal.run_id,
                               phase,
                               kind,
                               reason,
                               journal.profile.id,
                               journal.profile.schema,
                               journal.profile.digest,
                               journal.profile.effective_axes,
                               journal.previous_hash);
    return record;
}

static bool journal_profile_copy(const gameplay_journal_profile_t *profile) {
    if (profile == NULL || !journal_token_valid(profile->id, false) ||
        !journal_token_valid(profile->digest, false) ||
        !journal_token_valid(profile->effective_axes, false)) {
        return false;
    }
    snprintf(VS(journal.profile_id), "%s", profile->id);
    snprintf(VS(journal.profile_digest), "%s", profile->digest);
    snprintf(VS(journal.profile_axes), "%s", profile->effective_axes);
    journal.profile = (gameplay_journal_profile_t){.id = journal.profile_id,
                                                   .schema = profile->schema,
                                                   .digest = journal.profile_digest,
                                                   .effective_axes = journal.profile_axes};
    return true;
}

bool gameplay_journal_init(const char *datapath,
                           const char *server_id,
                           const gameplay_journal_profile_t *profile) {
    gameplay_journal_deinit();
    journal.required = true;
    if (datapath == NULL || !journal_token_valid(server_id, false) ||
        !journal_profile_copy(profile) ||
        snprintf(VS(journal.directory), "%s/%s", datapath, JOURNAL_DIRECTORY) >=
            (int)sizeof(journal.directory) ||
        path_ensure_real_directory(journal.directory, SAVE_MODE_DIR) != PATH_DIRECTORY_OK) {
        journal.failed = true;
        return false;
    }
#ifndef WIN32
    struct stat metadata;
    if (lstat(journal.directory, &metadata) != 0 || (metadata.st_mode & 0777) != SAVE_MODE_DIR) {
        journal.failed = true;
        return false;
    }
#endif
    if (!journal_lock() || !journal_counter_load() || !journal_random_id(journal.run_id)) {
        journal.failed = true;
        return false;
    }
    snprintf(VS(journal.server_id), "%s", server_id);
    if (!journal_open_file()) {
        journal.failed = true;
        return false;
    }
    StringBuffer *record = journal_record("boundary", NULL, "run", "run.start");
    if (record == NULL || !journal_append(record)) {
        if (record != NULL) {
            stringbuffer_free(record);
        }
        return false;
    }
    stringbuffer_free(record);
    return true;
}

void gameplay_journal_deinit(void) {
    if (journal.fp != NULL) {
        if (!journal_sync() || fclose(journal.fp) != 0) {
            LOG(ERROR, "Gameplay journal could not complete its shutdown flush.");
        }
    }
    if (journal.lock_fd >= 0) {
#ifdef WIN32
        OVERLAPPED overlapped = {0};
        HANDLE handle = (HANDLE)_get_osfhandle(journal.lock_fd);
        if (handle != INVALID_HANDLE_VALUE) {
            (void)UnlockFileEx(handle, 0, 1, 0, &overlapped);
        }
#else
        struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
        (void)fcntl(journal.lock_fd, F_SETLK, &lock);
#endif
        (void)close(journal.lock_fd);
    }
    memset(&journal, 0, sizeof(journal));
    journal.lock_fd = -1;
}

bool gameplay_journal_available(void) {
    return journal.fp != NULL && !journal.failed;
}

bool gameplay_journal_required(void) {
    return journal.required;
}

static ssize_t journal_pending_find(const char *transaction_id) {
    for (size_t i = 0; i < journal.pending_count; i++) {
        if (strcmp(journal.pending[i].id, transaction_id) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void journal_pending_remove(size_t index) {
    HARD_ASSERT(index < journal.pending_count);
    journal.pending[index] = journal.pending[journal.pending_count - 1];
    memset(&journal.pending[journal.pending_count - 1], 0, sizeof(journal.pending[0]));
    journal.pending_count--;
}

static void journal_pending_clear(void) {
    memset(journal.pending, 0, sizeof(journal.pending));
    journal.pending_count = 0;
}

static bool journal_player_domain_id(const char *account,
                                     const char *character,
                                     char id[GAMEPLAY_JOURNAL_ID_MAX + 1]) {
    int length = snprintf(id, GAMEPLAY_JOURNAL_ID_MAX + 1, "%s/%s", account, character);
    return length >= 0 && length <= GAMEPLAY_JOURNAL_ID_MAX;
}

static bool journal_append_domain(const char *transaction_id, const char *kind, const char *id) {
    if (!journal_token_valid(transaction_id, false) || !journal_token_valid(kind, false) ||
        !journal_identity_valid(id)) {
        return false;
    }
    StringBuffer *record = journal_record("domain", transaction_id, "transaction", "domain.add");
    if (record == NULL) {
        if (!gameplay_journal_available()) {
            journal_pending_clear();
        }
        return false;
    }
    stringbuffer_append_string(record, ",\"domain\":{\"kind\":\"");
    journal_append_json_string(record, kind);
    stringbuffer_append_string(record, "\",\"id\":\"");
    journal_append_json_string(record, id);
    stringbuffer_append_string(record, "\"}");
    bool ok = journal_append(record);
    stringbuffer_free(record);
    if (!ok && !gameplay_journal_available()) {
        journal_pending_clear();
    }
    return ok;
}

bool gameplay_journal_track_player(const char *transaction_id, object *player_ob) {
    ssize_t index = journal_pending_find(transaction_id);
    if (index < 0 || player_ob == NULL || player_ob->type != PLAYER || CONTR(player_ob) == NULL ||
        CONTR(player_ob)->cs == NULL || CONTR(player_ob)->cs->account == NULL ||
        player_ob->name == NULL) {
        return false;
    }
    char id[GAMEPLAY_JOURNAL_ID_MAX + 1];
    if (!journal_player_domain_id(CONTR(player_ob)->cs->account, player_ob->name, id)) {
        return false;
    }
    pending_transaction_t *pending = &journal.pending[(size_t)index];
    for (size_t i = 0; i < pending->player_count; i++) {
        if (strcmp(pending->player_ids[i], id) == 0) {
            pending->players[i] = player_ob;
            pending->player_counts[i] = player_ob->count;
            return true;
        }
    }
    if (pending->player_count == GAMEPLAY_JOURNAL_DOMAIN_LIMIT) {
        return false;
    }
    if (!journal_append_domain(transaction_id, "player", id)) {
        return false;
    }
    snprintf(VS(pending->player_ids[pending->player_count]), "%s", id);
    pending->players[pending->player_count] = player_ob;
    pending->player_counts[pending->player_count] = player_ob->count;
    pending->player_count++;
    return true;
}

static bool journal_track_map(const char *transaction_id, mapstruct *map, bool unique_component) {
    ssize_t index = journal_pending_find(transaction_id);
    if (index < 0 || map == NULL || map->count == 0) {
        return false;
    }
    if (MAP_UNIQUE(map)) {
        unique_component = false;
    }
    pending_transaction_t *pending = &journal.pending[(size_t)index];
    char id[GAMEPLAY_JOURNAL_ID_MAX + 1];
    if (!gameplay_journal_map_identity(map, id)) {
        return false;
    }
    for (size_t i = 0; i < pending->map_count; i++) {
        if (strcmp(pending->map_ids[i], id) == 0 && pending->map_unique[i] == unique_component) {
            pending->maps[i] = map;
            pending->map_counts[i] = map->count;
            return true;
        }
    }
    if (pending->map_count == GAMEPLAY_JOURNAL_DOMAIN_LIMIT) {
        return false;
    }
    if (!journal_append_domain(transaction_id,
                               unique_component ? "map-unique" : "map-runtime",
                               id)) {
        return false;
    }
    snprintf(VS(pending->map_ids[pending->map_count]), "%s", id);
    pending->maps[pending->map_count] = map;
    pending->map_counts[pending->map_count] = map->count;
    pending->map_unique[pending->map_count] = unique_component;
    pending->map_count++;
    return true;
}

bool gameplay_journal_track_map(const char *transaction_id, mapstruct *map) {
    return journal_track_map(transaction_id, map, false);
}

bool gameplay_journal_track_map_unique(const char *transaction_id, mapstruct *map) {
    return journal_track_map(transaction_id, map, true);
}

bool gameplay_journal_track_map_object(const char *transaction_id,
                                       mapstruct *map,
                                       int x,
                                       int y,
                                       const object *op) {
    if (map == NULL || op == NULL) {
        return false;
    }
    map = get_map_from_coord(map, &x, &y);
    if (map == NULL) {
        return false;
    }
    object *floor = GET_MAP_OB_LAYER(map, x, y, LAYER_FLOOR, 0);
    bool unique = QUERY_FLAG(op, FLAG_UNIQUE) || (floor != NULL && QUERY_FLAG(floor, FLAG_UNIQUE));
    return journal_track_map(transaction_id, map, unique);
}

bool gameplay_journal_player_checkpoint_allowed(const object *player_ob) {
    if (player_ob == NULL) {
        return false;
    }
    for (size_t i = 0; i < journal.pending_count; i++) {
        const pending_transaction_t *pending = &journal.pending[i];
        for (size_t j = 0; j < pending->player_count; j++) {
            if (pending->players[j] == player_ob &&
                OBJECT_VALID(player_ob, pending->player_counts[j])) {
                return false;
            }
        }
    }
    return true;
}

bool gameplay_journal_map_checkpoint_allowed(const mapstruct *map) {
    if (map == NULL) {
        return false;
    }
    for (size_t i = 0; i < journal.pending_count; i++) {
        const pending_transaction_t *pending = &journal.pending[i];
        for (size_t j = 0; j < pending->map_count; j++) {
            if (pending->maps[j] == map && map->count == pending->map_counts[j]) {
                return false;
            }
        }
    }
    return true;
}

bool gameplay_journal_begin(const gameplay_journal_subject_t *subject,
                            gameplay_journal_kind_t kind,
                            const char *reason,
                            const gameplay_journal_change_t *change,
                            char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    const char *kind_name = journal_kind_name(kind);
    char player_domain[GAMEPLAY_JOURNAL_ID_MAX + 1];
    if (!gameplay_journal_available() || subject == NULL || change == NULL ||
        transaction_id == NULL || kind_name == NULL || !journal_token_valid(reason, false) ||
        !journal_identity_valid(subject->account_id) ||
        !journal_identity_valid(subject->character_id) || !journal_map_id_valid(subject->map_id) ||
        !((kind == GAMEPLAY_JOURNAL_QUEST || kind == GAMEPLAY_JOURNAL_PROGRESSION)
              ? (change->subject_id != NULL && change->subject_id[0] != '\0' &&
                 journal_map_id_valid(change->subject_id))
              : journal_token_valid(change->subject_id, false)) ||
        !((kind == GAMEPLAY_JOURNAL_QUEST || kind == GAMEPLAY_JOURNAL_PROGRESSION)
              ? (change->lineage_id == NULL || change->lineage_id[0] == '\0' ||
                 journal_map_id_valid(change->lineage_id))
              : journal_token_valid(change->lineage_id, true)) ||
        !journal_change_valid(kind, change) || journal.pending_count == JOURNAL_PENDING_LIMIT ||
        !journal_player_domain_id(subject->account_id, subject->character_id, player_domain) ||
        !journal_random_id(transaction_id)) {
        return false;
    }
    StringBuffer *record = journal_record("intent", transaction_id, kind_name, reason);
    if (record == NULL) {
        if (!gameplay_journal_available()) {
            journal_pending_clear();
        }
        return false;
    }
    stringbuffer_append_string(record, ",\"account_id\":\"");
    journal_append_json_string(record, subject->account_id);
    stringbuffer_append_string(record, "\",\"character_id\":\"");
    journal_append_json_string(record, subject->character_id);
    stringbuffer_append_string(record, "\",\"context\":{\"map_id\":\"");
    journal_append_json_string(record, subject->map_id != NULL ? subject->map_id : "");
    stringbuffer_append_printf(record,
                               "\",\"x\":%" PRId32 ",\"y\":%" PRId32 "}"
                               ",\"change\":{\"subject_id\":\"%s\",\"lineage_id\":\"%s\""
                               ",\"before\":%" PRId64 ",\"delta\":%" PRId64 ",\"after\":%" PRId64
                               "}"
                               ",\"details\":{\"archetype\":\"",
                               subject->x,
                               subject->y,
                               change->subject_id,
                               change->lineage_id != NULL ? change->lineage_id : "",
                               change->before,
                               change->delta,
                               change->after);
    journal_append_json_string(record, change->archetype != NULL ? change->archetype : "");
    stringbuffer_append_printf(record,
                               "\",\"object_type\":%" PRId32 ",\"snapshot\":\"",
                               change->object_type);
    journal_append_json_string(record, change->snapshot != NULL ? change->snapshot : "");
    stringbuffer_append_printf(record,
                               "\",\"quantity\":%" PRIu32 ",\"source\":\"",
                               change->quantity);
    journal_append_json_string(record, change->source != NULL ? change->source : "");
    stringbuffer_append_string(record, "\",\"destination\":\"");
    journal_append_json_string(record, change->destination != NULL ? change->destination : "");
    stringbuffer_append_string(record, "\",\"actor\":\"");
    journal_append_json_string(record, change->actor != NULL ? change->actor : "");
    stringbuffer_append_string(record, "\",\"counterparty\":\"");
    journal_append_json_string(record, change->counterparty != NULL ? change->counterparty : "");
    stringbuffer_append_string(record, "\",\"provenance_before\":\"");
    journal_append_json_string(record,
                               change->provenance_before != NULL ? change->provenance_before : "");
    stringbuffer_append_string(record, "\",\"provenance_after\":\"");
    journal_append_json_string(record,
                               change->provenance_after != NULL ? change->provenance_after : "");
    stringbuffer_append_printf(record, "\",\"price\":%" PRId64 ",\"currency\":\"", change->price);
    journal_append_json_string(record, change->currency != NULL ? change->currency : "");
    stringbuffer_append_string(record, "\",\"funding\":\"");
    journal_append_json_string(record, change->funding != NULL ? change->funding : "");
    stringbuffer_append_string(record, "\"},\"domains\":[{\"kind\":\"player\",\"id\":\"");
    journal_append_json_string(record, player_domain);
    stringbuffer_append_string(record, "\"}]");
    bool ok = journal_append(record);
    stringbuffer_free(record);
    if (!ok) {
        if (!gameplay_journal_available()) {
            journal_pending_clear();
        }
        transaction_id[0] = '\0';
        return false;
    }
    pending_transaction_t *pending = &journal.pending[journal.pending_count];
    memset(pending, 0, sizeof(*pending));
    snprintf(VS(pending->id), "%s", transaction_id);
    snprintf(VS(pending->reason), "%s", reason);
    snprintf(VS(pending->player_ids[0]), "%s", player_domain);
    pending->player_count = 1;
    journal.pending_count++;
    return true;
}

static bool journal_finish(const char *transaction_id, const char *phase, const char *reason) {
    if (!gameplay_journal_available()) {
        journal_pending_clear();
        return false;
    }
    if (!journal_token_valid(transaction_id, false) || !journal_token_valid(reason, false)) {
        return false;
    }
    ssize_t index = journal_pending_find(transaction_id);
    if (index < 0) {
        return false;
    }
    pending_transaction_t *pending = &journal.pending[(size_t)index];
    if (strcmp(phase, "commit") == 0 && pending->player_count == 0 && pending->map_count == 0) {
        journal_pending_remove((size_t)index);
        return false;
    }
    StringBuffer *record = journal_record(phase, transaction_id, "transaction", reason);
    if (record == NULL) {
        if (gameplay_journal_available()) {
            journal_pending_remove((size_t)index);
        } else {
            journal_pending_clear();
        }
        return false;
    }
    stringbuffer_append_string(record, ",\"domains\":[");
    bool comma = false;
    for (size_t i = 0; i < pending->player_count; i++) {
        if (comma) {
            stringbuffer_append_char(record, ',');
        }
        stringbuffer_append_string(record, "{\"kind\":\"player\",\"id\":\"");
        journal_append_json_string(record, pending->player_ids[i]);
        stringbuffer_append_string(record, "\"}");
        comma = true;
    }
    for (size_t i = 0; i < pending->map_count; i++) {
        if (comma) {
            stringbuffer_append_char(record, ',');
        }
        stringbuffer_append_printf(record,
                                   "{\"kind\":\"%s\",\"id\":\"",
                                   pending->map_unique[i] ? "map-unique" : "map-runtime");
        journal_append_json_string(record, pending->map_ids[i]);
        stringbuffer_append_string(record, "\"}");
        comma = true;
    }
    stringbuffer_append_char(record, ']');
    bool ok = journal_append(record);
    stringbuffer_free(record);
    if (!ok) {
        journal_pending_clear();
        return false;
    }
    if (strcmp(phase, "commit") == 0) {
        for (size_t i = 0; i < pending->player_count; i++) {
            object *player_ob = pending->players[i];
            if (player_ob != NULL && OBJECT_VALID(player_ob, pending->player_counts[i]) &&
                player_ob->type == PLAYER && CONTR(player_ob) != NULL) {
                player *pl = CONTR(player_ob);
                snprintf(VS(pl->journal_run_id), "%s", journal.run_id);
                pl->journal_sequence = journal.sequence;
            }
        }
        for (size_t i = 0; i < pending->map_count; i++) {
            mapstruct *map = pending->maps[i];
            if (map->count != 0 && map->count == pending->map_counts[i]) {
                char *run =
                    pending->map_unique[i] ? map->journal_unique_run_id : map->journal_run_id;
                uint64_t *sequence =
                    pending->map_unique[i] ? &map->journal_unique_sequence : &map->journal_sequence;
                snprintf(run, 33, "%s", journal.run_id);
                *sequence = journal.sequence;
            }
        }
    }
#ifdef ATRINIK_TESTING
    if (strcmp(phase, "commit") == 0) {
        for (size_t i = 0; i < JOURNAL_PENDING_LIMIT; i++) {
            if (journal_test_counts[i].reason[0] == '\0' ||
                strcmp(journal_test_counts[i].reason, pending->reason) == 0) {
                snprintf(VS(journal_test_counts[i].reason), "%s", pending->reason);
                journal_test_counts[i].count++;
                break;
            }
        }
    }
#endif
    journal_pending_remove((size_t)index);
    return true;
}

bool gameplay_journal_commit(const char *transaction_id) {
    return journal_finish(transaction_id, "commit", "transaction.commit");
}

bool gameplay_journal_abort(const char *transaction_id, const char *reason) {
    return journal_finish(transaction_id, "abort", reason);
}

bool gameplay_journal_attempt(const char *transaction_id) {
    if (!gameplay_journal_available()) {
        journal_pending_clear();
        return false;
    }
    if (!journal_token_valid(transaction_id, false)) {
        return false;
    }
    ssize_t index = journal_pending_find(transaction_id);
    if (index < 0) {
        return false;
    }
    journal_pending_remove((size_t)index);
    return true;
}

bool gameplay_journal_profile_boundary(const gameplay_journal_profile_t *profile,
                                       const char *reason) {
    if (!gameplay_journal_available() || !journal_token_valid(reason, false) ||
        journal.pending_count != 0 || !journal_profile_copy(profile)) {
        return false;
    }
    StringBuffer *record = journal_record("boundary", NULL, "profile", reason);
    if (record == NULL) {
        return false;
    }
    bool ok = journal_append(record);
    stringbuffer_free(record);
    return ok;
}

bool gameplay_journal_player_begin(player *pl,
                                   const char *kind,
                                   const char *reason,
                                   const char *subject_id,
                                   const char *lineage_id,
                                   int64_t before,
                                   int64_t delta,
                                   int64_t after,
                                   char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    if (pl == NULL || pl->ob == NULL || pl->cs == NULL || pl->cs->account == NULL || kind == NULL) {
        return false;
    }
    gameplay_journal_kind_t journal_kind;
    if (strcmp(kind, "quest") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_QUEST;
    } else if (strcmp(kind, "progression") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_PROGRESSION;
    } else {
        return false;
    }
    gameplay_journal_change_t change = {
        .subject_id = subject_id,
        .lineage_id = lineage_id,
        .before = before,
        .delta = delta,
        .after = after,
    };
    return gameplay_journal_player_begin_change(pl, journal_kind, reason, &change, transaction_id);
}

bool gameplay_journal_player_begin_change(
    player *pl,
    gameplay_journal_kind_t kind,
    const char *reason,
    const gameplay_journal_change_t *change,
    char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    if (pl == NULL || pl->ob == NULL || pl->cs == NULL || pl->cs->account == NULL) {
        return false;
    }
    gameplay_journal_subject_t subject = {
        .account_id = pl->cs->account,
        .character_id = pl->ob->name,
        .x = pl->ob->x,
        .y = pl->ob->y,
    };
    char map_id[GAMEPLAY_JOURNAL_ID_MAX + 1];
    if (!gameplay_journal_map_identity(pl->ob->map, map_id)) {
        return false;
    }
    subject.map_id = map_id;
    if (!gameplay_journal_begin(&subject, kind, reason, change, transaction_id)) {
        return false;
    }
    if (!gameplay_journal_track_player(transaction_id, pl->ob)) {
        (void)gameplay_journal_abort(transaction_id, "domain-registration-failed");
        transaction_id[0] = '\0';
        return false;
    }
    return true;
}

bool gameplay_journal_currency_begin(object *player_ob,
                                     const char *reason,
                                     const char *subject_id,
                                     int64_t before,
                                     int64_t delta,
                                     int64_t after,
                                     const char *source,
                                     const char *destination,
                                     const char *funding,
                                     char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    return gameplay_journal_currency_begin_economy(player_ob,
                                                   reason,
                                                   subject_id,
                                                   before,
                                                   delta,
                                                   after,
                                                   source,
                                                   destination,
                                                   funding,
                                                   0,
                                                   transaction_id);
}

bool gameplay_journal_milestone_begin(object *player_ob,
                                      gameplay_journal_kind_t kind,
                                      const char *reason,
                                      const char *subject_id,
                                      const char *lineage_id,
                                      int64_t before,
                                      int64_t after,
                                      char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    HARD_ASSERT(transaction_id != NULL);
    transaction_id[0] = '\0';
    if (player_ob == NULL || player_ob->type != PLAYER || CONTR(player_ob) == NULL ||
        (kind != GAMEPLAY_JOURNAL_QUEST && kind != GAMEPLAY_JOURNAL_PROGRESSION) ||
        (before < 0 && after > INT64_MAX + before) || (before > 0 && after < INT64_MIN + before)) {
        return false;
    }
    gameplay_journal_change_t change = {
        .subject_id = subject_id,
        .lineage_id = lineage_id,
        .before = before,
        .delta = after - before,
        .after = after,
    };
    return !gameplay_journal_required() || gameplay_journal_player_begin_change(CONTR(player_ob),
                                                                                kind,
                                                                                reason,
                                                                                &change,
                                                                                transaction_id);
}

bool gameplay_journal_currency_begin_economy(
    object *player_ob,
    const char *reason,
    const char *subject_id,
    int64_t before,
    int64_t delta,
    int64_t after,
    const char *source,
    const char *destination,
    const char *funding,
    int64_t price,
    char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    HARD_ASSERT(transaction_id != NULL);
    transaction_id[0] = '\0';
    if (player_ob == NULL || player_ob->type != PLAYER || CONTR(player_ob) == NULL) {
        return false;
    }
    const char *actor = object_custody_actor_id(player_ob);
    if (actor == NULL) {
        return false;
    }
    gameplay_journal_change_t change = {
        .subject_id = subject_id,
        .lineage_id = "",
        .before = before,
        .delta = delta,
        .after = after,
        .archetype = "",
        .object_type = 0,
        .snapshot = "",
        .quantity = 0,
        .source = source,
        .destination = destination,
        .actor = actor,
        .counterparty = "",
        .provenance_before = "",
        .provenance_after = "",
        .price = price,
        .currency = "copper-equivalent",
        .funding = funding,
    };
    return !gameplay_journal_required() ||
           gameplay_journal_player_begin_change(CONTR(player_ob),
                                                GAMEPLAY_JOURNAL_CURRENCY,
                                                reason,
                                                &change,
                                                transaction_id);
}

bool gameplay_journal_semantic_commit(const char *transaction_id) {
    return transaction_id != NULL &&
           (transaction_id[0] == '\0' || gameplay_journal_commit(transaction_id));
}

bool gameplay_journal_semantic_abort(const char *transaction_id, const char *reason) {
    return transaction_id != NULL &&
           (transaction_id[0] == '\0' || gameplay_journal_abort(transaction_id, reason));
}
