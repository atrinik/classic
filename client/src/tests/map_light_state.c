/*************************************************************************
 * Atrinik client map-light state regression tests.
 *
 * Copyright 2026 The Atrinik Project
 ************************************************************************/

#include <global.h>

#define TEST_CHECK(condition) \
    do {                      \
        if (!(condition)) {   \
            abort();          \
        }                     \
    } while (0)

int main(void) {
    MapCell cell = {0};
    cell.light_radiance[2] = 128;
    cell.light_known[2] = 1;
    cell.light_rgb_radiance[2][0] = 128;
    cell.light_rgb_radiance[2][1] = 0;
    cell.light_rgb_radiance[2][2] = 0;
    cell.light_rgb_explicit = UINT8_C(1) << 2;

    map_cell_clear_light_state(&cell);
    TEST_CHECK(cell.light_radiance[2] == 128);
    TEST_CHECK(cell.light_known[2] == 0);
    TEST_CHECK(cell.light_rgb_explicit == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][0] == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][1] == 0);
    TEST_CHECK(cell.light_rgb_radiance[2][2] == 0);
    TEST_CHECK(sizeof(cell.light_radiance) + sizeof(cell.light_rgb_radiance) == 56);

    TEST_CHECK(map_layer_is_remembered(LAYER_FLOOR));
    TEST_CHECK(map_layer_is_remembered(LAYER_FMASK));
    TEST_CHECK(map_layer_is_remembered(LAYER_WALL));
    TEST_CHECK(!map_layer_is_remembered(LAYER_ITEM));
    TEST_CHECK(!map_layer_is_remembered(LAYER_ITEM2));
    TEST_CHECK(!map_layer_is_remembered(LAYER_LIVING));
    TEST_CHECK(!map_layer_is_remembered(LAYER_EFFECT));

    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        cell.door[sub_layer] = UINT8_MAX;
        cell.exit[sub_layer] = UINT8_MAX;
        cell.priority[sub_layer] = UINT8_MAX;
        cell.secondpass[sub_layer] = UINT8_MAX;
        snprintf(cell.pname[sub_layer], sizeof(cell.pname[sub_layer]), "actor");
        snprintf(cell.pcolor[sub_layer], sizeof(cell.pcolor[sub_layer]), "red");
        cell.probe[sub_layer] = 1;
        cell.target_object_count[sub_layer] = 2;
        cell.target_is_friend[sub_layer] = 1;
        cell.anim_flags[sub_layer] = 1;
    }
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = 1; object_layer <= NUM_LAYERS; object_layer++) {
            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            cell.faces[layer] = (int16_t)(layer + 1);
            cell.flags[layer] = UINT8_MAX;
            cell.roof[layer] = 1;
            cell.quick_pos[layer] = 1;
            cell.height[layer] = 1;
            cell.zoom_x[layer] = 2;
            cell.zoom_y[layer] = 2;
            cell.align[layer] = 3;
            cell.rotate[layer] = 4;
            cell.infravision[layer] = 1;
            cell.draw_double[layer] = 1;
            cell.alpha[layer] = 5;
            cell.anim_last[layer] = 1;
            cell.anim_speed[layer] = 2;
            cell.anim_facing[layer] = 3;
            cell.anim_state[layer] = 4;
            cell.glow_speed[layer] = 2;
            cell.glow_state[layer] = 1;
        }
    }

    map_cell_clear_live_state(&cell);
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        TEST_CHECK(cell.door[sub_layer] ==
                   ((UINT8_C(1) << (LAYER_FLOOR - 1)) |
                    (UINT8_C(1) << (LAYER_FMASK - 1)) |
                    (UINT8_C(1) << (LAYER_WALL - 1))));
        TEST_CHECK(cell.exit[sub_layer] == cell.door[sub_layer]);
        TEST_CHECK(cell.priority[sub_layer] == cell.door[sub_layer]);
        TEST_CHECK(cell.secondpass[sub_layer] == cell.door[sub_layer]);
        TEST_CHECK(cell.pname[sub_layer][0] == '\0');
        TEST_CHECK(cell.pcolor[sub_layer][0] == '\0');
        TEST_CHECK(cell.probe[sub_layer] == 0);
        TEST_CHECK(cell.target_object_count[sub_layer] == 0);
        TEST_CHECK(cell.target_is_friend[sub_layer] == 0);
        TEST_CHECK(cell.anim_flags[sub_layer] == 0);
    }
    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = 1; object_layer <= NUM_LAYERS; object_layer++) {
            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            if (map_layer_is_remembered((uint8_t)object_layer)) {
                TEST_CHECK(cell.faces[layer] == layer + 1);
                TEST_CHECK(cell.flags[layer] == UINT8_MAX);
                TEST_CHECK(cell.roof[layer] == 1);
                TEST_CHECK(cell.anim_speed[layer] == 2);
            } else {
                TEST_CHECK(cell.faces[layer] == 0);
                TEST_CHECK(cell.flags[layer] == 0);
                TEST_CHECK(cell.roof[layer] == 0);
                TEST_CHECK(cell.anim_speed[layer] == 0);
            }
        }
    }

    return EXIT_SUCCESS;
}
