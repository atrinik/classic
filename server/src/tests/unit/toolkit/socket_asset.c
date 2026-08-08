/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#include <global.h>
#include <server_main.h>
#include <server.h>
#include <metaserver_internal.h>
#include <initialization.h>
#include <check.h>
#include <checkstd.h>
#include <check_utils.h>
#include <toolkit/packet.h>
#include <toolkit/path.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

START_TEST(test_socket_asset_request_round_trip) {
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x89, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_request_append(packet, "client-maps/test.png", 123456, digest, 0);

    socket_asset_request_t request;
    ck_assert(socket_asset_request_parse(packet->data, packet->len, 0, &request));
    ck_assert_str_eq(request.path, "client-maps/test.png");
    ck_assert_uint_eq(request.cached_size, 123456);
    ck_assert_uint_eq(request.flags, 0);
    ck_assert_mem_eq(request.cached_digest, digest, sizeof(digest));
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_stream_preface_round_trip_and_malformed) {
    uint8_t preface[SOCKET_STREAM_PREFACE_SIZE];
    socket_stream_kind_t kind = SOCKET_STREAM_ASSET;
    socket_stream_preface_encode(preface, SOCKET_STREAM_GAME);
    ck_assert(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    ck_assert_int_eq(kind, SOCKET_STREAM_GAME);

    for (size_t truncated = 0; truncated < sizeof(preface); truncated++) {
        kind = SOCKET_STREAM_ASSET;
        ck_assert(!socket_stream_preface_decode(preface, truncated, &kind));
        ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
    }

    static const size_t malformed_offsets[] = {0, 4, 5, 6, 7};
    for (size_t i = 0; i < arraysize(malformed_offsets); i++) {
        socket_stream_preface_encode(preface, SOCKET_STREAM_GAME);
        preface[malformed_offsets[i]] ^= 0xff;
        kind = SOCKET_STREAM_ASSET;
        ck_assert(!socket_stream_preface_decode(preface, sizeof(preface), &kind));
        ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
    }

    socket_stream_preface_encode(preface, SOCKET_STREAM_ASSET);
    ck_assert(socket_stream_preface_decode(preface, sizeof(preface), &kind));
    ck_assert_int_eq(kind, SOCKET_STREAM_ASSET);
}
END_TEST

START_TEST(test_metaserver_rendezvous_token_bounds) {
    static const char token[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char response[128];
    int length = snprintf(VS(response), "{\"rendezvousToken\":\"%s\"}", token);
    ck_assert_int_gt(length, 0);
    ck_assert_int_lt(length, (int)sizeof(response));

    char parsed[65];
    ck_assert(metaserver_rendezvous_token_parse(response, (size_t)length, parsed));
    ck_assert_str_eq(parsed, token);

    for (size_t truncated = 0; truncated < (size_t)length - 1; truncated++) {
        ck_assert(!metaserver_rendezvous_token_parse(response, truncated, parsed));
    }

    response[sizeof("{\"rendezvousToken\":\"") - 1] = 'A';
    ck_assert(!metaserver_rendezvous_token_parse(response, (size_t)length, parsed));
    static const char cleared[sizeof(parsed)];
    ck_assert_mem_eq(parsed, cleared, sizeof(parsed));
}
END_TEST

START_TEST(test_metaserver_rendezvous_retry_policy) {
    ck_assert_int_gt(METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS,
                     METASERVER_RENDEZVOUS_CONNECT_TIMEOUT_MS);
    ck_assert_int_le(METASERVER_RENDEZVOUS_UPGRADE_TIMEOUT_MS, 30000L);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 0, 0), 3750);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 0, 2500), 6250);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(1, 0, 0), 7500);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(UINT32_MAX, 0, UINT32_MAX),
                      METASERVER_RENDEZVOUS_RETRY_MAX_MS);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, 60, 0), 60000);
    ck_assert_uint_eq(metaserver_rendezvous_retry_delay_ms(0, UINT32_MAX, 0),
                      METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS * 1000U);
    ck_assert_uint_eq(metaserver_rendezvous_retry_failures(4, METASERVER_RENDEZVOUS_STABLE_MS - 1U),
                      4);
    ck_assert_uint_eq(metaserver_rendezvous_retry_failures(4, METASERVER_RENDEZVOUS_STABLE_MS), 0);

    metaserver_rendezvous_headers_t headers = {0};
    char status[] = "HTTP/1.1 429 Too Many Requests\r\n";
    char retry[] = "Retry-After: 120\r\n";
    char protocol[] = "Sec-WebSocket-Protocol: " RENDEZVOUS_INVITE_SUBPROTOCOL "\r\n";
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, false));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert_uint_eq(metaserver_rendezvous_header(status, 1, strlen(status), &headers),
                      strlen(status));
    ck_assert_uint_eq(metaserver_rendezvous_header(retry, 1, strlen(retry), &headers),
                      strlen(retry));
    ck_assert_uint_eq(metaserver_rendezvous_header(protocol, 1, strlen(protocol), &headers),
                      strlen(protocol));
    ck_assert(headers.has_retry_after);
    ck_assert_uint_eq(headers.retry_after_seconds, 120);
    ck_assert(rendezvous_websocket_protocol_valid(&headers.protocol));
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    ck_assert_uint_eq(metaserver_rendezvous_header(protocol, 1, strlen(protocol), &headers),
                      strlen(protocol));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    char wrong_protocol[] = "Sec-WebSocket-Protocol: atrinik-rendezvous-v0\r\n";
    metaserver_rendezvous_header(status, 1, strlen(status), &headers);
    metaserver_rendezvous_header(wrong_protocol, 1, strlen(wrong_protocol), &headers);
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, false));

    char second_status[] = "HTTP/1.1 503 Service Unavailable\r\n";
    char invalid_retry[] = "Retry-After: tomorrow\r\n";
    char bounded_retry[] = "Retry-After: 999999999999999999999\r\n";
    metaserver_rendezvous_header(second_status, 1, strlen(second_status), &headers);
    ck_assert(!headers.has_retry_after);
    ck_assert(metaserver_rendezvous_protocol_allows(&headers, false));
    ck_assert(!metaserver_rendezvous_protocol_allows(&headers, true));
    metaserver_rendezvous_header(invalid_retry, 1, strlen(invalid_retry), &headers);
    ck_assert(!headers.has_retry_after);
    metaserver_rendezvous_header(bounded_retry, 1, strlen(bounded_retry), &headers);
    ck_assert(headers.has_retry_after);
    ck_assert_uint_eq(headers.retry_after_seconds, METASERVER_RENDEZVOUS_RETRY_AFTER_MAX_SECONDS);
}
END_TEST

