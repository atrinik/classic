/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PROJECTILE3D_H
#define PROJECTILE3D_H

#include <stdbool.h>
#include <stdint.h>
#include <decls.h>

/* Value-owned: object_copy copies a complete independent trajectory. No map,
 * target, owner or cast-group pointer is serialized here. */
typedef struct projectile3d_state {
    int32_t heading[3];
    int32_t fraction[3];
    int32_t remaining;
    int depth;
    unsigned version;
} projectile3d_state;

void projectile3d_reset(object *op);
bool projectile3d_valid(const projectile3d_state *state);
bool projectile3d_parse(object *op, const char *text);
void projectile3d_launch(object *op, const object *source, const object *target, int dir);
bool projectile3d_aim(object *op, const object *target);
void projectile3d_reflect(object *op, unsigned axes);
void projectile3d_turn(object *op, int dir);
object *projectile3d_move(object *op);
bool projectile3d_can_move(const object *op);
bool projectile3d_hit_current(object *op);

#endif
