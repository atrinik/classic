/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <access_admin.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *operations[] =
    {"issue", "list", "history", "revoke", "remove", "status", "result"};
static bool hex_id(const char *s) {
    if (strlen(s) != 32)
        return false;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
            return false;
    return true;
}
static bool decimal(const char *s, uint64_t *out) {
    if (!*s || (*s == '0' && s[1]))
        return false;
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return true;
}
static bool utf8(const unsigned char *p) {
    while (*p) {
        uint32_t cp = *p++;
        unsigned n = 0;
        uint32_t min = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 31;
            n = 1;
            min = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 15;
            n = 2;
            min = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            n = 3;
            min = 0x10000;
        } else if (cp >= 0x80)
            return false;
        while (n--) {
            if ((*p & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (*p++ & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
            (cp >= 0x7f && cp <= 0x9f) || (cp >= 0x200b && cp <= 0x200f) ||
            (cp >= 0x2028 && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff)
            return false;
    }
    return true;
}
typedef struct {
    const unsigned char *p, *end;
} parser;
static void space(parser *p) {
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\r' || *p->p == '\n'))
        p->p++;
}
static bool character(parser *p, unsigned char c) {
    space(p);
    if (p->p == p->end || *p->p != c)
        return false;
    p->p++;
    return true;
}
static bool hex4(parser *p, uint32_t *cp) {
    *cp = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (p->p == p->end)
            return false;
        unsigned c = *p->p++, v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return false;
        *cp = (*cp << 4) | v;
    }
    return true;
}
static bool string(parser *p, char *out, size_t cap) {
    if (!character(p, '"'))
        return false;
    size_t n = 0;
    while (p->p < p->end) {
        unsigned c = *p->p++;
        if (c == '"') {
            out[n] = 0;
            return utf8((const unsigned char *)out);
        }
        unsigned char encoded[4];
        size_t count = 1;
        encoded[0] = (unsigned char)c;
        if (c < 32)
            return false;
        if (c == '\\') {
            if (p->p == p->end)
                return false;
            c = *p->p++;
            if (c == '"' || c == '\\' || c == '/')
                encoded[0] = (unsigned char)c;
            else if (c == 'u') {
                uint32_t cp;
                if (!hex4(p, &cp))
                    return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t low;
                    if (p->end - p->p < 2 || p->p[0] != '\\' || p->p[1] != 'u')
                        return false;
                    p->p += 2;
                    if (!hex4(p, &low) || low < 0xdc00 || low > 0xdfff)
                        return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                }
                if (cp == 0 || (cp >= 0xdc00 && cp <= 0xdfff))
                    return false;
                if (cp < 0x80)
                    encoded[0] = (unsigned char)cp;
                else if (cp < 0x800) {
                    count = 2;
                    encoded[0] = 0xc0 | (cp >> 6);
                    encoded[1] = 0x80 | (cp & 63);
                } else if (cp < 0x10000) {
                    count = 3;
                    encoded[0] = 0xe0 | (cp >> 12);
                    encoded[1] = 0x80 | ((cp >> 6) & 63);
                    encoded[2] = 0x80 | (cp & 63);
                } else {
                    count = 4;
                    encoded[0] = 0xf0 | (cp >> 18);
                    encoded[1] = 0x80 | ((cp >> 12) & 63);
                    encoded[2] = 0x80 | ((cp >> 6) & 63);
                    encoded[3] = 0x80 | (cp & 63);
                }
            } else
                return false;
        }
        if (count >= cap - n)
            return false;
        memcpy(out + n, encoded, count);
        n += count;
    }
    return false;
}

bool access_admin_parse(const char *data, size_t size, access_admin_request_t *out) {
    static const char *keys[] = {"schema",
                                 "operation",
                                 "requestId",
                                 "expectedRevision",
                                 "label",
                                 "expiresAt",
                                 "tokenId",
                                 "cursor",
                                 "revision",
                                 "limit",
                                 "targetRequestId"};
    if (!data || !out || !size || size > ACCESS_ADMIN_REQUEST_MAX || memchr(data, 0, size))
        return false;
    parser p = {(const unsigned char *)data, (const unsigned char *)data + size};
    char values[11][129] = {{0}};
    unsigned seen = 0;
    if (!character(&p, '{'))
        return false;
    for (;;) {
        char key[32];
        if (!string(&p, key, sizeof(key)) || !character(&p, ':'))
            return false;
        unsigned k;
        for (k = 0; k < 11 && strcmp(keys[k], key); k++) {}
        if (k == 11 || (seen & (1u << k)))
            return false;
        seen |= 1u << k;
        if (k == 9) {
            space(&p);
            size_t n = 0;
            while (p.p < p.end && *p.p >= '0' && *p.p <= '9') {
                if (n >= 2)
                    return false;
                values[k][n++] = (char)*p.p++;
            }
            if (!n)
                return false;
        } else if (!string(&p, values[k], sizeof(values[k])))
            return false;
        space(&p);
        if (p.p < p.end && *p.p == '}') {
            p.p++;
            break;
        }
        if (!character(&p, ','))
            return false;
    }
    if (p.p != p.end || (seen & 7) != 7 || strcmp(values[0], "atrinik-access-admin-v1") ||
        !hex_id(values[2]))
        return false;
    memset(out, 0, sizeof(*out));
    unsigned op;
    for (op = 0; op < 7 && strcmp(values[1], operations[op]); op++) {}
    if (op == 7)
        return false;
    out->operation = (access_admin_operation_t)op;
    unsigned required = 7, allowed = 7;
    switch (out->operation) {
        case ACCESS_ADMIN_ISSUE:
            required |= (1u << 3) | (1u << 4);
            allowed = required | (1u << 5);
            break;
        case ACCESS_ADMIN_LIST:
            allowed |= (1u << 7) | (1u << 8) | (1u << 9);
            break;
        case ACCESS_ADMIN_HISTORY:
            required |= 1u << 6;
            allowed = required | (1u << 8);
            break;
        case ACCESS_ADMIN_REVOKE:
        case ACCESS_ADMIN_REMOVE:
            required |= (1u << 3) | (1u << 6);
            allowed = required;
            break;
        case ACCESS_ADMIN_RESULT:
            required |= 1u << 10;
            allowed = required;
            break;
        case ACCESS_ADMIN_STATUS:
            break;
    }
    if ((seen & required) != required || (seen & ~allowed))
        return false;
    memcpy(out->request_id, values[2], 33);
    if ((seen & (1u << 3)) && !decimal(values[3], &out->expected_revision))
        return false;
    if ((seen & (1u << 4)) && !values[4][0])
        return false;
    memcpy(out->label, values[4], sizeof(out->label));
    if (seen & (1u << 5)) {
        uint64_t expires;
        if (!decimal(values[5], &expires) || !expires || expires > UINT64_C(253402300799))
            return false;
        out->has_expiry = true;
        out->expires_at = (int64_t)expires;
    }
    if ((seen & (1u << 6)) && !hex_id(values[6]))
        return false;
    memcpy(out->token_id, values[6], 33);
    if (seen & (1u << 7)) {
        uint64_t offset;
        if (!(seen & (1u << 8)) || !decimal(values[7], &offset) || offset > ACCESS_TOKEN_LIMIT)
            return false;
        out->offset = (size_t)offset;
    }
    out->has_revision = (seen & (1u << 8)) != 0;
    if (out->has_revision && !decimal(values[8], &out->revision))
        return false;
    out->limit = ACCESS_PAGE_LIMIT;
    if (seen & (1u << 9)) {
        uint64_t limit;
        if (!decimal(values[9], &limit) || !limit || limit > ACCESS_PAGE_LIMIT)
            return false;
        out->limit = (size_t)limit;
    }
    if ((seen & (1u << 10)) && !hex_id(values[10]))
        return false;
    memcpy(out->target_request_id, values[10], 33);
    return true;
}

typedef struct {
    char *data;
    size_t used, capacity;
    bool ok;
} writer;
static void append(writer *w, const char *format, ...) {
    if (!w->ok)
        return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(w->data + w->used, w->capacity - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->capacity - w->used) {
        w->ok = false;
        return;
    }
    w->used += (size_t)n;
}
static void quoted(writer *w, const char *s) {
    append(w, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            append(w, "\\%c", c);
        else if (c < 32)
            append(w, "\\u%04x", c);
        else
            append(w, "%c", c);
    }
    append(w, "\"");
}
static void
begin(writer *w, const access_admin_request_t *r, access_outcome_t outcome, uint64_t revision) {
    append(w,
           "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\",\"requestId\":\"%s\","
           "\"outcome\":\"%s\",\"revision\":\"%" PRIu64 "\",\"result\":",
           operations[r->operation],
           r->request_id,
           access_outcome_name(outcome),
           revision);
}
static void token(writer *w, const access_token_info_t *t, bool history) {
    static const char *states[] = {"invalid", "pending", "active", "revoked", "expired"};
    append(w,
           "{\"tokenId\":\"%s\",\"revision\":\"%" PRIu64 "\",\"label\":",
           t->ref.token_id,
           t->ref.revision);
    quoted(w, t->label);
    append(w, ",\"createdAt\":\"%" PRId64 "\",\"expiresAt\":", t->created_at);
    if (t->has_expiry)
        append(w, "\"%" PRId64 "\"", t->expires_at);
    else
        append(w, "null");
    append(w,
           ",\"state\":\"%s\",\"routePending\":%s,\"lastAdmittedAt\":",
           (unsigned)t->state < 5 ? states[t->state] : "invalid",
           t->route_pending ? "true" : "false");
    if (t->last_admitted_at)
        append(w, "\"%" PRId64 "\"", t->last_admitted_at);
    else
        append(w, "null");
    if (history) {
        append(w, ",\"history\":[");
        for (size_t i = 0; i < t->history_count && i < ACCESS_HISTORY_LIMIT; i++)
            append(w, "%s\"%" PRId64 "\"", i ? "," : "", t->history[i]);
        append(w, "]");
    }
    append(w, "}");
}
static bool finish(writer *w, size_t *length) {
    append(w, "}");
    if (!w->ok || w->used > ACCESS_ADMIN_RESPONSE_MAX) {
        if (w->capacity)
            memset(w->data, 0, w->capacity);
        *length = 0;
        return false;
    }
    *length = w->used;
    return true;
}
static void status(writer *w, const access_status_t *s, bool absent) {
    char identity[65];
    for (size_t i = 0; i < 32; i++)
        snprintf(identity + 2 * i, 3, "%02x", s->server_identity[i]);
    append(w,
           "{\"state\":\"%s\",\"schemaVersion\":%u,\"serverIdentity\":\"%s\",\"policy\":\"%s\"",
           absent ? "absent_open" : "initialized",
           s->schema_version,
           identity,
           s->protected_policy ? "protected" : "open");
    if (absent)
        append(w, "}");
    else
        append(w,
               ",\"integrity\":\"%s\",\"durability\":\"%s\",\"revision\":\"%" PRIu64
               "\",\"pendingRouteSync\":%zu}",
               s->integrity_ok ? "ok" : "failed",
               s->durability_ok ? "ok" : "indeterminate",
               s->revision,
               s->pending_route_sync);
}
bool access_admin_status_encode(const access_status_t *s,
                                bool absent,
                                char *response,
                                size_t capacity,
                                size_t *length) {
    if (!s || !response || !length || !capacity || s->schema_version != ACCESS_STORE_SCHEMA ||
        s->pending_route_sync > ACCESS_TOKEN_LIMIT || (absent && s->protected_policy))
        return false;
    writer w = {response, 0, capacity, true};
    status(&w, s, absent);
    if (!w.ok) {
        memset(response, 0, capacity);
        *length = 0;
        return false;
    }
    *length = w.used;
    return true;
}
bool access_admin_execute_absent(const access_status_t *configured,
                                 const char *data,
                                 size_t size,
                                 char *response,
                                 size_t capacity,
                                 size_t *length) {
    access_admin_request_t r;
    if (!configured || !access_admin_parse(data, size, &r))
        return false;
    writer w = {response, 0, capacity, capacity > 0};
    /* No synthetic store revision, integrity or durability for absent state. */
    append(&w,
           "{\"schema\":\"atrinik-access-admin-v1\",\"operation\":\"%s\",\"requestId\":\"%s\","
           "\"outcome\":\"%s\",\"revision\":null,\"result\":",
           operations[r.operation],
           r.request_id,
           !configured->protected_policy && r.operation == ACCESS_ADMIN_STATUS ? "committed"
                                                                               : "unavailable");
    if (r.operation == ACCESS_ADMIN_STATUS && !configured->protected_policy)
        status(&w, configured, true);
    else
        append(&w, "{}");
    return finish(&w, length);
}
bool access_admin_execute(access_store_t *store,
                          const char *data,
                          size_t size,
                          access_route_callback_t route,
                          void *context,
                          char *response,
                          size_t capacity,
                          size_t *length) {
    access_admin_request_t r;
    if (!store || !access_admin_parse(data, size, &r))
        return false;
    writer w = {response, 0, capacity, capacity > 0};
    access_status_t s = access_store_status(store);
    if (r.operation == ACCESS_ADMIN_STATUS) {
        begin(&w, &r, ACCESS_COMMITTED, s.revision);
        status(&w, &s, false);
    } else if (r.operation == ACCESS_ADMIN_LIST) {
        access_token_info_t rows[ACCESS_PAGE_LIMIT];
        size_t count = 0, next = 0;
        uint64_t revision = r.has_revision ? r.revision : s.revision;
        access_outcome_t result =
            access_store_list(store, revision, r.offset, r.limit, rows, &count, &next);
        begin(&w, &r, result, s.revision);
        append(&w, "{\"tokens\":[");
        for (size_t i = 0; result == ACCESS_COMMITTED && i < count && i < ACCESS_PAGE_LIMIT; i++) {
            if (i)
                append(&w, ",");
            token(&w, &rows[i], false);
        }
        append(&w, "],\"cursor\":");
        if (result == ACCESS_COMMITTED && next)
            append(&w, "\"%zu\"", next);
        else
            append(&w, "null");
        append(&w, "}");
    } else if (r.operation == ACCESS_ADMIN_HISTORY) {
        access_token_info_t row;
        access_outcome_t result =
            access_store_history(store, r.token_id, r.has_revision ? r.revision : s.revision, &row);
        begin(&w, &r, result, s.revision);
        if (result == ACCESS_COMMITTED)
            token(&w, &row, true);
        else
            append(&w, "{}");
    } else {
        access_result_t result;
        int64_t now = (int64_t)time(NULL);
        switch (r.operation) {
            case ACCESS_ADMIN_ISSUE:
                result = access_store_issue(store,
                                            r.request_id,
                                            r.expected_revision,
                                            r.label,
                                            r.has_expiry,
                                            r.expires_at,
                                            now,
                                            route,
                                            context);
                break;
            case ACCESS_ADMIN_REVOKE:
                result =
                    access_store_revoke(store, r.request_id, r.expected_revision, r.token_id, now);
                break;
            case ACCESS_ADMIN_REMOVE:
                result =
                    access_store_remove(store, r.request_id, r.expected_revision, r.token_id, now);
                break;
            default:
                result = access_store_result(store, r.target_request_id);
                break;
        }
        begin(&w, &r, result.outcome, result.revision);
        append(&w,
               "{\"tokenId\":\"%s\",\"tokenRevision\":\"%" PRIu64 "\",\"routePending\":%s",
               result.token.token_id,
               result.token.revision,
               result.route_pending ? "true" : "false");
        if (r.operation == ACCESS_ADMIN_ISSUE && result.outcome == ACCESS_COMMITTED &&
            result.code[0]) {
            append(&w, ",\"code\":");
            quoted(&w, result.code);
        }
        append(&w, "}");
        access_result_cleanse(&result);
    }
    return finish(&w, length);
}
