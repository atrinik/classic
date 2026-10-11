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
 * Common projectile object (arrow, bolt, bullet, etc) related
 * functions.
 */

#include <global.h>
#include <server_main.h>
#include <movement.h>
#include <monster.h>
#include <arch.h>
#include <object_methods.h>
#include <metrics.h>
#include <player.h>
#include <perception.h>
#include <server.h>
/**
 * Attempts to stick a projectile such as an arrow into the victim.
 * @param op
 * Projectile.
 * @param victim
 * Victim.
 * @return
 * Pointer to the projectile, which may or may not have merged.
 */
static object *projectile_stick(object *op, object *victim) {
    object *owner;

    /* Only insert arrows, and as long as they are no more than 1kg, and can be
     * picked up. */
    if (op->type != ARROW || op->weight > 1000 || QUERY_FLAG(op, FLAG_NO_PICK)) {
        return op;
    }

    owner = object_owner(op);

    if (owner) {
        op->attacked_by = owner;
        op->attacked_by_count = owner->count;
    }

    object_remove(op, 0);
    op = object_insert_into(op, victim, 0);

    return op;
}

/** @copydoc object_methods_t::process_func */
void common_object_projectile_process(object *op) {
    if (op->map == NULL) {
        return;
    }
    /* Legacy saves have no trajectory record and resume on their original
     * level. A selected target is used only at an explicit new launch. */
    if (op->projectile.version == 0) {
        projectile3d_launch(op, op, NULL, op->direction);
    }
    if (!projectile3d_valid(&op->projectile) || op->last_sp-- <= 0) {
        object_projectile_stop(op, OBJECT_PROJECTILE_STOP_EOL);
        return;
    }
    op->projectile.remaining = MAX(0, op->last_sp);
    object_projectile_move(op);
}

/** @copydoc object_methods_t::projectile_move_func */
object *common_object_projectile_move(object *op) {
    if (op->projectile.version == 0) {
        projectile3d_launch(op, op, NULL, op->direction);
    }
    return projectile3d_move(op);
}

/** @copydoc object_methods_t::projectile_stop_func */
object *common_object_projectile_stop_missile(object *op, int reason) {
    projectile3d_reset(op);
    /* Reset 'owner' when picking it up. */
    if (reason == OBJECT_PROJECTILE_PICKUP) {
        op->attacked_by = NULL;
        op->attacked_by_count = 0;
    }

    /* Already stopped, nothing to do. */
    if (DBL_EQUAL(op->speed, 0.0)) {
        return op;
    }

    /* Small chance of breaking */
    if (op->last_eat && rndm_chance(op->last_eat)) {
        object_remove(op, 0);
        object_destroy(op);
        return NULL;
    }

    /* Restore arrow's properties. */
    if (op->type == ARROW) {
        object *owner;

        owner = object_owner(op);
        object_owner_clear(op);
        op->chosen_skill = NULL;

        op->direction = 0;
        SET_ANIMATION_STATE(op);

        CLEAR_FLAG(op, FLAG_FLYING);
        CLEAR_FLAG(op, FLAG_IS_MISSILE);
        CLEAR_FLAG(op, FLAG_WALK_ON);
        CLEAR_FLAG(op, FLAG_FLY_ON);

        /* Restore WC, damage and range. */
        op->stats.wc = op->last_heal;
        op->stats.dam = op->stats.hp;
        op->last_sp = op->stats.sp;

        op->last_heal = op->stats.hp = op->stats.sp = 0;

        /* Reset level, speed and wc_range. */
        op->level = op->arch->clone.level;
        op->speed = op->arch->clone.speed;
        op->stats.wc_range = op->arch->clone.stats.wc_range;

        if (QUERY_FLAG(&op->arch->clone, FLAG_CAN_STACK)) {
            SET_FLAG(op, FLAG_CAN_STACK);
        }

        if (!owner || owner->type != PLAYER) {
            SET_FLAG(op, FLAG_IS_USED_UP);
            SET_FLAG(op, FLAG_NO_PICK);

            op->type = MISC_OBJECT;
            op->speed = 0.1f;
            op->speed_left = 0.0f;
            op->stats.food = 20;
        }

        object_update_speed(op);

        bool insert_map = op->map != NULL;
        if (insert_map) {
            object_remove(op, 0);
        }

        op->layer = op->arch->clone.layer;

        if (insert_map) {
            op = object_insert_map(op, op->map, op, INS_FALL_THROUGH);
        } else {
            op = object_merge(op);
        }
    } else if (op->inv) {
        object *payload;

        /* Not an arrow, the object has payload instead. */

        payload = op->inv;

        object_remove(payload, 0);
        object_remove(op, 0);
        object_destroy(op);
        payload->x = op->x;
        payload->y = op->y;
        payload = object_insert_map(payload, op->map, op, INS_FALL_THROUGH);

        return payload;
    } else {
        /* Should not happen... */

        object_remove(op, 0);
        object_destroy(op);
        return NULL;
    }

    return op;
}

/** @copydoc object_methods_t::projectile_stop_func */
object *common_object_projectile_stop_spell(object *op, int reason) {
    if (reason == OBJECT_PROJECTILE_STOP_HIT && op->stats.dam > 0) {
        return op;
    }

    projectile3d_reset(op);

    if (op->other_arch) {
        explode_object(op);
    } else {
        object_remove(op, 0);
        object_destroy(op);
    }

    return NULL;
}

