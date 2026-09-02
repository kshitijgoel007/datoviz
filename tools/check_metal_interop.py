#!/usr/bin/env python3
"""Check the experimental Vulkan-Metal interop boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "src" / "vklite" / "metal_interop.c"
UMBRELLA = ROOT / "include" / "datoviz" / "vk.h"
BINDINGS = ROOT / "spec" / "bindings" / "ctypes.yml"


def function_body(source: str, name: str) -> str:
    """Return one C function body using balanced braces."""
    start = source.index(f"{name}(")
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise SystemExit(f"unterminated function body: {name}")


def main() -> int:
    source = CORE.read_text(encoding="utf-8")
    forbidden_calls = (
        "dvz_commands(",
        "dvz_device_wait(",
        "dvz_queue_wait(",
        "dvz_semaphore_wait(",
        "dvz_submit_send(",
        "vkDeviceWaitIdle(",
        "vkQueueSubmit(",
        "vkQueueSubmit2(",
        "vkQueueWaitIdle(",
        "vkWaitSemaphores(",
    )
    found = [call for call in forbidden_calls if call in source]
    if found:
        raise SystemExit(f"Metal interop core must remain record-only; found: {', '.join(found)}")

    record_forbidden = (
        "calloc(",
        "malloc(",
        "dvz_commands(",
        "dvz_submit(",
        "dvz_submit_create_wrapper(",
        "dvz_submit_send(",
        "vkAllocateCommandBuffers(",
        "vkQueueSubmit(",
        "vkQueueSubmit2(",
    )
    for name in (
        "dvz_interop_metal_buffer_record_acquire",
        "dvz_interop_metal_buffer_record_release",
    ):
        body = function_body(source, name)
        found = [call for call in record_forbidden if call in body]
        if found:
            raise SystemExit(f"{name} must only record; found: {', '.join(found)}")

    if "metal_interop.h" in UMBRELLA.read_text(encoding="utf-8"):
        raise SystemExit("experimental Metal interop header must not enter datoviz/vk.h")
    if "metal_interop.h" in BINDINGS.read_text(encoding="utf-8"):
        raise SystemExit("experimental Metal interop header must not enter ctypes inputs")

    print("Metal interop boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
