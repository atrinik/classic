#include <toolkit/porting.h>

#include <live_movement_route.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(expression)                    \
    do {                                       \
        if (!(expression)) {                   \
            fprintf(stderr, "%d\n", __LINE__); \
            return 1;                          \
        }                                      \
    } while (0)

#ifdef WIN32
#define LIVE_ROUTE_TEST_TEMP "atrinik-live-route-XXXXXX"
#else
#define LIVE_ROUTE_TEST_TEMP "/tmp/atrinik-live-route-XXXXXX"
#endif

static const char route_xml[] =
    "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
    "<checkpoint map=\"/maps/a\" x=\"10\" y=\"10\" direction=\"0\"/>"
    "<checkpoint map=\"/maps/a\" x=\"11\" y=\"10\" direction=\"6\"/>"
    "<checkpoint map=\"/maps/a\" x=\"11\" y=\"9\" direction=\"8\"/>"
    "<checkpoint map=\"/maps/b\" x=\"0\" y=\"0\" direction=\"6\"/>"
    "</live-movement-route>";

static bool load_xml(const char *xml, live_movement_route_t **route) {
    char path[] = LIVE_ROUTE_TEST_TEMP;
    int fd = mkstemp(path);
    if (fd < 0) {
        return false;
    }
    size_t size = strlen(xml);
    size_t written = 0;
    while (written < size) {
        ssize_t amount = write(fd, xml + written, size - written);
        if (amount <= 0) {
            close(fd);
            unlink(path);
            return false;
        }
        written += (size_t)amount;
    }
    close(fd);
    char error[256];
    bool loaded = live_movement_route_load(path, route, error, sizeof(error));
    unlink(path);
    return loaded;
}

static char *build_route(size_t checkpoints) {
    size_t capacity = 160U + checkpoints * 80U;
    char *xml = malloc(capacity);
    if (xml == NULL) {
        return NULL;
    }
    size_t used = (size_t)snprintf(
        xml,
        capacity,
        "<live-movement-route version=\"1\" timeout-ms=\"3600000\" step-timeout-ms=\"60000\">");
    for (size_t i = 0; i < checkpoints; i++) {
        int written = snprintf(xml + used,
                               capacity - used,
                               "<checkpoint map=\"/m%zu\" x=\"0\" y=\"0\" direction=\"%u\"/>",
                               i,
                               i == 0U ? 0U : 6U);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(xml);
            return NULL;
        }
        used += (size_t)written;
    }
    int written = snprintf(xml + used, capacity - used, "</live-movement-route>");
    if (written < 0 || (size_t)written >= capacity - used) {
        free(xml);
        return NULL;
    }
    return xml;
}

static char *add_destination(const char *xml) {
    static const char close[] = "</live-movement-route>";
    static const char destination[] =
        "<checkpoint map=\"/destination\" x=\"0\" y=\"0\" direction=\"6\"/>";
    const char *position = strstr(xml, close);
    if (position == NULL) {
        return NULL;
    }
    size_t prefix = (size_t)(position - xml);
    size_t size = strlen(xml) + sizeof(destination);
    char *result = malloc(size);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, xml, prefix);
    memcpy(result + prefix, destination, sizeof(destination) - 1U);
    memcpy(result + prefix + sizeof(destination) - 1U, position, strlen(position) + 1U);
    return result;
}

static live_movement_route_observation_t
observation(uint64_t now, const char *map, uint8_t x, uint8_t y, uint64_t generation) {
    return (live_movement_route_observation_t){
        .now_ms = now,
        .connected = true,
        .play = true,
        .published_ready = true,
        .map = map,
        .x = x,
        .y = y,
        .publication_generation = generation,
    };
}

static live_movement_route_state_t *state_at_start(const live_movement_route_t *route,
                                                   uint64_t now) {
    live_movement_route_state_t *state = live_movement_route_state_create(route, now);
    live_movement_route_observation_t current = observation(now, "/maps/a", 10, 10, 1);
    live_movement_route_action_t action = live_movement_route_tick(state, &current);
    if (action.type != LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL || action.checkpoint_index != 0U) {
        live_movement_route_state_free(state);
        return NULL;
    }
    return state;
}

