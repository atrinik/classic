#include <toolkit/celestial_closure.h>
#include <toolkit/celestial_solver.h>
#include <toolkit/packet.h>

#include <limits.h>
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
    /* A sky exit beneath a closed cover, which the server never sends: the
     * sky face skips the cover, so it would light a sealed cell. */
    static const uint8_t sealed_sky[][5] = {{4, 0x00, 0x02, 0x80, 0x00}};
    require(!celestial_closure_read(buffer, encode(buffer, 0, 0, 0, 2, 2, sealed_sky, 1), 4, 4,
                                    &closure));
    require(!celestial_closure_value_valid(CELESTIAL_CLOSURE_IN | CELESTIAL_CLOSURE_SKY_EXIT));
    require(celestial_closure_value_valid(CELESTIAL_CLOSURE_IN | CELESTIAL_CLOSURE_SKY_EXIT |
                                          CELESTIAL_CLOSURE_COVER_OPEN));
    /* More fragment cells than non-overlapping fragments could cover: a 1x1
     * window has 13 cells over all depths, and 14 fragments claim 14. */
    {
        uint8_t many[6 + 14 * 11];
        size_t at = 0;
        many[at++] = 0; many[at++] = 0; many[at++] = 0; many[at++] = 7;
        many[at++] = 0; many[at++] = 14;
        for (int i = 0; i < 14; i++) {
            const uint8_t fragment[] = {0, 0, 0, 1, 1, 3, 1, 0x00, 0x00, 0x80, 0x00};
            memcpy(&many[at], fragment, sizeof(fragment));
            at += sizeof(fragment);
        }
        require(!celestial_closure_read(many, at, 1, 1, &closure));
    }
    /* A reserved bit (bit 28; bit 27 alone with bit 15 is an outside record). */
    static const uint8_t reserved[][5] = {{4, 0x10, 0x00, 0x80, 0x00}};
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

/** A 3 x 3 open map under the sky that counts its memory hooks. */
typedef struct counting_lattice {
    int map;
    size_t reserves, releases, reserved_bytes, released_bytes;
} counting_lattice;

static bool counting_resolve(void *context, void *origin, int x, int y, int z,
                             celestial_solver_cell_t *cell) {
    (void)context;
    if (z != 0 || x < 0 || y < 0 || x >= 3 || y >= 3) {
        return false;
    }
    *cell = (celestial_solver_cell_t){.map = origin, .x = x, .y = y, .z = 0};
    return true;
}

static void counting_dimensions(void *context, void *map, int *width, int *height) {
    (void)context;
    (void)map;
    *width = 3;
    *height = 3;
}

static bool counting_sky(void *context, const celestial_solver_face_t *face) {
    (void)context;
    return face->to.map == NULL;
}

static bool counting_continuous(void *context, const celestial_solver_face_t *face) {
    (void)context;
    return face->from.map != NULL && face->to.map != NULL;
}

static uint16_t counting_light(void *context, const celestial_solver_face_t *face,
                               bool *aperture) {
    (void)context;
    (void)face;
    if (aperture != NULL) {
        *aperture = false;
    }
    return CELESTIAL_MODEL_SCALE;
}

static bool counting_reserve(void *context, size_t bytes) {
    counting_lattice *lattice = context;
    lattice->reserves++;
    lattice->reserved_bytes += bytes;
    return true;
}

static void counting_release(void *context, size_t bytes) {
    counting_lattice *lattice = context;
    lattice->releases++;
    lattice->released_bytes += bytes;
}

