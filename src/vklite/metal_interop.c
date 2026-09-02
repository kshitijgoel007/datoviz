/*
 * Copyright (c) 2021 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************************************/
/*  Vulkan-Metal buffer interop                                                                  */
/*************************************************************************************************/



/*************************************************************************************************/
/*  Includes                                                                                     */
/*************************************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

#include "_alloc.h"
#include "_assertions.h"
#include "_buffers.h"
#include "_commands.h"
#include "_compat.h"
#include "_log.h"
#include "../vk/_memory.h"
#include "_sync.h"
#include "datoviz/vk/device.h"
#include "datoviz/vk/metal_interop.h"
#include "datoviz/vk/queues.h"
#include "datoviz/vklite/buffers.h"
#include "datoviz/vklite/commands.h"
#include "datoviz/vklite/sync.h"



/*************************************************************************************************/
/*  Constants                                                                                    */
/*************************************************************************************************/

#define DVZ_INTEROP_METAL_CONFIG_KNOWN_FLAGS 0u

#if OS_MACOS && defined(__aarch64__)
#define DVZ_INTEROP_METAL_RUNTIME 1
#else
#define DVZ_INTEROP_METAL_RUNTIME 0
#endif



/*************************************************************************************************/
/*  Helpers                                                                                      */
/*************************************************************************************************/

/**
 * Validate the shared interop export configuration ABI.
 *
 * @param config export configuration
 * @return true when the configuration is valid
 */
static bool _metal_export_config_validate(const DvzInteropBufferExportConfig* config)
{
    if (config == NULL)
    {
        log_error("Metal buffer export requires a configuration");
        return false;
    }
    if (!DVZ_STRUCT_VALID(
            config, DvzInteropBufferExportConfig, DVZ_INTEROP_METAL_CONFIG_KNOWN_FLAGS))
    {
        log_error("invalid DvzInteropBufferExportConfig ABI prologue");
        return false;
    }
    return true;
}



#if DVZ_INTEROP_METAL_RUNTIME
/**
 * Return whether exact external-buffer properties permit MTLBuffer export.
 *
 * @param buffer live buffer
 * @return true when the exact flags and usage are exportable and compatible
 */
static bool _metal_external_buffer_supported(DvzBuffer* buffer)
{
    ANN(buffer);
    ANN(buffer->device);

    VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = 0,
        .usage = buffer->req_usage,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT,
    };
    VkExternalBufferProperties properties = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalBufferProperties(
        dvz_device_physical_device(buffer->device), &info, &properties);

    VkExternalMemoryProperties memory = properties.externalMemoryProperties;
    if ((memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0)
    {
        log_error("exact Vulkan buffer configuration is not exportable as MTLBuffer");
        return false;
    }
    if ((memory.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT) == 0)
    {
        log_error("exact Vulkan buffer configuration is not compatible with MTLBuffer");
        return false;
    }
    return true;
}



/**
 * Resolve synchronization scopes for a Metal interop consumer.
 *
 * @param consumer declared Vulkan consumer
 * @param allow_none whether the initial no-access state is accepted
 * @param[out] stage synchronization2 pipeline stage
 * @param[out] access synchronization2 access mask
 * @param[out] usage required Vulkan buffer usage
 * @return true when the consumer is valid
 */
static bool _metal_consumer_sync(
    DvzInteropBufferConsumer consumer, bool allow_none, VkPipelineStageFlags2* stage,
    VkAccessFlags2* access, VkBufferUsageFlags* usage)
{
    ANN(stage);
    ANN(access);
    ANN(usage);

    switch (consumer)
    {
    case DVZ_INTEROP_BUFFER_CONSUMER_VERTEX_ATTRIBUTE_READ:
        *stage = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        *access = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        *usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        return true;
    case DVZ_INTEROP_BUFFER_CONSUMER_TRANSFER_READ:
        *stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_READ_BIT;
        *usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        return true;
    case DVZ_INTEROP_BUFFER_CONSUMER_NONE:
        if (allow_none)
        {
            *stage = VK_PIPELINE_STAGE_2_NONE;
            *access = VK_ACCESS_2_NONE;
            *usage = 0;
            return true;
        }
        break;
    default:
        break;
    }
    log_error("invalid Metal interop buffer consumer (%d)", consumer);
    return false;
}