static int test_parser(void) {
    live_movement_route_t *route = NULL;
    REQUIRE(load_xml(route_xml, &route));
    REQUIRE(live_movement_route_checkpoint_count(route) == 4U);
    REQUIRE(live_movement_route_timeout_ms(route) == 100U);
    REQUIRE(live_movement_route_step_timeout_ms(route) == 10U);
    REQUIRE(live_movement_route_sha256(route) != NULL);
    REQUIRE(strlen(live_movement_route_sha256(route)) == 64U);
    live_movement_route_free(route);

#ifndef WIN32
    char fifo_path[] = "/tmp/atrinik-live-route-fifo-XXXXXX";
    int fifo_fd = mkstemp(fifo_path);
    REQUIRE(fifo_fd >= 0);
    close(fifo_fd);
    REQUIRE(unlink(fifo_path) == 0);
    REQUIRE(mkfifo(fifo_path, 0600) == 0);
    REQUIRE(!live_movement_route_load(fifo_path, &route, NULL, 0));
    REQUIRE(unlink(fifo_path) == 0);
#endif

    char oversized_path[] = LIVE_ROUTE_TEST_TEMP;
    int oversized_fd = mkstemp(oversized_path);
    REQUIRE(oversized_fd >= 0);
    close(oversized_fd);
    FILE *oversized = fopen(oversized_path, "wb");
    REQUIRE(oversized != NULL);
    REQUIRE(fseek(oversized, (long)LIVE_MOVEMENT_ROUTE_FILE_MAX, SEEK_SET) == 0);
    REQUIRE(fputc(0, oversized) == 0);
    REQUIRE(fclose(oversized) == 0);
    REQUIRE(!live_movement_route_load(oversized_path, &route, NULL, 0));
    REQUIRE(unlink(oversized_path) == 0);

    char map[LIVE_MOVEMENT_ROUTE_MAP_MAX + 2U];
    map[0] = '/';
    memset(map + 1, 'a', LIVE_MOVEMENT_ROUTE_MAP_MAX);
    map[LIVE_MOVEMENT_ROUTE_MAP_MAX] = '\0';
    char boundary_xml[1024];
    int boundary_size = snprintf(boundary_xml,
                                 sizeof(boundary_xml),
                                 "<live-movement-route version=\"1\" timeout-ms=\"3600000\" "
                                 "step-timeout-ms=\"60000\"><checkpoint map=\"%s\" x=\"255\" "
                                 "y=\"255\" direction=\"0\"/><checkpoint map=\"/destination\" "
                                 "x=\"0\" y=\"0\" direction=\"6\"/></live-movement-route>",
                                 map);
    REQUIRE(boundary_size > 0 && (size_t)boundary_size < sizeof(boundary_xml));
    REQUIRE(load_xml(boundary_xml, &route));
    live_movement_route_free(route);
    map[LIVE_MOVEMENT_ROUTE_MAP_MAX] = 'a';
    map[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1U] = '\0';
    boundary_size = snprintf(boundary_xml,
                             sizeof(boundary_xml),
                             "<live-movement-route version=\"1\" timeout-ms=\"1\" "
                             "step-timeout-ms=\"1\"><checkpoint map=\"%s\" x=\"0\" y=\"0\" "
                             "direction=\"0\"/><checkpoint map=\"/destination\" x=\"0\" "
                             "y=\"0\" direction=\"6\"/></live-movement-route>",
                             map);
    REQUIRE(boundary_size > 0 && (size_t)boundary_size < sizeof(boundary_xml));
    REQUIRE(!load_xml(boundary_xml, &route));

    char *maximum = build_route(LIVE_MOVEMENT_ROUTE_STEPS_MAX);
    REQUIRE(maximum != NULL);
    REQUIRE(load_xml(maximum, &route));
    REQUIRE(live_movement_route_checkpoint_count(route) == LIVE_MOVEMENT_ROUTE_STEPS_MAX);
    live_movement_route_free(route);
    free(maximum);
    char *too_many = build_route(LIVE_MOVEMENT_ROUTE_STEPS_MAX + 1U);
    REQUIRE(too_many != NULL);
    REQUIRE(!load_xml(too_many, &route));
    free(too_many);

    static const char *const malformed[] = {
        "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\"/>",
        "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"2\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/></live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"0\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/></live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"11\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/></live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"010\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/></live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\" "
        "extra=\"x\"><checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route xmlns=\"urn:x\" version=\"1\" timeout-ms=\"10\" "
        "step-timeout-ms=\"1\"><checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<!DOCTYPE live-movement-route [<!ENTITY x \"/a\">]><live-movement-route "
        "version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\"><checkpoint map=\"&x;\" "
        "x=\"0\" y=\"0\" direction=\"0\"/></live-movement-route>",
        "<!--x--><live-movement-route version=\"1\" timeout-ms=\"10\" "
        "step-timeout-ms=\"1\"><checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">text"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<unknown/></live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/../a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a b\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"256\" y=\"0\" direction=\"0\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"1\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "<checkpoint map=\"/a\" x=\"2\" y=\"0\" direction=\"6\"/>"
        "</live-movement-route>",
        "<live-movement-route version=\"1\" timeout-ms=\"10\" step-timeout-ms=\"1\">"
        "<checkpoint map=\"/a\" x=\"0\" y=\"0\" direction=\"0\"/>"
        "<checkpoint map=\"/a\" x=\"1\" y=\"0\" direction=\"5\"/>"
        "</live-movement-route>",
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        char *expanded = i >= 2U ? add_destination(malformed[i]) : NULL;
        const char *candidate = expanded != NULL ? expanded : malformed[i];
        route = NULL;
        REQUIRE(!load_xml(candidate, &route));
        REQUIRE(route == NULL);
        free(expanded);
    }
    return 0;
}

