#include <toolkit/celestial_closure.h>
#include <toolkit/packet.h>

#include <stdlib.h>
#include <string.h>

#define require(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: requirement failed: %s\n",                  \
                    __FILE__,                                                     \
                    __LINE__,                                                     \
                    #condition);                                                  \
            return 1;                                                             \
        }                                                                         \
    } while (0)

/** Every face open and continuous, sky above. */
#define OPEN_AIR                                                                        \
    (CELESTIAL_CLOSURE_IN | UINT32_C(0x3ff) | CELESTIAL_CLOSURE_COVER_OPEN |            \
     CELESTIAL_CLOSURE_SKY_EXIT | CELESTIAL_CLOSURE_CONTINUOUS_NORTH |                  \
     CELESTIAL_CLOSURE_CONTINUOUS_EAST | CELESTIAL_CLOSURE_CONTINUOUS_SOUTH |           \
     CELESTIAL_CLOSURE_CONTINUOUS_WEST)

static uint32_t *values_of(size_t cells, uint32_t value) {
    uint32_t *values = malloc(cells * sizeof(*values));
    for (size_t i = 0; values != NULL && i < cells; i++) {
        values[i] = value;
    }
    return values;
}

static bool build_one(celestial_closure_t *closure, int8_t depth, uint8_t x, uint8_t y,
                      uint8_t width, uint8_t height, uint32_t value) {
    celestial_closure_fragment_t *fragment = calloc(1, sizeof(*fragment));
    if (fragment == NULL) {
        return false;
    }
    *fragment = (celestial_closure_fragment_t){.depth = depth, .x = x, .y = y, .width = width,
                                               .height = height, .slot = 3};
    fragment->values = values_of((size_t)width * height, value);
    return celestial_closure_build(closure, 7, 9, 9, fragment, 1);
}

static void model_at(uint16_t hour, celestial_model_t *model) {
    celestial_model_inputs_t inputs = {
        .day_linear = {65535, 65535, 65535},
        .night_linear = {32768, 49152, 65535},
        .day_brightness = 1024,
        .night_brightness = 256,
        .moon_linear = {49152, 57344, 65535},
        .moon_max = 20,
        .starlight_linear = {40960, 49152, 65535},
        .starlight_strength = 2,
        .lunar_period = 672,
        .solar_hour = hour,
        .season_phase = 0,
        .lunar_age = 336,
    };
    celestial_model_evaluate(&inputs, model);
}

static int test_round_trip(void) {
    celestial_closure_fragment_t *fragments = calloc(2, sizeof(*fragments));
    require(fragments != NULL);
    fragments[0] = (celestial_closure_fragment_t){.depth = 0, .x = 1, .y = 2, .width = 3,
                                                  .height = 2, .slot = 4};
    fragments[0].values = values_of(6, OPEN_AIR);
    fragments[0].values[4] = 0;
    fragments[1] = (celestial_closure_fragment_t){.depth = -6, .x = 0, .y = 0, .width = 2,
                                                  .height = 1, .slot = CELESTIAL_PROTOCOL_SLOT_NONE};
    fragments[1].values = values_of(2, CELESTIAL_CLOSURE_IN);
    celestial_closure_t closure;
    require(celestial_closure_build(&closure, 42, 9, 9, fragments, 2));
    packet_struct *packet = packet_new(0, 64, 0);
    require(celestial_closure_write(packet, &closure));
    require(packet->len == celestial_closure_size(&closure));
    celestial_closure_t decoded;
    require(celestial_closure_read(packet->data, packet->len, 9, 9, &decoded));
    require(decoded.generation == 42 && decoded.count == 2);
    for (size_t i = 0; i < decoded.count; i++) {
        const celestial_closure_fragment_t *a = &closure.fragments[i], *b = &decoded.fragments[i];
        require(a->depth == b->depth && a->x == b->x && a->y == b->y && a->width == b->width &&
                a->height == b->height && a->slot == b->slot);
        require(memcmp(a->values, b->values, (size_t)a->width * a->height * 4) == 0);
    }
    require(celestial_closure_value(&decoded, 0, 2, 3) == 0);
    require(celestial_closure_value(&decoded, 0, 1, 2) == OPEN_AIR);
    require(celestial_closure_value(&decoded, -6, 1, 0) == CELESTIAL_CLOSURE_IN);
    require(celestial_closure_value(&decoded, 1, 1, 2) == 0);

    /* Truncation and trailing bytes are rejected. */
    celestial_closure_t rejected;
    require(!celestial_closure_read(packet->data, packet->len - 1, 9, 9, &rejected));
    uint8_t *longer = malloc(packet->len + 1);
    require(longer != NULL);
    memcpy(longer, packet->data, packet->len);
    longer[packet->len] = 0;
    require(!celestial_closure_read(longer, packet->len + 1, 9, 9, &rejected));
    free(longer);
    /* A fragment beyond the window is rejected. */
    require(!celestial_closure_read(packet->data, packet->len, 3, 3, &rejected));
    packet_free(packet);
    celestial_closure_free(&decoded);
    celestial_closure_free(&closure);
    return 0;
}

