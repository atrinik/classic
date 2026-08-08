/*************************************************************************
 * Protected classic rendezvous authorization.                          *
 ************************************************************************/

#include "rendezvous.h"
#include "string.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

static const unsigned char rendezvous_proof_domain[] = RENDEZVOUS_INVITE_SUBPROTOCOL;

size_t
rendezvous_websocket_protocol_header(char *data, size_t size, size_t count, void *user_data) {
    rendezvous_websocket_protocol_t *protocol = user_data;
    if (size != 0 && count > SIZE_MAX / size) {
        return 0;
    }
    size_t bytes = size * count;
    static const char status_prefix[] = "HTTP/";
    static const char name[] = "Sec-WebSocket-Protocol:";
    if (protocol == NULL || (bytes != 0 && data == NULL)) {
        return 0;
    }
    if (bytes >= sizeof(status_prefix) - 1U &&
        memcmp(data, status_prefix, sizeof(status_prefix) - 1U) == 0) {
        protocol->echoes = 0;
        protocol->invalid = false;
        return bytes;
    }
    if (bytes < sizeof(name) - 1U || strncasecmp(data, name, sizeof(name) - 1U) != 0) {
        return bytes;
    }
    const char *value = data + sizeof(name) - 1U;
    const char *end = data + bytes;
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (end > value &&
           (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    protocol->echoes++;
    if ((size_t)(end - value) != sizeof(RENDEZVOUS_INVITE_SUBPROTOCOL) - 1U ||
        memcmp(value, RENDEZVOUS_INVITE_SUBPROTOCOL, sizeof(RENDEZVOUS_INVITE_SUBPROTOCOL) - 1U) !=
            0) {
        protocol->invalid = true;
    }
    return bytes;
}

bool rendezvous_websocket_protocol_valid(const rendezvous_websocket_protocol_t *protocol) {
    return protocol != NULL && protocol->echoes == 1U && !protocol->invalid;
}

bool rendezvous_server_auth_challenge_sent(rendezvous_server_auth_state_t *state) {
    if (state == NULL || *state != RENDEZVOUS_SERVER_AUTH_NEW) {
        return false;
    }
    *state = RENDEZVOUS_SERVER_AUTH_WAIT_PROOF;
    return true;
}

bool rendezvous_server_auth_result_sent(rendezvous_server_auth_state_t *state, bool authorized) {
    if (state == NULL || *state != RENDEZVOUS_SERVER_AUTH_WAIT_PROOF) {
        return false;
    }
    *state = authorized ? RENDEZVOUS_SERVER_AUTH_AUTHORIZED : RENDEZVOUS_SERVER_AUTH_DENIED;
    return true;
}

bool rendezvous_server_auth_candidate_consume(rendezvous_server_auth_state_t *state) {
    if (state == NULL || *state != RENDEZVOUS_SERVER_AUTH_AUTHORIZED) {
        return false;
    }
    *state = RENDEZVOUS_SERVER_AUTH_CONSUMED;
    return true;
}

static bool
rendezvous_hex_render(const unsigned char *value, size_t value_size, char *hex, size_t hex_size) {
    if (string_tohex(value, value_size, hex, hex_size, false) != value_size * 2U) {
        return false;
    }
    string_tolower(hex);
    return true;
}

void rendezvous_invite_cleanse(rendezvous_invite_t *invite) {
    if (invite != NULL) {
        OPENSSL_cleanse(invite, sizeof(*invite));
    }
}

bool rendezvous_invite_parse(const char *text, rendezvous_invite_t *invite) {
    rendezvous_invite_t parsed = {0};
    char secret_hex[RENDEZVOUS_SECRET_SIZE * 2U + 1U] = {0};
    char expiry_text[21] = {0};
    unsigned long long expiry;
    int consumed = 0;

    if (invite == NULL) {
        return false;
    }
    rendezvous_invite_cleanse(invite);
    if (text == NULL || strlen(text) >= RENDEZVOUS_INVITE_TEXT_SIZE ||
        sscanf(text,
               RENDEZVOUS_INVITE_PREFIX ".%64[0-9a-f].%32[0-9a-f].%64[0-9a-f].%20[0-9]%n",
               parsed.server_id,
               parsed.invite_id,
               secret_hex,
               expiry_text,
               &consumed) != 4 ||
        text[consumed] != '\0' ||
        !string_is_hex_fixed(parsed.server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) ||
        !string_is_hex_fixed(parsed.invite_id, RENDEZVOUS_INVITE_ID_HEX_SIZE, true) ||
        !string_is_hex_fixed(secret_hex, RENDEZVOUS_SECRET_SIZE * 2U, true) ||
        (expiry_text[0] == '0' && expiry_text[1] != '\0')) {
        OPENSSL_cleanse(&parsed, sizeof(parsed));
        OPENSSL_cleanse(secret_hex, sizeof(secret_hex));
        OPENSSL_cleanse(expiry_text, sizeof(expiry_text));
        return false;
    }
    errno = 0;
    char *end = NULL;
    expiry = strtoull(expiry_text, &end, 10);
    if (errno == ERANGE || end == expiry_text || *end != '\0' || expiry == 0 ||
        (uint64_t)expiry != expiry ||
        !string_decode_hex_fixed(secret_hex,
                                 RENDEZVOUS_SECRET_SIZE * 2U,
                                 true,
                                 parsed.secret,
                                 sizeof(parsed.secret))) {
        OPENSSL_cleanse(&parsed, sizeof(parsed));
        OPENSSL_cleanse(secret_hex, sizeof(secret_hex));
        OPENSSL_cleanse(expiry_text, sizeof(expiry_text));
        return false;
    }
    parsed.expiry = (uint64_t)expiry;
    *invite = parsed;
    OPENSSL_cleanse(&parsed, sizeof(parsed));
    OPENSSL_cleanse(secret_hex, sizeof(secret_hex));
    OPENSSL_cleanse(expiry_text, sizeof(expiry_text));
    return true;
}

bool rendezvous_invite_render(const rendezvous_invite_t *invite, char *text, size_t text_size) {
    char secret_hex[RENDEZVOUS_SECRET_SIZE * 2U + 1U];
    if (invite == NULL || text == NULL || text_size == 0 ||
        !string_is_hex_fixed(invite->server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) ||
        !string_is_hex_fixed(invite->invite_id, RENDEZVOUS_INVITE_ID_HEX_SIZE, true) ||
        invite->expiry == 0 ||
        !rendezvous_hex_render(invite->secret, sizeof(invite->secret), VS(secret_hex))) {
        if (text != NULL && text_size != 0) {
            text[0] = '\0';
        }
        OPENSSL_cleanse(secret_hex, sizeof(secret_hex));
        return false;
    }
    int length = snprintf(text,
                          text_size,
                          RENDEZVOUS_INVITE_PREFIX ".%s.%s.%s.%" PRIu64,
                          invite->server_id,
                          invite->invite_id,
                          secret_hex,
                          invite->expiry);
    OPENSSL_cleanse(secret_hex, sizeof(secret_hex));
    if (length < 0 || (size_t)length >= text_size) {
        text[0] = '\0';
        return false;
    }
    return true;
}

bool rendezvous_invite_generate(const char *server_id,
                                uint64_t expiry,
                                rendezvous_invite_t *invite) {
    unsigned char invite_id[RENDEZVOUS_INVITE_ID_HEX_SIZE / 2U];
    rendezvous_invite_t generated = {0};
    if (invite == NULL) {
        return false;
    }
    rendezvous_invite_cleanse(invite);
    if (!string_is_hex_fixed(server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) || expiry == 0 ||
        RAND_priv_bytes(VS(invite_id)) != 1 || RAND_priv_bytes(VS(generated.secret)) != 1 ||
        !rendezvous_hex_render(invite_id, sizeof(invite_id), VS(generated.invite_id))) {
        OPENSSL_cleanse(invite_id, sizeof(invite_id));
        OPENSSL_cleanse(&generated, sizeof(generated));
        return false;
    }
    snprintf(VS(generated.server_id), "%s", server_id);
    generated.expiry = expiry;
    *invite = generated;
    OPENSSL_cleanse(invite_id, sizeof(invite_id));
    OPENSSL_cleanse(&generated, sizeof(generated));
    return true;
}

bool rendezvous_invite_valid_at(const rendezvous_invite_t *invite,
                                const char *server_id,
                                uint64_t now) {
    return rendezvous_invite_matches_server(invite, server_id) &&
           !rendezvous_invite_expired_at(invite, now) &&
           invite->expiry - now <= RENDEZVOUS_INVITE_LIFETIME_MAX;
}

bool rendezvous_invite_matches_server(const rendezvous_invite_t *invite, const char *server_id) {
    return invite != NULL && string_is_hex_fixed(server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) &&
           string_is_hex_fixed(invite->server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) &&
           CRYPTO_memcmp(invite->server_id, server_id, RENDEZVOUS_SERVER_ID_HEX_SIZE) == 0;
}

bool rendezvous_invite_expired_at(const rendezvous_invite_t *invite, uint64_t now) {
    return invite == NULL || invite->expiry <= now;
}

bool rendezvous_invite_transcript(const rendezvous_invite_t *invite,
                                  const char *ticket,
                                  const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                                  unsigned char transcript[RENDEZVOUS_TRANSCRIPT_SIZE]) {
    unsigned char *cursor = transcript;
    unsigned char server_id[32], ticket_bytes[32], invite_id[16];
    bool ok = invite != NULL && challenge != NULL && transcript != NULL &&
              string_decode_hex_fixed(invite->server_id, 64, true, VS(server_id)) &&
              string_decode_hex_fixed(ticket, 64, true, VS(ticket_bytes)) &&
              string_decode_hex_fixed(invite->invite_id, 32, true, VS(invite_id));
    if (!ok) {
        if (transcript != NULL) {
            OPENSSL_cleanse(transcript, RENDEZVOUS_TRANSCRIPT_SIZE);
        }
        goto out;
    }
    memcpy(cursor, rendezvous_proof_domain, sizeof(rendezvous_proof_domain));
    cursor += sizeof(rendezvous_proof_domain);
    memcpy(cursor, server_id, sizeof(server_id));
    cursor += sizeof(server_id);
    memcpy(cursor, ticket_bytes, sizeof(ticket_bytes));
    cursor += sizeof(ticket_bytes);
    memcpy(cursor, invite_id, sizeof(invite_id));
    cursor += sizeof(invite_id);
    memcpy(cursor, challenge, RENDEZVOUS_CHALLENGE_SIZE);
    cursor += RENDEZVOUS_CHALLENGE_SIZE;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        cursor[7U - shift / 8U] = (unsigned char)(invite->expiry >> shift);
    }
out:
    OPENSSL_cleanse(server_id, sizeof(server_id));
    OPENSSL_cleanse(ticket_bytes, sizeof(ticket_bytes));
    OPENSSL_cleanse(invite_id, sizeof(invite_id));
    return ok;
}

bool rendezvous_invite_proof(const rendezvous_invite_t *invite,
                             const char *ticket,
                             const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                             unsigned char proof[RENDEZVOUS_PROOF_SIZE]) {
    unsigned char input[RENDEZVOUS_TRANSCRIPT_SIZE];
    EVP_PKEY *key = NULL;
    EVP_MD_CTX *context = NULL;
    size_t proof_size = 0;
    bool ok = proof != NULL && rendezvous_invite_transcript(invite, ticket, challenge, input);
    if (!ok) {
        goto out;
    }
    key = EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, NULL, invite->secret, sizeof(invite->secret));
    context = key != NULL ? EVP_MD_CTX_new() : NULL;
    proof_size = RENDEZVOUS_PROOF_SIZE;
    ok = context != NULL && EVP_DigestSignInit(context, NULL, EVP_sha256(), NULL, key) == 1 &&
         EVP_DigestSign(context, proof, &proof_size, input, sizeof(input)) == 1 &&
         proof_size == RENDEZVOUS_PROOF_SIZE;
out:
    if (!ok && proof != NULL) {
        OPENSSL_cleanse(proof, RENDEZVOUS_PROOF_SIZE);
    }
    OPENSSL_cleanse(input, sizeof(input));
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return ok;
}

