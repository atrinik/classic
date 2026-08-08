/*
 * Atrinik pathfinding core
 * Copyright (C) 2026 Atrinik Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <atrinik/pathfinding.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PF_NONE SIZE_MAX
#define PF_INITIAL_CAPACITY 32U

typedef struct pf_node {
    atrinik_pf_state_id state;
    uint64_t cost;
    uint64_t heuristic;
    uint64_t priority;
    atrinik_pf_transition_data transition_data;
    size_t parent;
    size_t heap_index;
    size_t sequence;
    bool closed;
} pf_node;

struct atrinik_pf_context {
    pf_node *nodes;
    size_t node_count;
    size_t node_capacity;
    size_t *hash;
    size_t hash_capacity;
    size_t *heap;
    size_t heap_count;
    size_t heap_capacity;
    atrinik_pf_step *path;
    size_t path_capacity;
    atrinik_pf_state_id *reachable;
    size_t reachable_capacity;

    const atrinik_pf_adapter *adapter;
    atrinik_pf_options options;
    atrinik_pf_metrics metrics;
    atrinik_pf_status stop_status;
    atrinik_pf_termination stop_termination;
    bool stopped;
    bool reachability;
    size_t active_node;
    size_t partial_node;
    uint64_t partial_rank;
};

static bool pf_multiply_size(size_t left, size_t right, size_t *result) {
    if (left != 0U && right > SIZE_MAX / left) {
        return false;
    }

    *result = left * right;
    return true;
}

static bool pf_grow_array(void **array, size_t element_size, size_t *capacity, size_t minimum) {
    size_t bytes;
    size_t next = *capacity == 0U ? PF_INITIAL_CAPACITY : *capacity;

    while (next < minimum) {
        if (next > SIZE_MAX / 2U) {
            next = minimum;
            break;
        }
        next *= 2U;
    }
    if (!pf_multiply_size(next, element_size, &bytes)) {
        return false;
    }

    void *replacement = realloc(*array, bytes);
    if (replacement == NULL) {
        return false;
    }

    *array = replacement;
    *capacity = next;
    return true;
}

static uint64_t pf_hash_state(atrinik_pf_state_id state) {
    uint64_t value = state;

    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value;
}

static size_t pf_hash_slot(const atrinik_pf_context *context, atrinik_pf_state_id state) {
    size_t slot = (size_t)(pf_hash_state(state) & (uint64_t)(context->hash_capacity - 1U));

    while (context->hash[slot] != 0U && context->nodes[context->hash[slot] - 1U].state != state) {
        slot = (slot + 1U) & (context->hash_capacity - 1U);
    }
    return slot;
}

static bool pf_rebuild_hash(atrinik_pf_context *context, size_t capacity) {
    size_t bytes;
    size_t *old_hash = context->hash;

    if (capacity < 2U || (capacity & (capacity - 1U)) != 0U ||
        !pf_multiply_size(capacity, sizeof(*context->hash), &bytes)) {
        return false;
    }
    context->hash = calloc(1U, bytes);
    if (context->hash == NULL) {
        context->hash = old_hash;
        return false;
    }
    context->hash_capacity = capacity;

    for (size_t i = 0U; i < context->node_count; i++) {
        context->hash[pf_hash_slot(context, context->nodes[i].state)] = i + 1U;
    }
    free(old_hash);
    return true;
}

static bool pf_prepare_hash(atrinik_pf_context *context) {
    if (context->hash_capacity == 0U) {
        return pf_rebuild_hash(context, 64U);
    }
    if (context->node_count + 1U <= context->hash_capacity / 2U) {
        return true;
    }
    if (context->hash_capacity > SIZE_MAX / 2U) {
        return false;
    }
    return pf_rebuild_hash(context, context->hash_capacity * 2U);
}

static size_t pf_find_node(const atrinik_pf_context *context, atrinik_pf_state_id state) {
    size_t slot;

    if (context->hash_capacity == 0U) {
        return PF_NONE;
    }
    slot = pf_hash_slot(context, state);
    return context->hash[slot] == 0U ? PF_NONE : context->hash[slot] - 1U;
}

static bool pf_add_node(atrinik_pf_context *context, const pf_node *node, size_t *index) {
    if (!pf_prepare_hash(context)) {
        return false;
    }
    if (context->node_count == context->node_capacity && !pf_grow_array((void **)&context->nodes,
                                                                        sizeof(*context->nodes),
                                                                        &context->node_capacity,
                                                                        context->node_count + 1U)) {
        return false;
    }

    *index = context->node_count++;
    context->nodes[*index] = *node;
    context->hash[pf_hash_slot(context, node->state)] = *index + 1U;
    return true;
}

static bool pf_node_before(const atrinik_pf_context *context, size_t left, size_t right) {
    const pf_node *lhs = &context->nodes[left];
    const pf_node *rhs = &context->nodes[right];

    if (lhs->priority != rhs->priority) {
        return lhs->priority < rhs->priority;
    }
    if (lhs->heuristic != rhs->heuristic) {
        return lhs->heuristic < rhs->heuristic;
    }
    return lhs->sequence < rhs->sequence;
}

static void pf_heap_swap(atrinik_pf_context *context, size_t left, size_t right) {
    size_t temporary = context->heap[left];

    context->heap[left] = context->heap[right];
    context->heap[right] = temporary;
    context->nodes[context->heap[left]].heap_index = left;
    context->nodes[context->heap[right]].heap_index = right;
}

static void pf_heap_up(atrinik_pf_context *context, size_t position) {
    while (position != 0U) {
        size_t parent = (position - 1U) / 2U;

        if (!pf_node_before(context, context->heap[position], context->heap[parent])) {
            break;
        }
        pf_heap_swap(context, position, parent);
        position = parent;
    }
}

static void pf_heap_down(atrinik_pf_context *context, size_t position) {
    for (;;) {
        size_t left = position * 2U + 1U;
        size_t best = position;

        if (left < context->heap_count &&
            pf_node_before(context, context->heap[left], context->heap[best])) {
            best = left;
        }
        if (left + 1U < context->heap_count &&
            pf_node_before(context, context->heap[left + 1U], context->heap[best])) {
            best = left + 1U;
        }
        if (best == position) {
            break;
        }
        pf_heap_swap(context, position, best);
        position = best;
    }
}

static bool pf_heap_push(atrinik_pf_context *context, size_t node) {
    if (context->heap_count == context->heap_capacity && !pf_grow_array((void **)&context->heap,
                                                                        sizeof(*context->heap),
                                                                        &context->heap_capacity,
                                                                        context->heap_count + 1U)) {
        return false;
    }

    context->heap[context->heap_count] = node;
    context->nodes[node].heap_index = context->heap_count;
    context->heap_count++;
    pf_heap_up(context, context->heap_count - 1U);
    if (context->heap_count > context->metrics.peak_frontier) {
        context->metrics.peak_frontier = context->heap_count;
    }
    return true;
}

static size_t pf_heap_pop(atrinik_pf_context *context) {
    size_t node = context->heap[0];

    context->heap_count--;
    context->nodes[node].heap_index = PF_NONE;
    if (context->heap_count != 0U) {
        context->heap[0] = context->heap[context->heap_count];
        context->nodes[context->heap[0]].heap_index = 0U;
        pf_heap_down(context, 0U);
    }
    return node;
}

static void
pf_stop(atrinik_pf_context *context, atrinik_pf_status status, atrinik_pf_termination termination) {
    context->stopped = true;
    context->stop_status = status;
    context->stop_termination = termination;
}

static void pf_reset(atrinik_pf_context *context,
                     const atrinik_pf_adapter *adapter,
                     const atrinik_pf_options *options) {
    context->node_count = 0U;
    context->heap_count = 0U;
    context->adapter = adapter;
    context->options = *options;
    memset(&context->metrics, 0, sizeof(context->metrics));
    if (context->hash != NULL) {
        memset(context->hash, 0, context->hash_capacity * sizeof(*context->hash));
    }
    context->stop_status = ATRINIK_PF_INVALID_INPUT;
    context->stop_termination = ATRINIK_PF_TERMINATION_INVALID_INPUT;
    context->stopped = false;
    context->reachability = false;
    context->active_node = PF_NONE;
    context->partial_node = PF_NONE;
    context->partial_rank = UINT64_MAX;
}

static uint64_t pf_priority(atrinik_pf_algorithm algorithm, uint64_t cost, uint64_t heuristic) {
    if (algorithm == ATRINIK_PF_GREEDY_BEST_FIRST) {
        return heuristic;
    }
    if (algorithm == ATRINIK_PF_ASTAR && UINT64_MAX - cost < heuristic) {
        return UINT64_MAX;
    }
    return algorithm == ATRINIK_PF_ASTAR ? cost + heuristic : cost;
}

static void pf_consider_partial(atrinik_pf_context *context, size_t node) {
    uint64_t rank;
    pf_node *candidate;
    pf_node *current;

    if (!context->options.return_partial) {
        return;
    }
    candidate = &context->nodes[node];
    rank = context->adapter->partial_rank(context->adapter->context, candidate->state);
    if (context->partial_node == PF_NONE) {
        context->partial_node = node;
        context->partial_rank = rank;
        return;
    }

    current = &context->nodes[context->partial_node];
    if (rank < context->partial_rank ||
        (rank == context->partial_rank && candidate->cost < current->cost) ||
        (rank == context->partial_rank && candidate->cost == current->cost &&
         candidate->sequence < current->sequence)) {
        context->partial_node = node;
        context->partial_rank = rank;
    }
}

static bool pf_frontier_available(atrinik_pf_context *context) {
    return context->options.max_frontier == 0U ||
           context->heap_count < context->options.max_frontier;
}

static bool pf_emit_transition(void *emit_context, const atrinik_pf_transition *transition) {
    atrinik_pf_context *context = emit_context;
    pf_node candidate;
    size_t parent_index = context->active_node;
    size_t index;
    uint64_t step_cost;
    uint64_t tentative_cost;

    if (context->stopped) {
        return false;
    }
    if (transition == NULL) {
        pf_stop(context, ATRINIK_PF_ADAPTER_ERROR, ATRINIK_PF_TERMINATION_ADAPTER_ERROR);
        return false;
    }
    if (context->adapter->cancelled != NULL &&
        context->adapter->cancelled(context->adapter->context)) {
        pf_stop(context, ATRINIK_PF_CANCELLED, ATRINIK_PF_TERMINATION_CANCELLED);
        return false;
    }
    if (context->options.max_transitions != 0U &&
        context->metrics.examined_transitions >= context->options.max_transitions) {
        pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
        return false;
    }
    context->metrics.examined_transitions++;

    if (context->reachability) {
        if (pf_find_node(context, transition->state) != PF_NONE) {
            return true;
        }
        if (context->options.max_generated != 0U &&
            context->metrics.generated >= context->options.max_generated) {
            pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
            return false;
        }
        if (context->options.max_frontier != 0U &&
            context->node_count - context->metrics.expanded >= context->options.max_frontier) {
            pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
            return false;
        }

        memset(&candidate, 0, sizeof(candidate));
        candidate.state = transition->state;
        candidate.parent = parent_index;
        candidate.heap_index = PF_NONE;
        candidate.sequence = context->node_count;
        candidate.transition_data = transition->data;
        if (!pf_add_node(context, &candidate, &index)) {
            pf_stop(context, ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            return false;
        }
        context->metrics.generated++;
        size_t frontier = context->node_count - context->metrics.expanded;
        if (frontier > context->metrics.peak_frontier) {
            context->metrics.peak_frontier = frontier;
        }
        return true;
    }

    step_cost =
        context->options.algorithm == ATRINIK_PF_BREADTH_FIRST ? UINT64_C(1) : transition->cost;
    if (UINT64_MAX - context->nodes[parent_index].cost < step_cost) {
        pf_stop(context, ATRINIK_PF_COST_OVERFLOW, ATRINIK_PF_TERMINATION_COST_OVERFLOW);
        return false;
    }
    tentative_cost = context->nodes[parent_index].cost + step_cost;
    index = pf_find_node(context, transition->state);
    if (index != PF_NONE && tentative_cost >= context->nodes[index].cost) {
        return true;
    }

    if (index == PF_NONE) {
        if (context->options.max_generated != 0U &&
            context->metrics.generated >= context->options.max_generated) {
            pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
            return false;
        }
        if (!pf_frontier_available(context)) {
            pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
            return false;
        }

        memset(&candidate, 0, sizeof(candidate));
        candidate.state = transition->state;
        candidate.cost = tentative_cost;
        candidate.heuristic =
            context->options.algorithm == ATRINIK_PF_ASTAR ||
                    context->options.algorithm == ATRINIK_PF_GREEDY_BEST_FIRST
                ? context->adapter->heuristic(context->adapter->context, transition->state)
                : 0U;
        candidate.priority =
            pf_priority(context->options.algorithm, candidate.cost, candidate.heuristic);
        candidate.transition_data = transition->data;
        candidate.parent = parent_index;
        candidate.heap_index = PF_NONE;
        candidate.sequence = context->node_count;
        if (!pf_add_node(context, &candidate, &index)) {
            pf_stop(context, ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            return false;
        }
        context->metrics.generated++;
        pf_consider_partial(context, index);
        if (!pf_heap_push(context, index)) {
            pf_stop(context, ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            return false;
        }
        return true;
    }

    pf_node *node = &context->nodes[index];
    if (node->closed && !pf_frontier_available(context)) {
        pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
        return false;
    }
    node->cost = tentative_cost;
    node->priority = pf_priority(context->options.algorithm, node->cost, node->heuristic);
    node->transition_data = transition->data;
    node->parent = parent_index;
    pf_consider_partial(context, index);
    if (node->closed) {
        node->closed = false;
        if (!pf_heap_push(context, index)) {
            pf_stop(context, ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            return false;
        }
    } else {
        pf_heap_up(context, node->heap_index);
    }
    return true;
}

static bool pf_build_path(atrinik_pf_context *context, size_t node, atrinik_pf_result *result) {
    size_t count = 0U;
    size_t current = node;

    while (current != PF_NONE) {
        count++;
        current = context->nodes[current].parent;
    }
    if (count > context->path_capacity && !pf_grow_array((void **)&context->path,
                                                         sizeof(*context->path),
                                                         &context->path_capacity,
                                                         count)) {
        return false;
    }

    current = node;
    for (size_t i = count; i != 0U; i--) {
        context->path[i - 1U].state = context->nodes[current].state;
        context->path[i - 1U].data = context->nodes[current].transition_data;
        current = context->nodes[current].parent;
    }
    context->path[0].data = 0U;
    result->steps = context->path;
    result->step_count = count;
    result->metrics.total_cost = context->nodes[node].cost;
    return true;
}

void atrinik_pf_options_init(atrinik_pf_options *options) {
    if (options == NULL) {
        return;
    }

    memset(options, 0, sizeof(*options));
    options->algorithm = ATRINIK_PF_ASTAR;
}

atrinik_pf_context *atrinik_pf_context_create(void) {
    return calloc(1U, sizeof(atrinik_pf_context));
}

void atrinik_pf_context_destroy(atrinik_pf_context *context) {
    if (context == NULL) {
        return;
    }

    free(context->nodes);
    free(context->hash);
    free(context->heap);
    free(context->path);
    free(context->reachable);
    free(context);
}

static atrinik_pf_result pf_empty_result(atrinik_pf_status status,
                                         atrinik_pf_termination termination) {
    atrinik_pf_result result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.termination = termination;
    return result;
}

static bool pf_valid_algorithm(atrinik_pf_algorithm algorithm) {
    return algorithm >= ATRINIK_PF_ASTAR && algorithm <= ATRINIK_PF_GREEDY_BEST_FIRST;
}

static atrinik_pf_result pf_finish_search(atrinik_pf_context *context,
                                          atrinik_pf_status status,
                                          atrinik_pf_termination termination,
                                          size_t node) {
    atrinik_pf_result result = pf_empty_result(status, termination);

    result.metrics = context->metrics;
    if (node != PF_NONE) {
        if (!pf_build_path(context, node, &result)) {
            result =
                pf_empty_result(ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            result.metrics = context->metrics;
        }
        return result;
    }

    if (context->options.return_partial && context->partial_node != PF_NONE &&
        (termination == ATRINIK_PF_TERMINATION_EXHAUSTED ||
         termination == ATRINIK_PF_TERMINATION_LIMIT ||
         termination == ATRINIK_PF_TERMINATION_CANCELLED)) {
        result.status = ATRINIK_PF_PARTIAL;
        if (!pf_build_path(context, context->partial_node, &result)) {
            result =
                pf_empty_result(ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
            result.metrics = context->metrics;
        }
    }
    return result;
}

atrinik_pf_result atrinik_pf_search(atrinik_pf_context *context,
                                    const atrinik_pf_adapter *adapter,
                                    atrinik_pf_state_id start,
                                    const atrinik_pf_options *options) {
    atrinik_pf_options defaults;
    pf_node start_node;
    size_t start_index;

    if (context == NULL) {
        return pf_empty_result(ATRINIK_PF_INVALID_INPUT, ATRINIK_PF_TERMINATION_INVALID_INPUT);
    }
    if (options == NULL) {
        atrinik_pf_options_init(&defaults);
        options = &defaults;
    }
    pf_reset(context, adapter, options);
    if (adapter == NULL || adapter->neighbors == NULL || adapter->goal == NULL ||
        !pf_valid_algorithm(options->algorithm) ||
        ((options->algorithm == ATRINIK_PF_ASTAR ||
          options->algorithm == ATRINIK_PF_GREEDY_BEST_FIRST) &&
         adapter->heuristic == NULL) ||
        (options->return_partial && adapter->partial_rank == NULL)) {
        return pf_empty_result(ATRINIK_PF_INVALID_INPUT, ATRINIK_PF_TERMINATION_INVALID_INPUT);
    }

    memset(&start_node, 0, sizeof(start_node));
    start_node.state = start;
    start_node.heuristic =
        options->algorithm == ATRINIK_PF_ASTAR || options->algorithm == ATRINIK_PF_GREEDY_BEST_FIRST
            ? adapter->heuristic(adapter->context, start)
            : 0U;
    start_node.priority = pf_priority(options->algorithm, 0U, start_node.heuristic);
    start_node.parent = PF_NONE;
    start_node.heap_index = PF_NONE;
    if (!pf_add_node(context, &start_node, &start_index) || !pf_heap_push(context, start_index)) {
        return pf_finish_search(context,
                                ATRINIK_PF_OUT_OF_MEMORY,
                                ATRINIK_PF_TERMINATION_OUT_OF_MEMORY,
                                PF_NONE);
    }
    context->metrics.generated = 1U;
    pf_consider_partial(context, start_index);

    while (context->heap_count != 0U) {
        size_t node;

        if (adapter->cancelled != NULL && adapter->cancelled(adapter->context)) {
            return pf_finish_search(context,
                                    ATRINIK_PF_CANCELLED,
                                    ATRINIK_PF_TERMINATION_CANCELLED,
                                    PF_NONE);
        }
        node = pf_heap_pop(context);
        if (adapter->goal(adapter->context, context->nodes[node].state)) {
            context->metrics.total_cost = context->nodes[node].cost;
            return pf_finish_search(context, ATRINIK_PF_FOUND, ATRINIK_PF_TERMINATION_GOAL, node);
        }
        if (options->max_expanded != 0U && context->metrics.expanded >= options->max_expanded) {
            return pf_finish_search(context,
                                    ATRINIK_PF_LIMIT_REACHED,
                                    ATRINIK_PF_TERMINATION_LIMIT,
                                    PF_NONE);
        }

        context->nodes[node].closed = true;
        context->metrics.expanded++;
        context->active_node = node;
        bool enumerated = adapter->neighbors(adapter->context,
                                             context->nodes[node].state,
                                             pf_emit_transition,
                                             context);
        context->active_node = PF_NONE;
        if (!enumerated || context->stopped) {
            if (!context->stopped) {
                pf_stop(context, ATRINIK_PF_ADAPTER_ERROR, ATRINIK_PF_TERMINATION_ADAPTER_ERROR);
            }
            return pf_finish_search(context,
                                    context->stop_status,
                                    context->stop_termination,
                                    PF_NONE);
        }
    }

    return pf_finish_search(context, ATRINIK_PF_NO_PATH, ATRINIK_PF_TERMINATION_EXHAUSTED, PF_NONE);
}

atrinik_pf_reachability_result atrinik_pf_reachable(atrinik_pf_context *context,
                                                    const atrinik_pf_adapter *adapter,
                                                    atrinik_pf_state_id start,
                                                    const atrinik_pf_options *options) {
    atrinik_pf_reachability_result result;
    atrinik_pf_options defaults;
    pf_node start_node;
    size_t start_index;
    size_t cursor = 0U;

    memset(&result, 0, sizeof(result));
    result.status = ATRINIK_PF_INVALID_INPUT;
    result.termination = ATRINIK_PF_TERMINATION_INVALID_INPUT;
    if (context == NULL) {
        return result;
    }
    if (options == NULL) {
        atrinik_pf_options_init(&defaults);
        options = &defaults;
    }
    pf_reset(context, adapter, options);
    context->reachability = true;
    if (adapter == NULL || adapter->neighbors == NULL) {
        return result;
    }

    memset(&start_node, 0, sizeof(start_node));
    start_node.state = start;
    start_node.parent = PF_NONE;
    start_node.heap_index = PF_NONE;
    if (!pf_add_node(context, &start_node, &start_index)) {
        result.status = ATRINIK_PF_OUT_OF_MEMORY;
        result.termination = ATRINIK_PF_TERMINATION_OUT_OF_MEMORY;
        return result;
    }
    context->metrics.generated = 1U;
    context->metrics.peak_frontier = 1U;

    while (cursor < context->node_count) {
        if (adapter->cancelled != NULL && adapter->cancelled(adapter->context)) {
            pf_stop(context, ATRINIK_PF_CANCELLED, ATRINIK_PF_TERMINATION_CANCELLED);
            break;
        }
        if (options->max_expanded != 0U && context->metrics.expanded >= options->max_expanded) {
            pf_stop(context, ATRINIK_PF_LIMIT_REACHED, ATRINIK_PF_TERMINATION_LIMIT);
            break;
        }

        context->active_node = cursor;
        context->metrics.expanded++;
        bool enumerated = adapter->neighbors(adapter->context,
                                             context->nodes[cursor].state,
                                             pf_emit_transition,
                                             context);
        context->active_node = PF_NONE;
        if (!enumerated || context->stopped) {
            if (!context->stopped) {
                pf_stop(context, ATRINIK_PF_ADAPTER_ERROR, ATRINIK_PF_TERMINATION_ADAPTER_ERROR);
            }
            break;
        }
        cursor++;
    }

    bool have_storage = context->node_count <= context->reachable_capacity ||
                        pf_grow_array((void **)&context->reachable,
                                      sizeof(*context->reachable),
                                      &context->reachable_capacity,
                                      context->node_count);
    if (!have_storage) {
        pf_stop(context, ATRINIK_PF_OUT_OF_MEMORY, ATRINIK_PF_TERMINATION_OUT_OF_MEMORY);
    } else {
        for (size_t i = 0U; i < context->node_count; i++) {
            context->reachable[i] = context->nodes[i].state;
        }
        result.states = context->reachable;
        result.state_count = context->node_count;
    }
    result.metrics = context->metrics;
    if (context->stopped) {
        result.status = context->stop_status;
        result.termination = context->stop_termination;
    } else {
        result.status = ATRINIK_PF_COMPLETE;
        result.termination = ATRINIK_PF_TERMINATION_EXHAUSTED;
    }
    return result;
}

const char *atrinik_pf_status_string(atrinik_pf_status status) {
    static const char *const names[] = {"found",
                                        "no path",
                                        "limit reached",
                                        "cancelled",
                                        "partial",
                                        "complete",
                                        "invalid input",
                                        "adapter error",
                                        "out of memory",
                                        "cost overflow"};

    if ((size_t)status >= sizeof(names) / sizeof(names[0])) {
        return "unknown";
    }
    return names[status];
}