static int test_release_pairs_with_reserve(void) {
    celestial_model_t model;
    model_at(12, &model);
    int32_t values[9], rgb[9][3];
    /* With both hooks, every charge is returned exactly once. */
    static const celestial_solver_ops_t paired = {
        .resolve = counting_resolve, .dimensions = counting_dimensions, .sky = counting_sky,
        .continuous = counting_continuous, .light = counting_light,
        .reserve = counting_reserve, .release = counting_release};
    counting_lattice counts = {0};
    celestial_solver_lattice_t lattice = {.ops = &paired, .context = &counts};
    require(celestial_solver_solve(&lattice, &counts.map, &model, values, rgb));
    require(counts.reserves > 0);
    require(counts.releases == counts.reserves && counts.released_bytes == counts.reserved_bytes);
    /* A reserve without its release is never called: the charge could not
     * be returned. */
    static const celestial_solver_ops_t reserve_only = {
        .resolve = counting_resolve, .dimensions = counting_dimensions, .sky = counting_sky,
        .continuous = counting_continuous, .light = counting_light,
        .reserve = counting_reserve};
    counts = (counting_lattice){0};
    lattice.ops = &reserve_only;
    require(celestial_solver_solve(&lattice, &counts.map, &model, values, rgb));
    require(counts.reserves == 0);
    /* Without reserve nothing was charged, so nothing is released. */
    static const celestial_solver_ops_t release_only = {
        .resolve = counting_resolve, .dimensions = counting_dimensions, .sky = counting_sky,
        .continuous = counting_continuous, .light = counting_light,
        .release = counting_release};
    counts = (counting_lattice){0};
    lattice.ops = &release_only;
    require(celestial_solver_solve(&lattice, &counts.map, &model, values, rgb));
    require(counts.releases == 0);
    return 0;
}

static int test_outside_records_and_sky_exits(void) {
    /* An outside record is valid only alone, and solves exactly as a cell
     * of open air outside the closure. */
    require(celestial_closure_value_valid(CELESTIAL_CLOSURE_OUTSIDE_RECORD));
    require(!celestial_closure_value_valid(CELESTIAL_CLOSURE_OUTSIDE_RECORD |
                                           CELESTIAL_CLOSURE_COVER_OPEN));
    require(!celestial_closure_value_valid(CELESTIAL_CLOSURE_OUTSIDE));
    celestial_model_t model;
    model_at(12, &model);
    celestial_closure_t outside, open;
    require(build_one(&outside, 0, 2, 2, 5, 5, CELESTIAL_CLOSURE_OUTSIDE_RECORD));
    require(build_one(&open, 0, 2, 2, 5, 5, OPEN_AIR));
    int32_t a[25], a_rgb[25][3], b[25], b_rgb[25][3];
    require(celestial_closure_solve(&outside, 0, &model, a, a_rgb));
    require(celestial_closure_solve(&open, 0, &model, b, b_rgb));
    require(memcmp(a, b, sizeof(a)) == 0 && memcmp(a_rgb, b_rgb, sizeof(a_rgb)) == 0);
    require(a[12] > 0);
    celestial_closure_free(&outside);
    celestial_closure_free(&open);
    /* A sky exit beneath a closure cell is rejected; beneath an outside
     * record it stands. */
    for (int above = 0; above < 2; above++) {
        celestial_closure_fragment_t *fragments = calloc(2, sizeof(*fragments));
        require(fragments != NULL);
        fragments[0] = (celestial_closure_fragment_t){.depth = 0, .x = 1, .y = 1, .width = 1,
                                                      .height = 1, .slot = 3};
        fragments[0].values = values_of(1, OPEN_AIR);
        fragments[1] = (celestial_closure_fragment_t){.depth = 1, .x = 1, .y = 1, .width = 1,
                                                      .height = 1, .slot = 3};
        fragments[1].values =
            values_of(1, above == 0 ? (OPEN_AIR & ~CELESTIAL_CLOSURE_SKY_EXIT)
                                    : CELESTIAL_CLOSURE_OUTSIDE_RECORD);
        celestial_closure_t closure;
        bool built = celestial_closure_build(&closure, 7, 9, 9, fragments, 2);
        require(built == (above == 1));
        celestial_closure_free(&closure);
    }
    return 0;
}

static const celestial_solver_ops_t paired_open_ops = {
    .resolve = counting_resolve, .dimensions = counting_dimensions, .sky = counting_sky,
    .continuous = counting_continuous, .light = counting_light};