bool rendezvous_invite_proof_verify(const rendezvous_invite_t *invite,
                                    const char *ticket,
                                    const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                                    const unsigned char proof[RENDEZVOUS_PROOF_SIZE]) {
    unsigned char expected[RENDEZVOUS_PROOF_SIZE];
    bool ok = proof != NULL && rendezvous_invite_proof(invite, ticket, challenge, expected) &&
              CRYPTO_memcmp(expected, proof, sizeof(expected)) == 0;
    OPENSSL_cleanse(expected, sizeof(expected));
    return ok;
}

static bool rendezvous_frame_write(char *frame, size_t frame_size, int length) {
    if (frame == NULL || frame_size == 0 || length < 0 || (size_t)length >= frame_size ||
        (size_t)length > RENDEZVOUS_FRAME_MAX) {
        if (frame != NULL && frame_size != 0) {
            frame[0] = '\0';
        }
        return false;
    }
    return true;
}

bool rendezvous_auth_init_render(char *frame,
                                 size_t frame_size,
                                 const char *ticket,
                                 const char *invite_id) {
    if (frame != NULL && frame_size != 0) {
        frame[0] = '\0';
    }
    if (frame == NULL || frame_size == 0 || !string_is_hex_fixed(ticket, 64, true) ||
        !string_is_hex_fixed(invite_id, 32, true)) {
        return false;
    }
    return rendezvous_frame_write(
        frame,
        frame_size,
        snprintf(frame,
                 frame_size,
                 "{\"type\":\"auth_init\",\"version\":1,\"ticket\":\"%s\",\"invite_id\":\"%s\"}",
                 ticket,
                 invite_id));
}

