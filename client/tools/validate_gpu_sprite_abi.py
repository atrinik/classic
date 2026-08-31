#!/usr/bin/env python3
"""Validate the shared C/HLSL GPU sprite-instance declaration."""

from __future__ import annotations

import json
from pathlib import Path
import re


CLIENT_ROOT = Path(__file__).resolve().parents[1]
SHARED_FIELDS = (
    ("FLOAT4", "destination"),
    ("FLOAT4", "uv"),
    ("FLOAT4", "modulation"),
    ("UINT", "effect_flags"),
    ("UINT", "texture_flags"),
    ("UINT", "lighting_key"),
    ("UINT", "owner_depth"),
    ("FLOAT4", "texture_metadata"),
    ("FLOAT4", "transform"),
    ("FLOAT4", "effect_color"),
    ("FLOAT4", "effect_parameters"),
    ("UINT", "effect_time"),
    ("UINT", "effect_phase"),
    ("UINT", "effect_seed"),
    ("UINT", "abi_version"),
)
FIELD_PATTERN = re.compile(
    r"^\s*GPU_SPRITE_ABI_(FLOAT4|UINT)\(([a-z][a-z0-9_]*)\)\s*$",
    re.MULTILINE,
)
EFFECT_BITS = (
    ("SPRITE_EFFECT_TINT", 0),
    ("SPRITE_EFFECT_DARK", 1),
    ("SPRITE_EFFECT_GRAY", 2),
    ("SPRITE_EFFECT_RED", 3),
    ("SPRITE_EFFECT_FOG", 4),
    ("SPRITE_EFFECT_TRANSIENT_OVERLAY", 5),
    ("SPRITE_EFFECT_ROTATE", 6),
    ("SPRITE_EFFECT_SCALE", 7),
    ("SPRITE_EFFECT_STRETCH", 8),
    ("SPRITE_EFFECT_GLOW", 9),
    ("SPRITE_EFFECT_OUTLINE", 10),
    ("SPRITE_EFFECT_MASK_INPUT", 11),
    ("SPRITE_EFFECT_LIGHT_INPUT", 12),
)
TEXTURE_BITS = (
    ("SPRITE_TEXTURE_ATLAS", 0),
    ("SPRITE_TEXTURE_STANDALONE", 1),
    ("SPRITE_TEXTURE_SOURCE_COLOR_KEY", 2),
    ("SPRITE_TEXTURE_STRAIGHT_ALPHA", 3),
    ("SPRITE_TEXTURE_PREMULTIPLIED_ALPHA", 4),
    ("SPRITE_TEXTURE_NEAREST", 5),
    ("SPRITE_TEXTURE_CLAMP_EDGE", 6),
)


def fail(message: str) -> None:
    raise ValueError(message)


def validate() -> dict[str, object]:
    shared_path = CLIENT_ROOT / "shaders/sprite_effect_abi.inc"
    header_path = CLIENT_ROOT / "src/include/gpu_sprite_effect.h"
    shader_path = CLIENT_ROOT / "shaders/map.hlsl"
    renderer_path = CLIENT_ROOT / "src/client/gpu_map_renderer.c"
    shared = shared_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    shader = shader_path.read_text(encoding="utf-8")
    renderer = renderer_path.read_text(encoding="utf-8")

    fields = tuple(FIELD_PATTERN.findall(shared))
    if fields != SHARED_FIELDS:
        fail(f"shared ABI field order mismatch: {fields!r}")
    if '#include "../../shaders/sprite_effect_abi.inc"' not in header:
        fail("C ABI does not include the shared field declaration")
    if '#include "sprite_effect_abi.inc"' not in shader:
        fail("HLSL ABI does not include the shared field declaration")
    if "#define GPU_SPRITE_INSTANCE_STRIDE UINT32_C(144)" not in header:
        fail("C ABI stride is not 144 bytes")
    for name, bit in EFFECT_BITS + TEXTURE_BITS:
        declaration = f"static const uint {name} = 1u << {bit}u;"
        if declaration not in shader:
            fail(f"shader is missing stable bit declaration {name}")
        if f"#define GPU_{name}" not in header:
            fail(f"C ABI is missing stable bit declaration {name}")
    for name in (
        "effect_flags",
        "texture_flags",
        "lighting_key",
        "owner_depth",
        "texture_metadata",
        "transform",
        "effect_color",
        "effect_parameters",
        "effect_time",
        "effect_phase",
        "effect_seed",
    ):
        if f"instance.{name}" not in shader:
            fail(f"shader does not consume ABI field {name}")
    if "StructuredBuffer<SpriteInstance> world_instances" not in shader:
        fail("world shader does not bind SpriteInstance records")
    for declaration in (
        "#include <gpu_sprite_effect.h>",
        "typedef gpu_sprite_instance_t gpu_map_world_instance_t;",
        ".texture_flags = asset->texture_flags",
        ".texture_metadata =",
        ".owner_depth =",
        ".abi_version = GPU_SPRITE_INSTANCE_ABI_VERSION",
        "HARD_ASSERT(gpu_sprite_instance_valid(&instance));",
    ):
        if declaration not in renderer:
            fail(f"production map renderer is missing ABI binding {declaration}")

    return {
        "field_count": len(fields),
        "renderer_binding": "gpu_map_renderer.c",
        "stride": 144,
        "shared_declaration": str(shared_path.relative_to(CLIENT_ROOT)),
        "validated": True,
    }


if __name__ == "__main__":
    print(json.dumps(validate(), sort_keys=True))
