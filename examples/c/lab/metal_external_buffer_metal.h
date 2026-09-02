/*
 * Copyright (c) 2021 Cyrille Rossant and contributors. All rights reserved.
 * Licensed under the MIT license. See LICENSE file in the project root for details.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



typedef struct DvzMetalInteropLab DvzMetalInteropLab;



int dvz_metal_interop_lab_create(
    void* metal_buffer, void* metal_shared_event, uint64_t allocation_size,
    DvzMetalInteropLab** out_lab);



bool dvz_metal_interop_lab_enqueue_blit(
    DvzMetalInteropLab* lab, const void* bytes, size_t size, uint64_t wait_value,
    uint64_t signal_value);



bool dvz_metal_interop_lab_finish_and_verify(
    DvzMetalInteropLab* lab, const void* expected, size_t size, uint64_t signal_value);



void dvz_metal_interop_lab_destroy(DvzMetalInteropLab* lab);
