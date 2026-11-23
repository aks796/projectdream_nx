// vkdk: instance, physical device, device and queue, memory, buffers, images, views, samplers, fences, semaphores,
// query pools and descriptors. Copyright 2026 aks796. GPL-3.0.
//
// deko3d facts used here (renut-nx's docs/deko3d-renderer.md lists where each was found): a texture handle is the image
// id | sampler id << 20 in the bound descriptor tables; descriptors are 32 bytes the CPU writes; dkMemBlockCreate takes
// its storage from the C heap and aborts the process when that fails; dkImageLayoutInitialize's tile height bug.
#include <mutex>
#include <vector>
#include <unordered_map>
#include "vkdk_internal.h"

#include <unordered_set>

#include <malloc.h>

#include <cerrno>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace vkdk {

static void (*g_log)(const char*) = nullptr;

void Log(const char* fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (g_log) {
    g_log(line);
  } else {
    fprintf(stderr, "[vkdk] %s\n", line);
  }
}

uint64_t Hash64(const void* data, size_t size, uint64_t seed) {
  // FNV-1a 64 (the variant keys; speed does not matter there).
  uint64_t h = 0xcbf29ce484222325ull ^ seed;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

uint32_t RangeAllocator::Allocate(uint32_t count) {
  if (!count) {
    return 0;
  }
  for (size_t i = 0; i < free_.size(); ++i) {
    Range& r = free_[i];
    if (r.count >= count) {
      const uint32_t first = r.first;
      r.first += count;
      r.count -= count;
      if (!r.count) {
        free_.erase(free_.begin() + i);
      }
      return first;
    }
  }
  return UINT32_MAX;
}

void RangeAllocator::Free(uint32_t first, uint32_t count) {
  if (!count || first == UINT32_MAX) {
    return;
  }
  auto it = std::lower_bound(free_.begin(), free_.end(), first,
                             [](const Range& r, uint32_t f) { return r.first < f; });
  it = free_.insert(it, {first, count});
  // Merge with the neighbours.
  if (it + 1 != free_.end() && it->first + it->count == (it + 1)->first) {
    it->count += (it + 1)->count;
    free_.erase(it + 1);
  }
  if (it != free_.begin() && (it - 1)->first + (it - 1)->count == it->first) {
    (it - 1)->count += it->count;
    free_.erase(it);
  }
}

namespace {

VkPhysicalDevice_T g_physical;
std::atomic<VkDevice> g_device{nullptr};

template <typename T>
T* Make() {
  return new (std::nothrow) T();
}

// deko3d 0.5.0's tile height bug (SwitchWakerHD, via renut-nx's plume deko3d): when the tile it picks from 1.5 x the
// height shrinks for level 0, the shrunk tile is given as a custom tile size.
void TileSizeFix(DkImageLayoutMaker& m, uint32_t rows) {
  if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) {
    return;
  }
  if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
      m.type == DkImageType_Buffer) {
    return;
  }
  const uint32_t gobs = (rows + rows / 2 + 7) / 8;
  uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
  uint32_t shrunk = tile;
  while (shrunk && (8u << (shrunk - 1)) >= rows) {
    shrunk--;
  }
  if (shrunk == tile) {
    return;
  }
  m.flags |= DkImageFlags_CustomTileSize;
  m.tileSize = DkTileSize(shrunk);
}

void FillStruct(VkBaseOutStructure* s) {
  // Properties and features chains: the structures bo2 and the SDK ask for, everything else left zero.
  for (; s; s = s->pNext) {
    switch (s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(s);
        f->storageBuffer16BitAccess = VK_FALSE;
        f->shaderDrawParameters = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(s);
        f->descriptorIndexing = VK_TRUE;
        f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f->descriptorBindingPartiallyBound = VK_TRUE;
        f->runtimeDescriptorArray = VK_TRUE;
        f->scalarBlockLayout = VK_TRUE;
        f->hostQueryReset = VK_TRUE;
        f->timelineSemaphore = VK_FALSE;
        f->bufferDeviceAddress = VK_TRUE;
        f->separateDepthStencilLayouts = VK_TRUE;
        f->uniformBufferStandardLayout = VK_TRUE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT: {
        reinterpret_cast<VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT*>(s)->pipelineCreationCacheControl =
            VK_TRUE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(s);
        f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f->descriptorBindingPartiallyBound = VK_TRUE;
        f->runtimeDescriptorArray = VK_TRUE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
        auto* p = reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(s);
        p->maxUpdateAfterBindDescriptorsInAllPools = kImageDescriptors;
        p->maxPerStageDescriptorUpdateAfterBindSampledImages = kImageDescriptors;
        p->maxPerStageDescriptorUpdateAfterBindSamplers = kSamplerDescriptors;
        p->maxDescriptorSetUpdateAfterBindSampledImages = kImageDescriptors;
        p->maxDescriptorSetUpdateAfterBindSamplers = kSamplerDescriptors;
        p->maxPerStageUpdateAfterBindResources = kImageDescriptors;
        p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 16;
        p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 16;
        p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
        p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
        snprintf(p->driverName, sizeof(p->driverName), "vkdk");
        snprintf(p->driverInfo, sizeof(p->driverInfo), "deko3d");
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
        auto* f = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(s);
        f->nullDescriptor = VK_FALSE;
        break;
      }
      default:
        break;
    }
  }
}

}  // namespace

}  // namespace vkdk

using namespace vkdk;

// ---- device helpers

std::atomic<uint64_t> g_blockBytes{0};  // device memory in live blocks (vkdkDeviceMemoryBytes)
static void (*g_lowMemoryHook)(void);    // vkdkSetLowMemoryHook

DkMemBlock VkDevice_T::MakeBlock(uint32_t size, uint32_t flags, const char* what) {
  size = AlignUp(size, DK_MEMBLOCK_ALIGNMENT);
  // Big blocks (render targets, big textures) in whole megabytes: one let go fits the next of about its size (a
  // render target made again a few percent larger -- dynamic resolution -- reuses the old one's heap). sdt_nx round
  // 11: the console's heap, shared with everything, ran out of a 7.5 MB piece late in a session.
  if (size >= (4u << 20)) {
    size = AlignUp(size, 1u << 20);
  }
  // deko3d takes the block's storage from the C heap (aligned_alloc) and aborts the process when that fails: try the
  // same allocation first (renut-nx, "Do not let deko3d abort on a full heap").
  for (int attempt = 0;; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(blockMutex);
      void* probe = memalign(DK_MEMBLOCK_ALIGNMENT, size);
      if (probe) {
        free(probe);
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, dk, size);
        m.flags = flags;
        DkMemBlock b = dkMemBlockCreate(&m);
        if (b) {
          g_blockBytes.fetch_add(size, std::memory_order_relaxed);
        }
        if (attempt) {
          Log("out of memory: %u KB for %s found after %s", size >> 10, what,
              attempt == 1 ? "the blocks waiting to go were let go" : "the port's caches were dropped too");
        }
        return b;
      }
    }
    // Not there: the blocks waiting out their frames go now (the GPU drained first, so none is still in use), then
    // the port's caches (vkdkSetLowMemoryHook), then it fails as before (Unity: "Vulkan - Out of memory").
    if (attempt == 0) {
      VKDK_LOG_LIMITED(16, "out of memory: %u KB for %s -- letting the waiting blocks go and trying again", size >> 10,
                       what);
      dkQueueWaitIdle(queue.dk);
      RetireBlocks(true);
    } else if (attempt == 1 && g_lowMemoryHook) {
      g_lowMemoryHook();
    } else {
      VKDK_LOG_LIMITED(16, "out of memory: %u KB for %s", size >> 10, what);
      return nullptr;
    }
  }
}

extern "C" uint64_t vkdkDeviceMemoryBytes(void) {
  return g_blockBytes.load(std::memory_order_relaxed);
}

extern "C" void vkdkSetLowMemoryHook(void (*fn)(void)) {
  g_lowMemoryHook = fn;
}

// sdt_nx: device memory is heap memory, and a block given back at once was written after it went (Unity's first
// swapchain and render targets, replaced at the start: Ryubing's texture cache wrote their contents back into the
// heap, over the objects living there by then, and work still on the GPU would do the same on the console). A
// block is destroyed only once kRetireFrames frames have been presented since it was let go.
// vkdk_nofree (diagnostics): never destroyed at all.
void VkDevice_T::DestroyBlock(DkMemBlock block) {
  static const bool noFree = DebugSwitch("vkdk_nofree", false);
  if (!block || noFree) {
    return;
  }
  std::lock_guard<std::mutex> lock(blockMutex);
  retired.push_back({block, presents.load(std::memory_order_relaxed)});
}

// Blocks due go a few at a time after the frame's present, outside the lock: a destroy unmaps the block from the
// GPU (renut's ports measured 15-37 ms spikes from bookkeeping like this before the present, and an area change
// lets go of many blocks at once).
void VkDevice_T::RetireBlocks(bool all) {
  constexpr int kMost = 32;  // a frame's, when many are waiting (a load frees a lot, and presents few frames)
  DkMemBlock due[kMost];
  for (;;) {
    int n = 0;
    {
      std::lock_guard<std::mutex> lock(blockMutex);
      const uint64_t now = presents.load(std::memory_order_relaxed);
      const int limit = all || retired.size() > 64 ? kMost : 4;
      while (n < limit && !retired.empty() && (all || retired.front().second + kRetireFrames <= now)) {
        due[n++] = retired.front().first;
        retired.pop_front();
      }
    }
    for (int i = 0; i < n; ++i) {
      g_blockBytes.fetch_sub(dkMemBlockGetSize(due[i]), std::memory_order_relaxed);
      dkMemBlockDestroy(due[i]);
    }
    if (!all || n == 0) {
      return;
    }
  }
}

