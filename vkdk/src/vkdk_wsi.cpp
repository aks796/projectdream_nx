// vkdk: VK_NN_vi_surface and VK_KHR_swapchain on deko3d's DkSwapchain (libnx's NWindow). Copyright 2026 aks796.
// GPL-3.0.
#include "vkdk_internal.h"

#include <atomic>

#include <algorithm>

using namespace vkdk;

static VKAPI_ATTR VkResult VKAPI_CALL CreateViSurfaceNN(VkInstance, const VkViSurfaceCreateInfoNN* info,
                                                        const VkAllocationCallbacks*, VkSurfaceKHR* out) {
  VKDK_TRACE("CreateViSurfaceNN");
  VkSurfaceKHR s = new (std::nothrow) VkSurfaceKHR_T();
  if (!s) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  s->window = static_cast<NWindow*>(info->window);
  *out = s;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR s, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroySurfaceKHR");
  delete s;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceSupport(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* out) {
  VKDK_TRACE("GetSurfaceSupport");
  *out = VK_TRUE;
  return VK_SUCCESS;
}

// sdt_nx: the application's hook run before the window's size is read or used (vkdkSetWindowPrepare): libnx forgets
// a window's size when its last swapchain goes, and the next one would come out at the default 1280x720 under a
// docked 1920x1080 crop (Unity then drew its 1080p frame into a 720p swapchain: the top-left two thirds, zoomed).
static void (*g_windowPrepare)(void);
static std::atomic<u32> g_surfaceW{0}, g_surfaceH{0};
static void (*g_presentHook)(uint32_t, uint32_t);

static VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceCapabilities(VkPhysicalDevice, VkSurfaceKHR s,
                                                             VkSurfaceCapabilitiesKHR* c) {
  VKDK_TRACE("GetSurfaceCapabilities");
  u32 w = 1280, h = 720;
  if (g_windowPrepare) {
    g_windowPrepare();
  }
  if (g_surfaceW && g_surfaceH) {
    // the size the application's window has now (vkdkSetSurfaceSize): a docked/handheld change, while the window
    // still has the old swapchain's size
    w = g_surfaceW, h = g_surfaceH;
  } else if (s->window) {
    nwindowGetDimensions(s->window, &w, &h);
  }
  memset(c, 0, sizeof(*c));
  c->minImageCount = 2;
  c->maxImageCount = 3;
  c->currentExtent = {w, h};
  c->minImageExtent = {1, 1};
  c->maxImageExtent = {1920, 1080};
  c->maxImageArrayLayers = 1;
  c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  c->supportedUsageFlags =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSurfaceFormats(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                        VkSurfaceFormatKHR* out) {
  VKDK_TRACE("GetSurfaceFormats");
  // (sRGB ones too: a linear colour space game, Unity's Android player, presents an sRGB swapchain)
  static const VkSurfaceFormatKHR kFormats[] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  const uint32_t total = uint32_t(sizeof(kFormats) / sizeof(kFormats[0]));
  if (!out) {
    *count = total;
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, total);
  memcpy(out, kFormats, n * sizeof(kFormats[0]));
  *count = n;
  return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSurfacePresentModes(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                             VkPresentModeKHR* out) {
  VKDK_TRACE("GetSurfacePresentModes");
  static const VkPresentModeKHR kModes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  if (!out) {
    *count = 2;
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, 2);
  memcpy(out, kModes, n * sizeof(kModes[0]));
  *count = n;
  return n < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}

static void ReleaseSwapchain(VkSwapchainKHR sc) {
  VkDevice d = sc->device;
  if (sc->dk) {
    {
      std::lock_guard<std::mutex> lock(d->queue.mutex);
      dkQueueWaitIdle(d->queue.dk);
    }
    dkSwapchainDestroy(sc->dk);
    sc->dk = nullptr;
  }
  for (VkImage img : sc->images) {
    delete img;
  }
  sc->images.clear();
  d->DestroyBlock(sc->memory);
  sc->memory = nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR* info,
                                                      const VkAllocationCallbacks*, VkSwapchainKHR* out) {
  VKDK_TRACE("CreateSwapchain");
  // One DkSwapchain per window at a time: the old one goes first.
  if (info->oldSwapchain) {
    ReleaseSwapchain(info->oldSwapchain);
  }
  VkSwapchainKHR sc = new (std::nothrow) VkSwapchainKHR_T();
  if (!sc) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  sc->device = d;
  sc->width = info->imageExtent.width;
  sc->height = info->imageExtent.height;
  sc->format = info->imageFormat;
  const uint32_t count = std::clamp<uint32_t>(info->minImageCount, 2, 3);
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, d->dk);
  m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression |
            DkImageFlags_Usage2DEngine;
  m.format = ToDkFormat(info->imageFormat);
  m.dimensions[0] = sc->width;
  m.dimensions[1] = sc->height;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &m);
  uint32_t align = dkImageLayoutGetAlignment(&layout);
  uint64_t raw = dkImageLayoutGetSize(&layout);
  Log("swapchain: %ux%u format %d -> deko3d %d, layout %llu bytes, alignment %u", sc->width, sc->height,
      int(info->imageFormat), int(m.format), (unsigned long long)raw, align);
  if (!raw || !align) {
    // Dekopon-32's 32-bit deko3d: retry without compression and the 2D engine.
    m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    dkImageLayoutInitialize(&layout, &m);
    align = dkImageLayoutGetAlignment(&layout);
    raw = dkImageLayoutGetSize(&layout);
    Log("swapchain: retried without compression: %llu bytes, alignment %u", (unsigned long long)raw, align);
    if (!raw || !align) {
      delete sc;
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }
  const uint32_t size = AlignUp(uint32_t(raw), align);
  sc->memory = d->MakeBlock(size * count, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain");
  if (!sc->memory) {
    delete sc;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const DkImage* images[3];
  for (uint32_t i = 0; i < count; ++i) {
    VkImage img = new VkImage_T();
    img->format = info->imageFormat;
    img->dkFormat = m.format;
    img->dkType = DkImageType_2D;
    img->extent = {sc->width, sc->height, 1};
    img->usage = info->imageUsage;
    img->layout = layout;
    img->size = size;
    img->alignment = align;
    img->swapchain = true;
    dkImageInitialize(&img->image, &layout, sc->memory, i * size);
    img->bound = true;
    sc->images.push_back(img);
    images[i] = &img->image;
  }
  NWindow* window = info->surface ? info->surface->window : nwindowGetDefault();
  if (g_windowPrepare) {
    g_windowPrepare();
  }
  // the window shows exactly the swapchain's images (its size and crop: the compositor scales that to the screen)
  nwindowSetDimensions(window, sc->width, sc->height);
  nwindowSetCrop(window, 0, 0, sc->width, sc->height);
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, d->dk, window, images, count);
  sc->dk = dkSwapchainCreate(&sm);
  if (!sc->dk) {
    ReleaseSwapchain(sc);
    delete sc;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  dkSwapchainSetSwapInterval(sc->dk, info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : 1);
  Log("swapchain: %ux%u, %u images, %s", sc->width, sc->height, count,
      info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
  *out = sc;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySwapchain(VkDevice, VkSwapchainKHR sc, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroySwapchain");
  if (sc) {
    ReleaseSwapchain(sc);
    delete sc;
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImages(VkDevice, VkSwapchainKHR sc, uint32_t* count,
                                                         VkImage* out) {
  VKDK_TRACE("GetSwapchainImages");
  if (!out) {
    *count = uint32_t(sc->images.size());
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, uint32_t(sc->images.size()));
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = sc->images[i];
  }
  *count = n;
  return n < sc->images.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage(VkDevice, VkSwapchainKHR sc, uint64_t, VkSemaphore sem,
                                                       VkFence fence, uint32_t* index) {
  VKDK_TRACE("AcquireNextImage");
  if (!sc->dk) {
    return VK_ERROR_OUT_OF_DATE_KHR;
  }

  // deko3d's acquire waits for the compositor to give a buffer back (with two or three buffers, at once in practice):
  // a timeout of zero is not honoured. The image's ready fence goes to the semaphore (the next submission waits on it)
  // and to the fence.
  int slot = 0;
  DkFence ready;
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  dkSwapchainAcquireImage(sc->dk, &slot, &ready);
  g_acquireNs.fetch_add(armTicksToNs(armGetSystemTick()) - t0, std::memory_order_relaxed);
  if (sem) {
    sem->fence = ready;
    sem->pending = true;
  }
  if (fence) {
    fence->fence = ready;
    fence->state.store(1, std::memory_order_release);
  }
  *index = uint32_t(slot);
  return VK_SUCCESS;
}

// ---- a capture of the next presented image (vkdkCaptureNext): copied to linear memory, written as a 32-bit BMP

static std::mutex g_captureMutex;
static std::string g_capturePath;

static void CaptureImage(VkQueue q, VkImage img, const char* path) {
  VkDevice d = q->device;
  const uint32_t w = img->extent.width, h = img->extent.height, size = AlignUp(w * h * 4, DK_MEMBLOCK_ALIGNMENT);
  DkMemBlock staging = d->MakeBlock(size, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached, "capture");
  DkMemBlock cmdMem = d->MakeBlock(0x10000, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "capture cmds");
  if (!staging || !cmdMem) {
    Log("capture: no memory");
    d->DestroyBlock(staging);
    d->DestroyBlock(cmdMem);
    return;
  }
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, d->dk);
  DkCmdBuf cmd = dkCmdBufCreate(&cm);
  dkCmdBufAddMemory(cmd, cmdMem, 0, 0x10000);
  DkImageView view;
  dkImageViewDefaults(&view, &img->image);
  const DkImageRect rect = {0, 0, 0, w, h, 1};
  const DkCopyBuf buf = {dkMemBlockGetGpuAddr(staging), w * 4, h};
  dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
  dkCmdBufCopyImageToBuffer(cmd, &view, &rect, &buf, 0);
  dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
  dkQueueSubmitCommands(q->dk, dkCmdBufFinishList(cmd));
  dkQueueWaitIdle(q->dk);
  const uint8_t* px = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(staging));
  armDCacheFlush((void*)px, size);
  FILE* f = fopen(path, "wb");
  if (f) {
    const uint32_t data = w * h * 4;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    put32(2, 54 + data);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, uint32_t(-int32_t(h)));  // top-down
    hdr[26] = 1;
    hdr[28] = 32;
    put32(34, data);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(w * 4);
    const bool bgra = img->format == VK_FORMAT_B8G8R8A8_UNORM || img->format == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* s = px + size_t(y) * w * 4;
      for (uint32_t x = 0; x < w; ++x) {
        row[x * 4 + 0] = s[x * 4 + (bgra ? 0 : 2)];
        row[x * 4 + 1] = s[x * 4 + 1];
        row[x * 4 + 2] = s[x * 4 + (bgra ? 2 : 0)];
        row[x * 4 + 3] = 255;
      }
      fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    Log("capture: %ux%u -> %s", w, h, path);
  }
  dkCmdBufDestroy(cmd);
  d->DestroyBlock(staging);
  d->DestroyBlock(cmdMem);
}

static VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue q, const VkPresentInfoKHR* info) {
  VKDK_TRACE("QueuePresent");
  q->device->presents.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::mutex> lock(q->mutex);
  {
    std::string path;
    {
      std::lock_guard<std::mutex> cl(g_captureMutex);
      path.swap(g_capturePath);
    }
    if (!path.empty() && info->swapchainCount && info->pSwapchains[0]->dk) {
      CaptureImage(q, info->pSwapchains[0]->images[info->pImageIndices[0]], path.c_str());
    }
  }
  // a swapchain no longer the surface's size (vkdkSetSurfaceSize: docked/handheld) presents, and says OUT_OF_DATE:
  // the application makes a new one at the size the surface reports now. (Unity takes no notice of SUBOPTIMAL,
  // which phones report on every rotation, and out of date at the acquire left it a frame with no command buffer.)
  VkResult all = VK_SUCCESS;
  const u32 sw = g_surfaceW, sh = g_surfaceH;
  for (uint32_t i = 0; i < info->swapchainCount; ++i) {
    VkSwapchainKHR sc = info->pSwapchains[i];
    VkResult r = sc->dk ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
    if (sc->dk) {
      dkQueuePresentImage(q->dk, sc->dk, int(info->pImageIndices[i]));
      if (sw && sh && (sc->width != sw || sc->height != sh)) {
        r = VK_ERROR_OUT_OF_DATE_KHR;
      }
    }
    if (sc->dk && i == 0 && g_presentHook) {
      g_presentHook(sc->width, sc->height);
    }
    if (info->pResults) {
      info->pResults[i] = r;
    }
    if (r != VK_SUCCESS && all == VK_SUCCESS) {
      all = r;
    }
  }
  lock.unlock();
  CensusFrameEnd();
  // device memory let go some frames ago goes back now, after the frame is on its way (VkDevice_T::DestroyBlock)
  q->device->RetireBlocks(false);
  return all;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDebugUtilsMessenger(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*,
                                                                const VkAllocationCallbacks*,
                                                                VkDebugUtilsMessengerEXT* out) {
  VKDK_TRACE("CreateDebugUtilsMessenger");
  *out = new (std::nothrow) VkDebugUtilsMessengerEXT_T();
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDebugUtilsMessenger(VkInstance, VkDebugUtilsMessengerEXT m,
                                                             const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyDebugUtilsMessenger");
  delete m;
}

static VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) {
  VKDK_TRACE("SetDebugUtilsObjectName");
  return VK_SUCCESS;
}

namespace vkdk {

#define VKDK_ENTRY(name, fn) \
  if (!strcmp(n, #name)) return reinterpret_cast<PFN_vkVoidFunction>(fn)

PFN_vkVoidFunction WsiProcAddr(const char* n) {
  VKDK_ENTRY(vkCreateViSurfaceNN, CreateViSurfaceNN);
  VKDK_ENTRY(vkDestroySurfaceKHR, DestroySurfaceKHR);
  VKDK_ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR, GetSurfaceSupport);
  VKDK_ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, GetSurfaceCapabilities);
  VKDK_ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR, GetSurfaceFormats);
  VKDK_ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR, GetSurfacePresentModes);
  VKDK_ENTRY(vkCreateSwapchainKHR, CreateSwapchain);
  VKDK_ENTRY(vkDestroySwapchainKHR, DestroySwapchain);
  VKDK_ENTRY(vkGetSwapchainImagesKHR, GetSwapchainImages);
  VKDK_ENTRY(vkAcquireNextImageKHR, AcquireNextImage);
  VKDK_ENTRY(vkQueuePresentKHR, QueuePresent);
  VKDK_ENTRY(vkCreateDebugUtilsMessengerEXT, CreateDebugUtilsMessenger);
  VKDK_ENTRY(vkDestroyDebugUtilsMessengerEXT, DestroyDebugUtilsMessenger);
  VKDK_ENTRY(vkSetDebugUtilsObjectNameEXT, SetDebugUtilsObjectName);
  return AndroidProcAddr(n);
}

}  // namespace vkdk

extern "C" void vkdkCaptureNext(const char* path) {
  std::lock_guard<std::mutex> cl(g_captureMutex);
  g_capturePath = path ? path : "";
}

extern "C" void vkdkSetWindowPrepare(void (*fn)(void)) {
  g_windowPrepare = fn;
}

extern "C" void vkdkSetPresentHook(void (*fn)(uint32_t width, uint32_t height)) {
  g_presentHook = fn;
}

extern "C" void vkdkSetSurfaceSize(uint32_t w, uint32_t h) {
  g_surfaceW = w;
  g_surfaceH = h;
}
