/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Map header file.
 */

#ifndef MAP_H
#define MAP_H

#include <toolkit/map_protocol.h>
#include <map_visibility.h>

/** Map tile position Y offset */
#define MAP_TILE_POS_YOFF 23

/** Map tile position Y offset 2 */
#define MAP_TILE_POS_YOFF2 12

/** Map tile position X offset */
#define MAP_TILE_POS_XOFF 48

/** Map tile position X offset 2 */
#define MAP_TILE_POS_XOFF2 24

/** Map tile X offset */
#define MAP_TILE_XOFF 12

/** Map tile Y offset */
#define MAP_TILE_YOFF 24

/**
 * Number of off-screen tile anchors requested on each edge of the logical
 * look window. Large isometric sprites can project into the viewport from
 * these tiles even though their owning tile is outside it.
 */
#define MAP_RENDER_OVERSCAN 2

/** Convert a user-selected logical look size to the map protocol size. */
#define MAP_LOOK_TO_WIRE_SIZE(_size) ((_size) + MAP_RENDER_OVERSCAN * 2)

/** Convert a map protocol size back to the user-selected logical look size. */
#define MAP_WIRE_TO_LOOK_SIZE(_size) ((_size) - MAP_RENDER_OVERSCAN * 2)

/**
 * @defgroup LAYER_xxx Layer types
 * The layer types used for different objects.
 *@{*/
/** System objects. */
#define LAYER_SYS 0
/** Floor. */
#define LAYER_FLOOR 1
/** Floor masks. */
#define LAYER_FMASK 2
/** Items: weapons, armour, books, etc. */
#define LAYER_ITEM 3
/** Another layer for items, often decoration. */
#define LAYER_ITEM2 4
/** Walls. */
#define LAYER_WALL 5
/** Living objects like players and monsters. */
#define LAYER_LIVING 6
/** Spell effects. */
#define LAYER_EFFECT 7
/*@}*/

/**
 * The number of object layers.
 */
#define NUM_LAYERS MAP2_PROTOCOL_OBJECT_LAYERS
/**
 * Number of sub-layers.
 */
#define NUM_SUB_LAYERS MAP2_PROTOCOL_SUB_LAYERS
/**
 * Effective number of all the visible layers.
 */
#define NUM_REAL_LAYERS MAP2_PROTOCOL_REAL_LAYERS

#define GET_MAP_LAYER(_layer, _sub_layer) (NUM_LAYERS * (_sub_layer) + (_layer) - 1)

/**
 * Return whether an object layer is remembered static geometry.
 *
 * These are the only layers that survive a soft visibility clear. Actors,
 * items, and effects remain live MAP2 presentation and are discarded when a
 * cell leaves the current visibility set.
 */
static inline bool map_layer_is_remembered(uint8_t layer) {
    return layer == LAYER_FLOOR || layer == LAYER_FMASK || layer == LAYER_WALL;
}

/** Multi part object tile structure */
typedef struct _multi_part_tile {
    /** X-offset */
    int xoff;

    /** Y-offset */
    int yoff;
} _multi_part_tile;

/** Table of predefined multi arch objects.
 * mpart_id and mpart_nr in the arches are committed from server
 * to analyze the exact tile position inside a mpart object.
 *
 * The way of determinate the starting and shift points is explained
 * in the dev/multi_arch folder of the arches, where the multi arch templates &
 * masks are. */
typedef struct _multi_part_obj {
    /** Natural xlen of the whole multi arch */
    int xlen;

    /** Same for ylen */
    int ylen;

    /** Tile */
    _multi_part_tile part[16];
} _multi_part_obj;

/** Map data structure */
typedef struct _mapdata {
    /** Map name. */
    char name[HUGE_BUF];

    /** New map name. */
    char name_new[HUGE_BUF];

    /** Region's name. */
    char region_name[MAX_BUF];

    /** Whether the region itself actually has map. */
    bool region_has_map;

    /** Region's long name. */
    char region_longname[MAX_BUF];

    /** Map path. */
    char map_path[HUGE_BUF];

    uint32_t name_fadeout_start;

    /** X length. */
    int xlen;

    /** Y length. */
    int ylen;

    /** Position X. */
    int posx;

    /** Position Y. */
    int posy;

    /** Bounded sequencing state for the last full MAP2 update. */
    map_protocol_continuation_state_t continuation;

    /** Timed-light descriptor staged by the current complete MAP2 transaction. */
    uint64_t light_keyframe_generation;
    uint64_t light_keyframe_start_seconds;
    uint64_t light_keyframe_end_seconds;
    uint8_t light_keyframe_flags;
    bool light_keyframe_valid;

    /**
     * If set, height difference will be taken into account when rendering
     * tiles (even if they are not FoW tiles).
     */
    unsigned int height_diff : 1;

    /**
     * If 1, the player is currently in a building.
     */

    /**
     * Player's current sub-layer.
     */
    uint8_t player_sub_layer;

    /**
     * Region map.
     */
    struct region_map *region_map;
} _mapdata;

