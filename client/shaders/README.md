# GPU shader build

`map.hlsl` is the single authoritative shader source. Every client build embeds
SPIR-V, DXIL, and MSL artifacts generated from it; the client has no runtime
shader compiler. Generated binaries live only in ignored build directories and
release-package staging, never in Git or Git LFS.

The pinned toolchain is:

- Microsoft DirectXShaderCompiler `v1.9.2607`, Linux archive SHA-256
  `55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54`;
- KhronosGroup SPIRV-Cross
  `9c3c8e2cefdd8194b193bb8ed2fdff4d5527e382`.

`toolchain.lock.json` pins the compiler inputs and their checksums.
`SHA256SUMS` is the small, reviewable output lock: shader changes intentionally
update it, while every build rejects compiler output that does not reproduce the
locked cohort.

For an x86-64 Linux build without system `dxc` and `spirv-cross`, prepare the
pinned tools once:

```sh
python3 tools/prepare_gpu_shader_toolchain.py \
  --cache build/gpu-shader-downloads \
  --output build/gpu-shader-toolchain
```

CMake discovers that default location and generates shaders in its binary
directory. On macOS and other development hosts, install compatible system
`dxc` and `spirv-cross` tools or pass a validated external cohort. CI and
packaging generate one validated cohort and pass its absolute directory through
`ATRINIK_GPU_SHADER_DIRECTORY` to isolated offline builds.
