/*
 * Atrinik pathfinding core
 * Copyright (C) 2026 Atrinik Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef ATRINIK_PATHFINDING_H
#define ATRINIK_PATHFINDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque state identifier interpreted only by the adapter. */
typedef uint64_t atrinik_pf_state_id;

/** Adapter-owned value copied into the reconstructed path. */
typedef uint64_t atrinik_pf_transition_data;

/** A directed transition supplied by the adapter. */
typedef struct atrinik_pf_transition {
    atrinik_pf_state_id state;
    uint64_t cost;
    atrinik_pf_transition_data data;
} atrinik_pf_transition;

/** Search strategy. */
typedef enum atrinik_pf_algorithm {
    ATRINIK_PF_ASTAR,
    ATRINIK_PF_DIJKSTRA,
    ATRINIK_PF_BREADTH_FIRST,
    ATRINIK_PF_GREEDY_BEST_FIRST
} atrinik_pf_algorithm;

/** Public result status. */
typedef enum atrinik_pf_status {
    ATRINIK_PF_FOUND,
    ATRINIK_PF_NO_PATH,
    ATRINIK_PF_LIMIT_REACHED,
    ATRINIK_PF_CANCELLED,
    ATRINIK_PF_PARTIAL,
    ATRINIK_PF_COMPLETE,
    ATRINIK_PF_INVALID_INPUT,
    ATRINIK_PF_ADAPTER_ERROR,
    ATRINIK_PF_OUT_OF_MEMORY,
    ATRINIK_PF_COST_OVERFLOW
} atrinik_pf_status;

/** Underlying reason a search stopped, retained for partial results. */
typedef enum atrinik_pf_termination {
    ATRINIK_PF_TERMINATION_GOAL,
    ATRINIK_PF_TERMINATION_EXHAUSTED,
    ATRINIK_PF_TERMINATION_LIMIT,
    ATRINIK_PF_TERMINATION_CANCELLED,
    ATRINIK_PF_TERMINATION_INVALID_INPUT,
    ATRINIK_PF_TERMINATION_ADAPTER_ERROR,
    ATRINIK_PF_TERMINATION_OUT_OF_MEMORY,
    ATRINIK_PF_TERMINATION_COST_OVERFLOW
} atrinik_pf_termination;

/** Search counters. Generated counts unique states, including the start. */
typedef struct atrinik_pf_metrics {
    size_t expanded;
    size_t generated;
    size_t examined_transitions;
    size_t peak_frontier;
    uint64_t total_cost;
} atrinik_pf_metrics;

/** One reconstructed path step. The first step has data set to zero. */
typedef struct atrinik_pf_step {
    atrinik_pf_state_id state;
    atrinik_pf_transition_data data;
} atrinik_pf_step;

/** Result of a route search. Storage belongs to the search context. */
typedef struct atrinik_pf_result {
    atrinik_pf_status status;
    atrinik_pf_termination termination;
    const atrinik_pf_step *steps;
    size_t step_count;
    atrinik_pf_metrics metrics;
} atrinik_pf_result;

/** Result of a reachability traversal. Storage belongs to the context. */
typedef struct atrinik_pf_reachability_result {
    atrinik_pf_status status;
    atrinik_pf_termination termination;
    const atrinik_pf_state_id *states;
    size_t state_count;
    atrinik_pf_metrics metrics;
} atrinik_pf_reachability_result;

typedef struct atrinik_pf_context atrinik_pf_context;

/** Receives one transition. False asks the adapter to stop enumerating. */
typedef bool (*atrinik_pf_emit_fn)(void *emit_context, const atrinik_pf_transition *transition);

/**
 * Enumerates transitions in deterministic adapter-defined order.
 *
 * Return false only for an adapter failure or after emit returned false.
 */
typedef bool (*atrinik_pf_neighbors_fn)(void *adapter_context,
                                        atrinik_pf_state_id state,
                                        atrinik_pf_emit_fn emit,
                                        void *emit_context);

typedef bool (*atrinik_pf_goal_fn)(void *adapter_context, atrinik_pf_state_id state);
typedef uint64_t (*atrinik_pf_score_fn)(void *adapter_context, atrinik_pf_state_id state);
typedef bool (*atrinik_pf_cancel_fn)(void *adapter_context);

/** Adapter contract. Only neighbors is required for reachability. */
typedef struct atrinik_pf_adapter {
    void *context;
    atrinik_pf_neighbors_fn neighbors;
    atrinik_pf_goal_fn goal;
    atrinik_pf_score_fn heuristic;
    atrinik_pf_score_fn partial_rank;
    atrinik_pf_cancel_fn cancelled;
} atrinik_pf_adapter;

/** Per-search budgets. Zero means unlimited. */
typedef struct atrinik_pf_options {
    atrinik_pf_algorithm algorithm;
    size_t max_expanded;
    size_t max_generated;
    size_t max_transitions;
    size_t max_frontier;
    bool return_partial;
} atrinik_pf_options;

/** Fill options with A* and unlimited budgets. */
void atrinik_pf_options_init(atrinik_pf_options *options);

/** Create an isolated, reusable search context. */
atrinik_pf_context *atrinik_pf_context_create(void);

/** Destroy a context and all result storage it owns. */
void atrinik_pf_context_destroy(atrinik_pf_context *context);

/**
 * Find a route. The returned storage remains valid until the next operation on
 * this context or until it is destroyed. A context must not be used
 * concurrently, but independent contexts are safe to use concurrently.
 *
 * A* requires a heuristic; optimality requires that heuristic to be
 * admissible. Explicit partial results additionally require a partial_rank
 * callback; lower rank is better. Breadth-first search treats all transitions
 * as unit cost. Dijkstra ignores the heuristic. Greedy best-first requires a
 * heuristic and uses it without accumulated cost.
 */
atrinik_pf_result atrinik_pf_search(atrinik_pf_context *context,
                                    const atrinik_pf_adapter *adapter,
                                    atrinik_pf_state_id start,
                                    const atrinik_pf_options *options);

/**
 * Discover the start state's reachable component in deterministic breadth-
 * first order. Goal, heuristic, partial_rank, and transition costs are ignored.
 */
atrinik_pf_reachability_result atrinik_pf_reachable(atrinik_pf_context *context,
                                                    const atrinik_pf_adapter *adapter,
                                                    atrinik_pf_state_id start,
                                                    const atrinik_pf_options *options);

/** Return a stable, human-readable status name. */
const char *atrinik_pf_status_string(atrinik_pf_status status);

#ifdef __cplusplus
}
#endif

#endif
