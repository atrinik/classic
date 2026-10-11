/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ATRINIK_PROTOCOL_TALK_H
#define ATRINIK_PROTOCOL_TALK_H

#include <atrinik/protocol/game_commands.h>

/* TALK revision 1084, PLAYING only. Existing modes 1..6 retain their layouts.
 * NPC_TARGET (7): mode:u8, viewport_x:u8, viewport_y:u8, relative_depth:i8,
 * actor_count:u32, message:cstring. Integers use network byte order. The
 * nonempty message is bounded in bytes, excluding its required NUL terminator;
 * no trailing bytes are accepted. Count must be nonzero. All fields validate
 * before admission or mutation. Coordinates/depth bind an exact previously
 * published live MAP2 part to its head count. The server rechecks publication,
 * fresh perception and ordinary planar NPC talk range when executing it.
 * Rejection never falls back to another NPC or selects the requested actor.
 * Invalidated retained actors are cleared; malformed input preserves state.
 * Success uses the normal conversation and selection update.
 * One valid request per connection/tick, independent of SELECT admission;
 * stale/unpublished requests consume this allowance. No retained pointers or
 * deferred action. Old modes preserve ordinary typed-talk behavior.
 */
#define TALK_NPC_TARGET_FIXED_SIZE 8U

#endif
