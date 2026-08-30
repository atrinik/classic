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

#include <gpu_shader_data.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <gpu_map_renderer.h>
#include <map.h>
#include <toolkit/socket.h>
#include <gpu_renderer.h>
#include <lighting.h>
#include <toolkit/logger.h>
#include <toolkit/memory.h>
#include <toolkit/toolkit.h>

#define GPU_MAP_SURFACE_GENERATION_PROPERTY "atrinik.gpu.map_surface_generation"
#define GPU_MAP_SURFACE_ASSET_PROPERTY "atrinik.gpu.map_surface_asset"
/* Integer target clears are backend-defined for nonzero float values. */
#define GPU_MAP_OWNER_KEY_TRANSPARENT UINT8_C(0)
#define GPU_MAP_LIGHT_KEY_BITS 19U
#define GPU_MAP_LIGHT_KEY_MASK ((UINT32_C(1) << GPU_MAP_LIGHT_KEY_BITS) - 1U)
#define GPU_MAP_LIGHT_KEY_DARK (GPU_MAP_LIGHT_KEY_MASK - 1U)
#define GPU_MAP_LIGHT_KEY_UNLIT GPU_MAP_LIGHT_KEY_MASK
#define GPU_MAP_LIGHT_QUAD_KEY_MAX (GPU_MAP_LIGHT_KEY_MASK - 2U)
#define GPU_MAP_LIGHT_KEY_PROJECTED (UINT32_C(1) << GPU_MAP_LIGHT_KEY_BITS)
#define GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY 1024U
#define GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY 128U
#define GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY 1024U
#define GPU_MAP_LIGHT_BUCKET_SIZE 64U
#define GPU_MAP_TARGET_ROW_ALIGNMENT 256U
#define GPU_MAP_TARGET_PLACEMENT_ALIGNMENT (64U * 1024U)
#define GPU_MAP_TARGET_ALLOCATOR_OVERHEAD (64U * 1024U)
#define GPU_MAP_LIGHT_FORWARD_LUT_ENTRIES 256U
#define GPU_MAP_LIGHT_INVERSE_LUT_ENTRIES 65536U
#define GPU_MAP_ATLAS_SIZE 2048U
#define GPU_MAP_ATLAS_MAX_ASSET_SIZE 256U
/* Matches the 64 uint4 WorldSlotUniforms entries in shaders/map.hlsl. */
#define GPU_MAP_WORLD_SLOT_CHUNK 256U

typedef struct gpu_map_atlas_region {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    struct gpu_map_atlas_region *next;
} gpu_map_atlas_region_t;

typedef struct gpu_map_atlas_page {
    SDL_GPUTexture *texture;
    uint32_t next_x;
    uint32_t next_y;
    uint32_t row_height;
    size_t bytes;
    size_t allocations;
    gpu_map_atlas_region_t *free_regions;
    struct gpu_map_atlas_page *next;
} gpu_map_atlas_page_t;

typedef struct gpu_map_asset {
    SDL_Surface *surface;
    SDL_GPUTexture *texture;
    Uint64 generation;
    uint32_t width;
    uint32_t height;
    uint32_t atlas_x;
    uint32_t atlas_y;
    size_t bytes;
    size_t references;
    bool accounted;
    bool standalone;
    gpu_map_atlas_page_t *atlas_page;
    struct gpu_map_asset *next;
} gpu_map_asset_t;

typedef struct gpu_map_shader_blob {
    const Uint8 *code;
    size_t size;
    SDL_GPUShaderFormat format;
    const char *entrypoint;
} gpu_map_shader_blob_t;

typedef struct gpu_map_vertex_uniforms {
    float viewport[2];
    float padding[2];
} gpu_map_vertex_uniforms_t;

typedef struct gpu_map_world_instance {
    float destination[4];
    float uv[4];
    float modulation[4];
    uint32_t owner;
    uint32_t padding[3];
} gpu_map_world_instance_t;

typedef struct gpu_map_world_command {
    gpu_map_world_instance_t instance;
    gpu_map_asset_t *asset;
    SDL_Rect clip;
    uint64_t record_identity;
    uint32_t draw_variant;
} gpu_map_world_command_t;

typedef struct gpu_map_light_quad {
    int32_t x[4];
    int32_t y[4];
    uint32_t scalar[4];
    uint32_t red[4];
    uint32_t green[4];
    uint32_t blue[4];
    uint32_t owner;
    uint32_t padding[3];
} gpu_map_light_quad_t;

typedef struct gpu_map_light_span {
    int32_t first_x;
    int32_t last_x;
    uint32_t quad;
    uint32_t padding;
} gpu_map_light_span_t;

typedef struct gpu_map_light_row {
    uint32_t upper_offset;
    uint32_t upper_count;
    int32_t upper_y;
    uint32_t upper_padding;
    uint32_t lower_offset;
    uint32_t lower_count;
    int32_t lower_y;
    uint32_t lower_padding;
    uint32_t owner;
    int32_t sample_y;
    uint32_t padding[2];
} gpu_map_light_row_t;

typedef struct gpu_map_light_horizontal_row {
    uint32_t offset;
    uint32_t count;
    uint8_t owner;
    int32_t y;
} gpu_map_light_horizontal_row_t;

/** One independently retained map output (primary world or auxiliary minimap). */
typedef struct gpu_map_target_set {
    SDL_GPUTexture *albedo;
    SDL_GPUTexture *owner;
    SDL_GPUTexture *final;
    SDL_Texture *wrapped_final;
    int width;
    int height;
    bool accounted;
    bool published;
    SDL_GPUBuffer *world_instance_buffer;
    SDL_GPUTransferBuffer *world_instance_transfer;
    size_t world_instance_capacity;
    size_t world_instance_bytes;
    bool world_instance_valid;
    gpu_map_world_command_t *world_commands;
    uint8_t *world_slot_active;
    size_t world_commands_num;
    size_t world_commands_capacity;
} gpu_map_target_set_t;

static SDL_GPUDevice *map_device;
static SDL_Renderer *map_renderer;
static SDL_GPUSampler *map_sampler;
static SDL_GPUGraphicsPipeline *world_pipeline;
static SDL_GPUGraphicsPipeline *final_pipeline;
static SDL_GPUBuffer *light_quad_buffer;
static SDL_GPUTransferBuffer *light_quad_transfer;
static size_t light_quad_gpu_capacity;
static size_t light_quad_gpu_bytes;
static SDL_GPUBuffer *light_row_buffer;
static SDL_GPUTransferBuffer *light_row_transfer;
static size_t light_row_gpu_capacity;
static size_t light_row_gpu_bytes;
static SDL_GPUBuffer *light_span_buffer;
static SDL_GPUTransferBuffer *light_span_transfer;
static size_t light_span_gpu_capacity;
static size_t light_span_gpu_bytes;
static SDL_GPUBuffer *light_forward_lut_buffer;
static SDL_GPUBuffer *light_inverse_lut_buffer;
static SDL_GPUBuffer *projected_light_row_buffer;
static SDL_GPUTransferBuffer *projected_light_row_transfer;
static size_t projected_light_row_gpu_capacity;
static size_t projected_light_row_gpu_bytes;
static SDL_GPUTexture *albedo_target;
static SDL_GPUTexture *owner_target;
static SDL_GPUTexture *final_target;
static SDL_Texture *wrapped_final_target;
static gpu_map_target_set_t map_targets[2];
static size_t active_target_index;
static SDL_GPUCommandBuffer *map_command_buffer;
static SDL_GPURenderPass *world_pass;
static SDL_Surface *solid_surface;
static gpu_map_asset_t *assets;
static gpu_map_atlas_page_t *atlas_pages;
static Uint64 next_generation = 1;
static int target_width;
static int target_height;
static uint8_t current_light_owner = GPU_RENDERER_OWNER_UNLIT;
static int current_light_sample_y;
static bool current_light_projected;
static bool world_pass_has_content;
static gpu_map_world_command_t *world_commands;
static size_t world_commands_num;
static size_t world_commands_capacity;
static uint32_t *world_command_slots;
static size_t world_command_slots_capacity;
static gpu_map_world_instance_t *pending_world_instances;
static size_t *pending_world_command_indices;
static uint8_t *pending_world_active;
static size_t pending_world_capacity;
static size_t pending_world_slots_num;
static size_t *world_slot_hash;
static size_t world_slot_hash_capacity;
static uint64_t current_record_identity;
static uint32_t current_draw_variant;
static gpu_map_light_quad_t *light_quads;
static size_t light_quads_num;
static size_t light_quads_capacity;
static gpu_map_light_row_t *light_rows;
static size_t light_rows_num;
static size_t light_rows_capacity;
static gpu_map_light_span_t *light_spans;
static size_t light_spans_num;
static size_t light_spans_capacity;
static gpu_map_light_horizontal_row_t *light_horizontal_rows;
static size_t light_horizontal_rows_num;
static size_t light_horizontal_rows_capacity;
static size_t *light_horizontal_row_lookup;
static size_t *light_row_lookup;
static size_t light_lookup_entries;
static uint32_t *projected_light_rows;
static size_t projected_light_rows_num;
static bool projected_light_rows_used;
static bool projected_light_rows_uploaded;
static uint32_t *uploaded_projected_light_rows;
static size_t uploaded_projected_light_rows_num;
static bool uploaded_projected_light_rows_valid;
static uint32_t *light_bucket_offsets;
static uint32_t *light_bucket_cursors;
static uint32_t *light_bucket_indices;
static size_t light_bucket_count;
static size_t light_bucket_indices_capacity;
static uint32_t light_bucket_columns;
static uint32_t light_bucket_rows;
static bool light_bucket_index_valid;
static gpu_map_light_quad_t *uploaded_light_quads;
static size_t uploaded_light_quads_num;
static bool uploaded_light_quads_valid;
static gpu_map_light_row_t *uploaded_light_rows;
static size_t uploaded_light_rows_num;
static gpu_map_light_span_t *uploaded_light_spans;
static size_t uploaded_light_spans_num;
static bool light_lut_resources_accounted;
static bool core_resources_accounted;
static SDL_Rect map_clip;
static bool map_clip_enabled;
static uint64_t albedo_timing_started;

static void gpu_map_world_pass_end(void);
static void gpu_map_asset_release(gpu_map_asset_t *asset);

static void gpu_map_world_commands_release(gpu_map_world_command_t *commands, size_t count) {
    for (size_t index = 0; index < count; index++) {
        gpu_map_asset_release(commands[index].asset);
    }
}

static void gpu_map_command_cancel(void) {
    gpu_map_world_pass_end();
    if (map_command_buffer != NULL) {
        SDL_CancelGPUCommandBuffer(map_command_buffer);
        map_command_buffer = NULL;
    }
    gpu_map_world_commands_release(world_commands, world_commands_num);
    world_commands_num = 0;
    current_record_identity = 0;
    current_draw_variant = 0;
}

