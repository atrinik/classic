#include <atrinik/pathfinding.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct edge {
    atrinik_pf_state_id from;
    atrinik_pf_state_id to;
    uint64_t cost;
    atrinik_pf_transition_data data;
} edge;

typedef struct fixture {
    const edge *edges;
    size_t edge_count;
    atrinik_pf_state_id goal;
    const uint64_t *heuristics;
    const uint64_t *ranks;
    size_t score_count;
    size_t cancel_after;
    size_t cancel_calls;
    atrinik_pf_state_id fail_state;
    bool fail_enabled;
    bool ignore_emit_stop;
} fixture;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

static bool
neighbors(void *context, atrinik_pf_state_id state, atrinik_pf_emit_fn emit, void *emit_context) {
    fixture *graph = context;

    if (graph->fail_enabled && state == graph->fail_state) {
        return false;
    }
    for (size_t i = 0U; i < graph->edge_count; i++) {
        if (graph->edges[i].from == state) {
            atrinik_pf_transition transition = {
                .state = graph->edges[i].to,
                .cost = graph->edges[i].cost,
                .data = graph->edges[i].data,
            };
            if (!emit(emit_context, &transition) && !graph->ignore_emit_stop) {
                return false;
            }
        }
    }
    return true;
}

static bool is_goal(void *context, atrinik_pf_state_id state) {
    fixture *graph = context;
    return state == graph->goal;
}

static uint64_t heuristic(void *context, atrinik_pf_state_id state) {
    fixture *graph = context;
    return state < graph->score_count ? graph->heuristics[state] : 0U;
}

static uint64_t rank_partial(void *context, atrinik_pf_state_id state) {
    fixture *graph = context;
    return state < graph->score_count ? graph->ranks[state] : UINT64_MAX;
}

static bool cancelled(void *context) {
    fixture *graph = context;
    bool result = graph->cancel_after != 0U && graph->cancel_calls >= graph->cancel_after;
    graph->cancel_calls++;
    return result;
}

static atrinik_pf_adapter make_adapter(fixture *graph) {
    atrinik_pf_adapter adapter = {
        .context = graph,
        .neighbors = neighbors,
        .goal = is_goal,
        .heuristic = heuristic,
        .partial_rank = rank_partial,
        .cancelled = cancelled,
    };
    return adapter;
}

static bool
check_path(const atrinik_pf_result *result, const atrinik_pf_state_id *states, size_t count) {
    CHECK(result->step_count == count);
    for (size_t i = 0U; i < count; i++) {
        CHECK(result->steps[i].state == states[i]);
    }
    return true;
}

static bool test_algorithms_and_tie_breaking(atrinik_pf_context *context) {
    static const edge edges[] = {
        {0U, 1U, 10U, 101U},
        {0U, 2U, 1U, 102U},
        {1U, 3U, 1U, 113U},
        {2U, 3U, 1U, 123U},
    };
    static const uint64_t scores[] = {2U, 1U, 1U, 0U};
    static const atrinik_pf_state_id weighted_path[] = {0U, 2U, 3U};
    static const atrinik_pf_state_id bfs_path[] = {0U, 1U, 3U};
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
        .goal = 3U,
        .heuristics = scores,
        .ranks = scores,
        .score_count = sizeof(scores) / sizeof(scores[0]),
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 2U);
    CHECK(check_path(&result, weighted_path, sizeof(weighted_path) / sizeof(weighted_path[0])));
    CHECK(result.steps[1].data == 102U);
    CHECK(result.steps[2].data == 123U);

    options.algorithm = ATRINIK_PF_DIJKSTRA;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 2U);
    CHECK(check_path(&result, weighted_path, sizeof(weighted_path) / sizeof(weighted_path[0])));

    options.algorithm = ATRINIK_PF_BREADTH_FIRST;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 2U);
    CHECK(check_path(&result, bfs_path, sizeof(bfs_path) / sizeof(bfs_path[0])));

    options.algorithm = ATRINIK_PF_GREEDY_BEST_FIRST;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(check_path(&result, bfs_path, sizeof(bfs_path) / sizeof(bfs_path[0])));
    return true;
}

