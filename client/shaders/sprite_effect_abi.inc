/*
 *           Atrinik, a Multiplayer Online Role Playing Game
 *
 *   Copyright 2026 The Atrinik Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Shared GPU sprite-instance field order.
 *
 * The including language defines GPU_SPRITE_ABI_FLOAT4(name) and
 * GPU_SPRITE_ABI_UINT(name). FLOAT4 fields occupy one 16-byte lane; four
 * consecutive UINT fields occupy one 16-byte lane. Keep this file free of
 * language-specific types so C and HLSL consume the same declaration.
 */

/* Lane 0: destination rectangle in target pixels, x/y/width/height. */
GPU_SPRITE_ABI_FLOAT4(destination)
/* Lane 1: normalized visible texture rectangle, u/v/width/height. */
GPU_SPRITE_ABI_FLOAT4(uv)
/* Lane 2: straight-alpha RGBA modulation. */
GPU_SPRITE_ABI_FLOAT4(modulation)

/* Lane 3: feature and presentation metadata. */
GPU_SPRITE_ABI_UINT(effect_flags)
GPU_SPRITE_ABI_UINT(texture_flags)
GPU_SPRITE_ABI_UINT(lighting_key)
GPU_SPRITE_ABI_UINT(owner_depth)

/* Lane 4: texture metadata: texel size x/y, UV clamp inset x/y. */
GPU_SPRITE_ABI_FLOAT4(texture_metadata)
/* Lane 5: rotation radians, scale x/y, reserved stretch scalar. */
GPU_SPRITE_ABI_FLOAT4(transform)
/* Lane 6: effect color, interpreted by the selected effect operation. */
GPU_SPRITE_ABI_FLOAT4(effect_color)
/* Lane 7: strength, transient alpha, glow width, outline width. */
GPU_SPRITE_ABI_FLOAT4(effect_parameters)

/* Lane 8: deterministic animation inputs and ABI version. */
GPU_SPRITE_ABI_UINT(effect_time)
GPU_SPRITE_ABI_UINT(effect_phase)
GPU_SPRITE_ABI_UINT(effect_seed)
GPU_SPRITE_ABI_UINT(abi_version)