static gpu_map_shader_blob_t gpu_map_shader_blob(const char *name, const char *entrypoint) {
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(map_device);
#define GPU_SHADER_BLOB(_name, _extension, _format, _entrypoint) \
    (gpu_map_shader_blob_t){                                     \
        .code = gpu_shader_##_name##_##_extension,               \
        .size = gpu_shader_##_name##_##_extension##_size,        \
        .format = _format,                                       \
        .entrypoint = _entrypoint,                               \
    }
#define GPU_SHADER_SELECT(_name)                                                            \
    do {                                                                                    \
        if (strcmp(name, #_name) == 0) {                                                    \
            if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {                                     \
                return GPU_SHADER_BLOB(_name, spv, SDL_GPU_SHADERFORMAT_SPIRV, entrypoint); \
            }                                                                               \
            if (formats & SDL_GPU_SHADERFORMAT_DXIL) {                                      \
                return GPU_SHADER_BLOB(_name, dxil, SDL_GPU_SHADERFORMAT_DXIL, entrypoint); \
            }                                                                               \
            if (formats & SDL_GPU_SHADERFORMAT_MSL) {                                       \
                return GPU_SHADER_BLOB(_name, msl, SDL_GPU_SHADERFORMAT_MSL, "main0");      \
            }                                                                               \
        }                                                                                   \
    } while (0)
    GPU_SHADER_SELECT(world_vertex);
    GPU_SHADER_SELECT(world_fragment);
    GPU_SHADER_SELECT(final_vertex);
    GPU_SHADER_SELECT(final_fragment);
    GPU_SHADER_SELECT(light_vertex);
    GPU_SHADER_SELECT(light_fragment);
#undef GPU_SHADER_SELECT
#undef GPU_SHADER_BLOB
    return (gpu_map_shader_blob_t){0};
}

static SDL_GPUShader *gpu_map_shader_create(const char *name,
                                            const char *entrypoint,
                                            SDL_GPUShaderStage stage,
                                            uint32_t samplers,
                                            uint32_t storage_textures,
                                            uint32_t storage_buffers,
                                            uint32_t uniforms) {
    gpu_map_shader_blob_t blob = gpu_map_shader_blob(name, entrypoint);
    if (blob.code == NULL) {
        SDL_SetError("No supported embedded format for GPU shader %s", name);
        return NULL;
    }
    SDL_GPUShaderCreateInfo info = {
        .code_size = blob.size,
        .code = blob.code,
        .entrypoint = blob.entrypoint,
        .format = blob.format,
        .stage = stage,
        .num_samplers = samplers,
        .num_storage_textures = storage_textures,
        .num_storage_buffers = storage_buffers,
        .num_uniform_buffers = uniforms,
    };
    return SDL_CreateGPUShader(map_device, &info);
}

static bool gpu_map_pipelines_create(void) {
    SDL_GPUShader *world_vertex = gpu_map_shader_create("world_vertex",
                                                        "world_vertex",
                                                        SDL_GPU_SHADERSTAGE_VERTEX,
                                                        0,
                                                        0,
                                                        1,
                                                        2);
    SDL_GPUShader *world_fragment = gpu_map_shader_create("world_fragment",
                                                          "world_fragment",
                                                          SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                          1,
                                                          0,
                                                          1,
                                                          0);
    SDL_GPUShader *final_vertex = gpu_map_shader_create("final_vertex",
                                                        "final_vertex",
                                                        SDL_GPU_SHADERSTAGE_VERTEX,
                                                        0,
                                                        0,
                                                        0,
                                                        0);
    SDL_GPUShader *final_fragment = gpu_map_shader_create("final_fragment",
                                                          "final_fragment",
                                                          SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                          1,
                                                          1,
                                                          5,
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
            .blend_state =
                {
                    .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                    .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .color_blend_op = SDL_GPU_BLENDOP_ADD,
                    .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                    .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                    .enable_blend = true,
                },
        },
        {.format = SDL_GPU_TEXTUREFORMAT_R32_UINT},
    };
    SDL_GPUGraphicsPipelineCreateInfo world_info = {
        .vertex_shader = world_vertex,
        .fragment_shader = world_fragment,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state =
            {
                .fill_mode = SDL_GPU_FILLMODE_FILL,
                .cull_mode = SDL_GPU_CULLMODE_NONE,
                .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
            },
        .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
        .target_info =
            {
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
        .rasterizer_state =
            {
                .fill_mode = SDL_GPU_FILLMODE_FILL,
                .cull_mode = SDL_GPU_CULLMODE_NONE,
                .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
            },
        .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
        .target_info =
            {
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

static bool gpu_map_buffer_upload(SDL_GPUBuffer *buffer, const void *data, uint32_t size) {
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = size,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    void *mapped = transfer != NULL ? SDL_MapGPUTransferBuffer(map_device, transfer, false) : NULL;
    if (mapped == NULL) {
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(map_device, transfer);
    SDL_GPUCommandBuffer *upload_commands = SDL_AcquireGPUCommandBuffer(map_device);
    SDL_GPUCopyPass *copy = upload_commands != NULL ? SDL_BeginGPUCopyPass(upload_commands) : NULL;
    if (copy == NULL) {
        if (upload_commands != NULL) {
            SDL_CancelGPUCommandBuffer(upload_commands);
        }
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_GPUTransferBufferLocation source = {.transfer_buffer = transfer};
    SDL_GPUBufferRegion destination = {.buffer = buffer, .size = size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(upload_commands);
    bool completed = fence != NULL && SDL_WaitForGPUFences(map_device, true, &fence, 1);
    if (fence != NULL) {
        SDL_ReleaseGPUFence(map_device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(map_device, transfer);
    return completed;
}

static bool gpu_map_light_quad_buffers_reserve(size_t required) {
    if (required <= light_quad_gpu_capacity) {
        return true;
    }
    if (required > UINT32_MAX || required > UINT32_MAX / sizeof(gpu_map_light_quad_t)) {
        SDL_SetError("GPU map compact light-grid size exceeds the backend limit");
        return false;
    }
    /* The maximum wire view can legitimately carry hundreds of thousands of
     * compact cells. A power-of-two growth step retained almost 22 MiB of
     * unused upload/storage memory for the 28x28/all-depth row and triggered
     * avoidable allocation pressure on minimum devices. Rendering builds the
     * complete CPU array before this reserve, so a 1024-record granularity
     * gives stable steady-state storage without incremental-frame churn. */
    size_t capacity = MAX(required, (size_t)GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY);
    capacity = (capacity + GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY - 1U) /
               GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY * GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY;
    uint32_t bytes = (uint32_t)(capacity * sizeof(gpu_map_light_quad_t));
    SDL_GPUBufferCreateInfo info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        .size = bytes,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(map_device, &info);
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = bytes,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        SDL_ReleaseGPUBuffer(map_device, buffer);
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_ReleaseGPUBuffer(map_device, light_quad_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_quad_transfer);
    if (light_quad_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_quad_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_quad_gpu_bytes);
    }
    light_quad_buffer = buffer;
    light_quad_transfer = transfer;
    light_quad_gpu_capacity = capacity;
    light_quad_gpu_bytes = bytes;
    gpu_renderer_statistics_resource_create(bytes);
    gpu_renderer_statistics_resource_create(bytes);
    uploaded_light_quads_valid = false;
    return true;
}

static bool gpu_map_light_row_buffers_reserve(size_t required) {
    if (required <= light_row_gpu_capacity) {
        return true;
    }
    if (required > UINT32_MAX / sizeof(gpu_map_light_row_t)) {
        SDL_SetError("GPU map compact light-row size exceeds the backend limit");
        return false;
    }
    size_t capacity = MAX(required, (size_t)GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY);
    capacity = (capacity + GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY - 1U) /
               GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY * GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY;
    uint32_t bytes = (uint32_t)(capacity * sizeof(gpu_map_light_row_t));
    SDL_GPUBufferCreateInfo info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        .size = bytes,
    };
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = bytes,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(map_device, &info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        SDL_ReleaseGPUBuffer(map_device, buffer);
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_ReleaseGPUBuffer(map_device, light_row_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_row_transfer);
    if (light_row_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_row_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_row_gpu_bytes);
    }
    light_row_buffer = buffer;
    light_row_transfer = transfer;
    light_row_gpu_capacity = capacity;
    light_row_gpu_bytes = bytes;
    gpu_renderer_statistics_resource_create(bytes);
    gpu_renderer_statistics_resource_create(bytes);
    uploaded_light_quads_valid = false;
    return true;
}

static bool gpu_map_light_span_buffers_reserve(size_t required) {
    if (required <= light_span_gpu_capacity) {
        return true;
    }
    if (required > UINT32_MAX / sizeof(gpu_map_light_span_t)) {
        SDL_SetError("GPU map compact light-span size exceeds the backend limit");
        return false;
    }
    size_t capacity = MAX(required, (size_t)GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY);
    capacity = (capacity + GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY - 1U) /
               GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY * GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY;
    uint32_t bytes = (uint32_t)(capacity * sizeof(gpu_map_light_span_t));
    SDL_GPUBufferCreateInfo info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        .size = bytes,
    };
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = bytes,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(map_device, &info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        SDL_ReleaseGPUBuffer(map_device, buffer);
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_ReleaseGPUBuffer(map_device, light_span_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_span_transfer);
    if (light_span_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_span_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_span_gpu_bytes);
    }
    light_span_buffer = buffer;
    light_span_transfer = transfer;
    light_span_gpu_capacity = capacity;
    light_span_gpu_bytes = bytes;
    gpu_renderer_statistics_resource_create(bytes);
    gpu_renderer_statistics_resource_create(bytes);
    uploaded_light_quads_valid = false;
    return true;
}

static bool gpu_map_light_buffers_create(void) {
    SDL_GPUBufferCreateInfo info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
    };

    uint32_t forward[GPU_MAP_LIGHT_FORWARD_LUT_ENTRIES];
    uint32_t inverse[GPU_MAP_LIGHT_INVERSE_LUT_ENTRIES];
    for (size_t i = 0; i < SDL_arraysize(forward); i++) {
        forward[i] = lighting_srgb8_to_linear((uint8_t)i);
    }
    for (size_t i = 0; i < SDL_arraysize(inverse); i++) {
        inverse[i] = lighting_linear_to_srgb8((uint16_t)i);
    }
    info.size = sizeof(forward);
    light_forward_lut_buffer = SDL_CreateGPUBuffer(map_device, &info);
    info.size = sizeof(inverse);
    light_inverse_lut_buffer = SDL_CreateGPUBuffer(map_device, &info);
    bool created = gpu_map_light_quad_buffers_reserve(GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY) &&
                   gpu_map_light_row_buffers_reserve(GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY) &&
                   gpu_map_light_span_buffers_reserve(GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY) &&
                   light_forward_lut_buffer != NULL && light_inverse_lut_buffer != NULL &&
                   gpu_map_buffer_upload(light_forward_lut_buffer, forward, sizeof(forward)) &&
                   gpu_map_buffer_upload(light_inverse_lut_buffer, inverse, sizeof(inverse));
    if (created) {
        gpu_renderer_statistics_resource_create(sizeof(forward));
        gpu_renderer_statistics_resource_create(sizeof(inverse));
        gpu_renderer_statistics_light_upload(sizeof(forward));
        gpu_renderer_statistics_light_upload(sizeof(inverse));
        light_lut_resources_accounted = true;
    }
    return created;
}

/** Conservative physical budget for one four-byte target allocation. */
static size_t gpu_map_target_texture_budget(int width, int height) {
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4U - (GPU_MAP_TARGET_ROW_ALIGNMENT - 1U)) {
        return SIZE_MAX;
    }
    size_t row = (size_t)width * 4U;
    row = (row + GPU_MAP_TARGET_ROW_ALIGNMENT - 1U) & ~(GPU_MAP_TARGET_ROW_ALIGNMENT - 1U);
    if (row > SIZE_MAX / (size_t)height) {
        return SIZE_MAX;
    }
    size_t allocation = row * (size_t)height;
    if (allocation >
        SIZE_MAX - (GPU_MAP_TARGET_PLACEMENT_ALIGNMENT - 1U) - GPU_MAP_TARGET_ALLOCATOR_OVERHEAD) {
        return SIZE_MAX;
    }
    allocation = (allocation + GPU_MAP_TARGET_PLACEMENT_ALIGNMENT - 1U) &
                 ~(GPU_MAP_TARGET_PLACEMENT_ALIGNMENT - 1U);
    return allocation + GPU_MAP_TARGET_ALLOCATOR_OVERHEAD;
}

static void gpu_map_target_activate(size_t index) {
    HARD_ASSERT(index < SDL_arraysize(map_targets));
    active_target_index = index;
    gpu_map_target_set_t *target = &map_targets[index];
    albedo_target = target->albedo;
    owner_target = target->owner;
    final_target = target->final;
    wrapped_final_target = target->wrapped_final;
    target_width = target->width;
    target_height = target->height;
}

static void gpu_map_target_destroy(gpu_map_target_set_t *target) {
    gpu_map_world_commands_release(target->world_commands, target->world_commands_num);
    free(target->world_commands);
    free(target->world_slot_active);
    SDL_ReleaseGPUBuffer(map_device, target->world_instance_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, target->world_instance_transfer);
    if (target->world_instance_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(target->world_instance_bytes);
        gpu_renderer_statistics_resource_destroy(target->world_instance_bytes);
    }
    if (target->wrapped_final != NULL) {
        SDL_DestroyTexture(target->wrapped_final);
    }
    if (target->accounted) {
        size_t target_budget = gpu_map_target_texture_budget(target->width, target->height);
        gpu_renderer_statistics_resource_destroy(target_budget);
        gpu_renderer_statistics_resource_destroy(target_budget);
        gpu_renderer_statistics_resource_destroy(target_budget);
    }
    SDL_ReleaseGPUTexture(map_device, target->albedo);
    SDL_ReleaseGPUTexture(map_device, target->owner);
    SDL_ReleaseGPUTexture(map_device, target->final);
    memset(target, 0, sizeof(*target));
}

static void gpu_map_targets_destroy(void) {
    for (size_t index = 0; index < SDL_arraysize(map_targets); index++) {
        gpu_map_target_destroy(&map_targets[index]);
    }
    albedo_target = NULL;
    owner_target = NULL;
    final_target = NULL;
    wrapped_final_target = NULL;
    target_width = 0;
    target_height = 0;
    active_target_index = 0;
}

static bool gpu_map_target_create(gpu_map_target_set_t *target, int width, int height) {
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_TARGET)) {
        return false;
    }
#endif
    if (target->albedo != NULL && target->width == width && target->height == height) {
        return true;
    }
    gpu_map_target_destroy(target);
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
    target->albedo = SDL_CreateGPUTexture(map_device, &info);
    target->final = SDL_CreateGPUTexture(map_device, &info);
    info.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    target->owner = SDL_CreateGPUTexture(map_device, &info);
    if (target->albedo == NULL || target->owner == NULL || target->final == NULL) {
        gpu_map_target_destroy(target);
        return false;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    bool configured =
        properties != 0 &&
        SDL_SetNumberProperty(properties,
                              SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                              SDL_PIXELFORMAT_RGBA32) &&
        SDL_SetNumberProperty(properties,
                              SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                              SDL_TEXTUREACCESS_STATIC) &&
        SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width) &&
        SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height) &&
        SDL_SetPointerProperty(properties,
                               SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,
                               target->final);
    if (configured) {
        target->wrapped_final = SDL_CreateTextureWithProperties(map_renderer, properties);
    }
    SDL_DestroyProperties(properties);
    if (!configured || target->wrapped_final == NULL ||
        !SDL_SetTextureScaleMode(target->wrapped_final, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(target->wrapped_final, SDL_BLENDMODE_BLEND)) {
        gpu_map_target_destroy(target);
        return false;
    }
    target->width = width;
    target->height = height;
    size_t target_budget = gpu_map_target_texture_budget(width, height);
    gpu_renderer_statistics_resource_create(target_budget);
    gpu_renderer_statistics_resource_create(target_budget);
    gpu_renderer_statistics_resource_create(target_budget);
    target->accounted = true;
    return true;
}

static gpu_map_atlas_page_t *gpu_map_atlas_page_create(void) {
    SDL_GPUTextureCreateInfo info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = GPU_MAP_ATLAS_SIZE,
        .height = GPU_MAP_ATLAS_SIZE,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(map_device, &info);
    if (texture == NULL) {
        return NULL;
    }
    gpu_map_atlas_page_t *page = xcalloc(1, sizeof(*page));
    page->texture = texture;
    page->bytes = (size_t)GPU_MAP_ATLAS_SIZE * GPU_MAP_ATLAS_SIZE * 4U;
    page->next = atlas_pages;
    atlas_pages = page;
    gpu_renderer_statistics_resource_create(page->bytes);
    return page;
}

static void gpu_map_atlas_regions_destroy(gpu_map_atlas_page_t *page) {
    while (page->free_regions != NULL) {
        gpu_map_atlas_region_t *region = page->free_regions;
        page->free_regions = region->next;
        free(region);
    }
}

static void gpu_map_atlas_page_destroy(gpu_map_atlas_page_t *page) {
    HARD_ASSERT(page != NULL && page->allocations == 0);
    gpu_map_atlas_regions_destroy(page);
    SDL_ReleaseGPUTexture(map_device, page->texture);
    gpu_renderer_statistics_resource_destroy(page->bytes);
    free(page);
}

static void gpu_map_atlas_page_reset(gpu_map_atlas_page_t *page) {
    HARD_ASSERT(page != NULL && page->allocations == 0);
    gpu_map_atlas_regions_destroy(page);
    page->next_x = 0;
    page->next_y = 0;
    page->row_height = 0;
}

/** Keep one pristine empty page warm and reclaim every additional empty page. */
static void gpu_map_atlas_empty_pages_trim(void) {
    bool warm_page_retained = false;
    gpu_map_atlas_page_t **link = &atlas_pages;
    while (*link != NULL) {
        gpu_map_atlas_page_t *page = *link;
        if (page->allocations != 0) {
            link = &page->next;
            continue;
        }
        if (!warm_page_retained) {
            gpu_map_atlas_page_reset(page);
            warm_page_retained = true;
            link = &page->next;
            continue;
        }
        *link = page->next;
        page->next = NULL;
        gpu_map_atlas_page_destroy(page);
    }
}

static bool gpu_map_atlas_region_take(gpu_map_atlas_page_t *page,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t *x,
                                      uint32_t *y) {
    gpu_map_atlas_region_t **link = &page->free_regions;
    while (*link != NULL) {
        gpu_map_atlas_region_t *region = *link;
        if (region->width >= width && region->height >= height) {
            *x = region->x;
            *y = region->y;
            *link = region->next;
            if (region->width > width) {
                gpu_map_atlas_region_t *right = xcalloc(1, sizeof(*right));
                right->x = region->x + width;
                right->y = region->y;
                right->width = region->width - width;
                right->height = height;
                right->next = page->free_regions;
                page->free_regions = right;
            }
            if (region->height > height) {
                gpu_map_atlas_region_t *bottom = xcalloc(1, sizeof(*bottom));
                bottom->x = region->x;
                bottom->y = region->y + height;
                bottom->width = region->width;
                bottom->height = region->height - height;
                bottom->next = page->free_regions;
                page->free_regions = bottom;
            }
            free(region);
            page->allocations++;
            return true;
        }
        link = &region->next;
    }
    if (page->next_x + width > GPU_MAP_ATLAS_SIZE) {
        page->next_x = 0;
        page->next_y += page->row_height;
        page->row_height = 0;
    }
    if (page->next_y + height > GPU_MAP_ATLAS_SIZE) {
        return false;
    }
    *x = page->next_x;
    *y = page->next_y;
    page->next_x += width;
    page->row_height = MAX(page->row_height, height);
    page->allocations++;
    return true;
}

static gpu_map_atlas_page_t *
gpu_map_atlas_allocate(uint32_t width, uint32_t height, uint32_t *x, uint32_t *y) {
    for (gpu_map_atlas_page_t *page = atlas_pages; page != NULL; page = page->next) {
        if (gpu_map_atlas_region_take(page, width, height, x, y)) {
            return page;
        }
    }
    gpu_map_atlas_page_t *page = gpu_map_atlas_page_create();
    if (page != NULL && gpu_map_atlas_region_take(page, width, height, x, y)) {
        return page;
    }
    gpu_map_atlas_empty_pages_trim();
    return NULL;
}

static void gpu_map_atlas_region_release(gpu_map_asset_t *asset) {
    HARD_ASSERT(asset->atlas_page != NULL && asset->atlas_page->allocations != 0);
    gpu_map_atlas_page_t *page = asset->atlas_page;
    gpu_map_atlas_region_t *region = xcalloc(1, sizeof(*region));
    region->x = asset->atlas_x;
    region->y = asset->atlas_y;
    region->width = asset->width;
    region->height = asset->height;
    bool merged;
    do {
        merged = false;
        for (gpu_map_atlas_region_t **link = &page->free_regions; *link != NULL;
             link = &(*link)->next) {
            gpu_map_atlas_region_t *other = *link;
            if (region->y == other->y && region->height == other->height &&
                (region->x + region->width == other->x || other->x + other->width == region->x)) {
                uint32_t left = MIN(region->x, other->x);
                uint32_t right = MAX(region->x + region->width, other->x + other->width);
                region->x = left;
                region->width = right - left;
            } else if (region->x == other->x && region->width == other->width &&
                       (region->y + region->height == other->y ||
                        other->y + other->height == region->y)) {
                uint32_t top = MIN(region->y, other->y);
                uint32_t bottom = MAX(region->y + region->height, other->y + other->height);
                region->y = top;
                region->height = bottom - top;
            } else {
                continue;
            }
            *link = other->next;
            free(other);
            merged = true;
            break;
        }
    } while (merged);
    region->next = page->free_regions;
    page->free_regions = region;
    page->allocations--;
    gpu_map_atlas_empty_pages_trim();
}

static void gpu_map_atlas_pages_destroy(void) {
    while (atlas_pages != NULL) {
        gpu_map_atlas_page_t *page = atlas_pages;
        atlas_pages = page->next;
        HARD_ASSERT(page->allocations == 0);
        page->next = NULL;
        gpu_map_atlas_page_destroy(page);
    }
}

static void gpu_map_asset_destroy(gpu_map_asset_t *asset) {
    HARD_ASSERT(asset->references == 0);
    if (asset->standalone && asset->texture != NULL) {
        SDL_ReleaseGPUTexture(map_device, asset->texture);
    } else if (asset->atlas_page != NULL) {
        gpu_map_atlas_region_release(asset);
    }
    if (asset->accounted) {
        gpu_renderer_statistics_resource_destroy(asset->bytes);
    }
    free(asset);
}

static void gpu_map_asset_retain(gpu_map_asset_t *asset) {
    HARD_ASSERT(asset != NULL && asset->references != SIZE_MAX);
    asset->references++;
}

static void gpu_map_asset_release(gpu_map_asset_t *asset) {
    if (asset == NULL) {
        return;
    }
    HARD_ASSERT(asset->references != 0);
    asset->references--;
    if (asset->references == 0) {
        gpu_map_asset_destroy(asset);
    }
}

static void SDLCALL gpu_map_asset_cleanup(void *userdata, void *value) {
    (void)userdata;
    gpu_map_asset_t *asset = value;
    gpu_map_asset_t **link = &assets;
    while (*link != NULL && *link != asset) {
        link = &(*link)->next;
    }
    if (*link == asset) {
        *link = asset->next;
    }
    asset->surface = NULL;
    asset->next = NULL;
    gpu_map_asset_release(asset);
}

static void gpu_map_assets_destroy(void) {
    while (assets != NULL) {
        gpu_map_asset_t *asset = assets;
        SDL_PropertiesID properties = SDL_GetSurfaceProperties(asset->surface);
        if (SDL_GetPointerProperty(properties, GPU_MAP_SURFACE_ASSET_PROPERTY, NULL) == asset) {
            SDL_ClearProperty(properties, GPU_MAP_SURFACE_ASSET_PROPERTY);
        } else {
            assets = asset->next;
            asset->surface = NULL;
            asset->next = NULL;
            gpu_map_asset_release(asset);
        }
    }
}

static bool gpu_map_projected_light_rows_reserve(size_t required) {
    if (required <= projected_light_row_gpu_capacity) {
        return true;
    }
    if (required > UINT32_MAX / sizeof(*projected_light_rows)) {
        SDL_SetError("GPU map projected-light lookup exceeds the backend limit");
        return false;
    }
    size_t capacity = MAX(required, (size_t)MAP2_LEVELS * 256U);
    uint32_t bytes = (uint32_t)(capacity * sizeof(*projected_light_rows));
    SDL_GPUBufferCreateInfo buffer_info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        .size = bytes,
    };
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = bytes,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(map_device, &buffer_info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        SDL_ReleaseGPUBuffer(map_device, buffer);
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_ReleaseGPUBuffer(map_device, projected_light_row_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, projected_light_row_transfer);
    if (projected_light_row_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(projected_light_row_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(projected_light_row_gpu_bytes);
    }
    projected_light_row_buffer = buffer;
    projected_light_row_transfer = transfer;
    projected_light_row_gpu_capacity = capacity;
    projected_light_row_gpu_bytes = bytes;
    uploaded_projected_light_rows_valid = false;
    gpu_renderer_statistics_resource_create(bytes);
    gpu_renderer_statistics_resource_create(bytes);
    return true;
}

static bool gpu_map_projected_light_rows_upload(void) {
    size_t required = MAX(projected_light_rows_num, (size_t)1);
    if (!gpu_map_projected_light_rows_reserve(required)) {
        return false;
    }
    if (!projected_light_rows_used) {
        return true;
    }
    bool full_upload = !uploaded_projected_light_rows_valid ||
                       projected_light_rows_num != uploaded_projected_light_rows_num;
    if (!full_upload && memcmp(projected_light_rows,
                               uploaded_projected_light_rows,
                               projected_light_rows_num * sizeof(*projected_light_rows)) == 0) {
        return true;
    }
    uint32_t *mapped = SDL_MapGPUTransferBuffer(map_device, projected_light_row_transfer, true);
    if (mapped == NULL) {
        return false;
    }
    size_t uploaded_bytes = 0;
    for (size_t first = 0; first < projected_light_rows_num;) {
        if (!full_upload && projected_light_rows[first] == uploaded_projected_light_rows[first]) {
            first++;
            continue;
        }
        size_t end = first + 1U;
        while (end < projected_light_rows_num &&
               (full_upload || projected_light_rows[end] != uploaded_projected_light_rows[end])) {
            end++;
        }
        size_t bytes = (end - first) * sizeof(*projected_light_rows);
        memcpy(&mapped[first], &projected_light_rows[first], bytes);
        uploaded_bytes += bytes;
        first = end;
    }
    SDL_UnmapGPUTransferBuffer(map_device, projected_light_row_transfer);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(map_command_buffer);
    if (copy == NULL) {
        return false;
    }
    for (size_t first = 0; first < projected_light_rows_num;) {
        if (!full_upload && projected_light_rows[first] == uploaded_projected_light_rows[first]) {
            first++;
            continue;
        }
        size_t end = first + 1U;
        while (end < projected_light_rows_num &&
               (full_upload || projected_light_rows[end] != uploaded_projected_light_rows[end])) {
            end++;
        }
        uint32_t offset = (uint32_t)(first * sizeof(*projected_light_rows));
        uint32_t bytes = (uint32_t)((end - first) * sizeof(*projected_light_rows));
        SDL_GPUTransferBufferLocation source = {
            .transfer_buffer = projected_light_row_transfer,
            .offset = offset,
        };
        SDL_GPUBufferRegion destination = {
            .buffer = projected_light_row_buffer,
            .offset = offset,
            .size = bytes,
        };
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        first = end;
    }
    SDL_EndGPUCopyPass(copy);
    gpu_renderer_statistics_projected_light_upload(uploaded_bytes);
    projected_light_rows_uploaded = true;
    return true;
}

static bool gpu_map_world_instance_buffers_reserve(gpu_map_target_set_t *target, size_t required) {
    if (required <= target->world_instance_capacity) {
        return true;
    }
    if (required > UINT32_MAX / sizeof(gpu_map_world_instance_t)) {
        SDL_SetError("GPU map painter instance stream exceeds the backend limit");
        return false;
    }
    size_t capacity = MAX(required, (size_t)GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY);
    capacity = (capacity + GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY - 1U) /
               GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY * GPU_MAP_LIGHT_QUAD_INITIAL_CAPACITY;
    uint32_t bytes = (uint32_t)(capacity * sizeof(gpu_map_world_instance_t));
    SDL_GPUBufferCreateInfo buffer_info = {
        .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
        .size = bytes,
    };
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = bytes,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(map_device, &buffer_info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    if (buffer == NULL || transfer == NULL) {
        SDL_ReleaseGPUBuffer(map_device, buffer);
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_ReleaseGPUBuffer(map_device, target->world_instance_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, target->world_instance_transfer);
    if (target->world_instance_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(target->world_instance_bytes);
        gpu_renderer_statistics_resource_destroy(target->world_instance_bytes);
    }
    target->world_instance_buffer = buffer;
    target->world_instance_transfer = transfer;
    target->world_instance_capacity = capacity;
    target->world_instance_bytes = bytes;
    target->world_instance_valid = false;
    gpu_renderer_statistics_resource_create(bytes);
    gpu_renderer_statistics_resource_create(bytes);
    return true;
}

static uint64_t gpu_map_world_command_hash(const gpu_map_world_command_t *command) {
    uint64_t value = command->record_identity ^ ((uint64_t)command->draw_variant << 32U);
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static void gpu_map_world_host_slots_reserve(gpu_map_target_set_t *target, size_t required) {
    if (required <= target->world_commands_capacity) {
        return;
    }
    size_t old_capacity = target->world_commands_capacity;
    size_t capacity = MAX(required, old_capacity == 0 ? 1024U : old_capacity * 2U);
    target->world_commands =
        xreallocarray(target->world_commands, capacity, sizeof(*target->world_commands));
    target->world_slot_active =
        xreallocarray(target->world_slot_active, capacity, sizeof(*target->world_slot_active));
    memset(target->world_commands + old_capacity,
           0,
           (capacity - old_capacity) * sizeof(*target->world_commands));
    memset(target->world_slot_active + old_capacity,
           0,
           (capacity - old_capacity) * sizeof(*target->world_slot_active));
    target->world_commands_capacity = capacity;
}

static void gpu_map_world_pending_reserve(size_t required) {
    if (required > world_command_slots_capacity) {
        world_command_slots_capacity = MAX(required, world_command_slots_capacity * 2U);
        world_command_slots = xreallocarray(world_command_slots,
                                            world_command_slots_capacity,
                                            sizeof(*world_command_slots));
    }
    if (required > pending_world_capacity) {
        size_t old_capacity = pending_world_capacity;
        pending_world_capacity = MAX(required, pending_world_capacity * 2U);
        pending_world_instances = xreallocarray(pending_world_instances,
                                                pending_world_capacity,
                                                sizeof(*pending_world_instances));
        pending_world_command_indices = xreallocarray(pending_world_command_indices,
                                                      pending_world_capacity,
                                                      sizeof(*pending_world_command_indices));
        pending_world_active = xreallocarray(pending_world_active,
                                             pending_world_capacity,
                                             sizeof(*pending_world_active));
        memset(pending_world_instances + old_capacity,
               0,
               (pending_world_capacity - old_capacity) * sizeof(*pending_world_instances));
    }
}

static bool gpu_map_world_command_key_equal(const gpu_map_world_command_t *left,
                                            const gpu_map_world_command_t *right) {
    return left->record_identity == right->record_identity &&
           left->draw_variant == right->draw_variant;
}

static size_t gpu_map_world_slot_lookup(const gpu_map_target_set_t *target,
                                        const gpu_map_world_command_t *command) {
    if (world_slot_hash_capacity == 0) {
        return SIZE_MAX;
    }
    size_t mask = world_slot_hash_capacity - 1U;
    size_t bucket = (size_t)gpu_map_world_command_hash(command) & mask;
    while (world_slot_hash[bucket] != SIZE_MAX) {
        size_t slot = world_slot_hash[bucket];
        if (gpu_map_world_command_key_equal(&target->world_commands[slot], command)) {
            return slot;
        }
        bucket = (bucket + 1U) & mask;
    }
    return SIZE_MAX;
}

static void gpu_map_world_slot_hash_build(const gpu_map_target_set_t *target) {
    size_t required = 16U;
    while (required < target->world_commands_num * 2U) {
        required *= 2U;
    }
    if (required > world_slot_hash_capacity) {
        world_slot_hash_capacity = required;
        world_slot_hash =
            xreallocarray(world_slot_hash, world_slot_hash_capacity, sizeof(*world_slot_hash));
    }
    for (size_t index = 0; index < world_slot_hash_capacity; index++) {
        world_slot_hash[index] = SIZE_MAX;
    }
    size_t mask = world_slot_hash_capacity - 1U;
    for (size_t slot = 0; slot < target->world_commands_num; slot++) {
        if (!target->world_slot_active[slot]) {
            continue;
        }
        size_t bucket = (size_t)gpu_map_world_command_hash(&target->world_commands[slot]) & mask;
        while (world_slot_hash[bucket] != SIZE_MAX) {
            bucket = (bucket + 1U) & mask;
        }
        world_slot_hash[bucket] = slot;
    }
}

static bool gpu_map_world_slots_prepare(gpu_map_target_set_t *target) {
    if (world_commands_num > SIZE_MAX - target->world_commands_num) {
        SDL_SetError("GPU map painter slot count exceeds the backend limit");
        return false;
    }
    size_t maximum = target->world_commands_num + world_commands_num;
    gpu_map_world_pending_reserve(MAX(maximum, (size_t)1));
    gpu_map_world_host_slots_reserve(target, maximum);
    pending_world_slots_num = target->world_commands_num;
    memset(pending_world_active, 0, maximum * sizeof(*pending_world_active));
    memset(pending_world_instances, 0, maximum * sizeof(*pending_world_instances));
    for (size_t slot = 0; slot < maximum; slot++) {
        pending_world_command_indices[slot] = SIZE_MAX;
    }
    gpu_map_world_slot_hash_build(target);

    for (size_t index = 0; index < world_commands_num; index++) {
        size_t slot = gpu_map_world_slot_lookup(target, &world_commands[index]);
        if (slot != SIZE_MAX && pending_world_active[slot]) {
            slot = SIZE_MAX;
        }
        world_command_slots[index] = slot == SIZE_MAX ? UINT32_MAX : (uint32_t)slot;
        if (slot != SIZE_MAX) {
            pending_world_active[slot] = 1;
        }
    }
    for (size_t index = 0; index < world_commands_num; index++) {
        if (world_command_slots[index] != UINT32_MAX) {
            continue;
        }
        size_t slot = 0;
        while (slot < pending_world_slots_num && pending_world_active[slot]) {
            slot++;
        }
        if (slot == pending_world_slots_num) {
            pending_world_slots_num++;
        }
        world_command_slots[index] = (uint32_t)slot;
        pending_world_active[slot] = 1;
    }
    for (size_t index = 0; index < world_commands_num; index++) {
        size_t slot = world_command_slots[index];
        pending_world_instances[slot] = world_commands[index].instance;
        pending_world_command_indices[slot] = index;
    }
    return true;
}

static bool gpu_map_world_slot_changed(const gpu_map_target_set_t *target, size_t slot) {
    if (!target->world_instance_valid || slot >= target->world_commands_num ||
        pending_world_active[slot] != target->world_slot_active[slot]) {
        return true;
    }
    return pending_world_active[slot] && memcmp(&pending_world_instances[slot],
                                                &target->world_commands[slot].instance,
                                                sizeof(*pending_world_instances)) != 0;
}

static void gpu_map_world_commands_commit(void) {
    gpu_map_target_set_t *target = &map_targets[active_target_index];
    gpu_map_world_host_slots_reserve(target, pending_world_slots_num);
    for (size_t slot = 0; slot < target->world_commands_num; slot++) {
        if (target->world_slot_active[slot]) {
            gpu_map_asset_release(target->world_commands[slot].asset);
            target->world_commands[slot].asset = NULL;
        }
    }
    for (size_t slot = 0; slot < pending_world_slots_num; slot++) {
        size_t index = pending_world_command_indices[slot];
        if (index != SIZE_MAX) {
            target->world_commands[slot] = world_commands[index];
        } else {
            memset(&target->world_commands[slot], 0, sizeof(target->world_commands[slot]));
        }
        target->world_slot_active[slot] = pending_world_active[slot];
    }
    target->world_commands_num = pending_world_slots_num;
    target->world_instance_valid = true;
    /* The retained slots now own the asset references accumulated while
     * building this frame. */
    world_commands_num = 0;
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
            .clear_color = {(float)GPU_MAP_OWNER_KEY_TRANSPARENT, 0.0f, 0.0f, 0.0f},
            .load_op = world_pass_has_content ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE,
        },
    };
    world_pass = SDL_BeginGPURenderPass(map_command_buffer, targets, SDL_arraysize(targets), NULL);
    if (world_pass == NULL) {
        return false;
    }
    SDL_BindGPUGraphicsPipeline(world_pass, world_pipeline);
    SDL_Rect scissor = map_clip_enabled ? map_clip : (SDL_Rect){0, 0, target_width, target_height};
    SDL_SetGPUScissor(world_pass, &scissor);
    return true;
}

static bool gpu_map_world_commands_submit(void) {
    gpu_map_target_set_t *target = &map_targets[active_target_index];
    if (!gpu_map_world_slots_prepare(target)) {
        return false;
    }
    if (!gpu_map_world_instance_buffers_reserve(target, MAX(pending_world_slots_num, (size_t)1))) {
        return false;
    }
    bool changed = false;
    for (size_t slot = 0; slot < pending_world_slots_num && !changed; slot++) {
        changed = gpu_map_world_slot_changed(target, slot);
    }
    if (changed) {
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
        if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_UPLOAD)) {
            return false;
        }
#endif
        gpu_map_world_instance_t *mapped =
            SDL_MapGPUTransferBuffer(map_device, target->world_instance_transfer, true);
        if (mapped == NULL) {
            return false;
        }
        if (!target->world_instance_valid) {
            memset(mapped, 0, target->world_instance_bytes);
        }
        for (size_t slot = 0; slot < pending_world_slots_num; slot++) {
            if (gpu_map_world_slot_changed(target, slot)) {
                mapped[slot] = pending_world_instances[slot];
            }
        }
        SDL_UnmapGPUTransferBuffer(map_device, target->world_instance_transfer);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(map_command_buffer);
        if (copy == NULL) {
            return false;
        }
        for (size_t first = 0; first < pending_world_slots_num;) {
            if (target->world_instance_valid && !gpu_map_world_slot_changed(target, first)) {
                first++;
                continue;
            }
            size_t end = first + 1U;
            while (end < pending_world_slots_num &&
                   (!target->world_instance_valid || gpu_map_world_slot_changed(target, end))) {
                end++;
            }
            uint32_t offset = (uint32_t)(first * sizeof(gpu_map_world_instance_t));
            uint32_t bytes = (uint32_t)((end - first) * sizeof(gpu_map_world_instance_t));
            SDL_GPUTransferBufferLocation source = {
                .transfer_buffer = target->world_instance_transfer,
                .offset = offset,
            };
            SDL_GPUBufferRegion destination = {
                .buffer = target->world_instance_buffer,
                .offset = offset,
                .size = bytes,
            };
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            gpu_renderer_statistics_instance_upload(bytes);
            first = end;
        }
        SDL_EndGPUCopyPass(copy);
    }

    if (!gpu_map_projected_light_rows_upload() || !gpu_map_world_pass_begin()) {
        return false;
    }
    gpu_map_vertex_uniforms_t vertex = {
        .viewport = {(float)target_width, (float)target_height},
    };
    SDL_PushGPUVertexUniformData(map_command_buffer, 0, &vertex, sizeof(vertex));
    SDL_BindGPUVertexStorageBuffers(world_pass, 0, &target->world_instance_buffer, 1);
    SDL_BindGPUFragmentStorageBuffers(world_pass, 0, &projected_light_row_buffer, 1);

    uint64_t batches = 0;
    for (size_t first = 0; first < world_commands_num;) {
        size_t end = first + 1U;
        while (end < world_commands_num &&
               world_commands[end].asset->texture == world_commands[first].asset->texture &&
               memcmp(&world_commands[end].clip,
                      &world_commands[first].clip,
                      sizeof(world_commands[first].clip)) == 0) {
            end++;
        }
        SDL_GPUTextureSamplerBinding binding = {
            .texture = world_commands[first].asset->texture,
            .sampler = map_sampler,
        };
        SDL_SetGPUScissor(world_pass, &world_commands[first].clip);
        SDL_BindGPUFragmentSamplers(world_pass, 0, &binding, 1);
        for (size_t chunk = first; chunk < end;) {
            size_t chunk_end = MIN(end, chunk + GPU_MAP_WORLD_SLOT_CHUNK);
            uint32_t slots[GPU_MAP_WORLD_SLOT_CHUNK] = {0};
            for (size_t index = chunk; index < chunk_end; index++) {
                slots[index - chunk] = world_command_slots[index];
            }
            size_t slot_count = chunk_end - chunk;
            size_t slot_bytes = (slot_count + 3U) / 4U * sizeof(uint32_t) * 4U;
            SDL_PushGPUVertexUniformData(map_command_buffer, 1, slots, (uint32_t)slot_bytes);
            gpu_renderer_statistics_slot_uniform_upload(slot_bytes);
            SDL_DrawGPUPrimitives(world_pass, 6, (uint32_t)slot_count, 0, 0);
            batches++;
            chunk = chunk_end;
        }
        first = end;
    }
    gpu_renderer_statistics_commands(world_commands_num, batches, batches);
    world_pass_has_content = true;
    return true;
}

/** Return whether one retained compact-light buffer contains changed records. */
static bool gpu_map_light_buffer_changed(const void *current,
                                         size_t current_count,
                                         const void *uploaded,
                                         size_t uploaded_count,
                                         size_t record_size) {
    if (!uploaded_light_quads_valid || current_count != uploaded_count) {
        return true;
    }
    return current_count != 0 && memcmp(current, uploaded, current_count * record_size) != 0;
}

/** Upload only contiguous runs whose compact-light records changed. */
static bool gpu_map_light_buffer_upload_delta(SDL_GPUBuffer *buffer,
                                              SDL_GPUTransferBuffer *transfer,
                                              const void *current,
                                              size_t current_count,
                                              const void *uploaded,
                                              size_t uploaded_count,
                                              size_t record_size) {
    if (current_count == 0) {
        return true;
    }
    uint8_t *mapped = SDL_MapGPUTransferBuffer(map_device, transfer, true);
    if (mapped == NULL) {
        return false;
    }

    bool full_upload = !uploaded_light_quads_valid;
    const uint8_t *current_bytes = current;
    const uint8_t *uploaded_bytes = uploaded;
    for (size_t index = 0; index < current_count; index++) {
        bool changed = full_upload || index >= uploaded_count ||
                       memcmp(current_bytes + index * record_size,
                              uploaded_bytes + index * record_size,
                              record_size) != 0;
        if (changed) {
            memcpy(mapped + index * record_size, current_bytes + index * record_size, record_size);
        }
    }
    SDL_UnmapGPUTransferBuffer(map_device, transfer);

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(map_command_buffer);
    if (copy == NULL) {
        return false;
    }
    for (size_t first = 0; first < current_count;) {
        bool changed = full_upload || first >= uploaded_count ||
                       memcmp(current_bytes + first * record_size,
                              uploaded_bytes + first * record_size,
                              record_size) != 0;
        if (!changed) {
            first++;
            continue;
        }
        size_t end = first + 1U;
        while (end < current_count && (full_upload || end >= uploaded_count ||
                                       memcmp(current_bytes + end * record_size,
                                              uploaded_bytes + end * record_size,
                                              record_size) != 0)) {
            end++;
        }
        size_t offset_bytes = first * record_size;
        size_t upload_bytes = (end - first) * record_size;
        HARD_ASSERT(offset_bytes <= UINT32_MAX && upload_bytes <= UINT32_MAX);
        SDL_GPUTransferBufferLocation source = {
            .transfer_buffer = transfer,
            .offset = (uint32_t)offset_bytes,
        };
        SDL_GPUBufferRegion destination = {
            .buffer = buffer,
            .offset = (uint32_t)offset_bytes,
            .size = (uint32_t)upload_bytes,
        };
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        gpu_renderer_statistics_light_upload(upload_bytes);
        first = end;
    }
    SDL_EndGPUCopyPass(copy);
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
        !SDL_GetSurfaceAlphaMod(surface, &alpha) || !SDL_GetSurfaceBlendMode(surface, &blend)) {
        SDL_DestroySurface(upload);
        return NULL;
    }
    bool color_cleared = SDL_SetSurfaceColorMod(surface, 255, 255, 255);
    bool alpha_cleared = color_cleared && SDL_SetSurfaceAlphaMod(surface, 255);
    bool blend_cleared = alpha_cleared && SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    if (!blend_cleared) {
        if (alpha_cleared) {
            SDL_SetSurfaceAlphaMod(surface, alpha);
        }
        if (color_cleared) {
            SDL_SetSurfaceColorMod(surface, red, green, blue);
        }
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

    gpu_map_asset_t *asset = xcalloc(1, sizeof(*asset));
    asset->surface = surface;
    asset->width = (uint32_t)surface->w;
    asset->height = (uint32_t)surface->h;
    asset->bytes = (size_t)surface->w * (size_t)surface->h * 4U;
    asset->standalone =
        asset->width > GPU_MAP_ATLAS_MAX_ASSET_SIZE || asset->height > GPU_MAP_ATLAS_MAX_ASSET_SIZE;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    bool allocation_fault =
        gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_ALLOCATION);
#else
    bool allocation_fault = false;
#endif
    if (asset->standalone) {
        SDL_GPUTextureCreateInfo texture_info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = asset->width,
            .height = asset->height,
            .layer_count_or_depth = 1,
            .num_levels = 1,
            .sample_count = SDL_GPU_SAMPLECOUNT_1,
        };
        asset->texture = allocation_fault ? NULL : SDL_CreateGPUTexture(map_device, &texture_info);
    } else if (!allocation_fault) {
        asset->atlas_page =
            gpu_map_atlas_allocate(asset->width, asset->height, &asset->atlas_x, &asset->atlas_y);
        asset->texture = asset->atlas_page != NULL ? asset->atlas_page->texture : NULL;
    }
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = (uint32_t)transfer_size,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    Uint8 *destination =
        transfer != NULL ? SDL_MapGPUTransferBuffer(map_device, transfer, false) : NULL;
    if (asset->texture == NULL || transfer == NULL || destination == NULL) {
        if (destination != NULL) {
            SDL_UnmapGPUTransferBuffer(map_device, transfer);
        }
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        gpu_map_asset_destroy(asset);
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
        gpu_map_asset_destroy(asset);
        return NULL;
    }
    SDL_GPUTextureTransferInfo source = {
        .transfer_buffer = transfer,
        .pixels_per_row = aligned_row_bytes / 4U,
        .rows_per_layer = (uint32_t)surface->h,
    };
    SDL_GPUTextureRegion destination_region = {
        .texture = asset->texture,
        .x = asset->atlas_x,
        .y = asset->atlas_y,
        .w = (uint32_t)surface->w,
        .h = (uint32_t)surface->h,
        .d = 1,
    };
    SDL_UploadToGPUTexture(copy, &source, &destination_region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(map_device, transfer);

    asset->references = 1;
    gpu_renderer_statistics_source_upload(asset->bytes);
    if (asset->standalone) {
        gpu_renderer_statistics_resource_create(asset->bytes);
        asset->accounted = true;
    }
    asset->generation = next_generation++;
    if (next_generation == 0) {
        next_generation = 1;
    }
    asset->next = assets;
    assets = asset;
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    if (!SDL_SetNumberProperty(properties,
                               GPU_MAP_SURFACE_GENERATION_PROPERTY,
                               (Sint64)asset->generation)) {
        assets = asset->next;
        asset->next = NULL;
        gpu_map_asset_release(asset);
        return NULL;
    }
    if (!SDL_SetPointerPropertyWithCleanup(properties,
                                           GPU_MAP_SURFACE_ASSET_PROPERTY,
                                           asset,
                                           gpu_map_asset_cleanup,
                                           NULL)) {
        /* SDL invokes the cleanup callback for a pointer value it could not store. */
        SDL_SetNumberProperty(properties, GPU_MAP_SURFACE_GENERATION_PROPERTY, 0);
        return NULL;
    }
    return asset;
}

static gpu_map_asset_t *gpu_map_asset(SDL_Surface *surface) {
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    Uint64 generation = SDL_GetNumberProperty(properties, GPU_MAP_SURFACE_GENERATION_PROPERTY, 0);
    gpu_map_asset_t *asset =
        SDL_GetPointerProperty(properties, GPU_MAP_SURFACE_ASSET_PROPERTY, NULL);
    if (asset != NULL && generation != 0 && asset->generation == generation) {
        return asset;
    }
    if (asset != NULL) {
        SDL_ClearProperty(properties, GPU_MAP_SURFACE_ASSET_PROPERTY);
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
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    bool pipeline_fault =
        gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_SHADER);
#else
    bool pipeline_fault = false;
#endif
    if (solid_surface == NULL || !SDL_FillSurfaceRect(solid_surface, NULL, UINT32_MAX) ||
        map_sampler == NULL || pipeline_fault || !gpu_map_pipelines_create() ||
        !gpu_map_light_buffers_create()) {
        gpu_map_renderer_destroy();
        return false;
    }
    gpu_renderer_statistics_resource_create(0);
    gpu_renderer_statistics_resource_create(0);
    gpu_renderer_statistics_resource_create(0);
    core_resources_accounted = true;
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
    gpu_map_world_commands_release(world_commands, world_commands_num);
    world_commands_num = 0;
    gpu_map_targets_destroy();
    gpu_map_assets_destroy();
    gpu_map_atlas_pages_destroy();
    free(world_commands);
    world_commands = NULL;
    world_commands_num = 0;
    world_commands_capacity = 0;
    free(world_command_slots);
    world_command_slots = NULL;
    world_command_slots_capacity = 0;
    free(pending_world_instances);
    pending_world_instances = NULL;
    free(pending_world_command_indices);
    pending_world_command_indices = NULL;
    free(pending_world_active);
    pending_world_active = NULL;
    pending_world_capacity = 0;
    pending_world_slots_num = 0;
    free(world_slot_hash);
    world_slot_hash = NULL;
    world_slot_hash_capacity = 0;
    free(light_quads);
    light_quads = NULL;
    light_quads_num = 0;
    light_quads_capacity = 0;
    free(light_rows);
    light_rows = NULL;
    light_rows_num = 0;
    light_rows_capacity = 0;
    free(light_spans);
    light_spans = NULL;
    light_spans_num = 0;
    light_spans_capacity = 0;
    free(light_horizontal_rows);
    light_horizontal_rows = NULL;
    light_horizontal_rows_num = 0;
    light_horizontal_rows_capacity = 0;
    free(light_horizontal_row_lookup);
    light_horizontal_row_lookup = NULL;
    free(light_row_lookup);
    light_row_lookup = NULL;
    free(projected_light_rows);
    projected_light_rows = NULL;
    projected_light_rows_num = 0;
    projected_light_rows_used = false;
    projected_light_rows_uploaded = false;
    free(uploaded_projected_light_rows);
    uploaded_projected_light_rows = NULL;
    uploaded_projected_light_rows_num = 0;
    uploaded_projected_light_rows_valid = false;
    light_lookup_entries = 0;
    free(light_bucket_offsets);
    light_bucket_offsets = NULL;
    free(light_bucket_cursors);
    light_bucket_cursors = NULL;
    free(light_bucket_indices);
    light_bucket_indices = NULL;
    light_bucket_count = 0;
    light_bucket_indices_capacity = 0;
    light_bucket_columns = 0;
    light_bucket_rows = 0;
    light_bucket_index_valid = false;
    free(uploaded_light_quads);
    uploaded_light_quads = NULL;
    uploaded_light_quads_num = 0;
    uploaded_light_quads_valid = false;
    free(uploaded_light_rows);
    uploaded_light_rows = NULL;
    uploaded_light_rows_num = 0;
    free(uploaded_light_spans);
    uploaded_light_spans = NULL;
    uploaded_light_spans_num = 0;
    SDL_DestroySurface(solid_surface);
    solid_surface = NULL;
    SDL_ReleaseGPUGraphicsPipeline(map_device, world_pipeline);
    SDL_ReleaseGPUGraphicsPipeline(map_device, final_pipeline);
    SDL_ReleaseGPUBuffer(map_device, light_quad_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_quad_transfer);
    SDL_ReleaseGPUBuffer(map_device, light_row_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_row_transfer);
    SDL_ReleaseGPUBuffer(map_device, light_span_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, light_span_transfer);
    SDL_ReleaseGPUBuffer(map_device, light_forward_lut_buffer);
    SDL_ReleaseGPUBuffer(map_device, light_inverse_lut_buffer);
    SDL_ReleaseGPUBuffer(map_device, projected_light_row_buffer);
    SDL_ReleaseGPUTransferBuffer(map_device, projected_light_row_transfer);
    SDL_ReleaseGPUSampler(map_device, map_sampler);
    if (light_quad_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_quad_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_quad_gpu_bytes);
        light_quad_gpu_bytes = 0;
        light_quad_gpu_capacity = 0;
    }
    if (light_row_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_row_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_row_gpu_bytes);
        light_row_gpu_bytes = 0;
        light_row_gpu_capacity = 0;
    }
    if (light_span_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(light_span_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(light_span_gpu_bytes);
        light_span_gpu_bytes = 0;
        light_span_gpu_capacity = 0;
    }
    if (projected_light_row_gpu_bytes != 0) {
        gpu_renderer_statistics_resource_destroy(projected_light_row_gpu_bytes);
        gpu_renderer_statistics_resource_destroy(projected_light_row_gpu_bytes);
        projected_light_row_gpu_bytes = 0;
        projected_light_row_gpu_capacity = 0;
    }
    if (light_lut_resources_accounted) {
        gpu_renderer_statistics_resource_destroy(GPU_MAP_LIGHT_FORWARD_LUT_ENTRIES *
                                                 sizeof(uint32_t));
        gpu_renderer_statistics_resource_destroy(GPU_MAP_LIGHT_INVERSE_LUT_ENTRIES *
                                                 sizeof(uint32_t));
        light_lut_resources_accounted = false;
    }
    if (core_resources_accounted) {
        gpu_renderer_statistics_resource_destroy(0);
        gpu_renderer_statistics_resource_destroy(0);
        gpu_renderer_statistics_resource_destroy(0);
        core_resources_accounted = false;
    }
    world_pipeline = NULL;
    final_pipeline = NULL;
    light_quad_buffer = NULL;
    light_quad_transfer = NULL;
    light_row_buffer = NULL;
    light_row_transfer = NULL;
    light_span_buffer = NULL;
    light_span_transfer = NULL;
    light_forward_lut_buffer = NULL;
    light_inverse_lut_buffer = NULL;
    projected_light_row_buffer = NULL;
    projected_light_row_transfer = NULL;
    map_sampler = NULL;
    map_device = NULL;
    map_renderer = NULL;
}

