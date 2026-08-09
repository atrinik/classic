#include <toolkit/metaserver_publisher.h>
#include <toolkit/path.h>
#include <toolkit/string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#ifndef WIN32
#include <sys/stat.h>
#endif

typedef struct publisher_fixture {
    char authority[254];
    char body[METASERVER_PUBLISH_BODY_MAX + 1U];
    char certificate[METASERVER_PUBLISH_CERTIFICATE_BASE64_MAX + 1U];
    char content_digest[64];
    char game_path[128];
    char game_signature_base[METASERVER_PUBLISH_SIGNATURE_BASE_MAX];
    char game_signature_input[METASERVER_PUBLISH_SIGNATURE_INPUT_MAX];
    char nonce[METASERVER_PUBLISH_NONCE_SIZE * 2U + 1U];
    char path[128];
    char sequence[21];
    char server_id[65];
    char signature_base[METASERVER_PUBLISH_SIGNATURE_BASE_MAX];
    char signature_base64[96];
    char signature_input[METASERVER_PUBLISH_SIGNATURE_INPUT_MAX];
    uint64_t created;
} publisher_fixture_t;

static void require_at(bool condition, int line) {
    if (!condition) {
        fprintf(stderr, "publisher fixture assertion failed at line %d\n", line);
        abort();
    }
}

#define require(condition) require_at((condition), __LINE__)

static void progress(const char *stage) {
#ifdef WIN32
    fprintf(stderr, "publisher fixture stage: %s\n", stage);
#else
    (void)stage;
#endif
}

static char *fixture_read(const char *path) {
    FILE *fp = fopen(path, "rb");
    require(fp != NULL);
    require(fseek(fp, 0, SEEK_END) == 0);
    long length = ftell(fp);
    require(length > 0 && length < 32768);
    require(fseek(fp, 0, SEEK_SET) == 0);
    char *json = calloc((size_t)length + 1U, 1);
    require(json != NULL);
    require(fread(json, 1, (size_t)length, fp) == (size_t)length);
    require(fclose(fp) == 0);
    return json;
}

