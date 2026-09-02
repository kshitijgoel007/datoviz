# Vulkan–Metal Buffer Interop

Status: experimental v0.4 contract. Runtime support is limited to Apple Silicon. Intel macOS is compile-only and every other platform exposes unavailable stubs.

## Boundary

Datoviz supports one direction: Vulkan owns a buffer allocation and timeline semaphore, exports borrowed Metal objects, and later destroys the Vulkan owners after all Vulkan and Metal work has finished. Metal-owned imports, textures, IOSurface, heaps, VideoToolbox, IPC, Python/MPS bindings, and a general asynchronous DRP2 scheduler are outside this contract.

The public experimental declarations live in `include/datoviz/vk/metal_interop.h`. That header is installed but intentionally excluded from `include/datoviz/vk.h` and every Python binding input. Applications must opt in with an explicit include.

## Capability contract

`dvz_interop_gpu_ctx()` and `dvz_interop_gpu_ctx_ex()` recognize only the exact `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT` value for this path. They request `VK_EXT_external_memory_metal` and `VK_EXT_metal_objects`, do not request Unix FD semaphore extensions, and reject the runtime when either extension or either required procedure is unavailable.

MoltenVK 1.4.1 is rejected in its default heap mode because its exported buffer can become desynchronized from the backing Metal heap. MoltenVK 1.4.2 or newer is accepted; older MoltenVK is accepted only when `MVK_CONFIG_USE_MTLHEAP=0` was set before Vulkan initialization.

Before allocation and again before export, Datoviz queries `vkGetPhysicalDeviceExternalBufferProperties()` with the exact Vulkan buffer flags and usage. The result must advertise `VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT`, and `compatibleHandleTypes` must contain `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`.

An exportable buffer must use a Vulkan allocator configured for exactly `VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT`, must request `DVZ_ALLOC_DEDICATED_MEMORY`, and must have allocation offset zero. The logical export range must be non-empty and contained in the requested Vulkan buffer size. VMA suballocations and legacy Metal-object buffer-export fallbacks are not supported.

An allocator configured for MTLBUFFER export accepts buffer creation only and rejects image creation because VMA applies its external-memory allocation policy to the whole allocator. Code that also needs ordinary Vulkan images, including a DRP2 runtime, must use a second non-external allocator on the same device. This contract does not expose Metal textures.

The semaphore must be a timeline semaphore created by `dvz_interop_metal_semaphore_timeline()`. Its creation chain declares `VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT`. `DvzInteropBufferExportConfig.semaphore_handle_type` must be zero because no native semaphore handle is exported.

## Exported descriptor and lifetime

`dvz_interop_metal_buffer_export_from_buffer()` obtains the buffer with `vkGetMemoryMetalHandleEXT()` and the shared event with `vkExportMetalObjectsEXT()`. A successful `DvzInteropMetalBufferExport` reports its ABI version, borrowed `MTLBuffer` and `MTLSharedEvent` pointers, exact allocation and logical range metadata, Vulkan and DRP2 usage metadata, allocation flags, device UUID, and timeline value.

The pointers are borrowed Objective-C objects. Callers must not retain ownership by releasing them, and must stop every Metal access before destroying the owning Vulkan memory, semaphore, or device. The Vulkan allocation and semaphore remain authoritative for lifetime.

Every failure returns an unavailable result and leaves the output descriptor entirely zeroed. Missing Metal, extensions, procedures, or exact capabilities never selects a CPU copy fallback.

## Ownership and synchronization

Metal access and Vulkan access are mutually exclusive for the exported range. Timeline values establish execution order, while queue-family ownership barriers establish Vulkan's external ownership transitions; both are required.

`dvz_interop_metal_buffer_record_release()` records a main-queue-family to `VK_QUEUE_FAMILY_EXTERNAL` release. `DVZ_INTEROP_BUFFER_CONSUMER_NONE` is valid only for the initial release before Vulkan has accessed the buffer. Later releases name the preceding Vulkan consumer so the source stage and access mask match the actual use.

`dvz_interop_metal_buffer_record_acquire()` records a `VK_QUEUE_FAMILY_EXTERNAL` to main-queue-family acquire. It requires the next Vulkan consumer and rejects `DVZ_INTEROP_BUFFER_CONSUMER_NONE`.

Both helpers operate on one explicit non-empty range and only record a synchronization2 barrier into an existing command buffer. They do not allocate command buffers, submit work, wait on a semaphore, or wait for queue/device idle. Callers submit the release with a timeline signal and submit the acquire with a timeline wait at a stage compatible with the declared consumer.

The canonical cycle is:

```text
Vulkan release -> signal odd value
Metal wait odd -> GPU work -> signal even value
Vulkan wait even -> acquire -> Vulkan consumer
Vulkan release -> signal next odd value
```

Timeline values must increase monotonically. Host waits are reserved for final verification or safe resource recycling, never the steady-state handoff.

## Lab proof

`examples/c/lab/metal_external_buffer.c` is a non-public Apple-only proof. It performs 100 Vulkan↔Metal cycles, uses a Metal GPU blit to write fullscreen-triangle vertices, acquires the buffer for Vulkan vertex input, renders through the existing DRP2 runtime, verifies a red pixel, checks final bytes and semaphore visibility, repeats teardown coverage, and requires a clean Vulkan validation count.
