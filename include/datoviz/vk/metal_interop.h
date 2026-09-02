/*
 * Copyright (c) 2021 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************************************/
/*  Vulkan-Metal buffer interop                                                                  */
/*************************************************************************************************/

#pragma once



/*************************************************************************************************/
/*  Includes                                                                                     */
/*************************************************************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "datoviz/vk/memory_interop.h"



/*************************************************************************************************/
/*  Typedefs                                                                                     */
/*************************************************************************************************/

typedef struct DvzCommands DvzCommands;
typedef struct DvzInteropMetalBufferExport DvzInteropMetalBufferExport;



/*************************************************************************************************/
/*  Constants                                                                                    */
/*************************************************************************************************/

#define DVZ_INTEROP_METAL_BUFFER_EXPORT_VERSION 1



/*************************************************************************************************/
/*  Structs                                                                                      */
/*************************************************************************************************/

/**
 * Borrowed Metal objects and Vulkan allocation metadata for one exported buffer.
 *
 * `metal_buffer` remains owned by the Vulkan device memory and `metal_shared_event` remains owned
 * by the Vulkan semaphore. The caller must not release either object and must stop using both
 * before destroying their Vulkan owners.
 */
struct DvzInteropMetalBufferExport
{
    uint32_t version;
    void* metal_buffer;
    void* metal_shared_event;
    uint64_t allocation_size;
    uint64_t offset;
    uint64_t size;
    uint32_t usage;
    uint32_t vk_usage;
    uint32_t drp2_usage;
    uint32_t flags;
    uint32_t allocation_flags;
    uint32_t device_uuid_valid;
    uint8_t device_uuid[VK_UUID_SIZE];
    uint64_t semaphore_value;
};



/*************************************************************************************************/
/*  Functions                                                                                    */
/*************************************************************************************************/

EXTERN_C_ON

/**
 * Create a timeline semaphore that may be exported as a borrowed Metal shared event.
 *
 * This function requires Apple Silicon and `VK_EXT_metal_objects`. `semaphore` must be an empty
 * wrapper created with dvz_semaphore_create_wrapper(). Destroy it with dvz_semaphore_destroy() and
 * dvz_semaphore_free().
 *
 * @param device logical Vulkan device with Metal-object export enabled
 * @param value initial timeline value
 * @param[out] semaphore created Metal-exportable timeline semaphore
 * @return 0 on success, -1 when unavailable or invalid
 */
DVZ_EXPORT int dvz_interop_metal_semaphore_timeline(
    DvzDevice* device, uint64_t value, DvzSemaphore* semaphore);



/**
 * Export a Vulkan-owned buffer allocation and timeline semaphore to borrowed Metal objects.
 *
 * The allocator must use `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`, the allocation must
 * be dedicated with offset zero, and `config` must name a semaphore created by
 * dvz_interop_metal_semaphore_timeline(). `config->semaphore_handle_type` must be zero because this
 * path exports a Metal shared event instead of an operating-system semaphore handle. The output is
 * zeroed on every failure.
 *
 * @param buffer live Vulkan-owned buffer
 * @param config logical range, usage metadata, and Metal-exportable semaphore
 * @param[out] out borrowed Metal objects and allocation metadata
 * @return 0 on success, -1 when unavailable or invalid
 */
DVZ_EXPORT int dvz_interop_metal_buffer_export_from_buffer(
    DvzBuffer* buffer, const DvzInteropBufferExportConfig* config,
    DvzInteropMetalBufferExport* out);



/**
 * Record an external-to-Vulkan buffer ownership acquire barrier.
 *
 * This helper only records into `cmds`; it never allocates, submits, or waits. The command buffer
 * must be submitted on the device main queue with an external timeline wait ordered before the
 * declared Vulkan consumer. `DVZ_INTEROP_BUFFER_CONSUMER_NONE` is not valid for acquire.
 *
 * @param cmds recording command buffer wrapper
 * @param buffer live exported buffer
 * @param offset byte offset within the logical Vulkan buffer
 * @param size non-zero byte size of the synchronized range
 * @param consumer first Vulkan consumer after the acquire
 * @return true when the barrier was recorded
 */
DVZ_EXPORT bool dvz_interop_metal_buffer_record_acquire(
    DvzCommands* cmds, DvzBuffer* buffer, uint64_t offset, uint64_t size,
    DvzInteropBufferConsumer consumer);



/**
 * Record a Vulkan-to-external buffer ownership release barrier.
 *
 * This helper only records into `cmds`; it never allocates, submits, or waits. The command buffer
 * must be submitted on the device main queue with an external timeline signal ordered after the
 * barrier. Use `DVZ_INTEROP_BUFFER_CONSUMER_NONE` for the initial release before any Vulkan access.
 *
 * @param cmds recording command buffer wrapper
 * @param buffer live exported buffer
 * @param offset byte offset within the logical Vulkan buffer
 * @param size non-zero byte size of the synchronized range
 * @param consumer last Vulkan consumer before the release, or NONE for the initial release
 * @return true when the barrier was recorded
 */
DVZ_EXPORT bool dvz_interop_metal_buffer_record_release(
    DvzCommands* cmds, DvzBuffer* buffer, uint64_t offset, uint64_t size,
    DvzInteropBufferConsumer consumer);



EXTERN_C_OFF
