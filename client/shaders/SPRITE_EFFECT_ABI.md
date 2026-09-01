# GPU sprite effect ABI

`sprite_effect_abi.inc` is the authoritative field order for one world-sprite
instance. The C upload type and the HLSL `SpriteInstance` include that file;
the generated SPIR-V, DXIL, and MSL artifacts therefore share one 144-byte
stride. Every field starts at a fixed 16-byte lane or is one of four scalar
words in the metadata/timing lane. `abi_version` is `1` for this contract.

## Instance fields

| Field | Contract |
| --- | --- |
| `destination` | Target-pixel x, y, width, and height. CPU clipping and painter order remain authoritative. |
| `uv` | Normalized visible source rectangle. It never contains a pointer or process-local texture identity. |
| `modulation` | Straight-alpha RGBA modulation, normalized to `[0, 1]`; alpha is multiplicative. |
| `effect_flags` | Versioned feature bits. Color modes are mutually exclusive; overlay and transform bits may combine. |
| `texture_flags` | Exactly one atlas/standalone bit, exactly one straight/premultiplied alpha bit, plus source color-key provenance, nearest filtering, and clamp-to-edge policy. |
| `lighting_key` | Existing compact light-row key, including projected-light encoding; the default is the explicit unlit value. It is presentation-only and does not grant visibility. |
| `owner_depth` | Opaque semantic owner/depth bytes for later lighting/mask consumers; unset bytes are `0xff`. High bits are zero. |
| `texture_metadata` | Texture texel size followed by the UV inset used to keep samples inside the visible source rectangle. Current nearest uploads use a half-texel inset. |
| `transform` | Rotation in radians, nonuniform x/y scale, and a reserved scalar. A zero/default instance has no transform. |
| `effect_color` | Effect color in normalized RGBA form for tint and transient/mask consumers. |
| `effect_parameters` | Strength, transient alpha, glow width, and outline width. Unsupported mask geometry remains a deferred child in ABI v1. |
| `effect_time`, `effect_phase`, `effect_seed` | Stable producer-supplied `uint32` deterministic inputs. `effect_time` is simulation/UI milliseconds modulo `2^32`; `effect_phase` is an unsigned Q16.16 cycle phase with wraparound; `effect_seed` is an opaque stable per-effect value, never a pointer or address. Shaders may animate from them without reading a wall clock or allocating a surface. |
| `abi_version` | Must equal `1`; unknown versions fail before upload. |

Effect bits are assigned from low to high in this order: `TINT` (0), `DARK`
(1), `GRAY` (2), `RED` (3), `FOG` (4), `TRANSIENT_OVERLAY` (5), `ROTATE`
(6), `SCALE` (7), `STRETCH` (8), `GLOW` (9), `OUTLINE` (10), `MASK_INPUT`
(11), and `LIGHT_INPUT` (12). `DARK`, `GRAY`, `RED`, and `FOG` are mutually
exclusive presentation modes; all other known bits may combine subject to
the resource rules below. Texture bits are `ATLAS` (0), `STANDALONE` (1),
`SOURCE_COLOR_KEY` (2), `STRAIGHT_ALPHA` (3), `PREMULTIPLIED_ALPHA` (4),
`NEAREST` (5), and `CLAMP_EDGE` (6). Exactly one storage bit and one alpha
representation bit must be set. `owner_depth` packs owner in bits 0-7 and
depth in bits 8-15; bits 16-31 are zero.

## Effect ownership matrix