static bool fixture_string(const char *json, const char *key, char *value, size_t size) {
    char marker[96];
    if (snprintf(VS(marker), "\n  \"%s\": \"", key) >= (int)sizeof(marker)) {
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
            if (cp == 'n') {
                cp = '\n';
            } else if (cp != '\\' && cp != '"') {
                return false;
            }
        } else if (cp < 0x20) {
            return false;
        }
        if (used + 1U >= size) {
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
    if (snprintf(VS(marker), "\n  \"%s\": ", key) >= (int)sizeof(marker)) {
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
           string_parse_uint64(digits, 10, 0, UINT64_MAX, value);
}

static size_t decode_base64(const char *encoded, unsigned char *decoded, size_t capacity) {
    size_t length = strlen(encoded);
    if (length == 0 || length % 4U != 0) {
        return 0;
    }
    size_t padding = 0;
    if (encoded[length - 1U] == '=') {
        padding++;
    }
    if (encoded[length - 2U] == '=') {
        padding++;
    }
    size_t decoded_size = length / 4U * 3U - padding;
    if (decoded_size > capacity) {
        return 0;
    }
    unsigned char *temporary = calloc(length / 4U * 3U, 1);
    if (temporary == NULL) {
        return 0;
    }
    int result = EVP_DecodeBlock(temporary, (const unsigned char *)encoded, (int)length);
    if (result <= 0) {
        free(temporary);
        return 0;
    }
    memcpy(decoded, temporary, decoded_size);
    OPENSSL_clear_free(temporary, length / 4U * 3U);
    return decoded_size;
}

static bool fixture_nested_string(const char *json,
                                  const char *object,
                                  const char *key,
                                  char *value,
                                  size_t size) {
    char object_marker[96], field_marker[96];
    if (snprintf(VS(object_marker), "\n  \"%s\": {", object) >= (int)sizeof(object_marker) ||
        snprintf(VS(field_marker), "\n    \"%s\": \"", key) >= (int)sizeof(field_marker)) {
        return false;
    }
    const char *object_start = strstr(json, object_marker);
    if (object_start == NULL) {
        return false;
    }
    const char *object_end = strstr(object_start + strlen(object_marker), "\n  }");
    const char *cursor = strstr(object_start + strlen(object_marker), field_marker);
    if (object_end == NULL || cursor == NULL || cursor >= object_end) {
        return false;
    }
    const char *duplicate = strstr(cursor + strlen(field_marker), field_marker);
    if (duplicate != NULL && duplicate < object_end) {
        return false;
    }
    cursor += strlen(field_marker);
    size_t used = 0;
    while (cursor < object_end && *cursor != '"') {
        unsigned char cp = (unsigned char)*cursor++;
        if (cp == '\\') {
            cp = (unsigned char)*cursor++;
            if (cp == 'n') {
                cp = '\n';
            } else if (cp != '\\' && cp != '"') {
                return false;
            }
        } else if (cp < 0x20) {
            return false;
        }
        if (used + 1U >= size) {
            return false;
        }
        value[used++] = (char)cp;
    }
    if (cursor >= object_end || *cursor != '"') {
        return false;
    }
    value[used] = '\0';
    return true;
}

static publisher_fixture_t fixture_parse(const char *json) {
    publisher_fixture_t fixture = {0};
    uint64_t version;
    require(fixture_uint64(json, "version", &version) && version == 1);
    require(fixture_uint64(json, "created", &fixture.created));
    require(fixture_string(json, "authority", VS(fixture.authority)));
    require(fixture_string(json, "body", VS(fixture.body)));
    require(fixture_string(json, "certificate_der_base64", VS(fixture.certificate)));
    require(fixture_string(json, "content_digest", VS(fixture.content_digest)));
    require(fixture_string(json, "game_path", VS(fixture.game_path)));
    require(fixture_string(json, "game_signature_base", VS(fixture.game_signature_base)));
    require(fixture_string(json, "game_signature_input", VS(fixture.game_signature_input)));
    require(fixture_string(json, "nonce", VS(fixture.nonce)));
    require(fixture_string(json, "path", VS(fixture.path)));
    require(fixture_string(json, "sequence", VS(fixture.sequence)));
    require(fixture_string(json, "server_id", VS(fixture.server_id)));
    require(fixture_string(json, "signature_base", VS(fixture.signature_base)));
    require(fixture_string(json, "signature_base64", VS(fixture.signature_base64)));
    require(fixture_string(json, "signature_input", VS(fixture.signature_input)));
    return fixture;
}

static void fixture_vector_verify(const char *json,
                                  const char *name,
                                  const publisher_fixture_t *fixture,
                                  const unsigned char *certificate,
                                  size_t certificate_size) {
    char body[METASERVER_PUBLISH_BODY_MAX + 1U];
    char digest[64];
    char nonce_hex[METASERVER_PUBLISH_NONCE_SIZE * 2U + 1U];
    char sequence_text[21];
    char signature_base[METASERVER_PUBLISH_SIGNATURE_BASE_MAX];
    char signature_header[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX];
    char signature_input[METASERVER_PUBLISH_SIGNATURE_INPUT_MAX];
    require(fixture_nested_string(json, name, "body", VS(body)));
    require(fixture_nested_string(json, name, "content_digest", VS(digest)));
    require(fixture_nested_string(json, name, "nonce", VS(nonce_hex)));
    require(fixture_nested_string(json, name, "sequence", VS(sequence_text)));
    require(fixture_nested_string(json, name, "signature_base", VS(signature_base)));
    require(fixture_nested_string(json, name, "signature_header", VS(signature_header)));
    require(fixture_nested_string(json, name, "signature_input", VS(signature_input)));

    uint64_t sequence;
    unsigned char nonce[METASERVER_PUBLISH_NONCE_SIZE];
    require(string_parse_uint64(sequence_text, 10, 1, UINT64_MAX, &sequence));
    require(string_decode_hex_fixed(nonce_hex, sizeof(nonce) * 2U, true, VS(nonce)));
    metaserver_publisher_components_t components;
    require(metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V1,
                                       fixture->authority,
                                       fixture->server_id,
                                       sequence,
                                       nonce,
                                       fixture->created,
                                       body,
                                       strlen(body),
                                       &components));
    require(strcmp(components.content_digest, digest) == 0);
    require(strcmp(components.signature_base, signature_base) == 0);
    require(strcmp(components.signature_input, signature_input) == 0);

    static const char prefix[] = "atrinik=:";
    size_t header_size = strlen(signature_header);
    require(header_size > sizeof(prefix) &&
            memcmp(signature_header, prefix, sizeof(prefix) - 1U) == 0 &&
            signature_header[header_size - 1U] == ':');
    signature_header[header_size - 1U] = '\0';
    unsigned char signature[64];
    require(decode_base64(signature_header + sizeof(prefix) - 1U, signature, sizeof(signature)) ==
            sizeof(signature));
    require(metaserver_publisher_verify(certificate,
                                        certificate_size,
                                        fixture->server_id,
                                        components.signature_base,
                                        signature));
    OPENSSL_cleanse(nonce, sizeof(nonce));
    OPENSSL_cleanse(signature, sizeof(signature));
}

static void sequence_persistence_test(const char *server_id) {
    char directory[HUGE_BUF];
#ifdef WIN32
    char temporary_root[HUGE_BUF];
    DWORD length = GetTempPathA(sizeof(temporary_root), temporary_root);
    require(length > 0 && length < sizeof(temporary_root));
    require(snprintf(VS(directory),
                     "%satrinik-publisher-%lu",
                     temporary_root,
                     (unsigned long)GetCurrentProcessId()) < (int)sizeof(directory));
    require(CreateDirectoryA(directory, NULL));
#else
    snprintf(VS(directory), "/tmp/atrinik-publisher-XXXXXX");
    require(mkdtemp(directory) != NULL);
#endif

#ifdef WIN32
    char probe[HUGE_BUF];
    require(snprintf(VS(probe), "%s/metaserver-publish-sequence-%s.0", directory, server_id) <
            (int)sizeof(probe));
    static const char probe_value[] = "1\n";
    SetLastError(ERROR_SUCCESS);
    path_secret_create_result_t probe_result =
        path_secret_create_atomic(probe, probe_value, sizeof(probe_value) - 1U);
    DWORD probe_error = GetLastError();
    fprintf(stderr,
            "publisher sequence path probe: result=%d system_error=%lu path_length=%u\n",
            probe_result,
            (unsigned long)probe_error,
            (unsigned int)strlen(probe));
    require(probe_result == PATH_SECRET_CREATE_OK);
    require(unlink(probe) == 0);
#endif

    uint64_t sequence = 0;
    metaserver_publish_sequence_result_t initial =
        metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence);
    if (initial != METASERVER_PUBLISH_SEQUENCE_OK) {
        fprintf(stderr,
                "initial sequence reservation failed: result=%d directory=%s\n",
                initial,
                directory);
        for (unsigned int i = 0; i < 2; i++) {
            char slot[HUGE_BUF], value[22];
            bool permissive = false;
            require(snprintf(VS(slot),
                             "%s/metaserver-publish-sequence-%s.%u",
                             directory,
                             server_id,
                             i) < (int)sizeof(slot));
            path_secret_error_t error = path_read_secret(slot, VS(value), &permissive);
            fprintf(stderr,
                    "sequence slot %u after failure: error=%d (%s) permissive=%d value=%s\n",
                    i,
                    error,
                    path_secret_error_string(error),
                    permissive,
                    error == PATH_SECRET_OK ? value : "<unavailable>");
        }
    }
    require(initial == METASERVER_PUBLISH_SEQUENCE_OK);
    require(sequence == 1);
    progress("sequence first reservation");
    require(metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    require(sequence == 2);
    progress("sequence second reservation");
    require(metaserver_publish_sequence_recover(directory, server_id, 100) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    progress("sequence recovery");
    require(metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    require(sequence == 100);
    require(metaserver_publish_sequence_recover(directory, server_id, 50) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    require(metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    require(sequence == 101);
    progress("sequence monotonic recovery");

    char alternate_id[65];
    snprintf(VS(alternate_id), "%s", server_id);
    alternate_id[0] = alternate_id[0] == '0' ? '1' : '0';
    require(metaserver_publish_sequence_reserve(directory, alternate_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_OK);
    require(sequence == 1);
    progress("sequence identity isolation");

    char slot0[HUGE_BUF], slot1[HUGE_BUF], alternate_slot0[HUGE_BUF];
    require(snprintf(VS(slot0), "%s/metaserver-publish-sequence-%s.0", directory, server_id) <
            (int)sizeof(slot0));
    require(snprintf(VS(slot1), "%s/metaserver-publish-sequence-%s.1", directory, server_id) <
            (int)sizeof(slot1));
    require(snprintf(VS(alternate_slot0),
                     "%s/metaserver-publish-sequence-%s.0",
                     directory,
                     alternate_id) < (int)sizeof(alternate_slot0));
    require(unlink(slot0) == 0 || errno == ENOENT);
    require(unlink(slot1) == 0 || errno == ENOENT);
    static const char maximum[] = "18446744073709551615\n";
    require(path_secret_create_atomic(slot0, maximum, sizeof(maximum) - 1U) ==
            PATH_SECRET_CREATE_OK);
    require(metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_EXHAUSTED);
    progress("sequence exhaustion");

    require(unlink(slot0) == 0);
    static const char corrupt[] = "01\n";
    require(path_secret_create_atomic(slot1, corrupt, sizeof(corrupt) - 1U) ==
            PATH_SECRET_CREATE_OK);
    require(metaserver_publish_sequence_reserve(directory, server_id, 1, &sequence) ==
            METASERVER_PUBLISH_SEQUENCE_ERROR);
    progress("sequence corrupt state");
    require(unlink(slot1) == 0);
    require(unlink(alternate_slot0) == 0);
#ifdef WIN32
    require(RemoveDirectoryA(directory));
#else
    require(rmdir(directory) == 0);
#endif

    static const char valid_replay[] = "{\"error\":{\"code\":\"publish_replay\","
                                       "\"minimumNextSequence\":\"18446744073709551615\"}}";
    uint64_t minimum = 0;
    require(metaserver_publish_replay_parse(valid_replay, sizeof(valid_replay) - 1U, &minimum));
    require(minimum == UINT64_MAX);
    static const char leading_zero[] =
        "{\"error\":{\"code\":\"publish_replay\",\"minimumNextSequence\":\"01\"}}";
    require(!metaserver_publish_replay_parse(leading_zero, sizeof(leading_zero) - 1U, &minimum));
    static const char wrong_code[] =
        "{\"error\":{\"code\":\"other\",\"minimumNextSequence\":\"2\"}}";
    require(!metaserver_publish_replay_parse(wrong_code, sizeof(wrong_code) - 1U, &minimum));
    require(!metaserver_publish_replay_parse(valid_replay, sizeof(valid_replay) - 2U, &minimum));
    progress("sequence and replay complete");
}

static void identity_signing_test(void) {
    progress("identity start");
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    X509 *certificate = X509_new();
    X509_NAME *name = X509_NAME_new();
    require(key != NULL && certificate != NULL && name != NULL);
    progress("identity objects created");
    require(X509_set_version(certificate, 2) == 1);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate), 0) != NULL);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != NULL);
    require(X509_set_pubkey(certificate, key) == 1);
    require(X509_NAME_add_entry_by_txt(name,
                                       "CN",
                                       MBSTRING_ASC,
                                       (const unsigned char *)"publisher test",
                                       -1,
                                       -1,
                                       0) == 1);
    require(X509_set_subject_name(certificate, name) == 1);
    require(X509_set_issuer_name(certificate, name) == 1);
    require(X509_sign(certificate, key, EVP_sha256()) > 0);
    progress("identity certificate signed");

    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_size = 0;
    char server_id[SHA256_DIGEST_LENGTH * 2U + 1U];
    require(X509_digest(certificate, EVP_sha256(), digest, &digest_size) == 1);
    require(digest_size == sizeof(digest));
    require(string_tohex(VS(digest), VS(server_id), false) == sizeof(server_id) - 1U);
    string_tolower(server_id);

    metaserver_publisher_identity_t *identity =
        metaserver_publisher_identity_create(certificate, key, server_id);
    require(identity != NULL);
    require(metaserver_publisher_identity_create(certificate, key, "0") == NULL);
    static const char signature_base[] = "publisher identity signing test";
    char signature_header[METASERVER_PUBLISH_SIGNATURE_HEADER_MAX];
    require(metaserver_publisher_identity_sign(identity, signature_base, signature_header));
    progress("identity message signed");
    static const char prefix[] = "atrinik=:";
    size_t header_size = strlen(signature_header);
    require(header_size > sizeof(prefix) &&
            memcmp(signature_header, prefix, sizeof(prefix) - 1U) == 0 &&
            signature_header[header_size - 1U] == ':');
    signature_header[header_size - 1U] = '\0';
    unsigned char signature[64];
    require(decode_base64(signature_header + sizeof(prefix) - 1U, signature, sizeof(signature)) ==
            sizeof(signature));

    int certificate_size = i2d_X509(certificate, NULL);
    require(certificate_size > 0 &&
            (size_t)certificate_size <= METASERVER_PUBLISH_CERTIFICATE_DER_MAX);
    unsigned char *certificate_der = malloc((size_t)certificate_size);
    require(certificate_der != NULL);
    unsigned char *cursor = certificate_der;
    require(i2d_X509(certificate, &cursor) == certificate_size);
    require(metaserver_publisher_verify(certificate_der,
                                        (size_t)certificate_size,
                                        server_id,
                                        signature_base,
                                        signature));
    progress("identity signature verified");

    OPENSSL_clear_free(certificate_der, (size_t)certificate_size);
    OPENSSL_cleanse(signature, sizeof(signature));
    OPENSSL_cleanse(signature_header, sizeof(signature_header));
    OPENSSL_cleanse(server_id, sizeof(server_id));
    OPENSSL_cleanse(digest, sizeof(digest));
    metaserver_publisher_identity_free(identity);
    X509_NAME_free(name);
    X509_free(certificate);
    EVP_PKEY_free(key);
    progress("identity complete");
}

int main(int argc, char **argv) {
    require(argc == 2);
    toolkit_import(path);

    char *fixture_json = fixture_read(argv[1]);
    publisher_fixture_t fixture = fixture_parse(fixture_json);
    uint64_t sequence;
    require(string_parse_uint64(fixture.sequence, 10, 1, UINT64_MAX, &sequence));
    unsigned char nonce[METASERVER_PUBLISH_NONCE_SIZE];
    require(string_decode_hex_fixed(fixture.nonce,
                                    METASERVER_PUBLISH_NONCE_SIZE * 2U,
                                    true,
                                    VS(nonce)));

    char body[METASERVER_PUBLISH_BODY_MAX + 1U];
    size_t body_size;
    metaserver_publisher_classic_payload_t payload = {
        .server_id = fixture.server_id,
        .certificate = fixture.certificate,
        .name = "Atrinik Classic",
        .players_count = 3,
        .version = "5.7.0",
        .text_comment = "Fixture",
        .is_public = true,
        .password_required = true,
    };
    require(metaserver_publisher_classic_body(&payload, body, &body_size));
    require(body_size == strlen(fixture.body));
    require(memcmp(body, fixture.body, body_size) == 0);
    payload.name = "quote \" and slash \\";
    require(metaserver_publisher_classic_body(&payload, body, &body_size));
    require(strstr(body, "\"name\":\"quote \\\" and slash \\\\\"") != NULL);
    payload.name = "bad\nname";
    require(!metaserver_publisher_classic_body(&payload, body, &body_size));
    payload.name = "\xf0\x28\x8c\x28";
    require(!metaserver_publisher_classic_body(&payload, body, &body_size));
    payload.name = "Atrinik Classic";
    payload.certificate = "AB==";
    require(!metaserver_publisher_classic_body(&payload, body, &body_size));
    payload.certificate = fixture.certificate;

    metaserver_publisher_components_t classic;
    require(metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V1,
                                       fixture.authority,
                                       fixture.server_id,
                                       sequence,
                                       nonce,
                                       fixture.created,
                                       fixture.body,
                                       strlen(fixture.body),
                                       &classic));
    require(strcmp(classic.path, fixture.path) == 0);
    require(strcmp(classic.content_digest, fixture.content_digest) == 0);
    require(strcmp(classic.signature_input, fixture.signature_input) == 0);
    require(strcmp(classic.signature_base, fixture.signature_base) == 0);

    unsigned char certificate[2048], signature[64];
    size_t certificate_size = decode_base64(fixture.certificate, certificate, sizeof(certificate));
    require(certificate_size > 0);
    require(decode_base64(fixture.signature_base64, signature, sizeof(signature)) ==
            sizeof(signature));
    require(metaserver_publisher_verify(certificate,
                                        certificate_size,
                                        fixture.server_id,
                                        classic.signature_base,
                                        signature));
    static const char *const vectors[] = {"heartbeat",
                                          "changed",
                                          "reused_nonce",
                                          "stale",
                                          "private"};
    for (size_t i = 0; i < arraysize(vectors); i++) {
        fixture_vector_verify(fixture_json, vectors[i], &fixture, certificate, certificate_size);
    }

    metaserver_publisher_components_t game;
    require(metaserver_publisher_build(METASERVER_PUBLISHER_GAME_V1,
                                       fixture.authority,
                                       fixture.server_id,
                                       sequence,
                                       nonce,
                                       fixture.created,
                                       fixture.body,
                                       strlen(fixture.body),
                                       &game));
    require(strcmp(game.path, fixture.game_path) == 0);
    require(strcmp(game.signature_input, fixture.game_signature_input) == 0);
    require(strcmp(game.signature_base, fixture.game_signature_base) == 0);
    require(!metaserver_publisher_verify(certificate,
                                         certificate_size,
                                         fixture.server_id,
                                         game.signature_base,
                                         signature));

    char mutated[METASERVER_PUBLISH_SIGNATURE_BASE_MAX];
    snprintf(VS(mutated), "%s", classic.signature_base);
    mutated[0] ^= 1;
    require(!metaserver_publisher_verify(certificate,
                                         certificate_size,
                                         fixture.server_id,
                                         mutated,
                                         signature));
    signature[0] ^= 1;
    require(!metaserver_publisher_verify(certificate,
                                         certificate_size,
                                         fixture.server_id,
                                         classic.signature_base,
                                         signature));
    require(!metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V1,
                                        "Publish.meta.atrinik.org",
                                        fixture.server_id,
                                        sequence,
                                        nonce,
                                        fixture.created,
                                        fixture.body,
                                        strlen(fixture.body),
                                        &classic));
    require(!metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V1,
                                        fixture.authority,
                                        fixture.server_id,
                                        0,
                                        nonce,
                                        fixture.created,
                                        fixture.body,
                                        strlen(fixture.body),
                                        &classic));
    memset(nonce, 0, sizeof(nonce));
    require(!metaserver_publisher_build(METASERVER_PUBLISHER_CLASSIC_V1,
                                        fixture.authority,
                                        fixture.server_id,
                                        sequence,
                                        nonce,
                                        fixture.created,
                                        fixture.body,
                                        strlen(fixture.body),
                                        &classic));

    sequence_persistence_test(fixture.server_id);
    identity_signing_test();

    OPENSSL_cleanse(certificate, sizeof(certificate));
    OPENSSL_cleanse(signature, sizeof(signature));
    OPENSSL_cleanse(nonce, sizeof(nonce));
    OPENSSL_cleanse(fixture_json, strlen(fixture_json));
    free(fixture_json);
    toolkit_deinit();
    return 0;
}