static int test_state_progression(const live_movement_route_t *route) {
    live_movement_route_state_t *state = live_movement_route_state_create(route, 100U);
    REQUIRE(state != NULL);
    live_movement_route_observation_t current = {0};
    current.now_ms = 101U;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.connected = true;
    current.play = true;
    current.published_ready = true;
    current.map = "/maps/a";
    current.x = 10;
    current.y = 10;
    REQUIRE(current.publication_generation == 0U);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.connected = true;
    current.play = true;
    current.published_ready = false;
    current.map = "/maps/wrong";
    current.x = 99;
    current.y = 99;
    current.publication_generation = 1;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.published_ready = true;
    current.map = "/maps/a";
    current.x = 10;
    current.y = 10;
    live_movement_route_action_t action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(action.checkpoint_index == 0U);
    current.now_ms++;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    REQUIRE(live_movement_route_arrival_presented(state));
    REQUIRE(!live_movement_route_arrival_presented(state));
    current.now_ms++;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    REQUIRE(action.direction == 6U && action.checkpoint_index == 1U);
    current.now_ms++;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.published_ready = false;
    current.map = "/maps/a";
    current.x = 11;
    current.publication_generation = 2;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.published_ready = true;
    current.publication_generation = 1;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.publication_generation = 2;
    current.x = 10;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current.x = 11;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(action.checkpoint_index == 1U);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms++;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    REQUIRE(action.direction == 8U && action.checkpoint_index == 2U);
    current.now_ms++;
    current.y = 9;
    current.publication_generation = 3;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(action.checkpoint_index == 2U);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms++;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    REQUIRE(action.direction == 6U && action.checkpoint_index == 3U);
    current.now_ms++;
    current.map = "/maps/b";
    current.x = 0;
    current.y = 0;
    current.publication_generation = 4;
    action = live_movement_route_tick(state, &current);
    REQUIRE(action.type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(action.checkpoint_index == 3U);
    current.now_ms++;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms++;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_DONE);
    live_movement_route_state_free(state);
    return 0;
}