/** Encode one fragment by hand: header, then the given runs. */
static size_t encode(uint8_t *out, int8_t depth, uint8_t x, uint8_t y, uint8_t width,
                     uint8_t height, const uint8_t (*runs)[5], size_t run_count) {
    size_t len = 0;
    out[len++] = 0;
    out[len++] = 0;
    out[len++] = 0;
    out[len++] = 1;
    out[len++] = 0;
    out[len++] = 1;
    out[len++] = (uint8_t)depth;
    out[len++] = x;
    out[len++] = y;
    out[len++] = width;
    out[len++] = height;
    out[len++] = 0;
    for (size_t i = 0; i < run_count; i++) {
        memcpy(&out[len], runs[i], 5);
        len += 5;
    }
    return len;
}

static int test_rejections(void) {
    uint8_t buffer[64];
    celestial_closure_t closure;
    static const uint8_t ok[][5] = {{4, 0x00, 0x00, 0x80, 0x00}};
    require(celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, ok, 1), 4, 4, &closure));
    celestial_closure_free(&closure);
    /* A run of length 0. */
    static const uint8_t zero[][5] = {{0, 0x00, 0x00, 0x80, 0x00}, {4, 0x00, 0x00, 0x80, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, zero, 2), 4, 4,
                                    &closure));
    /* A run past the rectangle. */
    static const uint8_t over[][5] = {{5, 0x00, 0x00, 0x80, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, over, 1), 4, 4,
                                    &closure));
    /* Runs that do not cover the rectangle. */
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 3, 2, ok, 1), 4, 4,
                                    &closure));
    /* An empty rectangle and a depth outside the window. */
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 0, 2, ok, 0), 4, 4,
                                    &closure));
    require(!celestial_closure_read(buffer, encode(buffer, 7, 0, 0, 2, 2, ok, 1), 4, 4,
                                    &closure));
    /* A nonzero value without the closure bit, and a sky exit that also
     * continues upward. */
    static const uint8_t outside[][5] = {{4, 0x00, 0x01, 0x00, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, outside, 1), 4, 4,
                                    &closure));
    static const uint8_t sky_up[][5] = {{4, 0x00, 0x42, 0x80, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, sky_up, 1), 4, 4,
                                    &closure));
    /* A reserved bit. */
    static const uint8_t reserved[][5] = {{4, 0x08, 0x00, 0x80, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, reserved, 1), 4, 4,
                                    &closure));
    /* Overlapping fragments of one depth. */
    uint8_t twice[64];
    size_t len = encode(twice, 0, 0, 0, 2, 2, ok, 1);
    twice[5] = 2;
    memcpy(&twice[len], &twice[6], len - 6);
    len += len - 6;
    require(!celestial_closure_read(twice, len, 4, 4, &closure));
    return 0;
}

static int test_open_air_solves_to_unobstructed_sources(void) {
    for (uint16_t hour = 0; hour < 24; hour += 3) {
        celestial_model_t model;
        model_at(hour, &model);
        celestial_closure_t closure;
        require(build_one(&closure, 0, 2, 2, 5, 4, OPEN_AIR));
        int32_t values[20], rgb[20][3];
        require(celestial_closure_solve(&closure, 0, &model, values, rgb));
        int32_t expected, expected_rgb[3];
        celestial_model_publish(&model, model.solar_elevation > 0 ? model.direct : 0,
                                model.diffuse, 0, model.moon_elevation > 0 ? model.moon : 0, 0,
                                model.starlight, &expected, expected_rgb);
        for (size_t i = 0; i < 20; i++) {
            require(values[i] == expected);
            require(memcmp(rgb[i], expected_rgb, sizeof(expected_rgb)) == 0);
        }
        celestial_closure_free(&closure);
    }
    return 0;
}

