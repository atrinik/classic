/**
 * @file
 * Asynchronous private face-cache writes.
 */

#ifndef FACE_CACHE_H
#define FACE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool face_cache_start(void);
void face_cache_enqueue(const char *name, const uint8_t *data, size_t size);
void face_cache_report_failures(void);
void face_cache_stop(void);

#endif