/**
 * Map cell structure.
 */
typedef struct MapCell {
    /** Name of the living object on each sub-layer. */
    char pname[NUM_SUB_LAYERS][64];

    /** Living-object name color on each sub-layer. */
    char pcolor[NUM_SUB_LAYERS][COLOR_BUF];

    /** Position. */
    uint8_t quick_pos[NUM_REAL_LAYERS];

    /** Target HP percentage for the living object on each sub-layer. */
    uint8_t probe[NUM_SUB_LAYERS];

    /** Pre-tone Q5.11 scalar radiance for each sub-layer. */
    uint16_t light_radiance[NUM_SUB_LAYERS];

    /** Whether each light level has been received from the server. */
    uint8_t light_known[NUM_SUB_LAYERS];

    /** Pre-tone Q5.11 RGB radiance for each sub-layer. */
    uint16_t light_rgb_radiance[NUM_SUB_LAYERS][3];

    /** Bitmap of sub-layers whose RGB state is explicitly colored. */
    uint8_t light_rgb_explicit;

    /** Next authoritative scalar endpoint for each sub-layer. */
    uint16_t light_next_radiance[NUM_SUB_LAYERS];

    /** Whether each next endpoint is explicitly established. */
    uint8_t light_next_known[NUM_SUB_LAYERS];

    /** Next authoritative RGB endpoint for each sub-layer. */
    uint16_t light_next_rgb_radiance[NUM_SUB_LAYERS][3];

    /** Bitmap of sub-layers with explicitly colored next endpoints. */
    uint8_t light_next_rgb_explicit;

    /** Timed-light generation and absolute interpolation interval. */
    uint64_t light_keyframe_generation;
    uint64_t light_keyframe_start_seconds;
    uint64_t light_keyframe_end_seconds;
    uint8_t light_keyframe_flags;
    uint8_t light_keyframe_valid;

    /** Object flags. */
    uint8_t flags[NUM_REAL_LAYERS];

    /** Whether fogged geometry is an authoritative structural boundary. */
    uint8_t structural_fow;

    /** Whether terrain stretch must be recomputed for this cell. */
    uint8_t stretch_dirty;

    /** Topmost nonzero floor height, cached for negative terrain seams. */
    int16_t stretch_top_height;

    /** Topmost nonzero floor height above the base sub-layer. */
    int16_t stretch_upper_height;

    /** Maximum nonnegative floor elevation supporting linked upper levels. */
    int16_t level_support_height;

    /** Server-provided base-map elevation used to project linked upper levels. */
    int16_t structural_support_height;

    /** Maximum floor/effect elevation used for screen-space rejection. */
    int16_t render_max_height;

    /** Whether a wall-layer object is a roof/camera surface. */
    uint8_t roof[NUM_REAL_LAYERS];

    /** Door bits for each object layer, grouped by sub-layer. */
    uint8_t door[NUM_SUB_LAYERS];

    /** Object-layer bitmap identifying visible map transitions. */
    uint8_t exit[NUM_SUB_LAYERS];

    /** Double drawing. */
    uint8_t draw_double[NUM_REAL_LAYERS];

    /** Alpha value. */
    uint8_t alpha[NUM_REAL_LAYERS];

    /** Presentation-only visibility transitions for transient layers. */
    map_visibility_fade_t visibility[NUM_REAL_LAYERS];

    /** Faces. */
    int16_t faces[NUM_REAL_LAYERS];

    /** Height of this maptile. */
    int16_t height[NUM_REAL_LAYERS];

    /** Zoom X. */
    int16_t zoom_x[NUM_REAL_LAYERS];

    /** Zoom Y. */
    int16_t zoom_y[NUM_REAL_LAYERS];

    /** Align. */
    int16_t align[NUM_REAL_LAYERS];

    /** Rotate. */
    int16_t rotate[NUM_REAL_LAYERS];

    /** Whether to show the object in red. */
    uint8_t infravision[NUM_REAL_LAYERS];

    /** How we stretch this is really 8 char for N S E W. */
    int32_t stretch[NUM_SUB_LAYERS];

    /** Targetable living-object ID on each sub-layer. */
    uint32_t target_object_count[NUM_SUB_LAYERS];

    /** Whether the targetable living object on each sub-layer is a friend. */
    uint8_t target_is_friend[NUM_SUB_LAYERS];

    uint8_t anim_last[NUM_REAL_LAYERS];

    uint8_t anim_speed[NUM_REAL_LAYERS];

    uint8_t anim_facing[NUM_REAL_LAYERS];

    uint8_t anim_state[NUM_REAL_LAYERS];

    uint8_t anim_flags[NUM_SUB_LAYERS];

    /**
     * Whether Fog of War is enabled on this cell.
     */
    uint8_t fow;

    uint8_t priority[NUM_SUB_LAYERS];

    uint8_t secondpass[NUM_SUB_LAYERS];

    char glow[NUM_REAL_LAYERS][COLOR_BUF];
    uint8_t glow_speed[NUM_REAL_LAYERS];
    uint8_t glow_state[NUM_REAL_LAYERS];
} MapCell;