static int test_covered_cell_is_darker_and_glass_transmits(void) {
    celestial_model_t model;
    model_at(12, &model);
    /* A sealed cover with no sky exit admits only spill through open sides. */
    uint32_t covered = OPEN_AIR & ~(CELESTIAL_CLOSURE_COVER_OPEN | CELESTIAL_CLOSURE_SKY_EXIT);
    celestial_closure_t closure;
    require(build_one(&closure, 0, 2, 2, 5, 5, OPEN_AIR));
    closure.fragments[0].values[12] = covered;
    int32_t open[25], open_rgb[25][3];
    require(celestial_closure_solve(&closure, 0, &model, open, open_rgb));
    require(open[12] < open[0]);
    require(open[12] > 0);
    celestial_closure_free(&closure);

    /* A glass roof over a single-level cell: the downward face of a cell above. */
    celestial_closure_fragment_t *fragments = calloc(2, sizeof(*fragments));
    require(fragments != NULL);
    fragments[0] = (celestial_closure_fragment_t){.depth = 0, .x = 0, .y = 0, .width = 3,
                                                  .height = 3, .slot = 0};
    fragments[0].values = values_of(9, (OPEN_AIR & ~CELESTIAL_CLOSURE_SKY_EXIT) |
                                           CELESTIAL_CLOSURE_CONTINUOUS_UP);
    uint32_t glass_floor = (OPEN_AIR & ~UINT32_C(3)) | 1U | CELESTIAL_CLOSURE_CONTINUOUS_DOWN;
    fragments[1] = (celestial_closure_fragment_t){.depth = 1, .x = 0, .y = 0, .width = 3,
                                                  .height = 3, .slot = 0};
    fragments[1].values = values_of(9, glass_floor);
    require(celestial_closure_build(&closure, 1, 9, 9, fragments, 2));
    int32_t under[9], under_rgb[9][3];
    require(celestial_closure_solve(&closure, 0, &model, under, under_rgb));
    int32_t top[9], top_rgb[9][3];
    require(celestial_closure_solve(&closure, 1, &model, top, top_rgb));
    require(under[4] > 0 && under[4] < top[4]);
    celestial_closure_free(&closure);
    return 0;
}

static int test_overhead_face_of_an_unseen_cell_above(void) {
    celestial_model_t model;
    model_at(12, &model);
    uint32_t open_above = (OPEN_AIR & ~CELESTIAL_CLOSURE_SKY_EXIT) |
                          CELESTIAL_CLOSURE_CONTINUOUS_UP |
                          (UINT32_C(3) << CELESTIAL_CLOSURE_OVERHEAD_SHIFT);
    celestial_closure_t closure;
    require(build_one(&closure, 0, 2, 2, 5, 5, open_above));
    int32_t open[25], open_rgb[25][3];
    require(celestial_closure_solve(&closure, 0, &model, open, open_rgb));
    int32_t expected, expected_rgb[3];
    celestial_model_publish(&model, model.direct, model.diffuse, 0, 0, 0, model.starlight,
                            &expected, expected_rgb);
    /* An open floor above an unseen cell is open sky. */
    require(open[12] == expected);
    celestial_closure_free(&closure);

    /* An opaque roof above the middle cells shades them. */
    require(build_one(&closure, 0, 2, 2, 5, 5, open_above));
    for (int i = 0; i < 25; i++) {
        int x = i % 5, y = i / 5;
        if (x >= 1 && x <= 3 && y >= 1 && y <= 3) {
            closure.fragments[0].values[i] &= ~(UINT32_C(3) << CELESTIAL_CLOSURE_OVERHEAD_SHIFT);
        }
    }
    int32_t roofed[25], roofed_rgb[25][3];
    require(celestial_closure_solve(&closure, 0, &model, roofed, roofed_rgb));
    require(roofed[12] < open[12]);
    require(roofed[0] == open[0]);
    celestial_closure_free(&closure);
    return 0;
}

int main(void) {
    toolkit_import(packet);
    int result = test_round_trip();
    if (result == 0) {
        result = test_rejections();
    }
    if (result == 0) {
        result = test_open_air_solves_to_unobstructed_sources();
    }
    if (result == 0) {
        result = test_covered_cell_is_darker_and_glass_transmits();
    }
    if (result == 0) {
        result = test_overhead_face_of_an_unseen_cell_above();
    }
    toolkit_deinit();
    return result;
}
