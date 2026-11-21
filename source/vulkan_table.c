/* vulkan_table.c -- libvulkan.so for Unity's Android player: vkdk
 *
 * Unity dlopens libvulkan.so and asks it for vkGetInstanceProcAddr (and the
 * few global functions Vulkan loaders export); everything else comes through
 * that. so_util resolves libvulkan.so to the import tables, this one among
 * them (main.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include <stdint.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "so_util.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name);

static const char *const k_global[] = {
  "vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateInstance", "vkDestroyInstance",
  "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties", "vkEnumerateInstanceVersion",
  "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties", "vkCreateDevice",
};

#define NGLOBAL (sizeof k_global / sizeof *k_global)

DynLibFunction vulkan_functions[NGLOBAL];
const size_t vulkan_numfunctions = NGLOBAL;

void vulkan_table_init(void) {
  for (size_t i = 0; i < NGLOBAL; i++) {
    vulkan_functions[i].symbol = k_global[i];
    vulkan_functions[i].func = (uintptr_t)vkGetInstanceProcAddr(NULL, k_global[i]);
  }
}