static int test_failures(const live_movement_route_t *route) {
    live_movement_route_state_t *state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    live_movement_route_observation_t current = observation(1, "/maps/wrong", 10, 10, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "checkpoint 0") != NULL);
    live_movement_route_state_free(state);

    state = state_at_start(route, 0);
    REQUIRE(state != NULL && live_movement_route_arrival_presented(state));
    current = observation(1, "/maps/a", 10, 10, 1);
    current.run_on = true;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "modes") != NULL);
    live_movement_route_state_free(state);

    state = state_at_start(route, 0);
    REQUIRE(state != NULL && live_movement_route_arrival_presented(state));
    current = observation(1, "/maps/a", 10, 10, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(2, "/maps/a", 10, 11, 2);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "neither") != NULL);
    live_movement_route_state_free(state);

    state = state_at_start(route, 0);
    REQUIRE(state != NULL && live_movement_route_arrival_presented(state));
    current = observation(1, "/maps/a", 10, 10, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current.now_ms = 11;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "step") != NULL);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 100);
    REQUIRE(state != NULL);
    current = (live_movement_route_observation_t){.now_ms = 200};
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "total") != NULL);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 10);
    REQUIRE(state != NULL);
    current = (live_movement_route_observation_t){.now_ms = 9};
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "regressed") != NULL);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    current = (live_movement_route_observation_t){.now_ms = 1};
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_NONE);
    current = observation(2, "/maps/a", 10, 10, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    current.connected = false;
    current.play = false;
    current.now_ms = 3;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    REQUIRE(strstr(live_movement_route_failure(state), "disconnected") != NULL);
    live_movement_route_state_free(state);
    return 0;
}

static int test_wilderness_transition_diagnostic(void) {
    static const char transition_xml[] =
        "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
        "<checkpoint map=\"/wilderness/0_68\" x=\"23\" y=\"18\" direction=\"0\"/>"
        "<checkpoint map=\"/wilderness/1_68\" x=\"0\" y=\"18\" direction=\"6\"/>"
        "</live-movement-route>";
    live_movement_route_t *route = NULL;
    REQUIRE(load_xml(transition_xml, &route));

    live_movement_route_state_t *state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    live_movement_route_observation_t current = observation(1, "/wilderness/0_68", 23, 18, 67);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    current.publication_generation = 68;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, "/wilderness/1_68", 0, 18, 69);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    current = observation(1, "/wilderness/0_68", 23, 18, 67);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    current.publication_generation = 68;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, "/wilderness/unexpected_68", 4, 7, 72);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    const char *failure = live_movement_route_failure(state);
    REQUIRE(failure != NULL);
    REQUIRE(strcmp(failure,
                   "published position is neither source nor destination: "
                   "actual=/wilderness/unexpected_68(4,7), "
                   "source=/wilderness/0_68(23,18), target=/wilderness/1_68(0,18), "
                   "dispatched_publication_generation=68, "
                   "committed_publication_generation=72") == 0);
    live_movement_route_state_free(state);
    live_movement_route_free(route);
    return 0;
}

