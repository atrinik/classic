/**
 * @file
 * Low-overhead server network and game-loop observability.
 */

#ifndef NETWORK_METRICS_H
#define NETWORK_METRICS_H

#include <toolkit/toolkit.h>

typedef enum server_transport_wake_reason {
    SERVER_TRANSPORT_WAKE_SIMULATION,
    SERVER_TRANSPORT_WAKE_LISTENER,
    SERVER_TRANSPORT_WAKE_CONNECTION,
    SERVER_TRANSPORT_WAKE_QUIC_TIMER,
    SERVER_TRANSPORT_WAKE_APPLICATION,
    SERVER_TRANSPORT_WAKE_ERROR,
    SERVER_TRANSPORT_WAKE_REASON_COUNT,
} server_transport_wake_reason_t;

void server_metrics_connection_accepted(size_t pending);
void server_metrics_connection_rejected(size_t pending);
void server_metrics_pending_changed(size_t pending);
void server_metrics_quic_service(bool network_ready);
void server_metrics_queue_changed(int64_t delta, size_t connection_bytes, bool rejected);
void server_metrics_asset_cache(size_t bytes);
void server_metrics_asset_response(uint64_t latency_us);
void server_metrics_asset_paced(void);
void server_metrics_asset_stream(int active_delta, size_t bytes, bool rejected);
void server_metrics_mapping(const char *method, bool open_failed, bool renewal_failed);
void server_metrics_transport_wait(uint64_t wait_us,
                                   server_transport_wake_reason_t reason,
                                   size_t ready_connections,
                                   size_t quic_timer_services,
                                   uint64_t pending_queue_age_us,
                                   bool work_limited);
void server_metrics_transport_service(uint64_t service_us);
void server_metrics_keepalive_echo(uint64_t latency_us);
void server_metrics_game_loop(uint64_t duration_us);
void server_metrics_stats(char *buffer, size_t size);

#endif
