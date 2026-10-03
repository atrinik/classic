#include <live_movement_route.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <openssl/evp.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct live_movement_checkpoint {
    char map[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1U];
    uint8_t x;
    uint8_t y;
    uint8_t direction;
} live_movement_checkpoint_t;

struct live_movement_route {
    uint64_t timeout_ms;
    uint64_t step_timeout_ms;
    size_t checkpoints_count;
    live_movement_checkpoint_t *checkpoints;
    char sha256[65];
};

typedef enum live_movement_route_phase {
    LIVE_MOVEMENT_ROUTE_PHASE_INITIAL,
    LIVE_MOVEMENT_ROUTE_PHASE_ARRIVAL,
    LIVE_MOVEMENT_ROUTE_PHASE_READY,
    LIVE_MOVEMENT_ROUTE_PHASE_WAITING,
    LIVE_MOVEMENT_ROUTE_PHASE_FAILED,
    LIVE_MOVEMENT_ROUTE_PHASE_DONE,
} live_movement_route_phase_t;

struct live_movement_route_state {
    const live_movement_route_t *route;
    live_movement_route_phase_t phase;
    size_t checkpoint_index;
    uint64_t start_ms;
    uint64_t last_ms;
    uint64_t move_started_ms;
    uint64_t move_generation;
    bool play_started;
    char failure[160];
};