bool gpu_map_renderer_begin(int width, int height, bool auxiliary) {
    size_t target_index = auxiliary ? 1U : 0U;
    if (map_device == NULL || width <= 0 || height <= 0 || !SDL_FlushRenderer(map_renderer) ||
        !gpu_map_target_create(&map_targets[target_index], width, height)) {
        return false;
    }
    gpu_map_target_activate(target_index);
    if ((size_t)target_height > SIZE_MAX / MAP2_LEVELS) {
        SDL_SetError("GPU map light-row lookup exceeds the backend limit");
        return false;
    }
    size_t lookup_entries = (size_t)target_height * MAP2_LEVELS;
    if (lookup_entries != light_lookup_entries) {
        light_horizontal_row_lookup =
            xreallocarray(light_horizontal_row_lookup, lookup_entries, sizeof(size_t));
        light_row_lookup = xreallocarray(light_row_lookup, lookup_entries, sizeof(size_t));
        projected_light_rows =
            xreallocarray(projected_light_rows, lookup_entries, sizeof(*projected_light_rows));
        light_lookup_entries = lookup_entries;
    }
    memset(light_horizontal_row_lookup, 0xff, lookup_entries * sizeof(size_t));
    memset(light_row_lookup, 0xff, lookup_entries * sizeof(size_t));
    for (size_t index = 0; index < lookup_entries; index++) {
        projected_light_rows[index] = GPU_MAP_LIGHT_KEY_UNLIT;
    }
    projected_light_rows_num = lookup_entries;
    projected_light_rows_used = false;
    projected_light_rows_uploaded = false;
    map_command_buffer = SDL_AcquireGPUCommandBuffer(map_device);
    world_pass = NULL;
    world_pass_has_content = false;
    gpu_map_world_commands_release(world_commands, world_commands_num);
    world_commands_num = 0;
    current_record_identity = 0;
    current_draw_variant = 0;
    current_light_owner = GPU_RENDERER_OWNER_UNLIT;
    current_light_sample_y = 0;
    current_light_projected = false;
    light_quads_num = 0;
    light_rows_num = 0;
    light_spans_num = 0;
    light_horizontal_rows_num = 0;
    light_bucket_index_valid = false;
    map_clip_enabled = false;
    albedo_timing_started = gpu_renderer_timing_begin();
    return map_command_buffer != NULL;
}