static int test_seams_between_profiles_are_closed(void) {
    /* Two adjacent fragments claiming a continuous seam: with one profile
     * slot light crosses it, with two it does not, as on the server. */
    celestial_model_t model;
    model_at(12, &model);
    int32_t field[2][9], field_rgb[2][9][3];
    for (int variant = 0; variant < 2; variant++) {
        celestial_closure_fragment_t *fragments = calloc(2, sizeof(*fragments));
        require(fragments != NULL);
        uint32_t covered = OPEN_AIR & ~(CELESTIAL_CLOSURE_COVER_OPEN | CELESTIAL_CLOSURE_SKY_EXIT);
        fragments[0] = (celestial_closure_fragment_t){.depth = 0, .x = 1, .y = 3, .width = 3,
                                                      .height = 3, .slot = 3};
        fragments[0].values = values_of(9, covered);
        fragments[1] = (celestial_closure_fragment_t){.depth = 0, .x = 4, .y = 3, .width = 3,
                                                      .height = 3,
                                                      .slot = (uint8_t)(variant == 0 ? 3 : 4)};
        fragments[1].values = values_of(9, OPEN_AIR);
        celestial_closure_t closure;
        require(celestial_closure_build(&closure, 7, 9, 9, fragments, 2));
        require(celestial_closure_solve(&closure, 0, &model, field[variant], field_rgb[variant]));
        celestial_closure_free(&closure);
    }
    /* The middle of the covered fragment's east edge receives spill from
     * the open one only across a continuous seam. */
    require(field[0][5] > field[1][5]);
    return 0;
}

static int test_continuity_is_symmetric(void) {
    /* Two adjacent closure cells disagreeing about their seam: light crosses
     * it neither way, as if both called it discontinuous. */
    celestial_model_t model;
    model_at(12, &model);
    uint32_t sealed_east = OPEN_AIR & ~CELESTIAL_CLOSURE_CONTINUOUS_EAST;
    int32_t field[2][9], field_rgb[2][9][3];
    for (int variant = 0; variant < 2; variant++) {
        celestial_closure_fragment_t *fragment = calloc(1, sizeof(*fragment));
        require(fragment != NULL);
        *fragment = (celestial_closure_fragment_t){.depth = 0, .x = 3, .y = 3, .width = 3,
                                                   .height = 3, .slot = 3};
        fragment->values = values_of(9, OPEN_AIR);
        /* Variant 0: only the west cell says sealed; variant 1: both do. */
        fragment->values[3] = sealed_east;
        if (variant == 1) {
            fragment->values[4] = OPEN_AIR & ~CELESTIAL_CLOSURE_CONTINUOUS_WEST;
        }
        celestial_closure_t closure;
        require(celestial_closure_build(&closure, 7, 9, 9, fragment, 1));
        require(celestial_closure_solve(&closure, 0, &model, field[variant], field_rgb[variant]));
        celestial_closure_free(&closure);
    }
    require(memcmp(field[0], field[1], sizeof(field[0])) == 0);
    return 0;
}

static int invalid_width;

static void invalid_dimensions(void *context, void *map, int *width, int *height) {
    (void)context;
    (void)map;
    *width = invalid_width;
    *height = 3;
}