static void route_error(char *error, size_t error_size, const char *format, ...) {
    if (error == NULL || error_size == 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool route_uint(const char *value, uint64_t minimum, uint64_t maximum, uint64_t *result) {
    if (value == NULL || *value == '\0' || (value[0] == '0' && value[1] != '\0')) {
        return false;
    }
    uint64_t parsed = 0;
    for (const unsigned char *cp = (const unsigned char *)value; *cp != '\0'; cp++) {
        if (*cp < '0' || *cp > '9') {
            return false;
        }
        uint8_t digit = (uint8_t)(*cp - '0');
        if (parsed > (maximum - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed < minimum || parsed > maximum) {
        return false;
    }
    *result = parsed;
    return true;
}

static bool route_attributes_closed(xmlNodePtr node,
                                    const char *const *names,
                                    size_t names_count) {
    size_t count = 0;
    for (xmlAttrPtr attribute = node->properties; attribute != NULL; attribute = attribute->next) {
        if (attribute->ns != NULL) {
            return false;
        }
        bool known = false;
        for (size_t i = 0; i < names_count; i++) {
            if (xmlStrEqual(attribute->name, BAD_CAST names[i])) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
        count++;
    }
    return count == names_count;
}

static char *route_property(xmlNodePtr node, const char *name) {
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    return (char *)value;
}

static bool route_map_valid(const char *map) {
    if (map == NULL) {
        return false;
    }
    size_t length = strlen(map);
    if (length < 2U || length > LIVE_MOVEMENT_ROUTE_MAP_MAX || map[0] != '/' ||
        map[length - 1U] == '/') {
        return false;
    }
    const char *component = map + 1;
    for (const char *cp = component;; cp++) {
        unsigned char ch = (unsigned char)*cp;
        if (ch == '/' || ch == '\0') {
            size_t component_size = (size_t)(cp - component);
            if (component_size == 0U ||
                (component_size == 1U && component[0] == '.') ||
                (component_size == 2U && component[0] == '.' && component[1] == '.')) {
                return false;
            }
            if (ch == '\0') {
                return true;
            }
            component = cp + 1;
            continue;
        }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) {
            return false;
        }
    }
}

static bool route_direction_valid(uint64_t direction) {
    return direction >= 1U && direction <= 9U && direction != 5U;
}

static bool route_adjacent(const live_movement_checkpoint_t *previous,
                           const live_movement_checkpoint_t *checkpoint) {
    if (strcmp(previous->map, checkpoint->map) != 0) {
        return true;
    }
    static const int8_t delta_x[10] = {0, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int8_t delta_y[10] = {0, 1, 1, 1, 0, 0, 0, -1, -1, -1};
    int expected_x = (int)previous->x + delta_x[checkpoint->direction];
    int expected_y = (int)previous->y + delta_y[checkpoint->direction];
    return expected_x >= 0 && expected_x <= UINT8_MAX && expected_y >= 0 &&
           expected_y <= UINT8_MAX && checkpoint->x == (uint8_t)expected_x &&
           checkpoint->y == (uint8_t)expected_y;
}

static bool route_checkpoint_parse(xmlNodePtr node,
                                   size_t index,
                                   live_movement_checkpoint_t *checkpoint) {
    static const char *const names[] = {"map", "x", "y", "direction"};
    if (node->type != XML_ELEMENT_NODE || node->ns != NULL || node->nsDef != NULL ||
        !xmlStrEqual(node->name, BAD_CAST "checkpoint") || node->children != NULL ||
        !route_attributes_closed(node, names, sizeof(names) / sizeof(names[0]))) {
        return false;
    }
    char *map = route_property(node, "map");
    char *x = route_property(node, "x");
    char *y = route_property(node, "y");
    char *direction = route_property(node, "direction");
    uint64_t parsed_x = 0;
    uint64_t parsed_y = 0;
    uint64_t parsed_direction = 0;
    bool valid = route_map_valid(map) && route_uint(x, 0, UINT8_MAX, &parsed_x) &&
                 route_uint(y, 0, UINT8_MAX, &parsed_y) &&
                 route_uint(direction, 0, 9, &parsed_direction) &&
                 (index == 0U ? parsed_direction == 0U
                              : route_direction_valid(parsed_direction));
    if (valid) {
        memcpy(checkpoint->map, map, strlen(map) + 1U);
        checkpoint->x = (uint8_t)parsed_x;
        checkpoint->y = (uint8_t)parsed_y;
        checkpoint->direction = (uint8_t)parsed_direction;
    }
    xmlFree(map);
    xmlFree(x);
    xmlFree(y);
    xmlFree(direction);
    return valid;
}

static bool route_read_file(const char *path, char **body, size_t *body_size) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct stat status;
    bool valid = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0 &&
                 (uint64_t)status.st_size <= LIVE_MOVEMENT_ROUTE_FILE_MAX;
    if (!valid) {
        close(fd);
        return false;
    }
    size_t size = (size_t)status.st_size;
    char *data = malloc(size);
    if (data == NULL) {
        close(fd);
        return false;
    }
    size_t used = 0;
    while (used < size) {
        ssize_t received = read(fd, data + used, size - used);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            valid = false;
            break;
        }
        used += (size_t)received;
    }
    unsigned char extra;
    if (valid) {
        ssize_t received;
        do {
            received = read(fd, &extra, 1);
        } while (received < 0 && errno == EINTR);
        if (received != 0) {
            valid = false;
        }
    }
    close(fd);
    if (!valid) {
        free(data);
        return false;
    }
    *body = data;
    *body_size = size;
    return true;
}

bool live_movement_route_load(const char *path,
                              live_movement_route_t **route,
                              char *error,
                              size_t error_size) {
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (route == NULL) {
        route_error(error, error_size, "route output is required");
        return false;
    }
    *route = NULL;
    char *body = NULL;
    size_t body_size = 0;
    if (path == NULL || !route_read_file(path, &body, &body_size)) {
        route_error(error, error_size, "route must be a regular file of 1..%u bytes",
                    LIVE_MOVEMENT_ROUTE_FILE_MAX);
        return false;
    }
    if (body_size > INT_MAX || memchr(body, '\0', body_size) != NULL) {
        free(body);
        route_error(error, error_size, "route contains invalid input bytes");
        return false;
    }
    unsigned char digest[32];
    unsigned int digest_size = 0;
    bool digest_valid = EVP_Digest(body,
                                   body_size,
                                   digest,
                                   &digest_size,
                                   EVP_sha256(),
                                   NULL) == 1 &&
                        digest_size == sizeof(digest);
    xmlDocPtr document =
        digest_valid
            ? xmlReadMemory(body,
                            (int)body_size,
                            path,
                            NULL,
                            XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                XML_PARSE_NOWARNING)
            : NULL;
    free(body);
    if (document == NULL || document->intSubset != NULL || document->extSubset != NULL) {
        xmlFreeDoc(document);
        route_error(error, error_size, "route is not safe, closed XML");
        return false;
    }
    xmlNodePtr root = xmlDocGetRootElement(document);
    static const char *const root_names[] = {"version", "timeout-ms", "step-timeout-ms"};
    bool valid = root != NULL && document->children == root && document->last == root &&
                 root->type == XML_ELEMENT_NODE && root->ns == NULL && root->nsDef == NULL &&
                 xmlStrEqual(root->name, BAD_CAST "live-movement-route") &&
                 route_attributes_closed(root,
                                         root_names,
                                         sizeof(root_names) / sizeof(root_names[0]));
    char *version = valid ? route_property(root, "version") : NULL;
    char *timeout = valid ? route_property(root, "timeout-ms") : NULL;
    char *step_timeout = valid ? route_property(root, "step-timeout-ms") : NULL;
    uint64_t parsed_timeout = 0;
    uint64_t parsed_step_timeout = 0;
    valid = valid && version != NULL && strcmp(version, "1") == 0 &&
            route_uint(timeout, 1, 3600000U, &parsed_timeout) &&
            route_uint(step_timeout, 1, 60000U, &parsed_step_timeout) &&
            parsed_step_timeout <= parsed_timeout;
    xmlFree(version);
    xmlFree(timeout);
    xmlFree(step_timeout);

    size_t checkpoints_count = 0;
    for (xmlNodePtr node = valid ? root->children : NULL; node != NULL; node = node->next) {
        if (node->type != XML_ELEMENT_NODE ||
            checkpoints_count == LIVE_MOVEMENT_ROUTE_STEPS_MAX + 1U) {
            valid = false;
            break;
        }
        checkpoints_count++;
    }
    valid = valid && checkpoints_count != 0U;
    live_movement_route_t *parsed = valid ? calloc(1, sizeof(*parsed)) : NULL;
    if (valid && parsed == NULL) {
        valid = false;
    }
    if (valid) {
        parsed->checkpoints = calloc(checkpoints_count, sizeof(*parsed->checkpoints));
        valid = parsed->checkpoints != NULL;
    }
    size_t index = 0;
    for (xmlNodePtr node = valid ? root->children : NULL; node != NULL; node = node->next) {
        live_movement_checkpoint_t *checkpoint = &parsed->checkpoints[index];
        memset(checkpoint, 0, sizeof(*checkpoint));
        if (!route_checkpoint_parse(node, index, checkpoint) ||
            (index > 0U && !route_adjacent(&parsed->checkpoints[index - 1U], checkpoint))) {
            valid = false;
            break;
        }
        index++;
    }
    if (valid) {
        parsed->checkpoints_count = checkpoints_count;
        parsed->timeout_ms = parsed_timeout;
        parsed->step_timeout_ms = parsed_step_timeout;
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(digest); i++) {
            parsed->sha256[i * 2U] = hex[digest[i] >> 4U];
            parsed->sha256[i * 2U + 1U] = hex[digest[i] & 0x0fU];
        }
        parsed->sha256[64] = '\0';
        *route = parsed;
    } else {
        live_movement_route_free(parsed);
        route_error(error, error_size, "route XML does not match the closed version 1 schema");
    }
    xmlFreeDoc(document);
    return valid;
}

size_t live_movement_route_checkpoint_count(const live_movement_route_t *route) {
    return route != NULL ? route->checkpoints_count : 0U;
}

uint64_t live_movement_route_timeout_ms(const live_movement_route_t *route) {
    return route != NULL ? route->timeout_ms : 0U;
}

uint64_t live_movement_route_step_timeout_ms(const live_movement_route_t *route) {
    return route != NULL ? route->step_timeout_ms : 0U;
}

const char *live_movement_route_sha256(const live_movement_route_t *route) {
    return route != NULL ? route->sha256 : NULL;
}

void live_movement_route_free(live_movement_route_t *route) {
    if (route != NULL) {
        free(route->checkpoints);
        free(route);
    }
}

live_movement_route_state_t *
live_movement_route_state_create(const live_movement_route_t *route, uint64_t start_ms) {
    if (route == NULL || route->checkpoints_count == 0U) {
        return NULL;
    }
    live_movement_route_state_t *state = calloc(1, sizeof(*state));
    if (state != NULL) {
        state->route = route;
        state->phase = LIVE_MOVEMENT_ROUTE_PHASE_INITIAL;
        state->start_ms = start_ms;
        state->last_ms = start_ms;
    }
    return state;
}

void live_movement_route_state_free(live_movement_route_state_t *state) {
    free(state);
}

static live_movement_route_action_t route_action(const live_movement_route_state_t *state,
                                                 live_movement_route_action_type_t type,
                                                 uint8_t direction) {
    return (live_movement_route_action_t){
        .type = type,
        .checkpoint_index = state != NULL ? state->checkpoint_index : 0U,
        .direction = direction,
    };
}

static live_movement_route_action_t route_fail(live_movement_route_state_t *state,
                                               const char *reason) {
    state->phase = LIVE_MOVEMENT_ROUTE_PHASE_FAILED;
    snprintf(state->failure, sizeof(state->failure), "%s", reason);
    return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_FAILED, 0);
}

static bool route_observation_matches(const live_movement_route_observation_t *observation,
                                      const live_movement_checkpoint_t *checkpoint) {
    return observation->map != NULL && strcmp(observation->map, checkpoint->map) == 0 &&
           observation->x == checkpoint->x && observation->y == checkpoint->y;
}

live_movement_route_action_t
live_movement_route_tick(live_movement_route_state_t *state,
                         const live_movement_route_observation_t *observation) {
    if (state == NULL) {
        return (live_movement_route_action_t){.type = LIVE_MOVEMENT_ROUTE_ACTION_FAILED};
    }
    if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_FAILED) {
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_FAILED, 0);
    }
    if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_DONE) {
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_DONE, 0);
    }
    if (observation == NULL) {
        return route_fail(state, "missing route observation");
    }
    if (observation->now_ms < state->last_ms || observation->now_ms < state->start_ms) {
        return route_fail(state, "monotonic clock regressed");
    }
    state->last_ms = observation->now_ms;
    if (observation->now_ms - state->start_ms >= state->route->timeout_ms) {
        return route_fail(state, "total route deadline expired");
    }
    if (observation->play) {
        state->play_started = true;
    }
    if (state->play_started && !observation->connected) {
        return route_fail(state, "disconnected after play started");
    }
    const live_movement_checkpoint_t *current =
        &state->route->checkpoints[state->checkpoint_index];
    if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_ARRIVAL) {
        if (observation->connected && observation->play && observation->published_ready &&
            observation->map != NULL && !route_observation_matches(observation, current)) {
            return route_fail(state, "published position changed before arrival presentation");
        }
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_NONE, 0);
    }
    if (!observation->connected || !observation->play || !observation->published_ready ||
        observation->map == NULL) {
        if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_WAITING &&
            observation->now_ms - state->move_started_ms >= state->route->step_timeout_ms) {
            return route_fail(state, "movement step deadline expired");
        }
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_NONE, 0);
    }

    if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_INITIAL) {
        if (!route_observation_matches(observation, current)) {
            return route_fail(state, "initial published position does not match checkpoint 0");
        }
        state->phase = LIVE_MOVEMENT_ROUTE_PHASE_ARRIVAL;
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL, 0);
    }
    if (state->phase == LIVE_MOVEMENT_ROUTE_PHASE_READY) {
        if (!route_observation_matches(observation, current)) {
            return route_fail(state, "position changed before the next movement dispatch");
        }
        if (state->checkpoint_index + 1U == state->route->checkpoints_count) {
            state->phase = LIVE_MOVEMENT_ROUTE_PHASE_DONE;
            return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_DONE, 0);
        }
        if (observation->run_on || observation->fire_on) {
            return route_fail(state, "run and fire modes must be disabled for movement");
        }
        const live_movement_checkpoint_t *next =
            &state->route->checkpoints[state->checkpoint_index + 1U];
        state->move_started_ms = observation->now_ms;
        state->move_generation = observation->publication_generation;
        state->phase = LIVE_MOVEMENT_ROUTE_PHASE_WAITING;
        live_movement_route_action_t action =
            route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_MOVE, next->direction);
        action.checkpoint_index++;
        return action;
    }
    if (observation->now_ms - state->move_started_ms >= state->route->step_timeout_ms) {
        return route_fail(state, "movement step deadline expired");
    }
    if (observation->publication_generation <= state->move_generation) {
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_NONE, 0);
    }
    if (route_observation_matches(observation, current)) {
        return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_NONE, 0);
    }
    const live_movement_checkpoint_t *next =
        &state->route->checkpoints[state->checkpoint_index + 1U];
    if (!route_observation_matches(observation, next)) {
        return route_fail(state, "published position is neither the source nor destination");
    }
    state->checkpoint_index++;
    state->phase = LIVE_MOVEMENT_ROUTE_PHASE_ARRIVAL;
    return route_action(state, LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL, 0);
}

bool live_movement_route_arrival_presented(live_movement_route_state_t *state) {
    if (state == NULL || state->phase != LIVE_MOVEMENT_ROUTE_PHASE_ARRIVAL) {
        return false;
    }
    state->phase = LIVE_MOVEMENT_ROUTE_PHASE_READY;
    return true;
}

const char *live_movement_route_failure(const live_movement_route_state_t *state) {
    return state != NULL && state->phase == LIVE_MOVEMENT_ROUTE_PHASE_FAILED ? state->failure
                                                                            : NULL;
}