static bool test_statuses_and_partial(atrinik_pf_context *context) {
    static const edge edges[] = {{0U, 1U, 1U, 0U}, {1U, 2U, 1U, 0U}};
    static const uint64_t scores[] = {9U, 4U, 2U, 0U};
    static const atrinik_pf_state_id partial_path[] = {0U, 1U};
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
        .goal = 3U,
        .heuristics = scores,
        .ranks = scores,
        .score_count = sizeof(scores) / sizeof(scores[0]),
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_NO_PATH);
    CHECK(result.termination == ATRINIK_PF_TERMINATION_EXHAUSTED);
    CHECK(result.steps == NULL);

    options.max_expanded = 1U;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_LIMIT_REACHED);
    CHECK(result.metrics.expanded == 1U);

    options.return_partial = true;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_PARTIAL);
    CHECK(result.termination == ATRINIK_PF_TERMINATION_LIMIT);
    CHECK(check_path(&result, partial_path, sizeof(partial_path) / sizeof(partial_path[0])));

    options.max_expanded = 0U;
    graph.cancel_after = 1U;
    graph.cancel_calls = 0U;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_PARTIAL);
    CHECK(result.termination == ATRINIK_PF_TERMINATION_CANCELLED);
    CHECK(result.metrics.examined_transitions == 0U);
    graph.cancel_after = 0U;
    return true;
}

static bool test_limits_errors_and_validation(atrinik_pf_context *context) {
    static const edge edges[] = {
        {0U, 1U, 1U, 0U},
        {0U, 2U, 1U, 0U},
        {1U, 3U, UINT64_MAX, 0U},
    };
    static const uint64_t scores[] = {2U, 1U, 1U, 0U};
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
        .goal = 9U,
        .heuristics = scores,
        .ranks = scores,
        .score_count = sizeof(scores) / sizeof(scores[0]),
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    options.max_generated = 2U;
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_LIMIT_REACHED);
    CHECK(result.metrics.generated == 2U);

    options.max_generated = 0U;
    options.max_frontier = 1U;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_LIMIT_REACHED);

    options.max_frontier = 0U;
    options.max_transitions = 1U;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_LIMIT_REACHED);
    CHECK(result.metrics.examined_transitions == 1U);

    options.max_transitions = 0U;
    options.algorithm = ATRINIK_PF_DIJKSTRA;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_COST_OVERFLOW);

    graph.fail_enabled = true;
    graph.fail_state = 0U;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_ADAPTER_ERROR);
    graph.fail_enabled = false;

    adapter.goal = NULL;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_INVALID_INPUT);
    adapter = make_adapter(&graph);
    options.return_partial = true;
    adapter.partial_rank = NULL;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_INVALID_INPUT);
    CHECK(strcmp(atrinik_pf_status_string(result.status), "invalid input") == 0);
    return true;
}

static bool test_zero_cost_and_large_ids(atrinik_pf_context *context) {
    static const edge edges[] = {
        {UINT64_MAX, UINT64_MAX - 1U, 0U, 7U},
        {UINT64_MAX - 1U, 42U, 0U, 8U},
        {42U, UINT64_MAX, 0U, 9U},
    };
    static const atrinik_pf_state_id path[] = {UINT64_MAX, UINT64_MAX - 1U, 42U};
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
        .goal = 42U,
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    options.algorithm = ATRINIK_PF_DIJKSTRA;
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, UINT64_MAX, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 0U);
    CHECK(check_path(&result, path, sizeof(path) / sizeof(path[0])));
    return true;
}

static bool test_adapter_cannot_emit_after_stop(atrinik_pf_context *context) {
    static const edge edges[] = {
        {0U, 1U, UINT64_MAX, 0U},
        {1U, 2U, 1U, 0U},
        {1U, 3U, 0U, 0U},
    };
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
        .goal = 9U,
        .ignore_emit_stop = true,
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    options.algorithm = ATRINIK_PF_DIJKSTRA;
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_COST_OVERFLOW);
    CHECK(result.metrics.generated == 2U);
    CHECK(result.metrics.examined_transitions == 2U);
    return true;
}

static bool test_frontier_updates_and_reopening(atrinik_pf_context *context) {
    static const edge decrease_edges[] = {
        {0U, 1U, 10U, 0U},
        {0U, 2U, 1U, 0U},
        {2U, 1U, 1U, 0U},
    };
    static const atrinik_pf_state_id expected[] = {0U, 2U, 1U};
    fixture graph = {
        .edges = decrease_edges,
        .edge_count = sizeof(decrease_edges) / sizeof(decrease_edges[0]),
        .goal = 1U,
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    options.algorithm = ATRINIK_PF_DIJKSTRA;
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 2U);
    CHECK(check_path(&result, expected, sizeof(expected) / sizeof(expected[0])));

    static const edge reopen_edges[] = {
        {0U, 1U, 3U, 0U},
        {0U, 2U, 1U, 0U},
        {1U, 3U, 10U, 0U},
        {2U, 1U, 1U, 0U},
    };
    static const uint64_t heuristic_scores[] = {0U, 0U, 5U, 0U};
    static const atrinik_pf_state_id reopened_path[] = {0U, 2U, 1U, 3U};
    graph.edges = reopen_edges;
    graph.edge_count = sizeof(reopen_edges) / sizeof(reopen_edges[0]);
    graph.goal = 3U;
    graph.heuristics = heuristic_scores;
    graph.score_count = sizeof(heuristic_scores) / sizeof(heuristic_scores[0]);
    options.algorithm = ATRINIK_PF_ASTAR;
    result = atrinik_pf_search(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_FOUND);
    CHECK(result.metrics.total_cost == 12U);
    CHECK(check_path(&result, reopened_path, sizeof(reopened_path) / sizeof(reopened_path[0])));
    return true;
}

