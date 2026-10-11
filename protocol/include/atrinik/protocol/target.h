/* Copyright 2026 The Atrinik Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ATRINIK_PROTOCOL_TARGET_H
#define ATRINIK_PROTOCOL_TARGET_H

/* TARGET revision 1084; command IDs and subtype IDs remain unchanged.
 * PLAYING only. Integers use the existing packet network byte order.
 * C2S SELECT: subtype:u8, x:u8, y:u8, relative_depth:i8, actor_count:u32
 * (exactly 8 bytes); CLEAR: subtype:u8 (exactly 1 byte). Coordinates address
 * the advertised viewport; depth addresses the live MAP2 level. SELECT binds
 * an exact physical part to its head's actor count. Count zero is never a
 * wildcard. Malformed packets preserve target state. A valid unauthorized
 * selection returns SELF without describing the rejected actor.
 * S2C: type:u8, color:cstring, name:cstring, level:u8, actor_count:u32,
 * relative_depth:i8, combat:u8, combat_force:u8. No trailing bytes. SELF has
 * count=0 and depth=0; non-self has nonzero count. Parse before mutation.
 * Reconnect clears target identity together with all other player state.
 */
#define TARGET_SELECT_PAYLOAD_SIZE 8U
#define TARGET_CLEAR_PAYLOAD_SIZE 1U

#endif
