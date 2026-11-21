// vkdk: what Unity's Android player asks of a Vulkan driver beyond what BO2 and Dekopon-32 did (sdt_nx).
//
// VK_KHR_android_surface: the ANativeWindow the loader gives Unity stands for the console's one window, so the surface
// is libnx's default NWindow (as VK_NN_vi_surface's is). And the Vulkan 1.0 entry points Unity loads by name without
// needing them to do much on this GPU: events (host state; the GPU's waits are barriers, vkdk_cmd.cpp), sparse
// queries (none), layers (none), the render area granularity, memory commitment, image subresource layouts.
//
// Copyright 2026 aks796. GPL-3.0, like the rest of vkdk.
#include "vkdk_internal.h"

#include <algorithm>

using namespace vkdk;

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* name);

namespace {

// Unity passes its ANativeWindow*; the console has one window.
struct AndroidSurfaceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* window;
};

VKAPI_ATTR VkResult VKAPI_CALL CreateAndroidSurfaceKHR(VkInstance, const AndroidSurfaceCreateInfo* info,
                                                       const VkAllocationCallbacks*, VkSurfaceKHR* out) {
  VKDK_TRACE("CreateAndroidSurfaceKHR");
  VkSurfaceKHR s = new (std::nothrow) VkSurfaceKHR_T();
  if (!s) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  s->window = nwindowGetDefault();
  Log("android surface: window %p -> the console's default window", info ? info->window : nullptr);
  *out = s;
  return VK_SUCCESS;
}

// ---- events

VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(VkDevice, const VkEventCreateInfo*, const VkAllocationCallbacks*,
                                           VkEvent* out) {
  VKDK_TRACE("CreateEvent");
  VkEvent e = new (std::nothrow) VkEvent_T();
  if (!e) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  *out = e;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyEvent(VkDevice, VkEvent e, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyEvent");
  delete e;
}

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(VkDevice, VkEvent e) {
  VKDK_TRACE("GetEventStatus");
  return e && e->set.load() ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(VkDevice, VkEvent e) {
  VKDK_TRACE("SetEvent");
  if (e) e->set.store(1);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(VkDevice, VkEvent e) {
  VKDK_TRACE("ResetEvent");
  if (e) e->set.store(0);
  return VK_SUCCESS;
}

// ---- queries with nothing (or little) to say

VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice, VkImage img, const VkImageSubresource* sub,
                                                     VkSubresourceLayout* out) {
  VKDK_TRACE("GetImageSubresourceLayout");
  // vkdk's images are all block linear (optimal tiling only, GetPhysicalDeviceImageFormatProperties): this is asked
  // of no image the application maps. A packed answer for the base level.
  uint32_t bw = 1, bh = 1;
  const uint32_t bytes = FormatBlockBytes(img->format, bw, bh);
  memset(out, 0, sizeof(*out));
  out->rowPitch = uint64_t((img->extent.width + bw - 1) / bw) * bytes;
  out->depthPitch = out->rowPitch * ((img->extent.height + bh - 1) / bh);
  out->arrayPitch = out->depthPitch * img->extent.depth;
  out->size = img->size;
  (void)sub;
}

VKAPI_ATTR void VKAPI_CALL GetRenderAreaGranularity(VkDevice, VkRenderPass, VkExtent2D* out) {
  VKDK_TRACE("GetRenderAreaGranularity");
  *out = {1, 1};
}

VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(VkDevice, VkDeviceMemory m, VkDeviceSize* out) {
  VKDK_TRACE("GetDeviceMemoryCommitment");
  *out = m ? m->size : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL MergePipelineCaches(VkDevice, VkPipelineCache, uint32_t, const VkPipelineCache*) {
  VKDK_TRACE("MergePipelineCaches");
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(VkDevice, VkImage, uint32_t* count,
                                                            VkSparseImageMemoryRequirements*) {
  VKDK_TRACE("GetImageSparseMemoryRequirements");
  *count = 0;
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements2(VkDevice, const void*, uint32_t* count, void*) {
  VKDK_TRACE("GetImageSparseMemoryRequirements2");
  *count = 0;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
                                                                        VkSampleCountFlagBits, VkImageUsageFlags,
                                                                        VkImageTiling, uint32_t* count,
                                                                        VkSparseImageFormatProperties*) {
  VKDK_TRACE("GetPhysicalDeviceSparseImageFormatProperties");
  *count = 0;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const void*,
                                                                         uint32_t* count, void*) {
  VKDK_TRACE("GetPhysicalDeviceSparseImageFormatProperties2");
  *count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties*) {
  VKDK_TRACE("EnumerateDeviceLayerProperties");
  *count = 0;
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t* count,
                                                                   VkQueueFamilyProperties2* out) {
  VKDK_TRACE("GetPhysicalDeviceQueueFamilyProperties2");
  auto base = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      vkGetInstanceProcAddr(nullptr, "vkGetPhysicalDeviceQueueFamilyProperties"));
  if (!out) {
    base(pd, count, nullptr);
    return;
  }
  VkQueueFamilyProperties p[1];
  uint32_t n = std::min<uint32_t>(*count, 1);
  base(pd, &n, p);
  for (uint32_t i = 0; i < n; ++i) {
    out[i].queueFamilyProperties = p[i];
  }
  *count = n;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                       const VkPhysicalDeviceImageFormatInfo2* info,
                                                                       VkImageFormatProperties2* out) {
  VKDK_TRACE("GetPhysicalDeviceImageFormatProperties2");
  auto base = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
      vkGetInstanceProcAddr(nullptr, "vkGetPhysicalDeviceImageFormatProperties"));
  return base(pd, info->format, info->type, info->tiling, info->usage, info->flags, &out->imageFormatProperties);
}

}  // namespace

namespace vkdk {

#define VKDK_ENTRY(name, fn) \
  if (!strcmp(n, #name)) return reinterpret_cast<PFN_vkVoidFunction>(fn)

PFN_vkVoidFunction AndroidProcAddr(const char* n) {
  VKDK_ENTRY(vkCreateAndroidSurfaceKHR, CreateAndroidSurfaceKHR);
  VKDK_ENTRY(vkCreateEvent, CreateEvent);
  VKDK_ENTRY(vkDestroyEvent, DestroyEvent);
  VKDK_ENTRY(vkGetEventStatus, GetEventStatus);
  VKDK_ENTRY(vkSetEvent, SetEvent);
  VKDK_ENTRY(vkResetEvent, ResetEvent);
  VKDK_ENTRY(vkGetImageSubresourceLayout, GetImageSubresourceLayout);
  VKDK_ENTRY(vkGetRenderAreaGranularity, GetRenderAreaGranularity);
  VKDK_ENTRY(vkGetDeviceMemoryCommitment, GetDeviceMemoryCommitment);
  VKDK_ENTRY(vkMergePipelineCaches, MergePipelineCaches);
  VKDK_ENTRY(vkGetImageSparseMemoryRequirements, GetImageSparseMemoryRequirements);
  VKDK_ENTRY(vkGetImageSparseMemoryRequirements2, GetImageSparseMemoryRequirements2);
  VKDK_ENTRY(vkGetImageSparseMemoryRequirements2KHR, GetImageSparseMemoryRequirements2);
  VKDK_ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties, GetPhysicalDeviceSparseImageFormatProperties);
  VKDK_ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2, GetPhysicalDeviceSparseImageFormatProperties2);
  VKDK_ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2KHR, GetPhysicalDeviceSparseImageFormatProperties2);
  VKDK_ENTRY(vkEnumerateDeviceLayerProperties, EnumerateDeviceLayerProperties);
  VKDK_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2, GetPhysicalDeviceQueueFamilyProperties2);
  VKDK_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2KHR, GetPhysicalDeviceQueueFamilyProperties2);
  VKDK_ENTRY(vkGetPhysicalDeviceImageFormatProperties2, GetPhysicalDeviceImageFormatProperties2);
  VKDK_ENTRY(vkGetPhysicalDeviceImageFormatProperties2KHR, GetPhysicalDeviceImageFormatProperties2);
  return nullptr;
}

}  // namespace vkdk