bool rendezvous_auth_init_parse(const char *frame, char ticket[65], char invite_id[33]) {
    char parsed_ticket[65], parsed_id[33], canonical[RENDEZVOUS_FRAME_MAX + 1U];
    int consumed = 0;
    if (ticket == NULL || invite_id == NULL) {
        return false;
    }
    ticket[0] = invite_id[0] = '\0';
    if (frame == NULL || strlen(frame) > RENDEZVOUS_FRAME_MAX ||
        sscanf(frame,
               "{\"type\":\"auth_init\",\"version\":1,\"ticket\":\"%64[0-9a-f]\",\"invite_id\":\"%"
               "32[0-9a-f]\"}%n",
               parsed_ticket,
               parsed_id,
               &consumed) != 2 ||
        frame[consumed] != '\0' ||
        !rendezvous_auth_init_render(VS(canonical), parsed_ticket, parsed_id) ||
        strcmp(frame, canonical) != 0) {
        return false;
    }
    memcpy(ticket, parsed_ticket, sizeof(parsed_ticket));
    memcpy(invite_id, parsed_id, sizeof(parsed_id));
    return true;
}

static bool rendezvous_auth_bytes_render(char *frame,
                                         size_t frame_size,
                                         const char *type,
                                         const char *field,
                                         const char *ticket,
                                         const unsigned char value[32]) {
    char hex[65] = {0};
    if (frame == NULL || frame_size == 0 || !string_is_hex_fixed(ticket, 64, true) ||
        value == NULL || !rendezvous_hex_render(value, 32, VS(hex))) {
        if (frame != NULL && frame_size != 0) {
            frame[0] = '\0';
        }
        OPENSSL_cleanse(hex, sizeof(hex));
        return false;
    }
    bool ok = rendezvous_frame_write(
        frame,
        frame_size,
        snprintf(frame,
                 frame_size,
                 "{\"type\":\"%s\",\"version\":1,\"ticket\":\"%s\",\"%s\":\"%s\"}",
                 type,
                 ticket,
                 field,
                 hex));
    OPENSSL_cleanse(hex, sizeof(hex));
    return ok;
}