bool VkDevice_T::LoadDksh(const uint8_t* data, size_t size, DkShader& out) {
  // DKSH: a header (magic "DKSH", header size, control size, code size, ...), the control section, then the code.
  struct Header {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
  };
  if (size < sizeof(Header)) {
    return false;
  }
  Header h;
  memcpy(&h, data, sizeof(h));
  if (h.magic != 0x48534B44u || h.control_sz > size || uint64_t(h.control_sz) + h.code_sz > size) {
    return false;
  }
  std::lock_guard<std::mutex> lock(codeMutex);
  constexpr uint32_t kCodeBlock = 16u << 20;
  const uint32_t need = h.code_sz + DK_SHADER_CODE_UNUSABLE_SIZE;
  if (codeBlocks.empty() || AlignUp(codeBlocks.back().second, DK_SHADER_CODE_ALIGNMENT) + need > kCodeBlock) {
    if (need > kCodeBlock) {
      return false;
    }
    DkMemBlock block =
        MakeBlock(kCodeBlock, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                  "shader code");
    if (!block) {
      return false;
    }
    codeBlocks.push_back({block, 0});
    Log("shader code: block %zu (16 MB)", codeBlocks.size());
  }
  auto& cb = codeBlocks.back();
  const uint32_t at = AlignUp(cb.second, DK_SHADER_CODE_ALIGNMENT);
  memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(cb.first)) + at, data + h.control_sz, h.code_sz);
  DkShaderMaker m;
  dkShaderMakerDefaults(&m, cb.first, at);
  m.control = data;
  m.programId = 0;
  dkShaderInitialize(&out, &m);
  cb.second = at + h.code_sz;
  codeEpoch.fetch_add(1, std::memory_order_release);
  return dkShaderIsValid(&out);
}

// ---- instance and physical device

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*,
                                                     VkInstance* out) {
  VKDK_TRACE("CreateInstance");
  (void)info;
  *out = Make<VkInstance_T>();
  g_physical.instance = *out;
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
  delete instance;
}

static const char* const kInstanceExtensions[] = {"VK_KHR_surface", "VK_NN_vi_surface", "VK_KHR_android_surface",
                                                  "VK_KHR_get_physical_device_properties2"};
// VK_EXT_pipeline_creation_cache_control: a pipeline asked for with FAIL_ON_PIPELINE_COMPILE_REQUIRED comes back
// VK_PIPELINE_COMPILE_REQUIRED unless all its shader variants are in memory or the DKSH file (MakeGraphics), so bo2's
// renderer compiles new ones on a thread of its own and its ring never waits for uam.
static const char* const kDeviceExtensions[] = {"VK_KHR_swapchain", "VK_KHR_dedicated_allocation",
                                                "VK_KHR_get_memory_requirements2", "VK_KHR_bind_memory2",
                                                "VK_KHR_maintenance1", "VK_EXT_pipeline_creation_cache_control"};

template <size_t N>
static VkResult ListExtensions(const char* const (&names)[N], uint32_t* count, VkExtensionProperties* out) {
  if (!out) {
    *count = uint32_t(N);
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, uint32_t(N));
  for (uint32_t i = 0; i < n; ++i) {
    memset(&out[i], 0, sizeof(out[i]));
    snprintf(out[i].extensionName, sizeof(out[i].extensionName), "%s", names[i]);
    out[i].specVersion = 1;
  }
  *count = n;
  return n < N ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char*, uint32_t* count,
                                                                           VkExtensionProperties* out) {
  VKDK_TRACE("EnumerateInstanceExtensionProperties");
  return ListExtensions(kInstanceExtensions, count, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
  VKDK_TRACE("EnumerateInstanceLayerProperties");
  *count = 0;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceVersion(uint32_t* version) {
  VKDK_TRACE("EnumerateInstanceVersion");
  *version = VK_API_VERSION_1_2;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance, uint32_t* count, VkPhysicalDevice* out) {
  VKDK_TRACE("EnumeratePhysicalDevices");
  if (!out) {
    *count = 1;
    return VK_SUCCESS;
  }
  if (*count < 1) {
    return VK_INCOMPLETE;
  }
  out[0] = &g_physical;
  *count = 1;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char*,
                                                                         uint32_t* count, VkExtensionProperties* out) {
  VKDK_TRACE("EnumerateDeviceExtensionProperties");
  return ListExtensions(kDeviceExtensions, count, out);
}

static void FillProperties(VkPhysicalDeviceProperties& p) {
  memset(&p, 0, sizeof(p));
  p.apiVersion = VK_API_VERSION_1_2;
  p.driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
  p.vendorID = 0x10DE;
  p.deviceID = 0x0B1D;  // GM20B (Tegra X1)
  p.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  snprintf(p.deviceName, sizeof(p.deviceName), "vkdk (deko3d, GM20B)");
  VkPhysicalDeviceLimits& l = p.limits;
  l.maxImageDimension1D = 16384;
  l.maxImageDimension2D = 16384;
  l.maxImageDimension3D = 2048;
  l.maxImageDimensionCube = 16384;
  l.maxImageArrayLayers = 2048;
  l.maxTexelBufferElements = 1u << 27;
  l.maxUniformBufferRange = DK_UNIFORM_BUF_MAX_SIZE;
  l.maxStorageBufferRange = 1u << 30;
  l.maxPushConstantsSize = kPushBytes;
  l.maxMemoryAllocationCount = 1u << 20;
  l.maxSamplerAllocationCount = kSamplerDescriptors;
  l.bufferImageGranularity = 1;
  l.maxBoundDescriptorSets = 8;
  l.maxPerStageDescriptorSamplers = kSamplerDescriptors;
  l.maxPerStageDescriptorUniformBuffers = 14;
  l.maxPerStageDescriptorStorageBuffers = DK_NUM_STORAGE_BUFS;
  l.maxPerStageDescriptorSampledImages = kImageDescriptors;
  l.maxPerStageDescriptorStorageImages = DK_NUM_IMAGE_BINDINGS;
  l.maxPerStageResources = kImageDescriptors;
  l.maxDescriptorSetSamplers = kSamplerDescriptors;
  l.maxDescriptorSetUniformBuffers = 64;
  l.maxDescriptorSetUniformBuffersDynamic = 16;
  l.maxDescriptorSetStorageBuffers = 64;
  l.maxDescriptorSetStorageBuffersDynamic = 16;
  l.maxDescriptorSetSampledImages = kImageDescriptors;
  l.maxDescriptorSetStorageImages = 64;
  l.maxVertexInputAttributes = DK_MAX_VERTEX_ATTRIBS;
  l.maxVertexInputBindings = DK_MAX_VERTEX_BUFFERS;
  l.maxVertexInputAttributeOffset = (1u << 14) - 1;
  l.maxVertexInputBindingStride = 2048;
  l.maxVertexOutputComponents = 128;
  l.maxFragmentInputComponents = 128;
  l.maxFragmentOutputAttachments = DK_MAX_RENDER_TARGETS;
  l.maxFragmentDualSrcAttachments = 1;
  l.maxFragmentCombinedOutputResources = 16;
  l.maxComputeSharedMemorySize = 48 * 1024;
  l.maxComputeWorkGroupCount[0] = l.maxComputeWorkGroupCount[1] = l.maxComputeWorkGroupCount[2] = 65535;
  l.maxComputeWorkGroupInvocations = 1024;
  l.maxComputeWorkGroupSize[0] = l.maxComputeWorkGroupSize[1] = 1024;
  l.maxComputeWorkGroupSize[2] = 64;
  l.subPixelPrecisionBits = 8;
  l.subTexelPrecisionBits = 8;
  l.mipmapPrecisionBits = 8;
  l.maxDrawIndexedIndexValue = UINT32_MAX;
  l.maxDrawIndirectCount = 1;
  l.maxSamplerLodBias = 15.0f;
  l.maxSamplerAnisotropy = 16.0f;
  l.maxViewports = DK_NUM_VIEWPORTS;
  l.maxViewportDimensions[0] = l.maxViewportDimensions[1] = 16384;
  l.viewportBoundsRange[0] = -32768.0f;
  l.viewportBoundsRange[1] = 32767.0f;
  l.viewportSubPixelBits = 8;
  l.minMemoryMapAlignment = 64;
  l.minTexelBufferOffsetAlignment = 16;
  l.minUniformBufferOffsetAlignment = DK_UNIFORM_BUF_ALIGNMENT;
  l.minStorageBufferOffsetAlignment = 16;
  l.minTexelOffset = -8;
  l.maxTexelOffset = 7;
  l.minTexelGatherOffset = -32;
  l.maxTexelGatherOffset = 31;
  l.maxFramebufferWidth = l.maxFramebufferHeight = 16384;
  l.maxFramebufferLayers = 2048;
  l.framebufferColorSampleCounts = l.framebufferDepthSampleCounts = l.framebufferStencilSampleCounts =
      l.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l.maxColorAttachments = DK_MAX_RENDER_TARGETS;
  l.sampledImageColorSampleCounts = l.sampledImageIntegerSampleCounts = l.sampledImageDepthSampleCounts =
      l.sampledImageStencilSampleCounts = l.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l.maxSampleMaskWords = 1;
  l.timestampComputeAndGraphics = VK_TRUE;
  // deko3d's timestamps are GPU ticks: dkTimestampToNs = ticks * 625 / 384.
  l.timestampPeriod = 625.0f / 384.0f;
  l.maxClipDistances = 8;
  l.maxCullDistances = 8;
  l.maxCombinedClipAndCullDistances = 8;
  l.discreteQueuePriorities = 2;
  l.pointSizeRange[0] = 1.0f;
  l.pointSizeRange[1] = 2047.0f;
  l.lineWidthRange[0] = 1.0f;
  l.lineWidthRange[1] = 1.0f;
  l.pointSizeGranularity = 0.125f;
  l.lineWidthGranularity = 1.0f;
  l.optimalBufferCopyOffsetAlignment = 256;
  l.optimalBufferCopyRowPitchAlignment = 1;
  l.nonCoherentAtomSize = 64;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
  VKDK_TRACE("GetPhysicalDeviceProperties");
  FillProperties(*p);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2* p) {
  VKDK_TRACE("GetPhysicalDeviceProperties2");
  FillProperties(p->properties);
  FillStruct(reinterpret_cast<VkBaseOutStructure*>(p->pNext));
}

static void FillFeatures(VkPhysicalDeviceFeatures& f) {
  memset(&f, 0, sizeof(f));
  f.robustBufferAccess = VK_FALSE;
  f.fullDrawIndexUint32 = VK_TRUE;
  f.imageCubeArray = VK_TRUE;
  f.independentBlend = VK_TRUE;
  f.geometryShader = VK_FALSE;  // vkdk binds vertex and fragment stages only
  f.tessellationShader = VK_FALSE;
  f.sampleRateShading = VK_FALSE;
  f.dualSrcBlend = VK_TRUE;
  f.logicOp = VK_TRUE;
  f.depthClamp = VK_TRUE;
  f.depthBiasClamp = VK_TRUE;
  f.fillModeNonSolid = VK_TRUE;
  f.depthBounds = VK_TRUE;
  f.samplerAnisotropy = VK_TRUE;
  f.textureCompressionBC = VK_TRUE;
  f.textureCompressionASTC_LDR = VK_TRUE;  // (Project Dream's Android build: ASTC 4x4 textures)
  f.occlusionQueryPrecise = VK_TRUE;
  f.pipelineStatisticsQuery = VK_FALSE;  // one counter only (vkdk_cmd.cpp QueryCounter): not advertised
  f.vertexPipelineStoresAndAtomics = VK_TRUE;
  f.fragmentStoresAndAtomics = VK_TRUE;
  f.shaderImageGatherExtended = VK_TRUE;
  f.shaderStorageImageExtendedFormats = VK_TRUE;
  // Round o diagnostics (sdmc:/switch/projectdream_nx/vkdk_noclip): no clip or cull distances, so Azahar's shaders write
  // none (its PICA z <= 0 plane is also the clip volume's near plane; only the user clip plane is lost).
  static const bool noClip = DebugSwitch("vkdk_noclip", false);
  f.shaderClipDistance = noClip ? VK_FALSE : VK_TRUE;
  f.shaderCullDistance = noClip ? VK_FALSE : VK_TRUE;
  f.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* f) {
  VKDK_TRACE("GetPhysicalDeviceFeatures");
  FillFeatures(*f);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* f) {
  VKDK_TRACE("GetPhysicalDeviceFeatures2");
  FillFeatures(f->features);
  FillStruct(reinterpret_cast<VkBaseOutStructure*>(f->pNext));
}

static uint32_t TexelBytes(VkFormat f);

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format,
                                                                   VkFormatProperties* p) {
  VKDK_TRACE("GetPhysicalDeviceFormatProperties");
  p->optimalTilingFeatures = FormatFeatures(format, true);
  p->linearTilingFeatures = 0;
  DkVtxAttribSize s;
  DkVtxAttribType t;
  bool bgra;
  p->bufferFeatures = VertexFormat(format, s, t, bgra) ? VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0;
  if (TexelBytes(format) && ToDkFormat(format) != DkImageFormat_None) {
    p->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;  // CreateBufferView
  }
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                                                    VkFormatProperties2* p) {
  VKDK_TRACE("GetPhysicalDeviceFormatProperties2");
  GetPhysicalDeviceFormatProperties(pd, format, &p->formatProperties);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count,
                                                                        VkQueueFamilyProperties* out) {
  VKDK_TRACE("GetPhysicalDeviceQueueFamilyProperties");
  if (!out) {
    *count = 1;
    return;
  }
  if (*count < 1) {
    return;
  }
  out[0] = {};
  out[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
  out[0].queueCount = 1;
  out[0].timestampValidBits = 64;
  out[0].minImageTransferGranularity = {1, 1, 1};
  *count = 1;
}

static void FillMemoryProperties(VkPhysicalDeviceMemoryProperties& m) {
  memset(&m, 0, sizeof(m));
  m.memoryHeapCount = 1;
  // The heap is the process's memory: what is left of it after the game is far less, but nothing in bo2 sizes itself by
  // this figure beyond a budget report.
  m.memoryHeaps[0].size = 3ull << 30;
  m.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
  m.memoryTypeCount = kMemTypeCount;
  m.memoryTypes[kMemImage].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  m.memoryTypes[kMemDevice].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  m.memoryTypes[kMemUpload].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  m.memoryTypes[kMemReadback].propertyFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                                                   VkPhysicalDeviceMemoryProperties* m) {
  VKDK_TRACE("GetPhysicalDeviceMemoryProperties");
  FillMemoryProperties(*m);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice,
                                                                    VkPhysicalDeviceMemoryProperties2* m) {
  VKDK_TRACE("GetPhysicalDeviceMemoryProperties2");
  FillMemoryProperties(m->memoryProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat format,
                                                                            VkImageType, VkImageTiling tiling,
                                                                            VkImageUsageFlags, VkImageCreateFlags,
                                                                            VkImageFormatProperties* p) {
  VKDK_TRACE("GetPhysicalDeviceImageFormatProperties");
  if (ToDkFormat(format) == DkImageFormat_None || tiling != VK_IMAGE_TILING_OPTIMAL) {
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  p->maxExtent = {16384, 16384, 2048};
  p->maxMipLevels = 15;
  p->maxArrayLayers = 2048;
  p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  p->maxResourceSize = 1ull << 32;
  return VK_SUCCESS;
}

// ---- device

static void DebugCallback(void*, const char* context, DkResult result, const char* message) {
  Log("deko3d: %s (%s, result %d)", message ? message : "", context ? context : "", int(result));
}

// deko3d's memory comes from the heap and goes back to it through these. A CpuUncached block is made uncached by
// libnx's nvMapCreate and cached again by nvMapClose, which ignores a failure of that svcSetMemoryAttribute: such a
// block went back to the heap still uncached (Dekopon-32 in Ryubing after a game switch: a thread stack malloc'd there
// could not be mapped, svcMapMemory InvalidCurrentMemory; on the console, data landing there would just be slow).
// So a freed block is checked and made cached here, or kept out of the heap and tried again at later frees.
namespace {
std::mutex g_memMutex;
std::unordered_map<void*, size_t> g_memSizes;
std::vector<std::pair<void*, size_t>> g_memKept;  // still uncached when freed

bool MakeCached(void* p, size_t size) {
  MemoryInfo mi;
  u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, reinterpret_cast<uintptr_t>(p))) || !(mi.attr & MemAttr_IsUncached)) {
    return true;
  }
  for (int i = 0; i < 4; ++i) {
    if (R_SUCCEEDED(svcSetMemoryAttribute(p, size, MemAttr_IsUncached, 0))) {
      return true;
    }
    svcSleepThread(1000000);
  }
  return false;
}

DkResult MemAlloc(void*, size_t alignment, size_t size, void** out) {
  size = (size + alignment - 1) & ~(alignment - 1);
  *out = aligned_alloc(alignment, size);
  if (!*out) {
    return DkResult_OutOfMemory;
  }
  std::lock_guard<std::mutex> lock(g_memMutex);
  g_memSizes[*out] = size;
  return DkResult_Success;
}

void MemFree(void*, void* mem) {
  if (!mem) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_memMutex);
  const auto it = g_memSizes.find(mem);
  const size_t size = it != g_memSizes.end() ? it->second : 0;
  if (it != g_memSizes.end()) {
    g_memSizes.erase(it);
  }
  if (!size || MakeCached(mem, size)) {
    free(mem);
  } else {
    g_memKept.emplace_back(mem, size);
    VKDK_LOG_LIMITED(8, "memory: a freed block (%u KB at %p) stayed uncached: kept out of the heap (%u kept)",
                     unsigned(size >> 10), mem, unsigned(g_memKept.size()));
  }
  for (size_t i = 0; i < g_memKept.size();) {
    if (MakeCached(g_memKept[i].first, g_memKept[i].second)) {
      free(g_memKept[i].first);
      g_memKept[i] = g_memKept.back();
      g_memKept.pop_back();
    } else {
      ++i;
    }
  }
}
}  // namespace

