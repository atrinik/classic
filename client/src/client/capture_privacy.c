/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <capture_privacy.h>

static bool blocked = true;
static bool composing;
static bool composition_sensitive;
static bool shared_chat_retained;

void capture_privacy_shared_chat_retained(bool retained) {
    shared_chat_retained = retained;
    if (retained) {
        capture_privacy_block();
    }
}

void capture_privacy_block(void) {
    blocked = true;
    if (composing) {
        composition_sensitive = true;
    }
}

void capture_privacy_frame_begin(bool sensitive_active) {
    blocked = true;
    composing = true;
    composition_sensitive = sensitive_active || shared_chat_retained;
}

void capture_privacy_frame_end(bool presented) {
    if (composing && presented && !composition_sensitive) {
        blocked = false;
    }
    composing = false;
}

bool capture_privacy_allowed(bool sensitive_active) {
    return !blocked && !composing && !sensitive_active && !shared_chat_retained;
}
