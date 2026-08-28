/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright 2026 The Atrinik Project                                  *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 ************************************************************************/

/**
 * @file
 * Raw SDL_GPU albedo/owner map passes and retained source textures.
 */

#include <global.h>
#include <gpu_shader_data.h>

#define GPU_MAP_SURFACE_GENERATION_PROPERTY "atrinik.gpu.map_surface_generation"
#define GPU_MAP_OWNER_TRANSPARENT UINT8_MAX
#define GPU_MAP_OWNER_UNLIT (UINT8_MAX - 1)

typedef struct gpu_map_asset {
    SDL_Surface *surface;
    SDL_GPUTexture *texture;
    Uint64 generation;
    uint32_t width;
    uint32_t height;
    size_t bytes;
    struct gpu_map_asset *next;
} gpu_map_asset_t;

typedef struct gpu_map_shader_blob {
    const Uint8 *code;
    size_t size;
    SDL_GPUShaderFormat format;
    const char *entrypoint;
} gpu_map_shader_blob_t;

typedef struct gpu_map_vertex_uniforms {
    float destination[4];
    float uv[4];
    float viewport[2];
    float padding[2];
} gpu_map_vertex_uniforms_t;

typedef struct gpu_map_fragment_uniforms {
    float modulation[4];
    uint32_t owner;
    uint32_t padding[3];
} gpu_map_fragment_uniforms_t;

static SDL_GPUDevice *map_device;
static SDL_Renderer *map_renderer;
static SDL_GPUSampler *map_sampler;
static SDL_GPUGraphicsPipeline *world_pipeline;
static SDL_GPUGraphicsPipeline *final_pipeline;
static SDL_GPUTexture *albedo_target;
static SDL_GPUTexture *owner_target;
static SDL_GPUTexture *final_target;
static SDL_Texture *wrapped_final_target;
static SDL_GPUCommandBuffer *map_command_buffer;
static SDL_GPURenderPass *world_pass;
static SDL_Surface *solid_surface;
static gpu_map_asset_t *assets;
static Uint64 next_generation = 1;
static int target_width;
static int target_height;
static uint8_t current_owner = GPU_MAP_OWNER_UNLIT;
static bool world_pass_has_content;

static void gpu_map_world_pass_end(void);

static void gpu_map_command_cancel(void) {
    gpu_map_world_pass_end();
    if (map_command_buffer != NULL) {
        SDL_CancelGPUCommandBuffer(map_command_buffer);
        map_command_buffer = NULL;
    }
}

