/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <admin_shutdown.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static bool valid_reason(const unsigned char *p, size_t len) {
    for (size_t i = 0; i < len;) {
        uint32_t cp = p[i++];
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 0x1f;
            continuation = 1;
            minimum = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 0x0f;
            continuation = 2;
            minimum = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            continuation = 3;
            minimum = 0x10000;
        } else if (cp >= 0x80) {
            return false;
        }
        if (continuation > len - i) {
            return false;
        }
        for (unsigned j = 0; j < continuation; j++) {
            if ((p[i] & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i++] & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029 ||
            (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
            (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff) {
            return false;
        }
    }
    return true;
}

bool admin_shutdown_parse(const char *data, size_t size, admin_shutdown_request *request) {
    const char prefix[] = "ATRINIK-ADMIN/1 SHUTDOWN ";
    size_t pos = sizeof(prefix) - 1;
    if (size > ADMIN_SHUTDOWN_REQUEST_MAX || size < pos + 32 + 1 + 2 + 1 + 1 + 1 ||
        memcmp(data, prefix, pos) != 0 || data[size - 1] != '\n' ||
        memchr(data, '\0', size) != NULL) {
        return false;
    }
    memset(request, 0, sizeof(*request));
    for (size_t i = 0; i < 32; i++) {
        char c = data[pos++];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
        request->id[i] = c;
    }
    if (data[pos++] != ' ' || data[pos] < '1' || data[pos] > '9') {
        return false;
    }
    size_t start = pos;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        if (pos - start >= 3) {
            return false;
        }
        request->seconds = request->seconds * 10 + (unsigned)(data[pos++] - '0');
    }
    if (request->seconds < 30 || request->seconds > 600 || pos >= size ||
        data[pos++] != ' ') {
        return false;
    }
    size_t reason_len = size - pos - 1;
    if (reason_len == 0 || reason_len > ADMIN_SHUTDOWN_REASON_MAX ||
        !valid_reason((const unsigned char *)data + pos, reason_len)) {
        return false;
    }
    memcpy(request->reason, data + pos, reason_len);
    return true;
}

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static struct {
    int listener;
    int directory;
    int client;
    dev_t device;
    ino_t inode;
    char name[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char input[ADMIN_SHUTDOWN_REQUEST_MAX + 1];
    size_t used;
    uint64_t accepted;
    uint64_t executing;
    uint64_t job;
    char output[32768 + 65];
    size_t output_size, output_sent;
    admin_access_start_fn access_start;
    admin_access_poll_fn access_poll;
    admin_access_cancel_fn access_cancel;
    admin_shutdown_schedule_fn schedule;
    admin_shutdown_request pending;
    bool expired;
    bool failed;
} state = {.listener = -1, .directory = -1, .client = -1};

#ifdef ATRINIK_TESTING
static bool test_sync_failure;
void admin_shutdown_fail_sync_for_test(bool fail) {
    test_sync_failure = fail;
}
#endif

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void cleanse(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--)
        *p++ = 0;
}

void admin_shutdown_set_access(admin_access_start_fn start,
                               admin_access_poll_fn poll,
                               admin_access_cancel_fn cancel) {
    state.access_start = start;
    state.access_poll = poll;
    state.access_cancel = cancel;
}

static void close_client(void) {
    if (state.job && state.access_cancel)
        state.access_cancel(state.job);
    state.job = 0;
    state.executing = 0;
    state.output_size = state.output_sent = 0;
    cleanse(state.input, sizeof(state.input));
    cleanse(state.output, sizeof(state.output));
    if (state.client >= 0) {
        close(state.client);
        state.client = -1;
    }
    state.used = 0;
}

void admin_shutdown_deinit(void) {
    close_client();
    if (state.listener >= 0) {
        close(state.listener);
        state.listener = -1;
    }
    if (state.directory >= 0) {
        struct stat st;
        if (fstatat(state.directory, state.name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISSOCK(st.st_mode) && st.st_dev == state.device && st.st_ino == state.inode) {
            (void)unlinkat(state.directory, state.name, 0);
        }
        close(state.directory);
        state.directory = -1;
    }
}

/* Hold each no-follow directory descriptor while descending. Never create or
 * remove an existing endpoint; an operator must inspect stale sockets. */
bool admin_shutdown_init(const char *path, admin_shutdown_schedule_fn schedule) {
    if (path[0] == '\0') {
        return true;
    }
    if (state.listener >= 0 || path[0] != '/' || strlen(path) >= sizeof(struct sockaddr_un) -
        offsetof(struct sockaddr_un, sun_path) || schedule == NULL) {
        return false;
    }
    char copy[sizeof(((struct sockaddr_un *)0)->sun_path)];
    memcpy(copy, path, strlen(path) + 1);
    int dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        return false;
    }
    char *part = copy + 1;
    char *slash;
    while ((slash = strchr(part, '/')) != NULL) {
        *slash = '\0';
        if (part[0] == '\0' || strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(dir);
            return false;
        }
        int next = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        struct stat st;
        if (next < 0) {
            return false;
        }
        if (fstat(next, &st) != 0 || (st.st_uid != 0 && st.st_uid != geteuid()) ||
            (st.st_mode & 0022) != 0) {
            close(next);
            return false;
        }
        dir = next;
        part = slash + 1;
    }
    struct stat parent;
    if (part[0] == '\0' || strcmp(part, ".") == 0 || strcmp(part, "..") == 0 ||
        fstat(dir, &parent) != 0 || parent.st_uid != geteuid() ||
        (parent.st_mode & 0777) != 0700) {
        close(dir);
        return false;
    }
    struct stat existing;
    if (fstatat(dir, part, &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        close(dir);
        return false;
    }
    state.directory = dir;
    snprintf(state.name, sizeof(state.name), "%s", part);
    state.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int length = snprintf(address.sun_path, sizeof(address.sun_path),
                          "/proc/self/fd/%d/%s", dir, part);
    if (state.listener < 0 || length < 0 || (size_t)length >= sizeof(address.sun_path) ||
        bind(state.listener, (struct sockaddr *)&address, sizeof(address)) != 0) {
        admin_shutdown_deinit();
        return false;
    }
    if (fstatat(dir, part, &existing, AT_SYMLINK_NOFOLLOW) != 0) {
        admin_shutdown_deinit();
        return false;
    }
    state.device = existing.st_dev;
    state.inode = existing.st_ino;
    if (fchmodat(dir, part, 0600, 0) != 0 || listen(state.listener, 4) != 0) {
        admin_shutdown_deinit();
        return false;
    }
    state.schedule = schedule;
    state.pending.id[0] = '\0';
    state.expired = false;
    state.failed = false;
    return true;
}

static bool result_name(const char *id, const char *suffix, char *out, size_t size) {
    int n = snprintf(out, size, "%s.%s.%s", state.name, id, suffix);
    return n >= 0 && (size_t)n < size;
}

static bool publish_result(const char *result) {
    char temporary[NAME_MAX + 1], final[NAME_MAX + 1], body[128];
    if (!result_name(state.pending.id, "tmp", temporary, sizeof(temporary)) ||
        !result_name(state.pending.id, "result", final, sizeof(final))) {
        return false;
    }
    int fd = openat(state.directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                    O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    int n = snprintf(body, sizeof(body), "ATRINIK-ADMIN/1 RESULT %s %s\n",
                     state.pending.id, result);
    bool ok = write(fd, body, (size_t)n) == n;
#ifdef ATRINIK_TESTING
    if (test_sync_failure) {
        ok = false;
    }
#endif
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (ok) {
        ok = linkat(state.directory, temporary, state.directory, final, 0) == 0;
    }
    if (unlinkat(state.directory, temporary, 0) != 0 || fsync(state.directory) != 0) {
        ok = false;
    }
    return ok;
}

bool admin_shutdown_cancel(void) {
    if (state.pending.id[0] != '\0') {
        state.failed |= !publish_result("cancelled");
        state.pending.id[0] = '\0';
        state.expired = false;
    }
    return !state.failed;
}

void admin_shutdown_expired(void) {
    if (state.pending.id[0] != '\0') {
        state.expired = true;
    }
}

bool admin_shutdown_finish(bool saved) {
    if (state.pending.id[0] != '\0') {
        state.failed |= !publish_result(!state.expired ? "cancelled" : saved ? "saved" : "failed");
        state.pending.id[0] = '\0';
    }
    return !state.failed;
}

static void respond(const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(state.output)) {
        close_client();
        return;
    }
    memcpy(state.output, message, length);
    state.output_size = length;
    state.output_sent = 0;
    state.executing = monotonic_ms();
    ssize_t sent = send(state.client, state.output, state.output_size, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent > 0) {
        state.output_sent = (size_t)sent;
        if (state.output_sent == state.output_size)
            close_client();
    } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        close_client();
}

static void handle_request(void) {
    const char capabilities[] = "ATRINIK-ADMIN/1 CAPABILITIES\n";
    if (state.used == sizeof(capabilities) - 1 &&
        memcmp(state.input, capabilities, sizeof(capabilities) - 1) == 0) {
        respond(
            state.access_start && state.access_poll && state.access_cancel
                ? "ATRINIK-ADMIN/1 CAPABILITIES shutdown-v1 durable-result-v1 access-tokens-v1\n"
                : "ATRINIK-ADMIN/1 CAPABILITIES shutdown-v1 durable-result-v1\n");
        return;
    }
    const char access[] = "ATRINIK-ADMIN/1 ACCESS ";
    if (state.used > sizeof(access) && memcmp(state.input, access, sizeof(access) - 1) == 0) {
        size_t length = state.used - sizeof(access);
        const char *json = state.input + sizeof(access) - 1;
        if (state.input[state.used - 1] != '\n' || memchr(json, '\n', length) ||
            memchr(json, '\r', length) || memchr(json, 0, length)) {
            respond("ATRINIK-ADMIN/1 ERROR malformed\n");
            return;
        }
        if (!state.access_start || !state.access_poll || !state.access_cancel) {
            respond("ATRINIK-ADMIN/1 ERROR unavailable\n");
            return;
        }
        state.job = state.access_start(json, length);
        if (!state.job) {
            respond("ATRINIK-ADMIN/1 ERROR unavailable\n");
            return;
        }
        state.executing = monotonic_ms();
        cleanse(state.input, sizeof(state.input));
        return;
    }
    admin_shutdown_request request;
    if (!admin_shutdown_parse(state.input, state.used, &request)) {
        respond("ATRINIK-ADMIN/1 ERROR malformed\n");
        return;
    }
    if (state.failed) {
        respond("ATRINIK-ADMIN/1 ERROR persistence\n");
        return;
    }
    if (state.pending.id[0] != '\0') {
        if (strcmp(state.pending.id, request.id) != 0 ||
            state.pending.seconds != request.seconds ||
            strcmp(state.pending.reason, request.reason) != 0) {
            respond("ATRINIK-ADMIN/1 ERROR busy\n");
            return;
        }
    } else {
        char name[NAME_MAX + 1];
        struct stat existing;
        if (!result_name(request.id, "result", name, sizeof(name)) ||
            fstatat(state.directory, name, &existing, AT_SYMLINK_NOFOLLOW) == 0 ||
            errno != ENOENT) {
            respond("ATRINIK-ADMIN/1 ERROR reused-id\n");
            return;
        }
        if (!state.schedule(request.seconds, request.reason)) {
            respond("ATRINIK-ADMIN/1 ERROR busy\n");
            return;
        }
        state.pending = request;
        state.expired = false;
    }
    char response[96];
    snprintf(response, sizeof(response), "ATRINIK-ADMIN/1 SCHEDULED %s\n", request.id);
    respond(response);
}

void admin_shutdown_poll(void) {
    if (state.listener < 0) {
        return;
    }
    uint64_t now = monotonic_ms();
    if (state.client >= 0 &&
        (now == UINT64_MAX ||
         (state.executing ? now - state.executing >= 35000 : now - state.accepted >= 2000))) {
        close_client();
    }
    if (state.client >= 0 && state.job) {
        size_t length = 0;
        if (state.access_poll(state.job, state.output + 64, 32768 + 1, &length)) {
            state.job = 0;
            if (!length || length > 32768 || memchr(state.output + 64, 0, length)) {
                close_client();
                return;
            }
            char header[64];
            int n = snprintf(header, sizeof(header), "ATRINIK-ADMIN/1 ACCESS %zu\n", length);
            memmove(state.output + n, state.output + 64, length);
            memcpy(state.output, header, (size_t)n);
            state.output_size = (size_t)n + length;
            state.output_sent = 0;
        } else if (now - state.executing >= 30000) {
            close_client();
            return;
        } else
            return;
    }
    if (state.client >= 0 && state.output_size) {
        ssize_t sent = send(state.client,
                            state.output + state.output_sent,
                            state.output_size - state.output_sent,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            state.output_sent += (size_t)sent;
            if (state.output_sent == state.output_size)
                close_client();
        } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            close_client();
        return;
    }
    if (state.client < 0) {
        state.client = accept4(state.listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (state.client < 0) {
            return;
        }
        struct ucred credentials;
        socklen_t size = sizeof(credentials);
        if (getsockopt(state.client, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
            size != sizeof(credentials) || credentials.uid != 0) {
            close_client();
            return;
        }
        state.accepted = now;
    }
    ssize_t n = recv(state.client, state.input + state.used, sizeof(state.input) - state.used,
                     MSG_DONTWAIT);
    if (n > 0) {
        state.used += (size_t)n;
        if (state.used > ADMIN_SHUTDOWN_REQUEST_MAX) {
            respond("ATRINIK-ADMIN/1 ERROR too-long\n");
        }
    } else if (n == 0) {
        handle_request();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_client();
    }
}
#else
void admin_shutdown_set_access(admin_access_start_fn start,
                               admin_access_poll_fn poll,
                               admin_access_cancel_fn cancel) {
    (void)start;
    (void)poll;
    (void)cancel;
}
bool admin_shutdown_init(const char *path, admin_shutdown_schedule_fn schedule) {
    (void)schedule;
    return path[0] == '\0';
}
#ifdef ATRINIK_TESTING
void admin_shutdown_fail_sync_for_test(bool fail) { (void)fail; }
#endif
void admin_shutdown_poll(void) {}
void admin_shutdown_deinit(void) {}
bool admin_shutdown_cancel(void) { return true; }
void admin_shutdown_expired(void) {}
bool admin_shutdown_finish(bool saved) { (void)saved; return true; }
#endif