/** Clear live presentation while retaining all remembered map geometry. */
static inline void map_cell_clear_live_state(MapCell *cell) {
    HARD_ASSERT(cell != NULL);
    uint8_t remembered_layer_mask = 0;

    for (int object_layer = 1; object_layer <= NUM_LAYERS; object_layer++) {
        if (map_layer_is_remembered((uint8_t)object_layer)) {
            remembered_layer_mask |= UINT8_C(1) << (object_layer - 1);
        }
    }
    uint8_t live_layer_mask = (uint8_t)~remembered_layer_mask;

    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        cell->door[sub_layer] &= (uint8_t)~live_layer_mask;
        cell->exit[sub_layer] &= (uint8_t)~live_layer_mask;
        cell->priority[sub_layer] &= (uint8_t)~live_layer_mask;
        cell->secondpass[sub_layer] &= (uint8_t)~live_layer_mask;
        cell->anim_flags[sub_layer] = 0;
        cell->probe[sub_layer] = 0;
        cell->target_object_count[sub_layer] = 0;
        cell->target_is_friend[sub_layer] = 0;
        cell->pname[sub_layer][0] = '\0';
        cell->pcolor[sub_layer][0] = '\0';
    }

    for (int sub_layer = 0; sub_layer < NUM_SUB_LAYERS; sub_layer++) {
        for (int object_layer = 1; object_layer <= NUM_LAYERS; object_layer++) {
            if (map_layer_is_remembered((uint8_t)object_layer)) {
                continue;
            }

            int layer = GET_MAP_LAYER(object_layer, sub_layer);
            /* A live transient may be in its bounded fade-out window after
             * the authoritative record was cleared. Keep its presentation
             * payload until the visibility clock expires it. */
            if (cell->visibility[layer].initialized && cell->visibility[layer].alpha != 0) {
                continue;
            }
            cell->faces[layer] = 0;
            cell->flags[layer] = 0;
            cell->roof[layer] = 0;
            cell->quick_pos[layer] = 0;
            cell->height[layer] = 0;
            cell->zoom_x[layer] = 0;
            cell->zoom_y[layer] = 0;
            cell->align[layer] = 0;
            cell->rotate[layer] = 0;
            cell->infravision[layer] = 0;
            cell->draw_double[layer] = 0;
            cell->alpha[layer] = 0;
            memset(&cell->visibility[layer], 0, sizeof(cell->visibility[layer]));
            cell->anim_last[layer] = 0;
            cell->anim_speed[layer] = 0;
            cell->anim_facing[layer] = 0;
            cell->anim_state[layer] = 0;
            cell->glow[layer][0] = '\0';
            cell->glow_speed[layer] = 0;
            cell->glow_state[layer] = 0;
        }
    }
}