| Existing CPU effect | ABI destination | ABI v1 status |
| --- | --- | --- |
| Tint/modulation and alpha | `modulation`, `effect_color`, fragment multiplication | Contracted; current producer still supplies the already-authorized SDL surface. |
| Dark | `DARK` and `effect_parameters.x`, fragment scalar operation | Contracted for the follow-up migration. |
| Gray/invisibility | `GRAY`, fragment luminance operation | Contracted for the follow-up migration. |
| Red/infravision | `RED`, fragment luminance-to-red operation | Contracted for the follow-up migration. |
| Fog of war | `FOG`, fragment presentation operation | Contracted; CPU visibility and remembered disclosure stay outside the shader. |
| Smooth structural/projected lighting | `lighting_key` and the existing CPU-built light buffers | Contracted; the key selects presentation data and never grants visibility. |
| Weather/effect overlay (`SPRITE_FLAG_EFFECTS`) | `TRANSIENT_OVERLAY`, `effect_color`, `effect_parameters`, and deterministic inputs | Basic RGBA overlay is contracted; configuration-selected per-channel coefficients and random variation are an explicit deferred child. |
| Transient visibility fade/overlay | `TRANSIENT_OVERLAY`, `effect_color`, `effect_parameters.y`, stable phase | Contracted; disclosure and fade eligibility remain CPU-owned. |
| Rotation and nonuniform zoom | `ROTATE`/`SCALE`, `transform`, vertex operation | Contracted for direct-source consumers; no CPU transform migration is implied here. |
| Tile stretch | `STRETCH` and destination geometry | Deferred child: traversal and directional stretch geometry remain CPU-owned until a mask/geometry lane exists. |
| Glow and outline | `GLOW`/`OUTLINE`, effect color/width, optional mask input | Deferred child: silhouette dilation and outline-only masking require an explicit mask binding. |
| Future mask/light inputs | `MASK_INPUT`/`LIGHT_INPUT`, owner/depth and optional bindings | Reserved and explicitly deferred; no flag is set without the corresponding bound resource contract. |
| Text rasterization | none | CPU-only; text must not acquire a sprite effect flag. |

The CPU owns authorized visibility, FoW/remembered disclosure, map traversal,
painter order, scene identity, clipping, and text rasterization. The shader
performs visual math only for fields it receives. There is no software fallback
in this contract.

ABI v1 uses one data-driven world pipeline. Effect and texture bits select
operations in the existing vertex/fragment pair; they do not select a
per-effect pipeline or a runtime-compiled shader. The texture remains an
explicit per-batch binding, while optional mask/light resources must be added
as a separately versioned binding contract before `MASK_INPUT` or `LIGHT_INPUT`
is set.

`effect_time` is sampled from the deterministic simulation/UI clock selected by
the CPU producer, not from `SDL_GetTicks()` in shader code. `effect_phase` uses
`0x00010000` for one complete cycle and wraps modulo `0x00010000`; producers
must retain the same phase for the same effect identity across a retained-frame
replay. `effect_seed` is derived from stable scene/effect identity and may be
zero; it must not be derived from an allocation address, pointer value, or
unordered traversal.

ABI v1 uses luminance coefficients `(0.212671, 0.715160, 0.072169)` for gray
and red presentation. Dark scales RGB by clamped `effect_parameters.x`; fog
scales luminance by `0.34` and adds `16/255` to blue. All of these operations
preserve sampled alpha until the normal straight-alpha blend, whose color
factors are source-alpha and one-minus-source-alpha and whose alpha factors
are one and one-minus-source-alpha.

## Texture and lifetime rules

The producer retains the SDL_GPU texture/atlas resource for every queued and
retained instance, releases it only after the corresponding retained slot is
replaced or destroyed, and uploads no process-local address. Source color keys
are converted to transparent alpha during the immutable RGBA upload. The
current world blend state is straight-alpha: a premultiplied source is
unpremultiplied in the fragment stage before modulation and effect math, while
straight-alpha sources are used as-is. The current map sampler is nearest and
clamp-to-edge. UVs are clamped to the visible rectangle with a half-texel inset
so an atlas edge cannot sample an adjacent allocation; a future filtered cohort
must retain the same metadata rule and provide padded edge texels.

Reset, resize, resource replacement, and device recovery discard or rebuild
the GPU resource bindings and re-upload the same value instances. The instance
payload itself is deterministic and reusable; it carries no SDL object lifetime.

If a source texture is missing, cannot be retained, or fails upload, the
producer rejects that draw and releases any partial transfer/asset state; it
never binds a null texture, queues an invalid instance, or falls back to the
software renderer. A surface-generation replacement leaves the old resource
alive for already queued/retained commands and gives the replacement a new
asset generation. Renderer reset, resize, or device recovery invalidates the
affected bindings and rebuilds them from the original source on the next
redraw; a failed rebuild is reported as a GPU-renderer failure and is not
silently converted into a CPU draw.
