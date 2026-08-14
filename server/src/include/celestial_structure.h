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

#ifndef CELESTIAL_STRUCTURE_H
#define CELESTIAL_STRUCTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <decls.h>

enum {
    CELESTIAL_FACE_DOWN = 1U << 0,
    CELESTIAL_FACE_NORTH = 1U << 1,
    CELESTIAL_FACE_EAST = 1U << 2,
    CELESTIAL_FACE_SOUTH = 1U << 3,
    CELESTIAL_FACE_WEST = 1U << 4,
};

typedef enum celestial_transmission {
    CELESTIAL_TRANSMISSION_INVALID = -1,
    CELESTIAL_TRANSMISSION_OPAQUE = 0,
    CELESTIAL_TRANSMISSION_GLASS = 192,
    CELESTIAL_TRANSMISSION_GRATE = 224,
    CELESTIAL_TRANSMISSION_OPEN = 256,
} celestial_transmission_t;

bool celestial_structure_finalize_map(mapstruct *map, char *error, size_t error_size);
bool celestial_structure_validate_header(mapstruct *map, char *error, size_t error_size);
bool celestial_structure_validate_topology(mapstruct *map, char *error, size_t error_size);
bool celestial_structure_cell_exposed(const mapstruct *map, int x, int y);
uint8_t celestial_structure_faces(const object *op);
celestial_transmission_t celestial_structure_transmission(const char *value);
bool celestial_structure_inventory(const mapstruct *map, FILE *fp, size_t max_records);
bool celestial_structure_inventory_maps_valid(const char *input);
int celestial_structure_inventory_run(void);
void celestial_structure_save_metadata(const mapstruct *map, FILE *fp);
void celestial_structure_free(mapstruct *map);
void celestial_structure_reset_parse_state(mapstruct *map);

#endif