static bool rendezvous_auth_bytes_parse(const char *frame,
                                        const char *expected_ticket,
                                        const char *format,
                                        const char *type,
                                        const char *field,
                                        unsigned char value[32]) {
    char ticket[65], hex[65], canonical[RENDEZVOUS_FRAME_MAX + 1U];
    int consumed = 0;
    bool ok = frame != NULL && expected_ticket != NULL && value != NULL &&
              strlen(frame) <= RENDEZVOUS_FRAME_MAX &&
              sscanf(frame, format, ticket, hex, &consumed) == 2 && frame[consumed] == '\0' &&
              strcmp(ticket, expected_ticket) == 0 &&
              string_decode_hex_fixed(hex, 64, true, value, 32) &&
              rendezvous_auth_bytes_render(VS(canonical), type, field, ticket, value) &&
              strcmp(frame, canonical) == 0;
    if (!ok && value != NULL) {
        OPENSSL_cleanse(value, 32);
    }
    OPENSSL_cleanse(hex, sizeof(hex));
    OPENSSL_cleanse(canonical, sizeof(canonical));
    OPENSSL_cleanse(ticket, sizeof(ticket));
    return ok;
}

bool rendezvous_auth_challenge_render(char *frame,
                                      size_t frame_size,
                                      const char *ticket,
                                      const unsigned char challenge[32]) {
    return rendezvous_auth_bytes_render(frame,
                                        frame_size,
                                        "auth_challenge",
                                        "challenge",
                                        ticket,
                                        challenge);
}

