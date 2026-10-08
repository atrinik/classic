/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <capture_privacy.h>
#include <stdio.h>

#define CHECK(value)                                                            \
    do {                                                                        \
        if (!(value)) {                                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #value);                 \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void) {
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_end(true); /* No composition cannot grant admission. */
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_end(true);
    CHECK(capture_privacy_allowed(false));
    CHECK(!capture_privacy_allowed(true));

    capture_privacy_block(); /* Private UI loaded, then closed before redraw. */
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(true);
    capture_privacy_frame_end(true);
    CHECK(!capture_privacy_allowed(false)); /* Closed after composition. */
    capture_privacy_frame_begin(false);
    capture_privacy_block(); /* Loaded and reset during composition. */
    capture_privacy_frame_end(true);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(false); /* Failed/no presentation. */
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_block(); /* Recovery alone cannot grant admission. */
    capture_privacy_frame_end(true);
    CHECK(!capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(true);
    CHECK(capture_privacy_allowed(false));
    capture_privacy_frame_begin(false);
    capture_privacy_frame_end(false); /* Failed clean redraw is conservative. */
    CHECK(!capture_privacy_allowed(false));
    return 0;
}
