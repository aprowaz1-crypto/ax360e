#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Shared Vulkan symbol declarations (replaces old vkapi.h).

#define VKFN(func) extern PFN_##func func##_
#include "vksym.h"
#undef VKFN

// Helper to clear all resolved pointers (used on unload or failure)
void ResetVulkanSymbols();

// Helper to resolve symbols from a loaded library handle using vkGetInstanceProcAddr (or vk_icdGetInstanceProcAddr).
// Returns true if critical symbols (CreateInstance, etc.) were resolved.
bool ResolveVulkanSymbols(void* libraryHandle);