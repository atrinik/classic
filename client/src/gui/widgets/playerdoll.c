/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2014 Zoey Rose and Atrinik Development Team      *
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
 * Implements player doll type widgets.
 *
 * @author Zoey Rose
 */

#include <global.h>
#include <session_client.h>
#include <toolkit/string.h>

/**
 * Player doll item positions.
 *
 * Used to determine where to put item sprites on the player doll.
 */
static int player_doll_positions[PLAYER_EQUIP_MAX][2] = {{22, 44},
                                                         {22, 6},
                                                         {22, 82},
                                                         {102, 82},
                                                         {102, 120},
                                                         {22, 158},
                                                         {62, 6},
                                                         {62, 44},
                                                         {62, 82},
                                                         {62, 120},
                                                         {62, 158},
                                                         {102, 6},
                                                         {102, 44},
                                                         {22, 82},
                                                         {22, 120},
                                                         {102, 158},
                                                         {-1, -1}};

/**
 * Text used in the player doll.
 */
static const char *player_doll_text =
    "[center][font=sans 12][b]Statistics[/b][/font][/center]\n"
    "Strength[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Dexterity[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Constitution[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Intelligence[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Power[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Speed[c=#ffffff][right][font=mono]%3.2f[/font][/right][/c]\n"
    "Armour class[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "[y=4][center][font=sans 12][b]Melee[/b][/font][/center]\n"
    "Damage[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Weapon class[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Weapon speed[c=#ffffff][right][font=mono]%3.2fs[/font][/right][/c]\n"
    "[y=4][center][font=sans 12][b]Ranged[/b][/font][/center]\n"
    "Damage[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Weapon class[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Weapon speed[c=#ffffff][right][font=mono]%3.2fs[/font][/right][/c]\n";

/**
 * Same as above, except with abbreviations to conserve horizontal space.
 */