bool gpu_map_renderer_active(void) {
    return map_command_buffer != NULL;
}

void gpu_map_renderer_set_owner(uint8_t owner, int sample_y, bool projected) {
    HARD_ASSERT(owner < MAP2_LEVELS || owner == GPU_RENDERER_OWNER_UNLIT);
    current_light_owner = owner;
    current_light_sample_y = sample_y;
    current_light_projected = projected && owner != GPU_RENDERER_OWNER_UNLIT;
}

void gpu_map_renderer_set_instance_identity(uint64_t record_identity, uint32_t draw_variant) {
    current_record_identity = record_identity;
    current_draw_variant = draw_variant;
}

void gpu_map_renderer_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]) {
    HARD_ASSERT(vertices != NULL);
    if (map_command_buffer == NULL) {
        return;
    }
    if (light_quads_num >= GPU_MAP_LIGHT_QUAD_KEY_MAX) {
        SDL_SetError("GPU map compact light-grid exceeds the packed owner key");
        gpu_map_command_cancel();
        return;
    }
    if (light_quads_num == light_quads_capacity) {
        light_quads_capacity = light_quads_capacity == 0 ? 1024 : light_quads_capacity * 2;
        light_quads = xreallocarray(light_quads, light_quads_capacity, sizeof(*light_quads));
    }
    gpu_map_light_quad_t *quad = &light_quads[light_quads_num++];
    for (size_t i = 0; i < 4; i++) {
        quad->x[i] = vertices[i].x;
        quad->y[i] = vertices[i].y;
        quad->scalar[i] = vertices[i].scalar;
        quad->red[i] = vertices[i].red;
        quad->green[i] = vertices[i].green;
        quad->blue[i] = vertices[i].blue;
    }
    quad->owner = owner;
    memset(quad->padding, 0, sizeof(quad->padding));
    light_bucket_index_valid = false;
}