static bool test_reachability(atrinik_pf_context *context) {
    static const edge edges[] = {
        {0U, 1U, 99U, 0U},
        {0U, 2U, 1U, 0U},
        {1U, 3U, 1U, 0U},
        {2U, 3U, 1U, 0U},
        {3U, 0U, 1U, 0U},
        {8U, 9U, 1U, 0U},
    };
    static const atrinik_pf_state_id expected[] = {0U, 1U, 2U, 3U};
    fixture graph = {
        .edges = edges,
        .edge_count = sizeof(edges) / sizeof(edges[0]),
    };
    atrinik_pf_adapter adapter = make_adapter(&graph);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    atrinik_pf_reachability_result result = atrinik_pf_reachable(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_COMPLETE);
    CHECK(result.state_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0U; i < result.state_count; i++) {
        CHECK(result.states[i] == expected[i]);
    }

    options.max_generated = 3U;
    result = atrinik_pf_reachable(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_LIMIT_REACHED);
    CHECK(result.state_count == 3U);

    options.max_generated = 0U;
    graph.cancel_after = 1U;
    graph.cancel_calls = 0U;
    result = atrinik_pf_reachable(context, &adapter, 0U, &options);
    CHECK(result.status == ATRINIK_PF_CANCELLED);
    CHECK(result.state_count >= 1U);
    return true;
}

static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static bool test_dijkstra_against_reference(atrinik_pf_context *context) {
    enum {
        NODE_COUNT = 24,
        CASE_COUNT = 100,
        MAX_EDGES = NODE_COUNT * NODE_COUNT
    };
    edge edges[MAX_EDGES];
    uint64_t distances[NODE_COUNT];
    uint32_t random_state = UINT32_C(0x8a71c4d3);
    atrinik_pf_options options;

    atrinik_pf_options_init(&options);
    options.algorithm = ATRINIK_PF_DIJKSTRA;
    for (size_t test_case = 0U; test_case < CASE_COUNT; test_case++) {
        size_t edge_count = 0U;
        for (size_t from = 0U; from < NODE_COUNT; from++) {
            for (size_t to = 0U; to < NODE_COUNT; to++) {
                if (from != to && next_random(&random_state) % 5U == 0U) {
                    edges[edge_count++] = (edge){
                        .from = from,
                        .to = to,
                        .cost = next_random(&random_state) % 20U + 1U,
                    };
                }
            }
        }

        for (size_t i = 0U; i < NODE_COUNT; i++) {
            distances[i] = UINT64_MAX;
        }
        distances[0] = 0U;
        for (size_t pass = 1U; pass < NODE_COUNT; pass++) {
            bool changed = false;
            for (size_t i = 0U; i < edge_count; i++) {
                if (distances[edges[i].from] != UINT64_MAX &&
                    distances[edges[i].from] <= UINT64_MAX - edges[i].cost &&
                    distances[edges[i].from] + edges[i].cost < distances[edges[i].to]) {
                    distances[edges[i].to] = distances[edges[i].from] + edges[i].cost;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }

        fixture graph = {
            .edges = edges,
            .edge_count = edge_count,
            .goal = NODE_COUNT - 1U,
        };
        atrinik_pf_adapter adapter = make_adapter(&graph);
        atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
        if (distances[NODE_COUNT - 1U] == UINT64_MAX) {
            CHECK(result.status == ATRINIK_PF_NO_PATH);
        } else {
            CHECK(result.status == ATRINIK_PF_FOUND);
            CHECK(result.metrics.total_cost == distances[NODE_COUNT - 1U]);
        }
    }
    return true;
}

int main(void) {
    atrinik_pf_context *context = atrinik_pf_context_create();
    if (context == NULL) {
        return EXIT_FAILURE;
    }

    bool passed = test_algorithms_and_tie_breaking(context) && test_statuses_and_partial(context) &&
                  test_limits_errors_and_validation(context) &&
                  test_zero_cost_and_large_ids(context) &&
                  test_adapter_cannot_emit_after_stop(context) &&
                  test_frontier_updates_and_reopening(context) && test_reachability(context);
    passed = passed && test_dijkstra_against_reference(context);
    atrinik_pf_context_destroy(context);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
