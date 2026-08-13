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
} journal_state_t;

static journal_state_t journal = {.lock_fd = -1};

#ifdef ATRINIK_TESTING
static bool journal_test_fail_writes;
static size_t journal_test_file_limit = JOURNAL_FILE_LIMIT;
static size_t journal_test_hard_limit = JOURNAL_FILE_HARD_LIMIT;

void gameplay_journal_fail_writes_for_test(bool fail) {
    journal_test_fail_writes = fail;
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

static void journal_append_json_string(StringBuffer *record, const char *value) {
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if (*cp == '"' || *cp == '\\') {
            stringbuffer_append_char(record, '\\');
        }
        stringbuffer_append_char(record, (char)*cp);
    }
}

static bool journal_change_valid(const gameplay_journal_change_t *change) {
    if (change->delta > 0 && change->before > INT64_MAX - change->delta) {
        return false;
    }
    if (change->delta < 0 && change->before < INT64_MIN - change->delta) {
        return false;
    }
    return change->before + change->delta == change->after;
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
    int flags = O_RDWR | O_CREAT;
#ifndef WIN32
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, SAVE_MODE);
    if (fd < 0) {
        return false;
    }
#ifndef WIN32
    struct stat metadata;
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        (metadata.st_mode & 0777) != SAVE_MODE) {
        close(fd);
        return false;
    }
#endif
#ifdef WIN32
    OVERLAPPED overlapped = {0};
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    bool ok = handle != INVALID_HANDLE_VALUE &&
              LockFileEx(handle,
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
    forced_failure = journal_test_fail_writes;
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
    if (journal.sequence == UINT64_MAX) {
        journal.failed = true;
        LOG(ERROR, "Gameplay journal exhausted its per-run sequence.");
        return NULL;
    }
    journal.sequence++;
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
    if (!journal_lock() || !journal_random_id(journal.run_id)) {
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

static ssize_t journal_pending_find(const char *transaction_id) {
    for (size_t i = 0; i < journal.pending_count; i++) {
        if (strcmp(journal.pending[i].id, transaction_id) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

bool gameplay_journal_begin(const gameplay_journal_subject_t *subject,
                            gameplay_journal_kind_t kind,
                            const char *reason,
                            const gameplay_journal_change_t *change,
                            char transaction_id[GAMEPLAY_JOURNAL_TRANSACTION_ID_SIZE]) {
    const char *kind_name = journal_kind_name(kind);
    if (!gameplay_journal_available() || subject == NULL || change == NULL ||
        transaction_id == NULL || kind_name == NULL || !journal_token_valid(reason, false) ||
        !journal_identity_valid(subject->account_id) ||
        !journal_identity_valid(subject->character_id) ||
        !journal_token_valid(subject->map_id, true) ||
        !journal_token_valid(change->subject_id, false) ||
        !journal_token_valid(change->lineage_id, true) || !journal_change_valid(change) ||
        journal.pending_count == JOURNAL_PENDING_LIMIT || !journal_random_id(transaction_id)) {
        return false;
    }
    StringBuffer *record = journal_record("intent", transaction_id, kind_name, reason);
    if (record == NULL) {
        return false;
    }
    stringbuffer_append_string(record, ",\"account_id\":\"");
    journal_append_json_string(record, subject->account_id);
    stringbuffer_append_string(record, "\",\"character_id\":\"");
    journal_append_json_string(record, subject->character_id);
    stringbuffer_append_printf(
        record,
        "\",\"context\":{\"map_id\":\"%s\",\"x\":%" PRId32 ",\"y\":%" PRId32 "}"
        ",\"change\":{\"subject_id\":\"%s\",\"lineage_id\":\"%s\""
        ",\"before\":%" PRId64 ",\"delta\":%" PRId64 ",\"after\":%" PRId64 "}",
        subject->map_id != NULL ? subject->map_id : "",
        subject->x,
        subject->y,
        change->subject_id,
        change->lineage_id != NULL ? change->lineage_id : "",
        change->before,
        change->delta,
        change->after);
    bool ok = journal_append(record);
    stringbuffer_free(record);
    if (!ok) {
        transaction_id[0] = '\0';
        return false;
    }
    snprintf(VS(journal.pending[journal.pending_count].id), "%s", transaction_id);
    journal.pending_count++;
    return true;
}

static bool journal_finish(const char *transaction_id, const char *phase, const char *reason) {
    if (!gameplay_journal_available() || !journal_token_valid(transaction_id, false) ||
        !journal_token_valid(reason, false)) {
        return false;
    }
    ssize_t index = journal_pending_find(transaction_id);
    if (index < 0) {
        return false;
    }
    StringBuffer *record = journal_record(phase, transaction_id, "transaction", reason);
    if (record == NULL) {
        return false;
    }
    bool ok = journal_append(record);
    stringbuffer_free(record);
    if (!ok) {
        return false;
    }
    journal.pending[(size_t)index] = journal.pending[journal.pending_count - 1];
    journal.pending_count--;
    return true;
}

bool gameplay_journal_commit(const char *transaction_id) {
    return journal_finish(transaction_id, "commit", "transaction.commit");
}

bool gameplay_journal_abort(const char *transaction_id, const char *reason) {
    return journal_finish(transaction_id, "abort", reason);
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
    if (strcmp(kind, "item") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_ITEM;
    } else if (strcmp(kind, "currency") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_CURRENCY;
    } else if (strcmp(kind, "quest") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_QUEST;
    } else if (strcmp(kind, "progression") == 0) {
        journal_kind = GAMEPLAY_JOURNAL_PROGRESSION;
    } else {
        return false;
    }
    gameplay_journal_subject_t subject = {
        .account_id = pl->cs->account,
        .character_id = pl->ob->name,
        .map_id = pl->ob->map != NULL ? pl->ob->map->path : "",
        .x = pl->ob->x,
        .y = pl->ob->y,
    };
    gameplay_journal_change_t change = {
        .subject_id = subject_id,
        .lineage_id = lineage_id,
        .before = before,
        .delta = delta,
        .after = after,
    };
    return gameplay_journal_begin(&subject, journal_kind, reason, &change, transaction_id);
}