static int64_t
gpu_map_light_edge(const gpu_map_light_quad_t *quad, size_t a, size_t b, int x, int y) {
    int64_t point_x_twice = (int64_t)x * 2 + 1;
    int64_t point_y_twice = (int64_t)y * 2 + 1;
    int64_t a_x_twice = (int64_t)quad->x[a] * 2;
    int64_t a_y_twice = (int64_t)quad->y[a] * 2;
    return (point_x_twice - a_x_twice) * ((int64_t)quad->y[b] - quad->y[a]) -
           (point_y_twice - a_y_twice) * ((int64_t)quad->x[b] - quad->x[a]);
}

static bool gpu_map_light_triangle_contains(const gpu_map_light_quad_t *quad,
                                            size_t a,
                                            size_t b,
                                            size_t c,
                                            int x,
                                            int y) {
    int64_t area =
        ((int64_t)quad->x[c] * 2 - (int64_t)quad->x[a] * 2) * ((int64_t)quad->y[b] - quad->y[a]) -
        ((int64_t)quad->y[c] * 2 - (int64_t)quad->y[a] * 2) * ((int64_t)quad->x[b] - quad->x[a]);
    if (area == 0) {
        return false;
    }
    int64_t orientation = area < 0 ? -1 : 1;
    int64_t weight_b = orientation * gpu_map_light_edge(quad, c, a, x, y);
    int64_t weight_c = orientation * gpu_map_light_edge(quad, a, b, x, y);
    return weight_b >= 0 && weight_c >= 0 && weight_b + weight_c <= llabs(area);
}

