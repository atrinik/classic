/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VIDEO_RECORDING_H
#define VIDEO_RECORDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Client-thread API. Captures only gameplay; encoding runs in an isolated child
 * fed by one transport thread. No configuration or credentials enter the wire. */
bool video_recording_initialize(const char *executable_name);
bool video_recording_start(const char *absolute_path);
void video_recording_stop(void);
/* Admission applies only to new GPU copies; denial still services lifecycle. */
void video_recording_frame(bool playing, bool presented, bool capture_allowed, uint64_t now_ms);
bool video_recording_message(char *message, size_t capacity, bool *failed);
bool video_recording_failed(void);
/* Call before SDL teardown; owns only its encoder child, thread and frames. */
void video_recording_shutdown(void);

#endif