/**
 * Validate a range and resolve the local main queue family.
 *
 * @param cmds recording command wrapper
 * @param buffer live buffer
 * @param offset byte offset
 * @param size byte size
 * @param[out] queue_family main queue family index
 * @return true when the recording inputs are valid
 */
static bool _metal_record_validate(
    DvzCommands* cmds, DvzBuffer* buffer, uint64_t offset, uint64_t size,
    uint32_t* queue_family)
{
    if (cmds == NULL || buffer == NULL || queue_family == NULL)
        return false;
    if (!dvz_obj_is_created(&buffer->obj) || buffer->vk_buffer == VK_NULL_HANDLE ||
        buffer->device == NULL || buffer->allocator == NULL || buffer->alloc == NULL ||
        dvz_allocator_external(buffer->allocator) !=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT ||
        !dvz_allocation_flags_contains(
            dvz_allocation_flags(buffer->alloc), DVZ_ALLOC_DEDICATED_MEMORY) ||
        buffer->alloc->info.offset != 0 || cmds->device != buffer->device ||
        dvz_commands_count(cmds) == 0 || dvz_commands_handle(cmds) == VK_NULL_HANDLE)
    {
        log_error("invalid Metal interop command or buffer wrapper");
        return false;
    }
    if (size == 0 || offset >= buffer->req_size || size > buffer->req_size - offset)
    {
        log_error("Metal interop synchronization range exceeds the logical buffer");
        return false;
    }

    DvzQueue* queue = dvz_device_queue(buffer->device, DVZ_QUEUE_MAIN);
    if (queue == NULL)
    {
        log_error("main Vulkan queue unavailable for Metal interop ownership transfer");
        return false;
    }
    *queue_family = dvz_queue_family(queue);
    if (cmds->queue == NULL || dvz_queue_family(cmds->queue) != *queue_family)
    {
        log_error("Metal interop ownership barriers must be submitted on the main queue family");
        return false;
    }
    return true;
}
#endif



/*************************************************************************************************/
/*  Functions                                                                                    */
/*************************************************************************************************/

/**
 * Create a Metal-exportable timeline semaphore.
 *
 * @param device logical Vulkan device
 * @param value initial timeline value
 * @param[out] semaphore created semaphore wrapper
 * @return 0 on success, -1 when unavailable or invalid
 */
int dvz_interop_metal_semaphore_timeline(
    DvzDevice* device, uint64_t value, DvzSemaphore* semaphore)
{
#if DVZ_INTEROP_METAL_RUNTIME
    if (device == NULL || semaphore == NULL)
        return -1;
    if (dvz_obj_is_created(&semaphore->obj) || semaphore->vk_semaphore != VK_NULL_HANDLE)
    {
        log_error("Metal timeline semaphore requires an empty wrapper");
        return -1;
    }
    if (!dvz_device_has_extension(device, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) ||
        vkExportMetalObjectsEXT == NULL)
    {
        log_error("Metal shared-event export is unavailable on this Vulkan device");
        return -1;
    }

    VkExportMetalObjectCreateInfoEXT metal_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT,
    };
    VkSemaphoreTypeCreateInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &metal_info,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = value,
    };
    VkSemaphoreCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_info,
    };

    VkSemaphore handle = VK_NULL_HANDLE;
    VkResult result = vkCreateSemaphore(dvz_device_handle(device), &info, NULL, &handle);
    if (result != VK_SUCCESS || handle == VK_NULL_HANDLE)
    {
        log_error("failed to create Metal-exportable timeline semaphore (%d)", result);
        return -1;
    }

    semaphore->device = device;
    semaphore->vk_semaphore = handle;
    semaphore->value = value;
    semaphore->timeline = true;
    semaphore->external_handle_types = 0;
    semaphore->metal_shared_event_exportable = true;
    dvz_obj_created(&semaphore->obj);
    return 0;
#else
    (void)device;
    (void)value;
    (void)semaphore;
    return -1;
#endif
}



/**
 * Export one dedicated Vulkan buffer and semaphore as borrowed Metal objects.
 *
 * @param buffer live Vulkan-owned buffer
 * @param config export configuration
 * @param[out] out borrowed Metal objects and metadata
 * @return 0 on success, -1 when unavailable or invalid
 */
