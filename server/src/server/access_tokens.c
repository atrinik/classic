/* Copyright (c) 2026 The Atrinik Project. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "access_tokens.h"
#include <toolkit/access_code.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT "access-tokens.snapshot"
#define SNAPSHOT_MAX (8U * 1024U * 1024U)
#define RECEIPT_DAYS (7 * 86400)
#define TOMBSTONE_DAYS (90 * 86400)

typedef struct {
    access_token_info_t info;
    uint8_t verifier[32], index[32];
    char route_request[33];
} token_t;
typedef struct {
    char request[33], token[33];
    uint8_t fingerprint[32];
    uint64_t revision, token_revision;
    int64_t created;
    access_operation_t operation;
    access_outcome_t outcome;
} receipt_t;
typedef struct { char id[33]; int64_t removed; uint64_t revision; } tombstone_t;
typedef struct {
    uint8_t identity[32];
    uint64_t revision, commit_sequence;
    int64_t clock;
    size_t token_count, receipt_count, tombstone_count;
    token_t tokens[ACCESS_TOKEN_LIMIT];
    receipt_t receipts[ACCESS_RECEIPT_LIMIT];
    tombstone_t tombstones[ACCESS_TOMBSTONE_LIMIT];
} snapshot_t;
struct access_store {
    pthread_mutex_t mutex;
    int directory;
    bool protected_policy, poisoned, fenced;
    int64_t observed_clock;
    snapshot_t *state;
};
typedef struct { uint8_t *data; size_t pos, size; bool ok; } codec_t;

static bool digest(const void *data, size_t size, uint8_t out[32])
{
    unsigned length = 0;
    return EVP_Digest(data, size, out, &length, EVP_sha256(), NULL) == 1 && length == 32;
}
static bool hex_id(const char *s)
{
    if (s == NULL || strnlen(s, 33) != 32) return false;
    for (size_t i = 0; i < 32; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool random_id(char out[33])
{
    uint8_t bytes[16];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return false;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[2 * i] = hex[bytes[i] >> 4]; out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[32] = '\0'; OPENSSL_cleanse(bytes, sizeof(bytes)); return true;
}
/* Strict scalar UTF-8, excluding ASCII/C1 controls and DEL. */
static bool label_valid(const char *s)
{
    if (s == NULL) return false;
    size_t n = strnlen(s, ACCESS_LABEL_BYTES + 1);
    if (n == 0 || n > ACCESS_LABEL_BYTES) return false;
    for (size_t i = 0; i < n;) {
        uint32_t c = (uint8_t) s[i++], minimum = 0;
        unsigned remaining = 0;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; minimum = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; minimum = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; minimum = 0x10000; c &= 7; }
        else if (c >= 0x80) return false;
        while (remaining-- > 0) {
            if (i == n || ((uint8_t) s[i] & 0xc0) != 0x80) return false;
            c = (c << 6) | ((uint8_t) s[i++] & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
                c < 0x20 || (c >= 0x7f && c <= 0x9f)) return false;
    }
    return true;
}
static void put(codec_t *c, const void *p, size_t n)
{
    if (!c->ok || n > c->size - c->pos) { c->ok = false; return; }
    memcpy(c->data + c->pos, p, n); c->pos += n;
}
static void get(codec_t *c, void *p, size_t n)
{
    if (!c->ok || n > c->size - c->pos) { c->ok = false; return; }
    memcpy(p, c->data + c->pos, n); c->pos += n;
}
static void put64(codec_t *c, uint64_t v)
{
    uint8_t b[8]; for (unsigned i = 0; i < 8; i++) b[7 - i] = (uint8_t) (v >> (i * 8));
    put(c, b, sizeof(b));
}
static uint64_t get64(codec_t *c)
{
    uint8_t b[8] = {0}; uint64_t v = 0; get(c, b, sizeof(b));
    for (unsigned i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}
static void string_put(codec_t *c, const char *s, size_t max)
{
    size_t n = strnlen(s, max + 1); put64(c, n);
    if (n > max) { c->ok = false; return; } put(c, s, n);
}
static void string_get(codec_t *c, char *s, size_t max)
{
    uint64_t n = get64(c);
    if (n > max) { c->ok = false; return; }
    get(c, s, (size_t) n); s[n] = '\0';
    if (memchr(s, '\0', (size_t) n) != NULL) c->ok = false;
}
static uint8_t *encode(const snapshot_t *s, size_t *length)
{
    uint8_t *data = malloc(SNAPSHOT_MAX);
    if (data == NULL) return NULL;
    codec_t c = {data, 0, SNAPSHOT_MAX - 32, true};
    put(&c, "ATACCESS", 8); put64(&c, ACCESS_STORE_SCHEMA); put(&c, s->identity, 32);
    put64(&c, s->revision); put64(&c, s->commit_sequence); put64(&c, (uint64_t) s->clock);
    put64(&c, s->token_count); put64(&c, s->receipt_count); put64(&c, s->tombstone_count);
    for (size_t i = 0; i < s->token_count; i++) {
        const token_t *t = &s->tokens[i]; const access_token_info_t *a = &t->info;
        string_put(&c, a->ref.token_id, 32); put64(&c, a->ref.revision);
        string_put(&c, a->label, ACCESS_LABEL_BYTES); put64(&c, (uint64_t) a->created_at);
        put64(&c, a->has_expiry); if (a->has_expiry) put64(&c, (uint64_t) a->expires_at);
        put64(&c, a->state); put64(&c, a->route_pending); put64(&c, (uint64_t) a->last_admitted_at);
        put64(&c, a->history_count);
        for (size_t j = 0; j < a->history_count; j++) put64(&c, (uint64_t) a->history[j]);
        put(&c, t->verifier, 32); put(&c, t->index, 32); string_put(&c, t->route_request, 32);
    }
    for (size_t i = 0; i < s->receipt_count; i++) {
        const receipt_t *r = &s->receipts[i];
        string_put(&c, r->request, 32); string_put(&c, r->token, 32); put(&c, r->fingerprint, 32);
        put64(&c, r->revision); put64(&c, r->token_revision); put64(&c, (uint64_t) r->created);
        put64(&c, r->operation); put64(&c, r->outcome);
    }
    for (size_t i = 0; i < s->tombstone_count; i++) {
        const tombstone_t *t = &s->tombstones[i]; string_put(&c, t->id, 32);
        put64(&c, (uint64_t) t->removed); put64(&c, t->revision);
    }
    if (!c.ok || !digest(data, c.pos, data + c.pos)) { free(data); return NULL; }
    *length = c.pos + 32; return data;
}
static size_t pending_count(const snapshot_t *s)
{
    size_t n = 0; for (size_t i = 0; i < s->token_count; i++) n += s->tokens[i].info.route_pending;
    return n;
}
static bool validate(const snapshot_t *s, bool uniqueness)
{
    if (s->revision == 0 || s->commit_sequence < s->revision || s->clock < 0 || s->token_count > ACCESS_TOKEN_LIMIT ||
            s->receipt_count > ACCESS_RECEIPT_LIMIT || s->tombstone_count > ACCESS_TOMBSTONE_LIMIT) return false;
    for (size_t i = 0; i < s->token_count; i++) {
        const token_t *t = &s->tokens[i]; const access_token_info_t *a = &t->info;
        if (!hex_id(a->ref.token_id) || !hex_id(t->route_request) || !label_valid(a->label) ||
                a->ref.revision == 0 || a->ref.revision > s->revision || a->created_at <= 0 ||
                a->created_at > s->clock || (a->has_expiry && (a->expires_at <= a->created_at || a->expires_at > INT64_C(253402300799))) ||
                a->state < ACCESS_TOKEN_PENDING || a->state > ACCESS_TOKEN_EXPIRED ||
                (a->state == ACCESS_TOKEN_PENDING && !a->route_pending) ||
                a->history_count > ACCESS_HISTORY_LIMIT ||
                (a->history_count == 0 && a->last_admitted_at != 0)) return false;
        int64_t last = a->created_at;
        for (size_t j = 0; j < a->history_count; j++) {
            if (a->history[j] < last || a->history[j] > s->clock) return false;
            last = a->history[j];
        }
        if (a->history_count && last != a->last_admitted_at) return false;
        if (uniqueness) {
            for (size_t j = 0; j < i; j++)
                if (strcmp(a->ref.token_id, s->tokens[j].info.ref.token_id) == 0 ||
                        CRYPTO_memcmp(t->index, s->tokens[j].index, 32) == 0 ||
                        CRYPTO_memcmp(t->verifier, s->tokens[j].verifier, 32) == 0) return false;
            for (size_t j = 0; j < s->tombstone_count; j++)
                if (strcmp(a->ref.token_id, s->tombstones[j].id) == 0) return false;
        }
    }
    for (size_t i = 0; i < s->receipt_count; i++) {
        const receipt_t *r = &s->receipts[i];
        if (!hex_id(r->request) || !hex_id(r->token) || r->revision == 0 || r->revision > s->revision ||
                r->token_revision == 0 || r->token_revision > r->revision || r->created <= 0 ||
                r->created > s->clock || r->operation < ACCESS_OP_ISSUE || r->operation > ACCESS_OP_REMOVE ||
                (r->outcome != ACCESS_PENDING && r->outcome != ACCESS_COMMITTED &&
                 r->outcome != ACCESS_LOCALLY_REVOKED)) return false;
        if (uniqueness) for (size_t j = 0; j < i; j++)
            if (strcmp(r->request, s->receipts[j].request) == 0) return false;
    }
    for (size_t i = 0; i < s->tombstone_count; i++) {
        const tombstone_t *t = &s->tombstones[i];
        if (!hex_id(t->id) || t->removed <= 0 || t->removed > s->clock ||
                t->revision == 0 || t->revision > s->revision) return false;
        if (uniqueness) for (size_t j = 0; j < i; j++)
            if (strcmp(t->id, s->tombstones[j].id) == 0) return false;
    }
    return true;
}
static snapshot_t *decode(uint8_t *data, size_t n, const uint8_t identity[32])
{
    uint8_t hash[32], magic[8];
    if (n < 120 || !digest(data, n - 32, hash) || CRYPTO_memcmp(hash, data + n - 32, 32)) return NULL;
    snapshot_t *s = calloc(1, sizeof(*s)); if (s == NULL) return NULL;
    codec_t c = {data, 0, n - 32, true}; get(&c, magic, 8);
    if (memcmp(magic, "ATACCESS", 8) || get64(&c) != ACCESS_STORE_SCHEMA) goto fail;
    get(&c, s->identity, 32); if (CRYPTO_memcmp(identity, s->identity, 32)) goto fail;
    s->revision = get64(&c); s->commit_sequence = get64(&c); s->clock = (int64_t) get64(&c);
    s->token_count = get64(&c); s->receipt_count = get64(&c); s->tombstone_count = get64(&c);
    if (s->token_count > ACCESS_TOKEN_LIMIT || s->receipt_count > ACCESS_RECEIPT_LIMIT ||
            s->tombstone_count > ACCESS_TOMBSTONE_LIMIT) goto fail;
    for (size_t i = 0; i < s->token_count && c.ok; i++) {
        token_t *t = &s->tokens[i]; access_token_info_t *a = &t->info;
        string_get(&c, a->ref.token_id, 32); a->ref.revision = get64(&c);
        string_get(&c, a->label, ACCESS_LABEL_BYTES); a->created_at = (int64_t) get64(&c);
        uint64_t flag = get64(&c); if (flag > 1) goto fail; a->has_expiry = flag != 0;
        if (a->has_expiry) a->expires_at = (int64_t) get64(&c);
        uint64_t state = get64(&c); if (state > ACCESS_TOKEN_EXPIRED) goto fail;
        a->state = (access_token_state_t) state;
        flag = get64(&c); if (flag > 1) goto fail; a->route_pending = flag != 0;
        a->last_admitted_at = (int64_t) get64(&c); a->history_count = get64(&c);
        if (a->history_count > ACCESS_HISTORY_LIMIT) goto fail;
        for (size_t j = 0; j < a->history_count; j++) a->history[j] = (int64_t) get64(&c);
        get(&c, t->verifier, 32); get(&c, t->index, 32); string_get(&c, t->route_request, 32);
    }
    for (size_t i = 0; i < s->receipt_count && c.ok; i++) {
        receipt_t *r = &s->receipts[i]; string_get(&c, r->request, 32); string_get(&c, r->token, 32);
        get(&c, r->fingerprint, 32); r->revision = get64(&c); r->token_revision = get64(&c);
        r->created = (int64_t) get64(&c);
        uint64_t op = get64(&c), outcome = get64(&c);
        if (op > ACCESS_OP_REMOVE || outcome > ACCESS_NOT_FOUND) goto fail;
        r->operation = (access_operation_t) op; r->outcome = (access_outcome_t) outcome;
    }
    for (size_t i = 0; i < s->tombstone_count && c.ok; i++) {
        tombstone_t *t = &s->tombstones[i]; string_get(&c, t->id, 32);
        t->removed = (int64_t) get64(&c); t->revision = get64(&c);
    }
    if (!c.ok || c.pos != c.size || !validate(s, true)) goto fail;
    return s;
fail:
    OPENSSL_cleanse(s, sizeof(*s)); free(s); return NULL;
}
/* Open each path component without following symlinks, including ancestors. */
static bool trusted_directory(int fd, bool root_only)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != 0 && (root_only || st.st_uid != geteuid())) return false;
    /* A root-owned sticky public ancestor (e.g. /tmp) cannot rename an owned
     * child. All task/store directories beneath it still require ownership. */
    return !(st.st_mode & 0022) || (!root_only && st.st_uid == 0 && (st.st_mode & S_ISVTX));
}
static int open_directory(const char *path, bool private_leaf)
{
    if (path == NULL || path[0] == '\0' || strlen(path) >= PATH_MAX) return -1;
    char copy[PATH_MAX];
    if (path[0] == '/') memcpy(copy, path, strlen(path) + 1);
    else {
        const char *relative = path;
        while (relative[0] == '.' && relative[1] == '/') relative += 2;
        if (*relative == '\0' || getcwd(copy, sizeof(copy)) == NULL) return -1;
        size_t prefix = strlen(copy), suffix = strlen(relative);
        if (prefix + suffix + 2 > sizeof(copy)) return -1;
        copy[prefix] = '/'; memcpy(copy + prefix + 1, relative, suffix + 1);
    }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!trusted_directory(fd, false)) { close(fd); return -1; }
    char *save = NULL, *part = strtok_r(copy, "/", &save);
    while (part != NULL) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); if (next < 0) return -1; fd = next;
        if (!trusted_directory(fd, false)) { close(fd); return -1; }
        part = strtok_r(NULL, "/", &save);
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (private_leaf &&
            (st.st_uid != geteuid() || (st.st_mode & 07777) != 0700))) { close(fd); return -1; }
    return fd;
}