START_TEST(test_metaserver_rendezvous_ticket_isolation) {
    static const char ticket_a[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char ticket_b[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    metaserver_rendezvous_auth_job_t jobs[2] = {0};
    metaserver_rendezvous_auth_job_t *job_a = NULL, *job_b = NULL, *claimed = NULL;
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_a, 100, &job_a),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_b, 200, &job_b),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_ptr_nonnull(job_a);
    ck_assert_ptr_nonnull(job_b);
    job_a->state = RENDEZVOUS_SERVER_AUTH_WAIT_PROOF;
    job_b->state = RENDEZVOUS_SERVER_AUTH_WAIT_PROOF;

    ck_assert_int_eq(
        metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), ticket_a, 300, &claimed),
        METASERVER_RENDEZVOUS_AUTH_CLAIM_DUPLICATE);
    ck_assert_ptr_null(claimed);
    ck_assert_ptr_eq(metaserver_rendezvous_auth_find(jobs,
                                                     arraysize(jobs),
                                                     ticket_b,
                                                     RENDEZVOUS_SERVER_AUTH_WAIT_PROOF),
                     job_b);
    ck_assert_ptr_null(metaserver_rendezvous_auth_find(jobs,
                                                       arraysize(jobs),
                                                       "malformed",
                                                       RENDEZVOUS_SERVER_AUTH_WAIT_PROOF));
    ck_assert_int_eq(
        metaserver_rendezvous_auth_claim(jobs, arraysize(jobs), "malformed", 300, &claimed),
        METASERVER_RENDEZVOUS_AUTH_CLAIM_INVALID);
    ck_assert(job_a->active);
    ck_assert(job_b->active);

    metaserver_rendezvous_auth_expire(jobs, arraysize(jobs), 150);
    ck_assert(!job_a->active);
    ck_assert(job_b->active);
    ck_assert_str_eq(job_b->ticket, ticket_b);

    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, 1, ticket_a, 300, &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(jobs, 1, ticket_b, 300, &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL);
    ck_assert(job_b->active);
    ck_assert_str_eq(job_b->ticket, ticket_b);
    for (size_t i = 0; i < arraysize(jobs); i++) {
        metaserver_rendezvous_auth_clear(&jobs[i]);
    }

    metaserver_rendezvous_auth_job_t full_jobs[METASERVER_RENDEZVOUS_AUTH_JOBS_MAX] = {0};
    char generated_ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    for (size_t i = 0; i < arraysize(full_jobs); i++) {
        ck_assert_int_eq(snprintf(VS(generated_ticket), "%064zx", i), RENDEZVOUS_TICKET_HEX_SIZE);
        ck_assert_int_eq(metaserver_rendezvous_auth_claim(full_jobs,
                                                          arraysize(full_jobs),
                                                          generated_ticket,
                                                          1000,
                                                          &claimed),
                         METASERVER_RENDEZVOUS_AUTH_CLAIM_OK);
    }
    ck_assert_int_eq(snprintf(VS(generated_ticket), "%064x", METASERVER_RENDEZVOUS_AUTH_JOBS_MAX),
                     RENDEZVOUS_TICKET_HEX_SIZE);
    ck_assert_int_eq(metaserver_rendezvous_auth_claim(full_jobs,
                                                      arraysize(full_jobs),
                                                      generated_ticket,
                                                      1000,
                                                      &claimed),
                     METASERVER_RENDEZVOUS_AUTH_CLAIM_FULL);
    ck_assert_ptr_null(claimed);
    for (size_t i = 0; i < arraysize(full_jobs); i++) {
        ck_assert(full_jobs[i].active);
        metaserver_rendezvous_auth_clear(&full_jobs[i]);
    }
}
END_TEST