/** Discard presentation light knowledge while retaining the scalar cache value. */
static inline void map_cell_clear_light_state(MapCell *cell) {
    HARD_ASSERT(cell != NULL);
    memset(cell->light_known, 0, sizeof(cell->light_known));
    memset(cell->light_rgb_radiance, 0, sizeof(cell->light_rgb_radiance));
    cell->light_rgb_explicit = 0;
    memset(cell->light_next_radiance, 0, sizeof(cell->light_next_radiance));
    memset(cell->light_next_known, 0, sizeof(cell->light_next_known));
    memset(cell->light_next_rgb_radiance, 0, sizeof(cell->light_next_rgb_radiance));
    cell->light_next_rgb_explicit = 0;
    cell->light_keyframe_generation = 0;
    cell->light_keyframe_start_seconds = 0;
    cell->light_keyframe_end_seconds = 0;
    cell->light_keyframe_flags = 0;
    cell->light_keyframe_valid = 0;
}

#define MAP_STARTX map_width *(MAP_FOW_SIZE / 2)
#define MAP_STARTY map_height *(MAP_FOW_SIZE / 2)
#define MAP_WIDTH map_width
#define MAP_HEIGHT map_height

typedef struct map_target_struct {
    uint32_t count;
    int x;
    int y;
} map_target_struct;

/** Font used for the map name. */
#define MAP_NAME_FONT FONT_SERIF14

/** Time in milliseconds for fade out/in effect of the map name. */
#define MAP_NAME_FADEOUT 500

/**
 * Maximum height difference between the rendered tile and the player's tile.
 *
 * Tiles that are lower/higher than this (relative to the player) will not
 * be rendered.
 *
 * Only applicable to tiles that are in the Fog of War, or if
 * MapData::height_diff is set.
 */
#define HEIGHT_MAX_RENDER 50

/**
 * @defgroup ANIM_xxx Animation types
 * Animation types.
 *@{*/
/** Damage animation. */
#define ANIM_DAMAGE 1
/** Kill animation. */
#define ANIM_KILL 2
/*@}*/

/**
 * Map animation structure.
 */
typedef struct map_anim {
    struct map_anim *next; ///< Next animation.
    struct map_anim *prev; ///< Previous animation.

    int type; ///< Type of the animation, one of @ref ANIM_xxx.
    int sub_layer; ///< Sub-layer the damage is happening on.
    int8_t depth; ///< Linked-map depth where the animation occurred.
    int value; ///< This is the number to display.
    int mapx; ///< Map position X.
    int mapy; ///< Map position Y.

    double xoff; ///< Movement in X per tick.
    double yoff; ///< Movement in Y per tick.

    uint32_t start_tick; ///< The time we started this anim.
    uint32_t last_tick; ///< This is the end-tick.
} map_anim_t;

/** Public API implemented in src/gui/widgets/map.c. */

extern _mapdata MapData;

extern _multi_part_obj MultiArchs[16];

extern struct map_anim *
map_anims_add(int type, int mapx, int mapy, int sub_layer, int depth, int value);

extern void maps_anims_remove(map_anim_t *anim);

extern void map_anims_mapscroll(int xoff, int yoff);

extern void map_anims_clear(void);

extern void map_anims_play(void);

extern int map_anims_need_redraw(void);

extern void load_mapdef_dat(void);

/** Load multipart geometry from an explicit immutable input. */
extern bool load_mapdef_file(const char *path);

/** Initialize map state without registering a graphical widget. */
extern void map_runtime_init(void);

/** Release map state initialized for either graphical or offscreen use. */
extern void map_runtime_deinit(void);

extern void clear_map(_Bool hard);

extern void map_update_size(int w, int h);

extern void display_mapscroll(int dx, int dy, int old_w, int old_h);

extern void update_map_name(const char *name);

extern void update_map_weather(const char *weather);

extern void update_map_height_diff(uint8_t height_diff);

extern void update_map_region_name(const char *region_name);

extern void update_map_region_longname(const char *region_longname);

extern void update_map_path(const char *map_path);

extern int map_get_player_direction(void);

extern void map_get_real_coords(int *x, int *y);

extern void init_map_data(int xl, int yl, int px, int py);

extern void adjust_tile_stretch(void);

extern void map_set_data(int x,
                         int y,
                         int layer,
                         int16_t face,
                         uint8_t quick_pos,
                         uint8_t obj_flags,
                         const char *name,
                         const char *name_color,
                         int16_t height,
                         uint8_t probe,
                         int16_t zoom_x,
                         int16_t zoom_y,
                         int16_t align,
                         uint8_t draw_double,
                         uint8_t alpha,
                         int16_t rotate,
                         uint8_t infravision,
                         uint32_t target_object_count,
                         uint8_t target_is_friend,
                         uint8_t anim_speed,
                         uint8_t anim_facing,
                         uint8_t anim_flags,
                         uint8_t anim_state,
                         uint8_t priority,
                         uint8_t secondpass,
                         uint8_t roof,
                         uint8_t door,
                         uint8_t exit,
                         const char *glow,
                         uint8_t glow_speed);