static bool gpu_map_light_quad_contains(const gpu_map_light_quad_t *quad, int x, int y) {
    return gpu_map_light_triangle_contains(quad, 0, 1, 2, x, y) ||
           gpu_map_light_triangle_contains(quad, 0, 2, 3, x, y);
}

static bool gpu_map_light_bucket_index_build(void) {
    light_bucket_columns =
        ((uint32_t)target_width + GPU_MAP_LIGHT_BUCKET_SIZE - 1U) / GPU_MAP_LIGHT_BUCKET_SIZE;
    light_bucket_rows =
        ((uint32_t)target_height + GPU_MAP_LIGHT_BUCKET_SIZE - 1U) / GPU_MAP_LIGHT_BUCKET_SIZE;
    size_t spatial_buckets = (size_t)light_bucket_columns * light_bucket_rows;
    if (spatial_buckets > (UINT32_MAX - 1U) / MAP2_LEVELS) {
        SDL_SetError("GPU map compact light-grid bucket count exceeds the backend limit");
        return false;
    }
    size_t required_buckets = spatial_buckets * MAP2_LEVELS;
    if (required_buckets != light_bucket_count) {
        light_bucket_offsets = xreallocarray(light_bucket_offsets,
                                             required_buckets + 1U,
                                             sizeof(*light_bucket_offsets));
        light_bucket_cursors =
            xreallocarray(light_bucket_cursors, required_buckets, sizeof(*light_bucket_cursors));
        light_bucket_count = required_buckets;
    }
    memset(light_bucket_offsets, 0, (light_bucket_count + 1U) * sizeof(*light_bucket_offsets));

    for (size_t index = 0; index < light_quads_num; index++) {
        const gpu_map_light_quad_t *quad = &light_quads[index];
        int min_x = quad->x[0], max_x = quad->x[0], min_y = quad->y[0], max_y = quad->y[0];
        for (size_t corner = 1; corner < 4; corner++) {
            min_x = MIN(min_x, quad->x[corner]);
            max_x = MAX(max_x, quad->x[corner]);
            min_y = MIN(min_y, quad->y[corner]);
            max_y = MAX(max_y, quad->y[corner]);
        }
        min_x = MAX(0, min_x);
        min_y = MAX(0, min_y);
        max_x = MIN(target_width - 1, max_x);
        max_y = MIN(target_height - 1, max_y);
        if (min_x > max_x || min_y > max_y) {
            continue;
        }
        uint32_t first_x = (uint32_t)min_x / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t last_x = (uint32_t)max_x / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t first_y = (uint32_t)min_y / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t last_y = (uint32_t)max_y / GPU_MAP_LIGHT_BUCKET_SIZE;
        for (uint32_t bucket_y = first_y; bucket_y <= last_y; bucket_y++) {
            for (uint32_t bucket_x = first_x; bucket_x <= last_x; bucket_x++) {
                size_t bucket = (size_t)quad->owner * spatial_buckets +
                                (size_t)bucket_y * light_bucket_columns + bucket_x;
                light_bucket_offsets[bucket + 1U]++;
            }
        }
    }
    for (size_t bucket = 1; bucket <= light_bucket_count; bucket++) {
        if (UINT32_MAX - light_bucket_offsets[bucket - 1U] < light_bucket_offsets[bucket]) {
            SDL_SetError("GPU map compact light-grid bucket entries exceed the backend limit");
            return false;
        }
        light_bucket_offsets[bucket] += light_bucket_offsets[bucket - 1U];
    }
    size_t required_indices = light_bucket_offsets[light_bucket_count];
    if (required_indices > light_bucket_indices_capacity) {
        light_bucket_indices =
            xreallocarray(light_bucket_indices, required_indices, sizeof(*light_bucket_indices));
        light_bucket_indices_capacity = required_indices;
    }
    memcpy(light_bucket_cursors,
           light_bucket_offsets,
           light_bucket_count * sizeof(*light_bucket_cursors));
    for (size_t index = 0; index < light_quads_num; index++) {
        const gpu_map_light_quad_t *quad = &light_quads[index];
        int min_x = quad->x[0], max_x = quad->x[0], min_y = quad->y[0], max_y = quad->y[0];
        for (size_t corner = 1; corner < 4; corner++) {
            min_x = MIN(min_x, quad->x[corner]);
            max_x = MAX(max_x, quad->x[corner]);
            min_y = MIN(min_y, quad->y[corner]);
            max_y = MAX(max_y, quad->y[corner]);
        }
        min_x = MAX(0, min_x);
        min_y = MAX(0, min_y);
        max_x = MIN(target_width - 1, max_x);
        max_y = MIN(target_height - 1, max_y);
        if (min_x > max_x || min_y > max_y) {
            continue;
        }
        uint32_t first_x = (uint32_t)min_x / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t last_x = (uint32_t)max_x / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t first_y = (uint32_t)min_y / GPU_MAP_LIGHT_BUCKET_SIZE;
        uint32_t last_y = (uint32_t)max_y / GPU_MAP_LIGHT_BUCKET_SIZE;
        for (uint32_t bucket_y = first_y; bucket_y <= last_y; bucket_y++) {
            for (uint32_t bucket_x = first_x; bucket_x <= last_x; bucket_x++) {
                size_t bucket = (size_t)quad->owner * spatial_buckets +
                                (size_t)bucket_y * light_bucket_columns + bucket_x;
                light_bucket_indices[light_bucket_cursors[bucket]++] = (uint32_t)index;
            }
        }
    }
    light_bucket_index_valid = true;
    return true;
}

