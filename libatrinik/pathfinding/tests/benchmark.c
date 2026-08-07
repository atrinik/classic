#include <atrinik/pathfinding.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    WIDTH = 256,
    HEIGHT = 256
};

typedef struct grid {
    atrinik_pf_state_id goal;
} grid;

static bool
neighbors(void *context, atrinik_pf_state_id state, atrinik_pf_emit_fn emit, void *emit_context) {
    (void)context;
    size_t x = (size_t)state % WIDTH;
    size_t y = (size_t)state / WIDTH;
    const int offsets[][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

    for (size_t i = 0U; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        int next_x = (int)x + offsets[i][0];
        int next_y = (int)y + offsets[i][1];
        if (next_x >= 0 && next_x < WIDTH && next_y >= 0 && next_y < HEIGHT) {
            atrinik_pf_transition transition = {
                .state = (atrinik_pf_state_id)((size_t)next_y * WIDTH + (size_t)next_x),
                .cost = 1U,
                .data = i,
            };
            if (!emit(emit_context, &transition)) {
                return false;
            }
        }
    }
    return true;
}

static bool goal(void *context, atrinik_pf_state_id state) {
    grid *map = context;
    return state == map->goal;
}

static uint64_t heuristic(void *context, atrinik_pf_state_id state) {
    grid *map = context;
    size_t x = (size_t)state % WIDTH;
    size_t y = (size_t)state / WIDTH;
    size_t goal_x = (size_t)map->goal % WIDTH;
    size_t goal_y = (size_t)map->goal / WIDTH;
    size_t dx = x > goal_x ? x - goal_x : goal_x - x;
    size_t dy = y > goal_y ? y - goal_y : goal_y - y;
    return (uint64_t)(dx + dy);
}

int main(void) {
    grid map = {.goal = WIDTH * HEIGHT - 1U};
    atrinik_pf_adapter adapter = {
        .context = &map,
        .neighbors = neighbors,
        .goal = goal,
        .heuristic = heuristic,
    };
    atrinik_pf_options options;
    atrinik_pf_context *context = atrinik_pf_context_create();
    if (context == NULL) {
        return EXIT_FAILURE;
    }

    atrinik_pf_options_init(&options);
    clock_t start = clock();
    atrinik_pf_result result = atrinik_pf_search(context, &adapter, 0U, &options);
    clock_t elapsed = clock() - start;
    printf("status=%s cost=%" PRIu64 " steps=%zu expanded=%zu generated=%zu "
           "transitions=%zu peak_frontier=%zu seconds=%.6f\n",
           atrinik_pf_status_string(result.status),
           result.metrics.total_cost,
           result.step_count,
           result.metrics.expanded,
           result.metrics.generated,
           result.metrics.examined_transitions,
           result.metrics.peak_frontier,
           (double)elapsed / (double)CLOCKS_PER_SEC);
    atrinik_pf_context_destroy(context);
    return result.status == ATRINIK_PF_FOUND ? EXIT_SUCCESS : EXIT_FAILURE;
}