int dvz_interop_metal_buffer_export_from_buffer(
    DvzBuffer* buffer, const DvzInteropBufferExportConfig* config,
    DvzInteropMetalBufferExport* out)
{
    if (out == NULL)
        return -1;
    dvz_memset(out, sizeof(*out), 0, sizeof(*out));

#if DVZ_INTEROP_METAL_RUNTIME
    if (buffer == NULL || !_metal_export_config_validate(config))
        return -1;
    if (!dvz_obj_is_created(&buffer->obj) || buffer->vk_buffer == VK_NULL_HANDLE ||
        buffer->alloc == NULL || buffer->allocator == NULL || buffer->device == NULL)
    {
        log_error("cannot export an uncreated Metal interop buffer");
        return -1;
    }
    if (dvz_allocator_external(buffer->allocator) !=
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT)
    {
        log_error("Metal buffer export requires an MTLBUFFER allocator handle type");
        return -1;
    }
    if (!dvz_allocation_flags_contains(
            dvz_allocation_flags(buffer->alloc), DVZ_ALLOC_DEDICATED_MEMORY) ||
        buffer->alloc->info.offset != 0)
    {
        log_error("Metal buffer export requires dedicated memory at allocation offset zero");
        return -1;
    }
    if (!dvz_device_has_extension(buffer->device, VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME) ||
        !dvz_device_has_extension(buffer->device, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) ||
        vkGetMemoryMetalHandleEXT == NULL || vkExportMetalObjectsEXT == NULL)
    {
        log_error("required Vulkan-Metal interop extensions or procedures are unavailable");
        return -1;
    }
    if (!_metal_external_buffer_supported(buffer))
        return -1;
    if (config->semaphore == NULL || config->semaphore_handle_type != 0)
    {
        log_error("Metal export requires a semaphore and zero native semaphore-handle type");
        return -1;
    }
    if (!config->semaphore->timeline ||
        !config->semaphore->metal_shared_event_exportable ||
        config->semaphore->device != buffer->device ||
        config->semaphore->vk_semaphore == VK_NULL_HANDLE)
    {
        log_error("Metal export requires a live Metal-exportable timeline semaphore");
        return -1;
    }

    uint64_t allocation_size = (uint64_t)dvz_allocation_size(buffer->alloc);
    uint64_t logical_size = (uint64_t)buffer->req_size;
    if (allocation_size == 0 || logical_size == 0 || logical_size > allocation_size)
    {
        log_error("invalid Metal interop buffer allocation or logical size");
        return -1;
    }
    uint64_t offset = config->offset;
    if (offset >= logical_size)
    {
        log_error("Metal interop export offset exceeds the logical buffer");
        return -1;
    }
    uint64_t size = config->size == 0 ? logical_size - offset : config->size;
    if (size == 0 || size > logical_size - offset)
    {
        log_error("Metal interop export range exceeds the logical buffer");
        return -1;
    }

    VkMemoryGetMetalHandleInfoEXT memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT,
        .memory = dvz_allocation_memory(buffer->alloc),
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT,
    };
    void* metal_buffer = NULL;
    VkResult result = vkGetMemoryMetalHandleEXT(
        dvz_device_handle(buffer->device), &memory_info, &metal_buffer);
    if (result != VK_SUCCESS || metal_buffer == NULL)
    {
        log_error("vkGetMemoryMetalHandleEXT failed (%d)", result);
        return -1;
    }

    VkExportMetalSharedEventInfoEXT event_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
        .semaphore = config->semaphore->vk_semaphore,
    };
    VkExportMetalObjectsInfoEXT objects_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &event_info,
    };
    vkExportMetalObjectsEXT(dvz_device_handle(buffer->device), &objects_info);
    if (event_info.mtlSharedEvent == NULL)
    {
        log_error("vkExportMetalObjectsEXT did not return a Metal shared event");
        return -1;
    }

    VkPhysicalDeviceIDProperties id = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &id,
    };
    vkGetPhysicalDeviceProperties2(dvz_device_physical_device(buffer->device), &properties);

    out->version = DVZ_INTEROP_METAL_BUFFER_EXPORT_VERSION;
    out->metal_buffer = metal_buffer;
    out->metal_shared_event = event_info.mtlSharedEvent;
    out->allocation_size = allocation_size;
    out->offset = offset;
    out->size = size;
    out->usage = buffer->req_usage;
    out->vk_usage = buffer->req_usage;
    out->drp2_usage = config->drp2_usage;
    out->flags = config->export_flags;
    out->allocation_flags = dvz_allocation_flags(buffer->alloc);
    out->device_uuid_valid = 1;
    dvz_memcpy(out->device_uuid, sizeof(out->device_uuid), id.deviceUUID, VK_UUID_SIZE);
    out->semaphore_value = config->semaphore_value;
    return 0;
