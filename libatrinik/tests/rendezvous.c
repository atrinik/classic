#include <toolkit/rendezvous.h>
#include <toolkit/socket.h>
#include <toolkit/string.h>

#include <openssl/crypto.h>

typedef struct fixture {
    char subprotocol[64];
    char server_id[65];
    char ticket[65];
    char invite_id[33];
    char secret[65];
    char challenge[65];
    char capability[RENDEZVOUS_INVITE_TEXT_SIZE];
    char transcript_hex[RENDEZVOUS_TRANSCRIPT_SIZE * 2U + 1U];
    char proof[65];
    char auth_init[RENDEZVOUS_FRAME_MAX + 1U];
    char auth_challenge[RENDEZVOUS_FRAME_MAX + 1U];
    char auth_proof[RENDEZVOUS_FRAME_MAX + 1U];
    char auth_result_authorized[RENDEZVOUS_FRAME_MAX + 1U];
    char auth_result_denied[RENDEZVOUS_FRAME_MAX + 1U];
    uint64_t expiry;
} fixture_t;

typedef struct negative_fixture {
    char wrong_ticket[65];
    char wrong_ticket_server_candidate[RENDEZVOUS_FRAME_MAX + 1U];
    char oversized_port_server_candidate[RENDEZVOUS_FRAME_MAX + 1U];
    char leading_zero_port_server_candidate[RENDEZVOUS_FRAME_MAX + 1U];
    char truncated_auth_challenge[RENDEZVOUS_FRAME_MAX + 1U];
    char wrong_ticket_auth_result[RENDEZVOUS_FRAME_MAX + 1U];
} negative_fixture_t;

static void require(bool condition) {
    if (!condition) {
        abort();
    }
}

static char *fixture_read(const char *path) {
    FILE *fp = fopen(path, "rb");
    require(fp != NULL);
    require(fseek(fp, 0, SEEK_END) == 0);
    long length = ftell(fp);
    require(length > 0 && length < 16384);
    require(fseek(fp, 0, SEEK_SET) == 0);
    char *json = calloc((size_t)length + 1U, 1);
    require(json != NULL);
    require(fread(json, 1, (size_t)length, fp) == (size_t)length);
    require(fclose(fp) == 0);
    return json;
}

static bool fixture_string(const char *json, const char *key, char *value, size_t value_size) {
    char marker[96];
    if (snprintf(VS(marker), "\"%s\": \"", key) >= (int)sizeof(marker)) {
        return false;
    }
    const char *cursor = strstr(json, marker);
    if (cursor == NULL || strstr(cursor + strlen(marker), marker) != NULL) {
        return false;
    }
    cursor += strlen(marker);
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char cp = (unsigned char)*cursor++;
        if (cp == '\\') {
            cp = (unsigned char)*cursor++;
            if (cp != '\\' && cp != '"') {
                return false;
            }
        } else if (cp < 0x20) {
            return false;
        }
        if (used + 1U >= value_size) {
            return false;
        }
        value[used++] = (char)cp;
    }
    if (*cursor != '"') {
        return false;
    }
    value[used] = '\0';
    return true;
}

static bool fixture_uint64(const char *json, const char *key, uint64_t *value) {
    char marker[96];
    if (snprintf(VS(marker), "\"%s\": ", key) >= (int)sizeof(marker)) {
        return false;
    }
    const char *cursor = strstr(json, marker);
    if (cursor == NULL || strstr(cursor + strlen(marker), marker) != NULL) {
        return false;
    }
    cursor += strlen(marker);
    char digits[21];
    size_t used = 0;
    while (isdigit((unsigned char)*cursor) && used + 1U < sizeof(digits)) {
        digits[used++] = *cursor++;
    }
    digits[used] = '\0';
    return (*cursor == ',' || *cursor == '\n') &&
           string_parse_uint64(digits, 10, 1, UINT64_MAX, value);
}

