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
bool celestial_structure_logical_map_id_valid(const char *path);
mapstruct *celestial_structure_create_map(int width,
                                          int height,
                                          const char *path,
                                          mapstruct *origin,
                                          const char *sky_above,
                                          int light,
                                          char *error,
                                          size_t error_size);
bool celestial_structure_initialize_generated_map(mapstruct *map,
                                                  const char *path,
                                                  mapstruct *origin,
                                                  int light,
                                                  char *error,
                                                  size_t error_size);
bool celestial_structure_validate_archetypes(char *error, size_t error_size);
/** Verify the immutable Classic artifact before the server accepts players. */
bool celestial_structure_startup_preflight(char *error, size_t error_size);
/** Acquire the process-wide writer exclusion used by celestial activation. */
bool celestial_structure_acquire_writer_lease(char *error, size_t error_size);
/** Release the process-wide celestial activation writer exclusion. */
void celestial_structure_release_writer_lease(void);
/** Whether startup has selected the fail-closed celestial-v1 runtime. */
bool celestial_structure_v1_runtime_active(void);
/** Publish and validate the digest-addressed mutable-map provenance sidecar. */
bool celestial_structure_write_provenance(const mapstruct *map, char *error, size_t error_size);
bool celestial_structure_validate_provenance(const mapstruct *map, char *error, size_t error_size);
bool celestial_structure_begin_map_transaction(const mapstruct *map,
                                               const char *map_file,
                                               const char *unique_file,
                                               char *error,
                                               size_t error_size);
bool celestial_structure_commit_map_transaction(const mapstruct *map, char *error, size_t error_size);
bool celestial_structure_finish_map_transaction(const mapstruct *map, char *error, size_t error_size);
bool celestial_structure_recover_map_transactions(char *error, size_t error_size);
int celestial_structure_inventory_run(void);
void celestial_structure_save_metadata(const mapstruct *map, FILE *fp);
void celestial_structure_free(mapstruct *map);
void celestial_structure_reset_parse_state(mapstruct *map);

#endif
