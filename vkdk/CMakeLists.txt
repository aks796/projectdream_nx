# vkdk: Vulkan on deko3d (the Switch's own GPU API). From the bo2-360-nx deko3d build (aks796; renut-nx's plume deko3d
# rules, ChanseyIsTheBest, reused with permission); in Dekopon-32 since 2026-11-11, where it also converts SPIR-V on
# the console (src/vkdk_spirv.cpp) and samples texel buffers. In sdt_nx (64-bit, devkitA64 + libnx) it is the
# libvulkan.so of Unity's Android player: the loader hands Unity vkGetInstanceProcAddr.
#   cmake -S vkdk -B build_vkdk -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.20)
project(vkdk C CXX)

set(UAM_PARENT_IS_VKDK ON)
add_subdirectory(uam)

# SPIRV-Cross (Khronos, Apache-2.0; the copy Vita3K-32 builds 32-bit, d8e3e2b): core + GLSL backend, compiled here
# with Dekopon-32's flags (libnx32's prebuilt one freed a bad pointer on its first conversion: a different C++
# library build).
add_library(vkdk_spirv_cross STATIC
    spirv_cross/spirv_cross.cpp
    spirv_cross/spirv_parser.cpp
    spirv_cross/spirv_cross_parsed_ir.cpp
    spirv_cross/spirv_cfg.cpp
    spirv_cross/spirv_glsl.cpp)
target_include_directories(vkdk_spirv_cross PUBLIC spirv_cross PRIVATE spirv_cross/spirv_cross)
set_target_properties(vkdk_spirv_cross PROPERTIES CXX_STANDARD 17)
target_compile_options(vkdk_spirv_cross PRIVATE -O2 -w -ffunction-sections -fdata-sections)

add_library(vkdk STATIC
    src/vkdk_android.cpp
    src/vkdk_blob.cpp
    src/vkdk_core.cpp
    src/vkdk_cmd.cpp
    src/vkdk_formats.cpp
    src/vkdk_pipeline.cpp
    src/vkdk_spirv.cpp
    src/vkdk_wsi.cpp)
if(NOT DEFINED VKDK_VULKAN_HEADERS)
    set(VKDK_VULKAN_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/vulkan-headers/include)
endif()
target_include_directories(vkdk PUBLIC include ${VKDK_VULKAN_HEADERS})
# deko3d.h: libnx's; SPIRV-Cross: compiled here
target_include_directories(vkdk PRIVATE src)
target_compile_definitions(vkdk PUBLIC VK_USE_64_BIT_PTR_DEFINES=1)
set_target_properties(vkdk PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_compile_options(vkdk PRIVATE -O2 -Wall -Wno-unused-function -ffunction-sections -fdata-sections)
target_include_directories(vkdk PRIVATE ${DEVKITPRO}/portlibs/switch/include)
target_link_libraries(vkdk PUBLIC uamlib vkdk_spirv_cross deko3d z)