static gpu_map_shader_blob_t gpu_map_shader_blob(const char *name,
                                                 const char *entrypoint) {
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(map_device);
#define GPU_SHADER_BLOB(_name, _extension, _format, _entrypoint)                  \
    (gpu_map_shader_blob_t) {                                                     \
        .code = gpu_shader_##_name##_##_extension,                                \
        .size = gpu_shader_##_name##_##_extension##_size,                         \
        .format = _format,                                                        \
        .entrypoint = _entrypoint,                                                \
    }
#define GPU_SHADER_SELECT(_name)                                                  \
    do {                                                                          \
        if (strcmp(name, #_name) == 0) {                                          \
            if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {                           \
                return GPU_SHADER_BLOB(_name, spv, SDL_GPU_SHADERFORMAT_SPIRV,    \
                                       entrypoint);                                \
            }                                                                     \
            if (formats & SDL_GPU_SHADERFORMAT_DXIL) {                            \
                return GPU_SHADER_BLOB(_name, dxil, SDL_GPU_SHADERFORMAT_DXIL,    \
                                       entrypoint);                                \
            }                                                                     \
            if (formats & SDL_GPU_SHADERFORMAT_MSL) {                             \
                return GPU_SHADER_BLOB(_name, msl, SDL_GPU_SHADERFORMAT_MSL,      \
                                       "main0");                                  \
            }                                                                     \
        }                                                                         \
    } while (0)
    GPU_SHADER_SELECT(world_vertex);
    GPU_SHADER_SELECT(world_fragment);
    GPU_SHADER_SELECT(final_vertex);
    GPU_SHADER_SELECT(final_fragment);
#undef GPU_SHADER_SELECT
#undef GPU_SHADER_BLOB
    return (gpu_map_shader_blob_t){0};
}

static SDL_GPUShader *gpu_map_shader_create(const char *name,
                                            const char *entrypoint,
                                            SDL_GPUShaderStage stage,
                                            uint32_t samplers,
                                            uint32_t uniforms) {
    gpu_map_shader_blob_t blob = gpu_map_shader_blob(name, entrypoint);
    if (blob.code == NULL) {
        SDL_SetError("No supported precompiled format for GPU shader %s", name);
        return NULL;
    }
    SDL_GPUShaderCreateInfo info = {
        .code_size = blob.size,
        .code = blob.code,
        .entrypoint = blob.entrypoint,
        .format = blob.format,
        .stage = stage,
        .num_samplers = samplers,
        .num_uniform_buffers = uniforms,
    };
    return SDL_CreateGPUShader(map_device, &info);
}

static bool gpu_map_pipelines_create(void) {
    SDL_GPUShader *world_vertex = gpu_map_shader_create("world_vertex",
                                                        "world_vertex",
                                                        SDL_GPU_SHADERSTAGE_VERTEX,
                                                        0,
                                                        1);
    SDL_GPUShader *world_fragment = gpu_map_shader_create("world_fragment",
                                                          "world_fragment",
                                                          SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                          1,
                                                          1);
    SDL_GPUShader *final_vertex = gpu_map_shader_create("final_vertex",
                                                        "final_vertex",
                                                        SDL_GPU_SHADERSTAGE_VERTEX,
                                                        0,
                                                        0);
    SDL_GPUShader *final_fragment = gpu_map_shader_create("final_fragment",
                                                          "final_fragment",
                                                          SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                          2,
                                                          0);
    if (world_vertex == NULL || world_fragment == NULL || final_vertex == NULL ||
        final_fragment == NULL) {
        SDL_ReleaseGPUShader(map_device, world_vertex);
        SDL_ReleaseGPUShader(map_device, world_fragment);
        SDL_ReleaseGPUShader(map_device, final_vertex);
        SDL_ReleaseGPUShader(map_device, final_fragment);
        return false;
    }

    SDL_GPUColorTargetDescription world_targets[2] = {
        {
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .blend_state = {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            },
        },
        {.format = SDL_GPU_TEXTUREFORMAT_R8_UINT},
    };
    SDL_GPUGraphicsPipelineCreateInfo world_info = {
        .vertex_shader = world_vertex,
        .fragment_shader = world_fragment,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
        },
        .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
        .target_info = {
            .color_target_descriptions = world_targets,
            .num_color_targets = SDL_arraysize(world_targets),
        },
    };
    world_pipeline = SDL_CreateGPUGraphicsPipeline(map_device, &world_info);

    SDL_GPUColorTargetDescription final_description = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    };
    SDL_GPUGraphicsPipelineCreateInfo final_info = {
        .vertex_shader = final_vertex,
        .fragment_shader = final_fragment,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {
            .fill_mode = SDL_GPU_FILLMODE_FILL,
            .cull_mode = SDL_GPU_CULLMODE_NONE,
            .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
        },
        .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
        .target_info = {
            .color_target_descriptions = &final_description,
            .num_color_targets = 1,
        },
    };
    final_pipeline = SDL_CreateGPUGraphicsPipeline(map_device, &final_info);
    SDL_ReleaseGPUShader(map_device, world_vertex);
    SDL_ReleaseGPUShader(map_device, world_fragment);
    SDL_ReleaseGPUShader(map_device, final_vertex);
    SDL_ReleaseGPUShader(map_device, final_fragment);
    return world_pipeline != NULL && final_pipeline != NULL;
}

static void gpu_map_targets_destroy(void) {
    if (wrapped_final_target != NULL) {
        SDL_DestroyTexture(wrapped_final_target);
        wrapped_final_target = NULL;
    }
    SDL_ReleaseGPUTexture(map_device, albedo_target);
    SDL_ReleaseGPUTexture(map_device, owner_target);
    SDL_ReleaseGPUTexture(map_device, final_target);
    albedo_target = NULL;
    owner_target = NULL;
    final_target = NULL;
    target_width = 0;
    target_height = 0;
}