START_TEST(test_metaserver_generation_cancellation) {
    const rendezvous_server_auth_state_t stages[] = {
        RENDEZVOUS_SERVER_AUTH_NEW,
        RENDEZVOUS_SERVER_AUTH_WAIT_PROOF,
        RENDEZVOUS_SERVER_AUTH_AUTHORIZED,
    };
    for (size_t i = 0; i < arraysize(stages); i++) {
        ck_assert_int_ne(stages[i], RENDEZVOUS_SERVER_AUTH_CONSUMED);
        ck_assert(metaserver_rendezvous_generation_allows(7, 7, false));
        ck_assert(!metaserver_rendezvous_generation_allows(8, 7, false));
        ck_assert(!metaserver_rendezvous_generation_allows(7, 7, true));
    }
}
END_TEST

START_TEST(test_metaserver_registration_key_retention) {
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 401, true),
                     METASERVER_REGISTRATION_KEY_DELETE);
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 500, true),
                     METASERVER_REGISTRATION_KEY_RETRY_ESTABLISHED);
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_ERROR, 0, true),
                     METASERVER_REGISTRATION_KEY_RETRY_ESTABLISHED);
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 400, true),
                     METASERVER_REGISTRATION_KEY_KEEP);
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 403, true),
                     METASERVER_REGISTRATION_KEY_KEEP);
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 429, true),
                     METASERVER_REGISTRATION_KEY_KEEP);

    unsigned char retained_key[SHA512_DIGEST_LENGTH];
    memset(retained_key, 0xa5, sizeof(retained_key));
    unsigned char original_key[sizeof(retained_key)];
    memcpy(original_key, retained_key, sizeof(original_key));
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 409, false),
                     METASERVER_REGISTRATION_KEY_RETRY_REGISTRATION);
    ck_assert_mem_eq(retained_key, original_key, sizeof(retained_key));
    ck_assert_int_eq(metaserver_registration_key_action(CURL_STATE_OK, 401, false),
                     METASERVER_REGISTRATION_KEY_KEEP);
    OPENSSL_cleanse(retained_key, sizeof(retained_key));
    OPENSSL_cleanse(original_key, sizeof(original_key));
}
END_TEST

