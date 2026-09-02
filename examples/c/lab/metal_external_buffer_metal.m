/*
 * Copyright (c) 2021 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>
#include <string.h>

#include "metal_external_buffer_metal.h"



@interface DvzMetalInteropLabContext : NSObject

@property(nonatomic, unsafe_unretained) id<MTLBuffer> sharedBuffer;
@property(nonatomic, unsafe_unretained) id<MTLSharedEvent> sharedEvent;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) id<MTLCommandBuffer> lastCommandBuffer;

@end



@implementation DvzMetalInteropLabContext
@end



/**
 * Create the Objective-C Metal side of the interop proof.
 *
 * @param metal_buffer borrowed MTLBuffer
 * @param metal_shared_event borrowed MTLSharedEvent
 * @param allocation_size exact Vulkan allocation size
 * @param[out] out_lab owned shim context
 * @return 0 on success, -1 on validation failure
 */
int dvz_metal_interop_lab_create(
    void* metal_buffer, void* metal_shared_event, uint64_t allocation_size,
    DvzMetalInteropLab** out_lab)
{
    if (metal_buffer == NULL || metal_shared_event == NULL || allocation_size == 0 ||
        out_lab == NULL)
    {
        return -1;
    }
    *out_lab = NULL;

    @autoreleasepool
    {
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)metal_buffer;
        id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)metal_shared_event;
        id<MTLDevice> device = buffer.device;
        if (device == nil || buffer.length < allocation_size)
        {
            fprintf(
                stderr,
                "Metal object validation failed (length=%llu minimum=%llu, buffer device=%s)\n",
                (unsigned long long)buffer.length, (unsigned long long)allocation_size,
                device != nil ? "yes" : "no");
            return -1;
        }

        bool enumerated_device = false;
        for (id<MTLDevice> candidate in MTLCopyAllDevices())
        {
            if (candidate.registryID == device.registryID)
            {
                enumerated_device = true;
                break;
            }
        }
        if (!enumerated_device)
        {
            fprintf(stderr, "exported MTLBuffer device is not an enumerated Metal device\n");
            return -1;
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (queue == nil)
        {
            fprintf(stderr, "unable to create Metal interop command queue\n");
            return -1;
        }

        DvzMetalInteropLabContext* context = [[DvzMetalInteropLabContext alloc] init];
        context.sharedBuffer = buffer;
        context.sharedEvent = event;
        context.device = device;
        context.commandQueue = queue;
        *out_lab = (__bridge_retained DvzMetalInteropLab*)context;
        return 0;
    }
}



/**
 * Enqueue one GPU-only Metal wait, blit, and signal sequence.
 *
 * @param lab shim context
 * @param bytes source bytes copied into a Metal-owned source buffer
 * @param size byte count
 * @param wait_value shared-event value waited by Metal
 * @param signal_value shared-event value signaled by Metal
 * @return true when the command buffer was committed
 */
bool dvz_metal_interop_lab_enqueue_blit(
    DvzMetalInteropLab* lab, const void* bytes, size_t size, uint64_t wait_value,
    uint64_t signal_value)
{
    if (lab == NULL || bytes == NULL || size == 0 || signal_value <= wait_value)
        return false;

    @autoreleasepool
    {
        DvzMetalInteropLabContext* context = (__bridge DvzMetalInteropLabContext*)lab;
        if (size > context.sharedBuffer.length)
            return false;

        id<MTLBuffer> source = [context.device newBufferWithBytes:bytes
                                                           length:size
                                                          options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [context.commandQueue commandBuffer];
        if (source == nil || command == nil)
            return false;

        [command encodeWaitForEvent:context.sharedEvent value:wait_value];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (blit == nil)
            return false;
        [blit copyFromBuffer:source
                sourceOffset:0
                    toBuffer:context.sharedBuffer
           destinationOffset:0
                        size:size];
        [blit endEncoding];
        [command encodeSignalEvent:context.sharedEvent value:signal_value];
        [command commit];
        context.lastCommandBuffer = command;
        return true;
    }
}



/**
 * Perform the one allowed final host wait and verify shared bytes and event visibility.
 *
 * @param lab shim context
 * @param expected expected final bytes
 * @param size byte count
 * @param signal_value final Metal signal value
 * @return true when final Metal execution and bytes are correct
 */
bool dvz_metal_interop_lab_finish_and_verify(
    DvzMetalInteropLab* lab, const void* expected, size_t size, uint64_t signal_value)
{
    if (lab == NULL || expected == NULL || size == 0)
        return false;

    @autoreleasepool
    {
        DvzMetalInteropLabContext* context = (__bridge DvzMetalInteropLabContext*)lab;
        [context.lastCommandBuffer waitUntilCompleted];
        if (context.lastCommandBuffer.status != MTLCommandBufferStatusCompleted ||
            context.lastCommandBuffer.error != nil)
        {
            const char* description =
                context.lastCommandBuffer.error.localizedDescription.UTF8String;
            if (description == NULL)
                description = "unknown error";
            fprintf(
                stderr, "Metal interop command failed: %s\n", description);
            return false;
        }
        if (context.sharedEvent.signaledValue < signal_value)
        {
            fprintf(
                stderr, "Metal shared event reached %llu, expected at least %llu\n",
                (unsigned long long)context.sharedEvent.signaledValue,
                (unsigned long long)signal_value);
            return false;
        }
        void* contents = context.sharedBuffer.contents;
        if (contents == NULL)
        {
            fprintf(stderr, "exported MTLBuffer is not host-visible for final verification\n");
            return false;
        }
        return memcmp(contents, expected, size) == 0;
    }
}



/**
 * Destroy the Objective-C shim without releasing borrowed Vulkan-owned Metal objects.
 *
 * @param lab shim context
 */
void dvz_metal_interop_lab_destroy(DvzMetalInteropLab* lab)
{
    if (lab == NULL)
        return;
    @autoreleasepool
    {
        DvzMetalInteropLabContext* context =
            (__bridge_transfer DvzMetalInteropLabContext*)lab;
        context.sharedBuffer = nil;
        context.sharedEvent = nil;
        context.lastCommandBuffer = nil;
        context.commandQueue = nil;
        context.device = nil;
    }
}