#else
    (void)buffer;
    (void)config;
    return -1;
#endif
}



/**
 * Record one external-to-Vulkan ownership acquire.
 *
 * @param cmds recording command wrapper
 * @param buffer live exported buffer
 * @param offset byte offset
 * @param size byte size
 * @param consumer first Vulkan consumer
 * @return true when recorded
 */
bool dvz_interop_metal_buffer_record_acquire(
    DvzCommands* cmds, DvzBuffer* buffer, uint64_t offset, uint64_t size,
    DvzInteropBufferConsumer consumer)
{
#if DVZ_INTEROP_METAL_RUNTIME
    uint32_t queue_family = 0;
    if (!_metal_record_validate(cmds, buffer, offset, size, &queue_family))
        return false;

    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkBufferUsageFlags usage = 0;
    if (!_metal_consumer_sync(consumer, false, &stage, &access, &usage))
        return false;
    if ((buffer->req_usage & usage) == 0)
    {
        log_error("Metal interop buffer lacks usage required by the Vulkan consumer");
        return false;
    }

    DvzBarriers barriers = {0};
    dvz_barriers(&barriers);
    DvzBarrierBuffer* barrier = dvz_barriers_buffer(
        &barriers, buffer->vk_buffer, (VkDeviceSize)offset, (VkDeviceSize)size);
    if (barrier == NULL)
        return false;
    dvz_barrier_buffer_stage(barrier, VK_PIPELINE_STAGE_2_NONE, stage);
    dvz_barrier_buffer_access(barrier, VK_ACCESS_2_NONE, access);
    dvz_barrier_buffer_queue(barrier, VK_QUEUE_FAMILY_EXTERNAL, queue_family);
    dvz_cmd_barriers(cmds, &barriers);
    return true;
#else
    (void)cmds;
    (void)buffer;
    (void)offset;
    (void)size;
    (void)consumer;
    return false;
#endif
}



/**
 * Record one Vulkan-to-external ownership release.
 *
 * @param cmds recording command wrapper
 * @param buffer live exported buffer
 * @param offset byte offset
 * @param size byte size
 * @param consumer last Vulkan consumer, or NONE for initial release
 * @return true when recorded
 */
bool dvz_interop_metal_buffer_record_release(
    DvzCommands* cmds, DvzBuffer* buffer, uint64_t offset, uint64_t size,
    DvzInteropBufferConsumer consumer)
{
#if DVZ_INTEROP_METAL_RUNTIME
    uint32_t queue_family = 0;
    if (!_metal_record_validate(cmds, buffer, offset, size, &queue_family))
        return false;

    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkBufferUsageFlags usage = 0;
    if (!_metal_consumer_sync(consumer, true, &stage, &access, &usage))
        return false;
    if (usage != 0 && (buffer->req_usage & usage) == 0)
    {
        log_error("Metal interop buffer lacks usage required by the Vulkan consumer");
        return false;
    }

    DvzBarriers barriers = {0};
    dvz_barriers(&barriers);
    DvzBarrierBuffer* barrier = dvz_barriers_buffer(
        &barriers, buffer->vk_buffer, (VkDeviceSize)offset, (VkDeviceSize)size);
    if (barrier == NULL)
        return false;
    dvz_barrier_buffer_stage(barrier, stage, VK_PIPELINE_STAGE_2_NONE);
    dvz_barrier_buffer_access(barrier, access, VK_ACCESS_2_NONE);
    dvz_barrier_buffer_queue(barrier, queue_family, VK_QUEUE_FAMILY_EXTERNAL);
    dvz_cmd_barriers(cmds, &barriers);
    return true;
#else
    (void)cmds;
    (void)buffer;
    (void)offset;
    (void)size;
    (void)consumer;
    return false;
#endif
}
