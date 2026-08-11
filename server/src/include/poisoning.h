/*
 * Atrinik server poison policy.
 *
 * Copyright (C) 2026 Atrinik Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef POISONING_H
#define POISONING_H

/** Number of damaging/stat-depleting pulses in a new poison effect. */
#define POISON_BASE_PULSES 5

/** Maximum pulses that repeated poison hits may leave pending. */
#define POISON_REFRESH_MAX_PULSES 10

/** Maximum unprotected depletion stored for any one player stat. */
#define POISON_MAX_STAT_DEPLETION 3

int poisoning_stat_depletion_limit(int protection);
int poisoning_stat_depletion_after_pulse(int depletion, int protection, bool drains, int amount);
void poisoning_apply_stat_depletion(object *poison, object *target);

#endif