static bool gpu_map_targets_create(int width, int height) {
    if (albedo_target != NULL && target_width == width && target_height == height) {
        return true;
    }
    gpu_map_targets_destroy();
    SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    albedo_target = SDL_CreateGPUTexture(map_device, &info);
    final_target = SDL_CreateGPUTexture(map_device, &info);
    info.format = SDL_GPU_TEXTUREFORMAT_R8_UINT;
    owner_target = SDL_CreateGPUTexture(map_device, &info);
    if (albedo_target == NULL || owner_target == NULL || final_target == NULL) {
        gpu_map_targets_destroy();
        return false;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    bool configured = properties != 0 &&
                      SDL_SetNumberProperty(properties,
                                            SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                            SDL_PIXELFORMAT_RGBA32) &&
                      SDL_SetNumberProperty(properties,
                                            SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                            SDL_TEXTUREACCESS_STATIC) &&
                      SDL_SetNumberProperty(properties,
                                            SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                            width) &&
                      SDL_SetNumberProperty(properties,
                                            SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                                            height) &&
                      SDL_SetPointerProperty(properties,
                                             SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,
                                             final_target);
    if (configured) {
        wrapped_final_target = SDL_CreateTextureWithProperties(map_renderer, properties);
    }
    SDL_DestroyProperties(properties);
    if (!configured || wrapped_final_target == NULL ||
        !SDL_SetTextureScaleMode(wrapped_final_target, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(wrapped_final_target, SDL_BLENDMODE_BLEND)) {
        gpu_map_targets_destroy();
        return false;
    }
    target_width = width;
    target_height = height;
    return true;
}

static void gpu_map_asset_destroy(gpu_map_asset_t *asset) {
    SDL_ReleaseGPUTexture(map_device, asset->texture);
    free(asset);
}

static void gpu_map_assets_destroy(void) {
    while (assets != NULL) {
        gpu_map_asset_t *asset = assets;
        assets = asset->next;
        gpu_map_asset_destroy(asset);
    }
}

static bool gpu_map_world_pass_begin(void) {
    if (world_pass != NULL) {
        return true;
    }
    SDL_GPUColorTargetInfo targets[2] = {
        {
            .texture = albedo_target,
            .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
            .load_op = world_pass_has_content ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE,
        },
        {
            .texture = owner_target,
            .clear_color = {(float)GPU_MAP_OWNER_TRANSPARENT, 0.0f, 0.0f, 0.0f},
            .load_op = world_pass_has_content ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE,
        },
    };
    world_pass = SDL_BeginGPURenderPass(map_command_buffer, targets, SDL_arraysize(targets), NULL);
    if (world_pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(world_pass, world_pipeline);
    return true;
}

static void gpu_map_world_pass_end(void) {
    if (world_pass != NULL) {
        SDL_EndGPURenderPass(world_pass);
        world_pass = NULL;
        world_pass_has_content = true;
    }
}

static SDL_Surface *gpu_map_upload_surface(SDL_Surface *surface) {
    SDL_Surface *upload = SDL_CreateSurface(surface->w, surface->h, SDL_PIXELFORMAT_RGBA32);
    if (upload == NULL || !SDL_FillSurfaceRect(upload, NULL, 0)) {
        SDL_DestroySurface(upload);
        return NULL;
    }
    Uint8 red, green, blue, alpha;
    SDL_BlendMode blend;
    if (!SDL_GetSurfaceColorMod(surface, &red, &green, &blue) ||
        !SDL_GetSurfaceAlphaMod(surface, &alpha) ||
        !SDL_GetSurfaceBlendMode(surface, &blend) ||
        !SDL_SetSurfaceColorMod(surface, 255, 255, 255) ||
        !SDL_SetSurfaceAlphaMod(surface, 255) ||
        !SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE)) {
        SDL_DestroySurface(upload);
        return NULL;
    }
    bool copied = SDL_BlitSurface(surface, NULL, upload, NULL);
    bool color_restored = SDL_SetSurfaceColorMod(surface, red, green, blue);
    bool alpha_restored = SDL_SetSurfaceAlphaMod(surface, alpha);
    bool blend_restored = SDL_SetSurfaceBlendMode(surface, blend);
    bool restored = color_restored && alpha_restored && blend_restored;
    if (!copied || !restored) {
        SDL_DestroySurface(upload);
        return NULL;
    }
    return upload;
}

static gpu_map_asset_t *gpu_map_asset_create(SDL_Surface *surface) {
    SDL_Surface *upload = gpu_map_upload_surface(surface);
    if (upload == NULL) {
        return NULL;
    }
    uint32_t row_bytes = (uint32_t)upload->w * 4U;
    uint32_t aligned_row_bytes = (row_bytes + 255U) & ~UINT32_C(255);
    uint64_t transfer_size = (uint64_t)aligned_row_bytes * (uint32_t)upload->h;
    if (transfer_size == 0 || transfer_size > UINT32_MAX) {
        SDL_DestroySurface(upload);
        SDL_SetError("GPU map texture upload is too large");
        return NULL;
    }

    SDL_GPUTextureCreateInfo texture_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (uint32_t)upload->w,
        .height = (uint32_t)upload->h,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(map_device, &texture_info);
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = (uint32_t)transfer_size,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    Uint8 *destination = transfer != NULL ? SDL_MapGPUTransferBuffer(map_device, transfer, false)
                                         : NULL;
    if (texture == NULL || transfer == NULL || destination == NULL) {
        if (destination != NULL) {
            SDL_UnmapGPUTransferBuffer(map_device, transfer);
        }
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        SDL_ReleaseGPUTexture(map_device, texture);
        SDL_DestroySurface(upload);
        return NULL;
    }
    for (int y = 0; y < upload->h; y++) {
        memcpy(destination + (size_t)y * aligned_row_bytes,
               (const Uint8 *)upload->pixels + (size_t)y * (size_t)upload->pitch,
               row_bytes);
    }
    SDL_UnmapGPUTransferBuffer(map_device, transfer);
    SDL_DestroySurface(upload);

    gpu_map_world_pass_end();
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(map_command_buffer);
    if (copy == NULL) {
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        SDL_ReleaseGPUTexture(map_device, texture);
        return NULL;
    }
    SDL_GPUTextureTransferInfo source = {
        .transfer_buffer = transfer,
        .pixels_per_row = aligned_row_bytes / 4U,
        .rows_per_layer = (uint32_t)surface->h,
    };
    SDL_GPUTextureRegion destination_region = {
        .texture = texture,
        .w = (uint32_t)surface->w,
        .h = (uint32_t)surface->h,
        .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &source, &destination_region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(map_device, transfer);

    gpu_map_asset_t *asset = xcalloc(1, sizeof(*asset));
    asset->surface = surface;
    asset->texture = texture;
    asset->width = (uint32_t)surface->w;
    asset->height = (uint32_t)surface->h;
    asset->bytes = (size_t)surface->w * (size_t)surface->h * 4U;
    asset->generation = next_generation++;
    if (next_generation == 0) {
        next_generation = 1;
    }
    SDL_SetNumberProperty(SDL_GetSurfaceProperties(surface),
                          GPU_MAP_SURFACE_GENERATION_PROPERTY,
                          (Sint64)asset->generation);
    asset->next = assets;
    assets = asset;
    return asset;
}

static gpu_map_asset_t *gpu_map_asset(SDL_Surface *surface) {
    Uint64 generation = SDL_GetNumberProperty(SDL_GetSurfaceProperties(surface),
                                              GPU_MAP_SURFACE_GENERATION_PROPERTY,
                                              0);
    gpu_map_asset_t **link = &assets;
    while (*link != NULL) {
        gpu_map_asset_t *asset = *link;
        if (asset->surface == surface) {
            if (generation != 0 && asset->generation == generation) {
                return asset;
            }
            *link = asset->next;
            gpu_map_asset_destroy(asset);
            break;
        }
        link = &asset->next;
    }
    return gpu_map_asset_create(surface);
}

bool gpu_map_renderer_create(SDL_GPUDevice *device, SDL_Renderer *renderer) {
    HARD_ASSERT(device != NULL);
    HARD_ASSERT(renderer != NULL);
    gpu_map_renderer_destroy();
    map_device = device;
    map_renderer = renderer;
    solid_surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    SDL_GPUSamplerCreateInfo sampler_info = {
        .min_filter = SDL_GPU_FILTER_NEAREST,
        .mag_filter = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    };
    map_sampler = SDL_CreateGPUSampler(map_device, &sampler_info);
    if (solid_surface == NULL ||
        !SDL_FillSurfaceRect(solid_surface, NULL, UINT32_MAX) || map_sampler == NULL ||
        !gpu_map_pipelines_create()) {
        gpu_map_renderer_destroy();
        return false;
    }
    return true;
}

void gpu_map_renderer_destroy(void) {
    if (map_device == NULL) {
        return;
    }
    gpu_map_world_pass_end();
    if (map_command_buffer != NULL) {
        SDL_CancelGPUCommandBuffer(map_command_buffer);
        map_command_buffer = NULL;
    }
    gpu_map_assets_destroy();
    SDL_DestroySurface(solid_surface);
    solid_surface = NULL;
    gpu_map_targets_destroy();
    SDL_ReleaseGPUGraphicsPipeline(map_device, world_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(map_device, final_pipeline);
    SDL_ReleaseGPUSampler(map_device, map_sampler);
    world_pipeline = NULL;
    final_pipeline = NULL;
    map_sampler = NULL;
    map_device = NULL;
    map_renderer = NULL;
}

bool gpu_map_renderer_begin(int width, int height) {
    if (map_device == NULL || width <= 0 || height <= 0 ||
        !SDL_FlushRenderer(map_renderer) || !gpu_map_targets_create(width, height)) {
        return false;
    }
    map_command_buffer = SDL_AcquireGPUCommandBuffer(map_device);
    world_pass = NULL;
    world_pass_has_content = false;
    current_owner = GPU_MAP_OWNER_UNLIT;
    return map_command_buffer != NULL;
}

bool gpu_map_renderer_active(void) {
    return map_command_buffer != NULL;
}

void gpu_map_renderer_set_owner(uint8_t owner) {
    current_owner = owner;
}

bool gpu_map_renderer_draw_surface(SDL_Surface *surface,
                                   const SDL_Rect *source,
                                   const SDL_FRect *destination) {
    if (map_command_buffer == NULL || surface == NULL || destination == NULL ||
        destination->w <= 0.0f || destination->h <= 0.0f) {
        return false;
    }
    gpu_map_asset_t *asset = gpu_map_asset(surface);
    if (asset == NULL || !gpu_map_world_pass_begin()) {
        return false;
    }
    Uint8 red, green, blue, alpha;
    if (!SDL_GetSurfaceColorMod(surface, &red, &green, &blue) ||
        !SDL_GetSurfaceAlphaMod(surface, &alpha)) {
        return false;
    }
    float source_x = source != NULL ? (float)source->x : 0.0f;
    float source_y = source != NULL ? (float)source->y : 0.0f;
    float source_width = source != NULL ? (float)source->w : (float)asset->width;
    float source_height = source != NULL ? (float)source->h : (float)asset->height;
    gpu_map_vertex_uniforms_t vertex = {
        .destination = {destination->x, destination->y, destination->w, destination->h},
        .uv = {source_x / (float)asset->width,
               source_y / (float)asset->height,
               source_width / (float)asset->width,
               source_height / (float)asset->height},
        .viewport = {(float)target_width, (float)target_height},
    };
    gpu_map_fragment_uniforms_t fragment = {
        .modulation = {(float)red / 255.0f,
                       (float)green / 255.0f,
                       (float)blue / 255.0f,
                       (float)alpha / 255.0f},
        .owner = current_owner,
    };
    SDL_GPUTextureSamplerBinding binding = {.texture = asset->texture, .sampler = map_sampler};
    SDL_BindGPUFragmentSamplers(world_pass, 0, &binding, 1);
    SDL_PushGPUVertexUniformData(map_command_buffer, 0, &vertex, sizeof(vertex));
    SDL_PushGPUFragmentUniformData(map_command_buffer, 0, &fragment, sizeof(fragment));
    SDL_DrawGPUPrimitives(world_pass, 6, 1, 0, 0);
    world_pass_has_content = true;
    return true;
}

bool gpu_map_renderer_draw_rect(const SDL_FRect *destination,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                uint8_t alpha,
                                bool filled) {
    if (solid_surface == NULL || destination == NULL || destination->w <= 0.0f ||
        destination->h <= 0.0f) {
        return false;
    }
    if (!filled) {
        SDL_FRect edges[4] = {
            {destination->x, destination->y, destination->w, 1.0f},
            {destination->x,
             destination->y + destination->h - 1.0f,
             destination->w,
             1.0f},
            {destination->x, destination->y + 1.0f, 1.0f, destination->h - 2.0f},
            {destination->x + destination->w - 1.0f,
             destination->y + 1.0f,
             1.0f,
             destination->h - 2.0f},
        };
        for (size_t i = 0; i < SDL_arraysize(edges); i++) {
            if (edges[i].w > 0.0f && edges[i].h > 0.0f &&
                !gpu_map_renderer_draw_rect(&edges[i], red, green, blue, alpha, true)) {
                return false;
            }
        }
        return true;
    }
    Uint8 old_red, old_green, old_blue, old_alpha;
    if (!SDL_GetSurfaceColorMod(solid_surface, &old_red, &old_green, &old_blue) ||
        !SDL_GetSurfaceAlphaMod(solid_surface, &old_alpha) ||
        !SDL_SetSurfaceColorMod(solid_surface, red, green, blue) ||
        !SDL_SetSurfaceAlphaMod(solid_surface, alpha)) {
        return false;
    }
    bool drawn = gpu_map_renderer_draw_surface(solid_surface, NULL, destination);
    bool color_restored = SDL_SetSurfaceColorMod(solid_surface, old_red, old_green, old_blue);
    bool alpha_restored = SDL_SetSurfaceAlphaMod(solid_surface, old_alpha);
    return drawn && color_restored && alpha_restored;
}

bool gpu_map_renderer_end(void) {
    if (map_command_buffer == NULL) {
        return false;
    }
    gpu_map_world_pass_end();
    SDL_GPUColorTargetInfo final_info = {
        .texture = final_target,
        .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *final_pass =
        SDL_BeginGPURenderPass(map_command_buffer, &final_info, 1, NULL);
    if (final_pass == NULL) {
        gpu_map_command_cancel();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(final_pass, final_pipeline);
    SDL_GPUTextureSamplerBinding bindings[2] = {
        {.texture = albedo_target, .sampler = map_sampler},
        {.texture = owner_target, .sampler = map_sampler},
    };
    SDL_BindGPUFragmentSamplers(final_pass, 0, bindings, SDL_arraysize(bindings));
    SDL_DrawGPUPrimitives(final_pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(final_pass);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(map_command_buffer);
    map_command_buffer = NULL;
    if (fence == NULL) {
        return false;
    }
    bool completed = SDL_WaitForGPUFences(map_device, true, &fence, 1);
    SDL_ReleaseGPUFence(map_device, fence);
    return completed;
}

SDL_Texture *gpu_map_renderer_texture(void) {
    return wrapped_final_target;
}

void gpu_map_renderer_invalidate_surface(SDL_Surface *surface) {
    gpu_map_asset_t **link = &assets;
    while (*link != NULL) {
        gpu_map_asset_t *asset = *link;
        if (asset->surface == surface) {
            *link = asset->next;
            gpu_map_asset_destroy(asset);
            SDL_SetNumberProperty(SDL_GetSurfaceProperties(surface),
                                  GPU_MAP_SURFACE_GENERATION_PROPERTY,
                                  0);
            return;
        }
        link = &asset->next;
    }
}