bool rendezvous_auth_challenge_parse(const char *frame,
                                     const char *expected_ticket,
                                     unsigned char challenge[32]) {
    return rendezvous_auth_bytes_parse(frame,
                                       expected_ticket,
                                       "{\"type\":\"auth_challenge\",\"version\":1,\"ticket\":\"%"
                                       "64[0-9a-f]\",\"challenge\":\"%64[0-9a-f]\"}%n",
                                       "auth_challenge",
                                       "challenge",
                                       challenge);
}

bool rendezvous_auth_proof_render(char *frame,
                                  size_t frame_size,
                                  const char *ticket,
                                  const unsigned char proof[32]) {
    return rendezvous_auth_bytes_render(frame, frame_size, "auth_proof", "proof", ticket, proof);
}

bool rendezvous_auth_proof_parse(const char *frame,
                                 const char *expected_ticket,
                                 unsigned char proof[32]) {
    return rendezvous_auth_bytes_parse(frame,
                                       expected_ticket,
                                       "{\"type\":\"auth_proof\",\"version\":1,\"ticket\":\"%64[0-"
                                       "9a-f]\",\"proof\":\"%64[0-9a-f]\"}%n",
                                       "auth_proof",
                                       "proof",
                                       proof);
}

bool rendezvous_auth_result_render(char *frame,
                                   size_t frame_size,
                                   const char *ticket,
                                   bool authorized) {
    if (frame != NULL && frame_size != 0) {
        frame[0] = '\0';
    }
    if (frame == NULL || frame_size == 0 || !string_is_hex_fixed(ticket, 64, true)) {
        return false;
    }
    return rendezvous_frame_write(
        frame,
        frame_size,
        snprintf(frame,
                 frame_size,
                 "{\"type\":\"auth_result\",\"version\":1,\"ticket\":\"%s\",\"authorized\":%s}",
                 ticket,
                 authorized ? "true" : "false"));
}

bool rendezvous_auth_result_parse(const char *frame,
                                  const char *expected_ticket,
                                  bool *authorized) {
    char expected[RENDEZVOUS_FRAME_MAX + 1U];
    if (authorized != NULL) {
        *authorized = false;
    }
    if (frame == NULL || expected_ticket == NULL || authorized == NULL ||
        strlen(frame) > RENDEZVOUS_FRAME_MAX) {
        return false;
    }
    for (unsigned int value = 0; value <= 1; value++) {
        if (rendezvous_auth_result_render(VS(expected), expected_ticket, value != 0) &&
            strcmp(frame, expected) == 0) {
            *authorized = value != 0;
            return true;
        }
    }
    return false;
}
