/**
 * @file
 *
 * OpenSSL 3.5 QUIC transport support.
 */

#include "socket_private.h"
#include "metaserver_publisher.h"
#include "path.h"
#include "string.h"
#include "datetime.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#ifdef WIN32
#include <mstcpip.h>

/* Older MinGW headers omit this Windows UDP ioctl declaration. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#if OPENSSL_VERSION_NUMBER >= 0x30500000L

#define SOCKET_CLIENT_ATTEMPT_TIMEOUT_MS 15000U

static const unsigned char socket_quic_alpn[] = {9, 'a', 't', 'r', 'i', 'n', 'i', 'k', '/', '2'};

/**
 * Prevent an ICMP error for one UDP datagram from failing a multiplexed QUIC
 * socket on Windows. This is especially important while hole punching, where
 * some candidate endpoints are expected to be unreachable.
 */
static bool socket_quic_disable_udp_connreset(int fd) {
#ifdef WIN32
    BOOL enabled = FALSE;
    DWORD bytes_returned = 0;
    if (WSAIoctl(fd,
                 SIO_UDP_CONNRESET,
                 &enabled,
                 sizeof(enabled),
                 NULL,
                 0,
                 &bytes_returned,
                 NULL,
                 NULL) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        LOG(ERROR, "Failed to disable UDP connection resets: %s (%d)", s_strerror(error), error);
        return false;
    }
#else
    (void)fd;
#endif

    return true;
}

static int socket_quic_select_alpn(SSL *ssl,
                                   const unsigned char **out,
                                   unsigned char *out_len,
                                   const unsigned char *in,
                                   unsigned int in_len,
                                   void *arg) {
    if (SSL_select_next_proto((unsigned char **)out,
                              out_len,
                              socket_quic_alpn,
                              sizeof(socket_quic_alpn),
                              in,
                              in_len) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static void socket_quic_log_error(const char *operation) {
    unsigned long error = ERR_get_error();
    LOG(ERROR,
        "QUIC %s failed: %s",
        operation,
        error != 0 ? ERR_error_string(error, NULL) : "unknown error");
}

static bool socket_quic_use_identity(SSL_CTX *ctx, X509 *cert, EVP_PKEY *key) {
    return cert != NULL && key != NULL && SSL_CTX_use_certificate(ctx, cert) == 1 &&
           SSL_CTX_use_PrivateKey(ctx, key) == 1 && SSL_CTX_check_private_key(ctx) == 1;
}

static bool socket_quic_load_identity(SSL_CTX *ctx, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
    EVP_PKEY *key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);

    bool ok = socket_quic_use_identity(ctx, cert, key);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

static bool socket_quic_save_identity(const char *path, X509 *cert, EVP_PKEY *key) {
    BIO *memory = BIO_new(BIO_s_mem());
    BUF_MEM *contents = NULL;
    bool ok = memory != NULL && PEM_write_bio_X509(memory, cert) == 1 &&
              PEM_write_bio_PrivateKey(memory, key, NULL, NULL, 0, NULL, NULL) == 1;
    if (ok) {
        BIO_get_mem_ptr(memory, &contents);
        ok = contents != NULL &&
             path_write_atomic(path, contents->data, contents->length, S_IRUSR | S_IWUSR);
    }
    BIO_free(memory);
    if (!ok) {
        LOG(ERROR, "Failed to write persistent QUIC identity %s", path);
    }
    return ok;
}

static bool socket_quic_generate_identity(SSL_CTX *ctx, const char *identity_path) {
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    X509 *cert = X509_new();
    if (key == NULL || cert == NULL || X509_set_version(cert, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)time(NULL)) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(cert), 0) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(cert), 315360000L) == NULL ||
        X509_set_pubkey(cert, key) != 1) {
        EVP_PKEY_free(key);
        X509_free(cert);
        return false;
    }

    X509_NAME *name = X509_NAME_new();
    bool ok = name != NULL &&
              X509_NAME_add_entry_by_txt(name,
                                         "CN",
                                         MBSTRING_ASC,
                                         (const unsigned char *)"Atrinik private server",
                                         -1,
                                         -1,
                                         0) == 1 &&
              X509_set_subject_name(cert, name) == 1 && X509_set_issuer_name(cert, name) == 1 &&
              X509_sign(cert, key, EVP_sha256()) > 0 && socket_quic_use_identity(ctx, cert, key) &&
              socket_quic_save_identity(identity_path, cert, key);

    X509_NAME_free(name);
    EVP_PKEY_free(key);
    X509_free(cert);
    return ok;
}

static SSL_CTX *socket_quic_server_ctx(const char *identity_path) {
    SSL_CTX *ctx = SSL_CTX_new(OSSL_QUIC_server_method());
    if (ctx == NULL) {
        socket_quic_log_error("server context creation");
        return NULL;
    }

    bool ok;
    if (identity_path == NULL || *identity_path == '\0') {
        ok = false;
    } else if (access(identity_path, F_OK) == 0) {
        ok = socket_quic_load_identity(ctx, identity_path);
        if (!ok) {
            LOG(ERROR, "Failed to load persistent QUIC identity %s", identity_path);
        }
    } else if (errno == ENOENT) {
        ok = socket_quic_generate_identity(ctx, identity_path);
        if (ok) {
            LOG(INFO, "Created persistent QUIC identity %s", identity_path);
        }
    } else {
        ok = false;
    }

    if (!ok) {
        socket_quic_log_error("certificate loading");
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_alpn_select_cb(ctx, socket_quic_select_alpn, NULL);
    return ctx;
}

static bool socket_quic_peer_addr(const struct sockaddr_storage *addr, BIO_ADDR *peer) {
    const struct sockaddr *generic = (const struct sockaddr *)addr;
    if (generic->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)addr;
        return BIO_ADDR_rawmake(peer, AF_INET, &v4->sin_addr, sizeof(v4->sin_addr), v4->sin_port) ==
               1;
    }
#ifdef HAVE_IPV6
    if (generic->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)addr;
        return BIO_ADDR_rawmake(peer,
                                AF_INET6,
                                &v6->sin6_addr,
                                sizeof(v6->sin6_addr),
                                v6->sin6_port) == 1;
    }