static fixture_t fixture_load(const char *path) {
    char *json = fixture_read(path);
    fixture_t fixture = {0};
    uint64_t version;
    require(fixture_uint64(json, "version", &version) && version == 1);
    require(fixture_uint64(json, "expiry", &fixture.expiry));
    require(fixture_string(json, "subprotocol", VS(fixture.subprotocol)));
    require(fixture_string(json, "server_id", VS(fixture.server_id)));
    require(fixture_string(json, "ticket", VS(fixture.ticket)));
    require(fixture_string(json, "invite_id", VS(fixture.invite_id)));
    require(fixture_string(json, "secret", VS(fixture.secret)));
    require(fixture_string(json, "challenge", VS(fixture.challenge)));
    require(fixture_string(json, "capability", VS(fixture.capability)));
    require(fixture_string(json, "transcript_hex", VS(fixture.transcript_hex)));
    require(fixture_string(json, "proof", VS(fixture.proof)));
    require(fixture_string(json, "auth_init", VS(fixture.auth_init)));
    require(fixture_string(json, "auth_challenge", VS(fixture.auth_challenge)));
    require(fixture_string(json, "auth_proof", VS(fixture.auth_proof)));
    require(fixture_string(json, "auth_result_authorized", VS(fixture.auth_result_authorized)));
    require(fixture_string(json, "auth_result_denied", VS(fixture.auth_result_denied)));
    free(json);
    return fixture;
}

static negative_fixture_t negative_fixture_load(const char *path) {
    char *json = fixture_read(path);
    negative_fixture_t fixture = {0};
    uint64_t version;
    require(fixture_uint64(json, "version", &version) && version == 1);
    require(fixture_string(json, "wrong_ticket", VS(fixture.wrong_ticket)));
    require(fixture_string(json,
                           "wrong_ticket_server_candidate",
                           VS(fixture.wrong_ticket_server_candidate)));
    require(fixture_string(json,
                           "oversized_port_server_candidate",
                           VS(fixture.oversized_port_server_candidate)));
    require(fixture_string(json,
                           "leading_zero_port_server_candidate",
                           VS(fixture.leading_zero_port_server_candidate)));
    require(fixture_string(json, "truncated_auth_challenge", VS(fixture.truncated_auth_challenge)));
    require(fixture_string(json, "wrong_ticket_auth_result", VS(fixture.wrong_ticket_auth_result)));
    free(json);
    return fixture;
}