static int test_maximum_map_diagnostic(void) {
    char source[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1U];
    char destination[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1U];
    char actual[LIVE_MOVEMENT_ROUTE_MAP_MAX + 1U];
    source[0] = destination[0] = actual[0] = '/';
    memset(source + 1, 'a', LIVE_MOVEMENT_ROUTE_MAP_MAX - 1U);
    memset(destination + 1, 'b', LIVE_MOVEMENT_ROUTE_MAP_MAX - 1U);
    memset(actual + 1, 'c', LIVE_MOVEMENT_ROUTE_MAP_MAX - 1U);
    source[LIVE_MOVEMENT_ROUTE_MAP_MAX] = '\0';
    destination[LIVE_MOVEMENT_ROUTE_MAP_MAX] = '\0';
    actual[LIVE_MOVEMENT_ROUTE_MAP_MAX] = '\0';

    char xml[2048];
    int xml_size =
        snprintf(xml,
                 sizeof(xml),
                 "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
                 "<checkpoint map=\"%s\" x=\"255\" y=\"255\" direction=\"0\"/>"
                 "<checkpoint map=\"%s\" x=\"0\" y=\"0\" direction=\"6\"/>"
                 "</live-movement-route>",
                 source,
                 destination);
    REQUIRE(xml_size > 0 && (size_t)xml_size < sizeof(xml));
    live_movement_route_t *route = NULL;
    REQUIRE(load_xml(xml, &route));
    live_movement_route_state_t *state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    live_movement_route_observation_t current = observation(1, source, 255, 255, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    current.publication_generation = UINT64_MAX - 1U;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, actual, 127, 63, UINT64_MAX);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    const char *failure = live_movement_route_failure(state);
    REQUIRE(failure != NULL);
    REQUIRE(strstr(failure, actual) != NULL);
    REQUIRE(strstr(failure, source) != NULL);
    REQUIRE(strstr(failure, destination) != NULL);
    REQUIRE(strstr(failure, "actual=") != NULL);
    REQUIRE(strstr(failure, "(127,63)") != NULL);
    REQUIRE(strstr(failure, "(255,255)") != NULL);
    REQUIRE(strstr(failure, "(0,0)") != NULL);
    REQUIRE(strstr(failure, "dispatched_publication_generation=18446744073709551614") != NULL);
    REQUIRE(strstr(failure, "committed_publication_generation=18446744073709551615") != NULL);
    live_movement_route_state_free(state);
    live_movement_route_free(route);
    return 0;
}

static int test_final_presentation(void) {
    static const char final_xml[] =
        "<live-movement-route version=\"1\" timeout-ms=\"100\" step-timeout-ms=\"10\">"
        "<checkpoint map=\"/only\" x=\"1\" y=\"2\" direction=\"0\"/>"
        "<checkpoint map=\"/only\" x=\"2\" y=\"2\" direction=\"6\"/>"
        "</live-movement-route>";
    live_movement_route_t *route = NULL;
    REQUIRE(load_xml(final_xml, &route));
    live_movement_route_state_t *state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    live_movement_route_observation_t current = observation(1, "/only", 1, 2, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, "/only", 2, 2, 2);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    live_movement_route_observation_t wrong = observation(4, "/only", 3, 2, 3);
    REQUIRE(live_movement_route_tick(state, &wrong).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    current = observation(1, "/only", 1, 2, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, "/only", 2, 2, 2);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 4;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_DONE);
    live_movement_route_state_free(state);

    state = live_movement_route_state_create(route, 0);
    REQUIRE(state != NULL);
    current = observation(1, "/only", 1, 2, 1);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current.now_ms = 2;
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_MOVE);
    current = observation(3, "/only", 2, 2, 2);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_ARRIVAL);
    REQUIRE(live_movement_route_arrival_presented(state));
    current = observation(4, "/only", 3, 2, 3);
    REQUIRE(live_movement_route_tick(state, &current).type == LIVE_MOVEMENT_ROUTE_ACTION_FAILED);
    live_movement_route_state_free(state);
    live_movement_route_free(route);
    return 0;
}

int main(void) {
    REQUIRE(test_parser() == 0);
    live_movement_route_t *route = NULL;
    REQUIRE(load_xml(route_xml, &route));
    REQUIRE(test_state_progression(route) == 0);
    REQUIRE(test_failures(route) == 0);
    live_movement_route_free(route);
    REQUIRE(test_wilderness_transition_diagnostic() == 0);
    REQUIRE(test_maximum_map_diagnostic() == 0);
    REQUIRE(test_final_presentation() == 0);
    return 0;
}