#endif
    return false;
}

static bool socket_quic_wait(SSL *ssl) {
    int fd = SSL_get_fd(ssl);
    if (fd == -1) {
        return false;
    }

    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    if (SSL_net_read_desired(ssl)) {
        FD_SET(fd, &read_fds);
    }
    if (SSL_net_write_desired(ssl)) {
        FD_SET(fd, &write_fds);
    }

    /* Keep candidate cancellation and first-success handoff responsive. */
    struct timeval timeout = {0, 20000};
    struct timeval event_timeout;
    int infinite = 1;
    if (SSL_get_event_timeout(ssl, &event_timeout, &infinite) && !infinite &&
        timercmp(&event_timeout, &timeout, <)) {
        timeout = event_timeout;
    }

    int rc = select(fd + 1, &read_fds, &write_fds, NULL, &timeout);
    return rc >= 0 || errno == EINTR;
}

static bool socket_quic_check_fingerprint(SSL *ssl, const char *expected) {
    if (!string_is_hex_fixed(expected, SHA256_DIGEST_LENGTH * 2, false)) {
        LOG(ERROR, "Invalid QUIC certificate fingerprint");
        return false;
    }

    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (cert == NULL) {
        LOG(ERROR, "QUIC peer did not provide a certificate");
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    bool ok = X509_digest(cert, EVP_sha256(), digest, &digest_len) == 1 &&
              digest_len == SHA256_DIGEST_LENGTH;
    X509_free(cert);
    if (!ok) {
        socket_quic_log_error("certificate fingerprint");
        return false;
    }

    char actual[SHA256_DIGEST_LENGTH * 2 + 1];
    string_tohex(digest, digest_len, VS(actual), false);
    if (strcasecmp(actual, expected) != 0) {
        LOG(ERROR, "QUIC certificate fingerprint mismatch (expected %s, got %s)", expected, actual);
        return false;
    }

    return true;
}

socket_t *socket_quic_server_create(const char *host,
                                    uint16_t port,
                                    bool dual_stack,
                                    const char *identity_path) {
    SSL_CTX *ctx = socket_quic_server_ctx(identity_path);
    if (ctx == NULL) {
        return NULL;
    }

    char port_str[6];
    snprintf(VS(port_str), "%" PRIu16, port);
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    int rc = getaddrinfo(host, port_str, &hints, &addresses);
    if (rc != 0) {
        LOG(ERROR, "Cannot resolve QUIC bind address: %s", gai_strerror(rc));
        SSL_CTX_free(ctx);
        return NULL;
    }

    socket_t *sc = xcalloc(1, sizeof(*sc));
    sc->handle = -1;
    sc->owns_handle = true;
    sc->transport = SOCKET_TRANSPORT_QUIC_LISTENER;
    sc->connection_mode = SOCKET_CONNECTION_MODE_QUIC;
    sc->role = SOCKET_ROLE_SERVER;
    sc->port = port;
    sc->host = host != NULL ? xstrdup(host) : NULL;
    sc->quic_ctx = ctx;
    if (!socket_connection_id_generate(sc)) {
        LOG(ERROR, "Failed to generate QUIC listener diagnostic ID");
        socket_destroy(sc);
        freeaddrinfo(addresses);
        return NULL;
    }
    sc->connection_id_final = true;

    for (struct addrinfo *ai = addresses; ai != NULL; ai = ai->ai_next) {
#ifdef HAVE_IPV6
        if (host == NULL && ai->ai_family != AF_INET6) {
            continue;
        }
#endif
        sc->handle = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (sc->handle == -1) {
            continue;
        }
        if (!socket_quic_disable_udp_connreset(sc->handle)) {
            socket_close(sc);
            sc->handle = -1;
            continue;
        }

#ifdef HAVE_IPV6
        if (ai->ai_family == AF_INET6) {
            int v6only = !dual_stack;
            if (setsockopt(sc->handle,
                           IPPROTO_IPV6,
                           IPV6_V6ONLY,
                           (const char *)&v6only,
                           sizeof(v6only)) != 0) {
                socket_close(sc);
                sc->handle = -1;
                continue;
            }
        }
#endif

        int reuse = 1;
        setsockopt(sc->handle, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        if (bind(sc->handle, ai->ai_addr, ai->ai_addrlen) == 0) {
            memcpy(&sc->addr, ai->ai_addr, ai->ai_addrlen);
            break;
        }

        socket_close(sc);
        sc->handle = -1;
    }

    freeaddrinfo(addresses);
    if (sc->handle == -1 || !socket_opt_non_blocking(sc, true)) {
        socket_destroy(sc);
        return NULL;
    }

    sc->quic = SSL_new_listener(ctx, 0);
    BIO *bio = sc->handle != -1 ? BIO_new_dgram(sc->handle, BIO_NOCLOSE) : NULL;
    bool configured = sc->quic != NULL && bio != NULL;
    if (configured) {
        SSL_set_bio(sc->quic, bio, bio);
        bio = NULL;
        configured = SSL_set_blocking_mode(sc->quic, 0) == 1 && SSL_listen(sc->quic) == 1;
    }
    BIO_free(bio);
    if (!configured) {
        socket_quic_log_error("listener creation");
        socket_destroy(sc);
        return NULL;
    }

    return sc;
}

metaserver_publisher_identity_t *socket_quic_publisher_identity(socket_t *sc,
                                                                const char *server_id) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(server_id != NULL);
    if (sc->transport != SOCKET_TRANSPORT_QUIC_LISTENER || sc->quic_ctx == NULL) {
        return NULL;
    }
    X509 *certificate = SSL_CTX_get0_certificate(sc->quic_ctx);
    EVP_PKEY *key = SSL_CTX_get0_privatekey(sc->quic_ctx);
    if (certificate == NULL || key == NULL) {
        return NULL;
    }
    return metaserver_publisher_identity_create(certificate, key, server_id);
}

static socket_t *socket_quic_client_socket_resolved(const char *host,
                                                    uint16_t port,
                                                    const struct sockaddr *address,
                                                    socklen_t address_length) {
    socket_t *sc = xcalloc(1, sizeof(*sc));
    sc->handle = socket(address->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    sc->owns_handle = true;
    sc->transport = SOCKET_TRANSPORT_QUIC_CONNECTION;
    sc->connection_mode = SOCKET_CONNECTION_MODE_QUIC;
    sc->role = SOCKET_ROLE_CLIENT;
    sc->port = port;
    sc->host = xstrdup(host);
    if (sc->handle == -1 || !socket_quic_disable_udp_connreset(sc->handle) ||
        !socket_connection_id_generate(sc) || !socket_opt_non_blocking(sc, true)) {
        socket_destroy(sc);
        return NULL;
    }
    memcpy(&sc->addr, address, address_length);
    return sc;
}

static socket_t *socket_quic_client_socket(const char *host,
                                           uint16_t port,
                                           struct sockaddr_storage *address,
                                           socklen_t *address_length) {
    char port_str[6];
    snprintf(VS(port_str), "%" PRIu16, port);
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;

    int rc = getaddrinfo(host, port_str, &hints, &addresses);
    if (rc != 0) {
        LOG(ERROR, "Cannot resolve the directory QUIC endpoint: %s", gai_strerror(rc));
        return NULL;
    }

    socket_t *sc = NULL;
    for (struct addrinfo *ai = addresses; ai != NULL; ai = ai->ai_next) {
        sc = socket_quic_client_socket_resolved(host, port, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (sc == NULL) {
            continue;
        }
        memcpy(address, ai->ai_addr, ai->ai_addrlen);
        *address_length = (socklen_t)ai->ai_addrlen;
        break;
    }

    freeaddrinfo(addresses);
    return sc;
}

static socket_t *socket_quic_client_socket_unresolved(void) {
    socket_direct_candidate_t local_candidates[SOCKET_DIRECT_MAX_CANDIDATES];
    size_t local_count = socket_local_candidates(1, local_candidates, arraysize(local_candidates));
    int family = AF_INET;
#ifdef HAVE_IPV6
    bool have_ipv4 = false, have_ipv6 = false;
    for (size_t i = 0; i < local_count; i++) {
        if (strchr(local_candidates[i].host, ':') != NULL) {
            have_ipv6 = true;
        } else {
            have_ipv4 = true;
        }
    }
    if (!have_ipv4 && have_ipv6) {
        family = AF_INET6;
    }
#else
    (void)local_count;
#endif
    struct sockaddr_storage local;
    socklen_t local_length;
    memset(&local, 0, sizeof(local));
    if (family == AF_INET) {
        struct sockaddr_in *address4 = (struct sockaddr_in *)&local;
        address4->sin_family = AF_INET;
        address4->sin_addr.s_addr = htonl(INADDR_ANY);
        local_length = sizeof(*address4);
#ifdef HAVE_IPV6
    } else {
        struct sockaddr_in6 *address6 = (struct sockaddr_in6 *)&local;
        address6->sin6_family = AF_INET6;
        address6->sin6_addr = in6addr_any;
        local_length = sizeof(*address6);
#endif
    }
    socket_t *sc =
        socket_quic_client_socket_resolved("", 0, (const struct sockaddr *)&local, local_length);
    if (sc == NULL || bind(sc->handle, (const struct sockaddr *)&local, local_length) != 0) {
        if (sc != NULL) {
            socket_destroy(sc);
        }
        return NULL;
    }
    return sc;
}

typedef bool (*socket_quic_cancelled_t)(void *data);

static bool socket_quic_client_handshake(socket_t *sc,
                                         const struct sockaddr *address,
                                         socklen_t address_length,
                                         const char *certificate_sha256,
                                         const char *host,
                                         double timeout,
                                         uint64_t absolute_deadline_ms,
                                         socket_quic_cancelled_t cancelled,
                                         void *cancel_data) {
    uint64_t now = datetime_monotonic_ms();
    if (now >= absolute_deadline_ms) {
        return false;
    }
    memcpy(&sc->addr, address, address_length);
    if (connect(sc->handle, address, address_length) != 0) {
        return false;
    }

    /* Rendezvous punch datagrams deliberately use the QUIC socket so the NAT
     * sees the same five-tuple. Remove those non-QUIC probes before OpenSSL
     * owns the receive path; otherwise they can abort the QUIC state machine. */
    char probe[2048];
    for (unsigned int drained = 0; drained < SOCKET_PUNCH_DRAIN_MAX; drained++) {
        if (recv(sc->handle, probe, sizeof(probe), 0) <= 0) {
            break;
        }
    }

    if (sc->quic != NULL) {
        SSL_free(sc->quic);
        sc->quic = NULL;
    }
    if (sc->quic_ctx != NULL) {
        SSL_CTX_free(sc->quic_ctx);
        sc->quic_ctx = NULL;
    }

    sc->quic_ctx = SSL_CTX_new(OSSL_QUIC_client_method());
    sc->quic = sc->quic_ctx != NULL ? SSL_new(sc->quic_ctx) : NULL;
    BIO_ADDR *peer = BIO_ADDR_new();
    BIO *bio = sc->handle != -1 ? BIO_new_dgram(sc->handle, BIO_NOCLOSE) : NULL;
    bool configured = sc->quic_ctx != NULL && sc->quic != NULL && peer != NULL && bio != NULL &&
                      socket_quic_peer_addr(&sc->addr, peer);

    if (configured) {
        SSL_CTX_set_verify(sc->quic_ctx, SSL_VERIFY_NONE, NULL);
        SSL_set_bio(sc->quic, bio, bio);
        bio = NULL;
        configured =
            SSL_set_feature_request_uint(sc->quic,
                                         SSL_VALUE_QUIC_IDLE_TIMEOUT,
                                         SOCKET_QUIC_IDLE_TIMEOUT_MS) == 1 &&
            SSL_set_default_stream_mode(sc->quic, SSL_DEFAULT_STREAM_MODE_NONE) == 1 &&
            SSL_set_blocking_mode(sc->quic, 0) == 1 &&
            SSL_set_alpn_protos(sc->quic, socket_quic_alpn, sizeof(socket_quic_alpn)) == 0 &&
            SSL_set1_initial_peer_addr(sc->quic, peer) == 1;
    }

    BIO_free(bio);
    BIO_ADDR_free(peer);
    if (!configured) {
        socket_quic_log_error("client setup");
        return false;
    }

    uint64_t deadline = MIN(now + (uint64_t)(timeout * 1000.0), absolute_deadline_ms);
    for (;;) {
        if (cancelled != NULL && cancelled(cancel_data)) {
            return false;
        }
        int result = SSL_connect(sc->quic);
        if (result == 1) {
            break;
        }

        int ssl_error = SSL_get_error(sc->quic, result);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            socket_quic_log_error("client handshake");
            return false;
        }

        if (datetime_monotonic_ms() >= deadline || !socket_quic_wait(sc->quic)) {
            LOG(DEBUG, "QUIC candidate handshake timed out");
            return false;
        }
    }

    if (datetime_monotonic_ms() >= deadline) {
        LOG(DEBUG, "QUIC candidate handshake exceeded its connection deadline");
        return false;
    }

    if (!socket_quic_check_fingerprint(sc->quic, certificate_sha256)) {
        return false;
    }

    if (!socket_connection_id_export(sc)) {
        socket_quic_log_error("connection diagnostic ID derivation");
        return false;
    }

    return true;
}

static socket_candidate_kind_t
socket_quic_preference_kind(socket_connection_preference_t preference) {
    switch (preference) {
        case SOCKET_CONNECTION_PREFERENCE_LAN:
            return SOCKET_CANDIDATE_LAN;

        case SOCKET_CONNECTION_PREFERENCE_IPV6:
            return SOCKET_CANDIDATE_IPV6;

        case SOCKET_CONNECTION_PREFERENCE_MAPPED:
            return SOCKET_CANDIDATE_MAPPED;

        case SOCKET_CONNECTION_PREFERENCE_SRFLX:
            return SOCKET_CANDIDATE_SRFLX;

        case SOCKET_CONNECTION_PREFERENCE_DIRECTORY:
            return SOCKET_CANDIDATE_DIRECTORY;

        case SOCKET_CONNECTION_PREFERENCE_AUTO:
        case SOCKET_CONNECTION_PREFERENCE_NUM:
        default:
            return SOCKET_CANDIDATE_NUM;
    }
}

typedef struct socket_quic_candidate_race {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    size_t completed;
    size_t task_count;
    size_t best_rank;
    size_t attempted;
    size_t succeeded;
    size_t failed;
    uint64_t total_handshake_ms;
    uint64_t maximum_handshake_ms;
    uint64_t first_success_ms;
    bool cancelled;
} socket_quic_candidate_race_t;

typedef struct socket_quic_candidate_task {
    socket_direct_candidate_t candidates[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    struct sockaddr_storage addresses[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    socklen_t address_lengths[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    size_t ranks[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    size_t candidate_count;
    const char *certificate_sha256;
    uint64_t deadline_ms;
    socket_quic_candidate_race_t *race;
    socket_t *socket;
    socket_t *result;
    socket_direct_candidate_t selected_candidate;
    size_t selected_rank;
    pthread_t thread;
    bool started;
} socket_quic_candidate_task_t;

static bool socket_quic_candidate_cancelled(void *data) {
    socket_quic_candidate_race_t *race = data;
    pthread_mutex_lock(&race->lock);
    bool cancelled = race->cancelled;
    pthread_mutex_unlock(&race->lock);
    return cancelled;
}

static void socket_quic_candidate_finished(socket_quic_candidate_task_t *task) {
    pthread_mutex_lock(&task->race->lock);
    task->race->completed++;
    if (task->result != NULL && task->selected_rank < task->race->best_rank) {
        if (task->race->first_success_ms == 0) {
            task->race->first_success_ms = datetime_monotonic_ms();
        }
        task->race->best_rank = task->selected_rank;
    }
    pthread_cond_broadcast(&task->race->condition);
    pthread_mutex_unlock(&task->race->lock);
}

static void *socket_quic_candidate_thread(void *data) {
    socket_quic_candidate_task_t *task = data;
    for (size_t i = 0; i < task->candidate_count; i++) {
        socket_direct_candidate_t *candidate = &task->candidates[i];
        if (socket_quic_candidate_cancelled(task->race) ||
            datetime_monotonic_ms() >= task->deadline_ms) {
            break;
        }
        if (task->socket == NULL) {
            task->socket =
                socket_quic_client_socket_resolved(candidate->host,
                                                   candidate->port,
                                                   (const struct sockaddr *)&task->addresses[i],
                                                   task->address_lengths[i]);
        }
        if (task->socket == NULL) {
            continue;
        }

        LOG(INFO, "Checking a %s QUIC candidate", socket_candidate_kind_name(candidate->kind));
        uint64_t started_ms = datetime_monotonic_ms();
        bool connected =
            socket_quic_client_handshake(task->socket,
                                         (const struct sockaddr *)&task->addresses[i],
                                         task->address_lengths[i],
                                         task->certificate_sha256,
                                         candidate->host,
                                         socket_candidate_kind_timeout(candidate->kind),
                                         task->deadline_ms,
                                         socket_quic_candidate_cancelled,
                                         task->race);
        uint64_t elapsed_ms = datetime_monotonic_ms() - started_ms;
        pthread_mutex_lock(&task->race->lock);
        task->race->attempted++;
        task->race->total_handshake_ms += elapsed_ms;
        task->race->maximum_handshake_ms = MAX(task->race->maximum_handshake_ms, elapsed_ms);
        if (connected) {
            task->race->succeeded++;
        } else {
            task->race->failed++;
        }
        pthread_mutex_unlock(&task->race->lock);
        if (!connected) {
            LOG(INFO,
                "%s QUIC candidate failed after %" PRIu64 " ms",
                socket_candidate_kind_name(candidate->kind),
                elapsed_ms);
            continue;
        }

        LOG(INFO,
            "%s QUIC candidate succeeded after %" PRIu64 " ms",
            socket_candidate_kind_name(candidate->kind),
            elapsed_ms);
        task->socket->connection_mode = socket_candidate_kind_mode(candidate->kind);
        free(task->socket->host);
        task->socket->host = xstrdup(candidate->host);
        task->socket->port = candidate->port;
        task->result = task->socket;
        task->socket = NULL;
        task->selected_candidate = *candidate;
        task->selected_rank = task->ranks[i];
        break;
    }

    if (task->socket != NULL) {
        socket_destroy(task->socket);
        task->socket = NULL;
    }
    socket_quic_candidate_finished(task);
    return NULL;
}

static bool socket_quic_candidate_address(const socket_direct_candidate_t *candidate,
                                          const struct sockaddr_storage *initial_address,
                                          socklen_t initial_length,
                                          struct sockaddr_storage *address,
                                          socklen_t *address_length) {
    if (candidate->kind == SOCKET_CANDIDATE_DIRECTORY) {
        if (initial_length == 0) {
            return false;
        }
        memcpy(address, initial_address, initial_length);
        *address_length = initial_length;
        return true;
    }

    char service[6];
    snprintf(VS(service), "%" PRIu16, candidate->port);
    struct addrinfo hints, *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo(candidate->host, service, &hints, &addresses);
    if (rc != 0 || addresses == NULL || addresses->ai_addrlen > sizeof(*address)) {
        LOG(ERROR,
            "Invalid numeric %s QUIC rendezvous candidate",
            socket_candidate_kind_name(candidate->kind));
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        return false;
    }

    memcpy(address, addresses->ai_addr, addresses->ai_addrlen);
    *address_length = (socklen_t)addresses->ai_addrlen;
    freeaddrinfo(addresses);
    return true;
}

static bool socket_quic_candidate_is_nat(socket_candidate_kind_t kind) {
    return kind == SOCKET_CANDIDATE_PRFLX || kind == SOCKET_CANDIDATE_MAPPED ||
           kind == SOCKET_CANDIDATE_SRFLX;
}

static void socket_quic_candidate_wait(socket_quic_candidate_race_t *race,
                                       unsigned int timeout_ms) {
    struct timeval now;
    GETTIMEOFDAY(&now);
    uint64_t deadline_ns = (uint64_t)now.tv_usec * 1000 + (uint64_t)timeout_ms * 1000000;
    struct timespec deadline = {.tv_sec = now.tv_sec + (time_t)(deadline_ns / 1000000000),
                                .tv_nsec = (long)(deadline_ns % 1000000000)};
    pthread_cond_timedwait(&race->condition, &race->lock, &deadline);
}

socket_t *socket_quic_client_create(const char *host,
                                    uint16_t port,
                                    const char *certificate_sha256,
                                    const char *rendezvous_url,
                                    const char *stun_endpoint,
                                    const rendezvous_invite_t *invite,
                                    socket_connection_preference_t preference,
                                    socket_connect_failure_t *failure) {
    socket_connect_failure_t local_failure = {0};
    if (failure == NULL) {
        failure = &local_failure;
    }
    failure->code = SOCKET_CONNECT_FAILURE_UNAVAILABLE;
    failure->retry_after_seconds = 0;

    uint64_t started_ms = datetime_monotonic_ms();
    uint64_t deadline_ms = started_ms > UINT64_MAX - SOCKET_CLIENT_ATTEMPT_TIMEOUT_MS
                               ? UINT64_MAX
                               : started_ms + SOCKET_CLIENT_ATTEMPT_TIMEOUT_MS;
    time_t wall_time = time(NULL);
    if (!string_is_hex_fixed(certificate_sha256, RENDEZVOUS_SERVER_ID_HEX_SIZE, true) ||
        wall_time < 0) {
        return NULL;
    }
    uint64_t now = (uint64_t)wall_time;
    if (invite != NULL && rendezvous_invite_expired_at(invite, now)) {
        failure->code = SOCKET_CONNECT_FAILURE_INVITE_EXPIRED;
        return NULL;
    }
    if (invite != NULL && !rendezvous_invite_valid_at(invite, certificate_sha256, now)) {
        failure->code = SOCKET_CONNECT_FAILURE_AUTHORIZATION;
        return NULL;
    }

    unsigned char ticket_bytes[RENDEZVOUS_TICKET_HEX_SIZE / 2U];
    char ticket[RENDEZVOUS_TICKET_HEX_SIZE + 1U];
    socket_rendezvous_attempt_t *attempt = NULL;
    bool ticket_ok = RAND_bytes(ticket_bytes, sizeof(ticket_bytes)) == 1 &&
                     string_tohex(ticket_bytes, sizeof(ticket_bytes), VS(ticket), false) ==
                         RENDEZVOUS_TICKET_HEX_SIZE;
    if (ticket_ok) {
        string_tolower(ticket);
        attempt = socket_rendezvous_attempt_create(certificate_sha256, ticket, invite, deadline_ms);
    }
    OPENSSL_cleanse(ticket_bytes, sizeof(ticket_bytes));
    OPENSSL_cleanse(ticket, sizeof(ticket));
    if (attempt == NULL) {
        return NULL;
    }

    bool has_directory_address = host != NULL && *host != '\0' && port != 0;
    if (!has_directory_address && rendezvous_url == NULL) {
        socket_rendezvous_attempt_destroy(attempt);
        return NULL;
    }

    struct sockaddr_storage initial_address;
    socklen_t initial_length = 0;
    socket_t *sc = has_directory_address
                       ? socket_quic_client_socket(host, port, &initial_address, &initial_length)
                       : socket_quic_client_socket_unresolved();
    if (sc == NULL) {
        socket_rendezvous_attempt_destroy(attempt);
        return NULL;
    }
    if (datetime_monotonic_ms() >= deadline_ms) {
        failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        socket_destroy(sc);
        socket_rendezvous_attempt_destroy(attempt);
        return NULL;
    }
    socket_direct_candidate_t candidates[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    size_t count = 0;
    if (rendezvous_url != NULL) {
        count = socket_rendezvous_client(sc,
                                         rendezvous_url,
                                         stun_endpoint,
                                         attempt,
                                         candidates,
                                         SOCKET_DIRECT_MAX_CANDIDATES + 1,
                                         failure);
        if (count == 0) {
            LOG(ERROR, "Rendezvous signaling returned no direct candidates");
        }
    }
    if (has_directory_address) {
        snprintf(VS(candidates[count].host), "%s", host);
        candidates[count].port = port;
        candidates[count].kind = SOCKET_CANDIDATE_DIRECTORY;
        count++;
    }

    LOG(INFO,
        "Testing %" PRIu64 " direct QUIC candidate%s",
        (uint64_t)count,
        count == 1 ? "" : "s");
    static const socket_candidate_kind_t priorities[] = {
        SOCKET_CANDIDATE_LAN,
        SOCKET_CANDIDATE_IPV6,
        SOCKET_CANDIDATE_PRFLX,
        SOCKET_CANDIDATE_MAPPED,
        SOCKET_CANDIDATE_SRFLX,
        SOCKET_CANDIDATE_DIRECTORY,
    };
    socket_candidate_kind_t preferred_kind = socket_quic_preference_kind(preference);
    if (preference > SOCKET_CONNECTION_PREFERENCE_AUTO &&
        preference < SOCKET_CONNECTION_PREFERENCE_NUM) {
        LOG(INFO,
            "Prioritizing requested QUIC route type: %s",
            socket_connection_preference_name(preference));
    }

    socket_candidate_kind_t ordered_priorities[arraysize(priorities)];
    size_t priority_count = 0;
    if (preferred_kind == SOCKET_CANDIDATE_LAN || preferred_kind == SOCKET_CANDIDATE_IPV6) {
        ordered_priorities[priority_count++] = preferred_kind;
    }
    for (size_t i = 0; i < 2; i++) {
        if (priorities[i] != preferred_kind) {
            ordered_priorities[priority_count++] = priorities[i];
        }
    }
    if (preferred_kind >= SOCKET_CANDIDATE_PRFLX && preferred_kind < SOCKET_CANDIDATE_NUM) {
        ordered_priorities[priority_count++] = preferred_kind;
    }
    for (size_t i = 2; i < arraysize(priorities); i++) {
        if (priorities[i] != preferred_kind) {
            ordered_priorities[priority_count++] = priorities[i];
        }
    }

    socket_direct_candidate_t ordered_candidates[SOCKET_DIRECT_MAX_CANDIDATES + 2];
    size_t ordered_count = 0;
    for (size_t priority = 0; priority < priority_count; priority++) {
        for (size_t i = 0; i < count; i++) {
            if (candidates[i].kind != ordered_priorities[priority]) {
                continue;
            }
            ordered_candidates[ordered_count++] = candidates[i];
        }
    }

    socket_quic_candidate_task_t tasks[SOCKET_DIRECT_MAX_CANDIDATES + 2] = {0};
    size_t task_count = 0;
    size_t nat_task = SIZE_MAX;
    for (size_t rank = 0; rank < ordered_count; rank++) {
        socket_direct_candidate_t *candidate = &ordered_candidates[rank];
        struct sockaddr_storage address;
        socklen_t address_length;
        if (!socket_quic_candidate_address(candidate,
                                           &initial_address,
                                           initial_length,
                                           &address,
                                           &address_length)) {
            continue;
        }

        size_t task_index;
        if (socket_quic_candidate_is_nat(candidate->kind)) {
            if (nat_task == SIZE_MAX) {
                nat_task = task_count++;
            }
            task_index = nat_task;
        } else {
            task_index = task_count++;
        }
        socket_quic_candidate_task_t *task = &tasks[task_index];
        size_t slot = task->candidate_count++;
        task->candidates[slot] = *candidate;
        task->addresses[slot] = address;
        task->address_lengths[slot] = address_length;
        task->ranks[slot] = rank;
        task->certificate_sha256 = certificate_sha256;
        task->deadline_ms = deadline_ms;
    }
    if (task_count == 0) {
        socket_destroy(sc);
        socket_rendezvous_attempt_destroy(attempt);
        if (datetime_monotonic_ms() >= deadline_ms) {
            failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
            failure->retry_after_seconds = 0;
        } else if (failure->code == SOCKET_CONNECT_FAILURE_NONE ||
                   failure->code == SOCKET_CONNECT_FAILURE_UNAVAILABLE) {
            failure->code = SOCKET_CONNECT_FAILURE_SERVER_OFFLINE;
        }
        return NULL;
    }

    /* All NAT-derived candidates are serialized through the exact socket used
     * for STUN and punching. This preserves the externally mapped source port
     * while LAN, global IPv6 and directory candidates race independently. */
    size_t initial_socket_task = nat_task;
    if (initial_socket_task == SIZE_MAX && has_directory_address) {
        for (size_t i = 0; i < task_count; i++) {
            if (tasks[i].candidates[0].kind == SOCKET_CANDIDATE_DIRECTORY) {
                initial_socket_task = i;
                break;
            }
        }
    }
    if (initial_socket_task == SIZE_MAX) {
        int socket_family = ((struct sockaddr *)&sc->addr)->sa_family;
        for (size_t i = 0; i < task_count; i++) {
            if (((struct sockaddr *)&tasks[i].addresses[0])->sa_family == socket_family) {
                initial_socket_task = i;
                break;
            }
        }
    }
    if (initial_socket_task != SIZE_MAX) {
        tasks[initial_socket_task].socket = sc;
    } else {
        socket_destroy(sc);
    }

    socket_quic_candidate_race_t race = {.task_count = task_count, .best_rank = SIZE_MAX};
    pthread_mutex_init(&race.lock, NULL);
    pthread_cond_init(&race.condition, NULL);
    for (size_t i = 0; i < task_count; i++) {
        tasks[i].race = &race;
        tasks[i].started =
            pthread_create(&tasks[i].thread, NULL, socket_quic_candidate_thread, &tasks[i]) == 0;
        if (!tasks[i].started) {
            socket_quic_candidate_thread(&tasks[i]);
        }
    }

    /* Publish the first authenticated route immediately, with a brief window
     * for an already-running, higher-priority candidate to finish. */
    pthread_mutex_lock(&race.lock);
    while (race.completed < race.task_count) {
        uint64_t now = datetime_monotonic_ms();
        if (now >= deadline_ms || race.best_rank == 0 ||
            (race.best_rank != SIZE_MAX && now - race.first_success_ms >= 150)) {
            break;
        }
        socket_quic_candidate_wait(&race, (unsigned int)MIN(deadline_ms - now, 20U));
    }
    if (race.best_rank != SIZE_MAX || datetime_monotonic_ms() >= deadline_ms) {
        race.cancelled = true;
        pthread_cond_broadcast(&race.condition);
    }
    pthread_mutex_unlock(&race.lock);

    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i].started) {
            pthread_join(tasks[i].thread, NULL);
        }
    }

    socket_t *selected = NULL;
    size_t selected_index = SIZE_MAX;
    size_t selected_rank = SIZE_MAX;
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i].result != NULL && tasks[i].selected_rank < selected_rank) {
            if (selected != NULL) {
                socket_destroy(selected);
            }
            selected = tasks[i].result;
            selected_index = i;
            selected_rank = tasks[i].selected_rank;
        } else if (tasks[i].result != NULL) {
            socket_destroy(tasks[i].result);
        }
    }

    LOG(SYSTEM,
        "QUIC candidate race: attempted=%" PRIu64 " succeeded=%" PRIu64 " failed=%" PRIu64
        " total_handshake_ms=%" PRIu64 " maximum_handshake_ms=%" PRIu64,
        (uint64_t)race.attempted,
        (uint64_t)race.succeeded,
        (uint64_t)race.failed,
        race.total_handshake_ms,
        race.maximum_handshake_ms);
    pthread_cond_destroy(&race.condition);
    pthread_mutex_destroy(&race.lock);
    socket_rendezvous_attempt_destroy(attempt);
    if (selected != NULL) {
        failure->code = SOCKET_CONNECT_FAILURE_NONE;
        failure->retry_after_seconds = 0;
        LOG(SYSTEM,
            "Connection %s selected a %s direct QUIC route",
            socket_get_id(selected),
            socket_candidate_kind_name(tasks[selected_index].selected_candidate.kind));
        return selected;
    }

    LOG(ERROR,
        "No confirmed direct route to the server; game traffic relay is "
        "disabled");
    if (datetime_monotonic_ms() >= deadline_ms) {
        failure->code = SOCKET_CONNECT_FAILURE_TIMEOUT;
        failure->retry_after_seconds = 0;
    } else if (failure->code == SOCKET_CONNECT_FAILURE_NONE ||
               failure->code == SOCKET_CONNECT_FAILURE_UNAVAILABLE) {
        failure->code = SOCKET_CONNECT_FAILURE_SERVER_OFFLINE;
    }
    return NULL;
}

bool socket_certificate_sha256(socket_t *sc, char fingerprint[65]) {
    HARD_ASSERT(sc != NULL);
    HARD_ASSERT(fingerprint != NULL);

    X509 *cert = NULL;
    if (sc->transport == SOCKET_TRANSPORT_QUIC_LISTENER) {
        cert = SSL_CTX_get0_certificate(sc->quic_ctx);
    } else if (sc->transport == SOCKET_TRANSPORT_QUIC_CONNECTION) {
        cert = SSL_get0_peer_certificate(sc->quic);
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (cert == NULL || X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1 ||
        digest_len != SHA256_DIGEST_LENGTH) {
        return false;
    }

    if (string_tohex(digest, digest_len, fingerprint, 65, false) != 64) {
        return false;
    }

    string_tolower(fingerprint);
    return true;
}

static void socket_quic_schedule_event(socket_t *sc) {
    struct timeval timeout;
    int infinite = 1;
    if (!SSL_get_event_timeout(sc->quic, &timeout, &infinite) || infinite) {
        sc->quic_event_deadline_ms = UINT64_MAX;
        return;
    }
    uint64_t delay = (uint64_t)timeout.tv_sec * 1000 + (uint64_t)timeout.tv_usec / 1000;
    sc->quic_event_deadline_ms = datetime_monotonic_ms() + delay;
}

static bool socket_quic_buffered_pending(socket_t *sc) {
    return sc->quic != NULL &&
           (SSL_has_pending(sc->quic) != 0 ||
            (sc->game_stream != NULL && SSL_has_pending(sc->game_stream->ssl) != 0));
}

static bool socket_quic_application_pending(socket_t *sc) {
    return socket_quic_buffered_pending(sc) ||
           (sc->quic != NULL && SSL_get_accept_stream_queue_len(sc->quic) != 0);
}

unsigned int socket_quic_timeout(socket_t *sc, unsigned int maximum_ms) {
    HARD_ASSERT(sc != NULL);
    if (sc->transport != SOCKET_TRANSPORT_QUIC_CONNECTION) {
        return maximum_ms;
    }
    if (socket_quic_buffered_pending(sc)) {
        return 0;
    }
    if (sc->quic_event_deadline_ms == UINT64_MAX) {
        return maximum_ms;
    }
    uint64_t now = datetime_monotonic_ms();
    if (sc->quic_event_deadline_ms == 0 || sc->quic_event_deadline_ms <= now) {
        return 0;
    }
    uint64_t delay = sc->quic_event_deadline_ms - now;
    return delay < maximum_ms ? (unsigned int)delay : maximum_ms;
}

bool socket_quic_timer_due(socket_t *sc) {
    HARD_ASSERT(sc != NULL);
    if (sc->transport != SOCKET_TRANSPORT_QUIC_CONNECTION ||
        sc->quic_event_deadline_ms == UINT64_MAX) {
        return false;
    }
    return sc->quic_event_deadline_ms == 0 || datetime_monotonic_ms() >= sc->quic_event_deadline_ms;
}

bool socket_quic_service(socket_t *sc, bool network_ready, bool app_write_pending) {
    HARD_ASSERT(sc != NULL);
    if (sc->transport != SOCKET_TRANSPORT_QUIC_CONNECTION) {
        return network_ready || app_write_pending;
    }

    uint64_t now = datetime_monotonic_ms();
    bool buffered = socket_quic_application_pending(sc);
    bool timer_due = sc->quic_event_deadline_ms == 0 || (sc->quic_event_deadline_ms != UINT64_MAX &&
                                                         now >= sc->quic_event_deadline_ms);
    if (!network_ready && !buffered && !app_write_pending && !timer_due) {
        return false;
    }

    if (SSL_handle_events(sc->quic) != 1) {
        socket_quic_log_error("event handling");
        return true;
    }
    socket_quic_schedule_event(sc);
    /* A timer can transition the connection to its terminal state without
     * producing readable application data. Give the caller one I/O pass so
     * SSL_read_ex() observes and reports that closure immediately. */
    return network_ready || buffered || app_write_pending || timer_due ||
           SSL_has_pending(sc->quic) != 0 ||
           (sc->game_stream != NULL && SSL_has_pending(sc->game_stream->ssl) != 0) ||
           SSL_get_accept_stream_queue_len(sc->quic) != 0;
}

#else

unsigned int socket_quic_timeout(socket_t *sc, unsigned int maximum_ms) {
    (void)sc;
    return maximum_ms;
}

socket_t *socket_quic_server_create(const char *host,
                                    uint16_t port,
                                    bool dual_stack,
                                    const char *identity_path) {
    LOG(ERROR, "QUIC requires OpenSSL 3.5 or newer");
    return NULL;
}

socket_t *socket_quic_client_create(const char *host,
                                    uint16_t port,
                                    const char *certificate_sha256,
                                    const char *rendezvous_url,
                                    const char *stun_endpoint,
                                    const rendezvous_invite_t *invite,
                                    socket_connection_preference_t preference,
                                    socket_connect_failure_t *failure) {
    if (failure != NULL) {
        failure->code = SOCKET_CONNECT_FAILURE_PROTOCOL_REVISION;
        failure->retry_after_seconds = 0;
    }
    (void)invite;
    (void)preference;
    LOG(ERROR, "QUIC requires OpenSSL 3.5 or newer");
    return NULL;
}

bool socket_certificate_sha256(socket_t *sc, char fingerprint[65]) {
    return false;
}

metaserver_publisher_identity_t *socket_quic_publisher_identity(socket_t *sc,
                                                                const char *server_id) {
    (void)sc;
    (void)server_id;
    return NULL;
}

bool socket_quic_service(socket_t *sc, bool network_ready, bool app_write_pending) {
    return network_ready || app_write_pending;
}

bool socket_quic_timer_due(socket_t *sc) {
    (void)sc;
    return false;
}

#endif

const char *socket_connection_preference_name(socket_connection_preference_t preference) {
    static const char *const names[SOCKET_CONNECTION_PREFERENCE_NUM] = {
        "Automatic",
        "Local network (LAN)",
        "Native IPv6",
        "Router port mapping",
        "NAT traversal (STUN)",
        "Directory address",
    };

    if ((unsigned int)preference >= SOCKET_CONNECTION_PREFERENCE_NUM) {
        return names[SOCKET_CONNECTION_PREFERENCE_AUTO];
    }
    return names[preference];
}

bool socket_is_quic(socket_t *sc) {
    HARD_ASSERT(sc != NULL);
    return sc->transport != SOCKET_TRANSPORT_TCP;
}

socket_connection_mode_t socket_connection_mode_get(socket_t *sc) {
    HARD_ASSERT(sc != NULL);
    return sc->connection_mode;
}

const char *socket_connection_mode_name(socket_connection_mode_t mode) {
    static const char *const names[SOCKET_CONNECTION_MODE_NUM] =
        {"QUIC", "QUIC/LAN", "QUIC/IPv6", "QUIC/mapped", "QUIC/STUN", "QUIC/directory"};

    if (mode == SOCKET_CONNECTION_MODE_INTERNAL) {
        return "internal";
    }
    if ((unsigned int)mode >= SOCKET_CONNECTION_MODE_NUM) {
        return "unknown";
    }
    return names[mode];
}