int main(int argc, char **argv) {
    require(argc == 3);
    fixture_t vector = fixture_load(argv[1]);
    negative_fixture_t negative = negative_fixture_load(argv[2]);
    require(strcmp(vector.subprotocol, RENDEZVOUS_INVITE_SUBPROTOCOL) == 0);

    rendezvous_invite_t invite;
    require(rendezvous_invite_parse(vector.capability, &invite));
    require(strcmp(invite.server_id, vector.server_id) == 0);
    require(strcmp(invite.invite_id, vector.invite_id) == 0);
    require(invite.expiry == vector.expiry);
    unsigned char secret[RENDEZVOUS_SECRET_SIZE];
    require(string_decode_hex_fixed(vector.secret, 64, true, VS(secret)));
    require(CRYPTO_memcmp(secret, invite.secret, sizeof(secret)) == 0);
    char rendered[RENDEZVOUS_INVITE_TEXT_SIZE];
    require(rendezvous_invite_render(&invite, VS(rendered)));
    require(strcmp(rendered, vector.capability) == 0);
    require(rendezvous_invite_valid_at(&invite, invite.server_id, vector.expiry - 1U));
    require(!rendezvous_invite_valid_at(&invite, invite.server_id, vector.expiry));
    require(!rendezvous_invite_valid_at(&invite, invite.server_id, vector.expiry + 1U));

    unsigned char challenge[32], proof[32], expected_proof[32];
    unsigned char transcript[RENDEZVOUS_TRANSCRIPT_SIZE],
        expected_transcript[RENDEZVOUS_TRANSCRIPT_SIZE];
    require(string_decode_hex_fixed(vector.challenge, 64, true, VS(challenge)));
    require(string_decode_hex_fixed(vector.proof, 64, true, VS(expected_proof)));
    require(string_decode_hex_fixed(vector.transcript_hex,
                                    RENDEZVOUS_TRANSCRIPT_SIZE * 2U,
                                    true,
                                    VS(expected_transcript)));
    require(rendezvous_invite_transcript(&invite, vector.ticket, challenge, transcript));
    require(CRYPTO_memcmp(transcript, expected_transcript, sizeof(transcript)) == 0);
    require(rendezvous_invite_proof(&invite, vector.ticket, challenge, proof));
    require(CRYPTO_memcmp(proof, expected_proof, sizeof(proof)) == 0);
    require(rendezvous_invite_proof_verify(&invite, vector.ticket, challenge, proof));
    proof[0] ^= 1;
    require(!rendezvous_invite_proof_verify(&invite, vector.ticket, challenge, proof));

    char frame[RENDEZVOUS_FRAME_MAX + 1U], parsed_ticket[65], parsed_id[33];
    require(rendezvous_auth_init_render(VS(frame), vector.ticket, invite.invite_id));
    require(strcmp(frame, vector.auth_init) == 0);
    require(rendezvous_auth_init_parse(frame, parsed_ticket, parsed_id));
    require(strcmp(parsed_ticket, vector.ticket) == 0);
    require(strcmp(parsed_id, invite.invite_id) == 0);
    snprintfcat(frame, sizeof(frame), " ");
    require(!rendezvous_auth_init_parse(frame, parsed_ticket, parsed_id));

    require(rendezvous_auth_challenge_render(VS(frame), vector.ticket, challenge));
    require(strcmp(frame, vector.auth_challenge) == 0);
    unsigned char parsed_bytes[32];
    require(rendezvous_auth_challenge_parse(frame, vector.ticket, parsed_bytes));
    require(CRYPTO_memcmp(parsed_bytes, challenge, sizeof(challenge)) == 0);
    require(rendezvous_auth_proof_render(VS(frame), vector.ticket, expected_proof));
    require(strcmp(frame, vector.auth_proof) == 0);
    require(rendezvous_auth_proof_parse(frame, vector.ticket, parsed_bytes));
    require(CRYPTO_memcmp(parsed_bytes, expected_proof, sizeof(expected_proof)) == 0);
    bool authorized = false;
    require(rendezvous_auth_result_render(VS(frame), vector.ticket, true));
    require(strcmp(frame, vector.auth_result_authorized) == 0);
    require(rendezvous_auth_result_parse(frame, vector.ticket, &authorized) && authorized);
    require(rendezvous_auth_result_render(VS(frame), vector.ticket, false));
    require(strcmp(frame, vector.auth_result_denied) == 0);
    require(rendezvous_auth_result_parse(frame, vector.ticket, &authorized) && !authorized);
    memset(frame, 'x', sizeof(frame));
    require(!rendezvous_auth_init_render(VS(frame), "invalid", invite.invite_id));
    require(frame[0] == '\0');
    memset(frame, 'x', sizeof(frame));
    require(!rendezvous_auth_result_render(VS(frame), "invalid", true));
    require(frame[0] == '\0');
    authorized = true;
    require(!rendezvous_auth_result_parse("invalid", vector.ticket, &authorized));
    require(!authorized);

    rendezvous_websocket_protocol_t protocol = {0};
    char status[] = "HTTP/1.1 101 Switching Protocols\r\n";
    char echo[] = "Sec-WebSocket-Protocol: " RENDEZVOUS_INVITE_SUBPROTOCOL "\r\n";
    require(rendezvous_websocket_protocol_header(status, 1, strlen(status), &protocol) ==
            strlen(status));
    require(rendezvous_websocket_protocol_header(echo, 1, strlen(echo), &protocol) == strlen(echo));
    require(rendezvous_websocket_protocol_valid(&protocol));
    require(rendezvous_websocket_protocol_header(echo, 1, strlen(echo), &protocol) == strlen(echo));
    require(!rendezvous_websocket_protocol_valid(&protocol));

    socket_rendezvous_attempt_t *attempt =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, UINT64_MAX);
    require(attempt != NULL);
    require(!socket_rendezvous_attempt_directory_probe_allowed(attempt));
    require(!socket_rendezvous_attempt_peer_traffic_allowed(attempt));
    require(!socket_rendezvous_attempt_protocol_valid(attempt));
    require(socket_rendezvous_attempt_header(status, 1, strlen(status), attempt) == strlen(status));
    require(socket_rendezvous_attempt_header(echo, 1, strlen(echo), attempt) == strlen(echo));
    require(socket_rendezvous_attempt_protocol_valid(attempt));
    require(socket_rendezvous_attempt_auth_init(attempt, VS(frame)));
    require(strcmp(frame, vector.auth_init) == 0);
    char proof_frame[RENDEZVOUS_FRAME_MAX + 1U];
    require(socket_rendezvous_attempt_challenge(attempt,
                                                vector.auth_challenge,
                                                strlen(vector.auth_challenge),
                                                VS(proof_frame)) ==
            SOCKET_RENDEZVOUS_FRAME_CHALLENGE);
    require(strcmp(proof_frame, vector.auth_proof) == 0);
    require(socket_rendezvous_attempt_auth_result(attempt,
                                                  vector.auth_result_authorized,
                                                  strlen(vector.auth_result_authorized)) ==
            SOCKET_RENDEZVOUS_FRAME_AUTHORIZED);
    require(!socket_rendezvous_attempt_peer_traffic_allowed(attempt));
    require(socket_rendezvous_attempt_client_candidate(attempt, "192.0.2.1", 1730, VS(frame)));
    require(socket_rendezvous_attempt_peer_traffic_allowed(attempt));
    char client_candidate[RENDEZVOUS_FRAME_MAX + 1U];
    snprintf(VS(client_candidate), "%s", frame);

    rendezvous_server_auth_state_t server_state = RENDEZVOUS_SERVER_AUTH_NEW;
    require(!rendezvous_server_auth_candidate_consume(&server_state));
    require(rendezvous_server_auth_challenge_sent(&server_state));
    require(!rendezvous_server_auth_candidate_consume(&server_state));
    require(rendezvous_server_auth_result_sent(&server_state, true));
    require(rendezvous_server_auth_candidate_consume(&server_state));
    require(!rendezvous_server_auth_candidate_consume(&server_state));

    server_state = RENDEZVOUS_SERVER_AUTH_NEW;
    require(rendezvous_server_auth_challenge_sent(&server_state));
    require(rendezvous_server_auth_result_sent(&server_state, false));
    require(!rendezvous_server_auth_candidate_consume(&server_state));

    server_state = RENDEZVOUS_SERVER_AUTH_NEW;
    char candidate_host[65], candidate_ticket[65];
    uint16_t candidate_port;
    require(!socket_rendezvous_client_candidate_parse(client_candidate,
                                                      vector.ticket,
                                                      true,
                                                      server_state,
                                                      VS(candidate_host),
                                                      &candidate_port,
                                                      candidate_ticket));
    require(candidate_host[0] == '\0' && candidate_port == 0 && candidate_ticket[0] == '\0');
    require(rendezvous_server_auth_challenge_sent(&server_state));
    require(rendezvous_server_auth_result_sent(&server_state, true));
    require(!socket_rendezvous_client_candidate_parse(client_candidate,
                                                      negative.wrong_ticket,
                                                      true,
                                                      server_state,
                                                      VS(candidate_host),
                                                      &candidate_port,
                                                      candidate_ticket));
    require(socket_rendezvous_client_candidate_parse(client_candidate,
                                                     vector.ticket,
                                                     true,
                                                     server_state,
                                                     VS(candidate_host),
                                                     &candidate_port,
                                                     candidate_ticket));
    require(strcmp(candidate_host, "192.0.2.1") == 0 && candidate_port == 1730 &&
            strcmp(candidate_ticket, vector.ticket) == 0);

    socket_direct_candidate_t server_candidate = {
        .host = "198.51.100.7",
        .port = 1731,
        .kind = SOCKET_CANDIDATE_SRFLX,
    };
    memset(frame, 'x', sizeof(frame));
    require(!socket_rendezvous_server_candidate_render(VS(frame),
                                                       &server_candidate,
                                                       vector.ticket,
                                                       true,
                                                       RENDEZVOUS_SERVER_AUTH_NEW));
    require(frame[0] == '\0');
    require(socket_rendezvous_server_candidate_render(VS(frame),
                                                      &server_candidate,
                                                      vector.ticket,
                                                      true,
                                                      server_state));
    size_t server_candidate_length = strlen(frame);
    socket_direct_candidate_t parsed_candidate;
    require(
        socket_rendezvous_attempt_server_frame(attempt, frame, strlen(frame), &parsed_candidate) ==
        SOCKET_RENDEZVOUS_FRAME_CANDIDATE);
    require(strcmp(parsed_candidate.host, server_candidate.host) == 0);
    require(parsed_candidate.port == server_candidate.port);
    require(parsed_candidate.kind == server_candidate.kind);
    require(socket_rendezvous_complete_render(VS(frame), vector.ticket));
    require(
        socket_rendezvous_attempt_server_frame(attempt, frame, strlen(frame), &parsed_candidate) ==
        SOCKET_RENDEZVOUS_FRAME_COMPLETE);
    socket_rendezvous_stats_t stats;
    require(socket_rendezvous_attempt_stats(attempt, &stats));
    require(stats.client_frames == 3 && stats.server_frames == 4 && stats.server_candidates == 1);
    require(stats.signal_bytes ==
            strlen(vector.auth_init) + strlen(vector.auth_challenge) + strlen(vector.auth_proof) +
                strlen(vector.auth_result_authorized) + strlen(client_candidate) + strlen(frame) +
                server_candidate_length);
    socket_rendezvous_attempt_destroy(attempt);

    socket_rendezvous_attempt_t *denied =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, UINT64_MAX);
    require(denied != NULL && socket_rendezvous_attempt_auth_init(denied, VS(frame)));
    require(socket_rendezvous_attempt_challenge(denied,
                                                vector.auth_challenge,
                                                strlen(vector.auth_challenge),
                                                VS(proof_frame)) ==
            SOCKET_RENDEZVOUS_FRAME_CHALLENGE);
    require(socket_rendezvous_attempt_auth_result(denied,
                                                  vector.auth_result_denied,
                                                  strlen(vector.auth_result_denied)) ==
            SOCKET_RENDEZVOUS_FRAME_DENIED);
    require(!socket_rendezvous_attempt_client_candidate(denied, "192.0.2.1", 1730, VS(frame)));
    socket_rendezvous_attempt_destroy(denied);

    socket_rendezvous_attempt_t *incomplete =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, UINT64_MAX);
    require(incomplete != NULL && socket_rendezvous_attempt_auth_init(incomplete, VS(frame)));
    memset(proof_frame, 'x', sizeof(proof_frame));
    require(socket_rendezvous_attempt_challenge(incomplete,
                                                negative.truncated_auth_challenge,
                                                strlen(negative.truncated_auth_challenge),
                                                VS(proof_frame)) ==
            SOCKET_RENDEZVOUS_FRAME_INVALID);
    require(proof_frame[0] == '\0');
    socket_rendezvous_attempt_destroy(incomplete);

    socket_rendezvous_attempt_t *expired =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, 1);
    require(expired != NULL && socket_rendezvous_attempt_expired(expired, 1));
    memset(frame, 'x', sizeof(frame));
    require(!socket_rendezvous_attempt_auth_init(expired, VS(frame)) && frame[0] == '\0');
    socket_rendezvous_attempt_destroy(expired);

    socket_rendezvous_attempt_t *rate_limit =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, UINT64_MAX);
    require(rate_limit != NULL);
    char rate_status[] = "HTTP/1.1 429 Too Many Requests\r\n";
    char retry_after[] = "Retry-After: 60\r\n";
    require(socket_rendezvous_attempt_header(rate_status, 1, strlen(rate_status), rate_limit) ==
            strlen(rate_status));
    require(socket_rendezvous_attempt_header(retry_after, 1, strlen(retry_after), rate_limit) ==
            strlen(retry_after));
    require(socket_rendezvous_attempt_retry_after(rate_limit) == 60);
    char excessive_retry[] = "Retry-After: 86401\r\n";
    require(
        socket_rendezvous_attempt_header(excessive_retry, 1, strlen(excessive_retry), rate_limit) ==
        strlen(excessive_retry));
    require(socket_rendezvous_attempt_retry_after(rate_limit) == 0);
    require(socket_rendezvous_attempt_header(status, 1, strlen(status), rate_limit) ==
            strlen(status));
    require(socket_rendezvous_attempt_retry_after(rate_limit) == 0);
    socket_rendezvous_attempt_destroy(rate_limit);

    socket_rendezvous_attempt_t *wrong_result =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, &invite, UINT64_MAX);
    require(wrong_result != NULL && socket_rendezvous_attempt_auth_init(wrong_result, VS(frame)));
    require(socket_rendezvous_attempt_challenge(wrong_result,
                                                vector.auth_challenge,
                                                strlen(vector.auth_challenge),
                                                VS(proof_frame)) ==
            SOCKET_RENDEZVOUS_FRAME_CHALLENGE);
    require(socket_rendezvous_attempt_auth_result(wrong_result,
                                                  negative.wrong_ticket_auth_result,
                                                  strlen(negative.wrong_ticket_auth_result)) ==
            SOCKET_RENDEZVOUS_FRAME_INVALID);
    socket_rendezvous_attempt_destroy(wrong_result);

    socket_rendezvous_attempt_t *duplicate =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(duplicate != NULL && socket_rendezvous_attempt_directory_probe_allowed(duplicate) &&
            socket_rendezvous_attempt_client_candidate(duplicate, "192.0.2.1", 1730, VS(frame)));
    require(!socket_rendezvous_attempt_client_candidate(duplicate, "192.0.2.1", 1730, VS(frame)) &&
            frame[0] == '\0');
    socket_rendezvous_attempt_destroy(duplicate);
    socket_rendezvous_attempt_t *fresh =
        socket_rendezvous_attempt_create(vector.server_id, negative.wrong_ticket, NULL, UINT64_MAX);
    require(fresh != NULL &&
            socket_rendezvous_attempt_client_candidate(fresh, "192.0.2.1", 1730, VS(frame)));
    socket_rendezvous_attempt_destroy(fresh);

    socket_rendezvous_attempt_t *wrong_ticket =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(wrong_ticket != NULL &&
            socket_rendezvous_attempt_client_candidate(wrong_ticket, "192.0.2.1", 1730, VS(frame)));
    memset(&parsed_candidate, 0xff, sizeof(parsed_candidate));
    require(socket_rendezvous_attempt_server_frame(wrong_ticket,
                                                   negative.wrong_ticket_server_candidate,
                                                   strlen(negative.wrong_ticket_server_candidate),
                                                   &parsed_candidate) ==
            SOCKET_RENDEZVOUS_FRAME_INVALID);
    static const socket_direct_candidate_t empty_candidate;
    require(memcmp(&parsed_candidate, &empty_candidate, sizeof(parsed_candidate)) == 0);
    require(socket_rendezvous_attempt_stats(wrong_ticket, &stats) && stats.server_frames == 1);
    socket_rendezvous_attempt_destroy(wrong_ticket);

    socket_rendezvous_attempt_t *malformed =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(malformed != NULL &&
            socket_rendezvous_attempt_client_candidate(malformed, "192.0.2.1", 1730, VS(frame)));
    memset(&parsed_candidate, 0xff, sizeof(parsed_candidate));
    require(socket_rendezvous_attempt_server_frame(malformed,
                                                   negative.oversized_port_server_candidate,
                                                   strlen(negative.oversized_port_server_candidate),
                                                   &parsed_candidate) ==
            SOCKET_RENDEZVOUS_FRAME_INVALID);
    require(memcmp(&parsed_candidate, &empty_candidate, sizeof(parsed_candidate)) == 0);
    socket_rendezvous_attempt_destroy(malformed);

    socket_rendezvous_attempt_t *leading_zero =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(leading_zero != NULL &&
            socket_rendezvous_attempt_client_candidate(leading_zero, "192.0.2.1", 1730, VS(frame)));
    memset(&parsed_candidate, 0xff, sizeof(parsed_candidate));
    require(socket_rendezvous_attempt_server_frame(
                leading_zero,
                negative.leading_zero_port_server_candidate,
                strlen(negative.leading_zero_port_server_candidate),
                &parsed_candidate) == SOCKET_RENDEZVOUS_FRAME_INVALID);
    require(memcmp(&parsed_candidate, &empty_candidate, sizeof(parsed_candidate)) == 0);
    socket_rendezvous_attempt_destroy(leading_zero);

    socket_rendezvous_attempt_t *too_many =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(too_many != NULL &&
            socket_rendezvous_attempt_client_candidate(too_many, "192.0.2.1", 1730, VS(frame)));
    for (unsigned int i = 0; i < 12; i++) {
        snprintf(VS(server_candidate.host), "198.51.100.%u", i + 1U);
        require(socket_rendezvous_server_candidate_render(VS(frame),
                                                          &server_candidate,
                                                          vector.ticket,
                                                          false,
                                                          RENDEZVOUS_SERVER_AUTH_NEW));
        require(socket_rendezvous_attempt_server_frame(too_many,
                                                       frame,
                                                       strlen(frame),
                                                       &parsed_candidate) ==
                SOCKET_RENDEZVOUS_FRAME_CANDIDATE);
    }
    snprintf(VS(server_candidate.host), "198.51.100.13");
    require(socket_rendezvous_server_candidate_render(VS(frame),
                                                      &server_candidate,
                                                      vector.ticket,
                                                      false,
                                                      RENDEZVOUS_SERVER_AUTH_NEW));
    memset(&parsed_candidate, 0xff, sizeof(parsed_candidate));
    require(
        socket_rendezvous_attempt_server_frame(too_many, frame, strlen(frame), &parsed_candidate) ==
        SOCKET_RENDEZVOUS_FRAME_INVALID);
    require(memcmp(&parsed_candidate, &empty_candidate, sizeof(parsed_candidate)) == 0);
    require(socket_rendezvous_attempt_stats(too_many, &stats));
    require(stats.server_frames == 13 && stats.server_candidates == 12);
    socket_rendezvous_attempt_destroy(too_many);

    char wrong[] = "Sec-WebSocket-Protocol: wrong\r\n";
    require(rendezvous_websocket_protocol_header(status, 1, strlen(status), &protocol) ==
            strlen(status));
    require(rendezvous_websocket_protocol_header(wrong, 1, strlen(wrong), &protocol) ==
            strlen(wrong));
    require(!rendezvous_websocket_protocol_valid(&protocol));

    socket_rendezvous_attempt_t *passwordless_protocol =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(passwordless_protocol != NULL &&
            socket_rendezvous_attempt_protocol_valid(passwordless_protocol));
    require(socket_rendezvous_attempt_header(echo, 1, strlen(echo), passwordless_protocol) ==
            strlen(echo));
    require(!socket_rendezvous_attempt_protocol_valid(passwordless_protocol));
    socket_rendezvous_attempt_destroy(passwordless_protocol);

    passwordless_protocol =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(passwordless_protocol != NULL);
    require(socket_rendezvous_attempt_header(echo, 1, strlen(echo), passwordless_protocol) ==
            strlen(echo));
    require(socket_rendezvous_attempt_header(echo, 1, strlen(echo), passwordless_protocol) ==
            strlen(echo));
    require(!socket_rendezvous_attempt_protocol_valid(passwordless_protocol));
    socket_rendezvous_attempt_destroy(passwordless_protocol);

    passwordless_protocol =
        socket_rendezvous_attempt_create(vector.server_id, vector.ticket, NULL, UINT64_MAX);
    require(passwordless_protocol != NULL);
    require(socket_rendezvous_attempt_header(wrong, 1, strlen(wrong), passwordless_protocol) ==
            strlen(wrong));
    require(!socket_rendezvous_attempt_protocol_valid(passwordless_protocol));
    socket_rendezvous_attempt_destroy(passwordless_protocol);

    rendezvous_invite_cleanse(&invite);
    static const rendezvous_invite_t cleared;
    require(CRYPTO_memcmp(&invite, &cleared, sizeof(invite)) == 0);
    OPENSSL_cleanse(&vector, sizeof(vector));
    OPENSSL_cleanse(secret, sizeof(secret));
    OPENSSL_cleanse(challenge, sizeof(challenge));
    OPENSSL_cleanse(proof, sizeof(proof));
    OPENSSL_cleanse(expected_proof, sizeof(expected_proof));
    OPENSSL_cleanse(transcript, sizeof(transcript));
    OPENSSL_cleanse(expected_transcript, sizeof(expected_transcript));
    OPENSSL_cleanse(parsed_bytes, sizeof(parsed_bytes));
    return 0;
}