static uint32_t AcquireSamplerSlot(VkDevice d, const DkSamplerDescriptor& desc);

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo*,
                                                   const VkAllocationCallbacks*, VkDevice* out) {
  VKDK_TRACE("CreateDevice");
  VkDevice d = Make<VkDevice_T>();
  if (!d) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  d->physical = physical;
  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = DebugCallback;
  dm.cbAlloc = MemAlloc;
  dm.cbFree = MemFree;
  // Vulkan's conventions: framebuffer origin at the top left, depth 0..1. Clip space y down is Vulkan's too: the
  // viewport's ScaleY is written positive after every dkCmdBufSetViewports (vkdk_cmd.cpp, SetViewports).
  dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
  d->dk = dkDeviceCreate(&dm);
  if (!d->dk) {
    delete d;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, d->dk);
  qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_HighPrio;
  // Zcull off (Dekopon-32 round i). It culls tiles with the rasterized depth before the fragment shader runs, and
  // every PICA fragment shader writes its own depth (-z * scale + offset, over w when W-buffered): Zcull's view and
  // the depth buffer disagree and whole tiles of geometry that should pass were dropped (MK7: ground, walls and
  // backgrounds missing or flickering). NVK never enables Zcull. sdmc:/switch/projectdream_nx/vkdk_zcull turns it on again.
  {
    std::FILE* zf = std::fopen("sdmc:/switch/projectdream_nx/vkdk_zcull", "r");
    if (zf) {
      std::fclose(zf);
      Log("device: Zcull on (vkdk_zcull)");
    } else {
      qm.flags |= DkQueueFlags_DisableZcull;
    }
  }
  qm.commandMemorySize = 1u << 20;
  qm.flushThreshold = 256u << 10;
  d->queue.device = d;
  d->queue.dk = dkQueueCreate(&qm);

  // Descriptor tables, CPU-written (uncached) and read by the GPU through its descriptor cache.
  d->imageTable = d->MakeBlock(kImageDescriptors * sizeof(DkImageDescriptor),
                               DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "image descriptors");
  d->samplerTable = d->MakeBlock(kSamplerDescriptors * sizeof(DkSamplerDescriptor),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "sampler descriptors");
  d->pushBlock = d->MakeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                              "push constants");
  if (!d->queue.dk || !d->imageTable || !d->samplerTable || !d->pushBlock) {
    Log("device: deko3d queue or tables could not be made");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  d->imageCpu = static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(d->imageTable));
  d->samplerCpu = static_cast<DkSamplerDescriptor*>(dkMemBlockGetCpuAddr(d->samplerTable));
  memset(d->imageCpu, 0, kImageDescriptors * sizeof(DkImageDescriptor));
  memset(d->samplerCpu, 0, kSamplerDescriptors * sizeof(DkSamplerDescriptor));
  d->imageGpu = dkMemBlockGetGpuAddr(d->imageTable);
  {
    // Entry 0 of the sampler table: the default state (sampler bindings nothing was written to read it).
    DkSampler defaults;
    dkSamplerDefaults(&defaults);
    DkSamplerDescriptor desc;
    dkSamplerDescriptorInitialize(&desc, &defaults);
    d->defaultSamplerSlot = AcquireSamplerSlot(d, desc);
  }
  d->samplerGpu = dkMemBlockGetGpuAddr(d->samplerTable);
  d->pushGpu = dkMemBlockGetGpuAddr(d->pushBlock);
  memset(dkMemBlockGetCpuAddr(d->pushBlock), 0, DK_MEMBLOCK_ALIGNMENT);

  // The submission prologue: every cache the CPU's writes between submissions can make stale.
  d->queue.prologueMem = d->MakeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                      "prologue");
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, d->dk);
  d->queue.prologueBuf = dkCmdBufCreate(&cm);
  dkCmdBufAddMemory(d->queue.prologueBuf, d->queue.prologueMem, 0, DK_MEMBLOCK_ALIGNMENT);
  dkCmdBufBarrier(d->queue.prologueBuf, DkBarrier_None,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                      DkInvalidateFlags_L2Cache);
  dkCmdBufBindImageDescriptorSet(d->queue.prologueBuf, d->imageGpu, kImageDescriptors);
  dkCmdBufBindSamplerDescriptorSet(d->queue.prologueBuf, d->samplerGpu, kSamplerDescriptors);
  d->queue.prologue = dkCmdBufFinishList(d->queue.prologueBuf);

  // The GPU-time reports (VkQueue_T::stamp*): a list per slot for before a submission's work and one for after.
  d->queue.stampData = d->MakeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                    "gpu time reports");
  d->queue.stampMem = d->MakeBlock(4 * DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                   "gpu time lists");
  if (d->queue.stampData && d->queue.stampMem) {
    memset(dkMemBlockGetCpuAddr(d->queue.stampData), 0, DK_MEMBLOCK_ALIGNMENT);
    const DkGpuAddr at = dkMemBlockGetGpuAddr(d->queue.stampData);
    d->queue.stampBuf = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(d->queue.stampBuf, d->queue.stampMem, 0, 4 * DK_MEMBLOCK_ALIGNMENT);
    for (uint32_t i = 0; i < VkQueue_T::kStampSlots; ++i) {
      dkCmdBufReportCounter(d->queue.stampBuf, DkCounter_Timestamp, at + i * 32);
      d->queue.stampBegin[i] = dkCmdBufFinishList(d->queue.stampBuf);
      dkCmdBufReportCounter(d->queue.stampBuf, DkCounter_Timestamp, at + i * 32 + 16);
      d->queue.stampEnd[i] = dkCmdBufFinishList(d->queue.stampBuf);
    }
  }

  d->shaders = new ShaderCache(d);
  ApplyPendingCachePath(d);
  SetUpDrawnCopies(d);
  g_device.store(d);
  Log("device: deko3d ready (%u image and %u sampler descriptors)", kImageDescriptors, kSamplerDescriptors);
  *out = d;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice d, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyDevice");
  if (!d) {
    return;
  }
  dkQueueWaitIdle(d->queue.dk);
  delete d->shaders;
  if (d->queue.prologueBuf) {
    dkCmdBufDestroy(d->queue.prologueBuf);
  }
  d->DestroyBlock(d->queue.prologueMem);
  if (d->queue.stampBuf) {
    dkCmdBufDestroy(d->queue.stampBuf);
  }
  d->DestroyBlock(d->queue.stampMem);
  d->DestroyBlock(d->queue.stampData);
  dkQueueDestroy(d->queue.dk);
  for (auto& c : d->codeBlocks) {
    d->DestroyBlock(c.first);
  }
  d->DestroyBlock(d->imageTable);
  d->DestroyBlock(d->samplerTable);
  d->DestroyBlock(d->pushBlock);
  d->RetireBlocks(true);  // (the device is idle: every block let go goes now)
  dkDeviceDestroy(d->dk);
  g_device.store(nullptr);
  delete d;
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice d, uint32_t, uint32_t, VkQueue* out) {
  VKDK_TRACE("GetDeviceQueue");
  *out = &d->queue;
}

static VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice d) {
  VKDK_TRACE("DeviceWaitIdle");
  std::lock_guard<std::mutex> lock(d->queue.mutex);
  dkQueueWaitIdle(d->queue.dk);
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue q) {
  VKDK_TRACE("QueueWaitIdle");
  std::lock_guard<std::mutex> lock(q->mutex);
  dkQueueWaitIdle(q->dk);
  return VK_SUCCESS;
}

// ---- memory

static VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* info,
                                                     const VkAllocationCallbacks*, VkDeviceMemory* out) {
  VKDK_TRACE("AllocateMemory");
  if (info->memoryTypeIndex >= kMemTypeCount || info->allocationSize > 0xFFFFF000ull) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  static const uint32_t kFlags[kMemTypeCount] = {
      DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
      DkMemBlockFlags_GpuCached,
      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
      DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached,
  };
  VkDeviceMemory m = Make<VkDeviceMemory_T>();
  if (!m) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  m->size = AlignUp(uint32_t(info->allocationSize), DK_MEMBLOCK_ALIGNMENT);
  m->type = info->memoryTypeIndex;
  m->block = d->MakeBlock(m->size, kFlags[m->type], "device memory");
  if (!m->block) {
    delete m;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  m->cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(m->block));
  m->gpu = dkMemBlockGetGpuAddr(m->block);
  *out = m;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks*) {
  VKDK_TRACE("FreeMemory");
  if (m) {
    d->DestroyBlock(m->block);
    delete m;
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize,
                                                VkMemoryMapFlags, void** out) {
  VKDK_TRACE("MapMemory");
  if (!m->cpu) {
    return VK_ERROR_MEMORY_MAP_FAILED;
  }
  *out = m->cpu + offset;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice, VkDeviceMemory) {
  VKDK_TRACE("UnmapMemory");}

static VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice, uint32_t count,
                                                              const VkMappedMemoryRange* ranges) {
  VKDK_TRACE("FlushMappedMemoryRanges");
  for (uint32_t i = 0; i < count; ++i) {
    VkDeviceMemory m = ranges[i].memory;
    if (m->type != kMemReadback || !m->cpu) {
      continue;
    }
    const uint64_t size = ranges[i].size == VK_WHOLE_SIZE ? m->size - ranges[i].offset : ranges[i].size;
    armDCacheClean(m->cpu + ranges[i].offset, size_t(size));
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(VkDevice, uint32_t count,
                                                                   const VkMappedMemoryRange* ranges) {
  VKDK_TRACE("InvalidateMappedMemoryRanges");
  for (uint32_t i = 0; i < count; ++i) {
    VkDeviceMemory m = ranges[i].memory;
    if (m->type != kMemReadback || !m->cpu) {
      continue;
    }
    const uint64_t size = ranges[i].size == VK_WHOLE_SIZE ? m->size - ranges[i].offset : ranges[i].size;
    armDCacheFlush(m->cpu + ranges[i].offset, size_t(size));
  }
  return VK_SUCCESS;
}

// ---- buffers

static constexpr uint32_t kBufferTypes = (1u << kMemDevice) | (1u << kMemUpload) | (1u << kMemReadback);

static VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice, const VkBufferCreateInfo* info,
                                                   const VkAllocationCallbacks*, VkBuffer* out) {
  VKDK_TRACE("CreateBuffer");
  VkBuffer b = Make<VkBuffer_T>();
  if (!b) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  b->size = info->size;
  b->usage = info->usage;
  *out = b;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyBuffer");
  delete b;
}

static void BufferRequirements(VkBuffer b, VkMemoryRequirements& r) {
  r.size = AlignUp64(b->size, DK_UNIFORM_BUF_ALIGNMENT);
  r.alignment = (b->usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
                    ? DK_UNIFORM_BUF_ALIGNMENT
                    : 64;
  r.memoryTypeBits = kBufferTypes;
}

static VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice, VkBuffer b, VkMemoryRequirements* r) {
  VKDK_TRACE("GetBufferMemoryRequirements");
  BufferRequirements(b, *r);
}

static VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(VkDevice, const VkBufferMemoryRequirementsInfo2* info,
                                                              VkMemoryRequirements2* r) {
  VKDK_TRACE("GetBufferMemoryRequirements2");
  BufferRequirements(info->buffer, r->memoryRequirements);
  for (auto* s = reinterpret_cast<VkBaseOutStructure*>(r->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
      auto* dr = reinterpret_cast<VkMemoryDedicatedRequirements*>(s);
      dr->prefersDedicatedAllocation = VK_FALSE;
      dr->requiresDedicatedAllocation = VK_FALSE;
    }
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize offset) {
  VKDK_TRACE("BindBufferMemory");
  if (!m || offset + b->size > m->size) {
    VKDK_LOG_LIMITED(16, "buffer of %llu bytes bound at offset %llu of a %u-byte memory: past its end",
                     (unsigned long long)b->size, (unsigned long long)offset, m ? m->size : 0u);
  }
  b->memory = m;
  b->offset = offset;
  b->gpu = m->gpu + offset;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice d, uint32_t count,
                                                       const VkBindBufferMemoryInfo* infos) {
  VKDK_TRACE("BindBufferMemory2");
  for (uint32_t i = 0; i < count; ++i) {
    BindBufferMemory(d, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) {
  VKDK_TRACE("GetBufferDeviceAddress");
  return info->buffer ? info->buffer->gpu : 0;
}

// Bytes of one texel of a texel buffer format (the formats texel buffers are made with).
static uint32_t TexelBytes(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SFLOAT:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
      return 16;
    default: return 0;
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(VkDevice d, const VkBufferViewCreateInfo* info,
                                                       const VkAllocationCallbacks*, VkBufferView* out) {
  VKDK_TRACE("CreateBufferView");
  VkBufferView v = Make<VkBufferView_T>();
  if (!v) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  v->buffer = info->buffer;
  const VkBuffer b = info->buffer;
  const uint32_t texel = TexelBytes(info->format);
  const DkImageFormat format = ToDkFormat(info->format);
  if (b && b->memory && texel && format != DkImageFormat_None) {
    const uint64_t range = info->range == VK_WHOLE_SIZE ? b->size - info->offset : info->range;
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, d->dk);
    maker.type = DkImageType_Buffer;
    maker.format = format;
    maker.dimensions[0] = uint32_t(range / texel);
    dkImageLayoutInitialize(&v->layout, &maker);
    dkImageInitialize(&v->image, &v->layout, b->memory->block, uint32_t(b->offset + info->offset));
    DkImageView view;
    dkImageViewDefaults(&view, &v->image);
    dkImageDescriptorInitialize(&v->descriptor, &view, false, false);
    v->valid = true;
  } else {
    VKDK_LOG_LIMITED(8, "buffer view of format %d is not drawn with", int(info->format));
  }
  *out = v;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyBufferView(VkDevice, VkBufferView v, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyBufferView");
  delete v;
}

// ---- images

static VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice d, const VkImageCreateInfo* info,
                                                  const VkAllocationCallbacks*, VkImage* out) {
  VKDK_TRACE("CreateImage");
  const DkImageFormat format = ToDkFormat(info->format);
  if (format == DkImageFormat_None) {
    VKDK_LOG_LIMITED(32, "image format %d has no deko3d format", int(info->format));
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  if (info->samples != VK_SAMPLE_COUNT_1_BIT) {
    VKDK_LOG_LIMITED(8, "multisampled images are not supported");
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  VkImage img = Make<VkImage_T>();
  if (!img) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  img->type = info->imageType;
  img->format = info->format;
  img->dkFormat = format;
  img->extent = info->extent;
  img->mips = std::max(info->mipLevels, 1u);
  img->layers = std::max(info->arrayLayers, 1u);
  img->flags = info->flags;
  img->usage = info->usage;
  img->depth = IsDepthFormat(info->format) && info->format != VK_FORMAT_S8_UINT;
  img->stencil = HasStencil(info->format);
  DkImageType type;
  switch (info->imageType) {
    case VK_IMAGE_TYPE_1D:
      type = img->layers > 1 ? DkImageType_1DArray : DkImageType_1D;
      break;
    case VK_IMAGE_TYPE_3D:
      type = DkImageType_3D;
      break;
    default:
      if ((info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && img->layers % 6 == 0) {
        type = img->layers > 6 ? DkImageType_CubemapArray : DkImageType_Cubemap;
      } else {
        type = img->layers > 1 ? DkImageType_2DArray : DkImageType_2D;
      }
      break;
  }
  img->dkType = type;
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, d->dk);
  m.type = type;
  m.format = format;
  m.dimensions[0] = std::max(info->extent.width, 1u);
  m.dimensions[1] = std::max(info->extent.height, 1u);
  // deko3d's layer counts: a cube map's six faces are implied, a cube map array counts cubes, a 1D array gives its
  // layers as the height.
  m.dimensions[2] = type == DkImageType_3D            ? std::max(info->extent.depth, 1u)
                    : type == DkImageType_Cubemap      ? 6
                    : type == DkImageType_CubemapArray ? img->layers / 6
                                                       : img->layers;
  if (type == DkImageType_1D) {
    m.dimensions[1] = m.dimensions[2] = 0;
  } else if (type == DkImageType_1DArray) {
    m.dimensions[1] = img->layers;
    m.dimensions[2] = 0;
  } else if (type == DkImageType_2D) {
    m.dimensions[2] = 0;
  }
  m.mipLevels = img->mips;
  uint32_t flags = 0;
  const bool render =
      (info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
  {
    // Which formats a game makes images in, once each (round h: R5G6B5 and R4G4B4A4 targets drew garbage); a colour
    // target in a format deko3d cannot draw into is reported, it would come out wrong.
    static std::mutex censusMutex;
    static std::unordered_set<uint64_t> seen;
    const uint64_t key = (uint64_t(info->format) << 1) | (render ? 1 : 0);
    std::lock_guard<std::mutex> lock(censusMutex);
    if (seen.insert(key).second) {
      const bool bad = render && !img->depth && !Renderable(img->dkFormat);
      Log("images: first %s image in VkFormat %d (deko3d format %d)%s", render ? "target" : "sampled",
          int(info->format), int(img->dkFormat), bad ? ": NOT a deko3d render target, it will draw wrong" : "");
    }
  }
  // A single-level 2D colour image that copies write, in a format a drawn copy keeps exact, can be their render target
  // (vkdk_cmd.cpp DrawnCopy): the game's resolve textures.
  const bool copyTarget = !img->depth && type == DkImageType_2D && img->mips == 1 && img->layers == 1 &&
                          info->samples == VK_SAMPLE_COUNT_1_BIT && (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
                          DrawnCopyFormat(info->format);
  if (copyTarget && !render) {
    flags |= DkImageFlags_UsageRender;
  }
  if (render && type != DkImageType_3D) {
    flags |= DkImageFlags_UsageRender;
    // Colour targets compressed. Depth targets too unless they take copies (bo2 makes its Zcull depth images without
    // TRANSFER_DST for the same reason on NVK); the copy engine does not decompress.
    // vkdk_nocompress (round k diagnostics): no hardware compression at all.
    if ((!img->depth || !(info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) && !DebugSwitch("vkdk_nocompress", true) &&
        !DebugSwitch("vkdk_copyengine", true)) {  // (the copy engine cannot read compressed data)
      flags |= DkImageFlags_HwCompression;
    }
  }
  // A single-level 3D RGBA8 image that copies write: its blocks are written by a compute pass (vkdk_cmd.cpp
  // VolumeCopy), the game's lighting volume.
  img->storable = type == DkImageType_3D && img->mips == 1 && info->format == VK_FORMAT_R8G8B8A8_UNORM &&
                  (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  if (img->storable) {
    flags |= DkImageFlags_UsageLoadStore;
  }
  if (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
    flags |= DkImageFlags_UsageLoadStore;
  }
  if (type != DkImageType_3D && CanUse2DEngine(format)) {
    flags |= DkImageFlags_Usage2DEngine;
  }
  m.flags = flags;
  img->renderable = (flags & DkImageFlags_UsageRender) != 0 && type != DkImageType_3D;
  uint32_t rows = m.dimensions[1] ? m.dimensions[1] : 1;
  if (IsCompressed(format)) {
    uint32_t bw = 4, bh = 4;
    FormatBlockBytes(info->format, bw, bh);  // (ASTC blocks can be 4 to 12 rows tall)
    rows = (rows + bh - 1) / bh;
  }
  TileSizeFix(m, rows);
  dkImageLayoutInitialize(&img->layout, &m);
  img->size = dkImageLayoutGetSize(&img->layout);
  img->alignment = dkImageLayoutGetAlignment(&img->layout);
  *out = img;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice, VkImage img, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyImage");
  if (img && !img->swapchain) {
    delete img;
  }
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice, VkImage img, VkMemoryRequirements* r) {
  VKDK_TRACE("GetImageMemoryRequirements");
  r->size = img->size;
  r->alignment = img->alignment;
  r->memoryTypeBits = 1u << kMemImage;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice d, const VkImageMemoryRequirementsInfo2* info,
                                                             VkMemoryRequirements2* r) {
  VKDK_TRACE("GetImageMemoryRequirements2");
  GetImageMemoryRequirements(d, info->image, &r->memoryRequirements);
  for (auto* s = reinterpret_cast<VkBaseOutStructure*>(r->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
      auto* dr = reinterpret_cast<VkMemoryDedicatedRequirements*>(s);
      dr->prefersDedicatedAllocation = VK_FALSE;
      dr->requiresDedicatedAllocation = VK_FALSE;
    }
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice, VkImage img, VkDeviceMemory m, VkDeviceSize offset) {
  VKDK_TRACE("BindImageMemory");
  if (m->type != kMemImage || offset % img->alignment || offset + img->size > m->size) {
    VKDK_LOG_LIMITED(16, "image bound to memory type %u at offset %llu (alignment %u): refused", m->type,
                     (unsigned long long)offset, img->alignment);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  dkImageInitialize(&img->image, &img->layout, m->block, uint32_t(offset));
  img->bound = true;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice d, uint32_t count, const VkBindImageMemoryInfo* infos) {
  VKDK_TRACE("BindImageMemory2");
  for (uint32_t i = 0; i < count; ++i) {
    const VkResult r = BindImageMemory(d, infos[i].image, infos[i].memory, infos[i].memoryOffset);
    if (r != VK_SUCCESS) {
      return r;
    }
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice, const VkImageViewCreateInfo* info,
                                                      const VkAllocationCallbacks*, VkImageView* out) {
  VKDK_TRACE("CreateImageView");
  VkImage img = info->image;
  VkImageView v = Make<VkImageView_T>();
  if (!v) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  v->image = img;
  v->type = info->viewType;
  v->format = info->format;
  const VkImageSubresourceRange& r = info->subresourceRange;
  v->baseMip = r.baseMipLevel;
  v->mipCount = r.levelCount == VK_REMAINING_MIP_LEVELS ? img->mips - r.baseMipLevel : r.levelCount;
  v->baseLayer = r.baseArrayLayer;
  v->layerCount = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers - r.baseArrayLayer : r.layerCount;

  DkImageView& view = v->view;
  dkImageViewDefaults(&view, &img->image);
  // A different format of the same size (integer or sRGB views). Depth images keep their own: deko3d samples a depth
  // format as depth in the red channel, which is what an R32 view of depth reads.
  if (!img->depth && !img->stencil) {
    const DkImageFormat f = ToDkFormat(info->format);
    if (f != DkImageFormat_None && f != img->dkFormat) {
      view.format = f;
    }
  }
  // Stencil of a depth/stencil image.
  if ((r.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && !(r.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && img->depth) {
    view.dsSource = DkDsSource_Stencil;
  }
  switch (info->viewType) {
    case VK_IMAGE_VIEW_TYPE_1D: view.type = DkImageType_1D; break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: view.type = DkImageType_1DArray; break;
    case VK_IMAGE_VIEW_TYPE_2D:
      if (img->dkType != DkImageType_2D) view.type = DkImageType_2D;
      break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: view.type = DkImageType_2DArray; break;
    case VK_IMAGE_VIEW_TYPE_3D: view.type = DkImageType_3D; break;
    case VK_IMAGE_VIEW_TYPE_CUBE: view.type = DkImageType_Cubemap; break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: view.type = DkImageType_CubemapArray; break;
    default: break;
  }
  if (img->dkType != DkImageType_3D && (v->baseLayer || v->layerCount != img->layers)) {
    view.layerOffset = uint16_t(v->baseLayer);
    view.layerCount = uint16_t(v->layerCount);
  }
  if (v->baseMip || v->mipCount != img->mips) {
    view.mipLevelOffset = uint8_t(v->baseMip);
    view.mipLevelCount = uint8_t(v->mipCount);
  }
  const VkComponentMapping& c = info->components;
  view.swizzle[0] = ToDk(c.r, DkImageSwizzle_Red);
  view.swizzle[1] = ToDk(c.g, DkImageSwizzle_Green);
  view.swizzle[2] = ToDk(c.b, DkImageSwizzle_Blue);
  view.swizzle[3] = ToDk(c.a, DkImageSwizzle_Alpha);
  // A format deko3d stores in another channel order: each component the caller asked for, from where it sits.
  DkImageSwizzle base[4];
  if (FormatSwizzle(info->format, base)) {
    for (auto& sw : view.swizzle) {
      if (sw >= DkImageSwizzle_Red && sw <= DkImageSwizzle_Alpha) {
        sw = base[sw - DkImageSwizzle_Red];
      }
    }
  }
  // D3D and Vulkan read depth as (D, 0, 0, 1); deko3d gives (D, D, D, 1) for some formats (SwitchWakerHD).
  if (img->depth && (r.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
    for (int i = 1; i < 3; ++i) {
      if (view.swizzle[i] == DkImageSwizzle_Green || view.swizzle[i] == DkImageSwizzle_Blue) {
        view.swizzle[i] = DkImageSwizzle_Zero;
      }
    }
    if (view.swizzle[3] == DkImageSwizzle_Alpha) {
      view.swizzle[3] = DkImageSwizzle_One;
    }
  }
  if (img->bound) {
    dkImageDescriptorInitialize(&v->descriptor, &view, false, false);
  }
  // The target view: one mip; a 2D image's layer as 2D, layered ones as their layers.
  v->target = view;
  v->target.swizzle[0] = DkImageSwizzle_Red;
  v->target.swizzle[1] = DkImageSwizzle_Green;
  v->target.swizzle[2] = DkImageSwizzle_Blue;
  v->target.swizzle[3] = DkImageSwizzle_Alpha;
  v->target.mipLevelOffset = uint8_t(v->baseMip);
  v->target.mipLevelCount = 1;
  if (img->dkType == DkImageType_Cubemap || img->dkType == DkImageType_CubemapArray ||
      img->dkType == DkImageType_2DArray) {
    v->target.type = v->layerCount > 1 ? DkImageType_2DArray : DkImageType_2D;
  }
  *out = v;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice, VkImageView v, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyImageView");
  delete v;
}

// ---- samplers

// The sampler table entry for a sampler state: shared by all samplers with the same descriptor bytes, freed with the
// last of them. UINT32_MAX when the table is full.
static uint32_t AcquireSamplerSlot(VkDevice d, const DkSamplerDescriptor& desc) {
  const std::string key(reinterpret_cast<const char*>(&desc), sizeof(desc));
  std::lock_guard<std::mutex> lock(d->tableMutex);
  auto it = d->samplerStates.find(key);
  if (it != d->samplerStates.end()) {
    it->second.second++;
    return it->second.first;
  }
  const uint32_t slot = d->samplerRanges.Allocate(1);
  if (slot == UINT32_MAX) {
    return UINT32_MAX;
  }
  d->samplerCpu[slot] = desc;
  d->samplerStates.emplace(key, std::make_pair(slot, 1u));
  d->descriptorEpoch.fetch_add(1, std::memory_order_release);
  const size_t n = d->samplerStates.size();
  if (n == 64 || n == 256 || n == 1024 || n == 3072) {
    Log("samplers: %zu distinct sampler states in the table", n);
  }
  return slot;
}

static void ReleaseSamplerSlot(VkDevice d, const DkSamplerDescriptor& desc) {
  const std::string key(reinterpret_cast<const char*>(&desc), sizeof(desc));
  std::lock_guard<std::mutex> lock(d->tableMutex);
  auto it = d->samplerStates.find(key);
  if (it != d->samplerStates.end() && --it->second.second == 0) {
    if (it->second.first != d->defaultSamplerSlot) {
      d->samplerRanges.Free(it->second.first, 1);
    }
    d->samplerStates.erase(it);
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice d, const VkSamplerCreateInfo* info,
                                                    const VkAllocationCallbacks*, VkSampler* out) {
  VKDK_TRACE("CreateSampler");
  VkSampler s = Make<VkSampler_T>();
  if (!s) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  DkSampler ds;
  dkSamplerDefaults(&ds);
  ds.minFilter = info->minFilter == VK_FILTER_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
  ds.magFilter = info->magFilter == VK_FILTER_LINEAR ? DkFilter_Linear : DkFilter_Nearest;
  ds.mipFilter = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? DkMipFilter_Linear : DkMipFilter_Nearest;
  ds.wrapMode[0] = ToDk(info->addressModeU);
  ds.wrapMode[1] = ToDk(info->addressModeV);
  ds.wrapMode[2] = ToDk(info->addressModeW);
  ds.lodClampMin = info->minLod;
  ds.lodClampMax = info->maxLod;
  ds.lodBias = info->mipLodBias;
  ds.compareEnable = info->compareEnable;
  ds.compareOp = ToDk(info->compareOp);
  ds.maxAnisotropy = info->anisotropyEnable ? std::max(1.0f, info->maxAnisotropy) : 1.0f;
  switch (info->borderColor) {
    case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
      ds.borderColor[3].value_f = 1.0f;
      break;
    case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
      ds.borderColor[3].value_ui = 1;
      break;
    case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
      for (int i = 0; i < 4; ++i) ds.borderColor[i].value_f = 1.0f;
      break;
    case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
      for (int i = 0; i < 4; ++i) ds.borderColor[i].value_ui = 1;
      break;
    default:
      break;
  }
  dkSamplerDescriptorInitialize(&s->descriptor, &ds);
  s->slot = AcquireSamplerSlot(d, s->descriptor);
  if (s->slot == UINT32_MAX) {
    VKDK_LOG_LIMITED(8, "sampler table full: a sampler gets the default state");
    s->slot = d->defaultSamplerSlot;
  }
  *out = s;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySampler(VkDevice d, VkSampler s, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroySampler");
  if (s) {
    if (s->slot != d->defaultSamplerSlot) {
      ReleaseSamplerSlot(d, s->descriptor);
    }
    delete s;
  }
}

// ---- fences and semaphores

static VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo* info,
                                                  const VkAllocationCallbacks*, VkFence* out) {
  VKDK_TRACE("CreateFence");
  VkFence f = Make<VkFence_T>();
  if (!f) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  f->state = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) ? 2 : 0;
  *out = f;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, VkFence f, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyFence");
  delete f;
}

static VkResult FenceWait(VkFence f, int64_t timeout_ns) {
  const int s = f->state.load(std::memory_order_acquire);
  if (s == 2) {
    return VK_SUCCESS;
  }
  if (s == 0) {
    // Not submitted: Vulkan waits for a submission that will signal it; nothing here ever would.
    return timeout_ns == 0 ? VK_NOT_READY : VK_TIMEOUT;
  }
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  const DkResult r = dkFenceWait(&f->fence, timeout_ns);
  if (timeout_ns != 0) {
    const uint64_t waited = armTicksToNs(armGetSystemTick()) - t0;
    g_fenceWaitNs.fetch_add(waited, std::memory_order_relaxed);
    g_fenceWaitTotalNs.fetch_add(waited, std::memory_order_relaxed);
  }
  if (r == DkResult_Success) {
    f->state.store(2, std::memory_order_release);
    return VK_SUCCESS;
  }
  return timeout_ns == 0 ? VK_NOT_READY : VK_TIMEOUT;
}

static VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, uint32_t count, const VkFence* fences, VkBool32 all,
                                                    uint64_t timeout) {
  VKDK_TRACE("WaitForFences");
  const int64_t t = timeout > uint64_t(INT64_MAX) ? -1 : int64_t(timeout);
  if (all || count == 1) {
    for (uint32_t i = 0; i < count; ++i) {
      const VkResult r = FenceWait(fences[i], t);
      if (r != VK_SUCCESS) {
        return r == VK_NOT_READY ? VK_TIMEOUT : r;
      }
    }
    return VK_SUCCESS;
  }
  // Any: poll (bo2 waits for one fence at a time).
  const uint64_t start = armTicksToNs(armGetSystemTick());
  for (;;) {
    for (uint32_t i = 0; i < count; ++i) {
      if (FenceWait(fences[i], 0) == VK_SUCCESS) {
        return VK_SUCCESS;
      }
    }
    if (t >= 0 && armTicksToNs(armGetSystemTick()) - start >= uint64_t(t)) {
      return VK_TIMEOUT;
    }
    svcSleepThread(100000);
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice, VkFence f) {
  VKDK_TRACE("GetFenceStatus");
  return FenceWait(f, 0) == VK_SUCCESS ? VK_SUCCESS : VK_NOT_READY;
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, uint32_t count, const VkFence* fences) {
  VKDK_TRACE("ResetFences");
  for (uint32_t i = 0; i < count; ++i) {
    fences[i]->state.store(0, std::memory_order_release);
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*,
                                                      const VkAllocationCallbacks*, VkSemaphore* out) {
  VKDK_TRACE("CreateSemaphore");
  *out = Make<VkSemaphore_T>();
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore s, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroySemaphore");
  delete s;
}

// ---- queries

static VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* info,
                                                      const VkAllocationCallbacks*, VkQueryPool* out) {
  VKDK_TRACE("CreateQueryPool");
  VkQueryPool p = Make<VkQueryPool_T>();
  if (!p) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  p->type = info->queryType;
  p->count = info->queryCount;
  p->statistics = info->pipelineStatistics;
  // A 16-byte report per query, and for occlusion a begin report each after them.
  p->beginOffset = p->count * 16;
  const uint32_t bytes = p->count * 32;
  p->block = d->MakeBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "queries");
  if (!p->block) {
    delete p;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  p->cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(p->block));
  p->gpu = dkMemBlockGetGpuAddr(p->block);
  memset(p->cpu, 0, AlignUp(bytes, DK_MEMBLOCK_ALIGNMENT));
  *out = p;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice d, VkQueryPool p, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyQueryPool");
  if (p) {
    d->DestroyBlock(p->block);
    delete p;
  }
}

static uint64_t QueryValue(VkQueryPool p, uint32_t q) {
  uint64_t end, begin;
  memcpy(&end, p->cpu + q * 16, 8);
  if (p->type == VK_QUERY_TYPE_TIMESTAMP) {
    // The report's second word is the GPU time of the report.
    uint64_t ts;
    memcpy(&ts, p->cpu + q * 16 + 8, 8);
    return ts;
  }
  memcpy(&begin, p->cpu + p->beginOffset + q * 16, 8);
  return end - begin;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice, VkQueryPool p, uint32_t first, uint32_t count,
                                                          size_t size, void* data, VkDeviceSize stride,
                                                          VkQueryResultFlags flags) {
  VKDK_TRACE("GetQueryPoolResults");
  uint8_t* out = static_cast<uint8_t*>(data);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t v = QueryValue(p, first + i);
    uint8_t* at = out + i * stride;
    if (size_t(at - out) + ((flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4) > size) {
      break;
    }
    if (flags & VK_QUERY_RESULT_64_BIT) {
      memcpy(at, &v, 8);
    } else {
      const uint32_t v32 = uint32_t(std::min<uint64_t>(v, UINT32_MAX));
      memcpy(at, &v32, 4);
    }
    if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
      if (flags & VK_QUERY_RESULT_64_BIT) {
        const uint64_t one = 1;
        memcpy(at + 8, &one, 8);
      } else {
        const uint32_t one = 1;
        memcpy(at + 4, &one, 4);
      }
    }
  }
  // Results are read after the fence of the submission that wrote them (bo2 waits on its job fence first).
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice, VkQueryPool p, uint32_t first, uint32_t count) {
  VKDK_TRACE("ResetQueryPool");
  memset(p->cpu + first * 16, 0, size_t(count) * 16);
  memset(p->cpu + p->beginOffset + first * 16, 0, size_t(count) * 16);
}

// ---- descriptor set layouts, pools and sets

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
                                                                const VkAllocationCallbacks*,
                                                                VkDescriptorSetLayout* out) {
  VKDK_TRACE("CreateDescriptorSetLayout");
  VkDescriptorSetLayout l = Make<VkDescriptorSetLayout_T>();
  if (!l) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  uint32_t maxBinding = 0;
  for (uint32_t i = 0; i < info->bindingCount; ++i) {
    maxBinding = std::max(maxBinding, info->pBindings[i].binding + 1);
  }
  l->bindings.resize(maxBinding);
  // In binding order, so a set's layout is the same whatever order the bindings were given in.
  for (uint32_t b = 0; b < maxBinding; ++b) {
    for (uint32_t i = 0; i < info->bindingCount; ++i) {
      const VkDescriptorSetLayoutBinding& src = info->pBindings[i];
      if (src.binding != b) {
        continue;
      }
      auto& dst = l->bindings[b];
      dst.valid = true;
      dst.type = src.descriptorType;
      dst.count = src.descriptorCount;
      switch (src.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          dst.image = l->images;
          l->images += src.descriptorCount;
          break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
          dst.sampler = l->samplers;
          l->samplers += src.descriptorCount;
          break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          dst.image = l->images;
          l->images += src.descriptorCount;
          dst.sampler = l->samplers;
          l->samplers += src.descriptorCount;
          break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
          dst.dynamic = int32_t(l->dynamics);
          l->dynamics += src.descriptorCount;
          [[fallthrough]];
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          dst.buffer = l->buffers;
          l->buffers += src.descriptorCount;
          break;
        default:
          VKDK_LOG_LIMITED(8, "descriptor type %d is not supported", int(src.descriptorType));
          break;
      }
    }
  }
  *out = l;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout l,
                                                             const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyDescriptorSetLayout");
  delete l;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*,
                                                          const VkAllocationCallbacks*, VkDescriptorPool* out) {
  VKDK_TRACE("CreateDescriptorPool");
  *out = Make<VkDescriptorPool_T>();
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void FreeSet(VkDevice d, VkDescriptorSet s) {
  {
    std::lock_guard<std::mutex> lock(d->tableMutex);
    d->imageRanges.Free(s->imageFirst, s->layout.images);
    if (s->imageFirst != UINT32_MAX) d->imagesInSets -= s->layout.images;
  }
  delete s;
}

static VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags) {
  VKDK_TRACE("ResetDescriptorPool");
  for (VkDescriptorSet s : p->sets) {
    FreeSet(d, s);
  }
  p->sets.clear();
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyDescriptorPool");
  if (p) {
    ResetDescriptorPool(d, p, 0);
    delete p;
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* info,
                                                             VkDescriptorSet* out) {
  VKDK_TRACE("AllocateDescriptorSets");
  for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
    VkDescriptorSet s = Make<VkDescriptorSet_T>();
    if (!s) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    s->pool = info->descriptorPool;
    s->layout = *info->pSetLayouts[i];
    {
      std::lock_guard<std::mutex> lock(d->tableMutex);
      s->imageFirst = s->layout.images ? d->imageRanges.Allocate(s->layout.images) : 0;
      if (s->imageFirst != UINT32_MAX) {
        d->imagesInSets += s->layout.images;
        // High-water marks of the image table (each logged once).
        const uint32_t pct = uint32_t(uint64_t(d->imagesInSets) * 100 / kImageDescriptors);
        if (pct >= 25 && pct / 25 > d->imagesInSetsLogged) {
          d->imagesInSetsLogged = pct / 25;
          Log("descriptor tables: sets hold %u of %u image entries", d->imagesInSets, kImageDescriptors);
        }
      }
    }
    s->samplerFirst = UINT32_MAX;
    s->samplerSlots.assign(s->layout.samplers, uint16_t(d->defaultSamplerSlot));
    if (s->imageFirst == UINT32_MAX) {
      VKDK_LOG_LIMITED(16, "descriptor tables full: a set of %u images (%u in sets)", s->layout.images,
                       d->imagesInSets);
      s->layout.images = 0;
      FreeSet(d, s);
      for (uint32_t j = 0; j < i; ++j) {
        out[j]->pool->sets.pop_back();
        FreeSet(d, out[j]);
        out[j] = nullptr;
      }
      return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    s->buffers.assign(s->layout.buffers, DkBufExtents{0, 0});
    s->pool->sets.push_back(s);
    out[i] = s;
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t count,
                                                         const VkDescriptorSet* sets) {
  VKDK_TRACE("FreeDescriptorSets");
  for (uint32_t i = 0; i < count; ++i) {
    if (!sets[i]) {
      continue;
    }
    auto it = std::find(p->sets.begin(), p->sets.end(), sets[i]);
    if (it != p->sets.end()) {
      p->sets.erase(it);
    }
    FreeSet(d, sets[i]);
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice d, uint32_t writeCount,
                                                      const VkWriteDescriptorSet* writes, uint32_t copyCount,
                                                      const VkCopyDescriptorSet* copies) {
  VKDK_TRACE("UpdateDescriptorSets");
  bool tables = false;
  for (uint32_t w = 0; w < writeCount; ++w) {
    const VkWriteDescriptorSet& wr = writes[w];
    VkDescriptorSet s = wr.dstSet;
    uint32_t binding = wr.dstBinding;
    uint32_t element = wr.dstArrayElement;
    for (uint32_t i = 0; i < wr.descriptorCount; ++i, ++element) {
      // Consecutive bindings take the overflow (Vulkan's rule for descriptorCount past a binding's end).
      while (binding < s->layout.bindings.size() &&
             (!s->layout.bindings[binding].valid || element >= s->layout.bindings[binding].count)) {
        if (s->layout.bindings[binding].valid) {
          element -= s->layout.bindings[binding].count;
        }
        ++binding;
      }
      if (binding >= s->layout.bindings.size()) {
        break;
      }
      const auto& b = s->layout.bindings[binding];
      switch (wr.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
          const VkDescriptorImageInfo& ii = wr.pImageInfo[i];
          if (wr.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
              wr.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && ii.sampler) {
            if (b.sampler + element < s->samplerSlots.size()) s->samplerSlots[b.sampler + element] = uint16_t(ii.sampler->slot);
          }
          DkImageDescriptor& dst = d->imageCpu[s->imageFirst + b.image + element];
          if (!ii.imageView || !ii.imageView->image->bound) {
            memset(&dst, 0, sizeof(dst));
          } else if (wr.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            DkImageDescriptor desc;
            dkImageDescriptorInitialize(&desc, &ii.imageView->target, true, false);
            dst = desc;
          } else {
            // Made with the view (Vulkan binds an image's memory before any view of it is made).
            dst = ii.imageView->descriptor;
          }
          tables = true;
          break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
          const VkBufferView v = wr.pTexelBufferView[i];
          DkImageDescriptor& dst = d->imageCpu[s->imageFirst + b.image + element];
          if (v && v->valid) {
            dst = v->descriptor;
          } else {
            memset(&dst, 0, sizeof(dst));
          }
          tables = true;
          break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
          const VkDescriptorImageInfo& ii = wr.pImageInfo[i];
          if (ii.sampler) {
            if (b.sampler + element < s->samplerSlots.size()) s->samplerSlots[b.sampler + element] = uint16_t(ii.sampler->slot);
          }
          tables = true;
          break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
          const VkDescriptorBufferInfo& bi = wr.pBufferInfo[i];
          const uint64_t range = bi.range == VK_WHOLE_SIZE ? bi.buffer->size - bi.offset : bi.range;
          s->buffers[b.buffer + element] = {bi.buffer->gpu + bi.offset, uint32_t(std::min<uint64_t>(range, UINT32_MAX))};
          s->generation++;
          break;
        }
        default:
          VKDK_LOG_LIMITED(8, "descriptor write of type %d ignored", int(wr.descriptorType));
          break;
      }
    }
  }
  for (uint32_t c = 0; c < copyCount; ++c) {
    VKDK_LOG_LIMITED(4, "descriptor set copies are not supported");
  }
  if (tables) {
    d->descriptorEpoch.fetch_add(1, std::memory_order_release);
  }
}

// ---- the proc tables

#define VKDK_ENTRY(name, fn) {#name, reinterpret_cast<PFN_vkVoidFunction>(fn)}

struct Entry {
  const char* name;
  PFN_vkVoidFunction fn;
};

static const Entry kDeviceEntries[] = {
    VKDK_ENTRY(vkDestroyDevice, DestroyDevice),
    VKDK_ENTRY(vkGetDeviceQueue, GetDeviceQueue),
    VKDK_ENTRY(vkDeviceWaitIdle, DeviceWaitIdle),
    VKDK_ENTRY(vkQueueWaitIdle, QueueWaitIdle),
    VKDK_ENTRY(vkAllocateMemory, AllocateMemory),
    VKDK_ENTRY(vkFreeMemory, FreeMemory),
    VKDK_ENTRY(vkMapMemory, MapMemory),
    VKDK_ENTRY(vkUnmapMemory, UnmapMemory),
    VKDK_ENTRY(vkFlushMappedMemoryRanges, FlushMappedMemoryRanges),
    VKDK_ENTRY(vkInvalidateMappedMemoryRanges, InvalidateMappedMemoryRanges),
    VKDK_ENTRY(vkCreateBuffer, CreateBuffer),
    VKDK_ENTRY(vkDestroyBuffer, DestroyBuffer),
    VKDK_ENTRY(vkGetBufferMemoryRequirements, GetBufferMemoryRequirements),
    VKDK_ENTRY(vkGetBufferMemoryRequirements2, GetBufferMemoryRequirements2),
    VKDK_ENTRY(vkGetBufferMemoryRequirements2KHR, GetBufferMemoryRequirements2),
    VKDK_ENTRY(vkBindBufferMemory, BindBufferMemory),
    VKDK_ENTRY(vkBindBufferMemory2, BindBufferMemory2),
    VKDK_ENTRY(vkBindBufferMemory2KHR, BindBufferMemory2),
    VKDK_ENTRY(vkGetBufferDeviceAddress, GetBufferDeviceAddress),
    VKDK_ENTRY(vkGetBufferDeviceAddressKHR, GetBufferDeviceAddress),
    VKDK_ENTRY(vkCreateBufferView, CreateBufferView),
    VKDK_ENTRY(vkDestroyBufferView, DestroyBufferView),
    VKDK_ENTRY(vkCreateImage, CreateImage),
    VKDK_ENTRY(vkDestroyImage, DestroyImage),
    VKDK_ENTRY(vkGetImageMemoryRequirements, GetImageMemoryRequirements),
    VKDK_ENTRY(vkGetImageMemoryRequirements2, GetImageMemoryRequirements2),
    VKDK_ENTRY(vkGetImageMemoryRequirements2KHR, GetImageMemoryRequirements2),
    VKDK_ENTRY(vkBindImageMemory, BindImageMemory),
    VKDK_ENTRY(vkBindImageMemory2, BindImageMemory2),
    VKDK_ENTRY(vkBindImageMemory2KHR, BindImageMemory2),
    VKDK_ENTRY(vkCreateImageView, CreateImageView),
    VKDK_ENTRY(vkDestroyImageView, DestroyImageView),
    VKDK_ENTRY(vkCreateSampler, CreateSampler),
    VKDK_ENTRY(vkDestroySampler, DestroySampler),
    VKDK_ENTRY(vkCreateFence, CreateFence),
    VKDK_ENTRY(vkDestroyFence, DestroyFence),
    VKDK_ENTRY(vkWaitForFences, WaitForFences),
    VKDK_ENTRY(vkGetFenceStatus, GetFenceStatus),
    VKDK_ENTRY(vkResetFences, ResetFences),
    VKDK_ENTRY(vkCreateSemaphore, CreateSemaphore),
    VKDK_ENTRY(vkDestroySemaphore, DestroySemaphore),
    VKDK_ENTRY(vkCreateQueryPool, CreateQueryPool),
    VKDK_ENTRY(vkDestroyQueryPool, DestroyQueryPool),
    VKDK_ENTRY(vkGetQueryPoolResults, GetQueryPoolResults),
    VKDK_ENTRY(vkResetQueryPool, ResetQueryPool),
    VKDK_ENTRY(vkResetQueryPoolEXT, ResetQueryPool),
    VKDK_ENTRY(vkCreateDescriptorSetLayout, CreateDescriptorSetLayout),
    VKDK_ENTRY(vkDestroyDescriptorSetLayout, DestroyDescriptorSetLayout),
    VKDK_ENTRY(vkCreateDescriptorPool, CreateDescriptorPool),
    VKDK_ENTRY(vkDestroyDescriptorPool, DestroyDescriptorPool),
    VKDK_ENTRY(vkResetDescriptorPool, ResetDescriptorPool),
    VKDK_ENTRY(vkAllocateDescriptorSets, AllocateDescriptorSets),
    VKDK_ENTRY(vkFreeDescriptorSets, FreeDescriptorSets),
    VKDK_ENTRY(vkUpdateDescriptorSets, UpdateDescriptorSets),
};

static const Entry kInstanceEntries[] = {
    VKDK_ENTRY(vkCreateInstance, CreateInstance),
    VKDK_ENTRY(vkDestroyInstance, vkDestroyInstance),
    VKDK_ENTRY(vkEnumerateInstanceExtensionProperties, EnumerateInstanceExtensionProperties),
    VKDK_ENTRY(vkEnumerateInstanceLayerProperties, EnumerateInstanceLayerProperties),
    VKDK_ENTRY(vkEnumerateInstanceVersion, EnumerateInstanceVersion),
    VKDK_ENTRY(vkEnumeratePhysicalDevices, EnumeratePhysicalDevices),
    VKDK_ENTRY(vkEnumerateDeviceExtensionProperties, EnumerateDeviceExtensionProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceProperties, GetPhysicalDeviceProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceProperties2, GetPhysicalDeviceProperties2),
    VKDK_ENTRY(vkGetPhysicalDeviceProperties2KHR, GetPhysicalDeviceProperties2),
    VKDK_ENTRY(vkGetPhysicalDeviceFeatures, GetPhysicalDeviceFeatures),
    VKDK_ENTRY(vkGetPhysicalDeviceFeatures2, GetPhysicalDeviceFeatures2),
    VKDK_ENTRY(vkGetPhysicalDeviceFeatures2KHR, GetPhysicalDeviceFeatures2),
    VKDK_ENTRY(vkGetPhysicalDeviceFormatProperties, GetPhysicalDeviceFormatProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceFormatProperties2, GetPhysicalDeviceFormatProperties2),
    VKDK_ENTRY(vkGetPhysicalDeviceFormatProperties2KHR, GetPhysicalDeviceFormatProperties2),
    VKDK_ENTRY(vkGetPhysicalDeviceImageFormatProperties, GetPhysicalDeviceImageFormatProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties, GetPhysicalDeviceQueueFamilyProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceMemoryProperties, GetPhysicalDeviceMemoryProperties),
    VKDK_ENTRY(vkGetPhysicalDeviceMemoryProperties2, GetPhysicalDeviceMemoryProperties2),
    VKDK_ENTRY(vkGetPhysicalDeviceMemoryProperties2KHR, GetPhysicalDeviceMemoryProperties2),
    VKDK_ENTRY(vkCreateDevice, CreateDevice),
};

namespace vkdk {

PFN_vkVoidFunction DeviceProcAddr(const char* name) {
  for (const Entry& e : kDeviceEntries) {
    if (!strcmp(e.name, name)) {
      return e.fn;
    }
  }
  if (PFN_vkVoidFunction f = vkdk_cmd_proc(name)) {
    return f;
  }
  if (PFN_vkVoidFunction f = vkdk_pipeline_proc(name)) {
    return f;
  }
  return WsiProcAddr(name);
}

}  // namespace vkdk

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* name) {
  VKDK_TRACE("GetDeviceProcAddr");
  if (!strcmp(name, "vkGetDeviceProcAddr")) {
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  }
  return DeviceProcAddr(name);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  }
  if (!strcmp(name, "vkGetDeviceProcAddr")) {
    return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr);
  }
  for (const Entry& e : kInstanceEntries) {
    if (!strcmp(e.name, name)) {
      return e.fn;
    }
  }
  // Vulkan lets an instance return device functions too (they dispatch on their handle).
  return DeviceProcAddr(name);
}

// Dekopon-32's Switch platform code takes the driver's entry as NVK exports it (vk_platform.cpp).
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                            const char* name) {
  return vkGetInstanceProcAddr(instance, name);
}

namespace vkdk {
bool g_trace = [] {
  FILE* f = fopen("sdmc:/switch/projectdream_nx/vkdk_trace", "r");
  if (f) {
    fclose(f);
  }
  return f != nullptr;
}();
}  // namespace vkdk

// ---- vkdk.h

extern "C" void vkdkSetLogCallback(void (*callback)(const char* line)) {
  vkdk::g_log = callback;
}

extern "C" uint32_t vkdkDescriptorSetImageBase(uint64_t set) {
  return set ? reinterpret_cast<VkDescriptorSet>(set)->imageFirst : UINT32_MAX;
}

extern "C" uint32_t vkdkDescriptorSetSamplerBase(uint64_t set) {
  (void)set;
  return UINT32_MAX;  // sets hold no sampler table entries since round i (VkDescriptorSet_T::samplerSlots)
}

// devkitA64's newlib has memalign but no posix_memalign, which FFmpeg's libavutil and uam's Mesa files call; Mesa's NVK
// archives brought one in before vkdk replaced them. Weak, so a C library that has it wins.
extern "C" __attribute__((weak)) int posix_memalign(void** out, size_t alignment, size_t size) {
  void* p = memalign(alignment < sizeof(void*) ? sizeof(void*) : alignment, size);
  if (!p) {
    return ENOMEM;
  }
  *out = p;
  return 0;
}
