/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

#ifndef CELESTIAL_LUNAR_H
#define CELESTIAL_LUNAR_H

#include <stdbool.h>
#include <stdint.h>

/* The lunar input, sample, phase names and evaluator are shared with the
 * client through libatrinik's celestial model. */
#include <toolkit/celestial_model.h>

/** Fill the frozen root profile using direct absolute-hour modulo. */
extern void celestial_lunar_root_input(uint64_t absolute_hour, celestial_lunar_input *input);

#endif