extern bool map_select_level(int depth, bool create);

extern void map_set_level_mask(uint16_t mask);

/** Copy one decoded map cell for packet change detection. */
extern void map_cell_snapshot(int x, int y, MapCell *snapshot);

/** Return whether a decoded map cell differs from a prior snapshot. */
extern bool map_cell_changed(int x, int y, const MapCell *snapshot);

extern void map_level_scroll(int dz);

extern void map_clear_cell(int x, int y, bool hard);

extern void map_set_structural_support_height(int x, int y, int16_t height);

extern void map_set_fow(int x, int y, bool fow);

extern bool map_get_fow(int x, int y);

extern void map_set_light_radiance(int x, int y, int sub_layer, uint16_t radiance);
extern void
map_set_light_rgb_radiance(int x, int y, uint8_t bitmap, const uint16_t rgb[NUM_SUB_LAYERS][3]);
extern void map_set_light_keyframe(int x,
                                   int y,
                                   uint64_t generation,
                                   uint64_t start_seconds,
                                   uint64_t end_seconds,
                                   uint8_t flags,
                                   uint8_t scalar_bitmap,
                                   const uint16_t scalar[NUM_SUB_LAYERS],
                                   uint8_t rgb_bitmap,
                                   const uint16_t rgb[NUM_SUB_LAYERS][3]);
extern bool map_light_keyframe_transaction_begin(uint64_t generation,
                                                  uint64_t start_seconds,
                                                  uint64_t end_seconds,
                                                  uint8_t flags);
extern bool map_light_keyframe_transaction_pending(void);
extern bool map_light_keyframe_transaction_stage(int depth,
                                                  int x,
                                                  int y,
                                                  uint8_t scalar_bitmap,
                                                  const uint16_t scalar[NUM_SUB_LAYERS],
                                                  uint8_t rgb_bitmap,
                                                  const uint16_t rgb[NUM_SUB_LAYERS][3]);
extern void map_light_keyframe_transaction_commit(void);
extern void map_light_keyframe_transaction_abort(void);

/** Begin, commit, or roll back one complete MAP2 map-state transaction. */
extern void map_state_transaction_begin(bool full_snapshot);
extern void map_state_transaction_commit(void);
extern void map_state_transaction_abort(void);

extern void map_animate(void);

/** Why the primary map surface needs to be rebuilt. */
typedef enum map_redraw_reason {
    MAP_REDRAW_REASON_EXTERNAL = 1U << 0,
    MAP_REDRAW_REASON_MAP_PACKET = 1U << 1,
    MAP_REDRAW_REASON_ANIMATION = 1U << 2,
    MAP_REDRAW_REASON_RESIZE = 1U << 3,
    MAP_REDRAW_REASON_SCROLL = 1U << 4,
    MAP_REDRAW_REASON_LIGHTING = 1U << 5,
    MAP_REDRAW_REASON_UI = 1U << 6,
} map_redraw_reason_t;

/** Request a primary-map redraw and retain its production reason. */
extern void map_redraw_request(map_redraw_reason_t reason);

/** Return whether the production widget would rebuild its primary map surface. */
extern bool map_redraw_due(void);

/** Return whether changed animation output needs an object-only map pass. */
extern bool map_animation_redraw_due(void);

/** Return the reasons accumulated for the pending primary-map redraw. */
extern uint32_t map_redraw_pending_reasons(void);

/** Mark the pending primary-map redraw as consumed. */
extern void map_redraw_consume(void);

/** Mark an object-only animation pass as consumed. */
extern void map_animation_redraw_consume(void);

extern void map_draw_map(SDL_Surface *surface);

/** Repaint map objects over the cached lit ground without rebuilding static levels. */
extern bool map_draw_animation(SDL_Surface *surface);

/** Draw the current world-pointer cue over the completed map composition. */
extern void map_draw_pointer_overlay(void);

/**
 * Check whether the map pointer cue may occupy a screen point.
 * @param x
 * Screen X coordinate.
 * @param y
 * Screen Y coordinate.
 * @return
 * 1 if the point is not covered by a popup or higher-priority widget, 0
 * otherwise.
 */
