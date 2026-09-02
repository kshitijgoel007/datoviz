/*
 * Copyright (c) 2021 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/* metal_external_buffer - Vulkan-owned buffer sharing with Metal on Apple Silicon.
 *
 * Scenario: lab_metal_external_buffer
 * Style: non-public lab, Apple-Silicon-only Vulkan/Metal external buffer proof
 *
 * Vulkan releases a dedicated vertex buffer and signals timeline value 1. Metal waits for 1,
 * performs a GPU blit into the exported MTLBuffer, and signals 2. The proof repeats explicit
 * Vulkan-to-Metal ownership and timeline handoffs for 100 cycles, then DRP2 renders a fullscreen
 * red triangle from the same Vulkan buffer and verifies the readback pixel.
 *
 * Build and run: just example-c lab/metal_external_buffer
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <volk.h>

#include "_compat.h"
#include "datoviz/drp2.h"
#include "datoviz/vk/gpu_ctx.h"
#include "datoviz/vk/metal_interop.h"
#include "datoviz/vklite.h"
#include "metal_external_buffer_metal.h"



/*************************************************************************************************/
/*  Constants                                                                                    */
/*************************************************************************************************/

#define INTEROP_CYCLES 100

#define ID_EXTERNAL_VERTEX_BUFFER 1
#define ID_VERTEX_SHADER          2
#define ID_FRAGMENT_SHADER        3
#define ID_PIPELINE               4
#define ID_COLOR_TARGET           5
#define ID_READBACK_BUFFER        6
#define ID_ENCODER                10
#define ID_RENDER_PASS            11
#define ID_COMMAND_BUFFER         12
#define ID_SUBMIT                 13



/*************************************************************************************************/
/*  Types                                                                                        */
/*************************************************************************************************/

typedef struct
{
    float position[2];
} Vertex;



/*************************************************************************************************/
/*  Helpers                                                                                      */
/*************************************************************************************************/

/**
 * Submit one already-recorded command buffer with optional timeline wait and signal values.
 *
 * This function submits asynchronously and never waits for queue or device idle.
 *
 * @param queue main Vulkan queue
 * @param cmds recorded command wrapper
 * @param semaphore timeline semaphore
 * @param wait_value value to wait on, or zero for no wait
 * @param wait_stage earliest stage that consumes the waited resource
 * @param signal_value value to signal, or zero for no signal
 * @return true when queue submission succeeds
 */