static bool private_file(int fd, uid_t owner)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == owner &&
        st.st_nlink == 1 && (st.st_mode & 07777) == 0600;
}
static snapshot_t *read_snapshot(int directory, const uint8_t identity[32])
{
    int fd = openat(directory, SNAPSHOT, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (!private_file(fd, geteuid()) || fstat(fd, &st) != 0 || st.st_size < 120 || st.st_size > SNAPSHOT_MAX) {
        close(fd); return NULL;
    }
    size_t n = (size_t) st.st_size; uint8_t *data = malloc(n); size_t pos = 0;
    if (data == NULL) { close(fd); return NULL; }
    while (pos < n) {
        ssize_t got = read(fd, data + pos, n - pos);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        pos += (size_t) got;
    }
    uint8_t extra; ssize_t tail = read(fd, &extra, 1);
    bool ok = close(fd) == 0;
    snapshot_t *s = pos == n && tail == 0 && ok ? decode(data, n, identity) : NULL;
    OPENSSL_cleanse(data, n); free(data); return s;
}
/* Only installed, fsynced, read-back state becomes authoritative RAM. A failure
 * after replacement poisons this handle: reopening is the recovery boundary. */
static access_outcome_t commit(access_store_t *store, snapshot_t *next, bool initial)
{
    if (!validate(next, false)) return ACCESS_INVALID;
    struct stat directory_stat;
    if (fstat(store->directory, &directory_stat) != 0 || directory_stat.st_uid != geteuid() ||
            !S_ISDIR(directory_stat.st_mode) || (directory_stat.st_mode & 07777) != 0700) return ACCESS_SAVE_FAILED;
    size_t n = 0; uint8_t *data = encode(next, &n);
    if (data == NULL) return ACCESS_SAVE_FAILED;
    char id[33], temporary[64];
    if (!random_id(id)) { free(data); return ACCESS_UNAVAILABLE; }
    snprintf(temporary, sizeof(temporary), ".access-%s.tmp", id);
    int fd = openat(store->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && private_file(fd, geteuid());
    size_t pos = 0;
    while (ok && pos < n) {
        ssize_t written = write(fd, data + pos, n - pos);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        pos += (size_t) written;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (fd >= 0 && close(fd) != 0) ok = false;
    OPENSSL_cleanse(data, n); free(data);
    /* Recheck target privacy immediately before replacement. */
    struct stat st;
    if (ok) {
        int found = fstatat(store->directory, SNAPSHOT, &st, AT_SYMLINK_NOFOLLOW);
        if (initial) ok = found != 0 && errno == ENOENT;
        else ok = found == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
            st.st_nlink == 1 && (st.st_mode & 07777) == 0600;
    }
    if (!ok || renameat(store->directory, temporary, store->directory, SNAPSHOT) != 0) {
        unlinkat(store->directory, temporary, 0); return ACCESS_SAVE_FAILED;
    }
    if (fsync(store->directory) != 0) { store->poisoned = true; return ACCESS_INDETERMINATE; }
    snapshot_t *check = read_snapshot(store->directory, next->identity);
    if (check == NULL) { store->poisoned = true; return ACCESS_INDETERMINATE; }
    size_t check_n = 0, next_n = 0;
    uint8_t *check_bytes = encode(check, &check_n), *next_bytes = encode(next, &next_n);
    ok = check_bytes && next_bytes && check_n == next_n && CRYPTO_memcmp(check_bytes, next_bytes, next_n) == 0;
    if (check_bytes) { OPENSSL_cleanse(check_bytes, check_n); free(check_bytes); }
    if (next_bytes) { OPENSSL_cleanse(next_bytes, next_n); free(next_bytes); }
    if (!ok) { OPENSSL_cleanse(check, sizeof(*check)); free(check); store->poisoned = true; return ACCESS_INDETERMINATE; }
    OPENSSL_cleanse(store->state, sizeof(*store->state)); free(store->state); store->state = check;
    if (check->clock > store->observed_clock) store->observed_clock = check->clock;
    return ACCESS_COMMITTED;
}
static token_t *find_token(snapshot_t *s, const char *id)
{
    for (size_t i = 0; i < s->token_count; i++) if (!strcmp(s->tokens[i].info.ref.token_id, id)) return &s->tokens[i];
    return NULL;
}
static receipt_t *find_receipt(snapshot_t *s, const char *id)
{
    for (size_t i = 0; i < s->receipt_count; i++) if (!strcmp(s->receipts[i].request, id)) return &s->receipts[i];
    return NULL;
}
static snapshot_t *candidate(access_store_t *s, int64_t now)
{
    if (s->poisoned || s->fenced || now <= 0 || s->state->revision == UINT64_MAX || s->state->commit_sequence == UINT64_MAX) return NULL;
    snapshot_t *n = malloc(sizeof(*n)); if (n == NULL) return NULL;
    memcpy(n, s->state, sizeof(*n)); n->revision++; n->commit_sequence++;
    if (now > n->clock) n->clock = now;
    if (s->observed_clock > n->clock) n->clock = s->observed_clock;
    return n;
}
static void discard(snapshot_t *s) { if (s) { OPENSSL_cleanse(s, sizeof(*s)); free(s); } }
static access_result_t result(access_store_t *s, access_outcome_t o)
{
    access_result_t r = {0}; r.outcome = o; r.revision = s->state->revision; return r;
}
static access_result_t receipt_result(access_store_t *s, const receipt_t *r)
{
    access_result_t out = result(s, r->outcome);
    out.revision = r->revision; memcpy(out.token.token_id, r->token, 33); out.token.revision = r->token_revision;
    token_t *t = find_token(s->state, r->token); out.route_pending = t && t->info.route_pending;
    if (r->operation == ACCESS_OP_ISSUE && out.outcome == ACCESS_COMMITTED) out.outcome = ACCESS_SECRET_UNAVAILABLE;
    return out;
}
static bool fingerprint(access_operation_t op, uint64_t revision, const char *arg,
    bool has_expiry, int64_t expires, uint8_t out[32])
{
    uint8_t data[256]; codec_t c = {data, 0, sizeof(data), true};
    put64(&c, op); put64(&c, revision); string_put(&c, arg, ACCESS_LABEL_BYTES);
    put64(&c, has_expiry); if (has_expiry) put64(&c, (uint64_t) expires);
    return c.ok && digest(data, c.pos, out);
}
static void prune(snapshot_t *s, int64_t now)
{
    /* Never evict unexpired evidence, including pending issue receipts. */
    for (size_t i = 0; i < s->receipt_count;) {
        receipt_t *r = &s->receipts[i];
        if (r->outcome != ACCESS_PENDING && now >= r->created && now - r->created > RECEIPT_DAYS) {
            *r = s->receipts[--s->receipt_count]; memset(&s->receipts[s->receipt_count], 0, sizeof(*r));
        } else i++;
    }
    for (size_t i = 0; i < s->tombstone_count;) {
        tombstone_t *t = &s->tombstones[i];
        if (now >= t->removed && now - t->removed > TOMBSTONE_DAYS) {
            *t = s->tombstones[--s->tombstone_count]; memset(&s->tombstones[s->tombstone_count], 0, sizeof(*t));
        } else i++;
    }
}
static void new_receipt(snapshot_t *s, const char *id, const uint8_t hash[32],
    const token_t *t, access_operation_t op, access_outcome_t outcome, int64_t now)
{
    receipt_t *r = &s->receipts[s->receipt_count++]; memset(r, 0, sizeof(*r));
    memcpy(r->request, id, 33); memcpy(r->token, t->info.ref.token_id, 33); memcpy(r->fingerprint, hash, 32);
    r->revision = s->revision; r->token_revision = t->info.ref.revision; r->created = now;
    r->operation = op; r->outcome = outcome;
}
static access_route_t route_for(const token_t *t)
{
    access_route_t r = {0}; memcpy(r.request_id, t->route_request, 33); r.token = t->info.ref;
    memcpy(r.index, t->index, 32); r.has_expiry = t->info.has_expiry; r.expires_at = t->info.expires_at;
    r.revoke = t->info.state != ACCESS_TOKEN_PENDING; return r;
}

access_outcome_t access_store_open(access_store_t **out, const char *directory,
    const uint8_t identity[32], bool protected_policy, bool initialize)
{
    if (out == NULL || identity == NULL) return ACCESS_INVALID;
    *out = NULL; int fd = open_directory(directory, true);
    if (fd < 0) return ACCESS_UNAVAILABLE;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return ACCESS_UNAVAILABLE; }
    access_store_t *s = calloc(1, sizeof(*s));
    if (s == NULL) { close(fd); return ACCESS_UNAVAILABLE; }
    s->directory = fd; s->protected_policy = protected_policy;
    if (pthread_mutex_init(&s->mutex, NULL) != 0) { close(fd); free(s); return ACCESS_UNAVAILABLE; }
    access_outcome_t outcome = ACCESS_COMMITTED;
    if (initialize) {
        s->state = calloc(1, sizeof(*s->state));
        if (s->state == NULL) { outcome = ACCESS_UNAVAILABLE; goto fail; }
        memcpy(s->state->identity, identity, 32); s->state->revision = 1; s->state->commit_sequence = 1;
        snapshot_t *n = malloc(sizeof(*n));
        if (n == NULL) { outcome = ACCESS_UNAVAILABLE; goto fail; }
        memcpy(n, s->state, sizeof(*n)); outcome = commit(s, n, true); discard(n);
        if (outcome != ACCESS_COMMITTED) goto fail;
    } else {
        s->state = read_snapshot(fd, identity);
        if (s->state == NULL) { outcome = ACCESS_UNAVAILABLE; goto fail; }
    }
    snapshot_t *n = NULL;
    for (size_t i = 0; i < s->state->token_count; i++) {
        if (s->state->tokens[i].info.state != ACCESS_TOKEN_PENDING) continue;
        if (n == NULL) n = candidate(s, s->state->clock);
        if (n == NULL) { outcome = ACCESS_UNAVAILABLE; goto fail; }
        token_t *t = &n->tokens[i]; t->info.state = ACCESS_TOKEN_REVOKED;
        t->info.ref.revision++;
        for (size_t j = 0; j < n->receipt_count; j++) {
            receipt_t *r = &n->receipts[j];
            if (!strcmp(r->token, t->info.ref.token_id) && r->operation == ACCESS_OP_ISSUE && r->outcome == ACCESS_PENDING) {
                r->outcome = ACCESS_LOCALLY_REVOKED; r->revision = n->revision; r->token_revision = t->info.ref.revision;
            }
        }
    }
    if (n) { outcome = commit(s, n, false); discard(n); if (outcome != ACCESS_COMMITTED) goto fail; }
    s->observed_clock = s->state->clock;
    *out = s; return ACCESS_COMMITTED;
fail:
    access_store_close(s); return outcome;
}
void access_store_close(access_store_t *s)
{
    if (s == NULL) return;
    discard(s->state); close(s->directory); pthread_mutex_destroy(&s->mutex); free(s);
}
access_status_t access_store_status(access_store_t *s)
{
    access_status_t a = {0}; if (s == NULL) return a;
    pthread_mutex_lock(&s->mutex); a.schema_version = ACCESS_STORE_SCHEMA;
    memcpy(a.server_identity, s->state->identity, 32); a.protected_policy = s->protected_policy;
    a.integrity_ok = !s->poisoned; a.durability_ok = !s->poisoned; a.fenced = s->fenced;
    a.revision = s->state->revision; a.pending_route_sync = pending_count(s->state);
    pthread_mutex_unlock(&s->mutex); return a;
}
access_outcome_t access_store_list(access_store_t *s, uint64_t revision,
    size_t offset, size_t limit, access_token_info_t *rows, size_t *count, size_t *next_offset)
{
    if (!s || !rows || !count || !next_offset || limit == 0 || limit > ACCESS_PAGE_LIMIT) return ACCESS_INVALID;
    *count = 0; *next_offset = 0; pthread_mutex_lock(&s->mutex);
    access_outcome_t o = ACCESS_COMMITTED;
    if (s->poisoned) o = ACCESS_INDETERMINATE;
    else if (revision != s->state->revision) o = ACCESS_CONFLICT;
    else if (offset > s->state->token_count) o = ACCESS_INVALID;
    else {
        size_t n = s->state->token_count - offset; if (n > limit) n = limit;
        for (size_t i = 0; i < n; i++) rows[i] = s->state->tokens[offset + i].info;
        *count = n; *next_offset = offset + n < s->state->token_count ? offset + n : 0;
    }
    pthread_mutex_unlock(&s->mutex); return o;
}
access_outcome_t access_store_history(access_store_t *s, const char *id, uint64_t revision, access_token_info_t *row)
{
    if (!s || !hex_id(id) || !row) return ACCESS_INVALID;
    pthread_mutex_lock(&s->mutex); token_t *t = find_token(s->state, id);
    access_outcome_t o = s->poisoned ? ACCESS_INDETERMINATE : revision != s->state->revision ? ACCESS_CONFLICT :
        t == NULL ? ACCESS_NOT_FOUND : ACCESS_COMMITTED;
    if (o == ACCESS_COMMITTED) *row = t->info;
    pthread_mutex_unlock(&s->mutex); return o;
}
access_result_t access_store_result(access_store_t *s, const char *id)
{
    access_result_t out = { .outcome = ACCESS_INVALID };
    if (!s || !hex_id(id)) return out;
    pthread_mutex_lock(&s->mutex); receipt_t *r = find_receipt(s->state, id);
    out = s->poisoned ? result(s, ACCESS_INDETERMINATE) : r ? receipt_result(s, r) : result(s, ACCESS_NOT_FOUND);
    pthread_mutex_unlock(&s->mutex); return out;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec > (INT64_MAX - 30000) / 1000) return 0;
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static bool issue_material(snapshot_t *n, token_t *t, char code[17], unsigned *generated)
{
    uint8_t raw_route[32]; bool unique = false;
    while (*generated < 3 && !unique) {
        (*generated)++; OPENSSL_cleanse(code, 17);
        if (!random_id(t->info.ref.token_id) || !access_code_generate(code) ||
                !access_code_derive(code, n->identity, raw_route, t->index, t->verifier)) break;
        OPENSSL_cleanse(raw_route, sizeof(raw_route)); unique = true;
        for (size_t i = 0; i < n->token_count; i++) {
            if (&n->tokens[i] == t) continue;
            if (!strcmp(t->info.ref.token_id, n->tokens[i].info.ref.token_id) ||
                    !CRYPTO_memcmp(t->index, n->tokens[i].index, 32)) unique = false;
        }
        for (size_t i = 0; i < n->tombstone_count; i++)
            if (!strcmp(t->info.ref.token_id, n->tombstones[i].id)) unique = false;
    }
    OPENSSL_cleanse(raw_route, sizeof(raw_route)); return unique;
}

access_result_t access_store_issue(access_store_t *s, const char *id, uint64_t expected,
    const char *label, bool has_expiry, int64_t expires, int64_t now,
    access_route_callback_t route, void *context)
{
    access_result_t out = { .outcome = ACCESS_INVALID }; uint8_t hash[32];
    if (!s || !hex_id(id) || !label_valid(label) || now <= 0 ||
            (has_expiry && (expires <= 0 || expires > INT64_C(253402300799))) || !fingerprint(ACCESS_OP_ISSUE, expected, label, has_expiry, expires, hash)) return out;
    int64_t started = monotonic_ms();
    if (started <= 0) return out;
    int64_t deadline = started + 30000;
    pthread_mutex_lock(&s->mutex);
    receipt_t *old = find_receipt(s->state, id);
    if (s->poisoned || s->fenced) { out = result(s, ACCESS_UNAVAILABLE); goto done; }
    if (old) { out = CRYPTO_memcmp(hash, old->fingerprint, 32) ? result(s, ACCESS_CONFLICT) : receipt_result(s, old); goto done; }
    if (has_expiry && expires <= now) { out = result(s, ACCESS_INVALID); goto done; }
    if (expected != s->state->revision || now < s->state->clock) { out = result(s, ACCESS_CONFLICT); goto done; }
    snapshot_t *n = candidate(s, now);
    if (!n) { out = result(s, ACCESS_UNAVAILABLE); goto done; }
    prune(n, now);
    if (n->token_count == ACCESS_TOKEN_LIMIT || n->receipt_count == ACCESS_RECEIPT_LIMIT || pending_count(n) >= ACCESS_OUTBOX_LIMIT) {
        out = result(s, ACCESS_LIMIT); discard(n); goto done;
    }
    token_t *t = &n->tokens[n->token_count]; memset(t, 0, sizeof(*t));
    char code[17] = {0}; unsigned generated = 0;
    if (!issue_material(n, t, code, &generated)) {
        OPENSSL_cleanse(code, sizeof(code)); discard(n); out = result(s, ACCESS_UNAVAILABLE); goto done;
    }
    t->info.ref.revision = 1; memcpy(t->info.label, label, strlen(label) + 1);
    t->info.created_at = now; t->info.has_expiry = has_expiry; t->info.expires_at = has_expiry ? expires : 0;
    t->info.state = ACCESS_TOKEN_PENDING; t->info.route_pending = true; memcpy(t->route_request, id, 33);
    n->token_count++; new_receipt(n, id, hash, t, ACCESS_OP_ISSUE, ACCESS_PENDING, now);
    access_route_t request = route_for(t); access_outcome_t o = commit(s, n, false); discard(n);
    if (o != ACCESS_COMMITTED) { out = result(s, o); OPENSSL_cleanse(code, sizeof(code)); goto done; }
    out = receipt_result(s, find_receipt(s->state, id));
    access_outcome_t remote = ACCESS_PENDING;
    for (;;) {
        request.deadline_monotonic_ms = deadline;
        int64_t current = monotonic_ms();
        if (route && current > 0 && current < deadline) {
            pthread_mutex_unlock(&s->mutex);
            remote = route(context, &request);
            pthread_mutex_lock(&s->mutex);
        }
        if (remote != ACCESS_ROUTE_COLLISION || generated >= 3 || s->poisoned || s->fenced) break;
        t = find_token(s->state, request.token.token_id);
        if (!t || t->info.state != ACCESS_TOKEN_PENDING || t->info.ref.revision != request.token.revision) break;
        n = candidate(s, now);
        if (!n) { remote = ACCESS_UNAVAILABLE; break; }
        t = find_token(n, request.token.token_id);
        if (!issue_material(n, t, code, &generated) || !random_id(t->route_request)) {
            discard(n); remote = ACCESS_UNAVAILABLE; break;
        }
        receipt_t *retry = find_receipt(n, id); memcpy(retry->token, t->info.ref.token_id, 33);
        retry->token_revision = t->info.ref.revision; retry->revision = n->revision;
        access_route_t next_request = route_for(t);
        o = commit(s, n, false); discard(n);
        if (o != ACCESS_COMMITTED) { remote = o; break; }
        request = next_request; remote = ACCESS_PENDING;
    }
    if (monotonic_ms() >= deadline) remote = ACCESS_UNAVAILABLE;
    if (remote != ACCESS_COMMITTED || s->poisoned || s->fenced) {
        out = result(s, s->poisoned ? ACCESS_INDETERMINATE : ACCESS_PENDING);
        out.token = request.token; out.route_pending = true;
        OPENSSL_cleanse(code, sizeof(code)); goto done;
    }
    t = find_token(s->state, request.token.token_id);
    if (!t || t->info.state != ACCESS_TOKEN_PENDING || t->info.ref.revision != request.token.revision) {
        out = result(s, ACCESS_DENIED); OPENSSL_cleanse(code, sizeof(code)); goto done;
    }
    n = candidate(s, now);
    if (!n) { out = result(s, ACCESS_UNAVAILABLE); OPENSSL_cleanse(code, sizeof(code)); goto done; }
    t = find_token(n, request.token.token_id); t->info.state = ACCESS_TOKEN_ACTIVE; t->info.route_pending = false;
    receipt_t *r = find_receipt(n, id); r->outcome = ACCESS_COMMITTED; r->revision = n->revision;
    o = commit(s, n, false); discard(n);
    if (o == ACCESS_COMMITTED) {
        out = receipt_result(s, find_receipt(s->state, id)); out.outcome = ACCESS_COMMITTED; memcpy(out.code, code, sizeof(code));
    } else out = result(s, o);
    OPENSSL_cleanse(code, sizeof(code));
done:
    pthread_mutex_unlock(&s->mutex); return out;
}

static access_result_t mutate(access_store_t *s, const char *id, uint64_t expected,
    const char *token_id, int64_t now, bool remove_token)
{
    access_result_t out = { .outcome = ACCESS_INVALID }; uint8_t hash[32];
    access_operation_t op = remove_token ? ACCESS_OP_REMOVE : ACCESS_OP_REVOKE;
    if (!s || !hex_id(id) || !hex_id(token_id) || now <= 0 || !fingerprint(op, expected, token_id, false, 0, hash)) return out;
    pthread_mutex_lock(&s->mutex); receipt_t *old = find_receipt(s->state, id);
    bool resume_remove = false;
    if (s->poisoned || s->fenced) { out = result(s, ACCESS_UNAVAILABLE); goto done; }
    if (old) {
        if (CRYPTO_memcmp(hash, old->fingerprint, 32)) { out = result(s, ACCESS_CONFLICT); goto done; }
        token_t *retained = find_token(s->state, token_id);
        resume_remove = remove_token && old->outcome == ACCESS_PENDING && retained &&
            retained->info.state == ACCESS_TOKEN_REVOKED && !retained->info.route_pending;
        if (!resume_remove) { out = receipt_result(s, old); goto done; }
    }
    if ((!resume_remove && expected != s->state->revision) || now < s->state->clock) { out = result(s, ACCESS_CONFLICT); goto done; }
    token_t *current = find_token(s->state, token_id);
    if (!current) { out = result(s, ACCESS_NOT_FOUND); goto done; }
    snapshot_t *n = candidate(s, now);
    if (!n) { out = result(s, ACCESS_UNAVAILABLE); goto done; }
    prune(n, now); token_t *t = find_token(n, token_id);
    if (!resume_remove && n->receipt_count == ACCESS_RECEIPT_LIMIT) { out = result(s, ACCESS_LIMIT); discard(n); goto done; }
    if (remove_token) {
        if (t->info.state != ACCESS_TOKEN_REVOKED) {
            t->info.state = ACCESS_TOKEN_REVOKED; t->info.ref.revision++; t->info.route_pending = true;
            memcpy(t->route_request, id, 33);
            for (size_t i = 0; i < n->receipt_count; i++) {
                receipt_t *r = &n->receipts[i];
                if (!strcmp(r->token, token_id) && r->operation == ACCESS_OP_ISSUE && r->outcome == ACCESS_PENDING) {
                    r->outcome = ACCESS_LOCALLY_REVOKED; r->revision = n->revision; r->token_revision = t->info.ref.revision;
                }
            }
        }
        if (t->info.route_pending) new_receipt(n, id, hash, t, op, ACCESS_PENDING, now);
        else {
            if (n->tombstone_count == ACCESS_TOMBSTONE_LIMIT) { out = result(s, ACCESS_LIMIT); discard(n); goto done; }
            tombstone_t *dead = &n->tombstones[n->tombstone_count++]; memcpy(dead->id, token_id, 33);
            dead->removed = now; dead->revision = n->revision;
            if (resume_remove) {
                receipt_t *r = find_receipt(n, id); r->outcome = ACCESS_COMMITTED; r->revision = n->revision;
            } else new_receipt(n, id, hash, t, op, ACCESS_COMMITTED, now);
            /* Multiple independently idempotent removals can await the same
             * remote acknowledgement/capacity. Erasure completes all of them. */
            for (size_t i = 0; i < n->receipt_count; i++) {
                receipt_t *r = &n->receipts[i];
                if (!strcmp(r->token, token_id) && r->operation == ACCESS_OP_REMOVE && r->outcome == ACCESS_PENDING) {
                    r->outcome = ACCESS_COMMITTED; r->revision = n->revision;
                }
            }
            *t = n->tokens[--n->token_count]; memset(&n->tokens[n->token_count], 0, sizeof(*t));
        }
    } else {
        if (t->info.state != ACCESS_TOKEN_REVOKED) {
            if (t->info.ref.revision == UINT64_MAX) {
                out = result(s, ACCESS_LIMIT); discard(n); goto done;
            }
            t->info.state = ACCESS_TOKEN_REVOKED; t->info.ref.revision++; t->info.route_pending = true;
            memcpy(t->route_request, id, 33);
            /* A pending issue can no longer activate, even if its remote callback
             * is currently in flight. Preserve the original receipt for recovery. */
            for (size_t i = 0; i < n->receipt_count; i++) {
                receipt_t *r = &n->receipts[i];
                if (!strcmp(r->token, token_id) && r->outcome == ACCESS_PENDING) {
                    r->outcome = ACCESS_LOCALLY_REVOKED; r->revision = n->revision; r->token_revision = t->info.ref.revision;
                }
            }
        }
        new_receipt(n, id, hash, t, op, t->info.route_pending ? ACCESS_LOCALLY_REVOKED : ACCESS_COMMITTED, now);
    }
    access_outcome_t o = commit(s, n, false); discard(n);
    out = o == ACCESS_COMMITTED ? receipt_result(s, find_receipt(s->state, id)) : result(s, o);
done:
    pthread_mutex_unlock(&s->mutex); return out;
}
access_result_t access_store_revoke(access_store_t *s, const char *id, uint64_t expected, const char *token_id, int64_t now)
{
    return mutate(s, id, expected, token_id, now, false);
}
access_result_t access_store_remove(access_store_t *s, const char *id, uint64_t expected, const char *token_id, int64_t now)
{
    return mutate(s, id, expected, token_id, now, true);
}
access_outcome_t access_store_outbox(access_store_t *s, access_route_t *rows, size_t capacity, size_t *count)
{
    if (!s || !rows || !count || capacity > ACCESS_OUTBOX_LIMIT) return ACCESS_INVALID;
    *count = 0; pthread_mutex_lock(&s->mutex);
    access_outcome_t o = s->poisoned ? ACCESS_INDETERMINATE : ACCESS_COMMITTED;
    if (o == ACCESS_COMMITTED) for (unsigned pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < s->state->token_count && *count < capacity; i++) {
            token_t *t = &s->state->tokens[i];
            bool terminal = t->info.state != ACCESS_TOKEN_PENDING;
            if (t->info.route_pending && terminal == (pass == 0)) rows[(*count)++] = route_for(t);
        }
    }
    pthread_mutex_unlock(&s->mutex); return o;
}
access_outcome_t access_store_route_ack(access_store_t *s, const access_route_t *ack, int64_t now)
{
    if (!s || !ack || !hex_id(ack->request_id) || !hex_id(ack->token.token_id) || !ack->revoke || now <= 0) return ACCESS_INVALID;
    pthread_mutex_lock(&s->mutex); access_outcome_t o = ACCESS_CONFLICT;
    token_t *t = find_token(s->state, ack->token.token_id);
    if (s->poisoned || s->fenced) { o = ACCESS_UNAVAILABLE; goto done; }
    if (!t || t->info.state != ACCESS_TOKEN_REVOKED || t->info.ref.revision != ack->token.revision ||
            strcmp(t->route_request, ack->request_id) || CRYPTO_memcmp(t->index, ack->index, 32) ||
            t->info.has_expiry != ack->has_expiry || (ack->has_expiry && t->info.expires_at != ack->expires_at)) goto done;
    if (!t->info.route_pending) { o = ACCESS_COMMITTED; goto done; }
    snapshot_t *n = candidate(s, now); if (!n) { o = ACCESS_UNAVAILABLE; goto done; }
    t = find_token(n, ack->token.token_id); t->info.route_pending = false;
    for (size_t i = 0; i < n->receipt_count; i++) {
        receipt_t *r = &n->receipts[i];
        if (!strcmp(r->token, t->info.ref.token_id) && r->outcome == ACCESS_LOCALLY_REVOKED) {
            /* An interrupted issue remains locally revoked, never becomes a
             * successful issue receipt merely because remote revocation finished. */
            if (r->operation == ACCESS_OP_REVOKE) r->outcome = ACCESS_COMMITTED;
            r->revision = n->revision;
        }
    }
    bool removing = false;
    for (size_t i = 0; i < n->receipt_count; i++)
        if (!strcmp(n->receipts[i].token, t->info.ref.token_id) &&
                n->receipts[i].operation == ACCESS_OP_REMOVE && n->receipts[i].outcome == ACCESS_PENDING) removing = true;
    if (removing) {
        prune(n, now);
        if (n->tombstone_count < ACCESS_TOMBSTONE_LIMIT) {
            tombstone_t *dead = &n->tombstones[n->tombstone_count++];
            memcpy(dead->id, t->info.ref.token_id, 33); dead->removed = now; dead->revision = n->revision;
            for (size_t i = 0; i < n->receipt_count; i++) {
                receipt_t *r = &n->receipts[i];
                if (!strcmp(r->token, dead->id) && r->operation == ACCESS_OP_REMOVE && r->outcome == ACCESS_PENDING) {
                    r->outcome = ACCESS_COMMITTED; r->revision = n->revision;
                }
            }
            *t = n->tokens[--n->token_count]; memset(&n->tokens[n->token_count], 0, sizeof(*t));
        }
    }
    o = commit(s, n, false); discard(n);
done:
    pthread_mutex_unlock(&s->mutex); return o;
}
access_outcome_t access_store_authorize(access_store_t *s, const char code[16],
    const uint8_t identity[32], int64_t now, access_token_ref_t *ref)
{
    if (ref) memset(ref, 0, sizeof(*ref));
    if (!s || !ref || !identity || !code || now <= 0 || !access_code_valid(code, 16)) return ACCESS_DENIED;
    uint8_t route[32], index[32], verifier[32];
    if (!access_code_derive(code, identity, route, index, verifier)) return ACCESS_DENIED;
    OPENSSL_cleanse(route, sizeof(route)); OPENSSL_cleanse(index, sizeof(index));
    pthread_mutex_lock(&s->mutex); access_outcome_t o = ACCESS_DENIED;
    if (!s->protected_policy || s->poisoned || s->fenced || now < s->state->clock || now < s->observed_clock || CRYPTO_memcmp(identity, s->state->identity, 32)) goto done;
    token_t *t = NULL;
    for (size_t i = 0; i < s->state->token_count; i++) {
        token_t *a = &s->state->tokens[i];
        bool equal = CRYPTO_memcmp(a->verifier, verifier, 32) == 0;
        if (equal) t = a;
    }
    if (!t || t->info.state != ACCESS_TOKEN_ACTIVE) goto done;
    if (now > s->observed_clock) s->observed_clock = now;
    if (t->info.has_expiry && (now >= t->info.expires_at || s->observed_clock >= t->info.expires_at || s->state->clock >= t->info.expires_at)) {
        snapshot_t *expired = candidate(s, now);
        if (!expired) { s->poisoned = true; o = ACCESS_UNAVAILABLE; goto done; }
        token_t *ended = find_token(expired, t->info.ref.token_id);
        ended->info.state = ACCESS_TOKEN_EXPIRED; ended->info.ref.revision++;
        o = commit(s, expired, false); discard(expired);
        if (o == ACCESS_COMMITTED) o = ACCESS_DENIED;
        else s->poisoned = true;
        goto done;
    }
    snapshot_t *n = candidate(s, now); if (!n) { o = ACCESS_UNAVAILABLE; goto done; }
    n->revision--; /* Admission audit has its own durable sequence, not management CAS. */
    token_t *a = find_token(n, t->info.ref.token_id);
    int64_t admitted = now;
    if (a->info.history_count == ACCESS_HISTORY_LIMIT) {
        memmove(a->info.history, a->info.history + 1, (ACCESS_HISTORY_LIMIT - 1) * sizeof(int64_t));
        a->info.history_count--;
    }
    a->info.history[a->info.history_count++] = admitted; a->info.last_admitted_at = admitted;
    access_token_ref_t accepted = a->info.ref;
    o = commit(s, n, false); discard(n);
    if (o == ACCESS_COMMITTED) *ref = accepted;
done:
    OPENSSL_cleanse(verifier, sizeof(verifier)); pthread_mutex_unlock(&s->mutex); return o;
}
static bool session_valid_locked(access_store_t *s, const access_token_ref_t *ref, int64_t now)
{
    token_t *t = find_token(s->state, ref->token_id);
    return !s->poisoned && !s->fenced && t && t->info.state == ACCESS_TOKEN_ACTIVE &&
        t->info.ref.revision == ref->revision && (!t->info.has_expiry ||
        (now >= s->state->clock && now >= s->observed_clock && now < t->info.expires_at && s->observed_clock < t->info.expires_at && s->state->clock < t->info.expires_at));
}
bool access_store_session_valid(access_store_t *s, const access_token_ref_t *ref, int64_t now)
{
    if (!s || !ref || !hex_id(ref->token_id) || now <= 0) return false;
    pthread_mutex_lock(&s->mutex); bool valid = session_valid_locked(s, ref, now);
    pthread_mutex_unlock(&s->mutex); return valid;
}
access_session_state_t access_store_session_check(access_store_t *s, const access_token_ref_t *ref, int64_t now)
{
    if (!s || !ref || !hex_id(ref->token_id) || now <= 0) return ACCESS_SESSION_DENIED;
    if (pthread_mutex_trylock(&s->mutex) != 0) return ACCESS_SESSION_BUSY;
    if (now > s->observed_clock) s->observed_clock = now;
    token_t *t = find_token(s->state, ref->token_id);
    bool expiry_pending = !s->poisoned && !s->fenced && t &&
        t->info.state == ACCESS_TOKEN_ACTIVE && t->info.ref.revision == ref->revision &&
        t->info.has_expiry && s->observed_clock >= t->info.expires_at;
    bool valid = session_valid_locked(s, ref, now); pthread_mutex_unlock(&s->mutex);
    return expiry_pending ? ACCESS_SESSION_BUSY : valid ? ACCESS_SESSION_VALID : ACCESS_SESSION_DENIED;
}
access_outcome_t access_store_expire(access_store_t *s, int64_t now)
{
    if (!s || now <= 0) return ACCESS_INVALID;
    pthread_mutex_lock(&s->mutex); access_outcome_t o = ACCESS_COMMITTED;
    if (s->poisoned || s->fenced) { o = ACCESS_UNAVAILABLE; goto done; }
    if (now > s->observed_clock) s->observed_clock = now;
    snapshot_t *n = NULL;
    for (size_t i = 0; i < s->state->token_count; i++) {
        token_t *t = &s->state->tokens[i];
        if (t->info.state != ACCESS_TOKEN_ACTIVE || !t->info.has_expiry ||
                (s->observed_clock < t->info.expires_at && s->state->clock < t->info.expires_at)) continue;
        if (n == NULL) n = candidate(s, now);
        if (n == NULL) { s->poisoned = true; o = ACCESS_UNAVAILABLE; goto done; }
        n->tokens[i].info.state = ACCESS_TOKEN_EXPIRED; n->tokens[i].info.ref.revision++;
    }
    if (n) { o = commit(s, n, false); discard(n); if (o != ACCESS_COMMITTED) s->poisoned = true; }
done:
    pthread_mutex_unlock(&s->mutex); return o;
}
access_outcome_t access_store_flush_for_shutdown(access_store_t *s)
{
    if (!s) return ACCESS_INVALID;
    pthread_mutex_lock(&s->mutex); s->fenced = true;
    access_outcome_t o = ACCESS_INDETERMINATE;
    if (!s->poisoned) {
        int fd = openat(s->directory, SNAPSHOT, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        bool ok = fd >= 0 && private_file(fd, geteuid()) && fsync(fd) == 0;
        if (fd >= 0 && close(fd) != 0) ok = false;
        if (ok && fsync(s->directory) == 0) {
            snapshot_t *check = read_snapshot(s->directory, s->state->identity);
            if (check && check->revision == s->state->revision && check->commit_sequence == s->state->commit_sequence) o = ACCESS_COMMITTED;
            discard(check);
        }
        if (o != ACCESS_COMMITTED) s->poisoned = true;
    }
    pthread_mutex_unlock(&s->mutex); return o;
}
void access_result_cleanse(access_result_t *r) { if (r) OPENSSL_cleanse(r, sizeof(*r)); }
const char *access_outcome_name(access_outcome_t o)
{
    static const char *names[] = {"committed", "pending", "locally_revoked_route_pending", "conflict", "denied",
        "unavailable", "save_failed", "indeterminate", "already_committed_secret_unavailable", "limit", "invalid", "not_found"};
    return (unsigned) o < sizeof(names) / sizeof(names[0]) ? names[o] : "invalid";
}
access_operator_lookup_t access_operator_lookup(const char *path, const char *account)
{
    if (!path || !account || !label_valid(account) || strlen(path) >= PATH_MAX) return ACCESS_OPERATOR_UNAVAILABLE;
    char parent[PATH_MAX]; memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/'); if (!slash || slash[1] == '\0') return ACCESS_OPERATOR_UNAVAILABLE;
    char name[NAME_MAX + 1]; if (strlen(slash + 1) > NAME_MAX) return ACCESS_OPERATOR_UNAVAILABLE;
    memcpy(name, slash + 1, strlen(slash + 1) + 1);
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    int directory = open_directory(parent, false); if (directory < 0) return ACCESS_OPERATOR_UNAVAILABLE;
    /* Re-walk from the root with the stricter root-managed config boundary. */
    if (parent[0] != '/') { close(directory); return ACCESS_OPERATOR_UNAVAILABLE; }
    char ancestry[PATH_MAX]; memcpy(ancestry, parent, strlen(parent) + 1);
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL, *part = strtok_r(ancestry, "/", &save);
    bool trusted = root >= 0 && trusted_directory(root, true);
    while (trusted && part) {
        int next = openat(root, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(root); root = next; trusted = root >= 0 && trusted_directory(root, true);
        part = strtok_r(NULL, "/", &save);
    }
    if (root >= 0) close(root);
    if (!trusted) { close(directory); return ACCESS_OPERATOR_UNAVAILABLE; }
    struct stat st;
    /* Root must own the containing directory, with no unprivileged writers. */
    if (fstat(directory, &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022)) { close(directory); return ACCESS_OPERATOR_UNAVAILABLE; }
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC); close(directory);
    if (fd < 0) return ACCESS_OPERATOR_UNAVAILABLE;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != 0 ||
            !(((st.st_mode & 07777) == 0600 && geteuid() == 0) ||
              (((st.st_mode & 07777) == 0440 || (st.st_mode & 07777) == 0640) && st.st_gid == getegid())) ||
            st.st_size < 0 || st.st_size > 16384) { close(fd); return ACCESS_OPERATOR_UNAVAILABLE; }
    char bytes[16385]; size_t n = (size_t) st.st_size, pos = 0;
    while (pos < n) {
        ssize_t got = read(fd, bytes + pos, n - pos);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        pos += (size_t) got;
    }
    char extra; ssize_t tail = read(fd, &extra, 1); bool ok = close(fd) == 0;
    if (!ok || pos != n || tail != 0 || memchr(bytes, '\0', n) || (n != 0 && bytes[n - 1] != '\n')) return ACCESS_OPERATOR_UNAVAILABLE;
    bytes[n] = '\0'; char *lines[256]; size_t count = 0; bool allowed = false;
    char *begin = bytes;
    while (begin < bytes + n) {
        char *end = strchr(begin, '\n'); if (!end || count == 256) return ACCESS_OPERATOR_UNAVAILABLE;
        *end = '\0'; if (!label_valid(begin)) return ACCESS_OPERATOR_UNAVAILABLE;
        /* Canonical account names are ASCII lower-case identifiers; exact
         * identity equality is deliberate, no Unicode/display normalization. */
        for (char *p = begin; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return ACCESS_OPERATOR_UNAVAILABLE;
        for (size_t i = 0; i < count; i++) if (!strcmp(lines[i], begin)) return ACCESS_OPERATOR_UNAVAILABLE;
        lines[count++] = begin; if (!strcmp(begin, account)) allowed = true;
        begin = end + 1;
    }
    return allowed ? ACCESS_OPERATOR_LISTED : ACCESS_OPERATOR_UNLISTED;
}

bool access_operator_allowed(const char *path, const char *account)
{
    return access_operator_lookup(path, account) == ACCESS_OPERATOR_LISTED;
}

int access_state_lock(const char *directory)
{
    int fd = open_directory(directory, true);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; }
    return fd;
}
void access_state_unlock(int fd) { if (fd >= 0) close(fd); }
access_outcome_t access_store_inspect(const char *directory, const uint8_t identity[32],
    bool protected_policy, access_status_t *status)
{
    if (!status || !identity) return ACCESS_INVALID;
    memset(status, 0, sizeof(*status));
    int fd = access_state_lock(directory); if (fd < 0) return ACCESS_UNAVAILABLE;
    snapshot_t *s = read_snapshot(fd, identity);
    if (s == NULL) { close(fd); return ACCESS_UNAVAILABLE; }
    status->schema_version = ACCESS_STORE_SCHEMA; memcpy(status->server_identity, s->identity, 32);
    status->protected_policy = protected_policy; status->integrity_ok = true; status->durability_ok = true;
    status->revision = s->revision; status->pending_route_sync = pending_count(s);
    discard(s); close(fd); return ACCESS_COMMITTED;
}

bool access_store_absent(const char *directory)
{
    if (!directory || !*directory || strlen(directory) >= PATH_MAX) return false;
    char parent[PATH_MAX]; memcpy(parent, directory, strlen(directory) + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1] || !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..")) return false;
    char leaf[NAME_MAX + 1]; if (strlen(slash + 1) > NAME_MAX) return false;
    memcpy(leaf, slash + 1, strlen(slash + 1) + 1);
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    if (!strcmp(parent, ".")) {
        if (!getcwd(parent, sizeof(parent))) return false;
    }
    int fd = open_directory(parent, false); if (fd < 0) return false;
    struct stat st; int found = fstatat(fd, leaf, &st, AT_SYMLINK_NOFOLLOW);
    bool absent = found != 0 && errno == ENOENT; close(fd); return absent;
}
