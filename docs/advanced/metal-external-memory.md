# Share Datoviz buffers with Metal

Status: advanced and experimental. Runtime support is limited to Apple Silicon and depends on Vulkan driver support for the required Metal external-object extensions. Intel macOS is compile-only.

## Scope

The supported direction is a Datoviz/Vulkan-owned buffer exported as a borrowed `MTLBuffer`. A Vulkan-owned timeline semaphore is exported as a borrowed `MTLSharedEvent`. Metal may perform GPU work on the shared buffer between explicit Vulkan ownership release and acquire operations.

```text
Vulkan owns a dedicated buffer and timeline semaphore
  -> Vulkan releases ownership and signals value 1
  -> Metal waits for 1, performs GPU work, and signals 2
  -> Vulkan waits for 2 and acquires ownership for its declared consumer
  -> Vulkan may release again and signal the next monotonically increasing value
```

This is buffer sharing only. Metal-owned imports, shared textures, IOSurface, heaps, VideoToolbox, IPC, Python/MPS APIs, and CPU-copy fallbacks are not supported.

## Opt in from C

Include the experimental header explicitly:

```c
#include "datoviz/vk/metal_interop.h"
```

The header is installed but deliberately absent from `datoviz/vk.h` and the Python binding inputs. Its API may change while the feature remains experimental.

Create the GPU context with the exact Metal buffer handle type, create a dedicated Vulkan buffer, and create the exportable timeline semaphore:

```c
DvzGpuCtx* gpu = dvz_interop_gpu_ctx(
    gpu_index, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT);

dvz_buffer(device, allocator, buffer);
dvz_buffer_size(buffer, byte_size);
dvz_buffer_usage(buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
dvz_buffer_flags(buffer, DVZ_ALLOC_DEDICATED_MEMORY);
dvz_buffer_create(buffer);

dvz_interop_metal_semaphore_timeline(device, 0, semaphore);
```

Set `DvzInteropBufferExportConfig.semaphore` to that semaphore and leave `semaphore_handle_type` equal to zero. `dvz_interop_metal_buffer_export_from_buffer()` returns `DvzInteropMetalBufferExport`, including borrowed Metal pointers, allocation/range metadata, usage, device UUID, flags, and the associated timeline value.

Only a dedicated allocation at offset zero is accepted. The allocator must use exactly `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`, and the logical export range must fit the live Vulkan buffer.

The external allocator is buffer-only because VMA applies its external-memory policy allocator-wide. Create a second allocator with external handle type zero when the same device also needs ordinary Vulkan images, as the DRP2 lab does.

## Record the handoff

Use `dvz_interop_metal_buffer_record_release()` inside a recording Vulkan command buffer, then submit that command buffer with a timeline signal. The first release may use `DVZ_INTEROP_BUFFER_CONSUMER_NONE`; later releases must name the preceding Vulkan use.

After Metal waits, performs GPU work, and signals the next value, use `dvz_interop_metal_buffer_record_acquire()` and submit it with a wait on that value. The consumer argument selects the Vulkan destination synchronization scope. The current consumers are vertex-attribute read and transfer read.

The record helpers do not allocate, submit, or wait. Timeline semaphore ordering does not replace the explicit `VK_QUEUE_FAMILY_EXTERNAL` ownership transfers, and the ownership transfers do not replace timeline ordering.

## Borrowed Metal objects

Cast `metal_buffer` to `id<MTLBuffer>` and `metal_shared_event` to `id<MTLSharedEvent>` in Objective-C or Objective-C++. Both objects are borrowed from their Vulkan owners. Do not release them, and keep the Vulkan buffer, memory, semaphore, and device alive until every queued Metal access has completed.

Check that the exported buffer length covers `allocation_size` and that its device matches an enumerated Metal device. Some Vulkan-on-Metal implementations expose the shared event through a proxy device without a stable registry identity, so successful timeline waits and signals are the authoritative shared-event compatibility check. Use the Vulkan device UUID metadata for additional device identity checks where the application has a matching Metal identity source.

## Availability and failures

The context requires `VK_EXT_external_memory_metal`, `VK_EXT_metal_objects`, `vkGetMemoryMetalHandleEXT()`, and `vkExportMetalObjectsEXT()`. The exact buffer flags and usage must also be exportable and compatible with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`.

MoltenVK 1.4.1 has a known exported-buffer/backing-heap desynchronization that was fixed in 1.4.2. Datoviz rejects the affected default configuration; applications that cannot update MoltenVK may set `MVK_CONFIG_USE_MTLHEAP=0` before any Vulkan initialization. The non-public lab applies that setting without overriding an explicit caller value.

Treat a null context or any nonzero export result as unavailable interop. The API never falls back to a CPU copy. Outside Apple Silicon, the functions remain linkable but return unavailable results, and export failures leave the descriptor zeroed.

## Non-public proof

`examples/c/lab/metal_external_buffer.c` and its narrow Objective-C shim demonstrate 100 asynchronous Vulkan↔Metal timeline cycles. Metal waits, blits fullscreen-triangle vertices, and signals; Vulkan waits, acquires for vertex input, and finally renders through DRP2 with red-pixel readback verification.

```sh
just example-c lab/metal_external_buffer
```

The lab target is built only on Apple systems with the native vklite runtime available. An explicit `SKIP` means the runtime lacks Metal or one of the required Vulkan capabilities; it never indicates a CPU fallback.

## Durable contract

The exact ownership, capability, lifetime, and synchronization rules are specified in [`spec/architecture/VULKAN_METAL_BUFFER_INTEROP.md`](https://github.com/datoviz/datoviz/blob/main/spec/architecture/VULKAN_METAL_BUFFER_INTEROP.md).
