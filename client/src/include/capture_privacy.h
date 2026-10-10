/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CAPTURE_PRIVACY_H
#define CAPTURE_PRIVACY_H

#include <stdbool.h>

/* Client thread only. Starts denied. Private UI and renderer recovery invalidate
 * the completed frame; closing/resetting UI never grants capture permission. */
void capture_privacy_block(void);
/* Snapshot privacy before composition. A private load during composition also
 * taints that frame, even if the UI closes before presentation. */
void capture_privacy_frame_begin(bool sensitive_active);
/* Only a successfully presented known-clean composition may admit captures. */
void capture_privacy_frame_end(bool presented);
/* The currently retained UI additionally denies new submissions. Already
 * submitted safe GPU copies keep their independent completion/lifetime. */
bool capture_privacy_allowed(bool sensitive_active);

#endif