static int test_invalid_inputs_fail_without_crashing(void) {
    celestial_model_t model;
    model_at(12, &model);
    int32_t values[9], rgb[9][3];
    /* Dimensions outside 1..CELESTIAL_SOLVER_DIMENSION_MAX fail the solve
     * before any size arithmetic. */
    static const celestial_solver_ops_t invalid = {
        .resolve = counting_resolve, .dimensions = invalid_dimensions, .sky = counting_sky,
        .continuous = counting_continuous, .light = counting_light};
    counting_lattice counts = {0};
    celestial_solver_lattice_t lattice = {.ops = &invalid, .context = &counts};
    static const int widths[] = {0, -1, CELESTIAL_SOLVER_DIMENSION_MAX + 1, INT_MAX};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        invalid_width = widths[i];
        require(!celestial_solver_solve(&lattice, &counts.map, &model, values, rgb));
        require(celestial_solver_scratch_width(&lattice, &counts.map) == 0);
    }
    /* A count no payload of this length can hold is malformed, decided
     * before any allocation, never reported as out of memory. */
    static const uint8_t claims_many[] = {0, 0, 0, 1, 0xff, 0xff};
    bool out_of_memory = true;
    celestial_closure_t decoded;
    require(!celestial_closure_decode(claims_many, sizeof(claims_many), 9, 9, &decoded,
                                      &out_of_memory));
    require(!out_of_memory);
    /* One 255x255 fragment with a single 255-cell run: truncated, found so
     * before its array is allocated. */
    static const uint8_t truncated_runs[] = {0, 0, 0, 1, 0, 1, 0, 0, 0, 255, 255, 3,
                                             255, 0, 0, 0x80, 0};
    out_of_memory = true;
    require(!celestial_closure_decode(truncated_runs, sizeof(truncated_runs), 255, 255,
                                      &decoded, &out_of_memory));
    require(!out_of_memory);
    /* A well-formed first fragment before a malformed second one: the whole
     * payload is framed before anything is allocated. */
    static const uint8_t bad_second[] = {0, 0, 0, 1, 0, 2, 0, 0, 0, 2, 2, 3, 4, 0, 0, 0x80, 0,
                                         0, 0, 0, 0, 0, 3};
    out_of_memory = true;
    require(!celestial_closure_decode(bad_second, sizeof(bad_second), 9, 9, &decoded,
                                      &out_of_memory));
    require(!out_of_memory);
    /* Only a built closure is encoded. */
    celestial_closure_t unbuilt = {.window_width = 9, .window_height = 9};
    packet_struct *refused = packet_new(0, 64, 0);
    require(!celestial_closure_write(refused, &unbuilt));
    require(refused->len == 0);
    packet_free(refused);
    /* A count without its fragment array is rejected and cleaned up. */
    celestial_closure_t closure;
    require(!celestial_closure_build(&closure, 1, 9, 9, NULL, 3));
    celestial_closure_free(&closure);
    /* An origin beyond the coordinate bound is rejected up front, so no
     * cursor arithmetic can overflow. */
    celestial_solver_cursor_t cursor;
    require(!celestial_solver_direction_init(&lattice, &cursor, &counts.map,
                                             CELESTIAL_SOLVER_COORDINATE_MAX + 1, 1, 1, 0, 0, 4));
    require(!celestial_solver_direction_init(&lattice, &cursor, &counts.map, 1, INT_MIN, 1, 0, 0,
                                             4));
    /* An azimuth outside the eight directions traces nothing either. */
    lattice.ops = &paired_open_ops;
    celestial_solver_ray_t probe;
    celestial_solver_ray_init(&lattice, &probe, &counts.map, 1, 1, 0, 16384);
    require(probe.done);
    celestial_solver_ray_init(&lattice, &probe, &counts.map, 1, 1, CELESTIAL_MODEL_NORTHWEST + 1,
                              16384);
    require(probe.done);
    celestial_solver_ray_init(&lattice, &probe, &counts.map, 1, 1, CELESTIAL_MODEL_NORTH, 16384);
    require(!probe.done);
    /* An elevation above the Q15 zenith traces nothing. */
    static const celestial_solver_ops_t open_ops = {
        .resolve = counting_resolve, .dimensions = counting_dimensions, .sky = counting_sky,
        .continuous = counting_continuous, .light = counting_light};
    lattice.ops = &open_ops;
    celestial_solver_ray_t ray;
    celestial_solver_ray_init(&lattice, &ray, &counts.map, 1, 1, CELESTIAL_MODEL_NORTH, 32769);
    require(ray.done);
    celestial_solver_ray_init(&lattice, &ray, &counts.map, 1, 1, CELESTIAL_MODEL_NORTH, 32768);
    require(!ray.done);
    /* A writer failing part way leaves no partial payload. */
    require(build_one(&closure, 0, 0, 0, 3, 3, OPEN_AIR));
    packet_struct *packet = packet_new(0, 64, 0);
    packet_writer_write_uint8(packet, 7);
    packet_writer_set_limit(packet, 8);
    require(!celestial_closure_write(packet, &closure));
    require(packet->len == 1);
    packet_free(packet);
    packet = packet_new(0, 64, 0);
    require(celestial_closure_write(packet, &closure));
    packet_free(packet);
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
    if (result == 0) {
        result = test_release_pairs_with_reserve();
    }
    if (result == 0) {
        result = test_invalid_inputs_fail_without_crashing();
    }
    if (result == 0) {
        result = test_outside_records_and_sky_exits();
    }
    if (result == 0) {
        result = test_continuity_is_symmetric();
    }
    if (result == 0) {
        result = test_seams_between_profiles_are_closed();
    }
    toolkit_deinit();
    return result;
}
