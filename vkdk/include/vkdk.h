// vkdk: the Vulkan subset bo2-360-nx uses, on deko3d (Switch only). What the application can ask of it beyond
// Vulkan. Plain C, callable before the instance exists.
//
// Copyright 2026 aks796. GPL-3.0.
#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <string>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// A shader module's code may be a vkdk shader blob instead of SPIR-V (the shader library's deko3d pack):
//   VkdkShaderBlobHeader, then the GLSL text (with its "// vkdk" binding line), NUL-terminated, raw or deflated
//   (zlib), padded to 4 bytes. SPIR-V is accepted only for the built-in shaders vkdk carries converted
//   (vkdk_builtin_shaders.inc).
#define VKDK_SHADER_BLOB_MAGIC 0x4B444B56u  // "VKDK"
#define VKDK_SHADER_BLOB_VERSION 1u
#define VKDK_SHADER_BLOB_ZLIB 1u
typedef struct VkdkShaderBlobHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t stage;         // 0 vertex, 4 fragment, 5 compute (DkStage)
  uint32_t glsl_bytes;    // the text, including the terminating NUL
  uint32_t flags;         // VKDK_SHADER_BLOB_ZLIB: the text is deflated
  uint32_t stored_bytes;  // bytes after the header (deflated or raw)
} VkdkShaderBlobHeader;

// Where log lines go (default: stderr).
void vkdkSetLogCallback(void (*callback)(const char* line));

// The DKSH variant cache file: read at the device's creation, appended to when uam compiles a variant on the console.
// tools/deko3d/dksh_cache.py builds and merges it on the PC. Call before vkCreateDevice.
void vkdkSetShaderCachePath(const char* path);

// The prebuilt DKSH cache (tools/deko3d/dksh_cache.py + dksh_pack.cpp): variants compiled on the PC, read only, each
// read from the file and inflated the first time it is asked for (only its index stays in memory). Looked up after
// the memory and before the cache file above; a variant found there is not copied into that file. Call before
// vkCreateDevice. Without zstd (VKDK_ZSTD) the file is not read.
//   VkdkPrebuiltHeader, the zstd dictionary (dict_bytes), count VkdkPrebuiltEntry sorted by key, the zstd frames.
#define VKDK_PREBUILT_MAGIC "VKDKPB1"
typedef struct VkdkPrebuiltHeader {
  char magic[8];        // VKDK_PREBUILT_MAGIC with its NUL
  uint32_t count;       // variants
  uint32_t dict_bytes;  // the dictionary after the header
  uint32_t uam_flags;   // uam's codegen passes on the PC (setBanjoFlags): for the log
  uint32_t reserved[3];
} VkdkPrebuiltHeader;
typedef struct VkdkPrebuiltEntry {
  uint64_t key;     // the variant's key (Hash64 of its text, seeded with the stage)
  uint64_t offset;  // its frame, from the start of the file
  uint32_t stored;  // bytes of the frame
  uint32_t size;    // bytes of the DKSH
} VkdkPrebuiltEntry;
void vkdkSetPrebuiltShaderCachePath(const char* path);

// Copies vkdk does on the 3D pipe instead of the copy engine (vkdk_cmd.cpp): same-format colour copies as draws
// (DrawnCopy) and blocks of a 2D image into a 3D one as a compute pass (VolumeCopy). Both on by default; call before
// vkCreateDevice.
void vkdkSetCopyPaths(bool drawn, bool volume);

// vkdk's one worker thread (8 MB stack: uam and SPIRV-Cross) starts at its first use; call this at program start so
// it starts while the heap still has room (an emulator's guest memory leaves no 8 MB block later).
void vkdkStartWorker(void);

// The first index of a descriptor set's image (or sampler) descriptors in the device's tables: a shader that makes its
// own texture handles (the game's descriptor heaps) adds it to the index it reads. UINT32_MAX for an unknown set.
uint32_t vkdkDescriptorSetImageBase(uint64_t descriptor_set);
uint32_t vkdkDescriptorSetSamplerBase(uint64_t descriptor_set);

// Run before a surface's size is read or a swapchain made (the application restores its window's size there: libnx
// forgets it when the last swapchain goes).
void vkdkSetWindowPrepare(void (*fn)(void));

// The next presented image, written to path as a 32-bit BMP (debug captures; the queue waits for it).
void vkdkCaptureNext(const char* path);

// Debug: count the draws of the next `frames` presented frames by render pass, then log them (target size, colour
// targets, depth-only passes such as shadow maps).
void vkdkDrawCensus(int frames);

// The size the application's window has now: what vkGetPhysicalDeviceSurfaceCapabilitiesKHR reports as the
// surface's extent (0x0: the NWindow's own). For a docked/handheld change, when the window still carries the old
// swapchain's size.
void vkdkSetSurfaceSize(uint32_t w, uint32_t h);

// Called after each present of a swapchain's image (the first swapchain of a vkQueuePresentKHR), on the presenting
// thread, with the swapchain's size: the console overlays' frame counter (SaltyNX) and the like.
void vkdkSetPresentHook(void (*fn)(uint32_t width, uint32_t height));

typedef struct VkdkStats {
  uint64_t draws, dispatches, pipeline_binds, state_binds, texture_binds, ubo_binds, barriers, submits;
  uint64_t variants_compiled, variants_loaded, variant_compile_ns;
  uint64_t code_bytes, command_bytes;
  uint64_t fence_wait_ns;   // time the application waited in vkWaitForFences (for the GPU, mostly)
  uint64_t acquire_ns;      // time in vkAcquireNextImageKHR (for the compositor to give a buffer back)
} VkdkStats;
// Counters since the last call (they are reset).
void vkdkTakeStats(VkdkStats* out);
// All the time waited in vkWaitForFences so far (never reset): a GPU that cannot keep up shows as this growing.
uint64_t vkdkFenceWaitTotalNs(void);
// the GPU's busy time on the queue's submissions so far, ns (timestamps around each one's work)
uint64_t vkdkGpuBusyTotalNs(void);
// Device memory in the blocks made and not yet destroyed (bytes).
uint64_t vkdkDeviceMemoryBytes(void);
// Called when a block cannot be made even after the blocks waiting to go were let go: the application gives back
// what it can (caches), and the block is tried once more.
void vkdkSetLowMemoryHook(void (*fn)(void));

#ifdef __cplusplus
}

#include <vector>
namespace vkdk {
// The GLSL of a blob (inflated), or empty if it is not one.
std::string BlobText(const uint32_t* words, size_t count, uint32_t* stage = nullptr);
// A blob made from GLSL (deflated when that is smaller).
std::vector<uint32_t> MakeBlob(const std::string& glsl, uint32_t stage, bool deflate = true);
// The renderer's SPIR-V rewrites, on a blob: the same shader without its colour writes (a depth-only variant;
// removed = colour outputs dropped, empty result when none), and with early fragment tests (empty with the reason when
// the shader writes depth or declares them already).
std::vector<uint32_t> BlobPruneColorWrites(const std::vector<uint32_t>& blob, uint32_t& removed);
std::vector<uint32_t> BlobWithTestsEarly(const std::vector<uint32_t>& blob, const char*& reason);
// The text holds this 32-bit constant (the shadow-min marker of shader_common.h).
bool BlobHasConstant(const std::vector<uint32_t>& blob, uint32_t value);
inline bool IsBlob(const uint32_t* words, size_t count) { return count >= 6 && words[0] == VKDK_SHADER_BLOB_MAGIC; }
}  // namespace vkdk
#endif