static bool _submit_handoff(
    DvzQueue* queue, DvzCommands* cmds, DvzSemaphore* semaphore, uint64_t wait_value,
    VkPipelineStageFlags2 wait_stage, uint64_t signal_value)
{
    DvzSubmit* submit = dvz_submit_create_wrapper();
    if (submit == NULL)
        return false;
    dvz_submit(submit);
    if (wait_value != 0)
    {
        dvz_submit_wait(
            submit, dvz_semaphore_handle(semaphore), wait_value, wait_stage);
    }
    dvz_submit_command(submit, dvz_commands_handle(cmds));
    if (signal_value != 0)
    {
        dvz_submit_signal(
            submit, dvz_semaphore_handle(semaphore), signal_value,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    VkResult result =
        (VkResult)dvz_submit_send(submit, dvz_queue_handle(queue), VK_NULL_HANDLE);
    dvz_submit_free(submit);
    return result == VK_SUCCESS;
}



/**
 * Render the shared fullscreen triangle through the existing DRP2 runtime and verify red output.
 *
 * @param device Datoviz Vulkan device
 * @param allocator Datoviz allocator
 * @param external_buffer shared Vulkan vertex buffer
 * @param vertex_size logical vertex byte size
 * @return 0 on success
 */
static int _render_external_buffer(
    DvzDevice* device, DvzVma* allocator, DvzBuffer* external_buffer, uint64_t vertex_size)
{
    int out = 1;
    DvzDrp2Runtime* runtime = NULL;
    DvzDrp2CommandStream* stream = NULL;

    DvzDrp2RuntimeConfig runtime_config = dvz_drp2_runtime_vklite_config(device, allocator);
    runtime = dvz_drp2_runtime_vklite(&runtime_config);
    if (runtime == NULL)
        goto cleanup;

    DvzDrp2ExternalBufferDesc external_desc = {
        DVZ_STRUCT_INIT_FIELDS(DvzDrp2ExternalBufferDesc),
        .buffer = external_buffer,
        .size = vertex_size,
        .usage = DVZ_DRP2_BUFFER_USAGE_VERTEX,
    };
    if (!dvz_drp2_runtime_register_external_buffer(
            runtime, ID_EXTERNAL_VERTEX_BUFFER, &external_desc))
    {
        goto cleanup;
    }

    uint32_t binding_stride = sizeof(Vertex);
    uint32_t binding_step = DVZ_DRP2_VERTEX_STEP_MODE_VERTEX;
    uint32_t attr_binding = 0;
    uint32_t attr_location = 0;
    DvzFormat attr_format = DVZ_FORMAT_R32G32_SFLOAT;
    uint32_t attr_offset = 0;

    stream = dvz_drp2_stream();
    if (stream == NULL)
        goto cleanup;

    bool ok = dvz_drp2_stream_hello_renderer(stream, "metal-external-buffer");
    ok = ok && dvz_drp2_stream_renderer_hello_reply(stream, "datoviz");
    ok = ok && dvz_drp2_stream_create_shader_module_format(
                   stream, ID_VERTEX_SHADER, "vertex", "glsl",
                   "#version 450\n"
                   "layout(location=0) in vec2 position;\n"
                   "void main(){gl_Position=vec4(position,0,1);}\n");
    ok = ok && dvz_drp2_stream_create_shader_module_format(
                   stream, ID_FRAGMENT_SHADER, "fragment", "glsl",
                   "#version 450\n"
                   "layout(location=0) out vec4 color;\n"
                   "void main(){color=vec4(1,0,0,1);}\n");

    DvzDrp2RenderPipelineDesc pipeline = dvz_drp2_render_pipeline_desc();
    pipeline.id = ID_PIPELINE;
    pipeline.vertex_shader_module_id = ID_VERTEX_SHADER;
    pipeline.fragment_shader_module_id = ID_FRAGMENT_SHADER;
    pipeline.vertex_buffer_slots = 1;
    pipeline.binding_count = 1;
    pipeline.binding_strides = &binding_stride;
    pipeline.binding_step_modes = &binding_step;
    pipeline.attr_count = 1;
    pipeline.attr_bindings = &attr_binding;
    pipeline.attr_locations = &attr_location;
    pipeline.attr_formats = &attr_format;
    pipeline.attr_offsets = &attr_offset;
    ok = ok && dvz_drp2_stream_create_render_pipeline(stream, &pipeline);
    ok = ok && dvz_drp2_stream_create_texture_2d_usage(
                   stream, ID_COLOR_TARGET, 2, 2,
                   DVZ_DRP2_TEXTURE_USAGE_RENDER_ATTACHMENT | DVZ_DRP2_TEXTURE_USAGE_COPY_SRC);
    ok = ok && dvz_drp2_stream_create_buffer(
                   stream, ID_READBACK_BUFFER, 4,
                   DVZ_DRP2_BUFFER_USAGE_COPY_DST | DVZ_DRP2_BUFFER_USAGE_MAP_READ);
    ok = ok && dvz_drp2_stream_begin_command_encoder(stream, ID_ENCODER);
    ok = ok && dvz_drp2_stream_begin_render_pass_clear(
                   stream, ID_RENDER_PASS, ID_ENCODER, ID_COLOR_TARGET, 0, 0, 0, 1);
    ok = ok && dvz_drp2_stream_set_pipeline(stream, ID_RENDER_PASS, ID_PIPELINE);
    ok = ok && dvz_drp2_stream_set_vertex_buffer(
                   stream, ID_RENDER_PASS, 0, ID_EXTERNAL_VERTEX_BUFFER, 0);
    ok = ok && dvz_drp2_stream_draw(stream, ID_RENDER_PASS, 3, 1, 0, 0);
    ok = ok && dvz_drp2_stream_end_render_pass(stream, ID_RENDER_PASS);
    ok = ok && dvz_drp2_stream_copy_texture_to_buffer(
                   stream, ID_ENCODER, ID_COLOR_TARGET, ID_READBACK_BUFFER, 0, 1, 1, 4, 1);
    ok = ok && dvz_drp2_stream_finish_command_encoder(stream, ID_ENCODER, ID_COMMAND_BUFFER);
    ok = ok && dvz_drp2_stream_queue_submit(stream, ID_COMMAND_BUFFER, ID_SUBMIT);
    if (!ok)
        goto cleanup;

    DvzDrp2ValidationResult result = dvz_drp2_runtime_execute(runtime, stream);
    if (!result.ok)
    {
        dvz_fprintf(
            stderr, "DRP2 execution failed at command %u with code %d\n", result.command_index,
            result.code);
        goto cleanup;
    }

    uint8_t pixel[4] = {0};
    ok = dvz_drp2_runtime_download_buffer(runtime, ID_READBACK_BUFFER, 0, 4, pixel);
    if (!ok || pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
    {
        dvz_fprintf(
            stderr, "unexpected render result: rgba(%u, %u, %u, %u)\n", pixel[0], pixel[1],
            pixel[2], pixel[3]);
        goto cleanup;
    }

    out = 0;

cleanup:
    if (stream != NULL)
        dvz_drp2_stream_destroy(stream);
    if (runtime != NULL)
        dvz_drp2_runtime_destroy(runtime);
    return out;
}



/*************************************************************************************************/
/*  Main                                                                                         */
/*************************************************************************************************/

int main(int argc, char** argv)
{
    (void)setenv("MVK_CONFIG_USE_MTLHEAP", "0", 0);

    uint32_t gpu_index = 0;
    if (argc > 1)
        gpu_index = (uint32_t)strtoul(argv[1], NULL, 10);

    int out = 1;
    DvzGpuCtx* gpu = NULL;
    DvzVma* render_allocator = NULL;
    DvzBuffer* buffer = NULL;
    DvzBuffer* staging = NULL;
    DvzSemaphore* semaphore = NULL;
    DvzMetalInteropLab* metal = NULL;
    DvzCommands* commands[INTEROP_CYCLES + 1] = {0};
    uint32_t command_count = 0;
    uint64_t last_metal_value = 0;
    bool metal_finished = false;

    const Vertex vertices[3] = {
        {{-1.0f, -1.0f}},
        {{3.0f, -1.0f}},
        {{-1.0f, 3.0f}},
    };
    const uint64_t vertex_size = sizeof(vertices);

    gpu = dvz_interop_gpu_ctx(
        gpu_index, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT);
    if (gpu == NULL)
    {
        dvz_fprintf(
            stdout,
            "metal_external_buffer: SKIP (Apple Silicon Vulkan-Metal extensions unavailable)\n");
        return 0;
    }

    DvzDevice* device = dvz_gpu_ctx_device(gpu);
    DvzVma* allocator = dvz_gpu_ctx_alloc(gpu);
    DvzQueue* queue = dvz_device_queue(device, DVZ_QUEUE_MAIN);
    if (queue == NULL)
        goto cleanup;
    render_allocator = dvz_allocator_create();
    if (render_allocator == NULL || dvz_device_allocator(device, 0, render_allocator) != 0)
        goto cleanup;

    staging = dvz_buffer_create_wrapper();
    if (staging == NULL)
        goto cleanup;
    dvz_buffer(device, render_allocator, staging);
    dvz_buffer_size(staging, vertex_size);
    dvz_buffer_usage(staging, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    dvz_buffer_flags(staging, DVZ_ALLOC_HOST_ACCESS_RANDOM);
    if (dvz_buffer_create(staging) != 0)
        goto cleanup;

    buffer = dvz_buffer_create_wrapper();
    if (buffer == NULL)
        goto cleanup;
    dvz_buffer(device, allocator, buffer);
    dvz_buffer_size(buffer, vertex_size);
    dvz_buffer_usage(
        buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    dvz_buffer_flags(buffer, DVZ_ALLOC_DEDICATED_MEMORY);
    if (dvz_buffer_create(buffer) != 0)
    {
        dvz_fprintf(stdout, "metal_external_buffer: SKIP (MTLBuffer allocation unavailable)\n");
        out = 0;
        goto cleanup;
    }

    semaphore = dvz_semaphore_create_wrapper();
    if (semaphore == NULL || dvz_interop_metal_semaphore_timeline(device, 0, semaphore) != 0)
    {
        dvz_fprintf(stdout, "metal_external_buffer: SKIP (Metal shared event unavailable)\n");
        out = 0;
        goto cleanup;
    }

    DvzInteropBufferExportConfig export_config = dvz_interop_buffer_export_config();
    export_config.size = vertex_size;
    export_config.drp2_usage =
        DVZ_DRP2_BUFFER_USAGE_VERTEX | DVZ_DRP2_BUFFER_USAGE_COPY_SRC;
    export_config.semaphore = semaphore;
    export_config.semaphore_handle_type = 0;
    export_config.semaphore_value = 0;
    DvzInteropMetalBufferExport export_desc = {0};
    if (dvz_interop_metal_buffer_export_from_buffer(
            buffer, &export_config, &export_desc) != 0)
    {
        dvz_fprintf(stdout, "metal_external_buffer: SKIP (Metal object export unavailable)\n");
        out = 0;
        goto cleanup;
    }

    if (dvz_metal_interop_lab_create(
            export_desc.metal_buffer, export_desc.metal_shared_event,
            export_desc.allocation_size, &metal) != 0)
    {
        goto cleanup;
    }

    DvzCommands* initial = dvz_commands_create_wrapper();
    if (initial == NULL)
        goto cleanup;
    commands[command_count++] = initial;
    dvz_commands(device, queue, 1, initial);
    if (dvz_cmd_begin_result(initial) != 0 ||
        !dvz_interop_metal_buffer_record_release(
            initial, buffer, 0, vertex_size, DVZ_INTEROP_BUFFER_CONSUMER_NONE) ||
        dvz_cmd_end_result(initial) != 0 ||
        !_submit_handoff(queue, initial, semaphore, 0, VK_PIPELINE_STAGE_2_NONE, 1))
    {
        goto cleanup;
    }

    uint64_t final_metal_value = 0;
    for (uint32_t cycle = 0; cycle < INTEROP_CYCLES; cycle++)
    {
        uint64_t metal_wait = 2 * (uint64_t)cycle + 1;
        uint64_t metal_signal = metal_wait + 1;
        uint64_t vulkan_signal = metal_signal + 1;
        bool final_cycle = cycle + 1 == INTEROP_CYCLES;

        if (!dvz_metal_interop_lab_enqueue_blit(
                metal, vertices, sizeof(vertices), metal_wait, metal_signal))
        {
            goto cleanup;
        }
        last_metal_value = metal_signal;

        DvzCommands* acquire = dvz_commands_create_wrapper();
        if (acquire == NULL)
            goto cleanup;
        commands[command_count++] = acquire;
        dvz_commands(device, queue, 1, acquire);
        if (dvz_cmd_begin_result(acquire) != 0 ||
            !dvz_interop_metal_buffer_record_acquire(
                acquire, buffer, 0, vertex_size,
                DVZ_INTEROP_BUFFER_CONSUMER_TRANSFER_READ))
        {
            goto cleanup;
        }
        VkBufferCopy copy = {
            .srcOffset = 0,
            .dstOffset = 0,
            .size = vertex_size,
        };
        vkCmdCopyBuffer(
            dvz_commands_handle(acquire), dvz_buffer_handle(buffer),
            dvz_buffer_handle(staging), 1, &copy);
        if (!final_cycle &&
            !dvz_interop_metal_buffer_record_release(
                acquire, buffer, 0, vertex_size,
                DVZ_INTEROP_BUFFER_CONSUMER_TRANSFER_READ))
        {
            goto cleanup;
        }
        if (final_cycle)
        {
            DvzBarriers barriers = {0};
            dvz_barriers(&barriers);
            DvzBarrierBuffer* barrier = dvz_barriers_buffer(
                &barriers, dvz_buffer_handle(buffer), 0, vertex_size);
            if (barrier == NULL)
                goto cleanup;
            dvz_barrier_buffer_stage(
                barrier, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
            dvz_barrier_buffer_access(
                barrier, VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
            dvz_barrier_buffer_queue(
                barrier, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
            dvz_cmd_barriers(acquire, &barriers);
        }
        if (dvz_cmd_end_result(acquire) != 0 ||
            !_submit_handoff(
                queue, acquire, semaphore, metal_signal, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                final_cycle ? 0 : vulkan_signal))
        {
            goto cleanup;
        }
        final_metal_value = metal_signal;
    }

    dvz_device_wait(device);
    metal_finished = true;
    if (!dvz_metal_interop_lab_finish_and_verify(
            metal, vertices, sizeof(vertices), final_metal_value))
    {
        dvz_fprintf(stderr, "final exported Metal buffer bytes do not match\n");
        goto cleanup;
    }
    if (dvz_semaphore_query(semaphore) < final_metal_value)
    {
        dvz_fprintf(stderr, "Vulkan cannot observe the final Metal timeline signal\n");
        goto cleanup;
    }
    Vertex vulkan_vertices[3] = {0};
    dvz_buffer_download(staging, 0, sizeof(vulkan_vertices), vulkan_vertices);
    if (memcmp(vulkan_vertices, vertices, sizeof(vertices)) != 0)
    {
        dvz_fprintf(stderr, "Vulkan transfer read did not observe the final Metal bytes\n");
        goto cleanup;
    }
    if (_render_external_buffer(device, render_allocator, buffer, vertex_size) != 0)
        goto cleanup;
    if (dvz_gpu_ctx_error_count(gpu) != 0)
    {
        dvz_fprintf(stderr, "Vulkan validation reported Metal interop errors\n");
        goto cleanup;
    }

    dvz_fprintf(
        stdout,
        "metal_external_buffer: OK (%u Vulkan<->Metal cycles, final timeline=%llu, red pixel)\n",
        INTEROP_CYCLES, (unsigned long long)final_metal_value);
    out = 0;

cleanup:
    if (metal != NULL && last_metal_value != 0 && !metal_finished)
    {
        (void)dvz_metal_interop_lab_finish_and_verify(
            metal, vertices, sizeof(vertices), last_metal_value);
    }
    if (gpu != NULL)
        dvz_device_wait(dvz_gpu_ctx_device(gpu));
    if (metal != NULL)
        dvz_metal_interop_lab_destroy(metal);
    for (uint32_t i = 0; i < command_count; i++)
    {
        dvz_commands_destroy(commands[i]);
        dvz_commands_free(commands[i]);
    }
    if (semaphore != NULL)
    {
        dvz_semaphore_destroy(semaphore);
        dvz_semaphore_free(semaphore);
    }
    if (buffer != NULL)
    {
        dvz_buffer_destroy(buffer);
        dvz_buffer_free(buffer);
    }
    if (staging != NULL)
    {
        dvz_buffer_destroy(staging);
        dvz_buffer_free(staging);
    }
    if (render_allocator != NULL)
    {
        dvz_allocator_destroy(render_allocator);
        dvz_allocator_free(render_allocator);
    }
    if (gpu != NULL)
        dvz_gpu_ctx_destroy(gpu);
    return out;
}