extern bool map_pointer_overlay_visible_at(int x, int y);

#define MAP_BENCHMARK_STATISTICS_VERSION UINT8_C(3)

/** Map renderer work accumulated since the last benchmark reset. */
typedef struct map_benchmark_statistics {
    uint64_t map_draws;
    uint64_t primary_map_draws;
    uint64_t auxiliary_map_draws;
    uint64_t animation_draws;
    uint64_t animation_level_draws;
    uint64_t presents;
    uint64_t present_failures;
    uint64_t render_failures;
    uint64_t fault_injections;
    uint64_t fault_detections;
    uint64_t level_draws;
    uint64_t render_commands;
    uint64_t annotations;
    uint64_t ui_tiles;
    uint64_t peak_render_commands;
    uint64_t peak_active_levels;
    /** False until the complete renderer allocation graph can be observed safely. */
    bool renderer_allocation_statistics_available;
    uint64_t renderer_allocations;
    uint64_t renderer_allocation_bytes;
} map_benchmark_statistics_t;

void map_benchmark_statistics_reset(void);
void map_benchmark_statistics_get(map_benchmark_statistics_t *statistics);
/** Record an attempted window presentation and whether it succeeded. */
void map_benchmark_statistics_present(bool success);

extern void map_draw_one(int x, int y, SDL_Surface *surface);

extern void map_target_handle(uint8_t is_friend);

extern bool mouse_to_tile_coords(int mx, int my, int *tx, int *ty);

extern bool map_mouse_fire(void);

extern void widget_map_init(widgetdata *widget);

#ifdef ATRINIK_WIDGET_TESTS
typedef enum map_benchmark_fault {
    MAP_BENCHMARK_FAULT_NONE,
    MAP_BENCHMARK_FAULT_MUTABLE_RLE,
} map_benchmark_fault_t;

typedef struct map_benchmark_fault_status {
    bool injected;
    bool detected;
} map_benchmark_fault_status_t;

/** Configure one movement-benchmark-only renderer fault. */
extern void map_benchmark_fault_configure(map_benchmark_fault_t fault);
/** Read fault state independently of per-phase telemetry resets. */
extern void map_benchmark_fault_status_get(map_benchmark_fault_status_t *status);
/** Remove any armed mutation and restore normal renderer state. */
extern void map_benchmark_fault_clear(void);

extern bool widget_map_interaction_test(widgetdata *widget);
extern bool widget_map_mouse_origin_test(int mx, int my, int expected_mx, int expected_my);
extern void widget_map_draw_test(widgetdata *widget);
extern void widget_map_ui_test_begin(void);
extern bool widget_map_ui_test_end(void);
extern void widget_map_animation_test_begin(void);
extern bool widget_map_animation_test_end(bool expect_damage,
                                          bool expect_kill,
                                          bool expect_elevated,
                                          bool expect_layer_content);
extern void widget_map_animation_test_death_texture_set(SDL_Surface *texture);
/** Set the synthetic pointer owner used by the cursor redraw benchmark. */
extern void widget_map_pointer_test_set(int x, int y, bool world_pointer);
extern void widget_map_animation_test_add(int type,
                                          int x_offset,
                                          int y_offset,
                                          int sub_layer,
                                          int depth,
                                          int value,
                                          uint32_t elapsed_ms);
#endif

/** Public API implemented in src/gui/widgets/minimap.c. */

/** Minimum interval between expensive local-world minimap renders. */
#define MINIMAP_DYNAMIC_REDRAW_INTERVAL 250U

/** Dynamic-map surface dimensions retained for identical zoom/crop coverage. */
#define MINIMAP_DYNAMIC_LEGACY_SURFACE_WIDTH (850 * (MAP_FOW_SIZE / 2))
#define MINIMAP_DYNAMIC_LEGACY_SURFACE_HEIGHT (600 * (MAP_FOW_SIZE / 2))

#define MINIMAP_DYNAMIC_SURFACE_WIDTH MINIMAP_DYNAMIC_LEGACY_SURFACE_WIDTH
#define MINIMAP_DYNAMIC_SURFACE_HEIGHT MINIMAP_DYNAMIC_LEGACY_SURFACE_HEIGHT

extern bool minimap_redraw_due(void);

extern void widget_minimap_init(widgetdata *widget);

#endif