static size_t gpu_map_light_quad_find_exact(uint8_t owner, int x, int y) {
    if (!light_bucket_index_valid && !gpu_map_light_bucket_index_build()) {
        return SIZE_MAX;
    }
    if (x < 0 || x >= target_width || y < 0 || y >= target_height) {
        return SIZE_MAX;
    }
    size_t spatial_buckets = (size_t)light_bucket_columns * light_bucket_rows;
    size_t bucket = (size_t)owner * spatial_buckets +
                    (size_t)((uint32_t)y / GPU_MAP_LIGHT_BUCKET_SIZE) * light_bucket_columns +
                    (uint32_t)x / GPU_MAP_LIGHT_BUCKET_SIZE;
    for (uint32_t entry = light_bucket_offsets[bucket + 1U];
         entry > light_bucket_offsets[bucket];) {
        size_t index = light_bucket_indices[--entry];
        if (light_quads[index].owner == owner &&
            gpu_map_light_quad_contains(&light_quads[index], x, y)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool gpu_map_light_span_append(int first_x, int last_x, size_t quad) {
    if (light_spans_num >= UINT32_MAX || quad > UINT32_MAX) {
        SDL_SetError("GPU map compact light spans exceed the backend limit");
        return false;
    }
    if (light_spans_num == light_spans_capacity) {
        light_spans_capacity = light_spans_capacity == 0 ? GPU_MAP_LIGHT_SPAN_INITIAL_CAPACITY
                                                         : light_spans_capacity * 2U;
        light_spans = xreallocarray(light_spans, light_spans_capacity, sizeof(*light_spans));
    }
    light_spans[light_spans_num++] = (gpu_map_light_span_t){
        .first_x = first_x,
        .last_x = last_x,
        .quad = (uint32_t)quad,
    };
    return true;
}

static const gpu_map_light_horizontal_row_t *gpu_map_light_horizontal_row_find(uint8_t owner,
                                                                               int y) {
    if (owner >= MAP2_LEVELS || y < 0 || y >= target_height) {
        return NULL;
    }
    size_t index = light_horizontal_row_lookup[(size_t)owner * target_height + (size_t)y];
    return index == SIZE_MAX ? NULL : &light_horizontal_rows[index];
}

static const gpu_map_light_horizontal_row_t *gpu_map_light_horizontal_row_build(uint8_t owner,
                                                                                int y) {
    const gpu_map_light_horizontal_row_t *existing = gpu_map_light_horizontal_row_find(owner, y);
    if (existing != NULL) {
        return existing;
    }
    if (light_horizontal_rows_num == light_horizontal_rows_capacity) {
        light_horizontal_rows_capacity = light_horizontal_rows_capacity == 0
                                             ? GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY
                                             : light_horizontal_rows_capacity * 2U;
        light_horizontal_rows = xreallocarray(light_horizontal_rows,
                                              light_horizontal_rows_capacity,
                                              sizeof(*light_horizontal_rows));
    }
    if (!light_bucket_index_valid && !gpu_map_light_bucket_index_build()) {
        return NULL;
    }
    uint32_t offset = (uint32_t)light_spans_num;
    size_t active_quad = SIZE_MAX;
    int active_first = 0;
    for (int x = 0; x < target_width; x++) {
        size_t quad = gpu_map_light_quad_find_exact(owner, x, y);
        if (quad == active_quad) {
            continue;
        }
        if (active_quad != SIZE_MAX &&
            !gpu_map_light_span_append(active_first, x - 1, active_quad)) {
            return NULL;
        }
        active_quad = quad;
        active_first = x;
    }
    if (active_quad != SIZE_MAX &&
        !gpu_map_light_span_append(active_first, target_width - 1, active_quad)) {
        return NULL;
    }
    size_t count = light_spans_num - offset;
    if (count > UINT32_MAX) {
        SDL_SetError("GPU map compact light row exceeds the backend limit");
        return NULL;
    }
    size_t row_index = light_horizontal_rows_num++;
    gpu_map_light_horizontal_row_t *row = &light_horizontal_rows[row_index];
    *row = (gpu_map_light_horizontal_row_t){
        .offset = offset,
        .count = (uint32_t)count,
        .owner = owner,
        .y = y,
    };
    light_horizontal_row_lookup[(size_t)owner * target_height + (size_t)y] = row_index;
    return row;
}

static size_t gpu_map_light_row_find(uint8_t owner, int sample_y) {
    return light_row_lookup[(size_t)owner * target_height + (size_t)sample_y];
}

static size_t gpu_map_light_row_build(uint8_t owner, int sample_y) {
    sample_y = MAX(0, MIN(target_height - 1, sample_y));
    size_t existing = gpu_map_light_row_find(owner, sample_y);
    if (existing != SIZE_MAX) {
        return existing;
    }
    const gpu_map_light_horizontal_row_t *requested =
        gpu_map_light_horizontal_row_build(owner, sample_y);
    if (requested == NULL) {
        return SIZE_MAX;
    }
    gpu_map_light_horizontal_row_t upper = *requested;
    gpu_map_light_horizontal_row_t lower = *requested;
    bool upper_found = requested->count != 0;
    bool lower_found = requested->count != 0;
    if (!upper_found) {
        for (int distance = 1; distance < target_height && (!upper_found || !lower_found);
             distance++) {
            if (!upper_found && sample_y - distance >= 0) {
                const gpu_map_light_horizontal_row_t *candidate =
                    gpu_map_light_horizontal_row_build(owner, sample_y - distance);
                if (candidate == NULL) {
                    return SIZE_MAX;
                }
                if (candidate->count != 0) {
                    upper = *candidate;
                    upper_found = true;
                }
            }
            if (!lower_found && sample_y + distance < target_height) {
                const gpu_map_light_horizontal_row_t *candidate =
                    gpu_map_light_horizontal_row_build(owner, sample_y + distance);
                if (candidate == NULL) {
                    return SIZE_MAX;
                }
                if (candidate->count != 0) {
                    lower = *candidate;
                    lower_found = true;
                }
            }
        }
        if (!upper_found && lower_found) {
            upper = lower;
            upper_found = true;
        }
        if (!lower_found && upper_found) {
            lower = upper;
            lower_found = true;
        }
        if (!upper_found || !lower_found) {
            light_row_lookup[(size_t)owner * target_height + (size_t)sample_y] =
                GPU_MAP_LIGHT_KEY_UNLIT;
            return GPU_MAP_LIGHT_KEY_UNLIT;
        }
    }
    if (light_rows_num >= GPU_MAP_LIGHT_QUAD_KEY_MAX) {
        SDL_SetError("GPU map compact light rows exceed the packed owner key");
        return SIZE_MAX;
    }
    if (light_rows_num == light_rows_capacity) {
        light_rows_capacity = light_rows_capacity == 0 ? GPU_MAP_LIGHT_ROW_INITIAL_CAPACITY
                                                       : light_rows_capacity * 2U;
        light_rows = xreallocarray(light_rows, light_rows_capacity, sizeof(*light_rows));
    }
    gpu_map_light_row_t *row = &light_rows[light_rows_num];
    *row = (gpu_map_light_row_t){
        .upper_offset = upper.offset,
        .upper_count = upper.count,
        .upper_y = upper.y,
        .lower_offset = lower.offset,
        .lower_count = lower.count,
        .lower_y = lower.y,
        .owner = owner,
        .sample_y = sample_y,
    };
    light_row_lookup[(size_t)owner * target_height + (size_t)sample_y] = light_rows_num;
    return light_rows_num++;
}

bool gpu_map_renderer_draw_surface(SDL_Surface *surface,
                                   const SDL_Rect *source,
                                   const SDL_FRect *destination) {
    if (map_command_buffer == NULL || surface == NULL || destination == NULL ||
        destination->w <= 0.0f || destination->h <= 0.0f) {
        return false;
    }
    gpu_map_asset_t *asset = gpu_map_asset(surface);
    if (asset == NULL) {
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
    float texture_width = asset->standalone ? (float)asset->width : (float)GPU_MAP_ATLAS_SIZE;
    float texture_height = asset->standalone ? (float)asset->height : (float)GPU_MAP_ATLAS_SIZE;
    gpu_map_world_instance_t instance = {
        .destination = {destination->x, destination->y, destination->w, destination->h},
        .uv = {((float)asset->atlas_x + source_x) / texture_width,
               ((float)asset->atlas_y + source_y) / texture_height,
               source_width / texture_width,
               source_height / texture_height},
        .modulation = {(float)red / 255.0f,
                       (float)green / 255.0f,
                       (float)blue / 255.0f,
                       (float)alpha / 255.0f},
    };

    SDL_Rect base_clip =
        map_clip_enabled ? map_clip : (SDL_Rect){0, 0, target_width, target_height};
    if (base_clip.w <= 0 || base_clip.h <= 0) {
        return true;
    }
    instance.owner = GPU_MAP_LIGHT_KEY_UNLIT;
    if (current_light_owner != GPU_RENDERER_OWNER_UNLIT) {
        if (current_light_projected) {
            int first_y = MAX(0, (int)floorf(destination->y));
            int last_y = MIN(target_height, (int)ceilf(destination->y + destination->h));
            for (int y = first_y; y < last_y; y++) {
                size_t row = gpu_map_light_row_build(current_light_owner, y);
                if (row == SIZE_MAX) {
                    return false;
                }
                projected_light_rows[(size_t)y * MAP2_LEVELS + current_light_owner] =
                    row == GPU_MAP_LIGHT_KEY_UNLIT ? GPU_MAP_LIGHT_KEY_UNLIT : (uint32_t)(row + 1U);
            }
            projected_light_rows_used = true;
            instance.owner = GPU_MAP_LIGHT_KEY_PROJECTED | current_light_owner;
        } else {
            int sample_y = MAX(0, MIN(target_height - 1, current_light_sample_y));
            size_t row = gpu_map_light_row_build(current_light_owner, sample_y);
            if (row == SIZE_MAX) {
                return false;
            }
            instance.owner =
                row == GPU_MAP_LIGHT_KEY_UNLIT ? GPU_MAP_LIGHT_KEY_UNLIT : (uint32_t)(row + 1U);
        }
    }
    if (world_commands_num == world_commands_capacity) {
        world_commands_capacity =
            world_commands_capacity == 0 ? 1024U : world_commands_capacity * 2U;
        world_commands =
            xreallocarray(world_commands, world_commands_capacity, sizeof(*world_commands));
    }
    uint32_t draw_variant =
        current_record_identity != 0 ? current_draw_variant : (uint32_t)world_commands_num;
    gpu_map_asset_retain(asset);
    world_commands[world_commands_num] = (gpu_map_world_command_t){
        .instance = instance,
        .asset = asset,
        .clip = base_clip,
        .record_identity = current_record_identity,
        .draw_variant = draw_variant,
    };
    world_commands_num++;
    current_record_identity = 0;
    current_draw_variant = 0;
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
            {destination->x, destination->y + destination->h - 1.0f, destination->w, 1.0f},
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

bool gpu_map_renderer_set_clip(const SDL_Rect *rectangle) {
    if (map_command_buffer == NULL) {
        return false;
    }
    map_clip_enabled = rectangle != NULL;
    map_clip = rectangle != NULL ? *rectangle : (SDL_Rect){0, 0, target_width, target_height};
    if (world_pass != NULL) {
        SDL_SetGPUScissor(world_pass, &map_clip);
    }
    return true;
}

bool gpu_map_renderer_end(void) {
    if (map_command_buffer == NULL) {
        gpu_map_command_cancel();
        return false;
    }
    if (!gpu_map_world_commands_submit()) {
        gpu_map_command_cancel();
        return false;
    }
    /* An empty frame still has to clear both retained world targets before
     * final composition, otherwise loading/FOW frames expose old pixels. */
    if (world_pass == NULL && !gpu_map_world_pass_begin()) {
        gpu_map_command_cancel();
        return false;
    }
    gpu_map_world_pass_end();
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_ALBEDO_OWNER, albedo_timing_started);
    uint64_t light_timing_started = gpu_renderer_timing_begin();
    size_t light_bytes = light_quads_num * sizeof(*light_quads);
    size_t row_bytes = light_rows_num * sizeof(*light_rows);
    size_t span_bytes = light_spans_num * sizeof(*light_spans);
    bool auxiliary_target = active_target_index != 0;
    if (auxiliary_target && (light_bytes != 0 || row_bytes != 0 || span_bytes != 0)) {
        SDL_SetError("auxiliary GPU map target unexpectedly submitted a light grid");
        gpu_map_command_cancel();
        return false;
    }
    if (!gpu_map_light_quad_buffers_reserve(light_quads_num) ||
        !gpu_map_light_row_buffers_reserve(light_rows_num) ||
        !gpu_map_light_span_buffers_reserve(light_spans_num)) {
        gpu_map_command_cancel();
        return false;
    }
    /* Growing any compact buffer invalidates the shared committed snapshot,
     * so decide the deltas only after all reserves have completed. */
    bool quad_changed = !auxiliary_target && gpu_map_light_buffer_changed(light_quads,
                                                                          light_quads_num,
                                                                          uploaded_light_quads,
                                                                          uploaded_light_quads_num,
                                                                          sizeof(*light_quads));
    bool row_changed = !auxiliary_target && gpu_map_light_buffer_changed(light_rows,
                                                                         light_rows_num,
                                                                         uploaded_light_rows,
                                                                         uploaded_light_rows_num,
                                                                         sizeof(*light_rows));
    bool span_changed = !auxiliary_target && gpu_map_light_buffer_changed(light_spans,
                                                                          light_spans_num,
                                                                          uploaded_light_spans,
                                                                          uploaded_light_spans_num,
                                                                          sizeof(*light_spans));
    bool light_changed = quad_changed || row_changed || span_changed;
    if (light_changed && (light_bytes != 0 || row_bytes != 0 || span_bytes != 0)) {
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
        if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_UPLOAD)) {
            gpu_map_command_cancel();
            return false;
        }
#endif
        if ((quad_changed && !gpu_map_light_buffer_upload_delta(light_quad_buffer,
                                                                light_quad_transfer,
                                                                light_quads,
                                                                light_quads_num,
                                                                uploaded_light_quads,
                                                                uploaded_light_quads_num,
                                                                sizeof(*light_quads))) ||
            (row_changed && !gpu_map_light_buffer_upload_delta(light_row_buffer,
                                                               light_row_transfer,
                                                               light_rows,
                                                               light_rows_num,
                                                               uploaded_light_rows,
                                                               uploaded_light_rows_num,
                                                               sizeof(*light_rows))) ||
            (span_changed && !gpu_map_light_buffer_upload_delta(light_span_buffer,
                                                                light_span_transfer,
                                                                light_spans,
                                                                light_spans_num,
                                                                uploaded_light_spans,
                                                                uploaded_light_spans_num,
                                                                sizeof(*light_spans)))) {
            gpu_map_command_cancel();
            return false;
        }
    }
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
    SDL_GPUViewport final_viewport = {
        .w = (float)target_width,
        .h = (float)target_height,
        .max_depth = 1.0f,
    };
    SDL_SetGPUViewport(final_pass, &final_viewport);
    SDL_Rect final_scissor = {0, 0, target_width, target_height};
    SDL_SetGPUScissor(final_pass, &final_scissor);
    SDL_GPUTextureSamplerBinding albedo_binding = {
        .texture = albedo_target,
        .sampler = map_sampler,
    };
    SDL_BindGPUFragmentSamplers(final_pass, 0, &albedo_binding, 1);
    SDL_GPUTexture *integer_textures[] = {owner_target};
    SDL_BindGPUFragmentStorageTextures(final_pass, 0, integer_textures, 1);
    SDL_GPUBuffer *tone_buffers[] = {
        light_quad_buffer,
        light_row_buffer,
        light_span_buffer,
        light_forward_lut_buffer,
        light_inverse_lut_buffer,
    };
    SDL_BindGPUFragmentStorageBuffers(final_pass, 0, tone_buffers, SDL_arraysize(tone_buffers));
    SDL_DrawGPUPrimitives(final_pass, 6, 1, 0, 0);
    gpu_renderer_statistics_commands(1, 1, 1);
    SDL_EndGPURenderPass(final_pass);

    gpu_renderer_timing_end(GPU_RENDERER_TIMING_LIGHT_TONE, light_timing_started);
    uint64_t submission_started = gpu_renderer_timing_begin();
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_SUBMISSION)) {
        gpu_map_command_cancel();
        return false;
    }
#endif
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(map_command_buffer);
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_SUBMISSION, submission_started);
    map_command_buffer = NULL;
    if (fence == NULL) {
        gpu_map_command_cancel();
        return false;
    }
    uint64_t completion_started = gpu_renderer_timing_begin();
    bool completed = SDL_WaitForGPUFences(map_device, true, &fence, 1);
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMPLETION, completion_started);
    SDL_ReleaseGPUFence(map_device, fence);
    if (completed && light_changed) {
        if (light_quads_num != 0) {
            uploaded_light_quads =
                xreallocarray(uploaded_light_quads, light_quads_num, sizeof(*uploaded_light_quads));
            memcpy(uploaded_light_quads, light_quads, light_bytes);
        }
        uploaded_light_quads_num = light_quads_num;
        if (light_rows_num != 0) {
            uploaded_light_rows =
                xreallocarray(uploaded_light_rows, light_rows_num, sizeof(*uploaded_light_rows));
            memcpy(uploaded_light_rows, light_rows, row_bytes);
        }
        uploaded_light_rows_num = light_rows_num;
        if (light_spans_num != 0) {
            uploaded_light_spans =
                xreallocarray(uploaded_light_spans, light_spans_num, sizeof(*uploaded_light_spans));
            memcpy(uploaded_light_spans, light_spans, span_bytes);
        }
        uploaded_light_spans_num = light_spans_num;
        uploaded_light_quads_valid = true;
    }
    if (completed && projected_light_rows_uploaded) {
        uploaded_projected_light_rows = xreallocarray(uploaded_projected_light_rows,
                                                      projected_light_rows_num,
                                                      sizeof(*uploaded_projected_light_rows));
        memcpy(uploaded_projected_light_rows,
               projected_light_rows,
               projected_light_rows_num * sizeof(*projected_light_rows));
        uploaded_projected_light_rows_num = projected_light_rows_num;
        uploaded_projected_light_rows_valid = true;
    }
    if (completed) {
        gpu_map_world_commands_commit();
        map_targets[active_target_index].published = true;
    } else {
        gpu_map_command_cancel();
    }
    return completed;
}

