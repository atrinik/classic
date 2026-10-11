/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PROPAGATION_H
#define PROPAGATION_H
#include <stdbool.h>
#include <stddef.h>
#include <decls.h>
#define PROPAGATION_CAST_CELLS 65536u
#define PROPAGATION_GLOBAL_CELLS 1048576u
#define PROPAGATION_TICK_EXPANSIONS 4096u
/* Main simulation thread only. No map loads. Admission reserves the complete
 * bounding volume before effects or costs; all storage is transient. */
typedef void (*propagation_visit)(object *, mapstruct *, int, int, int);
bool propagation_can_admit(int radius);
bool propagation_area(object *source, int radius, propagation_visit visit, int argument);
bool propagation_cone(object *source, object *caster, archetype_t *arch,
                      int strength, int damage, int dx, int dy, int dz);
bool propagation_reserve_explosion(object *effect, int strength);
void propagation_expand(object *effect);
void propagation_track_drop(object *effect, object *drop);
void propagation_object_destroyed(object *effect);
void propagation_expire_map(mapstruct *map);
void propagation_tick(void);
size_t propagation_reserved_cells(void);
size_t propagation_last_expansions(void);
#endif