START_TEST(test_metaserver_raw_endpoint_not_published) {
    char host[65] = "sentinel";
    uint16_t port = 0;
    ck_assert(!metaserver_public_endpoint_from_config("1.1.1.1", 1730, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1730);

    ck_assert(!metaserver_public_endpoint_from_config("", 1731, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1731);
    ck_assert(!metaserver_public_endpoint_from_config("192.168.1.10", 1732, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1732);
    ck_assert(
        !metaserver_public_endpoint_from_config("server.example.invalid", 1733, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1733);
#ifdef HAVE_IPV6
    ck_assert(
        !metaserver_public_endpoint_from_config("2606:4700:4700::1111", 1734, VS(host), &port));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 1734);
#endif
}
END_TEST

START_TEST(test_path_secret_reader) {
    char path[] = "/tmp/atrinik-secret-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    static const char value[] = "correct horse\r\n";
    ck_assert_int_eq(write(fd, value, sizeof(value) - 1), (ssize_t)(sizeof(value) - 1));
#ifndef WIN32
    ck_assert_int_eq(fchmod(fd, 0644), 0);
#endif
    ck_assert_int_eq(close(fd), 0);

    char secret[32];
    bool permissive = false;
    ck_assert_int_eq(path_read_secret(path, VS(secret), &permissive), PATH_SECRET_OK);
    ck_assert_str_eq(secret, "correct horse");
#ifndef WIN32
    ck_assert(permissive);
#endif

    fd = open(path, O_WRONLY | O_TRUNC);
    ck_assert_int_ne(fd, -1);
    static const char too_long[] = "a secret that cannot fit";
    ck_assert_int_eq(write(fd, too_long, sizeof(too_long) - 1), (ssize_t)(sizeof(too_long) - 1));
    ck_assert_int_eq(close(fd), 0);
    char small[8];
    memset(small, 0xaa, sizeof(small));
    ck_assert_int_eq(path_read_secret(path, VS(small), NULL), PATH_SECRET_TOO_LONG);
    static const char cleared[sizeof(small)];
    ck_assert_mem_eq(small, cleared, sizeof(small));

    fd = open(path, O_WRONLY | O_TRUNC);
    ck_assert_int_ne(fd, -1);
    static const char trailing[] = "valid secret\nsecond secret\n";
    ck_assert_int_eq(write(fd, trailing, sizeof(trailing) - 1), (ssize_t)(sizeof(trailing) - 1));
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(path_read_secret(path, VS(secret), NULL), PATH_SECRET_TRAILING_DATA);
    static const char cleared_secret[sizeof(secret)];
    ck_assert_mem_eq(secret, cleared_secret, sizeof(secret));

#if !defined(WIN32) && defined(O_NOFOLLOW)
    char link_path[sizeof(path) + 8];
    snprintf(VS(link_path), "%s.link", path);
    ck_assert_int_eq(symlink(path, link_path), 0);
    ck_assert_int_eq(path_read_secret(link_path, VS(secret), NULL), PATH_SECRET_UNSAFE_LINK);
    ck_assert_int_eq(unlink(link_path), 0);
#endif

    ck_assert_int_eq(path_read_secret("/tmp", VS(secret), NULL), PATH_SECRET_NOT_REGULAR);
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_path_safe_relative) {
    ck_assert(path_is_safe_relative("client-maps/world.png"));
    ck_assert(path_is_safe_relative("settings/file"));
    ck_assert(!path_is_safe_relative("../outside"));
    ck_assert(!path_is_safe_relative("inside/../outside"));
    ck_assert(!path_is_safe_relative("/absolute"));
    ck_assert(!path_is_safe_relative("C:\\absolute"));
    ck_assert(!path_is_safe_relative("double//component"));
}
END_TEST

START_TEST(test_path_write_atomic_replaces_complete_file) {
    char path[] = "/tmp/atrinik-path-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(unlink(path), 0);

    static const char first[] = "first";
    static const char second[] = "replacement";
    ck_assert(path_write_atomic(path, first, sizeof(first) - 1, 0600));
    ck_assert(path_write_atomic(path, second, sizeof(second) - 1, 0600));

    FILE *fp = fopen(path, "rb");
    ck_assert_ptr_nonnull(fp);
    char contents[sizeof(second)] = {0};
    ck_assert_uint_eq(fread(contents, 1, sizeof(second) - 1, fp), sizeof(second) - 1);
    ck_assert_int_eq(fclose(fp), 0);
    ck_assert_str_eq(contents, second);
#ifndef WIN32
    struct stat sb;
    ck_assert_int_eq(stat(path, &sb), 0);
    ck_assert_int_eq(sb.st_mode & 0777, 0600);
#endif
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_path_secret_create_atomic_no_replace) {
    char path[] = "/tmp/atrinik-secret-create-test.XXXXXX";
    int fd = mkstemp(path);
    ck_assert_int_ne(fd, -1);
    ck_assert_int_eq(close(fd), 0);
    ck_assert_int_eq(unlink(path), 0);

    static const char first[] = "first secret";
    static const char second[] = "replacement secret";
    ck_assert_int_eq(path_secret_create_atomic(path, first, sizeof(first) - 1U),
                     PATH_SECRET_CREATE_OK);
    ck_assert_int_eq(path_secret_create_atomic(path, second, sizeof(second) - 1U),
                     PATH_SECRET_CREATE_EXISTS);

    char secret[32];
    bool permissive = true;
    ck_assert_int_eq(path_read_secret(path, VS(secret), &permissive), PATH_SECRET_OK);
    ck_assert_str_eq(secret, first);
    ck_assert(!permissive);
#ifndef WIN32
    struct stat metadata;
    ck_assert_int_eq(lstat(path, &metadata), 0);
    ck_assert(S_ISREG(metadata.st_mode));
    ck_assert_uint_eq(metadata.st_uid, geteuid());
    ck_assert_uint_eq(metadata.st_mode & 0777, 0600);
    ck_assert_uint_eq(metadata.st_nlink, 1);
#endif
    ck_assert_int_eq(unlink(path), 0);
}
END_TEST

START_TEST(test_socket_rendezvous_messages) {
    static const char server_id[] =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    static const char ticket[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char other_ticket[] =
        "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    rendezvous_invite_t invite = {
        .server_id = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        .invite_id = "00112233445566778899aabbccddeeff",
        .expiry = UINT64_MAX,
    };
    memset(invite.secret, 0x42, sizeof(invite.secret));
    socket_rendezvous_attempt_t *attempt =
        socket_rendezvous_attempt_create(server_id, ticket, &invite, UINT64_MAX);
    ck_assert_ptr_nonnull(attempt);

    char message[RENDEZVOUS_FRAME_MAX + 1U], proof_frame[RENDEZVOUS_FRAME_MAX + 1U];
    ck_assert(socket_rendezvous_attempt_auth_init(attempt, VS(message)));
    char parsed_ticket[65], invite_id[33];
    ck_assert(rendezvous_auth_init_parse(message, parsed_ticket, invite_id));
    ck_assert_str_eq(parsed_ticket, ticket);
    ck_assert_str_eq(invite_id, invite.invite_id);

    unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE];
    memset(challenge, 0x24, sizeof(challenge));
    ck_assert(rendezvous_auth_challenge_render(VS(message), ticket, challenge));
    rendezvous_server_auth_state_t server_auth = RENDEZVOUS_SERVER_AUTH_NEW;
    ck_assert(rendezvous_server_auth_challenge_sent(&server_auth));
    ck_assert_int_eq(
        socket_rendezvous_attempt_challenge(attempt, message, strlen(message), VS(proof_frame)),
        SOCKET_RENDEZVOUS_FRAME_CHALLENGE);
    unsigned char proof[RENDEZVOUS_PROOF_SIZE];
    ck_assert(rendezvous_auth_proof_parse(proof_frame, ticket, proof));
    ck_assert(rendezvous_auth_result_render(VS(message), ticket, true));
    ck_assert(rendezvous_server_auth_result_sent(&server_auth, true));
    ck_assert_int_eq(socket_rendezvous_attempt_auth_result(attempt, message, strlen(message)),
                     SOCKET_RENDEZVOUS_FRAME_AUTHORIZED);
    ck_assert(socket_rendezvous_attempt_client_candidate(attempt, "192.0.2.10", 1730, VS(message)));

    char host[65];
    uint16_t port;
    rendezvous_server_auth_state_t preauth = RENDEZVOUS_SERVER_AUTH_NEW;
    ck_assert(!socket_rendezvous_client_candidate_parse(message,
                                                        ticket,
                                                        true,
                                                        preauth,
                                                        VS(host),
                                                        &port,
                                                        parsed_ticket));
    ck_assert(socket_rendezvous_client_candidate_parse(message,
                                                       ticket,
                                                       true,
                                                       server_auth,
                                                       VS(host),
                                                       &port,
                                                       parsed_ticket));
    ck_assert_str_eq(host, "192.0.2.10");
    ck_assert_uint_eq(port, 1730);
    ck_assert_str_eq(parsed_ticket, ticket);
    ck_assert(!socket_rendezvous_client_candidate_parse(message,
                                                        other_ticket,
                                                        true,
                                                        server_auth,
                                                        VS(host),
                                                        &port,
                                                        parsed_ticket));
    ck_assert_str_eq(host, "");
    ck_assert_uint_eq(port, 0);
    ck_assert_str_eq(parsed_ticket, "");

    socket_direct_candidate_t candidate = {
        .host = "2001:db8::1",
        .port = 1730,
        .kind = SOCKET_CANDIDATE_IPV6,
    };
    ck_assert(!socket_rendezvous_server_candidate_render(VS(proof_frame),
                                                         &candidate,
                                                         ticket,
                                                         true,
                                                         preauth));
    ck_assert(socket_rendezvous_server_candidate_render(VS(message),
                                                        &candidate,
                                                        ticket,
                                                        true,
                                                        server_auth));
    memset(&candidate, 0, sizeof(candidate));
    ck_assert_int_eq(
        socket_rendezvous_attempt_server_frame(attempt, message, strlen(message), &candidate),
        SOCKET_RENDEZVOUS_FRAME_CANDIDATE);
    ck_assert_str_eq(candidate.host, "2001:db8::1");
    ck_assert_uint_eq(candidate.port, 1730);
    ck_assert_int_eq(candidate.kind, SOCKET_CANDIDATE_IPV6);

    ck_assert(socket_rendezvous_complete_render(VS(message), ticket));
    ck_assert(socket_rendezvous_complete_parse(message, ticket));
    ck_assert_int_eq(
        socket_rendezvous_attempt_server_frame(attempt, message, strlen(message), &candidate),
        SOCKET_RENDEZVOUS_FRAME_COMPLETE);
    ck_assert(!socket_rendezvous_client_candidate_parse(
        "{\"type\":\"client_candidate\",\"host\":\"example.com\","
        "\"port\":1730,\"ticket\":\"bad\"}",
        NULL,
        false,
        RENDEZVOUS_SERVER_AUTH_NEW,
        VS(host),
        &port,
        parsed_ticket));
    socket_rendezvous_attempt_destroy(attempt);
    rendezvous_invite_cleanse(&invite);
    OPENSSL_cleanse(challenge, sizeof(challenge));
    OPENSSL_cleanse(proof, sizeof(proof));
    OPENSSL_cleanse(proof_frame, sizeof(proof_frame));
}
END_TEST

START_TEST(test_socket_asset_request_rejects_malformed) {
    packet_struct *packet = packet_new(0, 0, 0);
    uint8_t digest[ASSET_DIGEST_SIZE] = {0};
    socket_asset_request_append(packet, "data/listing.txt", 0, digest, 0);

    socket_asset_request_t request;
    memset(&request, 0xa5, sizeof(request));
    const socket_asset_request_t unchanged_request = request;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        ck_assert(!socket_asset_request_parse(packet->data, truncated, 0, &request));
        ck_assert_mem_eq(&request, &unchanged_request, sizeof(request));
    }
    packet_writer_write_uint8(packet, 0);
    ck_assert(!socket_asset_request_parse(packet->data, packet->len, 0, &request));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    digest[0] = 3;
    socket_asset_request_append(packet, "data/listing.txt", 2, digest, ASSET_REQUEST_METADATA);
    ck_assert(!socket_asset_request_parse(packet->data, packet->len, 0, &request));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    memset(digest, 0, sizeof(digest));
    socket_asset_request_append(packet, "data/listing.txt", 0, digest, ASSET_REQUEST_METADATA);
    ck_assert(socket_asset_request_parse(packet->data, packet->len, 0, &request));
    ck_assert_uint_eq(request.flags, ASSET_REQUEST_METADATA);
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_asset_response_round_trip) {
    static const uint8_t chunk[] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t digest[ASSET_DIGEST_SIZE];
    memset(digest, 0x12, sizeof(digest));
    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_response_append_ok(packet, sizeof(chunk), digest);

    socket_asset_response_t response;
    memset(&response, 0xa5, sizeof(response));
    const socket_asset_response_t unchanged_response = response;
    for (size_t truncated = 0; truncated < packet->len; truncated++) {
        ck_assert(!socket_asset_response_parse(packet->data, truncated, 0, &response));
        ck_assert_mem_eq(&response, &unchanged_response, sizeof(response));
    }
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_OK);
    ck_assert_uint_eq(response.total_size, sizeof(chunk));
    ck_assert_mem_eq(response.digest, digest, sizeof(digest));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_MODIFIED, sizeof(chunk), digest);
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_NOT_MODIFIED);
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_metadata(packet, sizeof(chunk), digest);
    ck_assert(socket_asset_response_parse(packet->data, packet->len, 0, &response));
    ck_assert_uint_eq(response.status, ASSET_STATUS_METADATA);
    ck_assert_uint_eq(response.total_size, sizeof(chunk));
    ck_assert_mem_eq(response.digest, digest, sizeof(digest));
    packet_free(packet);
}
END_TEST

