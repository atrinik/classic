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
/* Stack-scoped pre-event admission. Nested casts have independent escrows;
 * begin/end must be balanced in LIFO order on the simulation thread. */
typedef struct propagation_admission {
    size_t remaining;
    bool active;
    struct propagation_admission *previous;
} propagation_admission;
bool propagation_admission_begin(propagation_admission *admission, int radius,
                                 unsigned copies, bool swarm);
void propagation_admission_activate(propagation_admission *admission);
void propagation_admission_end(propagation_admission *admission);
typedef void (*propagation_visit)(object *, mapstruct *, int, int, int);
bool propagation_can_admit(int radius);
bool propagation_area(object *source, int radius, propagation_visit visit, int argument);
bool propagation_cone(object *source, object *caster, archetype_t *arch,
                      int strength, int damage, int dx, int dy, int dz);
bool propagation_reserve_explosion(object *effect, int strength);
bool propagation_reserve_explosion_from(object *effect, object *parent, int strength);
bool propagation_reserve_swarm(object *effect, int strength, unsigned count);
void propagation_expand(object *effect);
void propagation_track_drop(object *effect, object *drop);
void propagation_object_destroyed(object *effect);
void propagation_expire_map(mapstruct *map);
void propagation_tick(void);
size_t propagation_reserved_cells(void);
size_t propagation_last_expansions(void);
#endif
