#include "vk_symbols.h"

#include <android/log.h>
#include <dlfcn.h>

#define LOG_TAG "VulkanSymbols"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Actual storage for the resolved function pointers
#define VKFN(func) PFN_##func func##_
#include "vksym.h"
#undef VKFN

void ResetVulkanSymbols() {
#define VKFN(func) func##_ = nullptr
#include "vksym.h"
#undef VKFN
}

bool ResolveVulkanSymbols(void* libraryHandle) {
    if (!libraryHandle) {
        LOGE("ResolveVulkanSymbols: null handle");
        return false;
    }

    PFN_vkGetInstanceProcAddr getProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libraryHandle, "vkGetInstanceProcAddr"));

    if (!getProcAddr) {
        getProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libraryHandle, "vk_icdGetInstanceProcAddr"));
    }

    if (!getProcAddr) {
        LOGE("ResolveVulkanSymbols: Could not find vkGetInstanceProcAddr or vk_icdGetInstanceProcAddr");
        return false;
    }

    // Populate all the pointers using the same macro pattern as before
#define VKFN(func)                                                            \
    func##_ = reinterpret_cast<PFN_##func>(dlsym(libraryHandle, #func));      \
    if (!func##_ && getProcAddr) {                                            \
        func##_ = reinterpret_cast<PFN_##func>(getProcAddr(nullptr, #func));  \
    }                                                                         \
    if (!func##_) {                                                           \
        LOGW("Could not resolve %s", #func);                                  \
    }
#include "vksym.h"
#undef VKFN

    // Basic validation: at least the core instance functions should be present
    if (!vkCreateInstance_ || !vkDestroyInstance_ || !vkEnumeratePhysicalDevices_) {
        LOGE("ResolveVulkanSymbols: Critical symbols missing after resolution");
        return false;
    }

    LOGI("Vulkan symbols successfully resolved");
    return true;
}