#!/bin/sh
# vkdk (vkdk/: Vulkan on deko3d) for the NRO: build_vkdk/libvkdk.a, libvkdk_spirv_cross.a, uam/libuamlib.a.
# The C++ flags must keep the Switch platform's -mtp=soft: uam's thread_local cap read TPIDR_EL0 without it, which
# on the port's threads is a small fake TLS block (pthr.c) -- every shader compile then wrote past it into the heap.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
cmake -S "$HERE/vkdk" -B "$HERE/build_vkdk" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__"
ninja -C "$HERE/build_vkdk" vkdk