SDL_Texture *gpu_map_renderer_texture(bool auxiliary) {
    const gpu_map_target_set_t *target = &map_targets[auxiliary ? 1U : 0U];
    return target->published ? target->wrapped_final : NULL;
}

void gpu_map_renderer_invalidate_target(bool auxiliary) {
    map_targets[auxiliary ? 1U : 0U].published = false;
}

#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
size_t gpu_map_renderer_target_payload_bytes(int width, int height, uint8_t active_depths) {
    if (width <= 0 || height <= 0 || active_depths == 0 || active_depths > MAP2_LEVELS ||
        (size_t)width > SIZE_MAX / (size_t)height / 12U) {
        return SIZE_MAX;
    }
    return (size_t)width * (size_t)height * 12U;
}

size_t gpu_map_renderer_target_retained_bytes(int width, int height, uint8_t active_depths) {
    if (active_depths == 0 || active_depths > MAP2_LEVELS) {
        return SIZE_MAX;
    }
    size_t per_target = gpu_map_target_texture_budget(width, height);
    return per_target > SIZE_MAX / 3U ? SIZE_MAX : per_target * 3U;
}

size_t gpu_map_renderer_atlas_page_count(void) {
    size_t count = 0;
    for (gpu_map_atlas_page_t *page = atlas_pages; page != NULL; page = page->next) {
        HARD_ASSERT(count != SIZE_MAX);
        count++;
    }
    return count;
}

size_t gpu_map_renderer_atlas_allocation_count(void) {
    size_t count = 0;
    for (gpu_map_atlas_page_t *page = atlas_pages; page != NULL; page = page->next) {
        HARD_ASSERT(count <= SIZE_MAX - page->allocations);
        count += page->allocations;
    }
    return count;
}

size_t gpu_map_renderer_lit_instance_count(bool auxiliary) {
    const gpu_map_target_set_t *target = &map_targets[auxiliary ? 1U : 0U];
    size_t count = 0;
    for (size_t slot = 0; slot < target->world_commands_num; slot++) {
        if (target->world_slot_active[slot] &&
            target->world_commands[slot].instance.owner != GPU_MAP_LIGHT_KEY_UNLIT) {
            count++;
        }
    }
    return count;
}

static bool gpu_map_renderer_download_pixel(SDL_GPUTexture *texture,
                                            uint32_t layer,
                                            int x,
                                            int y,
                                            size_t bytes_per_pixel,
                                            void *pixel) {
    SDL_GPUTransferBufferCreateInfo transfer_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = 256,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(map_device, &transfer_info);
    SDL_GPUCommandBuffer *commands =
        transfer != NULL ? SDL_AcquireGPUCommandBuffer(map_device) : NULL;
    SDL_GPUCopyPass *copy = commands != NULL ? SDL_BeginGPUCopyPass(commands) : NULL;
    if (copy == NULL) {
        if (commands != NULL) {
            SDL_CancelGPUCommandBuffer(commands);
        }
        SDL_ReleaseGPUTransferBuffer(map_device, transfer);
        return false;
    }
    SDL_GPUTextureRegion source = {
        .texture = texture,
        .layer = layer,
        .x = (uint32_t)x,
        .y = (uint32_t)y,
        .w = 1,
        .h = 1,
        .d = 1,
    };
    SDL_GPUTextureTransferInfo destination = {
        .transfer_buffer = transfer,
        .pixels_per_row = 256U / (uint32_t)bytes_per_pixel,
        .rows_per_layer = 1,
    };
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    bool completed = fence != NULL && SDL_WaitForGPUFences(map_device, true, &fence, 1);
    void *mapped = completed ? SDL_MapGPUTransferBuffer(map_device, transfer, false) : NULL;
    if (mapped != NULL) {
        memcpy(pixel, mapped, bytes_per_pixel);
        SDL_UnmapGPUTransferBuffer(map_device, transfer);
    }
    if (fence != NULL) {
        SDL_ReleaseGPUFence(map_device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(map_device, transfer);
    return completed && mapped != NULL;
}

static bool gpu_map_renderer_probe_quad(size_t index, int x, int y, uint16_t light[4]) {
    if (!uploaded_light_quads_valid || index >= uploaded_light_quads_num) {
        return false;
    }
    const gpu_map_light_quad_t *quad = &uploaded_light_quads[index];
    const size_t triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (size_t attempt = 0; attempt < 2; attempt++) {
        for (size_t half = 0; half < SDL_arraysize(triangles); half++) {
            size_t a = triangles[half][0];
            size_t b = triangles[half][1];
            size_t c = triangles[half][2];
            int64_t area = ((int64_t)quad->x[c] * 2 - (int64_t)quad->x[a] * 2) *
                               ((int64_t)quad->y[b] - quad->y[a]) -
                           ((int64_t)quad->y[c] * 2 - (int64_t)quad->y[a] * 2) *
                               ((int64_t)quad->x[b] - quad->x[a]);
            if (area == 0) {
                continue;
            }
            int64_t orientation = area < 0 ? -1 : 1;
            int64_t weight_b = orientation * gpu_map_light_edge(quad, c, a, x, y);
            int64_t weight_c = orientation * gpu_map_light_edge(quad, a, b, x, y);
            uint64_t scale = (uint64_t)llabs(area);
            if (weight_b < 0 || weight_c < 0 || (uint64_t)(weight_b + weight_c) > scale) {
                continue;
            }
            uint64_t u = half == 0 ? (uint64_t)(weight_b + weight_c) : (uint64_t)weight_b;
            uint64_t v = half == 0 ? (uint64_t)weight_c : (uint64_t)(weight_b + weight_c);
            const uint32_t *channels[4] = {quad->scalar, quad->red, quad->green, quad->blue};
            for (size_t channel = 0; channel < 4; channel++) {
                light[channel] = lighting_bilinear_channel((uint16_t)channels[channel][0],
                                                           (uint16_t)channels[channel][1],
                                                           (uint16_t)channels[channel][2],
                                                           (uint16_t)channels[channel][3],
                                                           u,
                                                           v,
                                                           scale);
            }
            return true;
        }
        int min_x = MIN(MIN(quad->x[0], quad->x[1]), MIN(quad->x[2], quad->x[3]));
        int max_x = MAX(MAX(quad->x[0], quad->x[1]), MAX(quad->x[2], quad->x[3]));
        int min_y = MIN(MIN(quad->y[0], quad->y[1]), MIN(quad->y[2], quad->y[3]));
        int max_y = MAX(MAX(quad->y[0], quad->y[1]), MAX(quad->y[2], quad->y[3]));
        x = MAX(min_x, MIN(max_x, x));
        y = MAX(min_y, MIN(max_y, y));
    }
    size_t closest = 0;
    uint64_t closest_distance = UINT64_MAX;
    for (size_t corner = 0; corner < 4; corner++) {
        int64_t dx = (int64_t)x - quad->x[corner];
        int64_t dy = (int64_t)y - quad->y[corner];
        uint64_t distance = (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
        if (distance <= closest_distance) {
            closest = corner;
            closest_distance = distance;
        }
    }
    const uint32_t *channels[4] = {quad->scalar, quad->red, quad->green, quad->blue};
    for (size_t channel = 0; channel < 4; channel++) {
        light[channel] = (uint16_t)channels[channel][closest];
    }
    return true;
}

static uint16_t gpu_map_renderer_probe_interpolate(uint16_t left,
                                                   uint16_t right,
                                                   uint64_t progress,
                                                   uint64_t duration) {
    if (right >= left) {
        return (uint16_t)(left + ((uint64_t)(right - left) * progress) / duration);
    }
    return (uint16_t)(left - ((uint64_t)(left - right) * progress) / duration);
}

static bool gpu_map_renderer_probe_horizontal(uint32_t offset,
                                              uint32_t count,
                                              int row_y,
                                              int x,
                                              uint16_t light[4]) {
    const gpu_map_light_span_t *left = NULL;
    const gpu_map_light_span_t *right = NULL;
    for (uint32_t index = 0; index < count; index++) {
        const gpu_map_light_span_t *span = &uploaded_light_spans[offset + index];
        if (x < span->first_x) {
            right = span;
            break;
        }
        left = span;
        if (x <= span->last_x) {
            return gpu_map_renderer_probe_quad(span->quad, x, row_y, light);
        }
    }
    if (left == NULL) {
        return gpu_map_renderer_probe_quad(right->quad, right->first_x, row_y, light);
    }
    if (right == NULL) {
        return gpu_map_renderer_probe_quad(left->quad, left->last_x, row_y, light);
    }
    uint16_t left_light[4];
    uint16_t right_light[4];
    if (!gpu_map_renderer_probe_quad(left->quad, left->last_x, row_y, left_light) ||
        !gpu_map_renderer_probe_quad(right->quad, right->first_x, row_y, right_light)) {
        return false;
    }
    uint64_t duration = (uint64_t)(right->first_x - left->last_x);
    uint64_t progress = (uint64_t)(x - left->last_x);
    for (size_t channel = 0; channel < 4; channel++) {
        light[channel] = gpu_map_renderer_probe_interpolate(left_light[channel],
                                                            right_light[channel],
                                                            progress,
                                                            duration);
    }
    return true;
}

static bool gpu_map_renderer_probe_light(uint32_t key, int x, uint8_t owner, uint16_t light[4]) {
    uint32_t encoded_row = key & GPU_MAP_LIGHT_KEY_MASK;
    if (encoded_row == 0 || encoded_row > GPU_MAP_LIGHT_QUAD_KEY_MAX ||
        encoded_row - 1U >= uploaded_light_rows_num) {
        return false;
    }
    const gpu_map_light_row_t *row = &uploaded_light_rows[encoded_row - 1U];
    if (row->owner != owner || row->upper_offset + row->upper_count > uploaded_light_spans_num ||
        row->lower_offset + row->lower_count > uploaded_light_spans_num || row->upper_count == 0 ||
        row->lower_count == 0) {
        return false;
    }
    uint16_t upper[4];
    if (!gpu_map_renderer_probe_horizontal(row->upper_offset,
                                           row->upper_count,
                                           row->upper_y,
                                           x,
                                           upper)) {
        return false;
    }
    if (row->upper_y == row->lower_y) {
        memcpy(light, upper, sizeof(upper));
        return true;
    }
    uint16_t lower[4];
    if (!gpu_map_renderer_probe_horizontal(row->lower_offset,
                                           row->lower_count,
                                           row->lower_y,
                                           x,
                                           lower)) {
        return false;
    }
    uint64_t duration = (uint64_t)(row->lower_y - row->upper_y);
    uint64_t progress =
        (uint64_t)(MAX(row->upper_y, MIN(row->lower_y, row->sample_y)) - row->upper_y);
    for (size_t channel = 0; channel < 4; channel++) {
        light[channel] =
            gpu_map_renderer_probe_interpolate(upper[channel], lower[channel], progress, duration);
    }
    return true;
}

bool gpu_map_renderer_probe(int x, int y, uint8_t light_owner, gpu_map_renderer_probe_t *probe) {
    if (map_device == NULL || probe == NULL || x < 0 || y < 0 || x >= target_width ||
        y >= target_height || light_owner >= MAP2_LEVELS) {
        SDL_SetError("GPU map probe coordinates are outside the completed targets");
        return false;
    }
    memset(probe, 0, sizeof(*probe));
    bool downloaded = gpu_map_renderer_download_pixel(albedo_target, 0, x, y, 4, probe->albedo) &&
                      gpu_map_renderer_download_pixel(owner_target,
                                                      0,
                                                      x,
                                                      y,
                                                      sizeof(probe->lighting_key),
                                                      &probe->lighting_key) &&
                      gpu_map_renderer_download_pixel(final_target, 0, x, y, 4, probe->final_color);
    return downloaded &&
           gpu_map_renderer_probe_light(probe->lighting_key, x, light_owner, probe->light);
}
#endif

void gpu_map_renderer_invalidate_surface(SDL_Surface *surface) {
    if (surface == NULL) {
        return;
    }
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    SDL_ClearProperty(properties, GPU_MAP_SURFACE_ASSET_PROPERTY);
    SDL_SetNumberProperty(properties, GPU_MAP_SURFACE_GENERATION_PROPERTY, 0);
}
