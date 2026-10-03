#ifndef LIVE_MOVEMENT_ROUTE_H
#define LIVE_MOVEMENT_ROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_MOVEMENT_ROUTE_MAP_MAX 511U
#define LIVE_MOVEMENT_ROUTE_STEPS_MAX 50000U
#define LIVE_MOVEMENT_ROUTE_FILE_MAX (8U * 1024U * 1024U)

typedef struct live_movement_route live_movement_route_t;
typedef struct live_movement_route_state live_movement_route_state_t;

typedef enum live_movement_route_action_type {
    LIVE_MOVEMENT_ROUTE_ACTION_NONE,
    LIVE_MOVEMENT_ROUTE_ACTION_MOVE,
    LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL,
    LIVE_MOVEMENT_ROUTE_ACTION_FAILED,
    LIVE_MOVEMENT_ROUTE_ACTION_DONE,
} live_movement_route_action_type_t;

typedef struct live_movement_route_observation {
    uint64_t now_ms;
    bool connected;
    bool play;
    bool published_ready;
    const char *map;
    uint8_t x;
    uint8_t y;
    uint64_t publication_generation;
    bool run_on;
    bool fire_on;
} live_movement_route_observation_t;

typedef struct live_movement_route_action {
    live_movement_route_action_type_t type;
    /** Reached checkpoint for ARRIVAL, destination checkpoint for MOVE. */
    size_t checkpoint_index;
    uint8_t direction;
} live_movement_route_action_t;

/** Load a bounded, closed live-movement-route XML file. */
bool live_movement_route_load(const char *path,
                              live_movement_route_t **route,
                              char *error,
                              size_t error_size);

void live_movement_route_free(live_movement_route_t *route);

size_t live_movement_route_checkpoint_count(const live_movement_route_t *route);
uint64_t live_movement_route_timeout_ms(const live_movement_route_t *route);
uint64_t live_movement_route_step_timeout_ms(const live_movement_route_t *route);
/** Lowercase SHA-256 of the exact loaded file bytes. Owned by route. */
const char *live_movement_route_sha256(const live_movement_route_t *route);

/**
 * Create pure execution state. start_ms starts the total deadline before login.
 * The route must outlive the returned state. Neither object is thread-safe.
 */
live_movement_route_state_t *
live_movement_route_state_create(const live_movement_route_t *route, uint64_t start_ms);

void live_movement_route_state_free(live_movement_route_state_t *state);

/**
 * Advance the route from one immutable client observation. FAILED and DONE are
 * terminal; the adapter owns any post-DONE drain checks.
 */
live_movement_route_action_t
live_movement_route_tick(live_movement_route_state_t *state,
                         const live_movement_route_observation_t *observation);

/** Release the current arrival only after its frame was presented. */
bool live_movement_route_arrival_presented(live_movement_route_state_t *state);

/** Stable terminal failure text, or NULL before failure. Owned by state. */
const char *live_movement_route_failure(const live_movement_route_state_t *state);

#endif