/** @copydoc object_methods_t::projectile_fire_func */
object *common_object_projectile_fire_missile(object *op, object *shooter, int dir) {
    tag_t shooter_count = shooter->count;
    bool is_player = shooter->type == PLAYER;
    player *controller = is_player ? CONTR(shooter) : NULL;
    socket_struct *socket = is_player ? controller->cs : NULL;
    object_owner_set(op, shooter);
    op->direction = dir;
    SET_ANIMATION_STATE(op);

    if (DBL_EQUAL(op->speed, 0.0)) {
        op->speed = 1.0;
    }

    /* Save the shooter's level. */
    if (!op->level) {
        op->level = SK_level(shooter);
    }

    op->speed_left = 0;
    object_update_speed(op);

    SET_FLAG(op, FLAG_FLYING);
    SET_FLAG(op, FLAG_IS_MISSILE);
    SET_FLAG(op, FLAG_WALK_ON);
    SET_FLAG(op, FLAG_FLY_ON);

    /* Do not allow stacking, otherwise it is possible for rapidly-fired
     * missiles to merge, which does not make sense. */
    CLEAR_FLAG(op, FLAG_CAN_STACK);

    op->x = shooter->x;
    op->y = shooter->y;
    op->layer = LAYER_EFFECT;
    op->sub_layer = shooter->sub_layer;
    op = object_insert_map(op, shooter->map, op, 0);

    if (!op) {
        return NULL;
    }

    object *target = NULL;
    tag_t projectile_count = op->count;
    /* Insertion and hidden-object plugins can mutate selection or identity.
     * Reauthorize the current selection at the point that it supplies XYZ. */
    bool shooter_current = !is_player ||
        player_connection_is_current(shooter, shooter_count, controller, socket);
    if (shooter_current && OBJECT_VALID(shooter, shooter_count)) {
        object *selected = is_player ? CONTR(shooter)->target_object : shooter->enemy;
        tag_t selected_count = is_player ? CONTR(shooter)->target_object_count : shooter->enemy_count;
        if (OBJECT_VALID(selected, selected_count)) {
            bool authorized = is_player
                ? target_player_authorized(CONTR(shooter), selected, NULL)
                : perception_object_visible(shooter, selected);
            shooter_current = !is_player ||
                player_connection_is_current(shooter, shooter_count, controller, socket);
            if (shooter_current && authorized && OBJECT_VALID(shooter, shooter_count) &&
                (shooter->type == PLAYER) == is_player &&
                OBJECT_VALID(selected, selected_count) &&
                (is_player ? CONTR(shooter)->target_object == selected &&
                             CONTR(shooter)->target_object_count == selected_count
                           : shooter->enemy == selected && shooter->enemy_count == selected_count)) {
                target = selected;
            }
        }
    }
    if (!OBJECT_VALID(op, projectile_count)) {
        return NULL;
    }
    if (!shooter_current || !OBJECT_VALID(shooter, shooter_count)) {
        /* Returning NULL also prevents bow/throw callers from accessing a
         * departed shooter for metrics and sound after the callback. */
        object_remove(op, 0);
        object_destroy(op);
        return NULL;
    }
    projectile3d_launch(op, shooter, target, dir);
    object_process(op);

    return op;
}

/** @copydoc object_methods_t::projectile_hit_func */
int common_object_projectile_hit(object *op, object *victim) {
    object *owner;

    owner = object_owner(op);
    victim = HEAD(victim);
    object *victim_owner = object_owner(victim);

    /* Pass nonliving and friendly victims. A player's projectile must also
     * not bypass PvP rules by attacking another player's owned monster. */
    if (!IS_LIVE(victim) || is_friend_of(owner, victim) ||
        (owner != NULL && owner->type == PLAYER && victim_owner != NULL &&
         victim_owner->type == PLAYER)) {
        return OBJECT_METHOD_UNHANDLED;
    }

    if (op->stats.dam > 0) {
        int dam;
        tag_t owner_count = owner != NULL ? owner->count : 0;
        bool metrics_projectile =
            op->type == ARROW && OBJECT_IS_PROJECTILE(op) && QUERY_FLAG(op, FLAG_IS_MISSILE) &&
            !QUERY_FLAG(op, FLAG_IS_SPELL) && owner != NULL && owner->type == PLAYER;
        op = projectile_stick(op, victim);

        OBJ_DESTROYED_BEGIN(op) {
            dam = attack_hit_situational(victim, op, op->stats.dam);

            if (dam > 0 && OBJECT_VALID(owner, owner_count) && owner->type == PLAYER) {
                if (metrics_projectile) {
                    metrics_add(&CONTR(owner)->metrics, METRIC_CHARACTER_PROJECTILE_HITS, 1);
                }
            }

            if (OBJ_DESTROYED(op)) {
                return OBJECT_METHOD_ERROR;
            }

            int64_t residual_damage = (int64_t)op->stats.dam - dam;
            op->stats.dam = (int16_t)MAX(residual_damage, INT16_MIN);
        }
        OBJ_DESTROYED_END();
    }

    return OBJECT_METHOD_OK;
}

/** @copydoc object_methods_t::move_on_func */
int common_object_projectile_move_on(object *op, object *victim, object *originator, int state) {
    int ret;

    (void)originator;

    if (!state) {
        return OBJECT_METHOD_UNHANDLED;
    }

    ret = object_projectile_hit(op, victim);

    if (ret == OBJECT_METHOD_OK) {
        object_projectile_stop(op, OBJECT_PROJECTILE_STOP_HIT);
        return OBJECT_METHOD_OK;
    }

    return OBJECT_METHOD_OK;
}
