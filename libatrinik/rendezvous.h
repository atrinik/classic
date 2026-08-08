/*************************************************************************
 * Protected classic rendezvous authorization.                          *
 ************************************************************************/

#ifndef TOOLKIT_RENDEZVOUS_H
#define TOOLKIT_RENDEZVOUS_H

#include "toolkit.h"

#define RENDEZVOUS_INVITE_SUBPROTOCOL "atrinik-classic-rendezvous-invite-v1"
#define RENDEZVOUS_INVITE_PREFIX "atrinik-invite-v1"
#define RENDEZVOUS_SERVER_ID_HEX_SIZE 64U
#define RENDEZVOUS_TICKET_HEX_SIZE 64U
#define RENDEZVOUS_INVITE_ID_HEX_SIZE 32U
#define RENDEZVOUS_SECRET_SIZE 32U
#define RENDEZVOUS_CHALLENGE_SIZE 32U
#define RENDEZVOUS_PROOF_SIZE 32U
#define RENDEZVOUS_TRANSCRIPT_SIZE \
    (sizeof(RENDEZVOUS_INVITE_SUBPROTOCOL) + 32U + 32U + 16U + 32U + 8U)
#define RENDEZVOUS_FRAME_MAX 512U
#define RENDEZVOUS_INVITE_LIFETIME_MAX (7U * 24U * 60U * 60U)
/* Prefix, four separators, fixed hexadecimal fields, uint64 decimal, NUL. */
#define RENDEZVOUS_INVITE_TEXT_SIZE \
    (sizeof(RENDEZVOUS_INVITE_PREFIX) + 1U + 64U + 1U + 32U + 1U + 64U + 1U + 20U)

/**
 * Caller-owned sensitive invite capability.
 *
 * Values are mutable and copyable only before use. Every live copy must be
 * cleansed with ::rendezvous_invite_cleanse when its attempt ends. Concurrent
 * readers require no synchronization; mutation and cleansing are thread-confined.
 */
typedef struct rendezvous_invite {
    char server_id[RENDEZVOUS_SERVER_ID_HEX_SIZE + 1U];
    char invite_id[RENDEZVOUS_INVITE_ID_HEX_SIZE + 1U];
    unsigned char secret[RENDEZVOUS_SECRET_SIZE];
    uint64_t expiry;
} rendezvous_invite_t;

/** Mutable per-upgrade header-validation state; initialize to zero and do not share. */
typedef struct rendezvous_websocket_protocol {
    unsigned int echoes;
    bool invalid;
} rendezvous_websocket_protocol_t;

/**
 * One server-side authorization transition chain.
 *
 * The owning connection advances NEW -> WAIT_PROOF -> AUTHORIZED/DENIED and
 * consumes AUTHORIZED exactly once. Do not copy or reuse a state after the
 * first transition; confine it to the connection thread.
 */
typedef enum rendezvous_server_auth_state {
    RENDEZVOUS_SERVER_AUTH_NEW,
    RENDEZVOUS_SERVER_AUTH_WAIT_PROOF,
    RENDEZVOUS_SERVER_AUTH_AUTHORIZED,
    RENDEZVOUS_SERVER_AUTH_DENIED,
    RENDEZVOUS_SERVER_AUTH_CONSUMED
} rendezvous_server_auth_state_t;

/** Parse one exact canonical capability; clears `invite` on failure. */
bool rendezvous_invite_parse(const char *text, rendezvous_invite_t *invite);
/** Render one canonical capability; clears `text` on failure when possible. */
bool rendezvous_invite_render(const rendezvous_invite_t *invite, char *text, size_t text_size);
/** Generate a new caller-owned capability; clears `invite` on failure. */
bool rendezvous_invite_generate(const char *server_id,
                                uint64_t expiry,
                                rendezvous_invite_t *invite);
bool rendezvous_invite_valid_at(const rendezvous_invite_t *invite,
                                const char *server_id,
                                uint64_t now);
bool rendezvous_invite_matches_server(const rendezvous_invite_t *invite, const char *server_id);
bool rendezvous_invite_expired_at(const rendezvous_invite_t *invite, uint64_t now);
void rendezvous_invite_cleanse(rendezvous_invite_t *invite);
/** libcurl-compatible callback that consumes headers into one mutable upgrade state. */
size_t rendezvous_websocket_protocol_header(char *data, size_t size, size_t count, void *user_data);
/** True only after exactly one canonical subprotocol echo. */
bool rendezvous_websocket_protocol_valid(const rendezvous_websocket_protocol_t *protocol);
/** Advance the owned server state; false is non-mutating for an invalid transition. */
bool rendezvous_server_auth_challenge_sent(rendezvous_server_auth_state_t *state);
bool rendezvous_server_auth_result_sent(rendezvous_server_auth_state_t *state, bool authorized);
bool rendezvous_server_auth_candidate_consume(rendezvous_server_auth_state_t *state);

/** Derive the fixed proof; clears `proof` on every failure. */
bool rendezvous_invite_proof(const rendezvous_invite_t *invite,
                             const char *ticket,
                             const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                             unsigned char proof[RENDEZVOUS_PROOF_SIZE]);
/** Build the byte-exact shared transcript; clears `transcript` on failure. */
bool rendezvous_invite_transcript(const rendezvous_invite_t *invite,
                                  const char *ticket,
                                  const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                                  unsigned char transcript[RENDEZVOUS_TRANSCRIPT_SIZE]);
bool rendezvous_invite_proof_verify(const rendezvous_invite_t *invite,
                                    const char *ticket,
                                    const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE],
                                    const unsigned char proof[RENDEZVOUS_PROOF_SIZE]);

/**
 * Render/parse canonical authorization frames.
 *
 * Renderers clear their caller-owned frame on failure when possible. Parsers
 * reject non-canonical, trailing, wrong-ticket, and wrong-revision input and
 * clear every caller-owned output on failure. All inputs are borrowed for the
 * call and all output storage remains caller-owned.
 */
bool rendezvous_auth_init_render(char *frame,
                                 size_t frame_size,
                                 const char *ticket,
                                 const char *invite_id);
bool rendezvous_auth_init_parse(const char *frame, char ticket[65], char invite_id[33]);
bool rendezvous_auth_challenge_render(char *frame,
                                      size_t frame_size,
                                      const char *ticket,
                                      const unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE]);
bool rendezvous_auth_challenge_parse(const char *frame,
                                     const char *expected_ticket,
                                     unsigned char challenge[RENDEZVOUS_CHALLENGE_SIZE]);
bool rendezvous_auth_proof_render(char *frame,
                                  size_t frame_size,
                                  const char *ticket,
                                  const unsigned char proof[RENDEZVOUS_PROOF_SIZE]);
bool rendezvous_auth_proof_parse(const char *frame,
                                 const char *expected_ticket,
                                 unsigned char proof[RENDEZVOUS_PROOF_SIZE]);
bool rendezvous_auth_result_render(char *frame,
                                   size_t frame_size,
                                   const char *ticket,
                                   bool authorized);
bool rendezvous_auth_result_parse(const char *frame, const char *expected_ticket, bool *authorized);

#endif