START_TEST(test_socket_asset_response_rejects_malformed) {
    uint8_t digest[ASSET_DIGEST_SIZE] = {0};
    socket_asset_response_t response;
    uint8_t unknown[] = {0xff, 'x', '\0'};
    ck_assert(!socket_asset_response_parse(unknown, sizeof(unknown), 0, &response));

    packet_struct *packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_MODIFIED, 0, digest);
    packet_writer_write_uint8(packet, 0);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_ok(packet, ASSET_MAX_SIZE + 1U, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_FOUND, 1, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);

    packet = packet_new(0, 0, 0);
    digest[0] = 1;
    socket_asset_response_append_status(packet, ASSET_STATUS_NOT_FOUND, 0, digest);
    ck_assert(!socket_asset_response_parse(packet->data, packet->len, 0, &response));
    packet_free(packet);
}
END_TEST

static Suite *suite(void) {
    Suite *s = suite_create("socket_asset");
    TCase *tc_core = tcase_create("Core");

    tcase_add_unchecked_fixture(tc_core, check_setup, check_teardown);
    tcase_add_checked_fixture(tc_core, check_test_setup, check_test_teardown);
    suite_add_tcase(s, tc_core);
    tcase_add_test(tc_core, test_socket_asset_request_round_trip);
    tcase_add_test(tc_core, test_socket_stream_preface_round_trip_and_malformed);
    tcase_add_test(tc_core, test_socket_asset_request_rejects_malformed);
    tcase_add_test(tc_core, test_socket_asset_response_round_trip);
    tcase_add_test(tc_core, test_socket_asset_response_rejects_malformed);
    tcase_add_test(tc_core, test_socket_rendezvous_messages);
    tcase_add_test(tc_core, test_metaserver_rendezvous_token_bounds);
    tcase_add_test(tc_core, test_metaserver_rendezvous_retry_policy);
    tcase_add_test(tc_core, test_metaserver_rendezvous_ticket_isolation);
    tcase_add_test(tc_core, test_metaserver_generation_cancellation);
    tcase_add_test(tc_core, test_metaserver_registration_key_retention);
    tcase_add_test(tc_core, test_metaserver_raw_endpoint_not_published);
    tcase_add_test(tc_core, test_path_write_atomic_replaces_complete_file);
    tcase_add_test(tc_core, test_path_secret_create_atomic_no_replace);
    tcase_add_test(tc_core, test_path_secret_reader);
    tcase_add_test(tc_core, test_path_safe_relative);

    return s;
}

void check_server_socket_asset(void) {
    check_run_suite(suite(), __FILE__);
}
