/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CELESTIAL_DEPENDENCIES_H
#define CELESTIAL_DEPENDENCIES_H

#include <stdbool.h>
#include <stddef.h>
#include <decls.h>

typedef struct celestial_dependencies celestial_dependencies;

/* Server-thread only. The reservation counter must outlive the construction
 * context. Every allocated byte is charged to it until finish or release.
 * Exceeding either bound invalidates the entire solve, never drops a tuple. */
celestial_dependencies *celestial_dependencies_begin(size_t *reserved,
                                                      size_t limit,
                                                      size_t max_entries);
bool celestial_dependencies_observe(celestial_dependencies *ctx, const mapstruct *map);
bool celestial_dependencies_observe_at(celestial_dependencies *ctx,
                                       const mapstruct *map, int tx, int ty, int z);
bool celestial_dependencies_fresh(const celestial_dependencies *ctx);
bool celestial_dependencies_references_map(const celestial_dependencies *ctx,
                                           const mapstruct *map);
size_t celestial_dependencies_bytes(const celestial_dependencies *ctx);
size_t celestial_dependencies_count(const celestial_dependencies *ctx);
void celestial_dependencies_release(celestial_dependencies *ctx);

/* Atomically bind a completed context to all output maps and transfer its
 * reservation to retained memory. Current plus next metadata is capped at
 * 8192 bytes per output map, including registry records. The caller retains
 * its own reference and must release it after publication. */
bool celestial_dependencies_finish(celestial_dependencies *ctx,
                                    mapstruct *const *maps, size_t count, bool next);
bool celestial_dependencies_field_fresh(const mapstruct *map, bool next);
/* get returns an owned reference (or NULL); bind accepts a completed context
 * and keeps its own reference, permitting snapshot restoration/promotion. */
celestial_dependencies *celestial_dependencies_field_get(const mapstruct *map, bool next);
bool celestial_dependencies_field_bind(celestial_dependencies *ctx,
                                        mapstruct *map, bool next);
void celestial_dependencies_field_release(const mapstruct *map, bool next);
/* Invalidate only; does not release field bindings. Must run BEFORE map
 * storage is freed/recycled, including unready tile targets. Invalid contexts
 * subsequently return false without dereferencing any retained pointer. */
void celestial_dependencies_forget(const mapstruct *map);
/* Release the map's two registry references during its actual teardown. */
void celestial_dependencies_field_forget(const mapstruct *map);

#endif