static const char *player_doll_text_abbr =
    "[center][font=sans 12][b]Stats[/b][/font][/center]\n"
    "Str[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Dex[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Con[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Int[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Pow[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "Speed[c=#ffffff][right][font=mono]%3.2f[/font][/right][/c]\n"
    "AC[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "[y=4][center][font=sans 12][b]Melee[/b][/font][/center]\n"
    "DMG[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "WC[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "WS[c=#ffffff][right][font=mono]%3.2fs[/font][/right][/c]\n"
    "[y=4][center][font=sans 12][b]Ranged[/b][/font][/center]\n"
    "DMG[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "WC[c=#ffffff][right][font=mono]%02d[/font][/right][/c]\n"
    "WS[c=#ffffff][right][font=mono]%3.2fs[/font][/right][/c]\n";

#define PLAYER_DOLL_TEXT_RENDER(flags, box)      \
    text_show_format(widget->surface,            \
                     FONT_ARIAL10,               \
                     10,                         \
                     10,                         \
                     COLOR_HGOLD,                \
                     TEXT_MARKUP | flags,        \
                     box,                        \
                     text,                       \
                     player.stats.strength,      \
                     player.stats.dexterity,     \
                     player.stats.constitution,  \
                     player.stats.intelligence,  \
                     player.stats.power,         \
                     player.stats.speed,         \
                     player.stats.ac,            \
                     player.stats.damage,        \
                     player.stats.wc,            \
                     player.stats.weapon_speed,  \
                     player.stats.ranged_damage, \
                     player.stats.ranged_wc,     \
                     player.stats.ranged_weapon_speed);

object *playerdoll_get_equipment(int i, int *xpos, int *ypos) {
    object *obj;
    session_player_t player = {0};
    HARD_ASSERT(session_player_view(client_session_get(), &player));

    if (player.equipment[i] == 0) {
        return NULL;
    }

    if (player_doll_positions[i][0] == -1 && player_doll_positions[i][1] == -1) {
        return NULL;
    }

    obj = object_find(player.equipment[i]);

    if (obj == NULL) {
        return NULL;
    }

    if (i == PLAYER_EQUIP_SHIELD) {
        object *obj2 = NULL;

        if (player.equipment[PLAYER_EQUIP_WEAPON_RANGED] != 0) {
            obj2 = object_find(player.equipment[PLAYER_EQUIP_WEAPON_RANGED]);
        } else if (player.equipment[PLAYER_EQUIP_WEAPON] != 0) {
            obj2 = object_find(player.equipment[PLAYER_EQUIP_WEAPON]);
        }

        if (obj2 != NULL && obj2->flags & CS_FLAG_WEAPON_2H) {
            obj = obj2;
        }
    } else if (i == PLAYER_EQUIP_WEAPON_RANGED) {
        if (obj->flags & CS_FLAG_WEAPON_2H) {
            return NULL;
        }
    }

    *xpos = player_doll_positions[i][0] + 2;
    *ypos = player_doll_positions[i][1] + 2;

    return obj;
}

/** @copydoc widgetdata::draw_func */
static void widget_draw(widgetdata *widget) {
    int i, xpos, ypos, xoff, yoff;
    SDL_Surface *texture_slot_border, *texture;
    object *obj;
    SDL_Rect box, box2;
    const char *text;
    session_player_t player = {0};

    if (!widget->redraw) {
        return;
    }

    HARD_ASSERT(session_player_view(client_session_get(), &player));

    if (player.gender == GENDER_FEMALE) {
        texture = TEXTURE_CLIENT("player_doll_f");
    } else {
        texture = TEXTURE_CLIENT("player_doll");
    }

    xoff = widget->w - texture->w + 10;
    yoff = widget->h / 2 - texture->h / 2;

    box.w = xoff - 10;
    box.h = widget->h - 10 * 2;
    box2.w = 0;
    box2.h = 0;

    text = player_doll_text;

    PLAYER_DOLL_TEXT_RENDER(TEXT_MAX_WIDTH, &box2);

    if (box2.w > box.w) {
        text = player_doll_text_abbr;
    }

    PLAYER_DOLL_TEXT_RENDER(0, &box);

    texture_slot_border = TEXTURE_CLIENT("player_doll_slot_border");

    for (i = 0; i < PLAYER_EQUIP_MAX; i++) {
        if (player_doll_positions[i][0] == -1 && player_doll_positions[i][1] == -1) {
            continue;
        }

        rectangle_create(widget->surface,
                         player_doll_positions[i][0] + xoff,
                         player_doll_positions[i][1] + yoff,
                         texture_slot_border->w,
                         texture_slot_border->h,
                         PLAYER_DOLL_SLOT_COLOR);
    }

    surface_show(widget->surface, xoff, yoff, NULL, texture);

    for (i = 0; i < PLAYER_EQUIP_MAX; i++) {
        if (player_doll_positions[i][0] == -1 && player_doll_positions[i][1] == -1) {
            continue;
        }

        surface_show(widget->surface,
                     player_doll_positions[i][0] + xoff,
                     player_doll_positions[i][1] + yoff,
                     NULL,
                     texture_slot_border);

        obj = playerdoll_get_equipment(i, &xpos, &ypos);

        if (!obj) {
            continue;
        }

        object_show_centered(widget->surface,
                             obj,
                             xpos + xoff,
                             ypos + yoff,
                             INVENTORY_ICON_SIZE,
                             INVENTORY_ICON_SIZE,
                             false);
    }
}

/** @copydoc widgetdata::event_func */
static int widget_event(widgetdata *widget, SDL_Event *event) {
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        char buf[HUGE_BUF];
        object *obj;
        int i, xpos, ypos, xoff, yoff;

        buf[0] = '\0';
        xoff = widget->w - TEXTURE_CLIENT("player_doll")->w + 10;
        yoff = widget->h / 2 - TEXTURE_CLIENT("player_doll")->h / 2;

        for (i = 0; i < PLAYER_EQUIP_MAX; i++) {
            obj = playerdoll_get_equipment(i, &xpos, &ypos);

            if (obj == NULL) {
                continue;
            }

            xpos += xoff;
            ypos += yoff;

            if (event_mouse_x(event) - widget->x > xpos &&
                event_mouse_x(event) - widget->x <= xpos + INVENTORY_ICON_SIZE &&
                event_mouse_y(event) - widget->y > ypos &&
                event_mouse_y(event) - widget->y <= ypos + INVENTORY_ICON_SIZE) {
                if (buf[0] != '\0') {
                    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
                }

                if (obj->nrof > 1) {
                    snprintfcat(buf, sizeof(buf), "%" PRIu32 " %s", obj->nrof, obj->s_name);
                } else {
                    strncat(buf, obj->s_name, sizeof(buf) - strlen(buf) - 1);
                }
            }
        }

        if (buf[0] != '\0') {
            tooltip_create(event_mouse_x(event), event_mouse_y(event), FONT_ARIAL11, buf);
            tooltip_enable_delay(300);
            tooltip_multiline(200);
            return 1;
        }
    }

    return 0;
}

void widget_playerdoll_init(widgetdata *widget) {
    widget->draw_func = widget_draw;
    widget->event_func = widget_event;
}
